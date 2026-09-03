# 08 · ARM 虚拟化扩展：EL2 / VHE / Stage-2 / Nested / 中断与定时器虚拟化

> 目标：吃透 ARMv8/ARMv9 全部虚拟化硬件机制。KVM/ARM 的每一行代码都对应本章某个机制。规范依据：Arm ARM DDI 0487（Arm AXI/体系结构参考）、Arm KVM 文档。

---

## 1. EL2 与 HCR_EL2：陷阱路由总开关

EL2 提供两类能力：
1. **陷阱路由**：把 EL1/EL0 的敏感行为（系统寄存器访问、特定指令、异常、中断）重定向到 EL2
2. **Stage-2 翻译**：对 EL1/EL0 的地址做第二级翻译（§3）

`HCR_EL2`（Hyp Configuration Register）是核心位域：

| 位 | 置 1 效果（guest 视角） | KVM 用途 |
|----|------------------------|----------|
| `VM` | 开启 Stage-2 翻译 | 进入 guest 必开 |
| `IMO/FMO/AMO` | IRQ/FIQ/SError 路由到 EL2 | KVM 收物理中断 |
| `TWI/TWE` | WFI/WFE 陷出 | 闲置 vCPU 让出 CPU |
| `TVM` | 多数 MMU 系统寄存器写陷出 | 影子模式下同步（一般关闭，靠 stage-2 fault） |
| `TGE` | Trap Guest Exceptions（全路由） | 退出 guest 时用 |
| `E2H`+`TLR` | **VHE 开启**（§2） | host 内核直接跑 EL2 |
| `RW` | EL1 为 AArch64 | 64 位 guest |
| `DC` | stage-1 属性折叠为 stage-2 决定 | |
| `NV/NV1/NV2` | **嵌套虚拟化**（§5） | L1 hypervisor 支持 |

另有 `CPTR_EL2`（协处理器陷阱：FP/SVE 按需）、`SCTLR_EL2`、`VTTBR_EL2/VTCR_EL2`（§3）。

## 2. VHE（Virtualization Host Extension, ARMv8.1）

### 2.1 没有 VHE 的世界（nVHE）

```
host 内核 (EL1) ←── 切换到 EL2 运行 hyp 代码 ──→ KVM @ EL2
每次进出 guest 都要:
  1) 保存 host EL1 上下文, 加载 guest EL1 上下文
  2) 切换 TTBR0_EL1 + 设置 HCR_EL2.VM=1
  3) ERET 到 guest EL1
exit 时反向 + 手工保存全部 guest 寄存器到内存 (无 VMCS 硬件帮忙!)
```

问题：host 内核原本跑 EL1，访问不到 EL2 寄存器；每次 world switch 状态量巨大（ARM 没有 VMCS 这种硬件控制块，上下文全靠软件管理）。

### 2.2 VHE：让 host 内核直接跑在 EL2

ARMv8.1 起（`ID_AA64MMFR1_EL1.VH=1`），`HCR_EL2.E2H=1` 时：

- EL2 获得完整 EL1 视图：`TTBR0_EL2`/`SPSR_EL2` 等寄存器的访问被**重定向/别名**为 EL1 形式（`TTBR0_EL1` 在 EL2 访问实际操作的是 EL2 硬件）
- host 内核**本身运行在 EL2**，零成本访问 EL2 工具集
- 进入 guest：只改 `HCR_EL2.VM` + 切 `VTTBR_EL2` + 少量寄存器，上下文切换量大减

```
nVHE: host(EL1) ──保存host──► hyp(EL2) ──加载guest──► guest(EL1)
VHE : host(EL2) ──仅需切 VM 位与 vcpu 寄存器──► guest(EL1)
```

内核判定：`has_vhe()`（`cpus_have_const_cap(ARM64_HAS_VHOST)`）；world switch 实现二选一：`arch/arm64/kvm/hyp/nvhe/` vs `vhe/`。**同一套逻辑两份汇编**（见 [09 §2](09-kvm-arm-impl.md)）。

## 3. Stage-2 翻译（IPA→PA）

### 3.1 控制与基址寄存器

```
VTTBR_EL2: BADDR(stage-2 表基址) | VMID(8/16位, TLB tag)
VTCR_EL2:
  T0SZ  : IPA 有效位宽 (40 位 → T0SZ=24)
  SL0   : 起始层级 (4KB: 0=从L0开始三级, 1=从L1开始) —— KVM 按配置 3 或 4 级
  SH0/IRGN0/ORGN0 : stage-2 表自身的一致性
  TG0   : 粒度 (4K/16K/64K)
```

### 3.2 描述符与权限合成

Stage-2 表结构与 Stage-1 同构（L0–L3 + Block/Page），但描述符字段不同：

