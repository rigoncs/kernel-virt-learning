# 04 · KVM 内存子系统实现剖析

> 前置：[03](03-memory-virtualization.md)。本章把理论落到 KVM 源码：memslot、KVM MMU（含 TDP MMU）、MMU notifier、缺页全链路。

---

## 1. KVM 总体架构回顾

```
用户态 QEMU                     内核 KVM
┌──────────────┐   ioctl    ┌─────────────────────────────┐
│ vCPU 线程     │──────────► │ kvm_arch_vcpu_ioctl_run()   │
│  KVM_RUN     │            │  └─► vcpu_run() 主循环       │
│ KVM_SET_USER │──────────► │  └─► VM-entry/VM-exit        │
│ _MEMORY_REGION│           │      (EPT violation 处理)    │
└──────────────┘            └─────────────────────────────┘
        ▲                              │
        └───── VMexit 退出原因 ─────────┘
```

- 通用层：`virt/kvm/kvm_main.c`（API、vCPU 生命周期、MMU notifier 注册）
- x86 MMU：`arch/x86/kvm/mmu/`
- ARM64：`arch/arm64/kvm/mmu.c`（stage-2 建立/拆除、用户态 IPA 映射）

`kvm_arch_vcpu_ioctl_run` 主循环语义：**`ret > 0` 继续 run（在内核处理了 exit），`ret = 0` 退出到用户态（QEMU 介入），`< 0` 错误**。退出责任判定：`handle_exit` 返回值决定（>0 KVM 已处理，0 需 VMM 介入，<0 错误）。

---

## 2. memslot：GPA→HVA 的地基

### 2.1 数据结构

```
struct kvm_memory_slot {
    gfn_t base_gfn;              // 起始 guest 页帧
    unsigned long npages;
    unsigned long __user *userspace_addr;  // HVA
    struct kvm_memory_arch_flags arch;     // 含 readonly/log_dirty 等 flags
    struct {
        struct kvm_rmap_head *rmap;        // SPT 模式 gfn→影子PTE 反向表
        unsigned long *dirty_bitmap;       // 热迁移脏页记录
    } arch;                                // (示意，实际位于 kvm_memslots 相关结构)
};
```

所有 slot 存于 `kvm->memslots`，按 gfn 排序的二叉/数组结构（`slots->id_to_index`、`gfn_to_memslot()`）。

### 2.2 RCU 双缓冲更新

关键设计：**读写不对称**。

- 读路径（每秒百万次的 gfn 查询）：`srcu_dereference(kvm->memslots)`，无锁
- 写路径（`KVM_SET_USER_MEMORY_REGION`，低频）：
  1. 分配新 memslots 副本 → 修改 → `rcu_assign_pointer` 切换指针
  2. `synchronize_srcu_exclusive()` 等待所有读者退出旧副本
  3. 对旧 slot 做对应的 EPT/rmap 拆除（zap）

失效策略：slot 变更发出 `KVM_REQ_MMU_RELOAD` / `KVM_REQ_MEMSLOT_UPDATE`，vCPU 在进入 guest 前处理，保证不带着旧翻译进 guest。

---

## 3. KVM MMU：三套角色一套代码

`arch/x86/kvm/mmu/` 同时管理两类表：

| 角色 | 驱动的页表 | 说明 |
|------|-----------|------|
| 影子模式 | host 页表（由 guest CR3 触发） | 无 EPT 硬件时；也用于嵌套 |
| TDP 模式 | EPT/NPT | 主流：`tdp_mmu.c` |
| 嵌套（nested） | guest 虚拟 EPTP 指向的"nested EPT" | L1 是 hypervisor 时 |

### 3.1 核心抽象：`kvm_mmu_page`（SP）

影子页表/EPT 页缓存的基本单位：

```
struct kvm_mmu_page {
    gfn_t gfn;                  // 该页表页覆盖的 gfn 区间基址（按 role.level）
    union kvm_mmu_page_role role { level, direct, access, quadrant, ... };
    struct hlist_node hash_link; // 按 (role, gfn) 哈希查重：避免重复建表
    u64 *spt;                    // 实际的 512 项页表页（物理连续，slab 分配）
    ...
};
```

