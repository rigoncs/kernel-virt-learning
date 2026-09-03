# kernel-virt-learning：Linux 内核与虚拟化学习仓库（ARM64 主线）

一个自包含的 Linux 内核学习与开发入门项目，**以 ARM64（AArch64）架构为主线**，聚焦内存虚拟化与 PVM 技术，同时完整覆盖内核核心组件与开发实践。所有内容自包含，无需外部资料即可完成学习闭环。

> 目标读者：已具备 C 语言、操作系统基本概念，希望系统进入内核（尤其 ARM64 虚拟化方向）开发的工程师。
> 内核基准：5.10+ LTS（并标注更新版本演进，如 pKVM/Nested）。

## 仓库结构

```
kernel-virt-learning/
├── README.md               # 本文件：总览、学习路径、知识图谱
├── docs/
│   ├── 00-learning-path.md           # 渐进式学习路径与评估标准（ARM64 主线）
│   │
│   │ ── 第一部分：Linux 内核基座 ──
│   ├── 01-kernel-fundamentals.md     # 进程/调度/中断/文件系统/驱动
│   ├── 02-memory-management.md       # 页表数学/buddy/slab/缺页路径/rmap
│   ├── 06-debugging-tools.md         # ftrace/perf/bpftrace/crash/gdb
│   ├── 10-kernel-dev-practice.md     # 编译/模块开发/调试闭环/社区贡献
│   │
│   │ ── 第二部分：ARM64 架构与虚拟化扩展 ──
│   ├── 07-arm64-architecture.md      # ARMv8 ISA/异常级/内存模型/异常处理/ESR
│   ├── 08-arm64-virt-extensions.md   # EL2/HCR/VHE/Stage-2/NV嵌套/vGIC/vtimer/pKVM
│   │
│   │ ── 第三部分：内存虚拟化（核心主线）──
│   ├── 03-memory-virtualization.md   # 两级翻译/影子页表/EPT/NPT/Stage-2
│   ├── 04-kvm-memory-impl.md         # memslot/TDP MMU/MMU notifier（x86 实现对照）
│   ├── 05-pvm-deep-dive.md           # PVM：基于页表的软件虚拟化
│   │
│   │ ── 第四部分：KVM/ARM 实现 ──
│   └── 09-kvm-arm-impl.md            # world switch/stage-2 运行时/vGIC/调度/IO虚拟化
├── labs/
│   ├── README.md
│   ├── lab01-minimal-kvm-vm/         # x86 最小 VM（EPT violation 观测，对照用）
│   ├── lab02-ftrace-tracing/         # ftrace 追踪缺页与 KVM 路径
│   ├── lab03-mmu-notifier/           # MMU notifier 模块
│   ├── lab04-kvm-unit-tests/         # kvm-unit-tests
│   ├── lab05-pvm-experiments/        # PVM/影子页表观测
│   ├── lab06-arm64-kvm-vm/           # ★ ARM64 最小 VM：PSCI/Stage-2 fault/MMIO
│   ├── lab07-arm64-world-switch/     # ★ KVM/ARM world switch 与 vGIC 追踪
│   └── lab08-kernel-module/          # ★ ARM64 内核模块交叉编译闭环
├── resources/               # 参考资料（ARM 手册/论文/项目）
└── config/                  # 环境配置（setup.sh + env.md）
```

## 学习路径总览（五阶段，ARM64 主线）

| 阶段 | 目标 | 文档 | 实验 | 评估标准 |
|------|------|------|------|----------|
| **S1 内核基座** | 进程/调度/中断/内存核心机制 | 01, 02, 10 | lab08 | 能讲清缺页全路径与 ARM64 中断路径；独立完成模块闭环 |
| **S2 ARM 架构** | ARMv8 异常级/内存模型/异常处理 | 07 | lab07 前置阅读 | 能解码 ESR_EL2 并解释 EL0–EL3 路由规则 |
| **S3 虚拟化扩展** | EL2/VHE/Stage-2/NV/vGIC/vtimer | 08 | lab06 | 能写出 VHE 与 nVHE 的 world switch 差异；描述一次 Stage-2 fault 判定 |
| **S4 KVM/ARM 实现** | world switch/内存/中断/IO 运行时 | 09（+03/04 对照） | lab06, lab07 | 能用 ftrace 画出完整 world switch 时序图并解释 esr_ec 分布 |
| **S5 进阶专精** | 软件虚拟化/PVM/x86 对照 | 03, 04, 05 | lab01–05 | 能对比 SPT/EPT/Stage-2/PVM 四方案并说明适用场景 |

