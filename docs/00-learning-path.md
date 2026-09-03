# 00 · 学习路径与评估标准（ARM64 主线）

本仓库采用 **五阶段渐进式路径**（内核基座 → ARM 架构 → 虚拟化扩展 → KVM/ARM 实现 → 进阶专精）。每阶段给出：学习目标、必读材料、实验、预期成果、评估标准（含自测题）。

> 内核基准 5.10+ LTS；涉及新版本演进（pKVM、Nested NV、16 位 VMID 等）均在文档中标注。

---

## 阶段 S1：内核基座（核心组件 + 开发实践）

### 学习目标
- 进程生命周期（fork/exec/exit）、`task_struct`、上下文切换
- CPU 调度（CFS → EEVDF）与 vCPU 线程模型的关系
- **中断处理**：上半部/下半部、ARM64 GICv3 路径（ACK/EOI/softirq）
- 内存管理主干：Stage-1 页表数学、buddy/slab、缺页路径、rmap/COW
- 开发实践：交叉编译内核、模块开发、QEMU+GDB 调试闭环

### 必读材料
| 章节 | 内容 |
|------|------|
| [01-kernel-fundamentals.md](01-kernel-fundamentals.md) | 进程/调度/**中断(§5)**/文件系统/驱动 |
| [02-memory-management.md](02-memory-management.md) | 页表公式/缺页全路径/rmap |
| [10-kernel-dev-practice.md](10-kernel-dev-practice.md) | 编译/模块/调试/贡献 |

### 配套实验
- [lab08-kernel-module](../labs/lab08-kernel-module)：ARM64 模块交叉编译完整闭环

### 评估标准（自测）
1. ARM64 一次外设中断的完整路径（GIC → entry.S → hardirq → softirq）？
2. 缺页中断中"写 mmap 私有只读页"走哪条路径？COW 的判定条件？
3. 模块 `insmod` 报 Invalid module format 的三种可能原因？
4. 为什么调试内核要 `nokaslr`？

---

## 阶段 S2：ARM64 架构（虚拟化开发者视角）

### 学习目标
- 异常级模型 EL0–EL3 与异常路由规则
- AArch64 指令集最小集（MRS/MSR/ERET/HVC/WFI…）
- 内存模型：MAIR/Shareability/Stage-1 描述符
- **异常处理机制**：ESR_EL2 的 EC/ISS 解码、向量表、FAR/HPFAR 的区别
- PSCI/SMCCC 调用约定

### 必读材料
[07-arm64-architecture.md](07-arm64-architecture.md) 全篇。

### 评估标准（自测）
1. guest 执行 `MSR TTBR0_EL1, X0` 需要 HCR_EL2 哪个位？ESR_EC 是多少？
2. HPFAR_EL2 何时无效？KVM 的应对？
3. `HVC`/`SMC`/`SVC` 的目标异常级分别是什么？
4. MAIR 与 MTRR/PAT 的本质区别？

---

## 阶段 S3：ARM 虚拟化扩展

### 学习目标
- EL2 与 HCR_EL2 陷阱路由（VM/IMO/FMO/TWI/NV… 各位含义）
- **VHE 与 nVHE 的 world switch 差异**（本阶段核心）
- Stage-2 翻译：VTCR/VTTBR、描述符、权限合成公式、VMID/TLB
- 嵌套虚拟化 NV/NV2；pKVM 的信任边界
- GICv3 虚拟化（LR 直注、maintenance interrupt）与 vtimer（CNTVOFF）

### 必读材料
[08-arm64-virt-extensions.md](08-arm64-virt-extensions.md) 全篇。

### 配套实验
- [lab06-arm64-kvm-vm](../labs/lab06-arm64-kvm-vm)：ARM64 最小 VM（`KVM_ARM_VCPU_INIT`、PSCI、MMIO/ISS 数据回填）

### 评估标准（自测）
1. 默写 VHE 模式进入 guest 需写的寄存器集合；对比 nVHE 多出哪些步骤？
2. Stage-2 权限合成公式；guest 想把内存标 Device 但 stage-2 是 Normal，结果？
3. VMID 回绕时 KVM 做什么？
4. vGIC 如何做到 EOI 不退出？哪些情况仍陷出？
5. `KVM_ARM_VCPU_INIT` 不调用直接 `KVM_RUN` 会怎样？（lab06 亲手验证）

---

## 阶段 S4：KVM/ARM 实现

### 学习目标
- 主循环与 **world switch**（VHE/nVHE 两套汇编、无 VMCS 的软件上下文管理）
- vCPU 生命周期与 PSCI 上电路径
- Stage-2 运行时：`kvm_handle_guest_abort` 全链、lazy mapping、大页、卸载/失效
- vGIC 运行时（flush/sync_hwstate）与虚拟化调度（WFI 陷阱、steal time、vCPU 绑核）
- I/O 虚拟化：MMIO 模拟（ISS 优势）、virtio-mmio、直通（SMMU+VFIO+GICv4）
- 系统寄存器模拟（CPU 特性呈现中枢）

