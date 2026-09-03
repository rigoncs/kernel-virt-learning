# Lab 03 · MMU notifier 实验：宿主页失效与 KVM 的联动

> 目标：写一个注册 MMU notifier 的内核模块 + 用户态触发程序，亲手制造"宿主页表被修改"事件，并用 ftrace 观察 KVM notifier 回调与 EPT zap。

对应文档：[docs/04](../../docs/04-kvm-memory-impl.md) §4。

---

## 1. 背景

KVM 自己就是一个 MMU notifier 消费者（`virt/kvm/kvm_main.c` 的 `kvm_mmu_notifier_ops`）。要理解它，最好先自己写一个生产者/消费者。

```
宿主事件                    → notifier 回调                → KVM 动作
madvise(MADV_DONTNEED)      → invalidate_range_start/end   → zap EPT 项
页迁移/THP 拆分              → invalidate_range_start       → zap + 可能拆大页
KSM 合并 (change_pte)       → change_pte                   → 原地换 SPTE
LRU aging (回收扫描)         → clear_flush_young            → 清 SPTE Accessed 位
```

## 2. 实验一：观察 KVM 自身的 notifier 回调

```bash
# 1. 启动一个 QEMU guest, 记 pid
# 2. 追踪 notifier
cd /sys/kernel/debug/tracing
echo 1 > events/kvm/kvm_set_spte/enable
echo 1 > events/kvm/kvm_mmio/enable
echo function_graph > current_tracer
echo kvm_mmu_notifier_invalidate_range_start > set_graph_function
echo 1 > tracing_on

# 3. host 侧触发回收/失效
QPID=$(pgrep -f qemu-system)
# 内存压力触发 reclaim (宿主回收 QEMU 的匿名页 → notifier → zap)
stress-ng --vm 4 --vm-bytes 2G -t 10

echo 0 > tracing_on; cp trace /tmp/notifier-trace.txt
```

**观察点**：
- `invalidate_range_start` 的 [start,end) HVA 区间
- 区间内 `zap` 调用（TDP MMU: `tdp_mmu_zap_...`）
- 之后 guest 再访问这些页 → 重新出现 `kvm_mmu_page_fault`

## 3. 实验二：写自己的 notifier 消费者模块

代码见本目录 `notifier_module.c` / `trigger.c`。模块逻辑：mmap 一段匿名内存，注册 notifier；回调里打印事件；暴露 `/proc/mn_demo` 供用户态查询计数。

```bash
make                 # 构建模块 (需要内核头文件/本机内核编译环境)
sudo insmod notifier_demo.ko
cat /proc/mn_demo

# 触发: 用户态程序 madvise(MADV_DONTNEED) 该区域
./trigger
dmesg | tail
# 预期: invalidate_range_start count=1, range=[va, va+size)

sudo rmmod notifier_demo
```

**实验三（思考题，代码留空）**：把模块的回调改成"在 invalidate_range_start 里记录被失效的页"，然后让同一进程先 mmap 再 `mlock`，比较两种内存（可回收 vs 锁定）的 notifier 频率差异。回答：为什么生产环境给延迟敏感 VM 用 hugetlbfs/mlock 能显著减少 EPT zap？

## 4. 自检标准

- [ ] 能默写 `mmu_notifier_ops` 中 4 个核心回调的触发条件
- [ ] 能解释为何需要 `invalidate_range_start`/`end` 两个阶段（二次准入保护：期间 pfn 保证不落新映射）
- [ ] 能说出 KVM 在 range_start 中做 zap 而不在 range_end 做的原因
