// notifier_demo.c — Lab 03: 最小 MMU notifier 消费者模块
// 用法: make && sudo insmod notifier_demo.ko && ./trigger ; dmesg | tail
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/mmu_notifier.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

static struct mn_state {
    struct mmu_notifier mn;
    unsigned long nr_invalidate_start;
    unsigned long nr_invalidate_end;
    unsigned long nr_change_pte;
} *g_state;

static int mn_invalidate_start(struct mmu_notifier *mn, struct mm_struct *mm,
                               unsigned long start, unsigned long end)
{
    g_state->nr_invalidate_start++;
    pr_info("mn_demo: invalidate_range_start [0x%lx, 0x%lx)\n", start, end);
    return 0;
}

static void mn_invalidate_end(struct mmu_notifier *mn, struct mm_struct *mm,
                              unsigned long start, unsigned long end)
{
    g_state->nr_invalidate_end++;
    pr_info("mn_demo: invalidate_range_end\n");
}

static void mn_change_pte(struct mmu_notifier *mn, struct mm_struct *mm,
                          unsigned long addr, pte_t *pte)
{
    g_state->nr_change_pte++;
    pr_info("mn_demo: change_pte @0x%lx (KSM 合并等)\n", addr);
}

static const struct mmu_notifier_ops mn_ops = {
    .invalidate_range_start = mn_invalidate_start,
    .invalidate_range_end   = mn_invalidate_end,
    .change_pte             = mn_change_pte,
};

/* /proc/mn_demo: 只读输出计数 */
static int mn_proc_show(struct seq_file *m, void *v)
{
    seq_printf(m, "invalidate_range_start: %lu\n"
                  "invalidate_range_end:   %lu\n"
                  "change_pte:             %lu\n",
               g_state->nr_invalidate_start,
               g_state->nr_invalidate_end,
               g_state->nr_change_pte);
    return 0;
}

static int __init mn_init(void)
{
    struct proc_dir_entry *pe;

    g_state = kzalloc(sizeof(*g_state), GFP_KERNEL);
    if (!g_state)
        return -ENOMEM;

    /* 注册到当前进程 (insmod 的进程) 的 mm —— 简化演示;
     * KVM 真实实现注册在 VM 进程的 mm 上, 见 virt/kvm/kvm_main.c */
    g_state->mn.ops = &mn_ops;
    if (mmu_notifier_register(&g_state->mn, current->mm))
        return -EINVAL;

    pe = proc_create_single("mn_demo", 0, NULL, mn_proc_show);
    pr_info("mn_demo: loaded, mm=%p\n", current->mm);
    return pe ? 0 : -ENOMEM;
}

static void __exit mn_exit(void)
{
    mmu_notifier_unregister(&g_state->mn, current->mm);
    remove_proc_entry("mn_demo", NULL);
    kfree(g_state);
    pr_info("mn_demo: unloaded\n");
}

module_init(mn_init);
module_exit(mn_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Lab03: minimal MMU notifier demo");
