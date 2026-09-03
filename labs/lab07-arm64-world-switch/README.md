# Lab 07 · 追踪 KVM/ARM World Switch 与 Stage-2 Fault

> 目标：在真实 ARM64 + KVM 环境上，用 ftrace/trace-cmd 抓取完整 world switch 链路、stage-2 fault 处理与 vGIC/timer 活动；用 GDB 单步 `__guest_enter` 建立"软件上下文切换"的肌肉记忆。

对应文档：[docs/09 §2/§4](../../docs/09-kvm-arm-impl.md)、[docs/06](../../docs/06-debugging-tools.md)。

前置：ARM64 主机（或 ARM64 云主机）+ `/dev/kvm` + 一个 QEMU guest（`-accel kvm -cpu host -M virt,gic-version=3`）。

---

## Part A：KVM/ARM tracepoint 全景

```bash
cd /sys/kernel/debug/tracing   # sudo

echo 1 > events/kvm/kvm_entry/enable
echo 1 > events/kvm/kvm_exit/enable      # ARM 版含 esr_ec 字段!
echo 1 > events/kvm/kvm_inject_irq/enable
echo 1 > events/kvm/kvm_mmio/enable
echo 1 > events/kvm/kvm_set_spte/enable
# vGIC / timer (内核版本相关, ls events/kvm* 查看):
echo 1 > events/kvm/kvm_timer/enable 2>/dev/null || true

echo 1 > tracing_on
# guest 内: stress-ng --vm 1 --vm-bytes 256M -t 3   (制造大量 stage-2 fault)
# 或 guest 内: cat /dev/urandom > /dev/null         (纯内存负载)
echo 0 > tracing_on
cp trace /tmp/arm-kvm-trace.txt
```

**分析任务**：
1. 统计 `kvm_exit` 的 `esr_ec` 分布：`grep kvm_exit /tmp/arm-kvm-trace.txt | grep -o 'esr_ec=0x[0-9a-f]*' | sort | uniq -c | sort -rn`
   - 预期看到 `0x24`/`0x25`（Data Abort）、`0x18`（sysreg 陷阱）、`0x16`（SVC）等，对照 [docs/07 §5.3](../../docs/07-arm64-architecture.md)
2. 找一条完整的 entry→exit 对，标注两次事件之间 guest 大约执行了多久（时间戳差）
3. MMIO 事件与 `esr_ec=0x24` 的对应关系

## Part B：function_graph 抓 world switch 链

```bash
echo function_graph > current_tracer
echo kvm_arch_vcpu_ioctl_run > set_graph_function
# 更深: 再加 handle_exit / kvm_handle_guest_abort (echo 追加)
echo kvm_handle_guest_abort >> set_graph_function

echo 1 > tracing_on; sleep 2; echo 0 > tracing_on
cat trace | head -100
```

**应在 trace 中找到并标注**：
```
kvm_arch_vcpu_ioctl_run() {
  __kvm_vcpu_run() {            ← hyp 入口 (VHE)
    __guest_enter()             ← 汇编, 加载 guest 寄存器 + ERET
      [ guest 运行, 此段时间不在 trace 中! ]
    __guest_exit()              ← 保存 guest 寄存器, 返回 exit_code
  }
  handle_exit() {
    kvm_handle_guest_abort() {  ← stage-2 fault
      gfn_to_hva / hva_to_pfn (可能嵌套 host 缺页!)
      kvm_pgtable_stage2_map    ← 填描述符
    }
  }
}
```

**关键观察**：guest 运行时间在 trace 中是"空洞"——用时间戳估算 guest/host 时间比例（虚拟化效率的直接度量）。

## Part C：vGIC 与定时器活动

```bash
# guest 内制造定时器/中断负载: while :; do date; done
echo function_graph > current_tracer
echo 'kvm_vgic_*' > set_graph_function 2>/dev/null   # 或逐个: kvm_vgic_flush_hwstate
echo kvm_vgic_flush_hwstate > set_graph_function
echo kvm_vgic_sync_hwstate >> set_graph_function
echo 1 > tracing_on; sleep 1; echo 0 > tracing_on

# 观察: flush(进 guest 前装 LR) ↔ sync(退出后收 LR) 是否成对
# 思考: 为什么它们必须在关抢占临界区内? (docs/09 §5)
```

## Part D：GDB 单步 world switch（可选，需自编内核）

```bash
# 1. 按 docs/10 §1 编译调试内核, QEMU 加 -s -S
gdb-multiarch vmlinux
(gdb) target remote :1234
(gdb) b kvm_arch_vcpu_ioctl_run
(gdb) c
(gdb) step / next       # 走到 __kvm_vcpu_run
(gdb) x/20i $pc         # 观察汇编: callee 保存 + MSR VTTBR_EL2 + ERET
# 注意: ERET 后即进入 guest, gdb 无法跟踪 guest 代码 (无符号)
#       重新断在 __guest_exit 观察保存路径
```

**思考题**：为什么 GDB 跟不过 ERET？（提示：gdb 调试的是 host 内核符号；pKVM 下 hyp 代码甚至对 gdb 也隔离。）

## 自检标准

- [ ] 能画出本机抓到的 world switch 时序图（entry→guest 空洞→exit→handle_exit→返回）
- [ ] 能对本机 `esr_ec` 分布 Top3 给出解释（为什么该负载下这类退出多）
- [ ] 能指出 trace 中"guest 缺页嵌套 host 缺页"的位置
- [ ] 能解释 flush/sync_hwstate 成对出现的原因
