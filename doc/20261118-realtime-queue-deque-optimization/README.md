# 实时模式队列聚合节点优化：list→deque + 条件 notify

**任务开始时间：** 2026-11-18
**任务结束时间：** 2026-11-18
**状态：** 完成
**分支：** 三个 feature 分支并行开发后合并回 `develop`（见下）

## 背景

`tm_basic` 的 [bunch-throughput](../../tm_basic/doc/20261117-bunch-throughput/README.md) 任务为自建线程
聚合节点 `BunchAction` 提炼出四个不改语义、可显著提升吞吐的优化手法：

1. **M1** 队列容器 `std::list` → `std::deque`（块式分配省逐项 malloc/free，缓存局部性好，引用稳定，
   天然支持 move-only / 含 `const` 成员的 `InnerData<T>`）；
2. **M2** 消费侧 `processing.splice(end, incoming)` → `processing.swap(incoming)`（O(1) 不分配；
   前提是上一轮已清空 `processing`）；
3. **M3** drain 的 `while(!empty){front();pop_front();}` → `range-for + clear()`（`std::deque::pop_front()`
   逐个释放块的簿记开销远高于 `clear()`）；
4. **M4** 生产侧无条件 `notify_one()` → 仅队列空→非空（或计数 0→非 0）时 notify（省忙时逐项 futex）。

经排查，tm_infra 实时模式（RealTimeApp / RealTimeThreadedExecutor）有三个组件存在完全相同的 per-item
固定开销，是这些手法的直接适用对象（均为高频路径）。本任务分三个 feature 分支在独立 worktree 中并行
开发、分别测试，最终合并回 develop。

## 方案

三个组件互不重叠、分别在独立 worktree/branch 开发：

| feature 分支 | 组件 | 文件 | 手法 | 合并 commit |
|---|---|---|---|---|
| `feature/executor-deque-optimization` | `DedicatedThreadExecutor<Item>`（默认执行器） | `tm_kit/infra/RealTimeThreadedExecutor.hpp` | M1+M2+M3+M4 | `f3f1c46` |
| `feature/trigger-importer-deque-optimization` | `triggerImporter<HasOwnThread=true>` | `tm_kit/infra/RealTimeApp.hpp` | M1+M2+M3+M4 | `0055508` |
| `feature/bunched-const-importer-optimization` | `BunchedImporter`（M1+M2+M4）与 `constTriggerImporter<HasOwnThread=true>`（M4） | `tm_kit/infra/RealTimeApp.hpp` | M1+M2+M4 / M4 | `18763a2` |

各手法的安全性与语义不变论证同 bunch-throughput：

- **M1** 选 deque 而非 vector：`InnerData<T> = TimedDataWithEnvironment<T,StateT,TimePoint>` 含
  `Environment * const` 成员（`WithTimeData.hpp:448`），move 非 noexcept，vector 扩容会退化为逐元素
  拷贝、move-only 直接编译失败；deque 的 `push_back`/`clear` 不移动已存在元素，安全。三个节点均以此
  为由选 deque（`triggerImporter` 的 `T` 是裸值，同样安全）。
- **M2** 的 `swap` 语义等价于 `splice` 仅在 `processing` 为空时成立；上述节点的 drain 都会在进入下一轮
  wait 前清空 `processing`，不变式成立。
- **M4** 的丢失唤醒论证：首个使队列由空变非空（或计数由 0 变非 0）的生产者必 notify，其后到达者看到
  非空即不 notify，消费者谓词在下次检查时必为真，无丢失唤醒、无活锁；锁外 notify 避免
  hurry-up-and-wait。对 `DedicatedThreadExecutor` 的 `wait_for(lock, idlePeriod, pred)` + `wakeRequested`
  同样成立（空闲回调走独立 `wakeIdle()` 无条件 notify，不参与批量唤醒）。

## 代码改动（按文件列出）

- `tm_kit/infra/RealTimeThreadedExecutor.hpp`（`DedicatedThreadExecutor`，默认执行器）：
  `std::list<Item>` → `std::deque<Item>`；`splice` → `swap`；drain `while(front/pop_front)` →
  `for(auto &item){...} processing.clear()`（循环内 `running` 提前 return 语义保持不变）；
  `push` 锁内取 `wasEmpty`、锁外 `if (wasEmpty) notify_one()`；补充 `#include <deque>`。
