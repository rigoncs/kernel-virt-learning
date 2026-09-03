# 02 · 内核内存管理（虚拟化前置必修）

> 目标：建立 Stage-1 页表的完整心智模型与缺页处理全路径。这是理解 EPT / 影子页表 / PVM 的必要前置——**虚拟化就是把"本章这套机制"再叠加一层**。

---

## 1. 物理内存组织

### 1.1 三层结构

```
节点 node (pg_data_t)      # NUMA 节点
 └── 区 zone (struct zone) # ZONE_DMA / ZONE_NORMAL / ZONE_MOVABLE / ZONE_DEVICE
       └── 页 frame (struct page)  # 4KB 物理页的管理元数据
```

- `struct page`（`include/linux/mm_types.h`）：**物理页**的元数据，按物理帧号索引存于 `mem_map` 数组
- 两个常用换算：

```
pfn = phys_addr >> PAGE_SHIFT          # 物理地址 → 帧号
virt_to_page(kaddr) → mem_map[pfn]     # 内核虚拟地址 → page
page_to_pfn(page) × PAGE_SIZE = phys   # 反向
```

### 1.2 Buddy Allocator（伙伴系统）

- 每 zone 维护 `MAX_ORDER`（通常 11）个 free_list，order n 的块含 `2^n` 页
- 分配：order n 无空闲 → 找 order n+1 块对半分裂（伙伴地址 = `addr XOR (block_size)`）
- 释放：与伙伴合并，递归上溯

用途：所有 ≥ 页粒度的分配（页表、大块内核数据、DMA buffer）。小对象由 slab 系管理。

### 1.3 SLUB（对象分配器）

- 在 buddy 之上，把整页切成同尺寸对象缓存（`kmalloc-64`、`task_struct` 等 kmem_cache）
- 分配 O(1)：取 per-CPU slab 的 freelist 第一个对象；释放：头插回 freelist
- 与虚拟化的关联：`struct kvm_mmu_page`、`struct kvm_vcpu` 等高频 KVM 对象都走 slab

---

## 2. Stage-1 页表数学

### 2.1 x86-64 四级页表（Intel 4-level / 5-level 可选）

48 位虚拟地址在 4 级模式下拆分如下（页大小 4KB）：

```
63        48 47   39 38   30 29   21 20   12 11       0
┌───────────┬───────┬───────┬───────┬───────┬──────────┐
│  保留/规范  │  PML4 │  PDPT │   PD  │   PT  │  offset  │
└───────────┴───────┴───────┴───────┴───────┴──────────┘
   16 bits    9 bits  9 bits 9 bits 9 bits   12 bits
```

翻译推导（符号化）：

```
PML4E = CR3[PML4I]                    ; 每项 8B，索引 = addr[47:39]
PDPTE = PML4E.addr[PDPTI]             ; addr = 指针字段 >> 12（4KB 对齐）
PDE   = PDPTE.addr[PDI]
PTE   = PDE.addr[PTI]
PA    = PTE.addr | offset
```

大页捷径：PDE 的 PS 位=1 → 2MB 页（跳过 PT 层）；PDPTE 的 PS=1 → 1GB 页。**层数 = 4 − 大页层级**，这是 TLB miss 成本差异的根源。

PTE 关键标志位（后续影子页表协议的核心）：

| 位 | 名 | 含义 |
|----|----|------|
| P (0) | Present | 为 0 则访问触发 `#PF`（缺页） |
| R/W (1) | Writable | 0 时写触发 `#PF` |
| U/S (2) | User | 内核页与用户页隔离 |
| A (5) | Accessed | 访问时硬件置 1（反向映射/回收用） |
| D (6) | Dirty | 写时硬件置 1 |
| NX (63) | No-Execute | W^X 保护 |

### 2.2 ARM64 Stage-1（AArch64 4KB 粒度，48 位）

```
63      48 47   39 38   30 29   21 20   12 11      0
┌──────────┬───────┬───────┬───────┬───────┬─────────┐
│  保留     │  L0   │  L1   │  L2   │  L3   │ offset  │
└──────────┴───────┴───────┴───────┴───────┴─────────┘
```

- 基址寄存器：`TTBR0_EL1`（用户）/ `TTBR1_EL1`（内核），高位段自动选择
- 控制寄存器 `TCR_EL1.T0SZ/T1SZ` 决定有效地址宽度；`TG0/TG1` 选页粒度
- 大页：L1 块描述符 → 1GB，L2 块 → 2MB；L3 必为 page 描述符
- 属性编码与 x86 不同：`AP[2]`（特权/用户读写）、`PXN`、`XN`、MAIR 索引（内存类型分离在 MAIR 中）

### 2.3 TLB

- x86：部分自动管理；`invlpg` 单条失效；PCID（ASID）使进程切换可不 flush
- ARM64：**软件管理 TLB**，`TLBI VAAE1IS` 等指令显式失效；ASID 由 `TTBR0_EL1` 低 16 位携带
- **虚拟化伏笔**：二级翻译时代，TLB 缓存的是"GVA→HPA 最终结果"，必须用 (VMID/EPTP-ASID) 打 tag 区分不同 VM 的翻译，否则 VM 切换必须全量 flush（性能灾难）。

---

## 3. 地址空间布局

x86-64（48 位）典型布局：

```
0x0000_0000_0000_0000 ── 用户空间（128TB，task->mm）
0xffff_8000_0000_0000 ── 内核空间
   ├── 直接映射区 (direct map)：pa = va - PAGE_OFFSET，恒等偏移
   ├── vmalloc 区（物理不连续）
   ├── kmap/PKMAP（32 位遗留；64 位仅 ZONE_DEVICE 用）
   └── 内核映像 (__start_rodata 等)
```

