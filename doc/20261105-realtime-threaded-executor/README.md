# RealTimeApp threaded handler 框架开销优化与可替换执行模型

**任务开始时间：** 2026-11-05  
**任务结束时间：**（第一阶段 2026-11-06 完成；第二阶段进行中）  
**状态：** 进行中（第二阶段：执行器抽象重构）  
**分支：** `feature/realtime-threaded-executor`（tm_infra、tm_basic、tm_transport；commit：tm_infra `6836234`、tm_basic `437bfcc`、tm_transport `117fdb6`）

## 背景
- `ThreadedHandlerBase::runThread`（`tm_kit/infra/RealTimeApp.hpp:243`）使用不带 predicate 的 `cond_.wait_for(1ms)`：处理完一批后即使 `incoming_` 非空也要白等最多 1ms，消费端处理期间的 `notify_one` 会丢失。
- 其他开销：每次 `putData` 都 `notify_one`（futex syscall）；`std::list` 逐元素堆分配；空闲时每轮锁 `idleWorkerMutex_` 并调用 `std::function`。

## 目标
1. 默认执行器（零依赖自研 MPSC + eventcount 等待）消除 1ms 白等与热路径 syscall，保持现有语义。
2. 通过 StateT（Environment）提供可替换整个执行模型的扩展点（用户决定做到"替换执行模型"层级）。
3. 下游（tm_basic / tm_transport / tm_examples）源码零改动。

## 方案（含被放弃的方案和原因）
- 采用：执行器契约 `Exec<Item, Mode>`（`Mode ∈ {Blocking, BusyLoop}`），`ThreadedHandlerBase` / `BusyLoopThreadedHandlerBase` 改为薄壳；StateT 通过成员模板 `RealTimeThreadedExecutor` 选择执行器，缺省 `DedicatedThreadExecutor`。
- 默认队列：Vyukov 侵入式无界 MPSC，跨生产者按 exchange 顺序线性化，与现有 mutex+list 的 FIFO 语义一致。
- 放弃：tm_infra 内置 moodycamel / folly。moodycamel 跨生产者不保序，CheckTime=true 时 `TimeChecker<false,T>` 会多丢数据；folly 依赖过重。用户可按新契约自行适配。
- 可选参考实现：共享线程池执行器 `RealTimeSharedPoolExecutor`（opt-in），BusyLoop 模式退回专用线程。

### 第二阶段：执行器抽象重构（2026-11-06 起，进行中）
去掉 `ThreadedExecMode`，执行器契约只剩 `Exec<Item>`，执行方式由各执行器自行实现；新增 `InlineExecutor` 表示同步执行，所有基于 handler 的框架节点合并为一份实现，按策略选择同步或 threaded。BusyLoop 移出 infra，ChainWriter 改用本地 handler（基线实现，修掉 1ms 白等）；piece 生成脚本提交进仓库。第二阶段代码已实现，debug/release 与 TSan 核心测试已通过；transport socket 测试受当前沙箱限制，examples 回归进行中。设计、决策与验证计划见 [phase2-unified-handler-executor.md](phase2-unified-handler-executor.md)。

## 对上层的影响（实施前已核查）
- 源码兼容：直接继承者 GenericIOStreamImporterExporter、ByteDataWithTopicRecordFileImporterExporter、ChainWriter InnerHandler1/2、JsonRESTClientFacility×3、GrpcClientFacility 只用保留接口。
- ChainReader importer 形态自带线程，不受影响；action 形态依赖 `setIdleWorkerForAction`（构造后调用），需 `wakeIdle()` 唤醒。
- ChainWriter 的 idle（含 IdleLogic 驱动的外部效果）保持 1ms 节奏；idle 只在队列空且等待超时时调用。
- 池执行器打破"每个 threaded action 独占线程"的文档承诺（PATTERNS.md 4.1、tm_infra/01-app-types.md:166），仅 opt-in；带 IdleLogic 的 ChainWriter、阻塞型回调不适用。

