# 默认执行器回迁与 Enhanced MPSC 节点复用

**任务开始时间：** 2026-11-07
**任务结束时间：** 2026-11-08
**状态：** 完成（本轮迁移与节点复用；主任务第二阶段状态不变）
**分支：** `feature/realtime-threaded-executor`（tm_infra commit `0a9796c`；tm_basic commit `cbfde30`）

## 背景与需求
此前 [执行器开销 profiling](executor-overhead-optimization.md) 中，Dedicated 的 malloc/free self cycles 约占 48%，每消息仍有一次节点分配与释放。本次为同一任务的后续工作。

用户在实现过程中调整需求：infra 默认执行器改为 Legacy 实现，去掉 deprecated 后改名；旧 MPSC 执行器移到 basic，命名为 `EnhancedDedicatedThreadedExecutor`，继续优化延迟和吞吐，优先使用成熟第三方库。默认名称按原正确拼写保留 `DedicatedThreadExecutor`，最初暂留 Legacy 别名，后续按用户要求删除（见文末）。

## 最终方案
- infra 默认：原 Legacy 的 mutex + incoming/processing 两个 list + splice，实现保留已经修复的 predicate wait、wakeIdle 和每条 stop 检查。仍约每 1ms 调用 idle，即使 Host 的 hasIdleWork=false；忽略 spinWait。保留默认类型的第二 Config 模板参数，idlePeriod 使用 Config 值。
- basic Enhanced：保留原 MPSC 的入队 exchange 全局 FIFO、原位消费、eventcount 睡眠握手及有限自旋，新增八分片空节点缓存。
- 缓存使用已安装 Boost 1.91 的 `boost::lockfree::spsc_queue<Node*, capacity<32>>`。每个 Node 记住来源分片，MPSC 单消费者把安全退役的旧哨兵送回该分片。生产者只保存 thread_local 整数分片编号，不保存队列地址，不依赖生产者退出时回调。
- 每个分片只有一个逻辑读取方，通过 atomic_flag acquire/release 串行化调用 Boost pop；发生碰撞的生产者不等待，直接分配节点。因此任何时刻 Boost 队列只有一个写入方和一个读取方，读取方跨线程切换通过 flag 同步。缓存空时 new，满时 delete，仍无界接收。
- 回收的旧哨兵已完成上一个生产者的 next 链接写入；当前尾哨兵不回收。Item 在消费回调结束或抛异常时立即 reset，缓存中没有仍存活的 Item。构造异常由 unique_ptr 清理已取得的空节点。
- SharedPool 保留原未池化 MPSC，只提取到 infra 独立头文件，infra 不引入 Boost 或其他第三方依赖。

### 内存取舍
每队列最多缓存 256 个空 Node；Node 按需分配，不预分配消息对象。缓存元数据随队列存活，GCC15/x86_64 下队列对象为 3712 字节（原队列 128 字节）。额外堆内存上限是 256 × sizeof(Node)，与 Item 大小相关。队列本身仍无界；峰值排队内存取决于生产者与消费者速率差。析构要求外部并发访问已停止，释放缓存和剩余消息。部分分片空而其他分片满时不跨分片借用，避免引入新的竞争热点。

### 放弃的方案
1. 共享 SPMC 回收环：逐消息分配明显减少，但公共读取游标成为四生产者热点，CPU 回退，弃用。
2. 自研分片回收环：初测优于共享环，但用户要求优先采用成熟库，最终不保留自研回收协议。
3. Boost MPMC stack 空节点缓存：支持安全并发回收，但额外 CAS 成本较高，定向初测单生产者约 572→953 CPU ns/item，弃用。
4. 直接以 Boost 通用 queue 存 Item：要求 trivial assignment/destruction，不能覆盖框架任意 move-only 消息。以指针承载可以解决类型限制，但不会自动消除 Item 节点分配，因此本轮只复用空节点。
5. Context7 返回的新 mpsc_weak_queue 在本机 Boost 安装中不存在，未假设 API 可用，也未修改或安装第三方库。

