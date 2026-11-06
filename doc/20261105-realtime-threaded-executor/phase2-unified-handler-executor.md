# 第二阶段：执行器抽象重构（去掉 ThreadedExecMode，统一同步与 threaded handler）

**开始时间：** 2026-11-06  
**结束时间：**  
**状态：** 进行中  
**分支：** `feature/realtime-threaded-executor`（tm_infra、tm_basic、tm_transport）  
**主记录：** [README.md](README.md)

## 背景
第一阶段（tm_infra `6836234`/`d62a0c2`、tm_basic `437bfcc`、tm_transport `117fdb6`，均未推送）引入了执行器契约 `Exec<Item, ThreadedExecMode>`。遗留两个问题：
1. `ThreadedExecMode{Blocking,BusyLoop}` 把"执行方式"写死为两种，执行器只能按模板参数分支：共享池要回退到专用线程，moodycamel 执行器内部也按 Mode 分叉。执行方式应由每个执行器自己决定。
2. 每个基于 handler 的框架节点仍有 `Threaded=true/false` 两套特化，同步版和 threaded 版的处理逻辑重复；生成 piece 中 ActionCore/MultiActionCore 各有 19×2 份。

## 目标
- 执行器契约只剩 `Exec<Item>`。
- 同步执行也是一个执行器（`InlineExecutor`），所有基于 handler 的框架节点只保留一份实现，通过执行器策略区分同步 / threaded。

## 用户确定的决策
- BusyLoop 不再由 infra 提供。`ChainWriter`（tm_basic `simple_shared_chain/ChainWriter.hpp`）不再依赖 infra 的 `ThreadedHandler`/`BusyLoopThreadedHandler`，改用本地的两个 handler：照搬本分支基线版本的实现，只修掉固定等 1ms 的问题。
- 统一覆盖全部基于 handler 的框架节点。
- 生成 piece 的脚本提交进仓库。
- 删除 `BusyLoopThreadedHandler` 和 `ThreadedExecMode` 属于不兼容改动，已经用户确认。

## 设计

### 1. 执行器契约（`RealTimeThreadedExecutor.hpp`）
- 删除 `ThreadedExecMode`。契约变为 `template <class Item> class Exec`，接口仍是 `start/push/stop/wakeIdle/threadHandle`，Host 回调不变。
- 每个执行器声明两个能力常量，节点据此选型：
  - `static constexpr bool kOwnContext`：回调是否在调用方之外的上下文执行。决定 `isThreaded()`，以及是否调用 `waitForStart` 和 idle work。
  - `static constexpr bool kSerialCallbacks`：回调是否互斥。决定 `TimeChecker<!kSerialCallbacks, T>`，以及 FireOnceOnly 的 `done_` 是否用 atomic。
- `ThreadedExecOptions` 只保留通用项 `trackPendingCount`。`idlePeriod`、`spinBeforeSleep` 属于执行器自己的配置，改为 `DedicatedThreadExecutor` 的配置模板参数（带默认值）；`noYield` 随 BusyLoop 一起移出 infra。
- `RealTimeThreadedExecutorComponent<template <class> class Exec>`；Environment 成员改为 `template <class Item> using RealTimeThreadedExecutor`；`RealTimeThreadedExecutorFor<StateT, Item>` 去掉 Mode。
- 新增 `InlineExecutor<Item>`：`kOwnContext=false`、`kSerialCallbacks=false`。`push` 在调用线程直接调 `h->executorHandle`；`start` 不调 `waitForStart`；从不调 idle work；`stop` 之后 push 返回 false。语义与现有同步节点一致（多生产者可并发进入，所以配多线程 TimeChecker）。
- `RealTimeSharedPoolExecutor.hpp`：`RealTimeSharedPoolExecutor<Item>` 直接是 strand 执行器，删除 BusyLoop 回退。

### 2. 统一的 handler 核心（`RealTimeApp.hpp`）
- `ThreadedHandlerCore<T,W,Mode>` 改为 `ExecHandlerCore<T,W,Policy>`。`Policy` 提供 `template <class Item> using Executor`，有两种：
  - `ThreadedPolicy`：用 `RealTimeThreadedExecutorFor<StateT, Item>`；
  - `InlinePolicy`：用 `InlineExecutor<Item>`。
