# Executor 额外开销分析与优化优先级

**日期：** 2026-11-06
**状态：** 分析完成，优化尚未实施；编译继续按用户要求暂停
**分支：** `feature/realtime-threaded-executor`
**主记录：** [README.md](README.md)
**第二阶段：** [phase2-unified-handler-executor.md](phase2-unified-handler-executor.md)

## 范围与结论

分析当前 Inline、DedicatedThread、SharedPool 和 Moodycamel 四种实现。以下区分源码中确定存在的操作与尚未测量的性能收益，不承诺提升比例。

`optional<Item>` 自身不单独分配 Item；更值得优先处理的是间接调用、额外移动、逐消息节点分配、线程池调度和空闲扫描。已有整图 plain wall +6.2% 不能归因于某一个操作；push→handle 的十几微秒延迟也包含 OS 唤醒和调度，不能全部视为执行器自身的 CPU 成本。

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

本轮仅分析与记录，没有实施上述优化，也没有恢复编译。
