# Threaded 节点独立配置 spinWait 与绑核

**任务开始时间：** 2026-11-11  
**任务结束时间：** 2026-11-11  
**状态：** 完成  
**分支：** `feature/threaded-node-spin-wait`（tm_infra、tm_basic，均从 develop 建立）

## 背景
`ThreadedExecOptions::spinWait` 目前只能取默认值：`ExecHandlerCore::makeOptions()` 只设置 `trackPendingCount`，没有途径为单个节点指定自旋预算。用户要求 Threaded 模式的节点可在运行时按节点配置 spinWait，并追加按节点配置线程绑核。前序工作见 [realtime-threaded-executor](../20261105-realtime-threaded-executor/README.md)。

## 需求确认（用户答复）
- 语义：仅启动前设置（节点创建后、图运行前），不要求运行中动态调整。
- infra 默认 `DedicatedThreadExecutor`（mutex/list）：spinWait 保持忽略。
- basic `RealTimeSharedPoolExecutor`：池级自旋，节点配置忽略，不报错。
- 追加：节点线程绑核配置。

## 方案
- infra 新增 `ThreadedNodeOptions { std::optional<nanoseconds> spinWait; std::vector<int> cpus; }`。
- 执行器契约新增可选成员 `bool configure(ThreadedNodeOptions const &)`，用 SFINAE 探测；未实现的执行器（Inline、SharedPool、用户自定义旧执行器）视为不支持，返回 false，不报错。
- `IRealTimeAppPossiblyThreadedNode` 新增带默认实现的虚函数 `configureThread()`（默认返回 false），`ExecHandlerCore` 转发给执行器；`RealTimeApp::configureThreadedNode(node, opts)` 与已有的 `threadHandle(node)` 同形。
- 节点构造即启动线程，故绑核在调用方线程上用 `pthread_setaffinity_np(threadHandle)` 立即生效；spinWait 存为执行器状态内的原子量，消费者每次等待前 relaxed 读取。
- 放弃：经 `LiftParameters` 传参。需要改所有 lift 路径与 40 处生成的 variant 特化，且 importer/facility 等手写节点无统一入口。

## 代码改动（按文件列出）
- tm_infra `tm_kit/infra/RealTimeThreadedExecutor.hpp`：
  - 新增 `ThreadedNodeOptions`（`std::optional<nanoseconds> spinWait`、`std::vector<int> cpus`，链式 `SpinWait()` / `Cpus()`）。
  - detail 新增 `validCpus()`、`setThreadAffinity()`（Linux `pthread_setaffinity_np`，非 Linux 返回 false）和 `configure()` 分发（SFINAE 探测执行器的 `configure` 成员，缺失时返回 false）。
  - `DedicatedThreadExecutor::configure()`：忽略 spinWait；`cpus` 在线程已运行时立即生效，否则在 `start()` 时生效。`cpus` 为空时保留现有绑核。
  - 契约注释补充可选成员 `configure`。
- tm_infra `tm_kit/infra/RealTimeApp.hpp`：`IRealTimeAppPossiblyThreadedNode::configureThread()`（带默认实现，返回 false）；`ExecHandlerCore` 将其转发给执行器；新增静态函数 `RealTimeApp::configureThreadedNode(node, opts)`，与 `threadHandle(node)` 同形。
- tm_basic `tm_kit/basic/MoodycamelThreadedExecutor.hpp`：spinWait 改为状态内原子量 `spinWaitNs`（构造时取 `ThreadedExecOptions::spinWait`，消费者每次等待前 relaxed 读取，负值按 0 处理）；新增 `configure()`，绑核语义同 Dedicated。
- 测试：
  - tm_infra 新增 `test/threaded_handler_test/ThreadedNodeOptionsTest.cpp`（`threaded_node_options_test`）。
  - tm_basic `MoodycamelQueueTest.cpp` 新增 `nodeOptionsTest`；`ExecutorMechanismTest.cpp` 新增 `nodeOptionsIgnoredTest`。

## 测试与验证（命令、构建类型、结果）
均先 `source build-common/build-env-setting.sh`（GCC 15.2.1），Release 构建，`ninja -j2` 定向编译。tm_infra 头文件只安装到 worktree 私有前缀 `worktrees/realtime-threaded-executor/install`，未动共享 `install/`。
- tm_infra `meson test -C builddir_release --no-rebuild`：4/4 通过（新增 threaded_node_options_test，以及 dedicated_executor_test、threaded_handler_compatibility_test、time_checker_test）。覆盖：图中 threaded `liftPure` 节点绑核后线程亲和性确为目标 CPU；非 threaded 节点返回 false；无效 CPU（-1）返回 false 且原绑核不变；Dedicated 在 start 前设置的绑核于 start 时生效，spinWait 被忽略且数据正常处理；无 `configure` 的自定义执行器返回 false、不报错。`taskset -c 20-21` 下重跑同样通过。
- tm_basic `meson test`：threaded_handler_test、executor_mechanism_test、chain_writer_loop_test、moodycamel_executor_test、moodycamel_queue_test 5/5 通过。
  - moodycamel：start 前、start 后两次绑核均生效。处理一条数据后消费线程 CPU 时间：spinWait=0 约 0.02ms，spinWait=30ms 约 29.9ms，确认节点级 spinWait 生效；无效 CPU 整体拒绝，绑核与 spinWait 均保持不变；空选项保留绑核。
  - SharedPool 与 Inline：`configure` 分发返回 false，不抛异常。
- 未做：TSan、transport / examples 回归（未改动 transport；新增接口只追加，未改已有签名）。

## 兼容性与注意事项
- 纯新增接口，下游源码零改动。`IRealTimeAppPossiblyThreadedNode` 新增的是带默认实现的虚函数，已有实现类（ChainReader、ChainWriter、CommonFlowUtils 中的节点等）不受影响，对它们调用返回 false。
- 语义：在节点创建后、图运行前调用。线程在节点构造时已启动，所以绑核立即作用于该线程；moodycamel 的 spinWait 从消费者下一次等待开始生效。
- 返回值：true 表示执行器接受了设置（Dedicated 的 spinWait 按约定忽略，仍返回 true）；false 表示节点非 threaded、执行器不支持（SharedPool、Inline、旧的自定义执行器），或 CPU 号无效 / 设置亲和性失败。CPU 号无效时整个调用都不生效。
- SharedPool 的 spinWait 与绑核仍由 `SharedThreadPoolConfig` 在池级配置。
- 开发过程中另一个会话曾并发修改同一 worktree，在 `configure` 分发处留下三份重复定义。用户停掉该会话后已合并为一份。

## 相关文档
- [realtime-threaded-executor](../20261105-realtime-threaded-executor/README.md)
- [moodycamel执行器合并](../20261105-realtime-threaded-executor/moodycamel-unification.md)
- tm_kit_reference：`tm_infra/01-app-types.md`（执行器部分）待推送后同步