- 4 级 EPT：level 4（PML4, direct）→ level 1（4K 叶子）
- `direct` 角色：不经过 guest 页表（TDP 模式），gfn 直接展开（great for 大页合并）
- 哈希查重 + `role` 使同层级同区间的页表页可复用（TLB/内存友好）

### 3.2 SPTE 的软件保留位

硬件 PTE 位用尽，KVM 在 SPTE 的高位偷存状态（`mmu.h`）：

```
SPTE_MMU_PRESENT, SPTE_MMU_WRITABLE, SPTE_HOST_WRITABLE,
shadow-accessed/dirty 模拟位, remapped/volatile 标记 ...
```

**host writable 位**是 MMU notifier 联动的关键：记录该 spte 映射的宿主页当前是否可写（用于决定 FOLL_WRITE 获取和写保护降级）。

### 3.3 TDP MMU 设计要点（`tdp_mmu.c` / `tdp_iter.c`）

1. **迭代器而非递归**：`tdp_iter_next()` 沿页表 walk 迭代，支持"边走边改"
2. **RCU 保护遍历 + 分层锁**：
   - 读 SP（页表页连接关系）：RCU
   - 改 SPTE：`mmu_lock` 保留但 **改成尽量短临界区**；叶子 SPTE 的原子改写用 `cmpxchg64`（`tdp_mmu_set_spte_atomic`）
   - 链接新 SP：先 `rcu_assign_pointer` 再由写者等宽限期回收
3. **并发 fault**：多个 vCPU 同时 fault 同一区间可并行填表（旧 MMU 因整树 mmu_lock 串行化）
4. **yield 安全**：长遍历（zap 整个 memslot）中 `tdp_mmu_iter_cond_resched` 主动让出，避免软锁

### 3.4 缺页主流程（TDP 模式，必须烂熟）

```
guest 访问 GVA
  ├─ TLB hit → 直接访存
  └─ TLB miss → 硬件 walk guest PT → 得 GPA
        └─ 硬件 walk EPT → miss/权限不足
              └─ VM-exit (EPT violation, 退出限定符 R/W/X)
                    ▼
vmx_handle_exit() → handle_ept_violation()
  └─► kvm_mmu_page_fault(vcpu, gpa, error_code)
        ├─ gfn = gpa >> PAGE_SHIFT
        ├─ (mmio?) kvm_mmu_unprotect → fast_page_fault（SPTE 软件位可原地裁决的竞争）
        ├─ kvm_mmu_get_page(...)             # 建/复用上层 SP
        ├─ host 侧取物理页:
        │    hva = gfn_to_hva(kvm, gfn)
        │    pfn = hva_to_pfn_retry(... ksm/thp 处理, FOLL_GET)
        │    └─ 【这里可能触发 host 缺页: handle_mm_fault】
        ├─ make_spte(...)  # 权限合成: gpte 权限 ∩ memslot 权限 ∩ host 页可写性
        └─ tdp_mmu_set_spte_atomic / tdp_mmu_map(...)  # 原子填 EPT 叶子
              └─ KVM_REQ_TLB_FLUSH 视需要
                    ▼
              VM-resume，guest 重执行指令
```

**大页策略**：fault 处理时若 gfn 落在一个 host THP 对齐的 2M 区间且无冲突（无写保护/无 dirty log 拆分需求），KVM 会直接从 PTE 层"分裂落位"为 PMD 层 2M SPTE；后续 `kvm_mmu_zap` 可因 dirty logging 拆分回 4K。

---

## 4. MMU notifier：宿主内存事件如何传到 KVM

问题：guest 内存就是 QEMU 进程的普通匿名内存，host 的回收/迁移/THP 合并/`MADV_DONTNEED` 会随时改宿主页表——**KVM 的 EPT 里却缓存着旧 hpa 映射**。

机制：QEMU 打开 VM 时以 `KFDSLOT`+ notifier 注册；KVM 在 `kvm_init` 时把自己挂到 mmu notifier 链（`kvm_main.c: kvm_mmu_notifier_ops`）：

