# 10 · 内核开发实践：编译、模块、调试、贡献

> 目标：从"会读"到"会改"。覆盖 ARM64 内核交叉编译、QEMU 启动自编内核、内核模块开发、调试闭环与社区贡献流程。基于 5.10+ LTS，兼容更新主线。

---

## 1. 获取与编译内核（ARM64）

### 1.1 环境

```bash
# 主机为 x86_64 (交叉编译) —— 本仓库默认场景
sudo apt install gcc-aarch64-linux-gnu build-essential bc bison flex \
     libssl-dev libelf-dev dwarves cpio

git clone --depth 1 --branch linux-5.10.y \
    https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git
cd linux
```

### 1.2 配置与编译

```bash
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
make defconfig                       # QEMU virt 机型默认可启动
# 调试/虚拟化必开项:
./scripts/config \
    -e DEBUG_INFO -e DEBUG_INFO_DWARF4 -e GDB_SCRIPTS \
    -e KVM -e KVM_HOST -e VIRTUALIZATION \
    -e FUNCTION_TRACER -e FTRACE -e KPROBES -e BPF_SYSCALL \
    -e PROVE_LOCKING -e DEBUG_ATOMIC_SLEEP \
    -e DYNAMIC_DEBUG
make olddefconfig
make -j$(nproc) Image dtbs           # ARM64 产物是 Image (非 bzImage)
```

5.10+ 要点：
- `Image.gz` 也可用（QEMU `-kernel` 均支持）；`Image` 调试更直接
- 设备树：QEMU virt 机自动生成，`make dtbs` 主要用于真机
- 模块：`make modules`，装入 rootfs 的 `/lib/modules/$(uname -r)/`

### 1.3 QEMU 启动自编内核

```bash
qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a57 -smp 2 -m 1G \
    -kernel arch/arm64/boot/Image \
    -append "console=ttyAMA0 nokaslr" \
    -initrd initramfs.cpio.gz -nographic \
    -object memory-backend-file,id=mem,size=1G,share=on,mem-path=/dev/shm/guest \
    -numa node,memdev=mem \
    -device virtio-net-pci,netdev=n0 -netdev user,id=n0

# 带硬件虚拟化的嵌套环境 (宿主为 KVM ARM64 机器):
#   将 -cpu cortex-a57 换为 -cpu host -accel kvm
# gdb 调试: 追加 -s -S
```

initramfs 制作见 [config/env.md](../config/env.md) §3。

---

## 2. 内核模块开发

### 2.1 最小模块（完整代码见 [lab08](../labs/lab08-kernel-module)）

```c
// hello.c
#include <linux/module.h>
#include <linux/init.h>

static int __init hello_init(void)
{
    pr_info("hello: loaded on ARM64, PAGE_SIZE=%ld\n", PAGE_SIZE);
    return 0;
}
module_init(hello_init);

static void __exit hello_exit(void)
{
    pr_info("hello: unloaded\n");
}
module_exit(hello_exit);

MODULE_LICENSE("GPL");
```

```makefile
# Makefile (交叉编译)
obj-m += hello.o
KDIR ?= /path/to/linux        # 内核树 (必须与目标内核同版本!)
all:
    $(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
```

### 2.2 模块开发要点（虚拟化相关经验）

1. **版本一致性**：模块的 `vermagic` 必须匹配目标内核（`modinfo`），QEMU 里跑自编内核时模块也要用同一棵树编译
2. **接口不稳定性**：内核无稳定 ABI——5.10 与 6.x 的 `mmu_notifier_ops`、`kvm_host.h` 结构有差异；跨版本模块必须按目标内核源码树编译
3. 常用调试输出：`pr_info/dev_dbg` + dynamic debug（`echo 'module hello +p' > /sys/kernel/debug/dynamic_debug/control`）
4. 模块参数：`module_param(name, int, 0444)` → `/sys/module/<mod>/parameters/`
5. **符号导出**：使用 `EXPORT_SYMBOL_GPL` 的符号需 `MODULE_LICENSE("GPL")`；KVM 内部符号（`kvm_*`）多数未导出，实验模块要么用 kprobe 拿地址，要么直接把代码编进内核（见 lab08 进阶）

### 2.3 常用 API 速查（本仓库实验用到）

| 场景 | API |
|------|-----|
| 字符设备 | `misc_register`（最简）或 `cdev` + `alloc_chrdev_region` |
| procfs | `proc_create_single` |
| 用户内存交互 | `copy_to_user/copy_from_user` |
| 延迟工作 | `schedule_work`, `kthread_run` |
| 锁 | `spin_lock_irqsave`（中断上下文）、`mutex`（进程上下文） |

---

## 3. 调试闭环（ARM64 专项）

### 3.1 printk 与日志

```bash
dmesg -w          # 实时
# QEMU 里: console=ttyAMA0 直接打到终端; 也可 -serial file:serial.log
```

### 3.2 QEMU + GDB（源码级调试内核，最有效）

