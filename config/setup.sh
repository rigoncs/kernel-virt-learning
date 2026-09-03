#!/usr/bin/env bash
# setup.sh — 一键配置内核/虚拟化学习环境 (Ubuntu/Debian 为主, 其他发行版见 env.md)
set -euo pipefail

echo "== [1/5] 安装基础构建与调试工具 =="
sudo apt-get update
sudo apt-get install -y \
    build-essential gcc make git bc bison flex libssl-dev libelf-dev \
    cpio rsync dwarves zstd \
    gdb crash trace-cmd perf-tools-unstable bpftrace \
    qemu-system-x86 qemu-utils qemu-system-arm \
    stress-ng htop ncdu \
    gcc-aarch64-linux-gnu gdb-multiarch

echo "== [2/5] 检查虚拟化能力 =="
ARCH=$(uname -m)
if [ "$ARCH" = "aarch64" ]; then
    echo "  架构: ARM64 (主线环境)"
    ls -l /dev/kvm 2>/dev/null \
        && echo "  KVM: OK" \
        || echo "  /dev/kvm 不存在, 尝试: sudo modprobe kvm; 无硬件虚拟化时实验走 QEMU TCG"
elif grep -qE 'vmx|svm' /proc/cpuinfo; then
    echo "  硬件虚拟化: OK ($(grep -oE 'vmx|svm' /proc/cpuinfo | head -1))"
    echo "  提示: 当前为 x86 主机; ARM64 实验可用 qemu-system-aarch64 -accel tcg (交叉编译见 docs/10)"
else
    echo "  警告: 无 vmx/svm, KVM 加速不可用; 实验 will 走 QEMU TCG 或 ARM64 替代方案"
fi
ls -l /dev/kvm 2>/dev/null || true

echo "== [3/5] 检查调试基础设施 =="
ls /sys/kernel/debug/tracing >/dev/null 2>&1 \
    && echo "  ftrace: OK (需 sudo)" \
    || echo "  提示: 挂载 debugfs => sudo mount -t debugfs none /sys/kernel/debug"
grep -q CONFIG_KASAN /boot/config-$(uname -r) 2>/dev/null && echo "  发行版内核含 KASAN 配置符号" || true

echo "== [4/5] 内核源码树 (若 ../linux 已存在则跳过) =="
if [ -d "${KERNEL_DIR:-../linux}" ]; then
    echo "  使用已有源码树: ${KERNEL_DIR:-../linux}"
else
    echo "  建议手动克隆: git clone --depth=1 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git"
fi

echo "== [5/5] 可选: 编译调试用内核的依赖 =="
sudo apt-get install -y ncurses-dev libncurses5-dev pkg-config 2>/dev/null || true

cat <<'EOF'

环境就绪检查清单:
  [1] sudo ./minimal-vm (labs/lab01) 能跑通
  [2] sudo trace-cmd list -e kvm | head  有输出
  [3] gdb --version / perf --version 正常
  [4] qemu-system-x86_64 --version 正常
详细说明见 config/env.md
EOF
