# 03 · 内存虚拟化核心原理（SPT / EPT / NPT / Stage-2）

> 本章是全仓库的主线。目标：完整掌握 GVA→GPA→HPA 两级翻译的三种实现方案、开销模型与 TLB 管理。
>
> **ARM64 读者阅读指引**：本章 §1–2（概念与影子页表）通用必读；§3 中 ARM64 Stage-2 细节（§3.4）为 ARM 主线，x86 部分（§3.1–3.3）作为对照理解；§3.5 TLB 管理两架构都重要。深入 ARM 细节请配合 [08 §3](08-arm64-virt-extensions.md)（Stage-2 描述符/权限合成/VMID）与 [09 §4](09-kvm-arm-impl.md)（KVM/ARM fault 处理链）。

---

## 1. 为什么需要两级地址翻译

虚拟机里同时存在三个地址空间：

```
GVA  Guest Virtual Address    guest 进程视角虚拟地址
GPA  Guest Physical Address   guest 内核视角"物理"地址（软件抽象！）
HPA  Host Physical Address    真实物理内存
```

GPA 存在的原因：
1. guest OS 必须以为自己在管理真实物理内存（引导协议、NUMA 拓扑、E820 等）
2. VMM 需要自由重排/回收/超卖 guest 内存

内存虚拟化的本质任务：**在不修改（或最少修改）guest 的前提下，维护 GVA→HPA 的等价映射，并保证隔离**。

---

## 2. 方案一：影子页表（Shadow Page Table, SPT）

纯软件方案（Intel EPT 出现前的主流，2005 前后）：

### 2.1 原理

VMM 为每个 guest 页表维护一份"影子页表"，直接把 **GVA→HPA** 填进去，并让 CR3 指向影子表。Guest 自己的页表（GVA→GPA）被**写保护**。

```
        guest 页表 (GVA→GPA, 只读)
             ▲ 写保护
             │ 同步（VMM 软件翻译）
        影子页表 (GVA→HPA) ◄── 硬件 MMU 实际使用
```

### 2.2 同步协议

1. **按需构建**：guest 访问 GVA 硬件查影子表 miss（P=0）→ VMM 注入"影子缺页"→ 软件走一遍 guest 页表：`GVA →(guest PT) GPA →(软件查 memslot) HPA` → 填影子 PTE
2. **写保护监听**：guest 页表页被 VMM 映射为只读；guest 修改自身 PTE → 触发 host #PF → VMM 同步更新影子表项
3. **CR3 切换**：guest 写 CR3 → VMM 拦截 → 切换到对应进程的影子表（不同 guest 进程对应不同影子表集合）；TLB flush 语义由 VMM 模拟

### 2.3 开销模型

| 事件 | 成本 |
|------|------|
| 影子缺页 | 2 次软件页表遍历 + 填表（每次 fault 数千周期） |
| guest 写页表 | 额外 host #PF + 同步 |
| guest 切 CR3 | 需 flush/切换影子表，fork 密集负载崩溃性下降 |
| 内存占用 | 影子表 ≈ 与 guest 页表同量级的额外内存 |

**结论**：SPT 的成本集中在"guest 修改自身页表"的频率上，代码热路径（fork/exec、mmap 大量文件）都是重灾区。这就是硬件 EPT 出现的直接动因。

---

## 3. 方案二：硬件二级翻译

### 3.1 术语对照

| 架构 | 名称 | 根指针 | 缺页异常 | 控制 |
|------|------|--------|----------|------|
| Intel | EPT | VMCS EPTP | EPT violation / misconfig | `VMX_EPT_*` |
| AMD | NPT (RVI) | VMCB nCR3 | #NPF (nested page fault) | — |
| ARM64 | Stage-2 | VTTBR_EL2 | Data Abort (ESR.ISS.S2 == 1) | `VTCR_EL2` |

### 3.2 翻译路径（重点：两次页表遍历）

硬件 MMU 自动完成两段 walk：

```
GVA ──[guest 页表, 根=gCR3]──► GPA ──[EPT/Stage-2, 根=EPTP/VTTBR]──► HPA

x86-64 + EPT（各 4 级）最坏访存放大：
  guest PT 4 次访存（查 PML4E/PDPTE/PDE/PTE）
  × 每次都要再做一次 EPT walk（4 次访存）
  = 4 × (1 + 4) = 20 次内存访问（TLB 全 miss、无大页时）
```

**公式**：设 guest 页表级数 $L_g$，EPT 级数 $L_e$，全 miss 时访存次数
$N = L_g \times (1 + L_e)$
采用 1GB EPT 大页（$L_e = 2$）与 2M guest 大页（$L_g=3$）可优化至 $3 × 3 = 9$；TLB 命中时为 1。

### 3.3 EPT 条目与异常

EPT 条目位（对照 Stage-1 PTE）：

| 位 | 含义 |
|----|------|
| 0 | Read permission |
| 1 | Write permission |
| 2 | Execute permission（X86 内含 execute-only 能力，Stage-1 做不到） |
| 3-5 | Memory type（EPT MT，绕过 MTRR） |
| 6 | Ignore PAT |
| 52+ | Physical address |

两类异常：
- **EPT violation**：翻译路径走通但权限不足/表项不存在 → 类似 Stage-1 的"缺页或权限错误" → KVM 按 read/write/exec 退出限定符处理（补映射或注入 guest 缺页）
- **EPT misconfig**：表项内存类型/保留位非法 → 几乎总是 bug 或特殊页（如 MMIO 预留），KVM 直接处理为 MMIO

### 3.4 ARM64 Stage-2 细节

