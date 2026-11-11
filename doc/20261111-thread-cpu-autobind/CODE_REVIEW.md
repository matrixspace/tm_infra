# CpuAutoBind 代码评审报告

**记录日期：** 2026-11-11  
**来源：** 用户提供的 reviewer model 完整评审输出  
**状态：** 已修复并验证（2026-11-11）

## 评审结论

常规 Release 定向测试通过，但边界验证确认了死锁、物理核隔离失效、初始化状态累积及可选依赖构建失败等问题。评审基于现有 origin/develop；远程刷新因系统 SSH 配置权限错误失败。

## 评审意见

### 1. [P1] 避免在持锁的 reserve 中再次调用 allowedList

**位置：** `tm_basic/tm_kit/basic/HwlocThreadCpuAllocator.hpp:125-126`

当预留的 CPU 不在进程可用集合中时，`reserve()` 已持有 `mutex_`，错误消息中的 `allowedList()` 又尝试锁定同一个非递归 mutex，导致永久死锁而不是抛异常，并阻塞后续所有分配和释放。已用 `reserve({1000}, ...)` 复现超时；这里应直接读取锁内已有的 `allowedString_`。

### 2. [P2] 按物理核检查显式预留与自动分配的冲突

**位置：** `tm_basic/tm_kit/basic/HwlocThreadCpuAllocator.hpp:128-131`

当自动分配先占用某个 PU，随后显式预留其 SMT 兄弟 PU 时，这里只查找相同 CPU 编号，因而允许两个忙线程共享同一物理核。已复现自动分配 CPU 0 后，预留其兄弟 CPU 28 仍成功；反过来先预留再自动分配则会避开整核。应对所属物理核的全部 PU 检查自动分配状态，使隔离保证不依赖配置顺序。

### 3. [P2] 加载包含受限 PU 的完整拓扑再判断整核可用性

**位置：** `tm_basic/tm_kit/basic/HwlocThreadCpuAllocator.hpp:194-197`

当 cgroup/cpuset 只允许每个物理核的一部分 SMT 线程时，hwloc 默认会从拓扑中移除不允许的 PU，因此后面的 `core->cpuset` 已不包含这些兄弟线程，`info.full` 会错误地为 true，违反 `RequireFullCore` 的隔离语义。使用限制 allowed cpuset 的真实拓扑 XML 已复现部分核被接受。应在加载前设置 `HWLOC_TOPOLOGY_FLAG_INCLUDE_DISALLOWED`，再与 allowed cpuset 求交判断可用性。

### 4. [P2] 在拓扑初始化失败时回滚已构建的状态

**位置：** `tm_basic/tm_kit/basic/HwlocThreadCpuAllocator.hpp:202-203`

当 `buildModel()` 因非法 `excludeCpus` 抛异常时，`loaded_` 仍为 false，但已创建的拓扑、bitmap 和 `cores_` 被保留；下一次调用会覆盖资源指针并继续追加核记录。已复现配置合法 CPU 加非法 CPU、捕获异常、清空排除配置后重试，28 个物理核被 `usableCores()` 报告为 55 个。应在失败时释放资源并清空部分状态，或先在临时对象中构建成功后再提交。

### 5. [P2] 根据 moodycamel 可用性保护对应测试代码

**位置：** `tm_basic/test/hwloc_cpu_allocator_test/HwlocCpuAllocatorTest.cpp:297`

当 hwloc 可用但设置 `-Dmoodycamel_executor=disabled`，或未安装 moodycamel 头文件时，新测试仍会参与构建，而 `MoodycamelThreadedExecutorComponent` 不存在，导致整个构建失败。已通过定义禁用宏进行语法检查复现。应同时保护 `MoodyEnv` 定义及其测试调用，同时保留不依赖 moodycamel 的 hwloc 测试。

### 6. [P2] 按可用物理核数量确定租约测试的线程数

**位置：** `tm_basic/test/hwloc_cpu_allocator_test/HwlocCpuAllocatorTest.cpp:392-395`

当可用物理核不足 9 个时，图节点先占用一个核，再同时创建固定 8 个持有租约的线程必然耗尽分配器；线程函数没有捕获该异常，会触发 `std::terminate`。已在 `taskset -c 2-5,30-33` 的四整核限制下复现退出码 134。应按剩余可用核数确定并发度，资源不足时跳过对应测试，而不是让正常的容量限制导致测试进程崩溃。

### 7. [P2] 仅使用具有多个 PU 的物理核构造部分核测试

**位置：** `tm_basic/test/hwloc_cpu_allocator_test/run_constraints.sh:75-79`

在没有 SMT 或已禁用 SMT 的机器上，每核选择一个 PU 实际上仍选择了完整物理核，但脚本无条件执行 `partial` 模式并要求分配器拒绝这些核，因此正确实现也会使默认测试失败。应检查所选核确实具有多个 PU，只从这样的核构造部分掩码；没有合适的核时跳过该场景。

## 相关文档

- [任务开发记录](README.md)

## 修复记录（2026-11-11）

全部 7 项已在 develop 上修复并逐项复现验证。

| # | 修复方式 |
|---|---|
| 1 | `reserve()` 的错误消息改用锁内已有的 `allowedString_`，不再调用 `allowedList()`。`reserve({100000})` 现在立即抛 `CpuAutoBindError`。 |
| 2 | 新增 `corePus(cpu)`，`reserve()` 检查待预留 CPU 所属物理核的**全部** PU 是否已被自动分配。自动分配 CPU 2 后预留其兄弟 30 会报错，信息指出两个 CPU 与占用者。 |
| 3 | `hwloc_topology_set_flags(HWLOC_TOPOLOGY_FLAG_INCLUDE_DISALLOWED)` 后再 `load`，使 `core->cpuset` 含被 cgroup/taskset 禁止的兄弟 PU，`info.full` 与 `RequireFullCore` 语义恢复正确。 |
| 4 | `ensureLoaded()` 用 try/catch 包住 `buildModel()`，失败时 `discardModel()` 释放 topology、bitmap 并清空 `cores_`/`pusAllowed_`/`allowedString_`；析构也复用 `discardModel()`。非法配置重试不再累积核记录。 |
| 5 | 测试用 `#ifdef TM_KIT_BASIC_HAS_MOODYCAMEL_THREADED_EXECUTOR` 保护 moodycamel 头文件、`MoodyEnv` 定义和图用例；缺失时打印 skip。以 `-DTM_KIT_BASIC_DISABLE_MOODYCAMEL_THREADED_EXECUTOR=1` 做过语法检查。 |
| 6 | 并发线程数改为 `min(8, usableCores() - 1)`（节点已占一个核），不足 3 个可用核时跳过；线程内异常不再导致 terminate。 |
| 7 | `run_constraints.sh` 只从**确有多个 PU** 的核构造部分掩码；没有 SMT 兄弟对时跳过该场景并说明原因。 |

验证结果：tm_infra Release/Debug 4/4，tm_basic Release 8/8；`taskset -c 2,30,3,31,4,32,5,33`（四整核）与约束脚本三种场景全部通过；每核仅暴露一个 PU 的掩码下 `usableCores()==0`（RequireFullCore 正确拒绝）。

注：修复在 `develop` 工作树进行；`worktrees/realtime-threaded-executor/` 下的 feature 分支 checkout 未同步，其构建目录不包含这些修复。