- 核心持有 `TimeChecker<!kSerialCallbacks, T>`、Host 和 Exec，对外提供 `putData/stop/wakeIdle/timeCheckGood/timeChecker/startThread/threadHandle`。
- 公开 `ExecHandler<T,W,Policy>`，其 variant 偏特化放在 piece 中。`ThreadedHandler<T,W>` 改为别名 `ExecHandler<T,W,ThreadedPolicy>`；作为基类和在成员初始化列表中的写法都不变，tm_basic/tm_transport 现有继承者不改源码。
- 删除 `BusyLoopThreadedHandlerBase`/`BusyLoopThreadedHandler`（不兼容改动；下游只有 ChainWriter 和 moodycamel 测试在用）。

### 3. 框架节点只留一份实现
以下节点从"`Threaded` 偏特化 × 2"合并为一个类模板，按 `Threaded ? ThreadedPolicy : InlinePolicy` 继承 `ExecHandler`：
- ActionCore、MultiActionCore（含 2–20 路 variant piece）；
- ContinuationActionCore、OnOrderFacilityCore、OnOrderFacilityCoreWithStart、SimpleExporter。

逐项处理：
- `isThreaded()` 返回 `Exec::kOwnContext`。
- `setIdleWorker`/`setStartWaiter`/`idleWork`/`hasIdleWork` 用 `if constexpr (kOwnContext)` 包住；同步时仍是空操作。
- FireOnceOnly：`done_` 的类型按 `kSerialCallbacks` 选；完成后统一调 `this->stop()`。
- 同步路径是 `handle → putData → InlineExecutor::push → actuallyHandle`，全部是非虚的内联调用，不应增加开销，用基准确认。
- 外部模板参数（`bool Threaded`、`PureActionCore` 等别名、`liftParam.suggestThreaded` 分支）不变。

不在本次范围：自带线程的 importer（`triggerImporter<HasOwnThread>`、`BunchedImporter`、`SimpleImporter`）不是 handler，但同样有 1ms 白等，记为后续项。

### 4. 生成脚本
- 新增 `tm_infra/tools/piece_gen/gen_realtime_pieces.py`（先看 tm_kit_reference `tm_infra/09-generated-pieces.md` 有无目录约定），生成三个文件，文件名不变：
  - `RealTimeApp_ActionCore_Piece.hpp`
  - `RealTimeApp_MultiActionCore_Piece.hpp`
  - `RealTimeApp_ThreadedHandler_Piece.hpp`（内容变为 `ExecHandler` 的 variant 偏特化）
- 覆盖 2–20 路。生成结果先与手写的 2 路模板逐行对照，再全量生成；用法写在脚本文件头。

### 5. ChainWriter 自带 handler（tm_basic）
按用户要求，直接使用本分支基线版本（`6836234^` 的 `RealTimeApp.hpp` 中 `ThreadedHandlerBase` / `BusyLoopThreadedHandlerBase`，即 mutex + `std::list` + 每节点一个 detached 线程）的实现，只修 1ms 白等，不换成 MPSC 或执行器。
- 在 `ChainWriter.hpp` 新增 `chain_writer_detail::ThreadedHandler<Item, W>` 和 `chain_writer_detail::BusyLoopThreadedHandler<Item, W>`，从基线照搬，去掉 ChainWriter 用不到的部分：
  - 不要 TimeChecker：原 InnerHandler 只把数据转给 `parent_->actuallyHandle`，从未调 `timeCheckGood`，行为不变。
  - 保留 `IRealTimeAppPossiblyThreadedNode::threadHandle()` 和 `OutputRealTimeThreadBufferSizeComponent` 日志。
- 只修阻塞版的 1ms 白等，busy loop 版原样照搬：
  - 等待改为带 predicate 的 `cond_.wait_for(lock, 1ms, [&]{ return !running_ || !incoming_.empty(); })`。有数据立即返回，处理期间到达的 notify 不再丢失。
  - 只在超时且 `incoming_` 为空时调 `idleWork`，1ms idle 节奏不变。
  - 处理完一批回到循环顶部；`incoming_` 已非空时 predicate 立即成立，不再等待。
  - `stopThread` 在锁内修改 `running_` 后 notify，避免 predicate 等待漏掉停止信号。
- InnerHandler1/2 改为继承这两个本地类，`start()`/`handle()`/析构结构不变。
- 生命周期按基线保持（detach、析构 join 不生效），不在本次修复，仍是已记录的后续项。
- infra 的 `MPSCQueue` 留在 detail 中，不公开。

