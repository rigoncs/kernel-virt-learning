# 09 · KVM/ARM 实现剖析：world switch、内存、中断、调度与 I/O

> 前置：[07](07-arm64-architecture.md)、[08](08-arm64-virt-extensions.md)。本章把硬件机制串成 KVM/ARM 的完整运行时。基于 5.10+ 内核结构（pKVM/Nested 部分标注新版本演进）。

---

## 1. 总体架构

```
用户态 QEMU                         内核
┌─────────────┐   KVM_CREATE_VM    ┌────────────────────────────────┐
│ vCPU 线程    │──KVM_ARM_VCPU_INIT► struct kvm_vcpu (arch/arm64)   │
│ KVM_RUN     │───KVM_RUN────────► kvm_arch_vcpu_ioctl_run (arm.c)  │
│             │                    │   └► vcpu_load/load_vcpu       │
│             │                    │   └► __kvm_vcpu_run ──┐        │
│             │◄──exit────────────── ┌─────────────────────┼──────┐ │
└─────────────┘                    │ │ EL2 (VHE or nVHE):  │      │ │
                                   │ │  __guest_enter      ▼      │ │
                                   │ │   guest @ EL1 ◄──────      │ │
                                   │ └────────────────────────────┘ │
                                   │ host 端: handle_exit           │
                                   │  ├─ handle_exit_sync (MMIO等)  │
                                   │  ├─ kvm_handle_guest_abort     │
                                   │  └─ vgic/timer/psci 处理       │
                                   └────────────────────────────────┘
```

目录分工（源码树）：
- `virt/kvm/arm/`：ARM 通用 KVM 层（arm.c 主循环、vgic/、arch_timer.c、psci.c、mmio.c）
- `arch/arm64/kvm/`：vCPU/系统寄存器模拟（sys_regs.c）、MMU（mmu.c）、hyp/
- `arch/arm64/include/asm/kvm_*`：数据结构（kvm_host.h、kvm_emulate.h、kvm_arm.h）

---

## 2. World Switch（核心中的核心）

### 2.1 主循环

```
kvm_arch_vcpu_ioctl_run()   [virt/kvm/arm/arm.c]
  ├─ ret < 0 / kvm_request_pending? → 处理请求 (TLB flush, timer 等)
  ├─ preempt_disable / local_irq_disable
  ├─ kvm_vgic_flush_hwstate()      # 把 pending 虚拟中断装进 LR
  ├─ kvm_timer_flush_hwstate()
  ├─ __kvm_vcpu_run()              # hyp 入口 (VHE: vhe/ dir; nVHE: nvhe/)
  │    └─ __guest_enter(vcpu)      # 汇编: 加载 guest 寄存器, ERET ↓
  │         ... guest 运行 ...
  │    ◄─ 异常到 EL2, __guest_exit 保存 guest 寄存器, 返回 exit_code
  ├─ kvm_timer_sync_hwstate() / kvm_vgic_sync_hwstate()
  └─ handle_exit() → 决定 ret (>0 继续 / 0 退到用户态 / <0 错误)
```

### 2.2 VHE vs nVHE 的两套汇编

| 文件 | 模式 | 要点 |
|------|------|------|
| `hyp/vhe/switch.c` + `hyp.S` | VHE | host 已在 EL2；`__guest_enter` 仅保存 callee 寄存器 + 设 `HCR_EL2` guest 值 + 切 `VTTBR_EL2` + ERET |
| `hyp/nvhe/switch.c` + `hyp.S` | nVHE | 需额外保存/恢复 host EL1 全量上下文（TTBR0/EL1 寄存器组） |
| `hyp/nvhe/`（5.15+ 扩展） | pKVM | hyp 独立映射、host 无法访问其内存 |