## 代码改动（按文件列出）
- 新建 `tm_kit/infra/RealTimeThreadedExecutor.hpp`：执行器契约文档、`ThreadedExecMode`/`ThreadedExecOptions`、Vyukov MPSC 队列、`DedicatedThreadExecutor`（Blocking/BusyLoop）、`RealTimeThreadedExecutorComponent`、`RealTimeThreadedExecutorFor<StateT,Item,M>` 选择器。
  - 线程访问的状态放在 `shared_ptr<State>` 中由 detached 线程共同持有，执行器对象先析构也不会悬空。
  - 唤醒协议：生产者 seq_cst `exchange` 后 seq_cst 读 `sleeping`；消费者 seq_cst 写 `sleeping` 后 seq_cst 读 `head_`（`maybeNonEmpty`，覆盖 exchange 与链接之间的窗口）。
  - 踩坑：最初用 `atomic_thread_fence` 实现 Dekker 握手，GCC 15 报 `-Wtsan`（TSan 不建模 fence），TSan 结果对唤醒协议无效；改为纯 seq_cst 原子操作。
- `tm_kit/infra/RealTimeApp.hpp`：新增私有 `ThreadedHandlerCore<T,W,Mode>`（timeChecker + 执行器 + Host 回调适配器），`ThreadedHandlerBase` / `BusyLoopThreadedHandlerBase` 改为分别以 Blocking / BusyLoop 继承它的薄壳。
  - 保留的接口：`startThread()`、`putData()`、`stop()`、`timeCheckGood()`、`timeChecker()`、`threadHandle()`、`BusyLoopThreadedHandlerBase(bool noYield)`；新增 protected `wakeIdle()`。
  - `W::hasIdleWork()` 用 SFINAE 检测，没有时视为 true（保持 1ms idle 节奏）。
  - `OutputRealTimeThreadBufferSizeComponent` 日志改为报告批次开始时的待处理计数（原为 splice 后的批次大小），开启时才维护计数。
  - 去掉 `<list>` 依赖的两份 mutex+list+condvar 实现（-137/+83 行）。
- `tm_kit/infra/meson.build`：安装 `RealTimeThreadedExecutor.hpp`。
- ActionCore / MultiActionCore（`RealTimeApp.hpp` 中 2 处 + `RealTimeApp_ActionCore_Piece.hpp`、`RealTimeApp_MultiActionCore_Piece.hpp` 各 19 处 variant 特化，共 40 处）：
  - 新增 `std::atomic<bool> hasIdleWorker_ {false}`（默认成员初始化，构造函数初始化列表不变）。
  - `setIdleWorker()` 在锁内更新 `hasIdleWorker_` 与 `idleWorker_`，锁外调用 `this->wakeIdle()`，唤醒可能已无限期睡眠的执行线程（`setIdleWorkerForAction` 在构造启动之后调用）。
  - 新增 `hasIdleWork()` 返回该原子；`idleWork()` 在无 idle worker 时不加锁直接返回。没有 idle worker 的 threaded action 从此不再每 1ms 醒一次。
  - piece 生成器不在仓库中，用一次性 Python 脚本对每个特化做同一文本替换；替换后逐文件计数核对（每个 piece 文件 member/setter/idleWork 各 19 处），并抽查 diff 形状一致。
- 新建 `tm_kit/infra/RealTimeSharedPoolExecutor.hpp`（可选，非默认）：`SharedThreadPool`（进程级单例，`configure()` 须在首个节点构造前调用，之后抛 `std::logic_error`；可配线程数、绑核、idle tick、`maxBatch`）、每节点一个 strand、`RealTimeSharedPoolExecutorComponent`。
  - strand 协议：有符号 `pending_` 计数（队列项 + 一个 start token + 至多一个 idle token），由 0 变非 0 的一方投递 strand；slice 持 `hostMutex_` 执行，结束时减去消耗量，仍为正则重新投递。保证回调串行（契约 1）和跨生产者 FIFO（契约 2）。
  - 放弃的方案：用 `tail_` 指针比较判断"队列自上次以来是否变化"——节点释放后地址可能复用（ABA），改为计数协议。
  - BusyLoop 模式退回 `DedicatedThreadExecutor`；`threadHandle()` 返回 `nullopt`；executor 析构时等待正在运行的 slice（从自身回调内析构时改为 orphan 标记，由 slice 结束后释放 host）。
  - idle 由池级 tick 驱动；`executorHasIdleWork()` 为 false 的节点空闲时零开销。
  - 注释中的配置示例原用 C++20 指定初始化器，已改为 C++17 写法。
