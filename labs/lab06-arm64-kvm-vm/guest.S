// guest.S — Lab 06: AArch64 裸机 guest
// 构建:
//   aarch64-linux-gnu-as -o guest.o guest.S
//   aarch64-linux-gnu-ld -Ttext=0x40000000 -o guest.elf guest.o
//   aarch64-linux-gnu-objcopy -O binary guest.elf guest.bin
//
// 行为: 经 PL011 UART 打印消息 → 读未映射 IPA 0x10000000 制造
//       stage-2 fault (KVM_EXIT_MMIO) → PSCI SYSTEM_OFF 关机

.globl _start
_start:
    /* 1. 初始化 UART (QEMU virt: PL011 @ IPA 0x09000000, MMIO 无需初始化即可写) */
    ldr  x2, =0x09000000          // UART 基址

    /* 2. 逐字节打印 msg */
    adr  x0, msg
1:  ldrb w1, [x0], #1
    cbz  w1, 2f
    str  w1, [x2]
    b    1b

    /* 3. 读未映射 IPA 0x10000000 → stage-2 Data Abort → KVM_EXIT_MMIO (READ) */
2:  ldr  x3, =0x10000000
    ldrb w4, [x3]

    /* 4. PSCI SYSTEM_OFF (SMC32 调用约定: x0=function id)
     *    KVM 拦截 HVC/SMC → KVM_EXIT_SYSTEM_EVENT */
    mov  x0, #0x84000008
    hvc  #0

    /* 不应到达 */
3:  wfe
    b    3b

msg:
    .asciz "Hello from ARM64 guest!\n"
