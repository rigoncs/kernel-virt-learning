# 资源索引（ARM64 主线 · 自包含说明 + 外链补充）

> 本仓库文档已自包含核心知识；下列资源为扩展阅读与一手材料。按"必读 → 进阶 → 参考"分层，ARM64 相关置顶。

---

## 1. 官方文档与规范（一手，免费）

| 资源 | 位置/链接 | 用途 |
|------|-----------|------|
| **Arm ARM (DDI 0487)** | developer.arm.com → Arm Architecture Reference Manual | AArch64 全部机制权威定义（docs/07/08 的规范依据） |
| Arm ARM Supplement（GIC） | Arm Generic Interrupt Controller Architecture Specification (GICv3/v4) | vGIC（docs/08 §6） |
| **PSCI / SMCCC 规范** | developer.arm.com → PSCI, SMCCC | vCPU 上电/hypercall 约定（docs/07 §6） |
| Arm System Memory Management Unit (SMMU) 规范 | developer.arm.com | 设备直通（docs/09 §7.3） |
| 内核文档 KVM 部分 | 源码树 `Documentation/virt/kvm/`：api.rst、**devices/vgic.rst**、arm/ | KVM/ARM API 权威说明 |
| 内核文档其他 | `Documentation/arch/arm64/`（booting.rst, memory.rst, virtualization.rst） | ARM64 架构内核侧 |
| Intel SDM Vol.3 / AMD APM Vol.2 | intel.com/sdm / amd.com | x86 对照（EPT/NPT，docs/03） |
| KVM API 在线版 | docs.kernel.org/virt/kvm/api.html | 与 api.rst 同源 |

## 2. 论文与演讲（虚拟化主线）

| 材料 | 主题 |
|------|------|
| **Dall & Nieh, "KVM/ARM: The Design and Implementation of the Linux ARM Hypervisor" (NSDI'14)** | KVM/ARM 开山之作，docs/09 §1-2 的理论源头，**ARM 主线必读** |
| Popek & Goldberg (1974) | 虚拟化可判定性经典 |
| Adams & Agesen (ASPLOS'06) | SPT vs EPT 原始对比（docs/03） |
| Waldspurger (OSDI'02) | 内存超卖/ballooning |
| **KVM Forum: "KVM/ARM" 系列、pKVM/hypervisor-as-a-service、Nested Virtualization on ARM** | kvm-forum.github.io 历年存档；pKVM 与 NV 的一手材料 |
| Linux Plumbers Conference（virtualization microconf） | 年度虚拟化议题 |

## 3. 书籍

| 书 | 定位 |
|----|------|
| 《Understading the Linux Virtual Memory Manager》(Gorman) | MM 深入（思想仍适用） |
| 《Linux Kernel Development》(Love) | 子系统入门 |
| 《深入理解 Linux 内核》(ULK) | 内核基础框架 |
| 《系统虚拟化：原理与实现》(Intel 开源技术中心) | 虚拟化系统教材（x86 视角） |
| 《ARM 体系结构与编程》/ Arm 官方 "Arm Cortex-A Series Programmer's Guide" | ARM 架构入门（后者免费且官方） |

## 4. 开源项目（学习案例，ARM 优先）

| 项目 | 关注点 |
|------|--------|
| linux 源码 `virt/kvm/arm/`, `arch/arm64/kvm/`, `arch/arm64/kvm/hyp/` | **主战场**（docs/09 源码地图） |
| QEMU (gitlab.com/qemu-project/qemu) | 用户态 VMM：`accel/kvm/`、`hw/intc/arm_gicv3*`（vGIC 对照）、`hw/arm/virt.c`（lab06 的 virt 机型） |
| kvm-unit-tests (gitlab.com/kvm-unit-tests) | `arm/` 目录：stage-2/中断/timer 测试（lab04） |
| KVM selftests（内核树 `tools/testing/selftests/kvm/aarch64/`） | 现代 KVM/ARM 测试范式 |
| TF-A (Trusted Firmware-A, git.trustedfirmware.org) | EL3/PSCI 固件实现（理解启动链） |
| OpenCloudOS-Kernel PVM 分支 | 基于页表的虚拟化框架（docs/05） |
| Cloud Hypervisor / Firecracker (aarch64) | Rust 微 VMM 的 ARM64 实现 |
| kvmtool (kernelvirt) | 极简 KVM/ARM 用户态 VMM，比 QEMU 更适合通读 |

## 5. 在线课程 / 视频

| 资源 | 说明 |
|------|------|
| KVM Forum 历年视频 (kvm-forum.github.io / YouTube 存档) | 优先看 ARM 议题：KVM/ARM、pKVM、Nested、GICv4 |
| LWN.net：KVM/ARM 特稿、pKVM 系列文章 | 深度综述 |
| 《操作系统：三个简单的部分》等公开课 | 仅作 S1 补充 |

## 6. 工具速查（详见 docs/06、docs/10 §3）

ftrace / trace-cmd / perf / perf-kvm / bpftrace / crash / **gdb-multiarch**（ARM64 必备）
/ kvm debugfs statistics / QEMU monitor（`info registers`）/ virsh dump / faddr2line / `scripts/gdb/vmlinux-gdb.py`

## 7. 建议的最小阅读顺序（ARM64 主线）

1. `Documentation/virt/kvm/api.rst` 的 ARM64 小节 + `devices/vgic.rst`（配合 docs/09）
2. KVM/ARM NSDI'14 论文（配合 docs/09 §1–2）
3. Arm Cortex-A Programmer's Guide 的 Virtualization 章（配合 docs/08）
4. KVM Forum 近三年的 pKVM / Nested on ARM 演讲
5. 选一个真实问题（如"guest 中断延迟抖动"）从 trace（lab07）到源码走完整闭环
