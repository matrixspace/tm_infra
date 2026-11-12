# configureThreadedNode no-op for 非实时 App

**任务开始时间：** 2026-11-12
**任务结束时间：** 2026-11-12
**状态：** 完成
**分支：** `develop`

## 背景

用户希望同一段业务代码能在实时模式（`RealTimeApp`）和回测模式（`SinglePassIterationApp`、
`TopDownSinglePassIterationApp`、`BasicWithTimeApp`）中共享，且能直接调用
`M::configureThreadedNode(node, opts)` 来配置 CPU 绑核等线程选项，而不必在代码里加
`#ifdef` 或模板特化。

原问题：非实时 App 类中没有 `configureThreadedNode` 静态方法，调用会导致编译错误。

## 方案

在三种非实时 App 类的 public section 中各加一个接受任意节点和选项类型的 no-op 静态方法，
返回 `false`，不引用 `RealTimeThreadedExecutor.hpp` 中的任何类型：

```cpp
template <class T, class Opts>
static bool configureThreadedNode(std::shared_ptr<T> const &, Opts const &) {
    return false;
}
```

签名使用 `Opts` 模板参数（而非具体的 `ThreadedNodeOptions`）避免引入额外的头文件依赖，
同时保持与 `RealTimeApp::configureThreadedNode` 在调用点完全兼容（调用方传入的实际类型
是 `ThreadedNodeOptions`，模板推导自然匹配）。

## 代码改动

| 文件 | 改动 |
|---|---|
| `tm_kit/infra/SinglePassIterationApp.hpp` | 在 `actionIsOneTimeOnly` 之后加 no-op `configureThreadedNode`（第 1200–1206 行） |
| `tm_kit/infra/TopDownSinglePassIterationApp.hpp` | 同上（第 1054–1060 行） |
| `tm_kit/infra/BasicWithTimeApp.hpp` | 在 `actionIsOneTimeOnly` 之后加 no-op `configureThreadedNode`（第 108–114 行） |
| `test/threaded_handler_test/NonRealtimeConfigureThreadedNodeTest.cpp` | 新增测试文件 |
| `test/threaded_handler_test/meson.build` | 注册新测试目标 `non_realtime_configure_threaded_node_test` |

## 测试与验证

**构建：**
```bash
source build-common/build-env-setting.sh && source /opt/rh/gcc-toolset-15/enable
ninja -j2 -C tm_infra/builddir_debug test/threaded_handler_test/non_realtime_configure_threaded_node_test
```

**运行结果（debug 构建）：**
```
ok: SinglePassIterationApp::configureThreadedNode returns false
ok: TopDownSinglePassIterationApp::configureThreadedNode returns false
ok: BasicWithTimeApp::configureThreadedNode returns false
ok: RealTimeApp::configureThreadedNode on non-threaded node returns false
All passed.
```

注意事项：
- `SinglePassIterationApp` / `TopDownSinglePassIterationApp` 要求 `StateT::PreserveInputRelativeOrder == true`，测试使用了单独的 `SinglePassClock`
- `RealTimeApp` 的回归测试用非线程节点（`SuggestThreaded(false)`）避免触发无关的模板实例化

## 兼容性与注意事项

- 纯增量变更，不改变已有任何行为
- 非实时 App 下 `configureThreadedNode` 始终返回 `false`；上层代码若依赖返回值判断是否生效，
  在实时/回测模式下行为一致（`false` = 未生效）
- `HwlocThreadCpuAllocatorComponent` / `HwlocCrossProcessCpuAllocatorComponent` 配合非实时 App
  使用时，分配器本身不会被框架调用（无节点线程），但组件挂在 Env 上编译没有问题

## 相关文档

- 问题调查：见本次会话中对 `HwlocThreadCpuAllocatorComponent` 配合非实时 App 行为的分析
- [[tm_infra/doc/20261111-thread-cpu-autobind]]
