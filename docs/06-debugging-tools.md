# 06 · 内核调试技术与工具链

> 原则：先观测（trace）再剖析（profile）后断点（debugger）。虚拟化调试的特殊性：故障发生在 guest，证据散落在 host/guest/硬件三层。

---

## 1. ftrace（第一工具）

### 1.1 基础用法

```bash
cd /sys/kernel/debug/tracing          # 或 /sys/kernel/tracing
echo 0 > tracing_on
cat available_tracers                 # nop function function_graph ...

# 函数追踪
echo kvm_mmu_page_fault > set_ftrace_filter
echo function > current_tracer
echo 1 > tracing_on; sleep 1; echo 0 > tracing_on
cat trace

# 调用图（含子调用、耗时）
echo function_graph > current_tracer
echo kvm_arch_vcpu_ioctl_run > set_graph_function
```

### 1.2 KVM 专用 tracepoint（事件追踪，开销最低）

```bash
ls events/kvm/
echo 1 > events/kvm/kvm_exit/enable
echo 1 > events/kvm/kvm_entry/enable
echo 1 > events/kvm/kvm_page_fault/enable
echo 1 > events/kvm/kvm_mmio/enable

# 常看字段: kvm_exit 的 exit_reason / info1/info2 (EPT 限定符), kvm_page_fault 的 gfn/error_code
```

exit_reason 速查：`arch/x86/include/uapi/asm/kvm.h`（`EXIT_REASON_EPT_VIOLATION=48` 等）。

### 1.3 组合技巧

```bash
# 只追特定 vCPU 线程
echo 'common_pid==12345' > events/kvm/kvm_exit/filter

# 打桩自定义事件（内核模块中）
trace_printk("gfn=%lx\n", gfn);     # 出现在 trace 缓冲，慎用于生产

# 直方图聚合：哪个 exit_reason 最多
echo 'hist:keys=exit_reason:vals=count' > events/kvm/kvm_exit/hist
cat events/kvm/kvm_exit/hist
```

---

## 2. perf

```bash
# CPU 剖析（含 guest 符号）
perf kvm --host --guest record -a sleep 5
perf kvm --host --guest report

# 通用热点
perf top
perf record -g -e cycles:k -p $(pidof qemu-system-x86) sleep 3   # :k 仅内核态

# 统计 VMexit 分布（依赖 PMU/虚拟化支持）
perf stat -e 'kvm:*' -a sleep 2
perf stat -e cycles,instructions,LLC-load-misses -p $QEMU_PID

# 火焰图
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

---

## 3. KVM 内建统计与 debugfs

```bash
/sys/kernel/debug/kvm/
├── kvm-1234/               # 每 VM 一个目录
│   ├── vcpu0/
│   │   ├── exits, halt_wait_ns, efer, ...
│   └── statistics          # 每类 exit/事件计数
└── stats                   # 聚合视图

watch -d1 cat /sys/kernel/debug/kvm/kvm-*/vcpu0/exits
```

观察 `ept_violation`、`request_irq`、`mmio` 计数随负载变化的趋势，是定位"卡在哪类退出"的第一手资料。

---

## 4. GDB / QEMU 调试

### 4.1 调 host 内核（含 KVM 代码）

```bash
# 内核带调试信息编译 (CONFIG_DEBUG_INFO=y)
gdb vmlinux
(gdb) target remote localhost:1234      # QEMU: -s -S 或 kgdb
(gdb) b kvm_mmu_page_fault
(gdb) bt / p *vcpu->kvm / p vcpu->arch.mmu
```

### 4.2 调 guest 内核（两层嵌套调试）

```bash
# QEMU 启动 guest 时加 -s，host gdb 调 guest vmlinux
(gdb) target remote :1234
(gdb) add-symbol-file guest-vmlinux      # guest 符号
# 注意: guest 断点需理解 KVM 退出——在 EPT violation 路径下 host 侧断点先触发
```

### 4.3 crash（vmcore 分析）

```bash
crash vmlinux vmcore
crash> kvm -o kvm.kvm        # 查看某 VM 结构
crash> list kvm_mmu_page -H <kvm地址> | head   # 遍历 MMU 页链
crash> rd -a <spt指针> 64                      # dump 影子/EPT 表内容
```

获取 vmcore：`kexec-tools` + `CONFIG_KEXEC`，或 `/proc/sys/kernel/panic_on_oops` 配合 QEMU dump（`virsh dump` / monitor `dump-guest-memory`）。

---

## 5. eBPF / bpftrace（动态低侵入）

```bash
# 追踪 EPT violation 处理延迟分布
bpftrace -e 'kprobe:kvm_mmu_page_fault { @start[tid] = nsecs; }
             kretprobe:kvm_mmu_page_fault /@start[tid]/ {
                 @ns = hist(nsecs - @start[tid]); delete(@start[tid]); }'

