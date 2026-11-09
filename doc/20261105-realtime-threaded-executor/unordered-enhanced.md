# Enhanced取消全局FIFO后的优化

**任务开始时间：** 2026-11-09
**任务结束时间：** 2026-11-09
**状态：** 完成
**分支：** feature/realtime-threaded-executor

## 方案
基线basic 7edf028。用户授权取消全局FIFO，允许moodycamel；比较分块ConcurrentQueue替代每消息节点回收。使用consumer token与赋值适配器原位消费，支持非默认构造/不可赋值Item。睡眠需独立精确发布计数，不能依赖size_approx的非线性化空判断。先评估单项出队，再考虑bulk摊薄。

## 筛选进展
用户确认吞吐大幅提升时接受单生产者延迟稍增。采用分片发布计数+consumer token逐项原位消费；bulk64对小消息CPU/延迟不利，未选。单发布计数形成跨生产者写热点；八分片计数明显改善多生产者吞吐。fence+size_approx原型仅为归因对照，不作为最终睡眠协议。最终sleep扫描seq_cst发布计数，active轮询size_approx只作提示。

## 块大小与内存代价
比较BLOCK_SIZE=32/128/256，各5轮随机顺序。256下四生产者小消息CPU/item约152ns、wall65ns；大消息CPU363ns、wall125ns，优于32的218/84与569/190。采用256；部分填满块的保留空间变大，队列内存受生产者数量、消息大小及峰值积压影响，没有固定空节点数量上限。单生产者大消息回退需完整矩阵确认，不隐藏。

## 睡眠协议与异常
每次enqueue成功后在8个缓存行隔离计数之一做seq_cst增量，然后seq_cst读取sleeping。消费者设置sleeping=true后扫描所有计数，与仅消费者写的consumed比较：若生产者看到false，计数更新在seq_cst顺序中先于该扫描；否则生产者持通知mutex唤醒。tryConsume可先于计数更新，此时不相等保守保持唤醒，直到该push完成。不依赖size_approx决定睡眠。
原位赋值sink先增加consumed再调用回调；回调异常由moodycamel出队异常保障销毁对象。enqueue失败抛bad_alloc，构造异常传播；trackPendingCount在失败时回滚。TLS仅保留整数分片号；producer注册与块所有权交由库管理。

