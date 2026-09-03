// hello.c — Lab 08: ARM64 内核模块入门
// 交叉编译: make KDIR=/path/to/linux   (详见本目录 Makefile)
// 运行: insmod hello.ko && dmesg | tail && rmmod hello
#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <asm/memory.h>       /* PAGE_SIZE 等 */
#include <asm/virt.h>         /* is_kernel_in_hyp_mode() — VHE 判断! */

static int loop = 1;
module_param(loop, int, 0444);
MODULE_PARM_DESC(loop, "demo module param");

static int __init hello_init(void)
{
    pr_info("hello: loaded\n");
    pr_info("hello: PAGE_SIZE=%ld, KERNEL_BASE=%lx\n", PAGE_SIZE, PAGE_OFFSET);
    /* 虚拟化视角: 当前内核是否运行在 EL2 (VHE)? */
    pr_info("hello: kernel in hyp mode (VHE) = %d\n", is_kernel_in_hyp_mode());
    pr_info("hello: loop=%d\n", loop);
    return 0;
}

static void __exit hello_exit(void)
{
    pr_info("hello: unloaded\n");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kernel-virt-learning");
MODULE_DESCRIPTION("Lab08: minimal ARM64 kernel module");
