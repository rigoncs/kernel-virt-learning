# 05 · PVM 深度解析：基于页表的软件虚拟化

> PVM（Page-table / Pagetable-based Virtual Machine，也有文献称 Page-based Virtual Machine）是一类**不依赖硬件二级翻译扩展（EPT/Stage-2）暴露给 guest**、以页表与半虚拟化为核心的软件虚拟化框架。代表实现：腾讯 OpenCloudOS 的 PVM（嵌套/沙箱场景）、阿里巴巴提交主线 KVM 的 PVM 支持（`enable_pvm`，软件虚拟化）。本章建立统一模型。

---

## 1. 动机：为什么硬件辅助之后还要软件虚拟化

1. **翻译放大**：EPT 使 TLB miss 访存放大 $L_g × (1+L_e)$（见 [03 §3.2](03-memory-virtualization.md)），对 TLB 压力型负载（DB、HPC）仍显著；SPT 只有一级翻译，TLB 放大最小
2. **嵌套虚拟化开销**：L1 hypervisor + L2 双层 EPT 叠加（max 12+ 级），软件模拟代价大；基于页表的方案可把嵌套折叠成一层
3. **轻量隔离/沙箱场景**（AI 代码沙箱、FaaS、浏览器 sandbox）：需要 VM 级隔离 + 毫秒级启停 + 极低内存开销，但不需要（或无法信任）完整硬件虚拟化栈
4. **宿主透明性**：PVM guest 不依赖 VT-x/EPT —— 可运行在无虚拟化扩展的机器上，且宿主 hypervisor 无需向 guest 暴露虚拟化特性（嵌套场景对宿主完全透明）

**核心判断**：EPT 的主要代价在"翻译路径变长"；SPT 的主要代价在"guest 修改页表/CR3 的拦截"。PVM 的设计思路是：**用半虚拟化把 SPT 的第二类代价降到最低，从而享受第一级翻译的性能**。

---

## 2. PVM 统一架构模型

### 2.1 三个基本角色

```
┌──────────────────────────────────────────────────────────┐
│ PVM Guest（半虚拟化内核）                                  │
│  · 知道自己运行在 PVM 上（CPUID 探测）                      │
│  · 特权级布局: 内核态不执行特权指令, 改用 hypercall         │
│  · 通过共享内存与 host 交换状态（vcpu state area 等）        │
└────────────▲─────────────────────────────────────────────┘
             │ hypercall / 共享内存 / 中断半虚拟化
┌────────────┴─────────────────────────────────────────────┐
│ PVM Host（KVM 扩展）                                      │
│  · 以 guest 内核态为 "supervisor"，用户态进程跑非根特权级    │
│  · 维护影子页表: GVA→HPA 直接映射（一级翻译!）               │
│  · 拦截/模拟: 页表更新、CR3 切换、异常、中断                 │
└────────────▲─────────────────────────────────────────────┘
             │ 普通进程语义（无虚拟化扩展要求）
┌────────────┴─────────────────────────────────────────────┐
│ 宿主 Linux / 上层 Hypervisor（对 PVM 完全透明）             │
└──────────────────────────────────────────────────────────┘
```

### 2.2 与传统 SPT 的关键差异（PVM 的"改良影子页表"）

| 传统 SPT 痛点 | PVM 的解法 |
|---------------|-----------|
| guest 随意写 CR3 → 大量拦截 | guest 内核**协作式**切换：通过约定入口（如 hypercall / 共享内存中的切换请求）通知 host，host 精确知道切换语义，无需猜模拟 |
| guest 修改页表需写保护 + 被动同步 | guest **半虚拟化主动上报**页表变更（或在受控路径分配页表），同步协议确定性 |
| 中断/EOI 频繁 VMexit | 中断半虚拟化：EOI 拦截/合成中断控制器，共享内存投递 |
| 特权指令逐一陷阱模拟 | guest 内核编译为 PVM 感知：敏感操作直接编译为 hypercall（消除大部分陷阱） |
| 不同 guest 进程影子表集合管理复杂 | 借助半虚拟化 ASID/上下文标识，影子表按 guest 上下文精确复用 |

> 一句话：**PVM = 影子页表的一级翻译性能 + 半虚拟化消除 SPT 的拦截开销 + 对硬件虚拟化扩展零依赖**。

### 2.3 地址翻译路径

```
PVM guest 进程访存:
  GVA ──► 影子页表(GVA→HPA, host 维护) ──► HPA        # 一级翻译, TLB 放大 = L_g
                     ▲
                     │ 按需构建: guest 页表(GVA→GPA) + memslot(GPA→HPA)
                     │   (由 host 在 fault/上报路径软件完成)
```

对比三种方案访存放大（全 TLB miss）：

```
SPT / PVM :  N = L_g              （x86 4级 → 4 次）
EPT/NPT  :  N = L_g × (1 + L_e)   （4×5 = 20 次）
```

### 2.4 特权级与切换（x86 设计示例）

主线 PVM（Alibaba）在 x86 上的典型布局：

