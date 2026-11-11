# tm_infra 开发任务索引

| 任务 | 时间 | 概述 |
|---|---|---|
| [20261105-realtime-threaded-executor](20261105-realtime-threaded-executor/README.md) | 2026-11-05 起 | （进行中：第二阶段执行器抽象重构）RealTimeApp threaded handler 去掉 1ms 白等，改为零依赖 MPSC 默认执行器，并支持通过 Environment 替换执行模型；2026-11-06 完成一轮执行器开销优化和独立 profiling；完成 [ThreadedExecutor 多跳端到端延迟优化](20261105-realtime-threaded-executor/executor-latency-optimization.md)；2026-11-07 完成 [Legacy 执行器、等待修复及基准复测](20261105-realtime-threaded-executor/legacy-executor-baseline.md)，整体验收仍待完成；2026-11-07 [默认执行器回迁与 Enhanced MPSC 节点复用](20261105-realtime-threaded-executor/mpsc-node-recycling.md)，infra 默认 mutex/list，MPSC 迁至 basic，以 Boost 缓存减少分配；按用户要求删除 Legacy 兼容入口；继续评估缓存容量并优化回收节点批量读取；2026-11-08 完成延迟关键路径、Inline/SMT、自旋预算及90次双基线对照 |

- 2026-11-09：[执行器实现分层](20261105-realtime-threaded-executor/executor-layering.md)：SharedPool及其队列迁到basic，infra仅保留Inline/Dedicated默认实现。

2026-11-09：[默认关闭时间检查](20261105-realtime-threaded-executor/default-time-check.md)；显式true保留时间检查，允许后续使用moodycamel。

- 2026-11-09：[Enhanced取消全局FIFO后的优化](20261105-realtime-threaded-executor/unordered-enhanced.md)：采用moodycamel分块队列与分片发布计数，以单生产者部分延迟代价换取多生产者吞吐。

- 2026-11-09：[moodycamel执行器合并](20261105-realtime-threaded-executor/moodycamel-unification.md)：只保留MoodycamelThreadedExecutor，以可用性探测控制相关目标编译。

- 2026-11-09：[ThreadedHandler 继承兼容性修复](20261105-realtime-threaded-executor/threaded-handler-compatibility.md)：恢复公开类包装，兼容旧的基类初始化与限定成员访问。

- 2026-11-09：[合并 develop 与交付状态](20261105-realtime-threaded-executor/develop-integration.md)：按用户要求整合三个库；examples 无源码差异，远程访问受环境限制。

- 2026-11-11：[Threaded 节点独立配置 spinWait 与绑核](20261111-threaded-node-spin-wait-affinity/README.md)：新增 `RealTimeApp::configureThreadedNode`，在图启动前为单个 threaded 节点设置 spinWait（moodycamel 生效）和线程绑核。

- 2026-11-11：[节点线程自动绑核（CpuAutoBind）](20261111-thread-cpu-autobind/README.md)：infra 增加分配器扩展点，basic 增加基于 hwloc 的可选实现；考虑 taskset/numactl、SMT、NUMA 首核与避让列表，失败时抛异常。
