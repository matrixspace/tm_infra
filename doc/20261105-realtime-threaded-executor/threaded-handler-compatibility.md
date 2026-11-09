# ThreadedHandler 继承接口兼容性修复

**任务开始时间：** 2026-11-09  
**任务结束时间：** 2026-11-09  
**状态：** 完成  
**分支：** `feature/realtime-threaded-executor`

## 背景
评审发现第二阶段把公开类模板 `ThreadedHandler` 改为 `ExecHandler` 的别名后，具体 Environment 下的派生类无法继续通过 `H() : ThreadedHandler() {}` 构造基类，也无法用 `ThreadedHandler::` 访问继承成员。原有源码兼容性承诺要求保留这些写法。

## 方案
恢复无额外成员的公开薄类包装，继承 `ExecHandler<T, W, ThreadedPolicy>`，继续复用统一执行器实现。放弃直接使用别名，因为别名不提供原类的注入类名。

## 代码改动
- `tm_kit/infra/RealTimeApp.hpp`：恢复 `ThreadedHandler` 类包装。
- `test/threaded_handler_test/ThreadedHandlerCompatibilityTest.cpp` 与 `meson.build`：验证具体 Environment 派生类的注入类名构造、限定成员访问与异步数据处理。

## 测试与验证
- 构建环境：`source build-common/build-env-setting.sh`，gcc-toolset-15 / GCC 15，C++17，Release。
- 修复前：`g++ -std=c++17 -pthread -Iworktrees/realtime-threaded-executor/tm_infra -fsyntax-only worktrees/realtime-threaded-executor/tm_infra/test/threaded_handler_test/ThreadedHandlerCompatibilityTest.cpp`，确认报 `does not have any field named ThreadedHandler` 与 `ThreadedHandler has not been declared`，覆盖构造和限定成员访问两种回归。
- 修复后：`ninja -j4 -C worktrees/realtime-threaded-executor/tm_infra/builddir_release test/threaded_handler_test/threaded_handler_compatibility_test test/threaded_handler_test/dedicated_executor_test test/time_checker_test/time_checker_test`，构建通过。
- `meson test -C worktrees/realtime-threaded-executor/tm_infra/builddir_release --no-rebuild --print-errorlogs`：3/3 通过（兼容性、Dedicated 执行器、时间检查）。新测试经 `IHandler<int>` 发送数据，并通过旧的限定名调用启动、时间检查与停止，确认 CRTP 回调和异步处理正常。
- `git diff --check` 通过；新增 C++ 测试已按仓库 `.clang-format` 格式化。本轮未重新构建下游仓库。

## 兼容性与注意事项
保留统一 `ExecHandler` 的执行器选择和回调路径，恢复旧的公开类名使用方式。

## 相关文档
- [任务总记录](README.md)
- [第二阶段设计](phase2-unified-handler-executor.md)
- [执行模式参考](../../../../../tm_kit_reference/tm_infra/01-app-types.md)
