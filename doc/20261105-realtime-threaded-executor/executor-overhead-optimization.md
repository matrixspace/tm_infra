# Executor 额外开销分析与优化优先级

**日期：** 2026-11-06
**状态：** 本轮优化完成；第二阶段整体验收仍待 examples / transport 环境验证
**任务结束时间：** 2026-11-06
**分支：** `feature/realtime-threaded-executor`
**主记录：** [README.md](README.md)
**第二阶段：** [phase2-unified-handler-executor.md](phase2-unified-handler-executor.md)

## 范围与结论

分析当前 Inline、DedicatedThread、SharedPool 和 Moodycamel 四种实现。以下区分源码中确定存在的操作与尚未测量的性能收益，不承诺提升比例。

`optional<Item>` 自身不单独分配 Item；更值得优先处理的是间接调用、额外移动、逐消息节点分配、线程池调度和空闲扫描。已有整图 plain wall +6.2% 不能归因于某一个操作；push→handle 的十几微秒延迟也包含 OS 唤醒和调度，不能全部视为执行器自身的 CPU 成本。

以下第 1–4 节保留优化前的源码审查和候选方案；最终实现与测量结论见文末。

## 1. InlineExecutor：恢复静态调用路径

当前每条消息读取 `running_`，再经过 `void* host_` 和函数指针 `handle_` 调用 Host。编译器可能消除部分间接调用，但不能假定所有实例化、编译选项下都会消除。

建议给内置 InlineExecutor 提供带静态 Host 类型的内部提交入口，由统一 handler 核心的提交辅助函数在编译期选择；外部自定义执行器继续使用现有 `push(Item&&)` 契约。保留 stop 状态检查。

- 目标：去掉逐条函数指针调用，增加 Host→actuallyHandle→业务包装函数连贯内联的机会。
- 约束：只对明确支持该入口的执行器优化；不能用 `kOwnContext=false` 直接绕过自定义执行器，其内部可能还有串行化、限流或统计逻辑。
- 不拆回两套 Action / MultiAction 实现，不取消 stop 后拒收的行为。
- 优先级：高，改动范围相对小。

## 2. Dedicated / SharedPool 共用 MPSC

当前入队 `new Node()` 后将 Item 移入节点；出队又将 Item 移到局部 optional，再调用 handler，并删除旧哨兵节点。

### 原位消费

将队列内部接口改为类似 `tryConsume(F&&)`，消费回调直接拿节点中的 `Item&&`，消费完成后销毁 Item。保留 Node 内 optional 来支持空哨兵与非默认构造的 Item。

- 去掉消费端暂存和一次 Item 移动，不宣称消除节点分配。
- 应让回调保持模板化，不额外引入 `std::function` 类型擦除。
- 保留批次日志、pending 计数、每条消息 stop 检查和 SharedPool 的 maxBatch 限制。
- 正确维护哨兵推进、Item 销毁以及异常时的资源清理。
- 优先级：高，两种执行器共用收益。

### 节点分块分配与回收

这是减少逐消息 `new/delete` 的主要方向。先量化分配成本，再选择分块分配、缓存空闲节点或定制 allocator。

- 保持默认无界接收和跨生产者 FIFO，不以队满丢弃换取速度。
- 只回收已经安全退役的旧哨兵；新尾节点仍属于队列协议。
- 避免 ABA、生产者退出后回收路径悬空，以及全局空闲链表锁抵消收益。
- 区分队列初次增长与稳定复用阶段，报告峰值内存及回收策略。
- 优先级：中高，潜在收益大，但复杂度高于原位消费。

### 唤醒与内存序

多个生产者同时看到 sleeping=true，可能依次进入通知 mutex 并重复 notify。可研究一次唤醒周期内的通知合并，但必须重新证明睡眠握手，不能直接删锁或改弱原子序。

当前 seq_cst 操作明确参与防止漏唤醒的协议；更弱的 memory_order 不必然产生更快机器码。短自旋仅是在 CPU 与唤醒延迟之间权衡，不能算作无代价优化。

## 3. SharedPool：摊薄类型擦除和调度成本

### 类型擦除移到 slice 边界

当前 `IHostAdapter::handle` 每条消息虚调用一次。可以在 `start<Host>` 时设置一个类型擦除的 slice 入口，由 `runSliceTyped<Host>` 在整个批次内直接调用具体 Host。

- 将每条一次的间接调用变为每 slice 一次，给批次内回调提供内联机会。
- 可以同时评估取消仅用于保存 Host 指针的堆分配 adapter。
- 保持 hostMutex 的退出同步、回调内销毁的 orphan 处理和 strand 串行性；不能为了减少锁直接删除这些保护。
- 优先级：高。