第三方接口依据：[官方README](https://github.com/cameron314/concurrentqueue/blob/master/README.md)，通过Context7查询并核对已安装concurrentqueue.h的异常守卫、try_dequeue输出类型与BLOCK_SIZE traits。不修改第三方库，无新增vcpkg安装。

## 最终改动
- basic `EnhancedDedicatedThreadedExecutor.hpp`：Boost空节点缓存/Vyukov消息队列替换为moodycamel ConcurrentQueue，BLOCK_SIZE=256；一个ConsumerToken、八分片发布计数，原位消费保留。失败入队回滚pending。公共类名、Config、自旋预算、专用线程、stop/idle契约保留。
- direct测试：名称更新为块复用；增加8个队列生命周期×32轮生产者退出重建，以及0/200µs自旋各1000次纯数据唤醒睡眠测试。原16生产者碰撞、异常/生命周期/移动次数、wake竞争和handler集成测试继续通过。
- README、两仓库索引更新；infra仅更新记录，无源码行为变更。依赖使用已安装concurrentqueue，原Enhanced头文件不再依赖Boost回收队列。

## 最终性能（GCC15 Release，五次交替中位数）
基线basic `7edf028`，同一当前基准源码，仅覆盖Enhanced头文件；生产者CPU20–23、消费者24–27，计数/计时分离。完整矩阵120条、pipeline40条全部ok。

| 吞吐场景 | CPU ns/item 前→后 | wall ns/item 前→后 |
|---|---:|---:|
| 单生产者小消息 | 254→278 | 127→152 |
| 单生产者256B payload | 315→375 | 158→195 |
| 四生产者小消息 | 1031→147 | 227→66 |
| 四生产者256B payload | 1073→417 | 246→137 |

四生产者小/大消息吞吐约3.4/1.8倍，CPU成本约降低86%/61%。共享机器和队列积压影响绝对值；早期五轮小消息基线约826而非1031，已有任务记录说明基线双峰噪声，但当前候选约147–176仍有明显优势。单生产者吞吐有回退，不宣称全面提速。

直接闭环单生产者小/大消息p50为347→452ns、440→519ns。四生产者闭环p50为1324→1477ns、1295→1399ns；稀疏653→956ns。大消息单生产者p99为745→3566ns，四生产者大消息2561→3203ns；这些尾部回退保留，不据少量样本宣称延迟保证。用户已明确允许吞吐大幅提升时单生产者延迟稍增，仍需真实业务负载评估尾部。

另一个pipeline工具（消息结构/ack不同，不混合绝对值）：200µs自旋单跳p50 630→470ns，四跳4034→3290ns；零自旋单跳4840→3083ns，四跳52093→50530ns。该工具上没有复现直接闭环的p50回退，不能据此删除前一工具的结果。

闭环/稀疏测量窗口仍零节点/块分配；饱和分配与积压相关，小/大消息单生产者分配次数23→3649、115→3276，四生产者26254→15256、3337→15317。生产者更快可形成更多积压，因此这些数值不是同等队列长度下的分配效率比较。内存不再有320个空节点的限制；256项块也放大部分填满块空间。idle32节点300ms进程CPU约35.6→37.2µs。

原始数据：[完整矩阵](unordered-enhanced-profiling/results.jsonl)、[摘要](unordered-enhanced-profiling/summary.md)、[多跳](unordered-enhanced-profiling/pipeline256.json)、[编译信息与二进制哈希](unordered-enhanced-profiling/metadata.json)。[原型差异](unordered-enhanced-profiling/variants.patch)、[最初对照](unordered-enhanced-profiling/comparison.json)、[无token/fence归因](unordered-enhanced-profiling/followup.json)、[分片对照](unordered-enhanced-profiling/sharded-comparison.json)、[块大小](unordered-enhanced-profiling/block-comparison.json)均保留。纯fence原型没有采用。

复现完整矩阵使用 basic `test/executor_bench/run_enhanced_profile.py`，参数 `--baseline /tmp/enhanced-unordered/base/bench --baseline-counts /tmp/enhanced-unordered/base/counts --candidate <worktree>/tm_basic/builddir_release/test/executor_bench/executor_bench_enhanced --candidate-counts <同目录>/executor_bench_enhanced_counts --output <输出目录>`。各轮完整命令与哈希在JSONL中；baseline可从7edf028恢复头文件后通过include override编译，flags为 `-std=c++17 -O3 -pthread -DEXECUTOR_BENCH_ENGINE=7`；counts另加 `-DEXECUTOR_BENCH_COUNTS=1 -Wl,--wrap=malloc -Wl,--wrap=free`。

## 测试与安装
source build-common/build-env-setting.sh后，worktree Release定向ninja -j2编译direct/handler/两种enhanced基准与pipeline；meson test --no-rebuild direct/handler **2/2通过**，见[输出](unordered-enhanced-profiling/release-tests.txt)。direct以GCC15 `-std=c++17 -O1 -g -pthread -fsanitize=thread`及 `-fsanitize=address,undefined` 编译并运行均通过：[TSan](unordered-enhanced-profiling/tsan.txt)、[ASan/UBSan](unordered-enhanced-profiling/asan.txt)。ASan detect_leaks=0，因为测试有意保留detach host；不宣称LeakSanitizer完成。GCC对moodycamel内部fence发出TSan不支持警告，见[编译输出](unordered-enhanced-profiling/tsan-build.txt)；无运行报告不等于证明第三方内存序，睡眠协议论证见上。

已同步worktree私有install；未合并/推送。reference待推送后按流程同步。

本轮代码提交：tm_basic `d85a362`。