直接映射区是 `gfn_to_pfn` 之类的 phys→virt 转换的基础（`page_address()`）。

---

## 4. 缺页中断全路径（重点）

### 4.1 x86 路径

```
硬件: 访存异常 → #PF → IDT 14 号门
  asm:  asm_exc_page_fault (arch/x86/entry/)
   └─► exc_page_fault()
        ├─► user_mode(regs)? fault 地址 = CR2
        └─► do_user_addr_fault()            # arch/x86/mm/fault.c
              ├─► find_vma(mm, addr)        # 找到覆盖地址的 VMA
              ├─► handle_mm_fault()         # mm/memory.c
              │     ├─► __handle_mm_fault()
              │     │     ├─► alloc/set PGD→P4D→PUD→PMD→PTE（缺表则分配）
              │     │     └─► handle_pte_fault()
              │     │           ├─► pte_none  → do_fault()      # demand paging / 文件页
              │     │           ├─► !pte_write → do_wp_page()   # COW
              │     │           └─► swap entry → do_swap_page() # 换入
              │     └─► set_pte_at()        # 最终装填 PTE
              └─► 完成后重执行故障指令
```

### 4.2 关键子路径

**COW（写时复制）**——`do_wp_page()`：

```
写只读 PTE → 发现 VMA 可写 → pte 为 COW 映射
  → 页引用 >1 或在 swap cache：alloc 新页 + copy + 建新 PTE（可写）
  → 唯一引用：直接改写 PTE 为可写
```

**Demand paging / 文件页**——`do_read_fault` / `do_cow_fault` / `do_shared_fault`：经由 `filemap_fault()` 查页缓存，miss 则下发读 IO。

### 4.3 ARM64 路径对照

```
ESR_EL1.EC == 0x24/0x25/0x26/0x27   # Data Abort（读/写/对齐等细分在 ISS）
  └─► do_mem_abort() ─► do_page_fault()   # arch/arm64/mm/fault.c
        └─► handle_mm_fault()              # 与 x86 共用通用层
```

FAR_EL1 = 故障地址；ISS 位域区分读/写、权限/翻译错误——与 x86 的 error code 对应。

### 4.4 与虚拟化的对照（核心伏笔）

| Stage-1 缺页要素 | 虚拟化对应物 |
|---|---|
| CR2 / FAR_EL1 故障地址 | EPT violation 的 GVA/GPA（VMCS `GUEST_PHYSICAL_ADDRESS`） |
| `present/perm` 错误码 | EPT violation 退出限定符（read/write/execute 位） |
| `handle_mm_fault` 建表 | KVM `kvm_mmu_page_fault` 建影子/EPT 表 |
| COW 写保护 | KVM 对 guest 页的写保护（SPT 同步 / Log-dirty / KSM） |
| rmap 反查 | KVM 的 gfn→spte rmap（SPT 模式） |

---

## 5. 反向映射（rmap）与页回收

- **正向**：VMA→PTE→page（找数据快）；**反向**：page→所有映射它的 VMA/PTE（回收/迁移时必须）
- 结构：`struct page` 中的 `mapping`（匿名时指向 `anon_vma`，文件页指向 `address_space`）+ interval tree 定位 VMA
- 回收器 `vmscan.c`：LRU 链表（active/inactive × anon/file），页被访问时 PTE.A 置位 → `rmap` 找到 VMA → 清位、降级、必要时 swap/unmap
- **虚拟化关联**：宿主把某个 guest 正在用的页回收（unmap 时给宿主 PTE 建了 swap entry / 移除映射）→ **必须通知 KVM 摘掉对应的 EPT/影子表项**，否则 guest 仍在通过旧 EPT 映射直读物理页——这正是 MMU notifier 机制（见 [04](04-kvm-memory-impl.md) §4）。

---

## 6. 大页、KSM、THP 速览

| 机制 | 作用 | 虚拟化中的角色 |
|------|------|----------------|
| THP (Transparent Huge Pages) | 自动为用户匿名内存建 2M 页 | KVM 尝试建 2M EPT 大页，降低 TLB 压力 |
| hugetlbfs | 预留固定大页池 | QEMU `-object memory-backend-file,hugetlbfs` |
| KSM | 扫描相同内容页合并（COW 语义） | 同配置 VM 内存去重；与 KVM log-dirty 有交互成本 |
| MGLRU / zswap | 回收加速 / 压缩缓冲 | 大内存 VM 的 host 侧回收路径 |

---

## 7. 自测题

1. 手算：`0x00007f3a_9c21b678` 的 PML4/PDPT/PD/PT 索引与 offset。
2. `copy_page_range` 复制页表后，父子进程同一虚拟页的 PTE 各是什么状态？第一次写分别发生什么？
3. 为什么 `struct page` 是按物理页组织而非虚拟页？
4. 宿主回收一个被 KVM EPT 映射的页，若没有 MMU notifier 会发生什么安全事故？
5. 直接映射区为什么必须是恒等偏移映射？

---

## 源码地图

| 主题 | 文件 |
|------|------|
| 页表遍历/建表 | `mm/memory.c`（`handle_mm_fault` 等） |
| 缺页 x86 | `arch/x86/mm/fault.c` |
| 缺页 ARM64 | `arch/arm64/mm/fault.c` |
| rmap | `mm/rmap.c` |
| 回收 | `mm/vmscan.c` |
| buddy | `mm/page_alloc.c` |
| SLUB | `mm/slub.c` |
| 页表 walkers 工具 | `mm/pagewalk.c`（`/proc/pid/pagemap` 读侧在 `fs/proc/task_mmu.c`） |
