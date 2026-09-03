# Lab 08 · 内核模块开发入门（ARM64 交叉编译）

> 目标：完整走一遍"交叉编译模块 → 装入 QEMU 自编内核 → 观察输出 → 卸载"闭环，并验证 VHE 状态。这是内核开发实践（docs/10 §2）的动手版本。

对应文档：[docs/10](../../docs/10-kernel-dev-practice.md)、[docs/08 §2](../../docs/08-arm64-virt-extensions.md)（VHE）。

---

## 1. 前置

按 [docs/10 §1](../../docs/10-kernel-dev-practice.md) 编译好 ARM64 内核（记内核树路径为 `$KDIR`）并准备 initramfs。

**vermagic 约束**：模块必须用与目标内核**同一棵源码树、同一配置**编译，否则 `insmod` 报 `Invalid module format`。QEMU 场景下：内核与模块都出自你的 `$KDIR`。

## 2. 构建与装入

```bash
cd labs/lab08-kernel-module

# 交叉编译 (x86 主机)
make KDIR=$KDIR CROSS=aarch64-linux-gnu- ARCH=arm64

# 把模块放进 initramfs (重新打包, 见 config/env.md §3)
cp hello.ko ~/initramfs/
cd ~/initramfs && find . | cpio -o -H newc | gzip > ~/initramfs.cpio.gz

# 启动 QEMU
qemu-system-aarch64 -M virt -cpu cortex-a57 -smp 2 -m 1G -nographic \
    -kernel $KDIR/arch/arm64/boot/Image \
    -initrd ~/initramfs.cpio.gz -append "console=ttyAMA0 nokaslr"

# (guest 内)
# insmod hello.ko
# dmesg | tail
#   hello: loaded
#   hello: PAGE_SIZE=4096, KERNEL_BASE=ffff000010000000
#   hello: kernel in hyp mode (VHE) = 0     ← TCG 下无 EL2
# cat /sys/module/hello/parameters/loop
# rmmod hello && dmesg | tail -1
```

## 3. 观察点

1. **PAGE_OFFSET**：ARM64 内核映射基址（`ffff0000...`），对照 [docs/02 §3](../../docs/02-memory-management.md)
2. **VHE 标志**：`is_kernel_in_hyp_mode()` 在 TCG 下为 0；若你在 KVM guest（`-accel kvm -cpu host`）里运行且宿主开 VHE，观察值变化并解释（提示：guest 内核在 EL1，该函数查询的是自身是否在 EL2——所以 guest 里永远应看到 0，除非 guest 开启了嵌套 VHE 模拟）
3. `modinfo hello.ko` 看 vermagic / depends / 参数

## 4. 进阶任务（按序）

1. **加一个 procfs 接口**：`proc_create_single` 暴露只读计数器（参考 lab03 的 notifier 模块写法）
2. **加一个 misc 字符设备**：`misc_register`，用户态 `write` 触发 `pr_info`
3. **动态调试**：把 `pr_info` 换成 `pr_debug`，内核开 `DYNAMIC_DEBUG`，用
   `echo 'module hello +p' > /sys/kernel/debug/dynamic_debug/control` 开关
4. **测 KVM 符号可见性**（虚拟化方向关键实践）：写一个模块调 `kvm_get_running_vcpu()`——编译/加载会发生什么？记录结论并给出三种解决路径（kprobe / 编入内核 / 导出补丁），对照 [docs/10 §2.2](../../docs/10-kernel-dev-practice.md) 第 5 条

## 5. 自检标准

- [ ] 能独立完成"改模块→重编→重打包 initramfs→QEMU 验证"闭环
- [ ] 能解释 vermagic 不匹配的后果与成因
- [ ] 能说出 KVM 内核符号不可 link 的原因与至少两种实验绕行方案