- `tm_kit/infra/meson.build`：同时安装 `RealTimeSharedPoolExecutor.hpp`。

## 测试与验证（命令、构建类型、结果）
- 执行器冒烟测试（临时程序，g++ 15 `-O1`，普通与 `-fsanitize=thread` 各一次）：4 生产者 × 50000，Blocking/BusyLoop 均全部处理、每生产者顺序无误；Blocking 在无 idle work 时 idle 调用为 0，`wakeIdle` 后恢复约 1ms 节奏；stop 后 `push` 返回 false。TSan 无报告、无 `-Wtsan` 警告。正式测试见任务 #6。
- 薄壳接入后 tm_infra worktree debug 构建（`cd build-common && source ./build-env-setting.sh && ASSUME_YES=1 NO_INSTALL=1 ./build.sh ../worktrees/realtime-threaded-executor/tm_infra`）：通过，日志无错误。为避免覆盖共享 `install/` 中其他会话在用的头文件，未安装。唯一警告 `RealTimeApp.hpp:415` `-Wsign-compare` 为 develop 已有代码（develop 上为 470 行），非本次引入。
- `time_checker_test`：ALL PASSED（含两 importer 线程 → simpleExporter 图测试）。
- ActionCore 改动后 tm_infra worktree debug 构建（同上命令）：通过，日志无错误；`time_checker_test` 再次 ALL PASSED。
- ActionCore 冒烟测试（临时程序，gcc-toolset-15 g++ 15.2.1，普通与 `-fsanitize=thread` 各一次）：threaded `liftPure`、`liftPure2`（variant 2 路）、`liftMulti3`（variant 3 路）正确分派；构造启动后再 `setIdleWorkerForAction`，idle worker 仍被调用（约 47–48 次 / 50ms，符合 1ms 节奏）。TSan 无报告。
  - 踩坑：未 source 构建环境的 shell 里 `g++` 是系统 GCC 8.5，`-fsanitize=thread` 链接到不存在的 `/usr/lib64/libtsan.so.0.0.0`；手工编译测试必须先 `source build-common/build-env-setting.sh`（gcc-toolset-15）。
- 池执行器冒烟测试（临时程序，gcc-toolset-15，`-O1` 普通与 `-fsanitize=thread` 各一次）：池 2 线程、8 个 ThreadedHandler 节点、4 生产者 × 20000 写每个节点。每节点 80000 条全部处理，`overlap=0`（回调不重叠），`orderErr=0`（每生产者顺序正确），`waitForStart` 恰好 1 次；只有设置了 idle work 的节点 0 收到 idle 调用（安静 50ms 内 45–47 次）。`static_assert` 确认 Blocking 选中 strand 执行器、BusyLoop 退回专用线程。TSan 无报告。
- 正式测试 `tm_infra/test/threaded_handler_test`（加入 `test/meson.build`）：默认执行器与共享池各跑一遍，覆盖执行器选择、多生产者不丢不重且每生产者有序、回调不重叠、BusyLoop、回调内 `stop()`、idle 节奏（有 idle work 约 93–95 次/100ms，无 idle work 0 次，`wakeIdle` 后恢复）、图中 threaded `liftPure`/`liftPure2`/`liftMulti3`/FireOnceOnly、启动后 `setIdleWorkerForAction`、CheckTime=true 单生产者、单条延迟。debug 构建 46 项 ALL PASSED；单条 push→handle 延迟 p50≈15µs、p99≈23µs（dedicated），p50≈17µs（池），旧实现为 ms 级。
  - `-fsanitize=thread`（gcc-toolset-15，链接 worktree 的 debug 静态库）：ALL PASSED，0 报告。
  - 踩坑：TSan 首轮报的竞争全部在测试代码：idle 计数器、`Stats` 为栈对象，handler 线程 detach 后仍可能访问（即已知的 detach 生命周期问题）。测试改为堆上分配并有意泄漏 env / Stats / handler，不再 `delete` 运行中的 handler。
