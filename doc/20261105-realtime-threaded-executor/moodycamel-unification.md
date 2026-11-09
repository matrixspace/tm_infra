# 合并moodycamel执行器

**任务开始时间：** 2026-11-09
**任务结束时间：** 2026-11-09
**状态：** 完成
**分支：** feature/realtime-threaded-executor

## 目标与方案
用户要求只保留MoodycamelThreadedExecutor，融合Enhanced的吞吐与延迟优化，仅依赖可用时编译。沿用256消息块、ConsumerToken、原位消费、八分片发布计数、精确睡眠握手与有界自旋；移除原BlockingConcurrentQueue+optional唤醒token路径。保留每生产者顺序，取消全局FIFO。删除Enhanced头文件/类型/组件/基准入口，不保留兼容别名。Meson自动探测可用头文件，相关测试与专用基准条件构建，公共头文件亦条件声明。

## 实现与兼容性
- 仅保留basic::MoodycamelThreadedExecutor<Item, Config=infra::DedicatedThreadExecutorConfig>和MoodycamelThreadedExecutorComponent；原一参数用法保持有效。迁入原Enhanced的ConsumerToken、BLOCK_SIZE=256、八分片seq_cst发布计数、原位赋值sink、零wake token和有界自旋。旧Moody的BlockingQueue/optional批次缓冲路径移除；bulk64试验在前轮已显示小消息/闭环不利，不保留两套后端。trackPendingCount现在沿用发布前pending计数，而非旧size_approx估计。
- 删除Enhanced公开头文件、类型、组件及测试目录。直接队列测试迁入moodycamel_executor_test/MoodycamelQueueTest.cpp；handler测试只编译一次，不再通过宏重复实例化。没有Enhanced兼容别名。
- bench只留moody入口（engine6）；删除enhanced入口（engine7）、专用目标及历史未池化生成脚本。profile runner改名run_moodycamel_profile.py；contention runner默认moody，--executor可显式用于历史二进制复现。历史记录中的旧CLI/脚本可以从当时commit恢复。
- 公共头文件用__has_include探测两种既有布局，未安装concurrentqueue时可包含但不声明执行器。Meson新增feature选项moodycamel_executor：auto默认探测，enabled要求头文件存在，disabled跳过相关目标并在本项目编译中定义DISABLE宏。公共头文件仍安装，供下游按自己的依赖可用性探测。通用bench/pipeline在依赖不可用时仍可编译，运行moody报unknown/unavailable。
- infra的Inline/Dedicated默认实现无改动。已同步worktree私有安装并删除旧Enhanced安装头；没有修改公共安装或第三方代码。

## 验证
GCC15、C++17，先source build-common/build-env-setting.sh，所有Ninja限制-j2。auto Release定向编译moodycamel_queue_test、moodycamel_executor_test、executor_bench_moody及_counts、executor_latency_bench；[两项测试2/2通过](moodycamel-unification-validation/release-tests.txt)。移植的direct套件包含不可赋值/非默认构造Item、异常/移动次数/析构、多生产者碰撞与退出、地址复用、纯发布唤醒、idle/wake/stop竞争；handler覆盖图节点和显式CheckTime。

最终更名后的direct套件以-O1 -g -pthread和-fsanitize=thread、-fsanitize=address,undefined分别编译运行，通过：[TSan](moodycamel-unification-validation/tsan.txt)、[ASan/UBSan](moodycamel-unification-validation/asan.txt)。ASan detect_leaks=0，因为测试保留detached host；GCC提示moodycamel内部fence不受TSan支持，[编译警告](moodycamel-unification-validation/tsan-build.txt)保留，不将无报告解释为内存序证明。算法与前轮验证版一致。

依赖禁用路径在独立builddir_optional_off配置，源目录显式传给meson setup，--buildtype=release、--prefix=<worktree>/install、--cmake-prefix-path=<vcpkg>/installed/x64-linux、-Dmoodycamel_executor=disabled；PKG_CONFIG_PATH先放私有install。定向编译Dedicated基准和pipeline成功，[Dedicated冒烟](moodycamel-unification-validation/off-smoke.txt)正确；[目标清单](moodycamel-unification-validation/target-lists.json)确认无moody/enhanced测试和专用目标，[两种通用入口均拒绝moody](moodycamel-unification-validation/disabled-rejections.json)。本机仍安装有依赖，此验证通过disabled走与探测缺失相同的编译分支，不宣称实际卸载第三方库。

## 性能确认
旧Moody基线为basic d85a362中的MoodycamelThreadedExecutor.hpp，当前相同ExecutorBench.cpp，以include override编译 `-std=c++17 -O3 -DNDEBUG -pthread -DEXECUTOR_BENCH_ENGINE=6`。候选为Meson Release相同engine6。生产者CPU20–23、消费者24–27，各5轮随机顺序，无并发构建/sanitizer；40次全部ok。原始命令、哈希与结果见[JSON](moodycamel-unification-validation/comparison.json)、[摘要](moodycamel-unification-validation/comparison.txt)、[脚本](moodycamel-unification-validation/compare.py)。

| 场景 | CPU ns/item 旧Moody→合并版 | wall ns/item |
|---|---:|---:|
| 单生产者小消息 | 398→277 | 199→152 |
| 四生产者小消息 | 719→155 | 150→69 |
| 四生产者256B payload | 758→365 | 158→125 |

单生产者闭环p50 614→509ns、p99 1007→698ns。相对于旧Moody，这组测量未见所测指标回退；相对于前轮Enhanced，算法与调优参数相同，沿用[完整吞吐/延迟取舍记录](unordered-enhanced.md)，不以本轮名称合并宣称新算法提升。跨生产者全局FIFO不保证，每生产者顺序保留；显式时间/版本检查仍可能拒绝重排数据。

未合并或推送，reference待推送后按既有流程同步。

代码提交：tm_basic `761595d`。