- guest 用户态：CPL3 正常执行
- guest 内核态：不使用真正的 Ring0 特权指令，而以**约定的"supervisor 模式"**运行（借助半虚拟化切换），敏感操作走 hypercall
- 上下文切换（系统调用/中断进入 guest 内核）不再依赖 VMCS 的 root/non-root 切换，而是**页表 + 特权级翻转 + 共享内存状态区**：
  1. guest 用户态触发系统调用 → 直接跳入 guest 内核（非特权切换，快）
  2. 需要特权资源（页表、中断、计时）→ hypercall 陷出到 PVM host
  3. host 更新共享内存中的 vCPU 状态 → 影子页表切换/更新

这消除了传统 VMX 中"每次系统调用都 root/non-root 来回"的固定开销，也解释了为什么 PVM 对**轻量沙箱/微 VM** 场景延迟表现好。

---

## 3. 与 EPT 方案的量化对比模型

设：`f_fault` = guest 缺页频率，`f_ptmod` = guest 页表修改/CR3 切换频率，`f_exit` = 其他 VMexit 频率：

```
SPT 总开销  ≈ f_fault × C_spt_fault + f_ptmod × C_spt_sync
EPT 总开销  ≈ f_fault × C_ept_fault + L放大 × TLB miss 代价
PVM 总开销  ≈ f_fault × C_spt_fault + f_ptmod × C_pvm_pvc(半虚拟化同步, 远小于 C_spt_sync)
```

- 宿主内存充裕、TLB 压力大、页表修改频繁但可半虚拟化 → PVM 占优
- 大内存随机访问（TLB miss 多且无大页）→ PVM/SPT 一级翻译优势明显
- guest 不受控（Windows 等非半虚拟化 OS）→ PVM 不可用，EPT 唯一解
- 嵌套场景：PVM 折叠为一级 → 显著优于 nested EPT

---

## 4. 开源实现地图

| 项目 | 说明 | 入口 |
|------|------|------|
| OpenCloudOS PVM（腾讯） | 基于页表的嵌套虚拟化框架，构建于 KVM 之上，host 内核在 OpenCloudOS 内核分支维护；Cube Sandbox 等沙箱产品使用 | OpenCloudOS-Kernel 仓库（GitHub/Gitee）检索 `pvm` 分支；内核内 `arch/x86/kvm/pvm*` 相关文件 |
| KVM x86 PVM 支持（阿里巴巴） | 已进入主线演进方向的软件虚拟化支持：`kvm-intel.enable_pvm=1`，以 CPUID feature + VMX-less 模式运行 | 内核源码 `arch/x86/kvm/` 搜索 `pvm`；LWN/KVM Forum 材料 |
| kvm-unit-tests | `x86/pvm.c` 等测试（视版本） | `tools/kvm-unit-tests/x86/` |

> 实操提示：主线内核中 `grep -rn "PVM" arch/x86/kvm/ Documentation/virt/kvm/` 是确认你所处内核版本 PVM 支持程度的最快方式。本仓库配套源码树若未包含 PVM 代码，请按 resources/README.md 获取对应分支。

### 4.1 阅读顺序建议（源码层面）

1. `Documentation/virt/kvm/`：PVM/软件虚拟化相关文档与 API 变更
2. guest 侧半虚拟化接口（CPUID leaf、MSR/hypercall 编号约定）
3. host 侧影子页表管理（复用 KVM MMU 的 SPT 路径：`arch/x86/kvm/mmu/mmu.c` 的 shadow 模式 + `paging_tmpl.h`）
4. 切换路径：vCPU state 共享区 + hypercall 处理函数
5. 中断半虚拟化：EOI 拦截/合成中断投递

---

## 5. 场景定位：PVM vs EPT vs 传统方案

```
                硬件虚拟化扩展可用? ──否──► SPT(性能差) / PVM(推荐)
                       │是
                       ▼
              guest 可半虚拟化改造? ──否──► EPT (唯一解)
                       │是
                       ▼
        ┌─ 密集 TLB 压力 / 沙箱微VM / 嵌套 ──► PVM 更优
        └─ 通用云主机 / 不可信负载 ──────────► EPT (隔离与兼容性)
```

安全边界提醒：软件虚拟化的隔离依赖"guest 内核合作"，其威胁模型与硬件辅助不同——生产环境给不可信 guest 用 EPT/TDX 等硬件隔离；PVM 主要面向**受信/半受信工作负载**（自家容器内核、沙箱 agent 等）。

---

## 6. 自测题

1. 为什么 PVM 的 TLB 放大是 $L_g$ 而不是 $L_g×(1+L_e)$？代价换来了什么？
2. 描述 PVM guest 中"进程 A 切换到进程 B"的完整事件序列（含页表与共享内存交互）。
3. PVM 对宿主 hypervisor 透明的含义是什么？嵌套场景下为什么重要？
4. PVM 的威胁模型与 EPT 有何本质不同？
5. 若要给一个新架构（如 RISC-V）移植 PVM，最少需要哪些原语？（提示：页表访问、异常注入、共享内存、时钟/中断半虚拟化）

---

## 延伸阅读

见 [resources/README.md](../resources/README.md) §"PVM 与软件虚拟化"。