**关键认知（对照 x86）**：ARM **没有 VMCS**——guest 通用寄存器在 `__guest_enter/__guest_exit` 汇编里手工压栈/出栈（`vcpu->arch.ctxt.regs`，即 `struct user_pt_regs`）；系统寄存器上下文存于 `vcpu->arch.ctxt.sys_regs[]`（数组索引即 `sys_regs.h` 的枚举）。world switch 成本 ≈ 30~60 个通用寄存器存取 + 少量系统寄存器——比 x86 VMCS swap 略快但全部软件可见。

### 2.3 上下文一览

```
struct kvm_vcpu_arch {
    struct kvm_cpu_context ctxt;   // gp_regs + sys_regs + fpsimd
    struct kvm_mmu mu;  (via kvm_vcpu_arch 或 vcpu->arch)
    u64 hcr_el2;                  // 每个guest的HCR配置 (TPIDR等差异)
    u64 cptr_el2, vttbr_el2;
    struct kvm_timer timer;       // vtimer 状态
    struct vcpu_reset_state reset_state;
    bool pause; / power_off;
    ...
}
```

---

## 3. vCPU 生命周期与 PSCI 上电

```
QEMU: KVM_CREATE_VCPU (每 vCPU 一个线程)
  → KVM_ARM_VCPU_INIT  # 指定 target (CPU type/features), 初始化 reset 状态
  → vCPU0 直接从 kernel entry 开始; vCPU1..N 处于 power_off 状态
  → guest 内核跑 PSCI CPU_ON(affinity, entry, ctx) (HVC/SVC 陷出到 EL2)
  → kvm_psci_call() [virt/kvm/arm/psci.c]
      → 找到目标 vcpu, 设置 reset_state.entry = entry
      → kvm_make_request(KVM_REQ_VCPU_RESET); vcpu kick (唤醒其线程)
  → 目标 vCPU 线程在 __kvm_vcpu_run 前消费 RESET 请求 → 从 entry 运行
```

**注意**（硬约束，来自项目实践）：`KVM_ARM_VCPU_INIT` 必须在 vCPU 创建后、`KVM_RUN` 前调用；`target` 决定 `ID_AA64*` 系统寄存器的模拟值。

---

## 4. 内存虚拟化（ARM 版全景）

### 4.1 Stage-2 表的归属与生命周期

- **per-VM 资源**（`kvm->arch.mmu`），同 VM 所有 vCPU 共享（对照：vCPU 寄存器是 per-vCPU）
- 首次 guest 访问某 IPA → Stage-2 Data Abort → 延迟建立映射（lazy mapping）

### 4.2 fault 处理全链（对照 [04 §3.4](04-kvm-memory-impl.md) 的 x86 版）

```
guest 访存 → (stage-1 由 guest 硬件完成) → stage-2 miss/权限不足
  → Data Abort (ESR_EC=0x24/0x25, ISS.S2) 到 EL2
  → __guest_exit → handle_exit → kvm_handle_guest_abort()  [arch/arm64/kvm/mmu.c]
      ├─ fault_ipa = HPFAR_EL2 << 12 (或手工 walk)
      ├─ 若 S1PTW: fault 在 guest 页表遍历中 → 通常是 guest 页表页未映射 → 建映射
      ├─ gfn = fault_ipa >> PAGE_SHIFT; 查 memslot
      │    命中:
      │      hva = gfn_to_hva_memslot(); hva_to_pfn (可能嵌套 host 缺页)
      │      判 MMIO / RAM / 大页 (host PMD → 2MB block desc)
      │      kvm_pgtable_stage2_map (hyp 端 API) 填描述符 (含 memattr 合成)
      │    未命中:
      │      判定 MMIO → kvm_handle_mmio (模拟, 见 §7) 或注入 guest abort
  → ERET 回 guest 重执行
```

### 4.3 Stage-2 的卸载与失效

- `kvm_unmap_stage2_range`（memslot 变更 / MMU notifier / reset）
- `kvm_call_hyp(__kvm_tlb_flush_vmid_* )`：hyp 端 TLBI（VMID 定向）
- MMU notifier 与 x86 完全同源（`virt/kvm/kvm_main.c` 通用层），ARM 特有：`kvm_age_hva` 映射到 stage-2 AF 位清除