### 6. 下游跟进
- `MoodycamelThreadedExecutor.hpp`：去掉 Mode，只保留阻塞实现，加上两个能力常量，删除 BusyLoop 分支。
- `MoodycamelExecutorTest.cpp`：删除 BusyLoop 用例，更新类型断言。
- tm_transport：只做回归，预计不改源码。

## 被放弃的方案及原因
- infra 保留 BusyLoop 作为一种执行器：用户决定 BusyLoop 不进 infra。
- ChainWriter 改用 MPSC 或执行器驱动：用户要求使用基线实现，只修 1ms 白等。
- 手工修改 piece：生成器不在仓库中，结构重写后手改不可维护，改为提交生成脚本。

## 验证计划
构建前先 source `build-common/build-env-setting.sh`（gcc-toolset-15），安装到 worktree 私有前缀。
1. `ThreadedHandlerTest`：去掉 Mode；补 InlineExecutor 用例，覆盖多生产者并发进入、多线程 TimeChecker 丢弃乱序、FireOnceOnly、stop 后 push 被拒；dedicated 与 pool 两种执行器都跑。
2. 新增 tm_basic `test/chain_writer_loop_test`：用内存 chain，覆盖阻塞 / busyLoop / noYield 三种策略。验证全部写入成功且顺序不变；阻塞模式空闲时约每 1ms 调一次 idleWork；连续写入的往返延迟明显低于 1ms（证明白等已去掉）。tm_basic 现有 shared chain 相关测试也要跑。
3. TSan：上述测试加 moodycamel 测试全部干净。
4. 回归：tm_infra、tm_basic debug/release；tm_transport debug，跑 `threaded_handler_transport_test`；tm_examples 全量构建 291/291、0 错误。
5. 性能：`simple_graph_bench` release，`taskset -c 20-27`，5 次取中位数，与第一阶段的表对比。plain 变体（全部走 InlineExecutor）回退不得超出噪声（±20%）。

## 进展记录
- 2026-11-06：方案确定，建档。

## 代码改动
- `RealTimeThreadedExecutor.hpp`：`Exec<Item>` 契约、两个能力常量、`InlineExecutor`、默认配置类型；Environment 选择器去掉 Mode。
- `RealTimeSharedPoolExecutor.hpp`：直接选择 strand，不再回退 BusyLoop。
- `RealTimeApp.hpp`：统一 `ExecHandlerCore` / `ExecHandler` 与 `HandlerPolicy`；保留 `ThreadedHandler` 别名；Action / MultiAction / Continuation / 两种 facility / SimpleExporter 均只保留一份实现。
- 三个 `RealTimeApp_*_Piece.hpp`：统一 variant 分派与执行策略，元数 2–20；生成器及两份模板在 `tools/piece_gen/`。
- `ThreadedHandlerTest.cpp`：移除 BusyLoop 用例；新增 InlineExecutor 并发与停止、同步图、环境选择 InlineExecutor 的图回归。
- tm_basic `MoodycamelThreadedExecutor.hpp` 及测试：阻塞实现与新契约；moodycamel 原有跨生产者排序差异仍保留。
- tm_basic `ChainWriter.hpp`：从基线提取本地 mutex/list 阻塞与 busy loop handler；只修改阻塞等待 predicate / stop 通知；三个 polling 策略的调用结构不变。
- tm_basic `test/chain_writer_loop_test`：真实内存链 FIFO 与本地循环 idle / 漏通知窗口延迟回归，加入 Meson。

### 使用与迁移
- 自定义执行器从 `template<class Item, ThreadedExecMode Mode>` 改为 `template<class Item>`，声明 `kOwnContext` 和 `kSerialCallbacks`；Environment 的 alias 同样去掉 Mode。
- 同步节点仍由 `SuggestThreaded(false)` 选择，不依赖 Environment 的 threaded 执行器选择。选择 `InlineExecutor` 的 Environment 则让 `SuggestThreaded(true)` 节点也同步运行，`isThreaded()` 根据能力返回 false。
- 默认执行器配置通过 `DedicatedThreadExecutor<Item, MyConfig>` 提供；`MyConfig` 定义 `idlePeriod` / `spinBeforeSleep`，可用单参数 alias 接入组件。
- `ThreadedExecOptions` 只保留 `trackPendingCount`；`BusyLoopThreadedHandler` 已移除。ChainWriter 的 `ChainPollingPolicy` API 保持不变。
- 生成：`python3 tools/piece_gen/gen_realtime_pieces.py`；校验：追加 `--check`。需要已有 `clang-format`，按仓库配置格式化输出。


