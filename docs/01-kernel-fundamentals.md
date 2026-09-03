# 01 · Linux 内核核心组件总览

> 本文档覆盖进程管理、CPU 调度、文件系统、设备驱动四大块（内存管理单独在 [02](02-memory-management.md) 深入）。所有源码引用基于内核源码树（如 `../linux/kernel/fork.c`）。

---

## 1. 进程管理

### 1.1 核心数据结构：`task_struct`

每个进程/线程对应一个 `task_struct`（定义于 `include/linux/sched.h`），关键字段：

```
task_struct
├── thread_info / stack        # 内核栈（x86-64 16KB，ARM64 16KB）
├── mm / active_mm             # 地址空间（内核线程 mm == NULL）
├── files / fs                 # 打开文件表、根路径
├── pid / tgid                 # 线程组 id
├── sched_entity se            # 调度实体（CFS/EEVDF 用）
├── state                      # TASK_RUNNING / INTERRUPTIBLE / ...
└── signal / sighand           # 信号处理
```

**进程 vs 线程**：内核视角仅是"是否共享资源"的差别。`clone()` 的 flags 决定共享哪些子结构（`CLONE_VM` 共享 mm → 线程）。

### 1.2 进程生命周期

```
fork() 系调用链（kernel/fork.c）：
  sys_fork / sys_clone
    └─► kernel_clone()
          ├─► copy_process()
          │     ├─► dup_task_struct()        # 复制 task_struct + 内核栈
          │     ├─► copy_mm()                # COW：只复制 mm_struct 描述，不复制页
          │     ├─► copy_page_range()        # 复制页表（PTE 置写保护 → 写时复制）
          │     ├─► copy_files()/copy_fs()   # 视 clone flags 而定
          │     └─► sched_fork()             # 初始化调度实体
          └─► wake_up_new_task()             # 首次加入调度队列
```

`exec()`（`fs/exec.c: execve` 系调用）：释放旧 mm → `load_elf_binary()` 建立新地址空间。
`exit()`（`kernel/exit.c: do_exit`）：释放资源 → `do_task_dead` → 僵尸态直到父进程 `wait()`。

### 1.3 上下文切换

`context_switch()`（`kernel/sched/core.c`）两步：

```
switch_mm()          # 切地址空间：写 CR3（x86）/ TTBR0_EL1（ARM64）
switch_to()          # 切寄存器/栈：保存与恢复 callee 寄存器、栈指针
```

**与虚拟化的关联（主线伏笔）**：`switch_mm` 写 CR3 会 flush 非 global 的 TLB；guest vCPU 也是普通进程，vCPU 线程之间切换同样走这条路径——但 **guest 内部自己的切换** 由 guest CR3 完成，宿主并不直接感知。这就是内存虚拟化要解决的核心矛盾之一。

---

## 2. CPU 调度

### 2.1 调度类层次

内核按优先级依次询问各调度类（`kernel/sched/sched.h`）：

```
stop_sched_class      # 停机线程（迁移/热插拔）
dl_sched_class        # SCHED_DEADLINE (EDF)
rt_sched_class        # 实时 (FIFO/RR)
fair_sched_class      # CFS/EEVDF（普通进程）
idle_sched_class      # idle
```

`pick_next_task()` 从高到低找第一个有可运行任务的类。

### 2.2 CFS → EEVDF

- **CFS**（完全公平调度）：按 `vruntime`（虚拟运行时间）在红黑树中选最左节点：

  ```
  vruntime += delta_exec × (NICE_0_LOAD / weight)
  ```

  权重越低，vruntime 涨得越快，越靠后被调度 → 实现按权重分配 CPU。

- **EEVDF**（6.6 起替代 CFS）：在公平基础上引入**延迟**维度，两个量：
  - `lag`：实际应得服务 − 实际获得服务（公平性度量）
  - `deadline = eligible_time + slice / weight`：越早请求且权重越高的任务 deadline 越早

  选择规则：先过滤 `lag < 0`（超发）的节点，再选 deadline 最早者。对延迟敏感型任务不再需要手动 `nice` 调整。

### 2.3 与 vCPU 的关系

vCPU 本质是宿主进程中的线程，由 `fair_sched_class` 调度。问题：**双重大小失配**——guest 调度器认为自己在独占 CPU，宿主却可能随时抢占。缓解手段：

- 半虚拟化（paravirt spinlock：被抢占时不再自旋）
- vCPU pinning（`taskset`/cgroup 绑核）
- `KVM_CAP_ADJUST_STEAL_TIME`（steal time 记账，guest 内核据此调低负载估计）

---

## 3. 文件系统（VFS 层）

### 3.1 四大对象

| 对象 | 结构 | 含义 |
|------|------|------|
| 超级块 | `struct super_block` | 已挂载文件系统实例 |
| 索引节点 | `struct inode` | 文件元数据（磁盘上唯一） |
| 目录项 | `struct dentry` | 路径组件，dcache 缓存 |
| 文件 | `struct file` | 进程打开的文件实例（`files_struct` 表中） |

操作以 `->i_op`（inode operations）与 `->f_op`（file operations）函数表形式下发到具体文件系统。

### 3.2 读路径

```
read()
 └─► vfs_read() ─► file->f_op->read_iter()
       └─► generic_file_read_iter()          # mm/filemap.c
             ├─► pagecache lookup (XArray)
             └─► miss → submit_bio → 块层 → 驱动
```

页缓存（page cache）是文件系统与内存管理的交汇点——**与虚拟化直接相关**：QEMU 用 `mmap` guest 内存时，KVM 通过 `gfn_to_pfn` 拿到的 pfn 可能是页缓存页，因此需要 MMU notifier 感知页被回收/迁移。