# 追踪 gfn 分布（需 struct 定义: bpftrace -I vmlinux.h 或 BTF）
bpftrace -e 'tracepoint:kvm:kvm_page_fault { @[args->gfn >> 9] = count(); }'

# host 回收影响 guest
bpftrace -e 'kprobe:kvm_mmu_notifier_invalidate_range_start { @++; }'
```

前提：内核带 BTF（`CONFIG_DEBUG_INFO_BTF=y`）。

---

## 6. 静态分析与验证

| 工具 | 用途 |
|------|------|
| `make C=1` / sparse | 类型注解检查（`__rcu`、`__user`）——KVM 的 RCU 代码重点 |
| `scripts/checkpatch.pl` | 补丁格式审查（社区提交必过） |
| smatch / KCSAN / KASAN | 竞态与越界（`CONFIG_KASAN=y`，虚拟化代码常有 OOB 报告） |
| lockdep | 锁序验证（`CONFIG_PROVE_LOCKING=y`；KVM mmu_lock 语义练习极佳素材） |
| kvm-unit-tests | 虚拟化语义回归（见 lab04） |
| `scripts/faddr2line` | 把调用栈里的符号+offset 转成源码行：`faddr2line vmlinux kvm_mmu_page_fault+0x1a3` |

---

## 7. 虚拟化问题排查工作流

```
现象: guest 内访存慢 / 卡死 / 崩溃
  1. debugfs statistics → exit 分布是否异常（ept_violation 暴涨?）
  2. ftrace kvm_exit/kvm_page_fault → 定位 gfn、error_code
  3. 换算 gfn → memslot → hva → 检查 host 侧页 (THP 拆分? KSM? 回收?)
  4. perf kvm report → host/guest CPU 热点
  5. 必要时: gdb 断点 + 单 vCPU 复现; crash 分析存量 vmcore
  6. host 回收类问题: bpftrace mmu_notifier 回调频率
```

---

## 8. 实战清单（每日可用）

```bash
# 1. 看某 VM 的退出热点
cat /sys/kernel/debug/kvm/kvm-$(pgrep -f qemu|head -1)/statistics | sort -rn | head

# 2. 抓一次完整的缺页链
trace-cmd record -e kvm -e migrate_mm -p function_graph -g kvm_mmu_page_fault sleep 2
trace-cmd report | less

# 3. 快速确认 EPT 大页使用情况
grep -c 'tdp_page' /sys/kernel/debug/kvm/kvm-*/... # 或用 KVM 自测/模块参数开启 mmu stats
```

---

## 自测题

1. `kvm_exit` tracepoint 的 `info1/info2` 在 EPT violation 时分别是什么？
2. 为什么 `perf kvm` 需要同时提供 host 和 guest 符号？
3. 如何在不重启的情况下确认某 gfn 是否映射为 2M EPT 大页？
4. lockdep 在 KVM 开发中最常抓到的两类问题是什么？
