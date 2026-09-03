# Lab 01 · 从零手写最小 KVM 虚拟机（观察 EPT violation）

> 目标：不依赖 QEMU，直接用 `/dev/kvm` API 创建一个 1 vCPU 的裸机 VM，装入一段汇编代码，逐个处理 VMexit，重点观察 **EPT violation** 的产生与处理。这是理解 KVM 内存虚拟化用户态契约的最短路径。

对应文档：[docs/03](../../docs/03-memory-virtualization.md) §3、[docs/04](../../docs/04-kvm-memory-impl.md) §2-3。

---

## 1. 原理回顾

KVM 用户态 API 分层：

```
/dev/kvm
  KVM_GET_API_VERSION        # 校验
  KVM_CREATE_VM              # → vmfd
    KVM_CREATE_VCPU          # → vcpufd
    KVM_SET_USER_MEMORY_REGION  # memslot: GPA→HVA
  KVM_RUN (on vcpufd)        # 进入主循环
    → struct kvm_run 共享页: 携带每次 VMexit 的原因与数据
```

## 2. 实验：guest 代码与运行

guest 是 16 位实模式汇编，向 `0x500` 写入字符串后触发一次对**未映射 GPA（0x100000）**的读——制造一次 EPT violation：

```asm
; guest.S — 16-bit real mode
.code16
start:
    movw $msg, %si
    movw $0x500, %di
copy:
    movb (%si), %al
    movb %al, (%di)
    incw %si
    incw %di
    cmpb $0, %al
    jne copy
    /* 故意读未映射地址 → EPT violation */
    movl $0x100000, %ebx
    movb (%ebx), %al
    hlt

msg: .asciz "Hello from guest!"
```

构建并运行（主程序见 `minimal-vm.c`，本目录已提供完整实现）：

```bash
gcc -o minimal-vm guest.S minimal-vm.c -static   # guest.S 用 as/ld 组装嵌入亦可
sudo ./minimal-vm
```

`minimal-vm.c` 要点（完整源码见本目录）：

1. `open("/dev/kvm")` → `KVM_CREATE_VM`
2. `mmap(NULL, size, PROT_RW, MAP_SHARED, vcpufd, 0)` 得 `struct kvm_run *`（**注意**：KVM_RUN 结构通过 vCPU fd 的 `mmap` 获得，这是理解"内核与用户态共享状态"的第一个例子）
3. 分配 1MB guest 内存 `aligned_alloc(2<<20, 1<<20)`，注册 memslot：

```c
struct kvm_userspace_memory_region region = {
    .slot = 0,
    .guest_phys_addr = 0,
    .memory_size = 1 << 20,
    .userspace_addr = (uint64_t)guest_mem,
};
ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region);
```

4. 设置实模式入口（`sregs.cs` 与 `rip`），复制 guest 代码到 GPA 0
5. 主循环：

```c
for (;;) {
    ret = ioctl(vcpufd, KVM_RUN, NULL);
    /* 与 kvm_arch_vcpu_ioctl_run 语义对照:
       ret==0: 需要用户态介入, 看 run->exit_reason
       ret<0 : EINTR 等错误 */
    switch (run->exit_reason) {
    case KVM_EXIT_HLT:    puts("HLT"); return 0;
    case KVM_EXIT_IO:     /* 端口 IO: run->io */ break;
    case KVM_EXIT_MMIO:   /* MMIO: run->mmio, phys_addr 即故障 GPA */ break;
    default:              errx(1, "exit_reason=%d", run->exit_reason);
    }
}
```

## 3. 观察与思考

1. 读 `0x100000` 时 exit_reason 是什么？（提示：未注册 memslot 的 GPA 访问会被 KVM 判为 MMIO → `KVM_EXIT_MMIO`，`mmio.phys_addr == 0x100000`）
2. 用 `strace -e ioctl ./minimal-vm` 记录完整 ioctl 序列，与 docs/04 的 API 层图对照
3. **进阶**：把 memslot 改为只读（`KVM_MEM_READONLY`），让 guest 写该区域，观察 exit_reason 变化（EPT violation 触发的 RO 写 → `KVM_EXIT_MMIO` 写事件）
4. **进阶**：在宿主机同时开 `trace-cmd record -e kvm`，验证每次 `KVM_EXIT_MMIO` 对应内核里的一次 `kvm_exit tracepoint`

## 4. ARM64 替代方案

x86 不可用时：把 guest.S 换成 ARM64 裸机代码（入口 `arch/arm64` boot protocol），ioctl 流程一致；未映射 IPA 访问产生 `KVM_EXIT_MMIO` 同样成立。参考 `tools/testing/selftests/kvm/` 中的 `aarch64/` 目录（源码树内）。

## 5. 自检标准

- [ ] 能不看资料列出创建 VM 的 5 个必要 ioctl
- [ ] 能解释 `kvm_run` 共享页为什么用 mmap 而非 copy（成本：每次 VMexit 拷贝状态页 vs 零拷贝共享）
- [ ] 能说出"guest 写 RO memslot"时 EPT violation → KVM_EXIT_MMIO 的完整判定链
