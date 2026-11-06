# ThreadedExecutor 端到端延迟优化

**任务开始时间：** 2026-11-06  
**任务结束时间：** 2026-11-06  
**状态：** 本轮完成；第二阶段整体验收仍待 examples / transport 环境验证  
**分支：** `feature/realtime-threaded-executor`（infra commit `3e0cc84`；basic commit `9013c55`）  

## 背景与目标

前一轮减少了 Inline 调用、MPSC 移动和共享池 idle 开销，但线程边界仍包含 OS 休眠与唤醒延迟。本轮以 Inline 为下界，优化 Dedicated / SharedPool / Moodycamel 的单跳及多跳提交到末端回调延迟。用户明确允许有限自旋，长时间空闲后仍应休眠；examples 全量编译继续暂停。

## 方案与进度

开始前已阅读开发记录、git 历史、执行器源码和既有 executor_bench；工作区 clean。原延迟测试仅覆盖单跳闭环，因此补充多跳、固定间隔开放输入的端到端测量，并报告排队与生产者迟发。所有比较保持同一业务和拓扑。

拟在处理过数据后设置有限自旋窗口；窗口内发现数据直接继续消费，超时回到原阻塞协议。周期 idle 超时本身不重新触发自旋。保留 FIFO、单节点不重入、stop / wakeIdle、启动和析构协议；不将 threaded 回调搬到生产者线程。参数可配置，默认值以测量决定。

## 代码改动

- infra `RealTimeThreadedExecutor.hpp`：追加 spinWait，有限时间轮询、pause / yield、低频率让出 CPU；Dedicated 在真实数据后等待下一条，超时回到原 eventcount 协议，idle deadline 覆盖自旋时间。
- infra `RealTimeSharedPoolExecutor.hpp`：池级 spinWait、单 spinner、原子 ready 提示、锁内登记 / 退出 / 入队的握手，以及积压时唤醒其他 worker。通过每 slice 的局部返回值区分真实数据与 idle。
- infra `ExecutorMechanismTest.cpp`：有限自旋行为回归；已有并发、FIFO、stop、析构、idle 注册测试保持运行。
- basic `MoodycamelThreadedExecutor.hpp`：仅在真实数据后尝试有限非阻塞消费；wake token / idle timeout 不续期，保留原 bulk、FIFO 范围、stop 和 native semaphore 后备路径。
- basic `ExecutorLatencyBench.cpp` / `run_latency.py` / Meson / README：独立手动目标、线性多跳、闭环与开放输入、显式消费核列表、正确性校验、分位数、CPU 和空闲对照。生产代码未增加第三方依赖。

## 测试与验证

最终 production（包含协作 yield）release/debug 回归全部通过，机制 / handler / moody 的 TSan、机制的 ASan/UBSan 再次通过。未恢复 examples 全量编译。主要命令：

```sh
source build-common/build-env-setting.sh
ninja -j2 -C worktrees/realtime-threaded-executor/tm_infra/builddir_release \
  test/threaded_handler_test/executor_mechanism_test \
  test/threaded_handler_test/threaded_handler_test install
meson test -C worktrees/realtime-threaded-executor/tm_infra/builddir_release --no-rebuild
ninja -j2 -C worktrees/realtime-threaded-executor/tm_basic/builddir_release \
  test/executor_bench/executor_latency_bench test/executor_bench/executor_bench \
  test/moodycamel_executor_test/moodycamel_executor_test
meson test -C worktrees/realtime-threaded-executor/tm_basic/builddir_release --no-rebuild
```

Debug 同样定向编译对应测试，infra 3/3、basic chain_writer_loop / moodycamel_executor 2/2。TSan 使用 GCC 15 `-std=c++17 -O1 -g -fsanitize=thread -pthread`，源路径 include 指向本 worktree；handler / moody 测试链接本 worktree infra 静态归档，moody 增加本机 vcpkg include。机制 ASan/UBSan 使用 `-fsanitize=address,undefined`，`ASAN_OPTIONS=detect_leaks=0`。

## 兼容性与注意事项

现有 `push(Item&&)`、组件选择、线程 / FIFO / stop / idle 契约均保持；仅追加可选配置字段，默认采用用户授权的有限主动等待。`SharedThreadPoolConfig` 的新字段追加在末尾，既有 C++17 聚合初始化可继续使用。`DedicatedThreadExecutorConfig::spinBeforeSleep` 次数接口保留。


