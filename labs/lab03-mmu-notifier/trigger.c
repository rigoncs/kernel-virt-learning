// trigger.c — Lab 03: 触发宿主页表失效事件
// 用法: ./trigger   (与 notifier_demo.ko 在同一进程 mm 上不适用——
// 演示意义: 观察本进程内存事件的 notifier 行为需把模块注册到目标进程;
// 本程序用于通用观察: madvise/THP 拆分等事件在系统内造成的缺页行为)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define SIZE (4UL << 20)

int main(void)
{
    char *p = mmap(NULL, SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }

    /* 1. 触碰全部页: 建立 PTE */
    memset(p, 0x41, SIZE);
    printf("touched %lu KB\n", SIZE >> 10);

    /* 2. MADV_DONTNEED: 释放 PTE → 宿主页表失效事件 */
    if (madvise(p, SIZE, MADV_DONTNEED) < 0)
        perror("madvise DONTNEED");
    printf("after MADV_DONTNEED: first byte = 0x%02x (应为 0, 页被丢弃)\n",
           p[0]);

    /* 3. 再次触碰: 重新 demand paging */
    p[0] = 0x42;
    printf("re-touched: p[0]=0x%02x\n", p[0]);

    munmap(p, SIZE);
    return 0;
}
