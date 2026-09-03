# Lab 02 · ftrace 追踪缺页与 KVM 内存虚拟化路径

> 目标：用 ftrace/trace-cmd 完整抓取两条链路：① host 用户进程缺页（`handle_mm_fault`）；② guest EPT violation 的 KVM 处理（`kvm_mmu_page_fault` → TDP MMU 填表）。

对应文档：[docs/02](../../docs/02-memory-management.md) §4、[docs/04](../../docs/04-kvm-memory-impl.md) §3、[docs/06](../../docs/06-debugging-tools.md)。

---

## Part A：host 用户进程缺页链

```bash
cd /sys/kernel/debug/tracing   # sudo

# 1. function_graph 追踪缺页核心
echo function_graph > current_tracer
echo handle_mm_fault > set_graph_function
echo 1 > tracing_on
# 触发: 另一终端运行任何会缺页的进程, 或 dd if=/dev/zero of=/dev/null
sleep 2; echo 0 > tracing_on
cat trace | head -80
```

**应看到**：
```
handle_mm_fault() {
  __handle_mm_fault() {
    ...alloc pgd/pud/pmd/pte...
    handle_pte_fault() {
      do_fault() / do_wp_page() / do_swap_page()
    }
  }
}
```

对照 [docs/02 §4.1](../../docs/02-memory-management.md) 的路径图逐层核对。记录：该进程第一次访问 vs 第二次访问的差异（第二次无 alloc 层）。

## Part B：guest EPT violation 全链

```bash
# 0. 准备一个跑负载的 guest (QEMU 即可), 记录 QEMU pid
# 1. KVM tracepoints + 函数图
echo 0 > tracing_on; echo > trace
echo 1 > events/kvm/kvm_exit/enable
echo 1 > events/kvm/kvm_page_fault/enable
echo 1 > events/kvm/kvm_fast_mmio/enable
echo function_graph > current_tracer
echo kvm_mmu_page_fault > set_graph_function

# 2. 给 guest 打内存压力 (guest 内运行)
#    stress-ng --vm 1 --vm-bytes 512M -t 5s
echo 1 > tracing_on; sleep 3; echo 0 > tracing_on
cp trace /tmp/ept-trace.txt
```

**分析清单**（在 trace 中依次找到并标注）：
1. `kvm_exit: reason EPT_VIOLATION rip ... info1 info2` —— 记录 info1/info2（read/write/fetch/gpa_valid 位）
2. `kvm_page_fault: gfn=... error_code=...`
3. 函数图内：`kvm_mmu_get_page`（建上层 SP）→ `gfn_to_pfn`/`hva_to_pfn` → `handle_mm_fault`（**host 缺页嵌套在 guest 缺页里！**）→ `make_spte` → `tdp_mmu_map`/`__set_spte`
4. `kvm_entry` → VM-resume

**关键观察**：一次 guest 缺页可能嵌套一次 host 缺页 —— 用不同颜色标注 trace 中两级 `handle_mm_fault`。

## Part C：trace-cmd 一键采集与 report

```bash
trace-cmd record -e kvm -p function_graph -g kvm_mmu_page_fault sleep 2
trace-cmd report | grep -E 'kvm_exit|kvm_page_fault' | head -30

# 直方图: 哪个 gfn 缺页最多
trace-cmd report | grep kvm_page_fault | awk '{print $NF}' | sort | uniq -c | sort -rn | head
```

## Part D：进阶（可选）

1. 打开 `CONFIG_KVM_MMU_STAT`（若可用）或用 `perf stat -e 'kvm:*'` 统计 exit 分布随 guest 负载变化
2. 在 guest 内做 `mlockall`，观察 host 侧缺页次数下降
3. guest 使用 THP（默认开启），对比 trace 中 `try_to_map_to_huge` / 2M SPTE 相关函数是否出现

## 自检标准

- [ ] 能在 trace 中指出"guest 缺页嵌套 host 缺页"的确切位置
- [ ] 能解释 error_code 各位（present/write/user/fetch）与 EPT violation info1/info2 的对应关系
- [ ] 能画出本次 trace 对应的完整时序图（QEMU线程 → VMexit → fault 处理 → VMresume）
