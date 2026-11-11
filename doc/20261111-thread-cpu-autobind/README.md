# 节点线程自动绑核（CpuAutoBind）

**任务开始时间：** 2026-11-11  
**任务结束时间：** 2026-11-11  
**状态：** 完成（已合并 develop，未推送）；2026-11-11 追加 ThreadCpuLease  
**分支：** `feature/thread-cpu-autobind`（tm_infra、tm_basic，均从 develop 建立）

## 背景
[逐节点 spinWait 与绑核](../20261111-threaded-node-spin-wait-affinity/README.md) 已支持 `Cpus({...})` 手工绑核，但使用者要自己了解 taskset/numactl 限制、SMT 兄弟和 NUMA 布局。用户要求新增 `CpuAutoBind`：框架为忙线程（甚至 busy-loop）自动选择最优 CPU，考虑进程 taskset/numactl 设置与 SMT，异常情况必须报错不能默默处理；用 vcpkg 安装的 hwloc 获取拓扑。追加需求：支持预先配置避让列表，避开第三方库已显式绑定占用的 CPU。

## 需求确认（用户答复）
- 分层：infra 只放扩展点（保持零第三方依赖），hwloc 实现放 tm_basic，可选依赖。
- NUMA：默认集中在一个节点；可为单个节点显式指定 NUMA。
- 避让粒度：避让列表中的 PU 扩展到整个物理核。
- 特殊性：每个 NUMA 节点的第一个核（承载中断与内核杂务）也要避开，不只是全局 CPU0 所在的核。

## 方案
- infra：`ThreadedNodeOptions::CpuAutoBind()` / `CpuAutoBindOnNuma(numaNode)`；分配器接口 `IThreadCpuAllocator`（acquire / release / reserve）；Environment 组件 `ThreadCpuAllocatorComponent<A>`；`ExecHandlerCore::configureThread` 解析自动绑核，转成单个 CPU 交给执行器已有的 `configure`，并持有租约，析构时归还。
- basic：`HwlocThreadCpuAllocator`（进程级单例，`configure(HwlocCpuAllocatorConfig)`），以物理核为分配单位。
- 进程限制：可用 PU = `hwloc_get_cpubind(PROCESS)` ∩ allowed cpuset。NUMA 偏好：Linux 上 hwloc 不支持查询进程级 membind（实测 `get_thisproc_membind=0`，返回 -1），改为读取调用线程的 membind（numactl 设置会被所有线程继承），只有策略为 BIND 时才视为偏好节点（`numactl --membind/--preferred` 实测都报 pol=BIND、nodes=1；默认为 FIRSTTOUCH、nodes=0-1）。

## 代码改动（按文件列出）
- tm_infra `RealTimeThreadedExecutor.hpp`：`CpuAutoBindRequest`、`ThreadedNodeOptions::cpuAutoBind` 及链式接口 `CpuAutoBind()` / `CpuAutoBind(numaNode)`；分配器接口 `IThreadCpuAllocator`（`acquire` / `reserve` / `release(vector) noexcept`）；组件 `ThreadCpuAllocatorComponent<Access>`；SFINAE 检测 `HasThreadCpuAllocator<StateT>`。
  - 计划中的 `release(int)` 改为 `release(std::vector<int>) noexcept`，这样显式 `Cpus()` 的 reserve 也能用同一接口归还（重配置或节点析构时）。
- tm_infra `RealTimeApp.hpp` `ExecHandlerCore`：
  - `configureThread` 解析自动绑核。`Cpus` 与 `CpuAutoBind` 同时给出时抛 `std::invalid_argument`；Environment 没有分配器、节点无专用线程（Inline、SharedPool、无 `configure` 的执行器），或分配后 pin 失败时，均抛 `RealTimeAppException`。
  - 租约 `CpuLease` 声明在 `exec_` 之前，析构顺序保证线程 join 后才归还 CPU。重配置时先成功应用新设置，再释放旧租约。
  - Environment 带分配器时，显式 `Cpus()` 先 reserve，`configure` 失败则归还。