- tm_basic：对 worktree 安装前缀（`worktrees/realtime-threaded-executor/install`，避免覆盖共享 `install/`）重新编译，通过。
- tm_transport 新测试 `test/threaded_handler_transport_test`：同进程内 JSON REST 服务端 + `JsonRESTClientFacility`（ThreadedHandler 子类），两种执行器各跑：4 线程并发 2000 请求全部返回、无重复、内容正确；300 次顺序往返 p50≈1.35ms（以 HTTP 本身为主，两种执行器接近）。ALL PASSED。
  - 踩坑：worktree 中 `external/certify` 子模块未初始化导致 meson 配置失败，用主仓库作 reference 初始化（commit `b74d50d` 与主仓库一致）；两轮测试共用一个 env 时同一端口被绑定两次，改为每轮独立 env 与端口。
- tm_examples：在独立 worktree（detached `7fac148`，只需编译回归故未建分支）中对 worktree 安装前缀全量构建：291/291 目标完成，0 错误，确认下游源码零改动。

## tm_basic：基于 moodycamel 的执行器（任务 #7）
- 新建 `tm_basic/tm_kit/basic/MoodycamelThreadedExecutor.hpp`（安装），`MoodycamelThreadedExecutor<Item,Mode>` + `MoodycamelThreadedExecutorComponent`。依赖 vcpkg `concurrentqueue`（header-only，`__has_include` 探测，缺失时不定义，宏 `TM_KIT_BASIC_HAS_MOODYCAMEL_THREADED_EXECUTOR`）。
  - Blocking 用 `BlockingConcurrentQueue`（有 idle work 时 `wait_dequeue_bulk_timed(idlePeriod)`，否则 `wait_dequeue_bulk`）；BusyLoop 用 `ConcurrentQueue` + `try_dequeue_bulk`；批量 64。`wakeIdle`/`stop` 入队空 token 唤醒。
  - 契约差异（已写入头文件）：跨生产者不保序，CheckTime=true 且多线程输入时 `TimeChecker` 可能多丢数据；pending 计数为 `size_approx()` 近似值。
- 测试 `tm_basic/test/moodycamel_executor_test`：与 tm_infra 测试相同场景（只校验每生产者顺序），默认与 moodycamel 各跑：ALL PASSED，延迟 p50≈12µs。TSan 0 报告（moodycamel 头文件内 `atomic_thread_fence` 有 `-Wtsan` 警告，故 TSan 对其内部同步的覆盖不完整）。

## tm_basic 图框架开销基准（`test/simple_graph_bench`）
- 目的：衡量 RealTimeApp 模式下图框架本身的开销（节点、队列、环境、调度），不是测业务逻辑。手动运行，建议绑核。
- 图覆盖的节点/连接类型：`liftPure`、`liftMaybe`（丢弃 1/3）、`liftMulti`（1:2）、tuple 产生 + `dispatchTupleAction`（tuple→variant 扇出，`connect_2_0/2_1` 按分支连接）、`liftPure2`（两输入 variant 汇合）、`liftPureOnOrderFacility` + `triggerImporter`（on-order facility，输出 `KeyedData`），以及若干 exporter。
- 四个变体只改 `SuggestThreaded` 的分布，用差值体现不同执行模型的框架成本；采集 wall / CPU ns per push；收尾以"两个 sink 最后一次计数变化的时刻"为计时终点（不把排空后的静默窗口计入），不用固定期望计数（扇出深度取决于 maybe 步骤保留多少）。
- **release 构建**（`BUILD_TYPE=release`，tm_infra/tm_basic 均安装到 worktree 前缀），`taskset -c 20-27` 绑核，每变体 200000 次 push，5 次运行取中位数：