### 空闲扫描

当前池线程每个 idleTick 创建一个临时 alive vector，遍历全部 registry，对每个存活 weak_ptr 加引用，再判断 wantsIdle。无 idle work 的节点不会收到 idle 回调，但仍有扫描与引用计数成本；目前头文件的“cost nothing while idle”描述不够精确。

两步优化：
1. 复用临时 vector 容量；每轮结束及时 clear，避免延长 strand 的存活时间。
2. 维护需要 idle 回调的活跃集合；集合为空时停止周期性轮询，由 wakeIdle / 注册变化唤醒计时线程。

第二步必须保留构造完成后才设置 idle worker 的行为，以及原有 idle 节奏，复杂度高于复用 buffer。

### ready 队列与批次

当前 schedule 经过全局 mutex、deque、shared_from_this 和 notify；worker 取任务也经过同一个 mutex。maxBatch 默认为 64，繁忙 strand 达到上限后重新投递。

先测不同 maxBatch 对吞吐、CPU、其他 strand 尾延迟的影响，再评估 worker 本地续跑和批量取任务；若全局锁确实成为瓶颈，再考虑分片队列或工作窃取。必须保留公平性和单 strand 不重入，不能仅扩大批次并只报告吞吐。

注意：当前 shared_ptr 并非每次普通 push 都复制。引用计数主要发生在调度、扫描和生命周期操作，不能误判为所有执行器逐消息都会更新 shared_ptr 引用计数。

## 4. Moodycamel：复用 token、减少重复快速路径

当前没有显式 producer / consumer token。上游资料已通过本机安装的 blockingconcurrentqueue.h、lightweightsemaphore.h 交叉核对。

- ConsumerToken：单消费线程在循环外创建并复用，供 try / wait / timed bulk 路径共享。
- 合并消费入口：当前先 try_dequeue_bulk，失败后 wait_dequeue_bulk[_timed]；waitMany 内部已经先调用 tryWaitMany，可测试直接 wait bulk 的变体，减少空队列时重复检查。
- 预分配：按 producer 数和峰值积压预留块，降低运行中增长概率，仍允许增长以保持接收语义。
- ProducerToken：每个并发 producer 独占并持久复用；不能所有 push 共用一个成员 token，也不应每次 push 临时构造。当前 push 接口不携带 producer 上下文，因此需先设计可靠的身份与生命周期管理。
- 自然批次：如果调用方本来就有批次，可以额外设计批量提交，摊薄队列和 semaphore 更新；不能为凑批增加默认单条提交延迟。

成功单条 enqueue 会调用 semaphore signal，其中有原子计数更新，但只有需要唤醒等待者时才进入底层 semaphore，不能描述为每消息一次系统调用。try_dequeue_bulk 也经过 semaphore；外围先 try 并未绕开该成本。

`try_enqueue` 会引入失败路径，预分配容量也不等于硬容量。不能直接替换 enqueue 并忽略失败。bulk buffer 的 vector 仅在线程启动时分配一次，通常不是优先优化对象；换成栈上 array 还要评估大型 Item 的栈占用。