```bash
# 1. QEMU 加 -s -S (等待 gdb 连接)
gdb-multiarch vmlinux
(gdb) target remote :1234
(gdb) source scripts/gdb/vmlinux-gdb.py     # 内核自带辅助命令
(gdb) hbreak start_kernel                    # 硬件断点 (在解压早期也能用)
(gdb) b kvm_handle_guest_abort               # KVM/ARM 断点
(gdb) c
(gdb) bt
(gdb) p/x *vcpu                              # 直接看内核结构体 (调试信息)
(gdb) lx-dmesg / lx-lsmod / lx-tasks         # vmlinux-gdb.py 提供的命令
```

要点：`nokaslr` 必加（否则地址随机化断点失效）；ARM64 用 `gdb-multiarch`（或 `gdb` 带 aarch64 支持）。

### 3.3 KGDB / KDB（交互式内核调试）

`CONFIG_KGDB=y CONFIG_KGDB_KDB=y`，启动参数 `kgdboc=ttyAMA0,115200 kgdbwait`，另一终端 `gdb → target remote /dev/...`。适合单步走 `__guest_enter` 这类汇编。

### 3.4 ftrace/KVM tracepoint（详见 [06](06-debugging-tools.md)）

ARM64 KVM 专属事件：`events/kvm/kvm_entry`, `kvm_exit`（含 `esr_ec`）、`events/kvm/kvm_inject_irq`、`vgic_*`。lab07 有完整流程。

### 3.5 panic 与 crash 分析

```bash
# QEMU 侧直接导出 guest 内存: monitor (Ctrl-A C) 里
(qemu) dump-guest-memory /tmp/vmcore
# 宿主机 panic: kdump (crashkernel=256M) + crash vmlinux /var/crash/.../vmcore
crash vmlinux vmcore
crash> bt / dis -l <addr> / rd <addr>
```

### 3.6 语法/静态检查

```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- C=1 drivers/foo/   # sparse
./scripts/checkpatch.pl --strict my.patch
./scripts/get_maintainer.pl my.patch                                # 找收件人
```

---

## 4. 补丁与社区贡献（简明流程）

```bash
# 1. 分支与修改 (在 git clone 的内核树)
git checkout -b fix-kvm-arm-xxx
# 修改代码, 保证: 编译通过 + 相关 selftests/kvm 通过 + checkpatch 干净

# 2. 提交信息格式 (git log 查看同类提交学习)
#    子系统前缀: "KVM: arm64: ..." / "arm64: mm: ..."
#    主题行 <= 72 字符; 正文说清 what/why, 引用 spec 条款

# 3. 生成补丁
git format-patch -1 -o /tmp/out
./scripts/checkpatch.pl /tmp/out/0001-*.patch

# 4. 发送 (需配置 git send-email)
./scripts/get_maintainer.pl /tmp/out/0001-*.patch   # KVM/ARM: Marc Zyngier 等
git send-email --to=... --cc=... /tmp/out/0001-*.patch
# 5. 邮件列表 lkml/vkvm 讨论迭代; maintainer 分支合入后进入 -next
```

规范依据：`Documentation/process/submitting-patches.rst`（内核树内，权威且自包含）。

---

## 5. 实践路线建议

```
第 1 步: 编译 + QEMU 启动自编内核 (§1)          → "我能改内核"
第 2 步: lab08 内核模块, 改 pr_info 输出 PAGE_SIZE/EL → "我能扩展内核"
第 3 步: 在内核树里改一处 KVM 代码 (如加一个 trace_printk 到
         kvm_handle_guest_abort), 重编验证          → "我能改虚拟化代码"
第 4 步: gdb 断点走一遍 world switch (lab07 §4)   → "我能调试虚拟化"
第 5 步: 向内核邮件列表提一个真实小问题
         (从 checkpatch/文档 typo/BUG 报告入手)     → "我是内核贡献者"
```

## 6. 自测题

1. 交叉编译模块在目标机 `insmod` 报 "Invalid module format"，列出 3 个可能原因。
2. 为什么调试内核必须 `nokaslr`？KASLR 如何影响断点？
3. `__guest_enter` 是汇编且每 vCPU 不同实例，GDB 如何对它下断点？（提示：`hbreak` + 符号地址；pKVM 下会怎样？）
4. KVM 内核符号未导出时，实验模块还有哪些观测手段？（kprobe/ftrace/直接编入内核）

## 源码/文档地图

| 主题 | 位置 |
|------|------|
| 内核开发流程 | `Documentation/process/`（submitting-patches, development-process） |
| 模块接口 | `include/linux/module.h`, `Documentation/kbuild/modules.rst` |
| ARM64 启动协议 | `Documentation/arch/arm64/booting.rst` |
| KVM/ARM API | `Documentation/virt/kvm/api.rst`（ARM64 小节）, `devices/vgic.rst` |
| gdb 辅助 | `scripts/gdb/vmlinux-gdb.py` |
