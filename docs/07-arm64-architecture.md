# 07 · ARM64（AArch64）架构基础——虚拟化开发者视角

> 目标：建立 ARMv8/ARMv9 的执行状态、异常级、内存模型与异常处理模型。一切虚拟化扩展（EL2/VHE/Nested）都建立在这些机制上。规范依据：Arm ARM (DDI 0487) 最新版；内核行为基于 5.10+ LTS 及更新版本。

---

## 1. 执行状态与寄存器

ARMv8 起 AArch64 与 AArch32 并存（本仓库聚焦 AArch64）。

```
通用寄存器: X0–X30 (64位), Wn 为低 32 位视图
  · X29 = FP (帧指针), X30 = LR (链接寄存器)
特殊寄存器:
  · PC, PSTATE (NZCV/DAIF/EL/SP 等), SP_EL0..SP_EL3
  · ELR_ELx (异常返回地址) + SPSR_ELx (异常前 PSTATE)
零寄存器: XZR/WZR (读为 0, 可作目标寄存器丢弃结果)
```

PSTATE 关键位（虚拟化高频出现）：

| 位 | 含义 | 虚拟化角色 |
|----|------|-----------|
| `EL[2:0]` | 当前异常级 | VHE 判断 |
| `DAIF` | 中断/异常掩码 | vGIC 注入时机 |
| `IL` | 非法执行状态 | 异常注入 |
| `SS` | 单步 | 调试 |

## 2. 异常级（Exception Level）模型

```
EL0   用户应用
EL1   操作系统内核（guest 内核也运行于此）
EL2   Hypervisor（KVM 的栖身之所）
EL3   Secure Monitor（TrustZone, ATF/TF-A）
```

规则：
- 只能"同级"或"更高"异常级触发异常（EL0 不能直接陷出到 EL0）
- 每个 EL 有自己的 SP、栈、向量表 `VBAR_ELx`
- `SCR_EL3.NS` 切换 Secure/Non-secure；`HCR_EL2` 控制 **EL1→EL2 陷阱**（虚拟化核心，见 [08](08-arm64-virt-extensions.md)）

**虚拟化视角**：x86 用 root/non-root 两个世界；ARM 用**异常级 + 陷阱路由**实现同样效果——guest 内核跑在 EL1，敏感操作被 `HCR_EL2` 路由到 EL2 的 KVM 处理。这是两种架构虚拟化最根本的差异。

## 3. 指令集速览（读懂 KVM 代码所需的最小集）

```
// 内存访问
LDR X0, [X1, #imm]          ; 也支持 pre/post-index: LDR X0,[X1,#8]!
STP X29, X30, [SP, #-16]!   ; 成对压栈 (函数序言常见)

// 分支
B / BL / RET {Xn} / CBZ / TBZ
ERET                        ; ★ 异常返回: ELR_ELx→PC, SPSR_ELx→PSTATE (world switch 的出口)

// 系统寄存器访问 (特权, 是陷出点)
MRS X0, TTBR0_EL1           ; 读
MSR TTBR0_EL1, X0           ; 写
ISB / DSB / DMB             ; 屏障: 指令序列/全系统/内存域

// 异常相关
SVC #imm (EL0→EL1 系统调用) | HVC #imm (→EL2, hypercall) | SMC #imm (→EL3)
WFI / WFE                    ; 等中断 (KVM WFI 退出处理的基础)
```

**陷阱规则**：`HCR_EL2.TWI=1` 时 guest 执行 WFI 即陷出到 EL2；`TVM=1` 时 guest 写多数 MMU/TTLB 系统寄存器陷出——KVM 按需配置这些位决定"哪些操作交给硬件、哪些自己模拟"。

## 4. 内存模型与 Stage-1 MMU

### 4.1 内存属性：MAIR 机制

x86 用 MTRR/PAT 描述内存类型；ARM64 用 **MAIR_ELx** 提供 8 个属性索引（Attr0..Attr7），页表项的 `AttrIndx[2:0]` 指向其中之一：

```
MAIR_ELx 每字节一个属性, 常见值 (内核 head.S 中定义):
  0xff Device-nGnRE (外设 MMIO)
  0x04 Normal NC (非缓存)
  0xee Normal WB WA (Write-Back Write-Allocate, 主存)
```

**虚拟化关联**：Stage-2 页表**没有 AttrIndx**，只有 S2 的 MT 字段（或强制为 Normal）；最终内存类型 = Stage-1 与 Stage-2 的组合（见 [08 §3](08-arm64-virt-extensions.md)）。给 guest 分配 MMIO 区域时靠 Stage-2 fault 走模拟，而非 Stage-2 属性。

### 4.2 可共享性与缓存一致性

```
Shareability: NSH (不可共享) / ISH (Inner) / OSH (Outer)
→ 决定 DMB/DSB 的广播范围; 页表描述符中 SH 字段
```

Stage-2 描述符的 SH0（VTCR_EL2）决定第二级翻译的共享性——多核 vCPU 并发访问 stage-2 表时的缓存一致性由它保证。

### 4.3 地址空间划分

```
TTBR0_EL1: 低半区 (用户, 0..2^T0SZ)      ← ASID 16位, per-process
TTBR1_EL1: 高半区 (内核, 0xffff...)       ← 内核全局映射
TCR_EL1: T0SZ/T1SZ, TG0/TG1 (4K/16K/64K), AS/E ASID/EPI 位...
```

4KB 粒度、48 位 VA 时与 x86-64 四级表结构同构（L0–L3，见 [02 §2.2](02-memory-management.md)）。

### 4.4 页表描述符（Stage-1, L3 页描述符）