```
Stage-2 (L3 page 描述符):
63       53 52        10 9 8 7 6 5  4 3 2 1 0
┌──────────┬────────────┬─────────┬──────────┬─────┬───┐
│ SW 拥有位 │ 输出地址(PA) │XN[1:0] │Contiguous│Attr │ AF│ 1 │
└──────────┴────────────┴─────────┴──────────┴─────┴───┘
  XN[1:0]: XN(0) 管 EL0, XN(1) 管 EL1 —— 可分别设置 guest 用户/内核不可执行
  Attr: 两位组合 (基于 HMAIR_EL2 或强制 Normal)
  AF (Access Flag): 硬件访问标志
```

**权限合成公式**（两级正交，最终权限 = 交集）：

```
W_eff = W_stage1 ∧ W_stage2      (S2AP[1])
X_eff = X_stage1 ∧ X_stage2.XN   (且区分 EL0/EL1!)
MType_eff = 组合表 (ARM DDI 0487 Table G8-6-7: Device 优先级最高)
```

例：guest 想把一段内存标记 Device（MMIO 直通）——Stage-1 属性 Device + Stage-2 属性 Normal → 组合结果 **Device**。但若 Stage-2 拆分为只读 → 写即 fault，KVM 可借此实现 log-dirty。

### 3.3 Stage-2 fault 判定（对 KVM 至关重要）

```
Data Abort 到 EL2, ESR_EL2.ISS:
  S1PTW=1        → fault 在 walk guest stage-1 表时发生 (guest 页表页被 stage-2 拦下)
  ISS.BIT39 (LL) → 2 级表本身 fault? (细分)
  EA/CM          → 外部中止 / cache maintenance 触发
HPFAR_EL2 = 故障 IPA >> 12   (valid 除非 FnV)
```

KVM 的分派（`kvm_handle_guest_abort()`）：先查 fault IPA 是否命中 memslot——命中则建 stage-2 映射（`user_mem_abort`）；未命中判 MMIO（若 S1PTW/未对齐/指令取指等还可能是 guest 自身 bug → 注入 guest 异常）。

### 3.4 VMID 与 TLB

- TLB entry tag: (VMID, ASID, VA)；VM 切换只需换 VTTBR_EL2，不同 VMID 互不污染
- 8 位 VMID（256 个 VM）会回绕，KVM 维护 per-CPU `vmid generation`（`kvm_vmid`），回绕时全量 `TLBI VMALLS12E1IS`
- 内核 5.10+ 支持 16 位 VMID（`VTCR_EL2.VS`，`ARM64_HAS_..._VMID16`）

### 3.5 大页（Block Descriptor）

4KB 粒度下 L1 block = 1GB、L2 block = 2MB。KVM 在 `user_mem_abort` 中：若 host 侧是 PMD 大页（THP/匿名大页）且 fault IPA 按 2MB 对齐 → 直接建 L2 block；脏页跟踪/只读需求时拆回 4K。对应 x86 EPT 大页策略（[04 §3.4](04-kvm-memory-impl.md)）。

## 4. pKVM（protected KVM，5.15+ 演进）

- 动机：EL2 是最高特权，KVM bug = 全系统沦陷。pKVM 把 hyp 代码变为**host 内核不可访问**的受保护 EL2（pVM firmware），host 只能通过受控 hypercall 接口操作 guest
- 关键机制：stage-2 表所有权移交 hyp；host 访问 guest 内存须经 hyp 校验；`kvm_call_hyp()` 受限接口
- 内核位置：`arch/arm64/kvm/hyp/nvhe/`（pKVM 复用 nVHE 代码路径）+ `Documentation/virt/kvm/arm/*/`
- 学习价值：理解"EL2 软件与 host 内核的信任边界"这一前沿工程实践

## 5. 嵌套虚拟化（Nested Virtualization, ARMv8.3/8.4 NV/NV2）

### 5.1 问题：L1 也是 hypervisor，也要 EL2

传统方案：KVM 模拟 EL2（trap-and-emulate L1 的所有 EL2 操作，开销极大）。

### 5.2 NV 硬件方案（`ID_AA64MMFR2_EL1.NV=1`）

```
L0 KVM (真实 EL2)
 └─ L1 guest (EL1) —— HCR_EL2.NV=1 时 L1 的 EL2 寄存器访问重定向到
     其"虚拟 EL2"寄存器组 (寄存在内存: vcpu_sys_reg[])
     EL2 指令 (ERET/TLBI/HVC@EL2...) 也被陷阱化
L2 guest: L1 配置的 stage-2 与 L0 的 stage-2 串联 —— 三级翻译!
```

### 5.3 NV2 优化

`NV2`（Arm v8.4+）：把 L1 的 EL2 系统寄存器**直接映射到 VNCR_EL2 指向的内存区**，读多数 EL2 寄存器不再陷出（软件改为读内存），大幅降陷阱率。内核主线对 NV 的支持自 5.17 起逐步成熟（`arch/arm64/kvm/nested.c`）。

