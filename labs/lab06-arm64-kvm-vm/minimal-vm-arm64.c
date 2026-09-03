/*
 * minimal-vm-arm64.c — Lab 06: ARM64 最小 KVM 虚拟机
 *
 * 用法: sudo ./minimal-vm-arm64
 * guest 镜像: guest.bin (AArch64 裸机, 加载到 IPA 0x40000000)
 *
 * 学习要点:
 *   1. KVM_ARM_VCPU_INIT (硬约束: 必须在 KVM_RUN 前调用)
 *   2. KVM_SET_ONE_REG 设置 PC/PSTATE
 *   3. KVM_EXIT_MMIO: ARM ISS 让数据搬运由内核完成
 *   4. KVM_EXIT_SYSTEM_EVENT: PSCI SYSTEM_OFF 正常关机
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>
#include <linux/kvm_arm.h>

#define GUEST_LOAD_ADDR   0x40000000UL   /* IPA, 对应 QEMU virt 内核加载基址 */
#define MEM_SIZE          (1UL << 20)

/* KVM_REG_ARM64_CORE 寄存器编码 (linux/kvm.h):
 * off(reg) = KVM_REG_ARM_CORE | (offsetof(struct kvm_regs, pstate) << 2) 等
 * 为免依赖内核头细节, 这里直接用公开的偏移值:
 *   PC     偏移 0x40 (regs.pc)
 *   PSTATE 偏移 0x44*4=... 见 struct kvm_regs 定义
 * 以下两个 id 取自稳定 ABI ( Documentation/virt/kvm/api.rst ):
 *   KVM_REG_ARM64_CORE_REG(regs.pc)     = 0x6030 0000 0010 0042
 *   KVM_REG_ARM64_CORE_REG(regs.pstate) = 0x6030 0000 0010 0043
 */
#define REG_PC      0x6030000000100042ULL
#define REG_PSTATE  0x6030000000100043ULL
/* PSTATE = EL1h (SPSel=1) + DAIF 全掩 */
#define PSTATE_EL1H 0x3c5

static void set_reg64(int vcpufd, uint64_t id, uint64_t val)
{
    struct kvm_one_reg reg = { .id = id, .addr = (uint64_t)&val };
    if (ioctl(vcpufd, KVM_SET_ONE_REG, &reg) < 0)
        err(1, "KVM_SET_ONE_REG(id=0x%llx)", (unsigned long long)id);
}

int main(void)
{
    struct kvm_vcpu_init vcpu_init;
    struct kvm_userspace_memory_region region;
    struct kvm_run *run;
    int kvmfd, vmfd, vcpufd, ret;
    size_t run_size;
    void *guest_mem;

    kvmfd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvmfd < 0)
        err(1, "open /dev/kvm (需 ARM64 主机且内核 CONFIG_KVM=y)");
    if (ioctl(kvmfd, KVM_GET_API_VERSION, NULL) != KVM_API_VERSION)
        err(1, "KVM API version mismatch");

    vmfd = ioctl(kvmfd, KVM_CREATE_VM, 0);

    /* memslot: IPA 0x40000000 → HVA */
    guest_mem = aligned_alloc(1 << 21, MEM_SIZE);   /* 2MB 对齐利于 stage-2 block */
    memset(guest_mem, 0, MEM_SIZE);
    region = (struct kvm_userspace_memory_region){
        .slot = 0,
        .guest_phys_addr = GUEST_LOAD_ADDR,
        .memory_size = MEM_SIZE,
        .userspace_addr = (unsigned long)guest_mem,
    };
    if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
        err(1, "KVM_SET_USER_MEMORY_REGION");

    vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, 0);
    run_size = (size_t)ioctl(kvmfd, KVM_GET_VCPU_MMAP_SIZE, NULL);
    run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);
    if (run == MAP_FAILED)
        err(1, "mmap kvm_run");

    /* 加载 guest 镜像 */
    int imgfd = open("guest.bin", O_RDONLY);
    if (imgfd < 0)
        err(1, "open guest.bin");
    ret = read(imgfd, guest_mem, MEM_SIZE);
    if (ret <= 0)
        err(1, "read guest.bin");
    close(imgfd);

    /* ★ ARM64 特有: vCPU 初始化 (硬约束, 必须在 KVM_RUN 前) */
    if (ioctl(vcpufd, KVM_GET_PREFERRED_TARGET, &vcpu_init) < 0)
        err(1, "KVM_GET_PREFERRED_TARGET");
    if (ioctl(vcpufd, KVM_ARM_VCPU_INIT, &vcpu_init) < 0)
        err(1, "KVM_ARM_VCPU_INIT");

    /* 设置入口: PC + PSTATE (EL1h) */
    set_reg64(vcpufd, REG_PC, GUEST_LOAD_ADDR);
    set_reg64(vcpufd, REG_PSTATE, PSTATE_EL1H);

    for (;;) {
        ret = ioctl(vcpufd, KVM_RUN, NULL);
        if (ret < 0)
            err(1, "KVM_RUN");

        switch (run->exit_reason) {
        case KVM_EXIT_SYSTEM_EVENT:
            /* guest 执行 PSCI SYSTEM_OFF (HVC) 走到此处 */
            printf("guest requested SYSTEM_OFF (type=%llu) — 正常关机\n",
                   run->system_event.type);
            return 0;

        case KVM_EXIT_MMIO:
            /* 未映射 IPA 访问: stage-2 Data Abort → KVM 判为 MMIO
             * ARM 优势: 内核已按 ESR ISS (SAS/SSE) 把数据搬进 run->mmio.data */
            printf("KVM_EXIT_MMIO %s IPA=0x%llx len=%u data=0x%02x\n",
                   run->mmio.is_write ? "WRITE" : "READ",
                   run->mmio.phys_addr, run->mmio.len,
                   run->mmio.data[0]);
            /* 模拟读返回 0xAA */
            if (!run->mmio.is_write)
                memset(run->mmio.data, 0xAA, run->mmio.len);
            break;

        case KVM_EXIT_FAIL_ENTRY:
            errx(1, "KVM_EXIT_FAIL_ENTRY: hardware_entry_failure=0x%llx "
                    "(常为 PSTATE/PC 设置错误)",
                    run->fail_entry.hardware_entry_failure_reason);

        default:
            errx(1, "unexpected exit_reason=%u", run->exit_reason);
        }
    }
}