- `tm_kit/infra/RealTimeApp.hpp`：
  - `triggerImporter<HasOwnThread=true>`：M1+M2+M3+M4 全部应用（结构与原 BunchAction 同构）。
  - `BunchedImporter`：M1+M2+M4（drain 的 range-for+clear 已是目标形态，未动）。
  - `constTriggerImporter<HasOwnThread=true>`：M4（`count_` 由 0→非 0 才 notify）。
- `test/meson.build`：新增 `subdir('executor_throughput_bench')`、`subdir('trigger_importer_test')`、
  `subdir('bunched_importer_test')`、`subdir('const_trigger_importer_test')`。
- 新增测试/基准：
  - `test/executor_throughput_bench/`（吞吐基准，不注册为 test）
  - `test/trigger_importer_test/`（功能测试 + 基准）
  - `test/bunched_importer_test/`（功能测试 + 基准）
  - `test/const_trigger_importer_test/`（功能测试 + 基准）

## 测试与验证

构建环境：`source build-common/build-env-setting.sh`（gcc 15.2.1 / gcc-toolset-15），release 构建，
`ninja -j4`。各 feature 分支在独立 worktree 的 `builddir_release` 中构建与测试；合并后对 develop 再
做一次干净构建 + 全量测试。

- 合并后 develop 干净 release 构建：31/31 目标成功（仅既有模板告警）。
- `meson test -C builddir_release`：**9/9 PASS**（6 个既有 + 3 个新增：
  `const_trigger_importer_test`、`trigger_importer_test`、`bunched_importer_test`）。
- 功能测试覆盖：单生产者 FIFO 顺序（seq 无丢失/重复）、多生产者并发各 25k 条无丢失/重复、
  bunched 聚合总项数 == N 且顺序正确、final 批次标记、const 触发 N 次收到恰 N 条值正确。

**吞吐基准（wall ns/item，N=5,000,000，预热 100,000，taskset 绑空闲核，5 次取中位数；共享机噪声大，
以中位数+范围为准）：**

| 组件 | 优化前（中位） | 优化后（中位） | 提升 |
|---|---|---|---|
| `DedicatedThreadExecutor`（64B 载荷） | 437.5（422.3–542.5） | 131.0（105.3–169.8） | ~3.3× |
| `triggerImporter` | 616.6（554.8–646.1） | 97.5（91.1–111.4） | ~6.3× |
| `constTriggerImporter` | 244（208–312） | 154（107–323） | ~-37% |
| `bunchedImporter` | 833（711–1074） | 650（391–1065） | ~-22% |

const/bunched 两项提升幅度相对小且结果呈双峰（优化后快跑 391/402 已低于优化前最小值 711，慢跑
~1050 与共享机干扰有关），主要收益来自默认执行器与 `triggerImporter`。

## 兼容性与注意事项

- 对外语义不变：聚合周期、时间检查、finalFlag、绑核路径均不受影响。
- `DedicatedThreadExecutor` 的 `executorBeforeBatch(front(), size())` 在 deque 下照常成立。
- 三个分支都只改各自组件区域，`RealTimeApp.hpp` 的 `triggerImporter` 与 `bunchedImporter`/
  `constTriggerImporter` 区域不重叠，合并仅 `test/meson.build` 有冲突（保留全部 subdir 行解决）。

## 相关文档

- [bunch / bunchWithProcessing 吞吐优化](../../tm_basic/doc/20261117-bunch-throughput/README.md)（手法来源）
- [修复条件变量无谓词 1ms 轮询等待](../../tm_basic/doc/20261117-fix-cv-1ms-poll/README.md)（上游 wait 改造）
- [realtime-threaded-executor 执行器重构](../20261105-realtime-threaded-executor/README.md)（默认执行器演进背景）

## commit

三个 feature 分支分别提交，再各自 `--no-ff` 合并回 develop：

- `43ef935` `feature/executor-deque-optimization` → 合并 `f3f1c46`
- `845348f` `feature/trigger-importer-deque-optimization` → 合并 `0055508`
- `23b5f85` `feature/bunched-const-importer-optimization` → 合并 `18763a2`
- 本文档 + INDEX 由 leader 单独一个 `doc:` commit 提交。
