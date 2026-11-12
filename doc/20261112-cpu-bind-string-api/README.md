# 统一的线程绑核字符串接口

**任务开始时间：** 2026-11-12  
**任务结束时间：** 2026-11-12  
**状态：** 完成  
**分支：** `feature/cpu-bind-string-api`（commit 待填）

## 背景

tm_infra 现有的线程绑核入口分散在几个互不相干的 API 上，调用者要根据场景挑选不同的方法：

- 节点级：`M::configureThreadedNode(node, ThreadedNodeOptions().CpuAutoBind() | .CpuAutoBindOnNuma(n) | .Cpus({...}))`，三种策略各是一个独立的方法名，且 `Cpus()` 与 `CpuAutoBind*` 互斥（运行期抛 `std::invalid_argument`）。
- 自建线程：`infra::ThreadCpuLease::bindCurrentThread<Env>` / `bindCurrentThreadOnNuma<Env>` / `bindCurrentThreadTo<Env>`，同样三个独立入口。
- 配置层：`basic::HwlocCpuAllocatorConfig` 用结构体字段，`basic::SharedThreadPoolConfig::cpus` 用 `std::vector<int>`，外部（环境变量、配置文件、命令行）要拿到的是一个字符串，得各自再写一遍解析。

tm_transport 的 aeron_ipc 组件已经在 `AeronIpcComponent.cpp` 里写过一个本地版 `parseCpuList`（支持 `"3"`、`"2,3"`、`"2-3"`、`"2:3"`），说明这个需求是真实存在的，只是现在每个使用者各写一份。

目标：在 tm_infra 提供一个零依赖的字符串解析器，用一个统一语法的字符串表达全部绑核策略，并把四个入口（节点 `configureThreadedNode`、自建线程 `ThreadCpuLease`、hwloc 分配器配置、SharedPool cpus）都接到它上面。

## 方案（含被放弃的方案和原因）

### 语法（已与用户确认）

关键字 + 冒号，大小写不敏感、允许空白：

| 字符串 | 语义 |
|---|---|
| `""`（空串）或 `"none"` | 不绑定，保持现有亲和性不变（不调用执行器 configure、不归还已有租约） |
| `"auto"` | `CpuAutoBind()`，由分配器在全机范围内挑核 |
| `"numa:1"` | `CpuAutoBindOnNuma(1)`，限定在指定 NUMA 节点内挑核 |
| `"cpus:2,3,8-11"` | `Cpus({2,3,8,9,10,11})`，显式指定 CPU 列表 |
| `"8-11"` | 简写，纯数字列表（含 `-`、`,`、`:`）视同 `cpus:...` |

非法串抛 `std::invalid_argument`，带诊断信息（哪一段解析失败）。

被放弃的方案：
- 函数调用式 `auto(numa=1)`：可扩展性好但与 aeron 已有的 `parseCpuList` 风格不一致，且对最常用的 `auto`/`none` 引入冗余括号。
- `key=value`（`bind=auto;numa=1`）：和 `ConnectionLocator` 的查询串风格一致，但绑核策略本身只有一个主语义，没必要拆成多 key。

### `none` 的语义（已与用户确认）

`none` 表示"不动"：不改变线程当前的亲和性，也不归还分配器里已有的租约。这与现有空 `cpus` 行为一致，完全向后兼容。放弃"解绑并归还"是因为它要改执行器 `configure` 路径、新增一个"清除亲和性"分支，属于新行为而不是封装现有行为。

### 覆盖入口（已与用户确认）

1. 节点级 `configureThreadedNode`：`ThreadedNodeOptions::CpuBind("...")` 链式方法 + `ThreadedNodeOptions::fromCpuBindString` 工厂。
2. 自建线程 `ThreadCpuLease`：`ThreadCpuLease::bindCurrentThreadBy<Env>("...", who)`，`none` 时返回非激活的租约。
3. hwloc 分配器配置：`HwlocCpuAllocatorConfig::fromString("exclude=8,9;smt=partial;scope=cross;lockdir=...")`（tm_basic）。
4. SharedPool cpus：`SharedThreadPoolConfig::cpusFromString("2-5")`（tm_basic）。

### 分层

解析器放在 tm_infra（零依赖），导出一个公共类型 `CpuBindingSpec`（枚举 + CPU 列表）和一个 `parseCpuBinding(std::string)` 函数。tm_infra 的两个入口直接用它；tm_basic 的两个入口复用 CPU 列表解析逻辑（`parseCpuList` 也提到 tm_infra 作为公共工具），配置串解析在 tm_basic 本地写。

## 代码改动（按文件列出）

### tm_infra