### 3.3 常见文件系统定位

- ext4/xfs：本地日志文件系统；btrfs：COW + 快照
- 9p/virtiofs：**虚拟机文件共享**（guest 通过 virtio 访问宿主 FS），虚拟化实验常用

---

## 4. 设备驱动模型

### 4.1 核心：总线-设备-驱动三角

```
struct bus_type      # 匹配规则：probe 触发
struct device        # 设备实例
struct device_driver # 驱动实现
```

`driver_register()` 遍历总线上未绑定的 device，`match` 成功则调 `probe()`。

### 4.2 字符设备最小骨架

```c
static const struct file_operations my_fops = {
    .owner   = THIS_MODULE,
    .read    = my_read,
    .unlocked_ioctl = my_ioctl,
};

/* 注册流程 */
alloc_chrdev_region(&dev, 0, 1, "mydev");
cdev_init(&my_cdev, &my_fops);
cdev_add(&my_cdev, dev, 1);
```

`/dev/kvm` 就是一个字符设备：其 `fops->unlocked_ioctl` 实现在 `virt/kvm/kvm_main.c`（`kvm_dev_ioctl`），所有 KVM API（`KVM_CREATE_VM`、`KVM_RUN` 等）都从这里进入内核。

### 4.3 中断处理与内存映射

- `request_irq()` / 线程化中断（`request_threaded_irq`）
- `ioremap()`：把设备 MMIO 映射进内核地址空间（建立 Stage-1 页表项）
- `mmap` fop：把内核内存/设备内存暴露给用户态——QEMU 映射 `KVM_RUN` 结构体的方式即此

**与虚拟化的关联**：virtio 前端（guest 驱动）+ 后端（host 侧 vhost/QEMU）通过**共享环形缓冲区**通信，其地址都经各自页表翻译；IOMMU/DMA remapping 则是"I/O 侧的二级地址翻译"，与 CPU 侧 EPT 思想同构（见 [03](03-memory-virtualization.md) §6）。

---

## 5. 中断处理

### 5.1 通用模型：上半部/下半部

中断处理的经典两分法（对两种架构一致）：

```
硬件中断到达
 └─► 上半部 (hardirq, 不可睡眠, 快进快出): 应答硬件、拷贝关键数据
      └─► 下半部 (延迟处理, 允许更重的工作):
           ├─ softirq     (硬中断/软中断上下文, 可同 CPU 重入; NET_RX/TX, TIMER, TASKLET)
           ├─ tasklet     (softirq 的简化封装, 同 CPU 串行; 逐渐弃用)
           ├─ workqueue   (进程上下文, 可睡眠, 常用于 IO/驱动慢速路径)
           └─ threaded IRQ (kthread 化下半部, request_threaded_irq)
```

API 速查：

```c
request_irq(irq, handler, flags, name, dev)          // 注册上半部
request_threaded_irq(irq, hard_fn, thread_fn, ...)   // 上半部+内核线程下半部
enable_irq()/disable_irq();  local_irq_save/restore  // 中断开关
```

### 5.2 ARM64 中断路径（GICv3）

```
设备中断 → GIC (Distributor 分发 → Redistributor 选 CPU)
  → IRQ 线拉高 → CPU interface (ICC_IAR_EL1 读取中断号 = ACK)
  → 硬件跳到 VBAR_EL1 + offset (Current EL SP_EL0/SPx × IRQ 项)
  → entry.S: el1_irq → 保存现场 → do_IRQ() (generic_handle_irq)
      ├─ IRQF_EOI 已由 GIC driver 在 desc 回调处理
      └─ handler() = 驱动注册的上半部
            └─ 触发 softirq/workqueue
  → 返回被中断上下文; 退出前 do_softirq (irq_exit)
```

ARM64 特有：
- ACK 即读 `ICC_IAR_EL1`，EOI 写 `ICC_EOIR_EL1`（优先级 drop + deactivate 可分离：`EOImode=1`）
- `DAIF` 的 I/F 位为软件中断掩码；`pseudo-NMI`（5.4+）可让部分中断绕过掩码
- per-CPU 中断（PPI/SGI）vs 共享（SPI）vs 消息中断（LPI/ITS）

### 5.3 与虚拟化的关联（主线）

- **vGIC 模拟的就是上述接口**：guest 读 GICD/GICR → stage-2 fault → KVM `vgic_mmio` 模拟；中断投递用硬件 LR 直注（见 [08 §6](08-arm64-virt-extensions.md)、[09 §5](09-kvm-arm-impl.md)）
- **维护中断**：vGIC 的 EOI 需软件介入时由 `ICH_MISR_EL2` 通知 EL2 —— 这是"中断虚拟化"中 KVM 仍保留的一个硬件钩子
- 直通设备中断经 GICv4 ITS 的 VLPI 绕过 KVM，直接进 guest LR（`kvm_vgic_v4`）

---

## 6. 本章小结与主线衔接

```
 进程/调度 ──► vCPU 线程模型 ──────────┐
 内存管理 ──► 页表/缺页/rmap ──► 内存虚拟化的"被虚拟对象" ◄── KVM
 文件系统 ──► page cache ──► memslot 映射的宿主页来源 ──┤
 驱动模型 ──► /dev/kvm 字符设备 + virtio ──────────────┘
```

下一章 [02-memory-management.md](02-memory-management.md) 深入页表数学与缺页路径——它们是理解 EPT/影子页表的必要前置。
