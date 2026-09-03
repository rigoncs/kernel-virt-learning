/*
 * minimal-vm.c — Lab 01: 一个约 150 行的最小 KVM 虚拟机
 *
 * 用法: sudo ./minimal-vm
 * guest 镜像: 从 guest.bin 读取到 GPA 0 (16位实模式)
 *
 * 学习目标:
 *   1. 掌握 /dev/kvm 最小 ioctl 集合
 *   2. 理解 kvm_run 共享页 (mmap on vcpu fd)
 *   3. 观察 KVM_EXIT_MMIO (EPT violation → MMIO 判定)
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

int main(void)
{
    struct kvm_sregs sregs;
    struct kvm_regs regs;
    struct kvm_userspace_memory_region region;
    struct kvm_run *run;
    int kvmfd, vmfd, vcpufd, ret;
    size_t run_size;
    void *guest_mem;

    /* 1. 打开 /dev/kvm */
    kvmfd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvmfd < 0)
        err(1, "open /dev/kvm (需要 root 且 CPU 支持虚拟化)");
    ret = ioctl(kvmfd, KVM_GET_API_VERSION, NULL);
    if (ret != KVM_API_VERSION)
        err(1, "KVM API version mismatch: %d", ret);

    /* 2. 创建 VM */
    vmfd = ioctl(kvmfd, KVM_CREATE_VM, 0);

    /* 3. 分配 1MB guest 内存并注册 memslot: GPA 0 → HVA guest_mem */
    guest_mem = aligned_alloc(1 << 21, 1 << 20);   /* 2MB 对齐, 利于 EPT 大页实验 */
    memset(guest_mem, 0, 1 << 20);

    region = (struct kvm_userspace_memory_region){
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = 1 << 20,
        .userspace_addr = (unsigned long)guest_mem,
    };
    ret = ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region);
    if (ret < 0)
        err(1, "KVM_SET_USER_MEMORY_REGION");

    /* 4. 创建 vCPU 并 mmap kvm_run 共享页 */
    vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, 0);
    run_size = (size_t)ioctl(kvmfd, KVM_GET_VCPU_MMAP_SIZE, NULL);
    if (run_size < sizeof(*run))
        err(1, "KVM_GET_VCPU_MMAP_SIZE too small");
    run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);
    if (run == MAP_FAILED)
        err(1, "mmap kvm_run");

    /* 5. 加载 guest 镜像到 GPA 0 */
    int imgfd = open("guest.bin", O_RDONLY);
    if (imgfd < 0)
        err(1, "open guest.bin (先 as -o guest.o guest.S && ld -Ttext=0 --oformat=binary -o guest.bin guest.o)");
    ret = read(imgfd, guest_mem, 1 << 20);
    if (ret <= 0)
        err(1, "read guest.bin");
    close(imgfd);

    /* 6. 置实模式状态: CS.base=0, rip=0 */
    ret = ioctl(vcpufd, KVM_GET_SREGS, &sregs);
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    ret = ioctl(vcpufd, KVM_SET_SREGS, &sregs);
    memset(&regs, 0, sizeof(regs));
    regs.rip = 0;
    regs.rflags = 0x2;      /* 必须置位 bit1 */
    ret = ioctl(vcpufd, KVM_SET_REGS, &regs);

    /* 7. 主循环: 对照 docs/04 的 kvm_arch_vcpu_ioctl_run 语义 */
    for (;;) {
        ret = ioctl(vcpufd, KVM_RUN, NULL);
        if (ret < 0)
            err(1, "KVM_RUN");

        switch (run->exit_reason) {
        case KVM_EXIT_HLT:
            puts("guest executed HLT — done");
            return 0;

        case KVM_EXIT_IO:
            printf("KVM_EXIT_IO port=0x%x size=%d %s\n",
                   run->io.port, run->io.size,
                   run->io.direction == KVM_EXIT_IO_OUT ? "OUT" : "IN");
            break;

        case KVM_EXIT_MMIO:
            /* 未注册 memslot 的 GPA 访问: EPT violation → MMIO 判定 */
            printf("KVM_EXIT_MMIO phys=0x%llx len=%u is_write=%d data=0x%02x\n",
                   run->mmio.phys_addr, run->mmio.len,
                   run->mmio.is_write, run->mmio.data[0]);
            /* 模拟: 对 0x100000 读返回 0xAA, 然后 VM-resume */
            if (run->mmio.phys_addr == 0x100000 && !run->mmio.is_write)
                memset(run->mmio.data, 0xAA, run->mmio.len);
            break;

        default:
            errx(1, "unexpected exit_reason=%u (查 linux/kvm.h 注释)", run->exit_reason);
        }
    }
}