有限自旋会占用更多 CPU，无法消除跨核缓存传输和排队，也不能承诺达到同步 Inline 延迟。默认 CPU 和 idle 行为需要实测。Context7 当前仍未暴露，本轮仅复用已有经一手源码核对的 moodycamel API，不新增第三方接口依赖。

## 相关文档

- [主任务](README.md)
- [前轮开销优化](executor-overhead-optimization.md)
- [第二阶段](phase2-unified-handler-executor.md)

### 第一批实现

- 增加共享的有限自旋 helper，使用架构 pause / yield 提示并摊薄时钟读取；预算为零时完全跳过。
- `ThreadedExecOptions::spinWait`（Dedicated / Moodycamel）与 `SharedThreadPoolConfig::spinWait`（pool）提供时间预算。初始候选值 50 μs，零恢复阻塞策略，最终默认值待数据确认。原 Dedicated Config 的次数自旋接口继续保留。
- 仅真实数据处理后开启窗口；无数据启动、周期 idle 和 wake token 不反复触发自旋。Dedicated / Moody 的 idle 截止时间覆盖自旋时间，避免把原 1ms 周期再延长一个自旋窗口。
- 共享池通过 slice 的返回值报告是否处理了数据，不在 strand 上保存被下一 worker 覆写的报告字段；避免重投递后并发读写。
- 新增独立 `executor_latency_bench`，Inline / Dedicated / Pool / Moody，1–16 跳，闭环 / 固定到达周期，末端延迟、计划到达延迟、生产者迟发和 CPU。基线已在改动生产头文件前用同一测试源码、GCC 15.2.1 编译到 `/tmp/executor-latency-baseline`。

### 首轮测量与方案调整

首次完整矩阵每案例 5 次，1 / 4 跳、闭环无间隔 / 50 μs 休眠间隔、原基线 / 候选零自旋 / 50 / 200 μs。初步 Dedicated 4 跳连续输入 p50 约 49→4 μs；200 μs 覆盖了间隔 50 μs 输入加上生产者休眠调度的常见空档，而 50 μs 经常提前休眠。选择默认 200 μs，零预算仍可恢复原阻塞策略。空闲节点不应重复消耗该预算，最终需独立 idle CPU 测量。

共享池首版每次轮询都 try_lock 全局 ready mutex，4 跳收益不足且 CPU 上升，故放弃该轮询方案。最终候选使用原子 hasReady 提示，空队列不碰 mutex；最多一名 worker 登记为 spinner。生产者在相同 mutex 下见到 spinner 时无需唤醒睡眠 worker，spinner 退出时在锁内重新检查队列，避免丢通知；取走任务后若仍有积压会唤醒其他 worker，保持阻塞回调的并行容量。slice 返回的局部 bool 区分数据和 idle，不跨 worker 共享该报告字段。

### 验证检查点

Release：infra mechanism / threaded_handler / time_checker 3/3，通过；basic chain_writer_loop / moodycamel_executor / tsc_clock 3/3，通过。Debug：infra 3/3，basic 定向 chain_writer_loop / moodycamel_executor 2/2，通过。

最新 production 版本的机制测试、完整 threaded_handler、完整 moodycamel_executor 分别通过 TSan；机制测试通过 ASan/UBSan（`ASAN_OPTIONS=detect_leaks=0`，同前轮环境 ptrace 限制，未宣称 LeakSanitizer 检测）。新增测试验证零预算不轮询、已就绪立即退出、预算到期、并发发布使自旋退出；既有 FIFO、多生产者、动态 idle、stop、销毁和 shared pool 注册竞争测试均继续通过。

所有编译先 `source build-common/build-env-setting.sh`，工作区自己的 release/debug 目录，`ninja -j2` 仅请求相关目标，infra 安装到该 worktree 私有 install。最终 profiling 不与编译并行。

## 最终接口与配置

Dedicated / Moodycamel 使用追加的 `ThreadedExecOptions::spinWait`，SharedPool 使用追加的 `SharedThreadPoolConfig::spinWait`，默认均为 200 μs。零预算可恢复原等待策略；Moodycamel 库本身原有的有限 semaphore 自旋仍保留。Inline 忽略该选项。