---

## 5. vGIC 运行时（vGICv3 为主线）

```
QEMU: KVM_CREATE_DEVICE (KVM_DEV_TYPE_ARM_VGIC_V3)
  → KVM_DEV_ARM_VGIC_GRP_ADDR (设置 vgic 寄存器空间 GPA)
  → KVM_DEV_ARM_VGIC_GRP_NR_IRQS / CPU Interface / redist 区域
  → KVM_DEV_ARM_VGIC_GRP_CTRL (INIT: 分配 vgic 数据结构)

guest 覆盖 GICD/GICR 寄存器访问 → stage-2 fault (这些 GPA 未注册为 RAM)
  → kvm_handle_mmio → vgic_mmio_* 读写模拟 (vgic-mmio.c: 状态存 vgic_irq)
注入路径:
  QEMU/直通设备 → kvm_vgic_inject_irq()
    → 设 pending; 若 vCPU 在运行 → kvm_vgic_flush_hwstate 把它装进 ICH_LR<n>_EL2
    → 硬件直接给 guest 发虚拟中断 (无 VM exit!)
EOI:
  guest 写 ICC_IAR/EOIR → ICH_HCR_EL2.EOImode 配置下多数情况硬件完成
  需软件介入时: maintenance interrupt → EL2 → vgic 状态机更新
```

vGIC 三个视图：`vgic_irq`（per-interrupt 逻辑状态）→ `vgic_cpu`（per-vCPU 的 LR 队列）→ 硬件 LR（运行中快照）。`flush/sync_hwstate` 在每次 world switch 前后同步"内存状态 ↔ 硬件 LR"。

---

## 6. 虚拟化调度：vCPU 即线程（+两处硬件耦合）

1. vCPU = host 线程 → CFS/EEVDF 调度（[01 §2](01-kernel-fundamentals.md)）；被抢占即退出 guest，回来继续
2. 硬件耦合一：**定时器**——vtimer 到期用 hrtimer 抢回 vCPU 线程注入中断
3. 硬件耦合二：**WFI 陷阱**——guest idle 执行 WFI → 陷出 → KVM 无 pending 中断则 `kvm_vcpu_block()`（waitqueue 睡眠让出 CPU）→ 中断到达被唤醒
4. steal time：`KVM_CAP_ADJUST_STEAL_TIME`（PARAVIRT）、半虚拟化 spinlock（pvlockops）减轻锁持有者被抢占的自旋浪费
5. 亲和性：vCPU 线程可绑核（`taskset`）；KVM 侧每 CPU 缓存 `kvm_arm_hw/pmu`（FPSIMD/定时器 per-CPU 状态）要求进出 guest 在同一 CPU 完成（`vcpu_load/put` 用 preempt_notifier 保证）

---

## 7. I/O 虚拟化（ARM 路径）

### 7.1 MMIO 模拟（默认路径）

```
guest 访问未映射 IPA → stage-2 fault → kvm_handle_mmio
  → 解析 ISS: WnR/SAS/SRT (把模拟结果写回 guest X<SRT> 寄存器!)
  → exit 到 QEMU: run->mmio.{phys_addr,data,is_write}
  → QEMU 设备模拟 (如 virtio-mmio) → KVM_RUN 继续
```

关键细节：`run->mmio.data` 由内核侧填好（KVM 用 ISS 中的 SAS/SSE 完成数据搬移），QEMU 无需解码访存指令——**ARM 的 syndrome 信息让 MMIO 模拟比 x86（需手工解码指令）干净得多**。

### 7.2 virtio / virtio-mmio

ARM guest 无 legacy PIO 概念，全部走 MMIO：`virtio-mmio`（简单）或 PCI（`gpex` host bridge + `virtio-pci`）。高速路径：vhost（内核线程处理 virtqueue）或 vDPA；中断经 GICv4 ITS 直接注入。