- tm_basic 新增 `tm_kit/basic/HwlocThreadCpuAllocator.hpp`（安装）：
  - `HwlocCpuAllocatorConfig`：`excludeCpus`（避让列表，按整个物理核生效）；`avoidFirstCoreOfNumaNode`（默认 true）；`SmtPolicy`（默认 `RequireFullCore`）。
  - `HwlocThreadCpuAllocator` 是进程级单例：`configure()` 必须在首次使用前调用，之后调用抛 `std::logic_error`；也可构造独立实例供测试使用。另有诊断接口 `allowedList()`、`usableCores()`。
  - `HwlocThreadCpuAllocatorComponent` 混入 Environment 后即可使用自动绑核。所有失败都抛 `CpuAutoBindError`，消息列出可用 PU、被排除的核、已分配和已预留的 CPU，以及 SMT 策略。
  - 用 `__has_include(<hwloc.h>)` 探测，可用 `TM_KIT_BASIC_DISABLE_HWLOC_CPU_ALLOCATOR` 关闭。头文件只做声明，不进入 libtm_kit_basic；下游包含它时需自行依赖 `hwloc`（pkg-config）。
- tm_basic `meson_options.txt`、`meson.build`：新增 feature 选项 `hwloc_cpu_allocator`（默认 auto）。找到 hwloc 时把它加入 `common_deps`；找不到时定义 DISABLE 宏，并跳过 `test/hwloc_cpu_allocator_test`。

### 分配算法
1. 可用 PU = `hwloc_get_cpubind(PROCESS)` ∩ `hwloc_topology_get_allowed_cpuset`。前者覆盖 taskset 和 numactl `--physcpubind` / `--cpunodebind`，后者覆盖 cgroup cpuset。
2. 以物理核（`HWLOC_OBJ_CORE`）为单位。以下核会被排除：
   - RequireFullCore 下，PU 不全在可用集合内的核；
   - `excludeCpus` 中任一 PU 所在的核；
   - 每个 NUMA 节点首个 PU（按完整拓扑判断，不受进程掩码影响）所在的核；
   - 已分配或已预留 PU 所在的核。
3. NUMA 选择：
   - 显式 `CpuAutoBindOnNuma(n)` 时只在节点 n 内分配；节点不存在或不可用时抛异常。
   - 否则按以下顺序选节点：已有自动分配、且仍有空闲核的节点（集中）；线程 membind 为 BIND 策略时指定的节点；剩余节点按空闲核数降序。
   - 同一节点内按核顺序选择，线程绑到该核第一个可用 PU，同核的兄弟 PU 不再分配。
4. 配置校验：`excludeCpus` 中不在进程可用集合、或不属于任何核的 CPU 号会抛异常，避免笔误被静默忽略。
5. 实现中的坑：最初把 hwloc 的核 logical index 直接用作内部数组 `cores_` 的下标，进程掩码窄于整机时会越界（numactl 约束测试中 libstdc++ 断言失败）。已改为直接使用 `cores_` 的下标。
### 追加：自写线程复用绑核（ThreadCpuLease）
用户询问自写 importer 线程或其他能拿到 Environment 的地方能否复用绑核，并要求：限定 NUMA 的接口使用独立名字；`bindCurrentThread` 系列保证多线程调用安全。
- tm_infra `RealTimeThreadedExecutor.hpp` 新增公开类 `ThreadCpuLease`（RAII，可移动不可拷贝），以及异常类型 `ThreadCpuBindError`：
  - `bindCurrentThread<Env>(who)`：自动选核；
  - `bindCurrentThreadOnNuma<Env>(numaNode, who)`：限定 NUMA 节点自动选核；
  - `bindCurrentThreadTo<Env>(cpus, who)`：手工指定 CPU，先向分配器 reserve，让自动分配避开；
  - 以上三个都有接收 `IThreadCpuAllocator &` 的重载，供只持有分配器的代码使用；
  - `adopt(allocator, cpus)` 接管已分配的 CPU（图节点内部使用），另有 `cpu()`、`cpus()`、`active()`、`reset()`、`swap()`。
  - 只作用于调用线程：在线程函数第一行调用即可，不需要线程句柄，也就没有句柄失效的问题。分配器不存在时 `static_assert` 编译失败；无可用核、CPU 列表为空或非法、pin 失败都抛异常。
