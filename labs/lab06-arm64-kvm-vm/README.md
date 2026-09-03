# Lab 06 · ARM64 最小 KVM 虚拟机（PSCI 上电 + Stage-2 fault 观测）

> 目标：在 ARM64 上用 `/dev/kvm` API 手写最小 VMM，覆盖 ARM 特有流程：`KVM_ARM_VCPU_INIT`、PSCI CPU_ON、MMIO 退出（含 ISS 提供的 SRT 数据回填）。

对应文档：[docs/07](../../docs/07-arm64-architecture.md)、[docs/08](../../docs/08-arm64-virt-extensions.md)、[docs/09 §3/§7](../../docs/09-kvm-arm-impl.md)。

---

## 1. 与 x86 版（lab01）的差异

| 步骤 | x86 (lab01) | ARM64 (本 lab) |
|------|-------------|----------------|
| vCPU 初始化 | 设 sregs/rip | **`KVM_ARM_VCPU_INIT`（必调，见下）** + `KVM_SET_ONE_REG`（PC/PSTATE） |
| 入口状态 | 16 位实模式 | EL1、AArch64、MMU off，PC=kernel entry |
| guest 代码 | guest.S (实模式) | AArch64 裸机代码（下方） |
| 制造 stage-2 fault | 访问未映射 GPA | 同（未映射 IPA 访问 → Data Abort → KVM_EXIT_MMIO） |
| MMIO 数据 | 内核手工解码 | **ISS 的 SRT/SAS 直接给出，`run->mmio.data` 已备好** |
| 次级 vCPU 上电 | INIT-SIPI | **PSCI CPU_ON**（本 lab 单 vCPU，进阶做多 vCPU） |

**硬约束**：`KVM_ARM_VCPU_INIT` 必须在 `KVM_RUN` 之前调用，否则 `KVM_RUN` 返回 EINVAL；它设置 target（CPU 特性集合，决定 sys_regs 模拟值）并复位寄存器状态。

## 2. 构建

```bash
# 本机即 ARM64 + /dev/kvm 可用:
gcc -o minimal-vm-arm64 minimal-vm-arm64.c

# guest 镜像 (AArch64 裸机):
aarch64-linux-gnu-as -o guest.o guest.S
aarch64-linux-gnu-ld -Ttext=0x40000000 -o guest.elf guest.o   # 内核加载基址 1GB
aarch64-linux-gnu-objcopy -O binary guest.elf guest.bin

sudo ./minimal-vm-arm64
```

无 ARM64 硬件时：QEMU TCG 嵌套运行（宿主 x86）——`qemu-system-aarch64 -M virt,highmem=off -cpu cortex-a57 -accel tcg ...`，在 guest 内编译运行本 lab（性能慢但流程完整）。

## 3. 核心流程（minimal-vm-arm64.c 要点）

```c
/* 1. 创建 VM/vCPU 后: ARM 特有初始化 */
struct kvm_vcpu_init init;
ioctl(vcpufd, KVM_GET_PREFERRED_TARGET, &init);   /* 取 host 建议的 target */
ioctl(vcpufd, KVM_ARM_VCPU_INIT, &init);          /* ★ 必调: 设 target+features, 复位 */

/* 2. 设置 PC/PSTATE (KVM_SET_ONE_REG) */
struct kvm_one_reg reg;
uint64_t pc = 0x40000000, pstate = 0x3c5;         /* EL1h, DAIF 屏蔽 */
/* reg.id = ARM64_SYS_REG(3,0,12,0,0) 系统寄存器编码:
   PC  = 0x60300000001000c0  (见 linux/kvm.h KVM_REG_ARM64_CORE)
   PSTATE 用同组编码, 详见完整源码 */

/* 3. 注册 memslot: IPA 0x40000000 → HVA (对应 ARM64 内核加载地址) */
struct kvm_userspace_memory_region region = {
    .slot = 0,
    .guest_phys_addr = 0x40000000,
    .memory_size = 1 << 20,
    .userspace_addr = (uint64_t)guest_mem,
};

/* 4. KVM_RUN 主循环: 关注两类退出 */
case KVM_EXIT_MMIO:
    /* guest 访问未映射 IPA; ARM 的优势:
       run->mmio.data 内核已按 ISS(SAS/SSE) 搬好, 这里直接模拟 */
    printf("MMIO %s IPA=0x%llx len=%u data=0x%02x\n", ...);
    break;
case KVM_EXIT_SYSTEM_EVENT:
    /* guest 调 PSCI SYSTEM_OFF (HVC) → 正常关机 */
    return 0;
```

## 4. guest 代码（guest.S）

```asm
// AArch64 裸机: 写 UART PL011 (QEMU virt, IPA 0x09000000), 再读未映射
// IPA 0x10000000 制造 stage-2 fault, 最后 PSCI SYSTEM_OFF
_start:
    mov x0, #msg
1:  ldrb w1, [x0], #1
    cbz w1, 2f
    str w1, [x2_uart]        // x2_uart 需先装入 0x09000000 (见完整源文件)
    b 1b
2:  ldr x3, =0x10000000
    ldrb w4, [x3]            // ← stage-2 fault → KVM_EXIT_MMIO (读)
    // PSCI SYSTEM_OFF: function id 0x84000008, via HVC
    mov x0, #0x84000008
    hvc #0
```

## 5. 观察与思考

1. `strace -e ioctl` 全程记录：确认 `KVM_ARM_VCPU_INIT` 出现在 `KVM_RUN` 前；统计 `KVM_RUN` 次数
2. MMIO 退出时对比 x86：ARM 的 `run->mmio` 已含搬移好的数据——对照 [09 §7.1](../../docs/09-kvm-arm-impl.md) 解释为什么（ISS 的 SAS/SRT）
3. 在 guest.S 中把 MMIO 读换成**写**（`strb w4, [x3]`），重跑验证 `is_write=1`
4. **进阶 A**：故意跳过 `KVM_ARM_VCPU_INIT`，观察 `KVM_RUN` 返回什么错误
5. **进阶 B**：加第二个 vCPU，guest 主核执行 PSCI `CPU_ON`（function id 0xC4000003, SMC64 调用约定）点亮副核——参考 [09 §3](../../docs/09-kvm-arm-impl.md) 的 PSCI 流程与 `virt/kvm/arm/psci.c`
6. **进阶 C**：同时开 `trace-cmd record -e kvm`，把每次 `KVM_EXIT_MMIO` 与内核 tracepoint 对齐

## 6. 自检标准

- [ ] 能说出 `KVM_GET_PREFERRED_TARGET` / `KVM_ARM_VCPU_INIT` / `KVM_SET_ONE_REG` 各自的作用与先后顺序
- [ ] 能解释 MMIO 退出里数据为何"免费"（对比 x86 需解码指令）
- [ ] 能描述本 lab 中一次 stage-2 fault 的完整路径（guest ldrb → ESR EC=0x24 → kvm_handle_guest_abort → 未命中 memslot → KVM_EXIT_MMIO）
