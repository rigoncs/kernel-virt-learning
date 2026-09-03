# 开发环境配置指南

> 目标：一套环境覆盖三类工作——①读/改内核源码；②编译并启动调试内核；③运行 guest 做 KVM 实验。

---

## 1. 三种典型环境

| 场景 | 环境 | 说明 |
|------|------|------|
| A. 源码阅读 + QEMU 实验 | 当前主机 + QEMU | 最快上手，推荐默认 |
| B. 改内核并启动自己的内核 | QEMU + 自编内核 (initramfs) | 调试 KVM 代码的标准姿势 |
| C. 真机 bare-metal | 你有 root 的物理机 | lab01/lab03 的 notifier/EPT 实验更真实 |

云主机注意：多数云主机嵌套虚拟化默认关闭，`/dev/kvm` 不存在时可走 TCG（`qemu-system-x86_64 -accel tcg`），或使用带嵌套虚拟化的机型。

## 2. 编译一个可调试的内核（场景 B）

```bash
cd ~/src/linux          # 或你的源码树
make defconfig
# 打开调试必需项
./scripts/config -e DEBUG_INFO -e DEBUG_INFO_DWARF4 \
                 -e GDB_SCRIPTS -e KASAN -e KASAN_GENERIC \
                 -e PROVE_LOCKING -e DEBUG_ATOMIC_SLEEP \
                 -e KVM -e KVM_INTEL -e KVM_AMD
make olddefconfig
make -j$(nproc)
```

要点：
- `CONFIG_DEBUG_INFO=y` + `GDB_SCRIPTS`（生成 `vmlinux-gdb.py`，gdb 里 `lx-*` 命令直接看 task/mm/页表）
- KASAN/lockdep 让内核慢 2-5 倍，仅在调试内核开启
- 启动：`qemu-system-x86_64 -kernel arch/x86/boot/bzImage -append "console=ttyS0 nokaslr" -initrd initramfs.cpio.gz -nographic -s -S`
  - `-s -S`：gdbserver 监听 :1234 并暂停等待 gdb

## 3. initramfs 最小根文件系统

```bash
# busybox 方案
git clone https://git.busybox.net/busybox && cd busybox
make defconfig && make menuconfig   # Settings → Build static binary
make -j && make install
mkdir -p initramfs/{bin,sbin,etc,dev,proc,sys}
cp -r _install/* initramfs/
cat > initramfs/init <<'EOF'
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
echo "=== minimal initramfs ready ==="
exec /bin/sh
EOF
chmod +x initramfs/init
cd initramfs && find . | cpio -o -H newc | gzip > ../initramfs.cpio.gz
```

## 4. 宿主侧调试基础设施

```bash
# debugfs / tracefs
sudo mount -t debugfs none /sys/kernel/debug 2>/dev/null

# 保留内存跑 crash dump (可选, /etc/default/grub)
# crashkernel=256M
sudo apt install crash kexec-tools
```

## 5. 最佳实践清单

1. **源码树只读原则**：学习期源码树用 `git worktree` 或 `git stash` 管理改动；本学习仓库所有实验产物放在 labs/ 下
2. **每次实验留证据**：trace 文件、截图、dmesg 存入 `labs/<lab>/evidence/`，复盘时极有价值
3. **版本对齐**：文档源码引用基于当前内核树；行号会漂移，以符号名检索为准（`grep -n "kvm_mmu_page_fault" arch/x86/kvm/mmu/`）
4. **GDB 脚本**：内核自带 `vmlinux-gdb.py`，加载后可用：
   ```
   (gdb) source vmlinux-gdb.py
   (gdb) lx-tasklists / lx-dmesg / lx-pgdatalist
   ```
5. **ARM64 交叉环境**（如主机是 x86 但要学 ARM64 stage-2）：
   ```bash
   sudo apt install gcc-aarch64-linux-gnu
   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
   qemu-system-aarch64 -M virt -cpu cortex-a57 -m 2G -nographic \
       -kernel arch/arm64/boot/Image -initrd initramfs.cpio.gz -append "console=ttyAMA0"
   ```
6. **权限**：/dev/kvm 加入 kvm 组（`sudo usermod -aG kvm $USER`，重登生效）