```cpp
infra::ThreadedExecOptions opts;
opts.spinWait = std::chrono::microseconds(200); // 0 = 原等待策略
infra::DedicatedThreadExecutor<MyItem> dedicated(opts);
basic::MoodycamelThreadedExecutor<MyItem> moody(opts);

infra::SharedThreadPoolConfig pool;
pool.spinWait = std::chrono::microseconds(200);
// 保持原规则：必须在创建第一个池节点前配置。
infra::SharedThreadPool::configure(pool);
```

框架构建的默认执行器直接采用新默认值。需按 Environment 定制 Dedicated / Moody 策略时，继续使用现有 `RealTimeThreadedExecutorComponent<MyExec>` 扩展点，在 MyExec 的构造函数中复制传入 opts、覆盖 spinWait 后交给原执行器。没有新增框架级必填 API，原 Config 次数自旋接口也继续兼容。

## 剩余边界

200 μs 是主动轮询的时间预算，不是硬实时上界：线程可能被抢占，分配器、mutex 或一次回调仍可能阻塞。跨核缓存传输、节点分配、串行化和排队仍使 threaded 延迟高于 Inline。输入间隔超过窗口时会恢复休眠，下一条数据仍包含 OS 唤醒延迟；接近窗口内连续输入的低延迟需要消耗更多 CPU。线程数超过可用物理核时应减少窗口或禁用；不修改系统调度器、实时优先级或电源设置。

### CPU 超额订阅检查与调整

8 个 Dedicated / Moody 消费线程固定到 4 个消费 CPU（每核两个线程）时，纯 pause 的 200 μs 窗口使当前消费者占住下一个消费者需要的 CPU，反而把 8 跳延迟推到约 0.3–0.4ms。该方案不能作为最终实现。临时覆盖头文件实验：每 256 次轮询主动 `std::this_thread::yield()`，不改变预算截止时间、队列及唤醒协议；8 跳 Dedicated 连续 p50 约 415→39 μs，Moody 约 434→40 μs，而 4 跳 Dedicated 仍约 4 μs。最终采用低频率协作让出 CPU，再重新测量最终二进制。yield 只是调度提示，仍不承诺超额订阅下与独占 CPU 相同的延迟。

### CPU 位置与测量边界

测试机为双路 Intel Xeon Gold 5117（每路 14 核、SMT2），编号 20–27 横跨两个 NUMA 节点。最后的主要矩阵选择同一 NUMA 节点：producer 20，消费者 24,26,28,30,32,34,36,38；sysfs package/core/sibling 核对为不同物理核，列表中没有彼此的 SMT sibling。新增测试工具 `--consumer-cpus` 显式列表，保证 8 跳有 8 个消费物理核；另保留 24,25,26,27 承载 8 消费者的超额订阅矩阵，避免把跨 NUMA / 核数不足的结果混在一起。

闭环单生产者只用于隔离交接与唤醒。固定周期开放输入保留计划到达时间，独立报告生产者迟发和排队后的末端延迟，不以闭环数据承诺满负载尾延迟。所有延迟结果为三 uint64 Item，无业务分配；Inline 同拓扑、同计时边界，观察成本不是零，尤其 0.1 μs 量级的 Inline 控制项会受编译内联和代码布局影响，不能将其细微差值当作实现回退或精确减法基线。

## 最终结果（GCC 15.2.1 Release）

基线 infra `84b11b5`、basic `ceaa617`。同一最终测试源码使用该版本的三个原始执行器头文件，在 `/tmp/executor-latency-baseline-headers` 隔离构建；不修改原分支，也不复用 worktree 的 Meson 缓存。基线通过 `-DEXECUTOR_LATENCY_BASELINE=1` 隐去尚不存在的新配置字段，生产测量循环相同。最终原始数据和实际二进制 SHA256 见 [latency-profiling](latency-profiling/closed/results.jsonl)，复现命令见 [reproduce.sh](latency-profiling/reproduce.sh)，工具说明见 [README](../../../tm_basic/test/executor_bench/README.md)。

主要矩阵 5 次交替进程重复（连续每次 20000 条、休眠间隔案例每次 5000 条）；8 跳、固定周期和超额订阅各 3 次（每案例 2000–10000 条）。表内数值为每次运行分位数的中位数，不是混合样本的分位数。总计 445 行最终结果（220 主要矩阵、60 空闲、66 八跳、66 固定周期、33 超额订阅），所有阶段精确计数 / FIFO / 结果检查通过，无测量失败。

### 四跳闭环，完成后休眠 50 μs