## 测试与验证结果
构建均先 `source build-common/build-env-setting.sh`，使用 GCC 15.2.1，worktree 私有构建目录及安装前缀 `worktrees/realtime-threaded-executor/install`，Ninja `-j2`。
- infra debug：`ninja -j2 -C tm_infra/builddir_debug`；`threaded_handler_test`（同步、环境覆盖 Inline、dedicated、pool）ALL PASSED；`time_checker_test` ALL PASSED。
- basic debug：`ninja -j2 -C tm_basic/builddir_debug`；`moodycamel_executor_test` ALL PASSED，moodycamel 单条延迟 p50 12.3µs。
- `chain_writer_loop_test`：三种策略各写入 5000 条，全部 FIFO；100ms 空闲窗口保持约 1ms 节奏；200 次处理期间入队场景 p50 3.2µs（门限 300µs）。ALL PASSED。
- infra TSan：`g++ -std=c++17 -O1 -g -fsanitize=thread -pthread -I tm_infra ThreadedHandlerTest.cpp <worktree debug infra archive> -o <test>`，运行 ALL PASSED，0 报告。
- `python3 tools/piece_gen/gen_realtime_pieces.py --check`：通过。


- 2026-11-06：开始实施；已核对 `978b5df` 计划、`6836234` 实现与三个仓库工作区。沿用功能分支，先重构执行器和统一核心，再生成 variant 特化、迁移下游及验证。

- 2026-11-06：执行器统一、节点合并、三类 piece 生成器及下游迁移已实施。infra debug 编译通过；同步图、dedicated/pool 图与并发测试 ALL PASSED，单条 dedicated p50 16.4µs、pool 17.9µs。
- 新增 ChainWriter 内存链测试覆盖三种 polling 策略；本地阻塞循环用“上一条回调尚未结束时入队”明确触发旧实现漏通知的窗口。生成器使用仓库 `.clang-format` 格式化输出，并提供 `--check` 校验。

- 测试发现既有 `ChainWriter` 的 `SingleWritingMode=true` 分支以 `Key<Response>` 构造 `(status,response)`，无法实例化；该问题在基线已经存在，与本次循环迁移无关。内存链回归使用常规重试模式，未扩展修改此分支。

- 2026-11-06：infra / basic debug 与 release 均构建成功；infra release 两项测试通过。三组 TSan（infra、ChainWriter、moodycamel）均 ALL PASSED、0 报告。moodycamel 第三方头文件仍有 `-Wtsan` fence 警告（第一阶段已有），TSan 无法完整建模其内部 fence。
- transport debug 构建 23/23 通过；JSON REST 端到端测试启动失败，原因为当前沙箱禁止 socket：最小 `socket.socket()` 返回 `PermissionError: [Errno 1] Operation not permitted`，不是编译错误或端口占用。该项需在允许本地 socket 的环境重跑。
- examples 全量目标回归较慢，确认机器剩余可用内存约 40GiB、56 CPU 后，以 `-j4` 继续增量编译（仍仅使用自己的 builddir）。

### release 性能复测
为避免把不同机器负载混为实现变化，在临时 detached worktree 从第一阶段 `6836234` 重编译同一个基准，再交替运行第一阶段与当前版本各 5 次；`taskset -c 20-27`，每变体 200000 push，取中位数。比较结束已删除临时 worktree。

| 变体 | 第一阶段 wall ns/push | 第二阶段 wall ns/push | 第一阶段 CPU ns/push | 第二阶段 CPU ns/push |
|---|---:|---:|---:|---:|
| plain | 2469 | 2623 | 4443 | 4759 |
| src threaded | 2313 | 1469 | 7470 | 4955 |
| action+facility threaded | 2189 | 2204 | 7614 | 9287 |
| src+action+facility | 2364 | 1964 | 7865 | 6794 |

plain wall +6.2%、CPU +7.1%，符合 ±20% 门限。其他变体受线程调度及共享机器负载影响大，不据此声称稳定加速。第一阶段主记录的历史测量仍保留；本轮同机基线与历史绝对值不同，不能直接用历史数字判断回退。全部运行 `bad=0`，输出计数一致。
