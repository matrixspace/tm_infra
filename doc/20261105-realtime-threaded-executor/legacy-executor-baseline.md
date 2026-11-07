# LegacyDedicatedThreadExecutor profiling 基线

**任务开始时间：** 2026-11-07

**任务结束时间：** 2026-11-07

**状态：** 本轮完成；第二阶段整体验收状态不变

**分支：** `feature/realtime-threaded-executor`（infra commit `6333b4a`；basic commit `02079d0`）

## 背景与目标

按用户要求，把最初 develop 上的 ThreadedHandler 等待 / 队列实现迁移到当前 Executor 接口，命名 LegacyDedicatedThreadExecutor，标记 deprecated，仅作 profiling 基线，不改变当前默认执行器。再次运行吞吐、分配 / 移动计数、空闲 CPU、单跳及多跳端到端基准，纳入 Legacy 对照。

开始时已读根 CLAUDE.md、当前开发记录、git status / diff / 历史与测试源码。原始代码固定为 tm_infra develop `f569d42` 的 `RealTimeApp.hpp::ThreadedHandlerBase`，不随将来 develop 变化漂移。

## 方案

保留 mutex + incoming / processing 两条 std::list、splice、每次 push 通知，以及无 predicate 的 wait_for(1ms)，包括处理中通知丢失后下一批仍等待的历史成本。不要修复该等待方式，否则无法充当历史算法基线。

迁移仅作接口 / 安全适配：Host 承接等待启动、处理、idle 与批次统计；TimeChecker 继续由当前 handler 核心负责。状态由 detached 线程 shared_ptr 持有，避免直接复制原 this 生命周期问题；逐消息检查 stop，遵守当前停止后不继续回调的契约。原版 idle 即使无工作也周期触发，基线保留该特征，忽略 spinWait / hasIdleWork 优化。

## 代码改动

- infra `LegacyDedicatedThreadExecutor.hpp`：独立、deprecated 的 Executor 模板，只在测试与 bench 显式引用；原 mutex/list/splice/无 predicate wait_for(1ms) 路径保留，spinWait 不参与。
- infra `tm_kit/infra/meson.build`：安装显式头文件。默认 `RealTimeThreadedExecutor.hpp`、默认选择器和当前三个执行器均没有修改。
- infra `LegacyExecutorTest.cpp` / Meson：新增正式测试目标。
- basic `ExecutorBench.cpp` / Meson：追加 `legacy` 和独立 normal/counts 后端（ID 7）。
- basic `ExecutorLatencyBench.cpp`：支持 legacy，JSON 追加实际 warmup。
- basic 两个 Python runner：`--candidate-only --include-legacy` 对照同一代码版本中的所有后端，保留已有成对比较默认行为；延迟 runner 新增统一 `--warmup`，legacy 只运行无效自旋预算标记 0。
- 工具 README、两仓库 INDEX 和主任务追加关联说明。

## 测试与验证

先 `source build-common/build-env-setting.sh`，使用 GCC 15.2.1 / C++17，只构建相关目标，`ninja -j2`。构建目录和 install 均使用原 feature worktree 的私有路径。

```sh
ninja -j2 -C worktrees/realtime-threaded-executor/tm_infra/builddir_release \
  test/threaded_handler_test/legacy_executor_test install
meson test -C worktrees/realtime-threaded-executor/tm_infra/builddir_release --no-rebuild
ninja -j2 -C worktrees/realtime-threaded-executor/tm_infra/builddir_debug \
  test/threaded_handler_test/legacy_executor_test
meson test -C worktrees/realtime-threaded-executor/tm_infra/builddir_debug --no-rebuild
```

infra release / debug 均 4/4 通过（legacy、mechanism、threaded_handler、time_checker）。basic release 既有 chain_writer_loop / moodycamel_executor / tsc_clock 3/3 通过。Legacy 新测试分别使用 `-std=c++17 -O1 -g -pthread -fsanitize=thread` 与 `-fsanitize=address,undefined`，均 ALL PASSED，无 sanitizer 报告；ASan 使用 `ASAN_OPTIONS=detect_leaks=0`，未宣称完成 LeakSanitizer 检测。