真实间隔还包含 sleep 调度和上一条完成时间，不能将此称为精确 20k msg/s。用于隔离冷 / 暖唤醒，不产生开放式排队。

| 执行器 | p50 μs 基线→新默认 | p99 μs 基线→新默认 |
|---|---:|---:|
| Dedicated | 53.259→2.326 | 64.626→3.190 |
| SharedPool | 22.613→5.581 | 27.066→6.244 |
| Moodycamel | 45.513→3.136 | 52.906→5.426 |
| Inline 控制 | 0.272→0.260 | 0.296→0.281 |

新默认 process CPU/message 为 Dedicated 451 μs、Pool 133 μs、Moody 453 μs，原分别约 121 / 60 / 250 μs；该值是所有线程 CPU 时间之和，包含生产者等待，不是 wall latency。延迟改善以 CPU 换取，不能承诺同时省 CPU。

### 连续单条交接与八跳

单跳连续 p50：Dedicated 3.008→0.681 μs；Pool 5.771→1.021 μs；Moody 0.562→0.520 μs；Inline 新版本约 0.122 μs。

四跳连续 p50 / p99（μs）：Dedicated 35.331 / 43.492→1.986 / 2.621；Moody 1.989 / 4.194→1.890 / 4.255，原实现已有 semaphore 自旋，不宣称其连续输入有显著普遍收益。Pool 3.447 / 10.813→4.398 / 7.234，p50 回退、p99 改善，说明共享队列调度仍有取舍；偏重该负载的 p50 时可将 pool.spinWait 设为 0。

独立八个消费核的八跳连续 p50 / p99：Dedicated 104.520 / 114.415→3.366 / 3.984 μs；Pool 9.566 / 14.753→7.473 / 10.603 μs；Moody 36.470 / 42.231→4.074 / 8.542 μs。完整 CPU / 延迟矩阵见 [eight/summary.md](latency-profiling/eight/summary.md)。

### 开放输入、长间隔与超额订阅

固定周期 50 μs、四跳，实际提交到终点 p99：Dedicated 57.113→3.723 μs，Pool 22.130→6.260 μs，Moody 24.304→5.221 μs。计入计划到达至生产者实际发出的延迟后，arrival p99 为 Dedicated 115.988→68.577 μs、Pool 85.056→66.633 μs、Moody 85.773→63.435 μs；生产者 lateness p99 约 59–65 μs。不能把后者隐藏后宣称按计划到达也只有几微秒。

固定周期 1ms 超过主动等待窗口，三种执行器 p99 基本不变，CPU/message 显著增加（例如 Dedicated 约 84→900 μs）。这种长期稀疏负载应减小窗口或设为 0；有限自旋无法消除长休眠后的调度延迟。详见 [periodic/summary.md](latency-profiling/periodic/summary.md)。

8 消费者 / 4 核的连续输入，最终 yield 版 Dedicated p50 / p99 99.049 / 116.976→38.641 / 43.315 μs，Pool 13.680 / 22.787→10.495 / 18.428 μs。Moody p50 44.268→40.437 μs，但 p99 53.850→82.548 μs，仍有回退；该负载不能承诺所有指标改善，应减少专用线程、提供独立 CPU、改用池或关闭自旋。原纯 pause 的 0.4ms 回退方案已经放弃，原始中间汇总保存在 [pre-yield-oversubscribed.md](latency-profiling/pre-yield-oversubscribed.md)，最终结果见 [oversubscribed/summary.md](latency-profiling/oversubscribed/summary.md)。

### 空闲 CPU

128 节点、无 idle 回调：Dedicated 基线 / 新版 0.190 / 0.205 ms CPU/s，Pool 0.064 / 0.095，Moody 0.200 / 0.187，说明没有退化为常驻忙等。128 个有 idle 回调的节点仍按周期执行：Dedicated 1025 / 1017、Pool 705 / 785、Moody 2391 / 2429 ms CPU/s；Pool 的 ready 提示和报告也有额外 CPU 成本，不能描述为零开销。详见 [idle-summary.md](latency-profiling/closed/idle-summary.md)。

## 收尾

本轮优化和测试记录已随源码提交：infra `3e0cc84`，basic `9013c55`；本次文档补记随后的 doc commit。两仓库工作区在收尾时 clean。保留 feature 分支，不合并 / 不推送；主任务第二阶段仍待原有 examples 暂停和 transport socket 环境限制解除后完成整体验收，tm_kit_reference 按既定流程在推送后再同步。