| 变体 | wall ns/push | CPU ns/push | kpush/s |
|---|---|---|---|
| plain（无 threaded 节点） | 1697 | 3121 | ≈590 |
| 源节点 threaded | 1644 | 5292 | ≈610 |
| action + facility threaded | 1208 | 4493 | ≈830 |
| 源 + action + facility 全 threaded | 1255 | 4339 | ≈800 |

  wall 在 threaded 变体下更低（生产者与主线程解耦，主线程不用等下游处理），CPU 更高（跨线程唤醒与调度）；debug 构建同样形态但绝对值约 3–4 倍，故效率数字只采用 release。
  单次运行波动约 ±20%（共享机器），只适合同轮、同机器相对比较。
- 踩坑：`AppRunner` 经 `env.log()` 报图警告，环境必须带日志组件（用 `TimeComponentEnhancedWithSpdLogging`）；同一个 action 只能在第一次连接时注册名字，之后要用不带名字的重载；`registerOnOrderFacility` + `placeOrderWithFacility(带名字)` 会重复注册；facility 的输出 sink 类型是 `KeyedData<in,out>`；最早的"静默 100ms"等待把等待时间也计入计时，改为以最后一次计数变化为终点；kpush/s 列最初用整数运算得到 0/1，改为浮点 `messages/(wallNs/1e6)`；joinOut/facilityOut 改为减去预热前基数，只统计计时期间的输出。

## 兼容性与注意事项
- 第二阶段删除 `BusyLoopThreadedHandler`（含 `BusyLoopThreadedHandlerBase`）和 `ThreadedExecMode`，属于不兼容改动，已经用户确认；下游仅 ChainWriter 与 moodycamel 测试使用，随本阶段一起修改。
- ActionCore/MultiActionCore 的 variant 特化 piece（2–20 路）为机器生成，生成器不在仓库中（见 tm_kit_reference/tm_infra/09-generated-pieces.md），用脚本对各特化做同一改动。
- tm_kit_reference（`PATTERNS.md` 4.1、`tm_infra/01-app-types.md` 的执行器/共享池说明）待推送远程后再更新。
- 已有问题（非本次引入）：handler 线程 detach 后析构 join 不生效，ChainWriter 析构 `delete innerHandler1_` 时线程可能仍在运行。列为后续项。

## 相关文档（相关任务、tm_kit_reference 章节）
- tm_kit_reference：`PATTERNS.md` 4.1、`tm_infra/01-app-types.md`、`tm_infra/09-generated-pieces.md`、`tm_basic/08-shared-chain.md`
- 下游记录：`tm_basic/doc/INDEX.md`（基准扩展）、`tm_transport/doc/INDEX.md`（transport 层 threaded handler 回归测试），均链接回本记录

执行器额外开销优化已实施并完成本轮定向验证，包含 Inline 静态调用、MPSC 原位消费、SharedPool 批次调用与 idle 活跃集合；profiling 工具、取舍、数据和测试见 [executor-overhead-optimization.md](executor-overhead-optimization.md)。examples 全量编译仍暂停，第二阶段整体状态不变。

端到端延迟的后续优化与有限自旋测量见 [executor-latency-optimization.md](executor-latency-optimization.md)（本轮完成，2026-11-06；第二阶段整体验收状态不变）。

2026-11-07 已完成 deprecated 的历史执行器迁移及重新 profiling（460 行结果全部通过正确性检查），见 [legacy-executor-baseline.md](legacy-executor-baseline.md)。

同日已修复 Legacy 的无条件等待并完成 198 条延迟复测；修复前 / 后数据独立保存，见 [等待修复记录](legacy-executor-baseline.md)。