## 6. 中断虚拟化：GICv3/v4 与 vGIC

### 6.1 GICv3 硬件结构

```
Distributor (GICD)     ← 全局 SPI (共享外设中断) 分发
Redistributor (GICR)   ← 每 CPU 一个: LPI (消息中断, ITS 生成), PPI/SGI 配置
CPU Interface (ICC_*_EL1) ← priorities, EOI, ACK
```

### 6.2 vGIC：KVM 的三层

```
guest 视角            KVM 模拟                     硬件
─────────────────────────────────────────────────────────────
GICD/GICR regs  ←→  vgic-v2/v3 (kvm/vgic/)         (真实 GIC 由 host 占用)
虚拟中断到达     ←── List Registers (LRs, GICH)   ←── host 注入: 直接写 LR!
EOI            ←── IAR/EISR 中断号在 LR 中被标记 ← 硬件自动维护 (EOI 无需退出!)
```

关键点（对比 x86 必须每次中断 VM-exit）：
- **LR（List Register）**：GIC 虚拟化寄存器组（`ICH_LR<n>_EL2`），host 写入"中断号+优先级+vCPU+状态"，**硬件直接把中断递给 guest，无需退出**
- **维护中断**（`ICH_MISR_EL2`）：LR 被消费完/EOI 需要软件介入时硬件通知 EL2——多数 EOI 走硬件 fast path
- **GICv4 直接注入**（`ITS`+`VLPI`）：直通设备（VFIO/SMMU）中断经 GICv4 绕过 KVM 直接进 guest LR

### 6.3 陷阱位

`ICH_HCR_EL2` 控制 vGIC 行为；KVM 触发中断注入时机：guest 被抢占、hypercall 请求、MMIO 写 GICD 等。

## 7. 定时器虚拟化

```
物理计数器:   CNTVCT_EL0 (全局单调, 所有 CPU 共享)
虚拟计数器:   CNTVCT_EL0 减去 CNTVOFF_EL2  ← KVM 给每个 vCPU 配 offset
虚拟定时器:   CNTV_CVAL_EL0 / CNTV_CTL_EL0 —— 到期产生 vtimer 中断 (PPI 27)
```

KVM 侧（`arch/arm64/kvm/arch_timer.c`）：
- guest 读时钟 = 物理计数器 − vCPU offset（一次校准即可，运行时零成本）
- vtimer 到期：中断路由到 EL2（`CNTHCTL_EL2.EL1PCEN/EL1PCTEN` 控制），KVM 判断"到期时刻属于哪个 vCPU"→ 写 hrtimer + 注入 vGIC
- 迁移时把 vtimer 状态（cval/ctl/offset）打包走（`KVM_REG_ARM_TIMER`）

## 8. 其他相关扩展速览

| 扩展 | 版本 | 虚拟化角色 |
|------|------|-----------|
| FPSIMD/SVE | v8 base / v8.2 SVE | `CPTR_EL2` 按需陷阱；KVM 延迟切换 FPSIMD（避免每次 world switch 保存 128B×32） |
| Pointer Auth | v8.3 | 寄存器只陷出 keys，不陷出计算 |
| MTE (内存标签) | v8.5 | stage-2 需配 tag 大小（5.10 尚不支持 guest MTE） |
| RAS / SError | v8.2 | `HCR_EL2.AMO` 路由，KVM 需区分 host/guest SError |
| SME | v9 | SVE 的矩阵扩展，陷阱模型类似 |

## 9. 自测题

1. VHE 下进入 guest 需要写哪几个寄存器？与 nVHE 差多少？
2. guest 页表页自身访问被 Stage-2 拦截时（S1PTW=1），KVM 应建映射还是注入 guest fault？判据是什么？
3. VMID 回绕（wrap-around）时 KVM 做什么？为什么不能只用 8 位 VMID 就一劳永逸？
4. GICv3 LR 机制如何做到"EOI 不退出"？哪些情况仍需 KVM 介入？
5. CNTVOFF_EL2 的存在为什么让"guest 读时钟"几乎零成本？迁移时它如何处理？

## 源码地图

| 主题 | 文件 |
|------|------|
| HCR 配置 | `arch/arm64/kvm/hyp/vhe/nvhe/switch.S` (`__guest_enter` 前) |
| Stage-2 表管理 | `arch/arm64/kvm/hyp/pgtable.c`（hyp 端）、`arch/arm64/kvm/mmu.c`（host 端） |
| VMID | `arch/arm64/kvm/arm.c` (`kvm_arm_vmid_update`) |
| 嵌套 | `arch/arm64/kvm/nested.c` |
| vGIC | `virt/kvm/arm/vgic/`（vgic-v3.c, vgic-v2.c, vgic-mmio.c） |
| 定时器 | `virt/kvm/arm/arch_timer.c` |
| VHE/nVHE 切换 | `arch/arm64/kvm/hyp/` |
