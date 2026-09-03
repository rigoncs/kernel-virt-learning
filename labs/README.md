# Labs 目录：实践实验总览（ARM64 主线）

| Lab | 名称 | 对应文档 | 核心产出 |
|-----|------|----------|----------|
| [lab06](lab06-arm64-kvm-vm) ★ | ARM64 最小 KVM 虚拟机 | docs/07–09 | `KVM_ARM_VCPU_INIT`/PSCI/Stage-2 fault/MMIO（ISS 数据回填）全流程 |
| [lab07](lab07-arm64-world-switch) ★ | KVM/ARM world switch 追踪 | docs/06, 09 | ftrace 抓取 world switch/vGIC 链路 + GDB 单步 `__guest_enter` |
| [lab08](lab08-kernel-module) ★ | ARM64 内核模块 | docs/10 | 交叉编译→QEMU 装载→VHE 观测的模块闭环 |
| [lab02](lab02-ftrace-tracing) | ftrace 追踪缺页与 KVM 路径 | docs/02, 04, 06 | 完整抓取 host 缺页 / guest EPT fault 链路 |
| [lab03](lab03-mmu-notifier) | MMU notifier 实验 | docs/04 | 理解宿主页失效与 KVM 联动（架构无关，ARM 同样适用） |
| [lab04](lab04-kvm-unit-tests) | kvm-unit-tests 验证 | docs/03 | EPT/Stage-2 语义测试（ARM64 跑 `arm/` 用例） |
| [lab01](lab01-minimal-kvm-vm) | x86 最小 KVM 虚拟机（对照） | docs/03, 04 | 与 lab06 对照：x86 的 API 差异与 EPT violation |
| [lab05](lab05-pvm-experiments) | PVM / 影子页表观测 | docs/05 | 影子模式对比实验；PVM 支持探测 |

★ = ARM64 主线实验。

通用约定：
- 所有实验在 `~/src/kernel-virt-learning/labs/<lab>/` 下进行，不修改内核源码树
- 需要 root 权限的操作已单独标注
- 实验环境：见 `config/env.md` 与 `docs/10`；无 ARM64 硬件时可用 QEMU TCG（`qemu-system-aarch64 -accel tcg`）完成 lab06/lab08 流程（慢但完整），lab07 需要 KVM 加速环境（ARM64 主机/云主机）
- 内核基准 5.10+ LTS；涉及新版本特性（pKVM、Nested）在对应 lab 中标注