- 控制寄存器 `VTCR_EL2`：`T0SZ`（GPA 有效位宽）、`SL0`（起始层级，4KB 粒度时 SL0=1 从 level-1 开始）、`SH0/IRGN0/ORGN0`（共享性/缓存属性）
- Stage-2 描述符权限位与 Stage-1 **正交合成**：

```
最终访问权限 = Stage-1 权限 ∩ Stage-2 权限
示例: S1 允许 W，S2 XN... 组合出 S2AP[1:0]、XN[1:0]（含 xn 只对 EL1/EL0 之一生效）
```

- 异常判定：ESR_EL2 的 `ISS` 中 `S2` 位=1 表示 Stage-2 fault；`FAR_EL2`=GVA，`HPFAR_EL2`=故障 GPA（**注意：只有 Stage-2 fault 才把 GPA 放在 HPFAR**）
- VMID：`VTTBR_EL2.VMID` + `TCR_EL2.VS` 决定 8/16 位，TLB tag 用

### 3.5 TLB 管理

- x86：EPTP 在 VM-entry/exit 自动 flush 语义；开启 `EPTP switching`/`INVVPID`/`INVEPT` 精细管理；PCID 保留 guest 语境
- ARM64：TLB entry 以 VMID 打 tag；`TLBI VMALLE1` 只 flush 当前 VM 的 EL1 翻译；VM 切换不用全量 flush（只要 VMID 不同）
- KVM 的 TLB flush 半虚拟化接口：`KVM_REQ_TLB_FLUSH`，异步请求机制见 `virt/kvm/kvm_main.c`

### 3.6 开销对比（SPT vs EPT）

| 维度 | 影子页表 | EPT/Stage-2 |
|------|----------|-------------|
| 缺页处理 | VMM 软件遍历 guest PT，慢 | 硬件自动，仅 EPT miss 进 VMM |
| guest 写 CR3 | 拦截 + 切影子表（贵） | 无需拦截（普通 mov cr3） |
| TLB miss 放大 | 1（影子表直接 GVA→HPA） | $L_g × (1+L_e)$ |
| 内存开销 | 影子表额外大 | 无额外表，但 TLB 占用更大 |
| 适用 | 无硬件辅助/嵌套场景 | 主流生产环境 |

---

## 4. GPA 的管理与 memslot 概念（浅层，实现见 04 章）

- `memslot`：一段 [gpa_base, gpa_base+size) → 宿主虚拟地址 `userspace_addr` 的映射表（由 QEMU 通过 `KVM_SET_USER_MEMORY_REGION` 提供）
- gfn→hpa 的推导：

```
gfn = GPA >> PAGE_SHIFT
slot = 查 memslot (gfn 命中区间)
hva = slot->userspace_addr + (GPA - slot->base_gfn << 12)
hpa = hva_to_pfn(hva)          # 宿主缺页可能在此发生
```

- **无映射区**：gpa 未落入任何 slot（或 slot 只有只读/无执行权限）→ EPT violation → KVM 判定 MMIO 或注入总线错误。MMIO 的 fast path：KVM 在 EPT 建一个"永久 fault"的 trap 映射（Intel 上用 EPT misconfig 技巧区分 MMIO）。

---

## 5. 大页与内存特性在虚拟化下的行为

| 特性 | guest 视角 | 宿主实际效果 |
|------|-----------|--------------|
| guest THP（2M） | guest PT 用 PDE 大页 | EPT 侧可配合建 2M 映射，TLB 放大骤降 |
| host THP | 不可见 | KVM 尝试把连续 gfn 区间映射为 2M EPT 页 |
| KSM 合并 | 不可见 | 多 VM 共享同一物理页（COW） |
| log-dirty（热迁移） | 不可见 | EPT 页写保护/位图记录脏页 |
| 内存气球 (balloon) | 驱动交还页 | QEMU `madvise(MADV_DONTNEED)` → notifier 摘 EPT 项 |

---

## 6. I/O 侧的同构问题：IOMMU（简述）

设备 DMA 地址也面临同样的两级问题：设备看到的 IOVA → HPA。IOMMU（VT-d/SMMU）提供第三套页表，权限/隔离模型与 EPT 同构；`kvm_iommu_map` 会把 guest 内存同步映射进 IOMMU。理解了 EPT，IOMMU 几乎免费理解。

---

## 7. 章末自测

1. 画出"guest 进程 load 一个 TLB miss 地址"在 EPT 开启下的完整访存序列（含大页假设两种情况）。
2. EPT violation 与 EPT misconfig 分别在什么条件下产生？KVM 对 MMIO 用了哪个技巧？
3. ARM64 中为什么 fault GPA 在 HPFAR_EL2 而不是 FAR_EL2？
4. 影子页表在"guest 执行 execve（大量建页表）"场景为什么慢？逐步列出拦截事件。
5. VMID/EPTP-ASID 缺失时（旧硬件），VM 切换的 TLB 代价是什么？KVM 如何缓解？

---

## 源码地图

| 主题 | 文件 |
|------|------|
| EPT 定义/帮助函数 | `arch/x86/kvm/mmu/`（`mmu.c`, `tdp_mmu.c`, `tdp_iter.c`） |
| 影子页表逻辑 | `arch/x86/kvm/mmu/mmu.c`（`FNAME()` 宏展开 `paging_tmpl.h`） |
| AMD NPT | `arch/x86/kvm/svm/nested.c`, `mmu` 同上 |
| ARM64 stage-2 | `arch/arm64/kvm/mmu.c`, `arch/arm64/include/asm/kvm_mmu.h` |
| memslot | `virt/kvm/kvm_main.c`（`kvm_set_memory_region`）, `include/linux/kvm_host.h` |