第三方 API 通过 Context7 查询并对照本机头文件：Boost [SPSC 队列](https://www.boost.org/doc/libs/latest/boost/lockfree/spsc_queue.hpp)、[stack](https://www.boost.org/doc/libs/latest/boost/lockfree/stack.hpp)、[通用 queue 要求](https://www.boost.org/doc/libs/latest/boost/lockfree/queue.hpp)。无需新增 vcpkg 依赖。

## 代码改动
### tm_infra
- `RealTimeThreadedExecutor.hpp`：默认 Dedicated 改为 mutex/list 实现；Inline、选择器与公共契约保留。
- `LegacyDedicatedThreadExecutor.hpp`：初版为兼容别名；后续删除该头文件和安装项。
- `RealTimeMPSCQueue.hpp`：提取原始未池化队列，仅供 infra SharedPool 使用；加入安装清单。
- `RealTimeSharedPoolExecutor.hpp`：包含提取后的队列头文件。
- `DedicatedExecutorTest.cpp`（原 `LegacyExecutorTest.cpp`）：验证默认和显式 Environment 选择 Dedicated。
- `ThreadedHandlerTest.cpp`：分别验证默认历史 idle 节奏和 SharedPool 按需 idle。
### tm_basic
- `EnhancedDedicatedThreadedExecutor.hpp`：迁入原 MPSC 执行器，加入 Boost 缓存和 Environment component；加入安装清单。
- `test/enhanced_dedicated_executor_test/`：独立压力、异常和 executor 契约测试；复用既有 handler suite 覆盖完整图路径。
- `MoodycamelExecutorTest.cpp`：测试源码支持选择 Enhanced，更新默认后端的 idle 断言。
- `ExecutorBench.cpp` / `ExecutorLatencyBench.cpp`：新增 enhanced 引擎，dedicated 表示当前默认，legacy 引擎后续已删除；历史结果的引擎含义保持历史解释。
- profiling 脚本：重建历史未池化 Enhanced include override；独立计时/计数、五轮交替测量、保存命令与 SHA256。

## 测试与验证
所有构建先 `source build-common/build-env-setting.sh`，只构建任务相关目标，`ninja -j2`。安装仅写 worktree 私有 install，不覆盖工作区公共 install。

- infra Release、Debug：`meson test -C .../tm_infra/builddir_{release,debug} --no-rebuild` 均 **4/4 通过**，覆盖默认类型、predicate wait、SharedPool、time checker。
- basic Release：`enhanced_dedicated_executor_test`、`enhanced_handler_test`、`moodycamel_executor_test` **3/3 通过**。
- basic Debug：两项 Enhanced 测试 **2/2 通过**。
- Enhanced direct 测试：16 生产者 × 100000（覆盖八分片碰撞）；513 消息 × 1000 轮缓存周转/溢出；精确计数和每生产者 FIFO；move-only/非默认构造 Item；新分配与缓存节点构造异常；消费异常；销毁未消费消息；pending count、线程句柄、动态 idle、回调内 stop、stop 后拒收。
- handler 测试：Environment 选择、并发输入、串行回调、graph liftPure/liftPure2/liftMulti3、FireOnceOnly、动态 idle worker、CheckTime 和延迟冒烟；同一源码分别选择 Enhanced 与 Moodycamel，且各自比较默认执行器。
- GCC15 `-std=c++17 -O1 -g -pthread -fsanitize=thread`、`-fsanitize=address,undefined` 的 Enhanced direct 测试均通过，零 sanitizer 报告；[TSan 输出](node-recycling-profiling/enhanced-tsan.txt)、[ASan/UBSan 输出](node-recycling-profiling/enhanced-asan.txt)。ASan 使用 `detect_leaks=0`：测试的 detached Host 有意保留，不宣称完成泄漏检测。
- 私有前缀安装后，使用 installed include 单独编译 Enhanced 头文件成功。
- 已知编译警告来自原有 RealTimeApp signedness、旧测试和其他已有头文件，不是本轮新增错误。

### 性能与分配
历史基线是 infra `ff08700` 的原始逐消息分配 MPSC，**仅重命名为 basic Enhanced**，与候选使用同一最终 ExecutorBench 源码、`-O3 -std=c++17 -pthread -DEXECUTOR_BENCH_ENGINE=8`。计数二进制另加 hooks；不能用其 CPU 推断性能。通过 `prepare_unpooled_enhanced.py` 生成 include override，`run_enhanced_profile.py` 复测。运行期间无其他本任务编译或 sanitizer 压测。

```sh
python3 worktrees/realtime-threaded-executor/tm_basic/test/executor_bench/run_enhanced_profile.py \
  --baseline /tmp/mpsc-profile/baseline-normal \
  --baseline-counts /tmp/mpsc-profile/baseline-counts \
  --candidate /tmp/mpsc-profile/candidate-normal \
  --candidate-counts /tmp/mpsc-profile/candidate-counts \
  --output /tmp/mpsc-profile/final
```

producer 绑 CPU20–23、consumer 绑 CPU24–27（避开 SMT sibling），五轮交替、独立进程；全部 **120 条正确性通过**。[原始 JSONL](node-recycling-profiling/results.jsonl) 保存实际命令、二进制 SHA256、计数和分位数，[完整中位数](node-recycling-profiling/summary.md)。

| 场景 | CPU ns/item 基线→候选 | wall ns/item 基线→候选 | 分配次数（独立计数窗口） |
|---|---:|---:|---:|
| 单生产者，小消息吞吐 | 596→543（-8.9%） | 299→272 | 1000000→177 |
| 单生产者，256B payload 吞吐 | 876→620（-29.2%） | 605→311 | 1000000→23 |
| 四生产者，小消息吞吐 | 1099→1131（+2.9%） | 252→264 | 4000000→874664 |
| 四生产者，256B payload 吞吐 | 1180→1219（+3.3%） | 367→279 | 4000000→357799 |

闭环和稀疏输入预热后的计数窗口均为 **0 次节点分配**。四生产者大消息闭环 p50 2294→1605ns、p99 7162→2972ns；小消息闭环 p50 1495→1619ns、p99 3935→3031ns。稀疏输入 p50 775→930ns、p99 2034→1143ns。减少分配不等同于所有负载 CPU 或 p50 改善；保留四生产者 CPU 略升和部分 p50 回退，不能宣称普遍加速。极端尾延迟样本与共享机器波动仍限制结论，idle 总 CPU 为约35→37µs/300ms，不解释为有意义差异。

### 多跳延迟补测
同一最终 `ExecutorLatencyBench.cpp` 与 `-O3 -std=c++17 -pthread`，baseline/candidate 仅 Enhanced 头文件不同。CPU20 producer、CPU24–27 consumer，1/4 跳、spinWait=0/200µs，各五轮交替，每轮5000条。全部 **40 条正确性通过**，[命令、SHA256 与原始结果](node-recycling-profiling/multihop.jsonl)。

| 跳数 / spinWait | p50 ns 基线→候选 | p99 ns 基线→候选 |
|---|---:|---:|
| 1 / 0µs | 4862→4922 | 6008→5943 |
| 1 / 200µs | 813→725 | 1086→952 |
| 4 / 0µs | 50794→51622 | 60371→61804 |
| 4 / 200µs | 3856→4575 | 4713→5484 |

四跳自旋场景 p50 回退约19%、p99约16%，不能仅用单跳p99改善推断端到端延迟改善。复用缓存引入额外同步与节点访问，当前版本的确定收益是减少分配及部分吞吐场景改善，尚未消除多跳自旋场景回退；默认仍按用户指定使用 mutex/list，Enhanced 为显式 opt-in，不把这项代价隐藏在默认执行器中。没有因本轮测量修改自旋默认值或消息顺序。

## 兼容性与注意事项
用户明确授权切换默认实现与移动 MPSC 执行器。默认类型名、构造/stop/wakeIdle/threadHandle、Environment 选择和 Inline 不变。直接使用原 Dedicated 想继续走 MPSC 时须显式引入 basic Enhanced。后续已按用户要求删除 Legacy 类型、头文件及基准入口，不提供兼容；SharedPool 行为不变。默认重新采用历史周期 idle；Enhanced 继续按需 idle 和 spinWait。原有 detached 生命周期约束不变。

本轮不合并/推送：原任务第二阶段仍未整体验收，保留 feature worktree。tm_kit_reference 按既有约束待远程推送后同步；[现行 App 类型参考](../../../../../tm_kit_reference/tm_infra/01-app-types.md) 尚描述旧默认，应以本记录和源码为准。

## 相关文档
- [主记录](README.md)
- [先前开销分析](executor-overhead-optimization.md)
- [Legacy 等待修复](legacy-executor-baseline.md)
- [Enhanced 测试与用法](../../../tm_basic/test/enhanced_dedicated_executor_test/README.md)
- [Profiling 工具](../../../tm_basic/test/executor_bench/README.md)

## 提交与收尾
- tm_infra `0a9796c`：默认执行器回迁、Legacy 兼容别名、SharedPool 队列提取、默认行为回归和本记录/数据。
- tm_basic `cbfde30`：Enhanced 执行器、Boost 节点缓存、测试与 profiling 工具。
- 本文在代码提交后单独补记提交号。两仓库保留 feature 分支，未推送；主任务第二阶段整体验收及之后的 reference 同步仍按原任务安排。

## 后续：删除 Legacy 兼容入口（2026-11-07，完成）
用户明确要求无需保留 Legacy 向前兼容。删除 Legacy 别名头文件及安装项，专用回归测试改用 Dedicated 命名；基准工具删除 legacy 后端和 include-legacy 选项，历史测量数据保留原标签。此为用户授权的 API 删除，不改变 Dedicated/Enhanced 的运行算法，追加到原记录。

删除后 Enhanced 的独立 benchmark engine ID 为7（历史测量提交中的8仍保留在原始记录），README 与 Meson/C++ 分发同步更新。

验证（Release，GCC15，先 source 工作区环境）：
- `ninja -j2 -C .../tm_infra/builddir_release test/threaded_handler_test/dedicated_executor_test`，对应 `meson test --no-rebuild dedicated_executor_test` 1/1 通过。
- 重建 basic 的 generic/Dedicated/Enhanced/Enhanced counts/latency 五个基准目标成功，独立 Dedicated、Enhanced counts 冒烟正确。
- `run_profile.py --candidate-only --quick --repeats 1 ...` 37/37 正确；`run_latency.py --candidate-only --engines dedicated,enhanced --messages 20 --warmup 8 --hops 1 --intervals 0 --spins 0 --repeats 1 ...` 2/2 正确。此为工具回归，不作为新性能测量。
- 修改后的 Python runner 语法检查通过，源码与测试中无 LegacyDedicated 类型/头文件引用；私有 install 已同步，移除旧安装头文件。历史数据和历史说明未重写。

后续提交：tm_infra `a2357d0`（删除兼容 API、测试改名与记录），tm_basic `42d96ae`（删除基准兼容入口与说明）。仍保留 feature 分支，未推送。

## 后续效率优化（2026-11-07，完成）
比较缓存容量、空缓存快速路径，并结合此前共享环、Boost stack、分片 SPSC 试验评估；保持无界接收和跨生产者 FIFO。Moodycamel 已有独立适配器但不保证跨生产者 FIFO，不能无条件替代。先保存本轮基线，再实测筛选。

本轮初测（相同 GCC15 -O3、生产者CPU20–23/消费者CPU24–27、五轮交替）：容量8/32/128/512没有稳定整体收益，保留32。采用分片内批量取回8个空节点，摊薄 Boost 队列索引同步；批量缓冲由队列持有而非 TLS，生产者退出不会遗留节点；在已有 reading 保护下访问。缓存上限由256变为320节点，析构同时释放批量缓冲。初测单/四生产者 CPU/item 557→317、1116→846ns，完整验证进行中。

已核查 Boost bulk pop API 与本机实现；`read_available()` 要求单读取方，排除在 reading 保护外调用它的快速路径。其他方案：Boost MPMC stack（先前测量额外CAS较重）、直接Boost queue（任意Item类型限制）、Moodycamel（跨生产者FIFO不足）、扩大缓存（本轮无稳定收益）、批量消息提交（需要改变调用方，不适合透明替换）。本轮采用现有成熟库的 bulk 接口，不引入新依赖。

### 批量缓存最终验证与数据
基线为本轮前 tm_basic `42d96ae` 的逐节点 Boost pop；候选仅将空节点读取改为最多8个的 bulk pop 和受同一 reading 标志保护的分片暂存。消息入队/消费、FIFO、自旋预算和唤醒协议均不变。缓存节点读取次序改变不影响消息 FIFO。析构遍历暂存与Boost队列，节点所有权互斥。

Release：原 direct 与完整 handler 测试2/2通过；包含16生产者碰撞、生产者退出后继续使用同一队列、循环溢出回收、异常与析构、图节点、stop/idle/CheckTime。GCC15 `-O1 -g -fsanitize=thread` 和 `-fsanitize=address,undefined` 均通过；ASan仍使用detect_leaks=0，未宣称LeakSanitizer完成。输出见 [TSan](batch-recycling-profiling/tsan.txt)、[ASan/UBSan](batch-recycling-profiling/asan.txt)。

计时采用同一当前 ExecutorBench.cpp、GCC15 C++17 `-O3 -pthread -DEXECUTOR_BENCH_ENGINE=7`，分别通过 include override 编译基线与候选。counts独立加hooks。CPU20–23 producer、CPU24–27 consumer；五轮交替，测量期间无本任务构建/压力测试。命令：

```sh
python3 worktrees/realtime-threaded-executor/tm_basic/test/executor_bench/run_enhanced_profile.py \
  --baseline /tmp/enhanced-next/base/bench --baseline-counts /tmp/enhanced-next/base/counts \
  --candidate /tmp/enhanced-next/batch/bench --candidate-counts /tmp/enhanced-next/batch/counts \
  --output /tmp/enhanced-next/final
```

全部120条正确性通过；[JSONL（含命令与二进制SHA256）](batch-recycling-profiling/results.jsonl)、[完整中位数](batch-recycling-profiling/summary.md)。容量方案40次与批量初筛20次也全部正确，见 [容量](batch-recycling-profiling/capacity.json)、[批量初筛](batch-recycling-profiling/batch.json)。

| 吞吐场景 | CPU ns/item 基线→候选 | wall ns/item 基线→候选 |
|---|---:|---:|
| 单生产者，小消息 | 600→338（-44%） | 301→170 |
| 单生产者，256B payload | 665→452（-32%） | 333→227 |
| 四生产者，小消息 | 1144→815（-29%） | 267→196 |
| 四生产者，256B payload | 1225→898（-27%） | 287→212 |

闭环/稀疏计数窗口仍零节点分配；饱和场景分配量受队列积压与计数hook干扰，不把本轮次数和前一轮不同负载窗口直接比较。稀疏p50 889→696ns、p99 1135→1039ns；闭环单生产者大消息CPU 2698→2757ns，保留这项小幅回退。

多跳另测40次全部正确，命令在[原始结果](batch-recycling-profiling/multihop.json)中。1/4跳、spinWait=0/200µs、每轮5000消息、各五轮交替：四跳200µs p50 4509→4577ns、p99 5436→5486ns；四跳无自旋p50 51745→52201ns、p99 61111→62421ns。多跳没有稳定改善，本轮确定收益集中于吞吐与CPU成本，不能宣称解决了前一轮的多跳延迟问题。

本轮仅 basic 执行器头文件有行为改动；测试说明和两仓库索引同步更新，沿用现有feature分支，不推送。缓存上限增加64个空Node是此次吞吐收益的空间代价，无新增vcpkg依赖。第三方接口依据：[Boost SPSC bulk pop](https://www.boost.org/doc/libs/latest/boost/lockfree/spsc_queue.hpp)，同时核对本机Boost1.91源码。

本轮提交：tm_basic `11138a5`（批量回收优化与测试说明）；tm_infra `e642cf3`（方案比较、验证和原始数据）。代码与记录均已提交，私有install头文件已更新，未推送。

## 继续降低异步延迟（2026-11-07 至 2026-11-08，完成）
目标逼近 Inline 延迟，保留异步执行与串行回调契约。评估回收操作移出回调前关键路径、避免无请求时的 wakeRequested RMW、自旋观察已发布链接而非尚未链接的 head。保持原睡眠 seq_cst 握手，先用同工具测 Inline 和现有 Enhanced，再逐项消融。

消融进展：单独推迟回收、条件化wake清理、观察已发布链接及组合均完成1/4跳五轮测量。组合单跳p50约737→469ns、四跳4496→3765ns，Inline同工具约71–77ns。隔离Node.next与value缓存行单跳略退、四跳略好但增加节点内存，暂不采用。

进一步原型：将MPSC改为生产者CAS发布链表、消费者exchange批量接管后反转，证明批次内/批次间FIFO并通过TSan压力测试。它减少单生产者吞吐CPU（约328→182ns/item），但单跳p50约470→614ns，四生产者CPU也退化，故不作为低延迟默认方案。原型仅保留实验数据与说明，不进入最终头文件。

硬件布局进展：本机/sys确认CPU20/48为SMT同胞，24/52为另一物理核。同工具五轮配对：组合版独立核心单跳p50约510ns、SMT约147ns，Inline此前约71ns；单生产者饱和CPU/item约307→80ns。该收益属于线程布局，不归因于代码；SMT可能与真实业务争用执行单元，不作为默认绑定策略。拓扑试验首轮因为工具默认检查连续4个consumer CPU而拒绝狭窄affinity，改用显式 `--consumer-cpus` 列表后通过，没有把失败结果计入。

完整组合方案回归发现四生产者小消息CPU约924→1006ns/item（其他吞吐场景改善），因此追加按积压情况选择回收时机的原型：若next->next已经发布，则回调前归还旧哨兵，否则延后归还；只读取当前消息节点上的原子链接，不额外读取生产者head。正在与无条件延后方案配对验证，不因单跳收益忽略吞吐回退。

积压自适应原型五轮结果：四生产者小消息 CPU/item 基线828、无条件延后969、自适应821ns；四跳p50分别4629、3746、3580ns。单生产者小消息CPU321/296/298ns，四生产者大消息1064/921/1001ns。选择自适应版本，以较均衡的吞吐/延迟替代无条件延后。保持缓存容量和全局FIFO，回收时机提示即使读到旧值也只改变性能，不改变节点所有权；RAII仍负责异常路径。

自适应长窗口复核未保持初测收益：四生产者小消息CPU约811→1012ns/item，故不能凭短测定案。追加两组：自适应+控制/数据缓存行隔离，以及只改变唤醒/轮询而保持原回收时机。最终方案待此轮筛选，保留所有回退数据，不宣称自适应已稳定消除吞吐回退。

最终筛选改为保留原回收时机，只优化唤醒/轮询（control-local）。在原组合基础上，run中的idle判断先看已发布链接；有消息时省掉对producer head缓存行的再次读取，无链接时仍用原head判断覆盖发布窗口。独立五轮对照：单跳p50约618→480ns，四跳4587→4122ns；单生产者吞吐CPU342→254ns，四生产者802→822ns。自适应加缓存行隔离虽然四跳约3582ns，但四生产者CPU约869ns并增加节点内存，未采用。回收前/后与批次反转等试验不进入最终实现。

### 本轮最终实现与验证（2026-11-08）
最终采用 `control-local`，基线为 tm_basic `11138a5`。三项修改：
1. 自旋观察已发布的 tail->next，减少 head 已交换但链接尚未发布时的空 drain。
2. 没有显式 idle wake 时不做 wakeRequested.exchange，避免无意义地写入与 sleeping 共用的缓存行。
3. 自旋返回后的 idle 判断先检查已发布链接；已有消息时无需再读生产者 head 缓存行，无链接时仍读取 head 覆盖发布窗口。

原 seq_cst 睡眠握手保留；FIFO、异步上下文、串行回调、默认200µs自旋、缓存容量和回收时机均保持原契约。代码与测试仅改 basic，infra 为记录更新。新增零自旋/200µs两种配置下各20000次数据提交与20000次idle开关/唤醒并发，检查最终idle激活与stop。Release direct/handler测试2/2通过；最新TSan、ASan/UBSan均无报告，ASan仍明确关闭泄漏检测。私有安装前缀同步，未改公共install或第三方代码。

实验同时覆盖延后回收、按积压回收、控制/数据缓存行隔离、CAS链表批量接管反转、不同自旋判据、SMT布局和自旋预算。最终未采用增加节点内存或改变队列结构的原型。所有候选与基线的差异见 [variants.patch](latency-critical-path-profiling/variants.patch)，编译参数及SHA256见 [metadata.json](latency-critical-path-profiling/metadata.json)。基线头文件可用 `git show 11138a5:tm_kit/basic/EnhancedDedicatedThreadedExecutor.hpp` 重建；最终源码相对于 control-local 实验头文件仅格式与注释不同。

消融/筛选数据：[单项与组合](latency-critical-path-profiling/ablation.json)、[节点对齐](latency-critical-path-profiling/alignment.json)、[链表批次接管](latency-critical-path-profiling/snapshot-comparison.json)、[积压自适应](latency-critical-path-profiling/adaptive-comparison.json)、[只优化控制路径](latency-critical-path-profiling/control-comparison.json)、[保留早期head判据](latency-critical-path-profiling/early-comparison.json)。各轮输入、命令、正确性结果均在原始数据中；原型的局部优势不等于最终实现的收益。

### 最终性能结果与不确定性
采用现有两套工具，GCC15、C++17、`-O3 -pthread`，计数与计时分离。本任务测量期间无并发编译或sanitizer压力测试。

- pipeline工具（每轮10000条、五轮交替、producer CPU20、consumer CPU24–27，200µs自旋）：control-comparison中单跳p50 **618→480ns**、四跳 **4587→4122ns**。独立early-comparison复核为 **586→502ns**、四跳 **4444→4000ns**。不同轮有波动，单跳改善约14%–22%、四跳约10%，不挑选最小值作为保证。
- 完整微基准120条正确性通过：[结果](latency-critical-path-profiling/results.jsonl)、[五次中位数](latency-critical-path-profiling/summary.md)。单生产者小/大消息CPU/item **337→241ns / 436→329ns**；四生产者大消息 **1053→913ns**。闭环与稀疏计数窗口继续为零节点分配。该工具的消息结构及ack路径与pipeline不同，不能混合解释两套工具的p50。
- 四生产者小消息在五轮矩阵中曾显示明显回退，但逐次结果存在约800/1000ns两档，同一基线也如此。追加**30轮随机顺序、每轮两个同一基线二进制及一个候选，共90次**对照，全部正确。两个基线标签中位数分别1023和824ns，证明少量样本中位数会误导。合并基线平均CPU/item **934.8ns**，候选 **917.8ns**（-1.8%）；按轮成对bootstrap的95%区间约 **-7.5%至+4.0%**，本轮不能确认稳定提升或明显回退。原始数据：[90次对照](latency-critical-path-profiling/p4-noise-control.jsonl)、[均值/分位数/区间](latency-critical-path-profiling/p4-noise-summary.json)。区间是当前共享机器与此负载的统计结果，不是所有部署的性能承诺。保留前面所有回退结果，不据此追改算法。

### 与 Inline 的距离及 CPU 代价
最终版本按本机SMT拓扑CPU20/48复测，五轮交替；[完整结果](latency-critical-path-profiling/control-smt.json)。所有消息正确，保持两个线程的异步执行。

| 输入与配置 | Inline p50 / p99 | Enhanced p50 / p99 | 进程CPU时间 / wall时间 |
|---|---:|---:|---:|
| 连续闭环，Enhanced自旋200µs | 87 / 97ns | 224 / 297ns | Inline约1.00、Enhanced约1.86逻辑CPU |
| 闭环sleep间隔1ms，Enhanced自旋200µs | 194 / 222ns | 14270 / 16139ns | Inline约0.007、Enhanced约0.22逻辑CPU |
| 闭环sleep间隔1ms，Enhanced自旋2ms | 194 / 222ns | 206 / 375ns | Inline约0.007、Enhanced约1.00逻辑CPU |

在**SMT同胞+2ms自旋+此小消息负载**下，稀疏输入p50已接近Inline；连续闭环仍约2.6倍，不能宣称异步执行器普遍达到Inline延迟。百纳秒绝对值包含计时/缓存影响，早期轮次甚至出现稀疏Inline比持续自旋的Enhanced更慢，不能解释为异步执行本质更快。SMT共用执行资源，真实业务计算可能改变结论；有idle worker时spinWait还受idlePeriod限制。默认不强制绑核，也不提高默认自旋预算。

可通过已有 `ThreadedExecOptions::spinWait` 配置直接执行器；复现命令及拓扑核查说明已写入 [benchmark README](../../../tm_basic/test/executor_bench/README.md)。前期布局与预算数据另见 [placement](latency-critical-path-profiling/placement.json)、[sparse-budget](latency-critical-path-profiling/sparse-budget.json)，其 combined/adaptive 原型结果不冒充最终版本。

最终验证输出：[TSan](latency-critical-path-profiling/tsan.txt)、[ASan/UBSan](latency-critical-path-profiling/asan.txt)。保留feature分支，主任务第二阶段整体状态不变，reference仍待推送后同步。

复现工具：basic `test/executor_bench/run_contention_profile.py` 保存随机双基线实验并支持 `--summarize-only`。用记录中的90条数据复算，输出与已归档的 `p4-noise-summary.json` 完全一致；无需重新运行负载即可核查统计过程。Release测试输出另存 [tests.txt](latency-critical-path-profiling/tests.txt)。