- `tm_kit/infra/RealTimeThreadedExecutor.hpp`
  - 新增 `parseCpuList(std::string const &)`：解析 `"3"`、`"2,3"`、`"2-5"`、`"2:5"` 格式的 CPU 列表，空白忽略，空串返回空 vector，`lo>hi` 和非数字抛 `std::invalid_argument`。放在 `infra` 命名空间，零依赖，供 tm_basic 复用。
  - 新增 `enum class CpuBindKind { None, Auto, AutoOnNuma, Cpus }` 和 `struct CpuBindingSpec`（含 `kind`、`numaNode`、`cpus` 字段及静态 `parse()` 方法）：解析统一语法字符串 `""`/`"none"`/`"auto"`/`"numa:1"`/`"cpus:2,3,8-11"`/纯数字简写，大小写不敏感、空白 trim，非法串抛 `std::invalid_argument`。
  - `ThreadedNodeOptions` 新增链式方法 `CpuBind(std::string const &)` 和静态工厂 `fromCpuBindString(std::string const &)`，内部 dispatch 到现有 `Cpus()`/`CpuAutoBind()`/`CpuAutoBindOnNuma()`；`none` 不改动现有字段。
  - `ThreadCpuLease` 新增 `bindCurrentThreadBy<Env>(spec, who)` 模板重载和 `bindCurrentThreadBy(IThreadCpuAllocator &, spec, who)` 重载，`none` 返回非激活租约且不触碰分配器。
- `test/meson.build`：新增 `subdir('cpu_bind_string_test')`。
- `test/cpu_bind_string_test/CpuBindStringTest.cpp` + `meson.build`：覆盖 `parseCpuList`、`CpuBindingSpec::parse`、`ThreadedNodeOptions::CpuBind`、`ThreadCpuLease::bindCurrentThreadBy("none"/"")` 四组用例，40 个断言。

### tm_basic

- `tm_kit/basic/HwlocThreadCpuAllocator.hpp`
  - `CpuAllocatorScope` 枚举上移到 `HwlocCpuAllocatorConfig` 之前（`fromString` 需要它作参数）。
  - `HwlocCpuAllocatorConfig` 新增静态方法 `fromString(std::string const &, CpuAllocatorScope *scopeOut = nullptr)`：分号分隔的 `key=value` 串，支持 `exclude`/`avoid_first`/`smt`/`scope`/`lockdir`，大小写不敏感、key/val 空白 trim，未知 key 和坏值抛 `std::invalid_argument`。复用 `infra::parseCpuList`。
- `tm_kit/basic/RealTimeSharedPoolExecutor.hpp`
  - `SharedThreadPoolConfig` 新增 `cpusFromString(std::string const &)` 链式方法：接受 `none`/`""`（清空）、纯列表（`"2,3"`/`"2-5"`）和带 `cpus:` 前缀的形式，复用 `infra::parseCpuList`。
  - 新增 `using infra::parseCpuList`。
- `test/meson.build`：新增 `subdir('cpu_bind_string_test')`。
- `test/cpu_bind_string_test/CpuBindStringBasicTest.cpp` + `meson.build`：覆盖 `HwlocCpuAllocatorConfig::fromString`（含 hwloc 不可用时跳过）和 `SharedThreadPoolConfig::cpusFromString`，41 个断言。

## 测试与验证（命令、构建类型、结果）

构建类型：debug。

tm_infra：
```bash
meson setup builddir_debug --buildtype=debug --prefix=$INSTALL_PREFIX
ninja -j4 -C builddir_debug test/cpu_bind_string_test/cpu_bind_string_test
./builddir_debug/test/cpu_bind_string_test/cpu_bind_string_test
```
结果：40/40 PASS（parseCpuList 10、CpuBindingSpec 16、ThreadedNodeOptions CpuBind 10、ThreadCpuLease bindCurrentThreadBy 4）。

tm_basic（需先安装 tm_infra）：
```bash
ninja -j4 -C ../tm_infra/builddir_debug install
meson setup builddir_debug --buildtype=debug --prefix=$INSTALL_PREFIX --cmake-prefix-path=$VCPKG_ROOT/installed/x64-linux
ninja -j4 -C builddir_debug test/cpu_bind_string_test/cpu_bind_string_basic_test
./builddir_debug/test/cpu_bind_string_test/cpu_bind_string_basic_test
```
结果：41/41 PASS（HwlocCpuAllocatorConfig::fromString 29、SharedThreadPoolConfig::cpusFromString 12）。hwloc 在本机可用，两个分支均执行。

## 兼容性与注意事项

- 新增 API，不改现有 `Cpus()`/`CpuAutoBind()`/`CpuAutoBindOnNuma()` 任何签名与语义，完全向后兼容。
- `none`/空串与现有"空 cpus 不清除绑核"一致，不引入"解绑"新行为。
- tm_transport 的 `parseCpuList` 暂不改（独立本地实现，后续可单独迁移）。

## 相关文档（相关任务、tm_kit_reference 章节）

- [20261111-threaded-node-spin-wait-affinity](../20261111-threaded-node-spin-wait-affinity/README.md)：`configureThreadedNode` 与 `ThreadedNodeOptions` 的引入。
- [20261111-thread-cpu-autobind](../20261111-thread-cpu-autobind/README.md)：`CpuAutoBind` 与 `IThreadCpuAllocator` 扩展点。
- tm_kit_reference：`tm_infra/10-realtime-executors.md`（逐节点绑核 / 自动绑核 / ThreadCpuLease）、`tm_basic/11-hwloc-cpu-allocator.md`。
- tm_basic 侧记录：[../../tm_basic/doc/20261112-cpu-bind-string-api/README.md](../../tm_basic/doc/20261112-cpu-bind-string-api/README.md)