一手资料：
- [官方 README：token、预分配与 bulk](https://github.com/cameron314/concurrentqueue/blob/master/README.md)
- [BlockingConcurrentQueue](https://github.com/cameron314/concurrentqueue/blob/master/blockingconcurrentqueue.h)
- [LightweightSemaphore](https://github.com/cameron314/concurrentqueue/blob/master/lightweightsemaphore.h)

当前会话仍未暴露 Context7 工具。按仓库回退规则委派只读上游资料核查，并与本机头文件核对；未安装工具、未修改环境或第三方代码。

## 建议实施顺序与验收

1. 先建立执行器直接调用的微基准，隔离业务逻辑、TimeChecker 与日志；记录分配次数、Item 移动次数和 CPU ns/item。
2. Inline 静态提交 + MPSC 原位消费。
3. SharedPool 批次级类型擦除 + 复用 idle 扫描 buffer；Moodycamel ConsumerToken + 简化消费入口的独立对照。
4. 根据测量结果推进节点回收、idle 活跃集合、ready 队列改造和持久 ProducerToken。

覆盖单生产者 / 多生产者、稀疏 / 突发 / 饱和输入、大量空闲节点，以及小标量 / 大对象 / 移动构造可能昂贵的 Item。分别报告生产者 CPU、消费者 CPU、吞吐、p50/p99/p999、唤醒及上下文切换；保留 FIFO、stop、idle、FireOnceOnly、动态设置 idle worker 与 TSan 回归。

以上为实施前分析；最终取舍和验证见下文。


## 实施记录
- 2026-11-06：开始优化前已重新检查功能测试、low_latency_bench、simple_graph_bench 和 IPC profiler 源码。handler 测试每消息多次统计原子更新，适合正确性回归，不适合隔离 Executor CPU；low_latency_bench 主要覆盖同步图与基础部件；simple_graph_bench 含时钟、TimeChecker、业务 vector 分配，CPU 截止点还包含静默等待；IPC profiler 面向跨进程延迟，不能直接替代执行器微基准。
- 方案确定：保留现有回归工具；新增 tm_basic/test/executor_bench，直接比较 direct / Inline / Dedicated / SharedPool / Moodycamel。独立进程运行每个案例，预热后测量；正常计时与分配 / 移动计数采用不同二进制；支持多生产者、大小消息、稀疏 / 饱和 / 空闲场景、JSONL 与重复对照脚本。先保存基线二进制及结果，再实施优化。
- 本阶段不改变无界队列接收、FIFO、stop、idle 语义；先实施静态提交、原位消费、共享池批次调用和扫描 buffer 复用、moodycamel 消费路径优化，较复杂方案根据测量决定。

- profiling 工具验证：normal / counts 分离；counts 同时捕获 new/delete 与可执行文件引用的 malloc/free，避免漏计 moodycamel。四生产者小消息基线确认 Dedicated 每条 1 次节点分配、2 次 Item 移动；Moodycamel 每条 3 次移动，块扩容由 C malloc 捕获。完整同二进制自对照矩阵（quick、2 次）全部正确性检查通过。perf 软件与硬件事件均可用。
- 第一批方案已实施：Inline 增加可选静态提交 hook，统一核心保留自定义执行器 fallback；MPSC 原位消费，入队构造异常自动释放 Node、消费异常销毁值并留下有效哨兵；SharedPool 类型擦除移到 slice 入口，保留 hostMutex / orphan 退出协议，复用 tick vector 容量；Moodycamel 复用 ConsumerToken，并使用内部已有快速路径的 wait bulk 接口。
- 新增 executor_mechanism_test，覆盖 move-only/non-default 类型、零额外出队移动、构造 / 回调异常、两种 Inline 提交的 stop、自定义同步 Executor 不被绕过、SharedPool 从外部析构等待和回调内析构。

- 首轮短测量的 direct 控制项也有显著波动，不能据此认定 CPU 收益。工具已补每角色显式绑核和较长同步测量；CPU 20–27 各自的 SMT sibling 为 48–55，选择 producer 20–23、consumer 24–27，避免两角色落在同一物理核心。重建基线时只使用临时 worktree 的旧执行器头文件，使用完全相同的新测试工具与编译参数。
- 定向长测量（3 次交替中位数）中 direct 控制变化约 +0.2% / -1.0%，Inline 小 / 大消息 CPU/item 约 -13.1% / -7.5%。此为中间结果，最终按完整矩阵收尾，不把小幅异步差异当作确定收益。
- perf record 的 Dedicated 四生产者基线取得约 4K 个用户态 cycles 样本、0 丢样；malloc / _int_malloc / _int_free / cfree 的 self 样本合计约 48%。逐消息分配仍是重要剩余成本；节点池需要独立设计回收协议，不能以未经验证的全局锁或 ABA 风险替换。
- 进一步实施 idle 活跃集合：无 idle 节点不进入 timer registry；集合空时条件变量等待，wakeIdle 更新恢复计时；stop 在同一 registry 锁下移除，延迟激活在锁内重查 running，避免停机后重新登记。框架内明确为空的 Continuation / 两种 facility / SimpleExporter 新增 hasIdleWork=false，外部 handler 的默认 idle 兼容策略不变。
- 增加数据输入与频繁 wakeIdle 并发、关闭 / 恢复空闲计时、启动回调阻塞期间 stop 的定向回归。最新机制测试、TSan、ASan/UBSan 均通过；LeakSanitizer 因环境 ptrace 限制不能运行，已明确禁用该部分后运行 ASan/UBSan，未宣称完成泄漏检测。

## 最终方案与取舍（2026-11-06）

- `RealTimeThreadedExecutor.hpp`：内置 Inline 使用 `pushWithHost` 静态调用，原 `push(Item&&)` 及其成员函数指针用法保持兼容；辅助函数仅匹配确切内置类型，派生自定义执行器仍调用自身 push。MPSC 在节点内直接消费，移动次数 2→1，Node 构造抛异常自动释放，回调抛异常也清理值并保留有效哨兵。`optional` 继续提供原位生命周期管理，本身不额外分配。
- `RealTimeSharedPoolExecutor.hpp`：Host 类型擦除移到 slice 入口，移除适配器分配；保留原 hostMutex / orphan 生命周期协议、默认 maxBatch 和 ready 队列策略。idle registry 仅登记活跃节点，连续 vector 扫描、哈希索引支持移除；复用扫描 buffer，集合为空时停止周期唤醒。stop 与登记在同锁内协调，并重查状态，防止延迟激活重新登记已停止节点。
- `RealTimeApp.hpp`：明确无 idle 工作的 Continuation、两类 facility 和 SimpleExporter 返回 false；外部旧 handler 未提供查询时仍兼容原周期调用。
- `ExecutorMechanismTest.cpp`：原位消费、异常、move-only / non-default Item、Inline API 与自定义派生类型兼容、析构等待 / 回调内析构、动态 idle 和 stop 竞争、集合压缩与重新激活回归。
- tm_basic 的 profiling 工具新增独立后端编译，固定 producer / consumer 绑核，测量循环外缓存场景判断。整图工具修正精确完成计数、CPU 截止点、单案例隔离，以及旧 “src threaded” 参数错误，并验证 action 实际线程模式。

Moodycamel 的 ConsumerToken + 合并等待入口已独立试验，但未得到稳定整体收益，部分 idle 场景 CPU 增加，因此最终撤回该后端改动。保留工具测量能力和通用 handler 的无效 idle 消除。不能把最终基准中 unchanged moody 后端的起伏解释为实现收益。

逐消息节点回收、持久 ProducerToken、批量提交、ready 队列分片和调大默认批次均未实施。Dedicated 的 malloc/free 用户态 cycles self 样本约占 48%，分配仍是下一步重点；回收方案需要单独证明生命周期、ABA 和竞争成本。

## 最终 profiling

Release，GCC toolset 15，C++17；基线 infra `fdcdcdd`、basic `ee21502` 的旧头文件与候选使用**同一最终测试源码和编译选项**。旧头文件通过临时 detached worktree 隔离，输出 `/tmp/executor-before`，没有复用其他 worktree 的 Meson 缓存。独立编译每个后端，5 次交替进程运行，producer CPU 20–23、consumer 24–27；这些 CPU 的 SMT sibling 为 48–55。时间和计数二进制分离。测试源码、用法和观察成本限制见 [executor_bench README](../../../tm_basic/test/executor_bench/README.md)。

实际命令（工作区根目录）：

```sh
python3 worktrees/realtime-threaded-executor/tm_basic/test/executor_bench/run_profile.py \
  --baseline /tmp/executor-before/executor_bench \
  --baseline-counts /tmp/executor-before/executor_bench_counts \
  --candidate worktrees/realtime-threaded-executor/tm_basic/builddir_release/test/executor_bench/executor_bench \
  --candidate-counts worktrees/realtime-threaded-executor/tm_basic/builddir_release/test/executor_bench/executor_bench_counts \
  --split --repeats 5 --cpus 20-27 \
  --producer-cpus 20,21,22,23 --consumer-cpus 24,25,26,27 \
  --perf --output /tmp/executor-profile-result
```

全部 402 行结果正确性检查通过，数据保存在 [profiling/results.jsonl](profiling/results.jsonl)，中位数见 [profiling/summary.md](profiling/summary.md)，同目录保存 perf stat CSV（包含启动 / 预热，不能视为内部测量窗口的精确逐消息计数）。每行保留实际二进制 SHA256、绑核、计数和延迟分位数。

| 项目 | 结果与范围 |
|---|---|
| direct 控制 CPU/item | 小消息约 0%，大消息 +0.7% / +2.3% |
| Inline 静态调用 | CPU/item -22.0% 至 -33.5%；擦除调用接口基本持平 |
| Dedicated | CPU/item -2.8% 至 +2.6%，不宣称普遍吞吐提升 |
| SharedPool 单 strand | CPU/item -3.8% 至 +4.1%，不宣称普遍吞吐提升 |
| SharedPool 32 strands | CPU/item 1095.3→1001.5 ns，-8.6% |
| SharedPool 128 节点，无 idle 工作 | CPU 17.510→0.105 ms/s，-99.4%；计数窗口的分配 2232→0 |
| SharedPool 128 节点，有 idle 工作 | CPU -0.8%，保留周期调用；复用 buffer 消除扫描临时 vector 的重复分配，调度队列仍可能分配 |
| MPSC 计数 | 80000 条消息移动 160000→80000；Dedicated 节点分配仍为 80000 |

稀疏延迟采用 3000 个闭环样本；数据保留 p50/p99/p999，但这一数量不足以建立稳定的极端尾延迟结论。Moodycamel 原后端在多生产者案例仍有明显波动（CPU/item -16% 至 +12.8%），保留原始数据并明确不归因于代码更改。background CPU 包含协调线程，不能冒充精确 consumer CPU；计数 hooks 的覆盖边界见工具 README。

## 最终验证

所有编译均先 `source build-common/build-env-setting.sh`，仅定向目标、`ninja -j2`，安装到本 worktree 私有 install。

- infra release / debug：`meson test -C .../tm_infra/builddir_{release,debug} --no-rebuild`，mechanism / threaded_handler / time_checker 均 3/3 通过。
- basic release：chain_writer_loop / moodycamel_executor / tsc_clock，3/3 通过；debug 的 chain_writer_loop / moodycamel_executor 均 2/2 通过。
- 最新机制测试：`g++ -std=c++17 -O1 -g -fsanitize=thread -pthread -I.../tm_infra .../ExecutorMechanismTest.cpp`，全部通过，无 TSan 报告。`-fsanitize=address,undefined` 同样通过；`ASAN_OPTIONS=detect_leaks=0`，因环境 ptrace 限制未完成 LeakSanitizer 检测。
- 旧阶段已有 FIFO、FireOnceOnly、动态 idle worker 和多执行器兼容测试继续运行。examples 全量编译没有恢复；transport HTTP 运行仍受原环境 socket 限制。本轮完成不代表第二阶段整体已验收。

记录与源码一起提交，提交号随后补记。按主任务原约束，保留 feature 分支、不合并 dev / 不推送；tm_kit_reference 待开发验证、推送后同步。

测试工具提交：tm_basic `62b1b08`（角色绑核）、`ee21502`（独立测试工具与整图完成判定），最终工具提交见下方收尾记录。

### 整图回归补充

修正后的同一整图工具，baseline / candidate 各 5 次、每次 500000 输入，独立进程交替运行、taskset CPU 20–27；本轮没有并发编译。40 次运行均收到精确期望的输出，bad=0。原始输出见 [graph-comparison.log](profiling/graph-comparison.log)。

| 图配置 | wall ns/msg 基线→候选 | CPU ns/msg 基线→候选 |
|---|---:|---:|
| plain | 2552→2501 | 4665→4584 |
| src threaded | 1424→1398 | 2929→2917 |
| action+facility threaded | 1990→2249 | 10007→10853 |
| src+action+facility | 2087→2654 | 9641→9894 |

该整图同时包含时钟、TimeChecker、业务 vector 分配、多个独立队列和调度竞争，不能当作隔离 Executor 开销的指标；但全线程 wall 回退也不能省略，优化收益具有负载依赖性，不能宣称所有图加速。为排查原位消费的影响，另做只恢复一次出队移动的实验，结果补记如下。

追加三组对照：只在临时覆盖头文件的 `tryConsume` 中恢复 `Item local(std::move(*next->value))`，提前 reset，再把 local 交给回调；其余候选实现不变。GCC 15.2.1、同编译参数、各 5 次，轮换进程顺序，均精确计数通过。结果见 [graph-move-experiment.log](profiling/graph-move-experiment.log)。实验二进制没有替换最终实现，也没有写入工作区头文件。

| 配置 | 原基线 wall / CPU ns/msg | 候选 wall / CPU | 恢复移动 wall / CPU |
|---|---:|---:|---:|
| action+facility threaded | 2298 / 10656 | 2326 / 10417 | 2667 / 11286 |
| src+action+facility | 3107 / 11550 | 2159 / 10021 | 1851 / 7446 |

恢复移动的效果在两个场景中方向相反，同一基线全线程 wall 中位数在两轮间由 2087 变为 3107 ns/msg，候选则由 2654 变为 2159。因此尚不能建立原位消费导致固定性能回退的因果关系，也不能认定全线程整图有稳定加速。最终保留确定减少一次移动、通过异常与并发回归的原位消费；多级图的调度 / 缓存竞争继续作为后续 profiling 范围，不以这组高波动数据设普遍性能保证。

最终测试工具提交：tm_basic `ceaa617`。临时基线两个 worktree 已删除，保留主 feature worktree 和本机 `/tmp` 的测量二进制以便复查。
