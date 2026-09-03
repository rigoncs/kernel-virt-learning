# Lab 04 · kvm-unit-tests：验证 EPT / 页表语义

> 目标：使用 KVM 官方测试套件运行与内存虚拟化直接相关的测试，并读懂其中 1-2 个测试源码，建立"guest 物理内存语义"的精确认知。

对应文档：[docs/03](../../docs/03-memory-virtualization.md)。

---

## 1. 获取与构建

```bash
git clone https://gitlab.com/kvm-unit-tests/kvm-unit-tests.git
cd kvm-unit-tests
./configure --arch=x86_64     # ARM64: --arch=arm64
make -j$(nproc)
```

> 若克隆受限，可自行实现等价测试（见 §4）；测试框架的内核源码参考在 `tools/testing/selftests/kvm/`（内核树内）。

## 2. 运行内存虚拟化相关测试

```bash
# x86_64
./x86-run ./x86/ept.flat               # EPT 语义测试 (需硬件 EPT)
./x86-run ./x86/flat.elf               # 物理/大页语义
./x86-run ./x86/mmu.flat               # 页表行为
# 大页/2M 相关:
./x86-run ./x86/hugepage.flat          # (若存在; 用 ls x86/*.flat 查看全量)
```

ARM64 替代：

```bash
# QEMU TCG 或支持 KVM 的 ARM64 机器
./arm/run arm/s2flat_test              # stage-2 语义 (视版本)
ls arm/*.flat
```

## 3. 读源码任务（必做其一）

任选其一精读并写笔记：

1. `x86/ept.c`：列出它对 EPT 行为做了哪些断言（如 violation 后 guest 侧是否收到 #PF？misconfig 的处理？），对照 docs/03 §3.3
2. `lib/x86/vm.c`：测试框架如何建立 guest 页表（四级表手工填充），对照 docs/02 §2.1 手算一次拆分
3. selftests `kvm/page_fault_test.c`（内核树内）：用户态如何用 memslots/ucall 检查 fault 语义

## 4. 替代实验：手工验证大页 EPT

无外部网络时，用 lab01 的 minimal-vm 改造：注册 2MB 对齐的 8MB 内存，让 guest 顺序触碰整片内存，然后：

```bash
# 宿主侧观察 EPT 大页生效 (KVM 模块参数)
cat /sys/module/kvm_intel/parameters/ept
# QEMU 监视器: info registers / 或 perf 采样 tdp 大页填充函数
trace-cmd record -e kvm -p function_graph -g kvm_mmu_page_fault \
    -- ./minimal-vm
grep -E 'tdp|huge' /tmp/trace.txt | head
```

**观察点**：顺序访问整片 2M 对齐区域后，后续 fault 中出现 PMD 层 SPTE 合并迹象（fault 次数骤降）；对照 [docs/04](../../docs/04-kvm-memory-impl.md) §3.4 大页策略。

## 5. 自检标准

- [ ] 能解释 kvm-unit-tests 的"guest 里测试 guest"结构（test 运行在 guest 模式，断言失败经 ucall 报出）
- [ ] 能说清 EPT violation 后 KVM 何时注入 guest #PF、何时静默补映射
- [ ] （ARM64）能解释 stage-2 测试中断言的 IPA 权限行为