### 必读材料
| 章节 | 内容 |
|------|------|
| [09-kvm-arm-impl.md](09-kvm-arm-impl.md) | 全篇（核心） |
| [03-memory-virtualization.md](03-memory-virtualization.md) | §1–2、§3.4–3.5（ARM 视角的内存虚拟化理论） |
| [04-kvm-memory-impl.md](04-kvm-memory-impl.md) | 对照阅读：memslot/MMU notifier 与 x86 TDP MMU |

### 配套实验
- [lab07-arm64-world-switch](../labs/lab07-arm64-world-switch)：world switch/vGIC 追踪 + GDB 单步
- lab06 进阶 B：PSCI CPU_ON 多 vCPU

### 评估标准（自测）
1. 画出 VHE 模式一次 MMIO 读的完整路径（标注每步异常级）。
2. `esr_ec=0x24` 与 `0x18` 的处理函数分别是哪个？S1PTW=1 时 KVM 如何决策？
3. 为什么 stage-2 表是 per-VM？vCPU 被 CFS 抢占时 guest 感知到什么？
4. KVM/ARM 中断注入为什么可以零 VM exit？对照 x86 说明。

---

## 阶段 S5：进阶专精（软件虚拟化 / PVM / x86 对照）

### 学习目标
- 影子页表（SPT）同步协议与开销模型
- EPT/NPT 硬件二级翻译（x86 视角，作为 Stage-2 的对照）
- KVM x86 内存子系统（TDP MMU、MMU notifier——通用机制两架构共享）
- PVM：基于页表的软件虚拟化（半虚拟化 + 宿主侧影子页表 + 对宿主透明）

### 必读材料
| 章节 | 内容 |
|------|------|
| [03-memory-virtualization.md](03-memory-virtualization.md) | §2 影子页表、§3 硬件方案全貌 |
| [04-kvm-memory-impl.md](04-kvm-memory-impl.md) | memslot/TDP MMU/MMU notifier |
| [05-pvm-deep-dive.md](05-pvm-deep-dive.md) | PVM 统一模型与方案对比 |

### 配套实验
lab01（x86 对照）、lab02、lab03（MMU notifier，通用）、lab04、lab05

### 评估标准（自测）
1. 完成四方案对比表：SPT vs EPT vs ARM Stage-2 vs PVM（翻译级数/拦截开销/内存/隔离模型/适用场景）。
2. SPT 时代 fork 密集负载为何崩溃性慢？PVM 用什么手段消除？
3. memslot 为何用 RCU 双缓冲？MMU notifier 保证什么因果性？
4. 什么负载下 PVM 优于硬件方案？什么场景绝不能用 PVM（威胁模型）？

---

## 知识点关联图谱（细粒度，ARM64 主线）

```
 内核基座                    ARM64 架构                KVM/ARM 运行时
 ─────────                   ─────────                ─────────────
 进程/调度 ─► vCPU 线程 ─────────────────────────────► kvm_arch_vcpu_ioctl_run
 中断处理 ─► GICv3 ─► ┌── vGIC (LR 直注) ──────────► vgic flush/sync
 缺页/rmap ─────────┐ │
 Stage-1 页表 ──────┼─┴► EL2/HCR 陷阱路由 ─► ESR_EL2 ─► handle_exit
                    │        │
                    │        ├─ VHE ─► __guest_enter (ERET)
                    │        ├─ Stage-2 (VTTBR/VTCR/VMID) ─► kvm_handle_guest_abort
                    │        ├─ vtimer (CNTVOFF) ─► arch_timer.c
                    │        └─ NV/NV2 嵌套 / pKVM
                    │
                    └─► 通用 KVM 层: memslot(RCU) / MMU notifier / gfn_to_pfn
                              │
                              ▼
                    内存虚拟化方案: SPT / EPT / Stage-2 / PVM
```

## 建议节奏（以周为单位，可伸缩）

| 周 | 内容 |
|----|------|
| 1 | docs/01 + docs/02；lab08（模块闭环） |
| 2 | docs/10（编译自编内核 + GDB）；完成 S1 自测 |
| 3 | docs/07（ARM 架构）；手算 ESR 解码练习 |
| 4 | docs/08（虚拟化扩展）；lab06（重点投入） |
| 5 | docs/09 §1–4；lab07 Part A/B |
| 6 | docs/09 §5–8；lab07 Part C/D；完成 S4 自测 |
| 7 | docs/03 + docs/04（对照 x86）；lab03 |
| 8 | docs/05（PVM）+ lab05 + 四方案对比表；完成 S5 自测 |

> 每阶段结束把自测答案写入自建 `notes/` 目录；能脱稿讲清才算掌握。