| 回调 | 触发场景 | KVM 动作 |
|------|----------|----------|
| `invalidate_range_start` | 迁移/回收/分裂开始 | 对 [start,end) HVA→GFN 区间 zap 对应 SPTE（`kvm_mmu_notifier_invalidate_range_start`） |
| `invalidate_range_end` | 上述结束 | 释放引用、清请求 |
| `change_pte` | 同址换页（KSM 快路径） | 尝试原地替换 SPTE |
| `clear_flush_young` | LRU aging | 回应宿主页访问位（映射到 SPTE 软件 Accessed 位） |

**语义**：notifier 保证"宿主 PTE 失效 ⇒ guest EPT 失效"，二者因果闭合。反向（guest 侧写保护降级再升级）由 `kvm_mmu_make_writable` / `FOLL_WRITE` 引用计数兜底。

关键一致性问题：`invalidate_range_start` 中 zap 与 fault 并发——TDP MMU 用原子 SPTE 交换 + 重试（`hva_to_pfn_retry` 循环）保证"拿到的 pfn 与最终写入 SPTE 时宿主页未变"。

---

## 5. 脏页跟踪与热迁移联动（简述）

- `KVM_GET_DIRTY_LOG`：KVM 把对应 memslot 的 EPT 叶子降为只读，写 trap 时置 `dirty_bitmap` 位并恢复可写（PML 硬件加速时由 CPU 自动记录，大幅降 trap）
- 与 MMU notifier、KSM 的交互：merge 前需先清脏记录

---

## 6. ARM64 KVM 内存实现要点

- IPA（Intermediate Physical Address）= GPA。**stage-2 表是 per-VM 资源**，由同一 VM 的所有 vCPU 共享
- 首次访问延迟建表：guest 第一次访问某 IPA 区间 → Data Abort (S2) → `kvm_handle_guest_abort()` → `user_mem_abort()`：gfn→hva→pfn（同样走 `gfn_to_pfn`）→ 建 stage-2 描述符（`kvm_pgtable_stage2_map`，`arch/arm64/kvm/hyp/pgtable.c`——注意这段代码运行在 EL2/nVHE 语境）
- 权限合成：VMA 权限 ∩ memslot 权限 ∩（设备/MMIO 判定）
- TLB：`__tlb_switch_to_guest()` 切 VMID 后 `TLBI`；hyp 侧 `__kvm_tlb_flush_vmid()`

---

## 7. 自测题

1. 用 5 步以内描述 `KVM_SET_USER_MEMORY_REGION` 缩小一个 slot 后，正在运行的 vCPU 如何感知？为什么不用全局锁？
2. `fast_page_fault` 解决什么竞争？为什么读 SPTE 软件位就能避免慢路径？
3. 迁移一个 2M EPT 大页时 notifier 回调的粒度是什么？拆分动作发生在哪？
4. 为什么 stage-2 表是 per-VM 而 VCPU 寄存器上下文是 per-VCPU？
5. 画出 lab02 追踪到的 `kvm_mmu_page_fault` 调用树，并标注每个函数读/写了哪些结构。

---

## 源码地图

| 主题 | 文件 |
|------|------|
| API 主入口 | `virt/kvm/kvm_main.c`（`kvm_dev_ioctl`, `kvm_vcpu_ioctl`, `kvm_mmu_notifier_*`） |
| memslot 实现 | `virt/kvm/kvm_main.c` + `include/linux/kvm_host.h` |
| x86 exit 分发 | `arch/x86/kvm/vmx/vmx.c`（`vmx_handle_exit`） |
| MMU 核心 | `arch/x86/kvm/mmu/mmu.c` |
| TDP MMU | `arch/x86/kvm/mmu/tdp_mmu.c`, `tdp_iter.c`, `tdp_iter.h` |
| SPTE 格式 | `arch/x86/kvm/mmu/mmu.c`（`make_spte`）, `spte.h` |
| ARM64 stage-2 | `arch/arm64/kvm/mmu.c`, `arch/arm64/kvm/hyp/pgtable.c` |
| 自测样例 | `tools/testing/selftests/kvm/` |