- 多线程安全：
  - 分配器内部的 mutex 保证核的选择互斥；每个线程只改自己的亲和性。
  - 同一线程同时只能持有一个 bindCurrentThread 租约，第二次调用抛异常，避免一个线程悄悄占两个核。登记表由 mutex 保护。
  - 单个租约对象不支持两个线程同时操作（与标准库对象的约定一致），已写进头文件注释。
- 踩坑：最初用 `std::thread::id` 记录哪些线程持有租约。线程结束后 OS 会复用 id；如果租约被 move 出线程且仍存活，复用了该 id 的新线程会被误判为重复绑定（basic 的混合测试中出现）。已改为进程内从不复用的线程序号（`thread_local`，由原子计数器分配）。
- `ExecHandlerCore` 删除私有 `CpuLease`，改用 `ThreadCpuLease::adopt`，图节点与自写线程共用同一套归还逻辑。

以下命令都先 `source build-common/build-env-setting.sh`（GCC 15.2.1），用 `ninja -j2` 定向编译；infra 头文件只装到 worktree 私有前缀。
- tm_infra Release `threaded_node_options_test`：22 项 ALL PASSED，其中新增 14 项用假分配器覆盖扩展点：无分配器抛异常；Cpus+AutoBind 互斥；Inline 与无 configure 执行器抛异常且不占用 CPU；分配器异常透传；成功 pin 到分配的 CPU；重配置释放旧 CPU；节点 `delete` 后 CPU 归还；显式 Cpus 被 reserve，重配置释放，自动分配会避开。
- tm_infra Release 与 Debug 各 4/4 通过。
- ThreadCpuLease（infra，假分配器）：调用线程被 pin 到分配的 CPU；同一线程第二次绑定抛异常；线程函数结束后归还；异常退出也归还；`OnNuma(99)` 失败后同一线程仍可重新绑定；`bindCurrentThreadTo` 先 reserve、pin、结束后归还；空列表抛异常；move 后源租约失效、只归还一次；16 线程同时绑定，分到的 CPU 两两不同且都已 pin，结束后全部归还。
- ThreadCpuLease（basic，真实 hwloc 分配器）：1 个自动绑核的图节点加 8 个并发自写线程，全部落在不同物理核；线程结束后核可以再次分配。
- TSan：infra 与 basic 两个测试的 TSan 构建均 ALL PASSED，没有数据竞争报告。为此去掉了 infra 测试里故意泄漏节点、执行器和租约的写法：执行器现在会在析构时 join，测试可以正常销毁，TSan 也不再报线程泄漏。
- tm_basic Release `meson test`：8/8 通过，其中新增两项：
  - `hwloc_cpu_allocator_test`：
    - 26 次分配各占不同物理核，且都在进程掩码内；
    - 本机 2 个 NUMA 节点，PU 0/1 所在的首核被避开（28 核中可用 26 核）；
    - `excludeCpus` 去掉整核，兄弟 PU 同样避开；非法 CPU 号抛异常；
    - 显式 NUMA 1 只分配节点 1 的 CPU；节点 99 抛异常；
    - 耗尽时抛 `CpuAutoBindError`，消息完整；release 后可复用；
    - 预留的核不会被自动分配；预留已自动分配的 CPU 时抛异常；首次使用后 configure 抛异常；
    - 8 线程 × 200 轮并发 acquire/release，从未分配同一核；
    - 图集成：Dedicated 与 Moodycamel 节点各 pin 到单个 PU；`CpuAutoBindOnNuma(1)` 的节点线程落在节点 1 的 CPU 上，`CpuAutoBindOnNuma(99)` 抛异常；SharedPool 节点抛异常。
