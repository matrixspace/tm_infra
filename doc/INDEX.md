# tm_infra 开发任务索引

| 任务 | 时间 | 概述 |
|---|---|---|
| [20261105-realtime-threaded-executor](20261105-realtime-threaded-executor/README.md) | 2026-11-05 起 | （进行中：第二阶段执行器抽象重构）RealTimeApp threaded handler 去掉 1ms 白等，改为零依赖 MPSC 默认执行器，并支持通过 Environment 替换执行模型；2026-11-06 完成一轮执行器开销优化和独立 profiling；完成 [ThreadedExecutor 多跳端到端延迟优化](20261105-realtime-threaded-executor/executor-latency-optimization.md)；2026-11-07 完成 [Legacy 执行器、等待修复及基准复测](20261105-realtime-threaded-executor/legacy-executor-baseline.md)，整体验收仍待完成；2026-11-07 [默认执行器回迁与 Enhanced MPSC 节点复用](20261105-realtime-threaded-executor/mpsc-node-recycling.md)，infra 默认 mutex/list，MPSC 迁至 basic，以 Boost 缓存减少分配 |