> **ARM64 优先阅读顺序**：01 → 02 → 07 → 08 → 03（§1-2 + §3.4-3.5）→ 09 → 04（对照）→ 05。
> 详细目标/自测题见 [docs/00-learning-path.md](docs/00-learning-path.md)。

## 知识关联图谱（ARM64 主线）

```
┌─────────────────────────────────────────────────────────────┐
│  Linux 内核基座                                             │
│  进程管理 ─► CPU调度(CFS/EEVDF) ─► vCPU线程模型              │
│  内存管理 ─► Stage-1页表 ─► 缺页 ─► rmap/COW                │
│  中断处理 ─► GICv3 ─► 上/下半部                             │
│  文件系统 ─► 页缓存 ─► 设备驱动 ─► /dev/kvm + virtio        │
└──────────────────────────┬──────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  ARM64 架构层 (docs/07, 08)                                 │
│  EL0–EL3 异常级 ─► EL2 ─► HCR_EL2 陷阱路由                   │
│  VHE (host 内核跑 EL2) ─► nVHE 对照                          │
│  Stage-2 翻译 (IPA→PA, VTCR/VTTBR, VMID)                    │
│  Nested Virtualization (NV/NV2) ─► pKVM                     │
│  GICv3 虚拟化 (LR 直注) ─► vtimer (CNTVOFF)                  │
└──────────────────────────┬──────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  KVM/ARM 运行时 (docs/09)                                   │
│  kvm_arch_vcpu_ioctl_run 主循环                              │
│   └► __kvm_vcpu_run → __guest_enter (ERET) → guest@EL1      │
│   ◄─ ESR_EL2 ─ handle_exit                                  │
│       ├─ kvm_handle_guest_abort (stage-2 建表/lazy mapping)  │
│       ├─ kvm_handle_mmio (ISS.SRT 数据回填)                  │
│       ├─ vgic flush/sync (LR 装载)                           │
│       └─ psci / sys_regs 模拟                                │
└──────────────────────────┬──────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  内存虚拟化方案全景 (docs/03, 05)                            │
│  SPT(影子页表) / EPT(x86) / Stage-2(ARM) / PVM(软件+PV)      │
└─────────────────────────────────────────────────────────────┘
```

**主线一句话**：内核基座（S1）→ ARM64 如何用异常级与陷阱实现虚拟化（S2/S3）→ KVM/ARM 如何把这些机制组装成运行时（S4）→ 四种内存虚拟化方案的统一视角（S5）。

## 快速开始

```bash
# 1. 配置环境
bash config/setup.sh          # Ubuntu/Debian; ARM64 交叉编译见 docs/10 §1

# 2. ARM64 主线推荐顺序
#    读 docs/07 → docs/08 → 做 lab06 (ARM64 最小 VM)
#    读 docs/09 → 做 lab07 (world switch 追踪)

# 3. 源码对照: 文档引用基于内核源码树 (../linux), 建议并排打开
```

## 核心源码目录速查（ARM64 主线）

| 目录 | 内容 | 相关文档 |
|------|------|----------|
| `virt/kvm/arm/` | KVM/ARM 通用层（arm.c 主循环、vgic/、arch_timer.c、psci.c） | docs/09 |
| `arch/arm64/kvm/` | vCPU/系统寄存器模拟、MMU、nested | docs/09 |
| `arch/arm64/kvm/hyp/` | world switch（vhe/ 与 nvhe/ 两套汇编） | docs/08, 09 |
| `arch/arm64/kernel/` | entry.S 异常入口、traps、psci | docs/07 |
| `arch/arm64/mm/` | Stage-1 页表、fault.c | docs/02 |
| `mm/` | 通用内存管理 | docs/02 |
| `kernel/` / `fs/` / `drivers/` | 调度/FS/驱动 | docs/01 |
| `virt/kvm/`（kvm_main.c） | KVM 通用层：memslot/MMU notifier | docs/04 |
| `arch/x86/kvm/mmu/` | x86 TDP MMU（对照阅读） | docs/03, 04 |
| `Documentation/virt/kvm/` | KVM 官方文档（api.rst, devices/vgic.rst） | resources |