- 改名为 `CpuAutoBindOnNuma` 后重跑：tm_infra Release 4/4，tm_basic Release 8/8。
  - `hwloc_cpu_allocator_constraints`（`run_constraints.sh`）：
    - `taskset` 6 个整核（含全部 SMT 兄弟）时，6 个核全部可分配；
    - `taskset` 每核只给一个 PU 时，RequireFullCore 无可用核并抛异常；
    - `numactl --cpunodebind=1 --membind=1` 时只分配节点 1，共 13 核（节点 14 核减去首核）；
    - 无约束时 26 核可用。
- 依赖禁用：在独立 `builddir_hwloc_off` 中配置 `-Dhwloc_cpu_allocator=disabled`，无 hwloc 目标，其他目标正常编译。
- TSan：`hwloc_cpu_allocator_test` 以 `-O1 -fsanitize=thread` 编译运行 ALL PASSED，无 TSan 报告；`-Wtsan` 警告来自 moodycamel 头文件内已有的 fence。
- tm_basic Debug：7/8 通过，失败的是 `tsc_clock_test`（“TscClock::nowNs() is cheaper than system_clock::now()”）。develop 上的同一二进制连跑 3 次都失败，属于既有的 Debug 性能断言问题，与本任务无关；Release 下该测试通过。
## 合并（2026-11-11）
- tm_infra develop：`ff1c8ea`（--no-ff 合并 `feature/thread-cpu-autobind`，领先 origin/develop 4 个提交）。
- tm_basic develop：`10d4306`（同上，领先 4 个提交）。
- 按用户要求本轮未推送。
- 合并后在 develop 上复测：infra Release/Debug 均 4/4；basic Release 8/8，Debug 7/8（`tsc_clock_test` 为 develop 既有的性能断言失败）。
- infra 的 Release/Debug 头文件已安装到工作区 `install/`；tm_basic 的全量安装与 reference 同步未做，等推送后再进行。

## 兼容性与注意事项
- 命名：限定 NUMA 的接口最初叫 `CpuAutoBind(int numaNode)`，调用处 `CpuAutoBind(1)` 容易被读成“自动绑定到 CPU 1”。合并前按用户确认改为 `CpuAutoBindOnNuma(1)`，没有保留旧重载。参数是 NUMA 节点而不是 CPU：指定具体 CPU 用 `Cpus()`，自动模式下只约束挑选范围，具体的核由框架按 SMT、节点首核和避让列表选出。
- 只追加接口：已有 `Cpus()`、`SpinWait()` 的语义不变。`IThreadCpuAllocator`、`ThreadCpuAllocatorComponent` 是新的 infra 公开类型，infra 仍零第三方依赖。
- 与 `Cpus()` / `SpinWait()` 的“不支持就返回 false”不同，`CpuAutoBind()` 任何不可满足的情况都抛异常（用户要求“异常情况下报错，不要默默处理”）。
- 下游包含 `HwlocThreadCpuAllocator.hpp` 时需自行链接 hwloc；tm_basic 的 .pc 不要求 hwloc，未使用该功能的下游不受影响。
- 自动绑核与显式绑核共存时，显式 `Cpus()` 只有在 Environment 带分配器时才会被预留；没有分配器时，两种方式的冲突无法检测。
## 相关文档
- tm_kit_reference：`tm_infra/10-realtime-executors.md`「自动绑核」一节、`BUILD.md` 的 feature 选项（待合并推送后同步）
- [逐节点 spinWait 与绑核](../20261111-threaded-node-spin-wait-affinity/README.md)
- [实时执行器总记录](../20261105-realtime-threaded-executor/README.md)