basic 相关构建：generic normal/counts、latency，以及 direct / inline / inline_erased / dedicated / pool / moody / legacy 每后端 normal/counts，共 17 个手动 profiling 目标。预期的 class deprecated 编译诊断保留；没有全局禁止该警告。


Legacy 多跳闭环可能每跳约 1ms，旧默认 20000 条 / 2000 条预热可能超出 profiling 超时；为各引擎使用相同的明确样本量与较短预热，记录样本数，不将过少样本的 p999 当作稳定尾延迟。

## 兼容性与注意事项

独立头文件、显式 include、class [[deprecated]]，生产默认路径不引用该类。不是完整旧 RealTimeApp 的二进制复刻，只对照旧执行器算法；新的 Host / TimeChecker 和安全适配仍存在。examples 全量编译继续暂停，主任务第二阶段整体验收状态不变。

## 相关文档

- [主任务](README.md)
- [执行器开销](executor-overhead-optimization.md)
- [端到端延迟优化](executor-latency-optimization.md)
- [profiling 工具](../../../tm_basic/test/executor_bench/README.md)

### 实施检查点

- Legacy 独立头文件并加入安装清单，不由默认 Executor 头文件自动 include；class deprecated 诊断保留。
- 正确性测试覆盖四生产者 × 5000 move-only/non-default 消息、FIFO / payload、启动门控、原 splice 批次统计、即使 hasIdleWork=false 仍周期 idle、停止后拒收、回调内 stop 截断当前批次和启动等待期间析构。Host 测试夹具保持到进程退出，遵循 detached 生命周期契约。
- infra release/debug 均 4/4，通过；新测试 TSan 和 ASan/UBSan 通过，LeakSanitizer 同前轮环境限制未启用。
- 首次 basic benchmark 编译发现 generic 工具原来仅间接 include RealTimeThreadedExecutor，迁移时漏加 Legacy 显式 include；已补上并重新定向构建。未修改生产默认 include 路径，也未全局关闭 deprecated 警告。
- 两个 runner 新增 candidate-only/include-legacy 模式；旧的成对比较默认列表保持不变。延迟 runner 可指定统一 warmup，Legacy 不重复运行无效的自旋配置。吞吐 / 计数 split 模式追加 engine ID 7，不改变旧 ID。

## 最终 profiling（2026-11-07）

所有执行器均来自本轮同一源码版本，不使用旧轮保存的可执行文件混合比较。配置为 release，normal / counts 分离，吞吐使用每后端独立编译的 split 目标。吞吐 5 次重复，闭环 / 固定周期 / 超额订阅各 3 次；普通计时不启用分配计数，分配数据不用于判断延迟 / CPU 收益。perf stat 对各真实执行器保存整进程 counters（含预热、输出），不等同内部测量窗口。

共 460 行数据全部正确性检查通过：普通完整矩阵 244 行（225 次计时、14 个计数案例、5 个 perf stat）、多跳闭环 144、固定周期 48、超额订阅 24。原始 JSONL、二进制 SHA256、各表和 CSV 见 [legacy-profiling](legacy-profiling/full/results.jsonl)；完整复现命令见 [reproduce.sh](legacy-profiling/reproduce.sh)。所有 profiling 串行运行，不与编译并行。

CPU：同一 NUMA 节点、无测试进程内 SMT sibling 冲突。普通矩阵 producers 16,18,20,22，consumers 24,26,28,30；多跳 producer 20，consumers 24,26,28,30,32,34,36,38。超额订阅另外固定到 24,25,26,27 四个消费 CPU。与前轮不同的预热 / 样本数及角色核配置意味着不能把跨轮数值差异解释为本轮实现收益。

### 吞吐：四生产者，小 Item

每生产者 1000000 条，Item 32 字节。wall 是总完成时间除以总消息数，不是单条端到端延迟；CPU 是所有线程 CPU 时间之和除以总消息数。