```
63          55 54   10 9 8 7 6 5 4 3 2 1 0
┌────────────┬─────────┬───────────────────┐
│ NX | UXN/PXN│ 输出地址 │XN│AP│NS│AttrIndx│nG│P │
└────────────┴─────────┴───────────────────┘
AP[2]=0 特权RW/用户无; AP[2]=1 只读; nG=0 全局页(内核), 1 非全局(进程)
```

## 5. 异常处理机制（重点）

### 5.1 异常分类与路由

```
同步异常 (SVC/HVC/SMC/MMU fault/SP 对齐...)  ← ESR_ELx 有 syndrome
IRQ / FIQ (中断)                            ← GIC 分发, 可路由到 EL1/EL2/EL3
SError (系统错误, 异步)
```

路由控制（虚拟化必考）：
- `SCR_EL3.IRQ/FIQ/EA`：是否路由到 EL3
- `HCR_EL2.IMO/FMO/AMO`：**置 1 时硬件中断路由到 EL2**（KVM 运行 guest 时的标准配置：物理中断由 KVM 先收，再决定注入或退出处理）
- 虚拟中断：`HCR_EL2.VI/VF` 手动拉起；或 vGIC list register 直注（见 [08 §6](08-arm64-virt-extensions.md)）

### 5.2 向量表

```
VBAR_ELx 指向 16 个 entry, 每 entry 2KB 对齐:
  4 种异常 (Current EL SP_EL0 / Current EL SPx / Lower EL AArch64 / Lower EL AArch32)
  × 4 种类型 (Sync / IRQ / FIQ / SError)
内核入口: arch/arm64/kernel/entry.S 的 vectors 宏展开
```

### 5.3 ESR_ELx（Exception Syndrome Register）——KVM 的"错误码"

```
ESR_EL2 (guest 退出到 EL2 时):
  EC[31:26]  异常类别
     0x16 SVC (AArch64), 0x17 HVC, 0x18 SMC
     0x18/0x19 MSR/MRS 陷阱 (系统寄存器访问)
     0x20/0x21 指令 abort (取指)
     0x24/0x25 Data Abort (读/写!)      ← 内存虚拟化主战场
     0x22 PC 对齐, 0x3F  SError...
  IL[25]     指令长度 (0=32bit)
  ISS[24:0]  具体信息: Data Abort 时含 ISV/SAS/SSE/SRT/FnV/ISV/CM/EA/S1PTW/ords
```

Data Abort ISS 关键子域（`arch/arm64/include/asm/esr.h`）：

| 域 | 含义 |
|----|------|
| `ISV` | 是否有关于访存指令的立即信息（SAS/SSE/SRT 才有效） |
| `SAS` | 访问大小 (1/2/4/8B) |
| `SRT` | 目标寄存器编号（KVM 模拟 MMIO 后把数据写进 guest 的这个寄存器！） |
| `WnR` | 读还是写 |
| `S1PTW` | **fault 发生在 Stage-1 页表遍历期间**（stage-2 保护了 guest 页表本身） |
| `CM` | cache maintenance 指令触发 |

关键寄存器：`FAR_EL2`=故障 GVA；`HPFAR_EL2`=故障 **IPA**（仅 Stage-2 fault 有效；`FnV=1` 时 KVM 需自己手工 walk stage-1 取 GPA）。

### 5.4 与 x86 的对照表

| 概念 | x86-64 | ARM64 |
|------|--------|-------|
| 特权环 | Ring0-3 + root/non-root | EL0-EL3（无 root 划分） |
| 进入 VMM | VM-exit（硬件自动换寄存器上下文） | 异常陷出到 EL2（软件保存/恢复上下文） |
| 退出原因载体 | VMCS exit_reason + qualification | ESR_EL2.EC/ISS |
| 故障地址 | CR2 / VMCS GUEST_PHYSICAL | FAR_EL2 / HPFAR_EL2 |
| 返回 guest | VMRESUME | ERET |
| TLB tag | PCID + EPTP-ASID | ASID + VMID |
| hypercall | VMCALL | HVC |

## 6. PSCI / SMCCC（电源与固件调用）

`HVC/SMC` 承载的标准化调用（PSCI: CPU_ON/CPU_OFF/SYSTEM_OFF/SUSPEND；SMCCC: 版本与固件服务）。KVM 在 EL2 拦截 guest 的 PSCI 调用并**软件模拟多 vCPU 启动**——这是 KVM/ARM vCPU 上电的标准路径（lab06 会亲手触发）。

```
PSCI v1.0 调用约定: X0=function_id, X1..=参数, 返回值 X0
  CPU_ON(target_cpu, entry_point, context_id)   ← VMM 用来"点亮"次 vCPU
```

## 7. 自测题

1. guest (EL1) 执行 `MSR TTBR0_EL1, X0`，KVM 要拦到它需要 `HCR_EL2` 的哪个位？陷出后 ESR_EC 是多少？
2. `HPFAR_EL2` 何时有效？`FnV=1` 意味着什么、KVM 怎么办？
3. 为什么 ARM 的虚拟中断可以"注入而不退出"（对比 x86 中断必须 VM-exit）？
4. `MAIR` 与 MTRR/PAT 的本质区别是什么？Stage-2 描述符里为什么没有 AttrIndx？
5. 描述 guest 中一次 WFI 的完整路径（从指令执行到 KVM 调度决策）。

## 源码地图

| 主题 | 文件 |
|------|------|
| ESR 解码 | `arch/arm64/include/asm/esr.h`, `kernel/traps.c` |
| 异常入口 | `arch/arm64/kernel/entry.S` |
| 内核页表建立 | `arch/arm64/kernel/head.S`, `mm/mmu.c` |
| 系统寄存器定义 | `arch/arm64/include/asm/sysreg.h` |
| PSCI | `arch/arm64/kernel/psci.c`, `virt/kvm/arm/psci.c` |