### 7.3 设备直通：SMMU（=IOMMU）+ VFIO

```
直通设备 DMA 发 IOVA → SMMU stage-2 翻译 (硬件, 与 CPU stage-2 同构!) → PA
VFIO: 建立stage-2映射 == kvm_s2 (经由 VFIO-IOMMU接口与 kvm iommu 联动)
中断: GICv4 ITS + VLPI → 免 KVM 介入直接投递
```

---

## 8. 系统寄存器模拟（ARM 特有大项）

guest 读 `ID_AA64MMFR0_EL1`、`CTR_EL0`、`MPIDR_EL1` 等 → `MRS` 陷阱（EC=0x18/0x19）→ `kvm_handle_sys_reg()`（`arch/arm64/kvm/sys_regs.c`）→ 查 `sys_reg_desc` 表：

```
每个 sys_reg_desc = { Op0/Op1/CRn/CRm/Op2 编码, 读回调, 写回调, reset 值 }
  ID 寄存器: 从 vcpu->kvm->arch.vmid / cpu_type 特性集合返回 (决定 guest 可见 ISA)
  TPIDR 等: 直通 vcpu->arch.sys_regs[...]
  未定义: 注入 UNDEF 异常给 guest
```

这是 KVM/ARM "CPU 特性呈现"的中枢（`KVM_ARM_VCPU_INIT` 的 target + features 决定初始值）。

---

## 9. 与 x86 KVM 的差异总表

| 维度 | x86 (KVM/VMX) | ARM64 (KVM/ARM) |
|------|---------------|------------------|
| 上下文载体 | VMCS（硬件） | 软件结构 + 汇编手工切换 |
| 退出原因 | exit_reason | ESR_EL2 EC/ISS |
| guest 缺页 | EPT violation → KVM_EXIT_MMIO/内部处理 | Stage-2 Data Abort → `kvm_handle_guest_abort` |
| MMIO 数据 | 内核手工解码指令 | ISS 直接给出 SRT/SAS |
| 中断注入 | 必 VM-exit | LR 直注（可零退出） |
| 时钟 | TSC offset + kvmclock | CNTVOFF + vtimer |
| idle | hlt 陷阱 | WFI 陷阱 |
| 多 vCPU 启动 | INIT-SIPI-SIPI | PSCI CPU_ON |
| 特性呈现 | CPUID 模拟 | sys_regs 模拟 |
| 安全世界 | TDX/SEV | pKVM / Realm (RME, CCA) |

## 10. 自测题

1. 描述 VHE 模式下一次 MMIO 读的完整路径（guest 指令 → QEMU 返回数据），标注每步所在的异常级。
2. `kvm_vgic_flush_hwstate` 与 `sync_hwstate` 为什么必须在关抢占临界区内？
3. vCPU 线程被 CFS 抢占时，guest 会感知到什么？steal time 如何让 guest 内核"知道"？
4. 为什么 stage-2 表是 per-VM？若 per-vCPU 会浪费什么、引入什么问题？
5. 对比 x86 KVM，说明 KVM/ARM 中断注入路径为什么可以完全避免 VM exit。

## 源码地图（按阅读顺序）

1. `virt/kvm/arm/arm.c` — 主循环与 vCPU 管理
2. `arch/arm64/kvm/hyp/vhe/hyp.S` + `switch.c` — world switch（精读汇编）
3. `arch/arm64/kvm/handle_exit.c` — exit 分发
4. `arch/arm64/kvm/mmu.c` — stage-2 全部
5. `virt/kvm/arm/vgic/vgic-v3.c` + `vgic.c` — vGIC
6. `virt/kvm/arm/arch_timer.c` — 定时器
7. `virt/kvm/arm/psci.c`、`mmio.c` — PSCI 与 MMIO
8. `arch/arm64/kvm/sys_regs.c` — 系统寄存器模拟
9. `arch/arm64/kvm/nested.c`（新版本） — 嵌套