| 执行器 | wall ns/item | CPU ns/item |
|---|---:|---:|
| Inline | 3.2 | 12.7 |
| Legacy | 654.9 | 2841.3 |
| Dedicated | 163.0 | 699.3 |
| SharedPool | 253.1 | 1215.6 |
| Moodycamel | 100.0 | 494.8 |

完整大小消息 / 生产者 / 多 strand / 空闲表见 [full/summary.md](legacy-profiling/full/summary.md)。结果仅对该负载成立，不把某一吞吐比例当作普遍提升。

### 端到端延迟

所有引擎使用相同的 2000 条消息与 32 次预热。当前 Dedicated / Pool / Moody 表内为默认 200 μs 有限自旋；Legacy 不支持自旋，Inline 同拓扑。表格单位 μs，每次运行分位数的中位数。

| 执行器 | 单跳连续 p50 / p99 | 四跳、完成后休眠 50 μs p50 / p99 | 八跳连续 p50 / p99 |
|---|---:|---:|---:|
| Inline | 0.073 / 0.077 | 0.271 / 0.307 | 0.159 / 0.166 |
| Legacy | 1064.563 / 1068.103 | 54.321 / 67.481 | 106.010 / 120.492 |
| Dedicated | 1.010 / 1.197 | 2.497 / 4.670 | 6.267 / 7.819 |
| SharedPool | 1.138 / 1.647 | 5.362 / 9.116 | 9.298 / 22.496 |
| Moodycamel | 0.692 / 1.421 | 3.129 / 5.916 | 6.134 / 13.157 |

Legacy 单跳连续交接时，producer 常在 consumer 尚未重新 wait 时通知，复现了原来的约 1ms 白等；但单跳完成后休眠 50 μs 时，Legacy p50 / p99 为 14.731 / 16.530 μs。此时消费者通常已经 wait，通知可以立即唤醒。多跳也给上游节点留下进入等待的时间，因此不能把旧算法描述成“所有消息每跳必加 1ms”。

固定周期 50 μs 的四跳开放输入，Legacy p50 / p99 48.323 / 58.593 μs；Dedicated 2.699 / 7.061、Pool 5.894 / 10.406、Moody 2.860 / 6.869 μs。生产者迟发和 planned-arrival p99 仍单独保存在 JSONL，不将 submit-to-terminal 当成计划到达延迟；完整数据见 [periodic/summary.md](legacy-profiling/periodic/summary.md)。

零自旋、1ms 固定周期和 8 消费者 / 4 核的局限也完整保留，见 [closed/summary.md](legacy-profiling/closed/summary.md)、[oversubscribed/summary.md](legacy-profiling/oversubscribed/summary.md)。2000 个样本的极端尾分位数不足以建立稳定 SLA；不根据 p999 的两三个尾样本宣称保证。

### 分配与空闲

计数窗口 80000 条小 Item：Legacy 和 Dedicated 均为 80000 次分配、3840000 请求字节、80000 次 Item 移动；Pool 同样每消息一个节点，额外有少量调度容器分配；Moody 的块分配较少、Item 移动为三次。**Legacy 的 std::list 也逐消息分配节点**，它不构成无分配基线，不能用该比对直接扣出 malloc 的耗时。旧实现还把节点分配放在队列 mutex 内，和当前 MPSC 的并发分配 / 发布方式不同。

128 个无 idle 工作节点，Legacy 约 987 ms CPU/s，Dedicated 0.188、Pool 0.095、Moody 0.195 ms CPU/s。Legacy 即使 hasIdleWork=false 仍保留周期空闲唤醒，当前实现可以长期休眠。主动 idle 工作场景仍按原工具一并测量，详见原始数据。

## 收尾

代码、结果和记录已一起提交：infra `6333b4a`、basic `02079d0`，本次 doc commit 补记编号。保持 feature 分支，未合并 / 未推送；examples 全量编译仍暂停，transport 环境限制和第二阶段整体验收没有在本轮解除。tm_kit_reference 按既定流程在完成开发验证并推送后再同步。
