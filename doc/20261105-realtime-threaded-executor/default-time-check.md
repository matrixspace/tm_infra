# 默认关闭时间检查

**任务开始时间：** 2026-11-09
**任务结束时间：** 2026-11-09
**状态：** 完成
**分支：** `feature/realtime-threaded-executor`

## 背景与方案
用户明确无需跨生产者全局FIFO，允许moodycamel；后续队列优化可采用该库，保留每生产者顺序。本轮将infra时间检查改为显式开启：未提供CheckTime默认false，CheckTimeComponent<>默认false，显式true继续检查。版本检查与时间检查独立，不关闭版本检查。不更换执行器默认实现。

## 实现与兼容性
- `Environments.hpp`：`CheckTimeComponent<DoCheck=false>`，新增 `CheckTimeEnabled<State>` 检测策略；未定义CheckTime时false，显式定义时遵循该值。
- RealTimeApp、SinglePassIterationApp、TopDownSinglePassIterationApp及各variant生成片段统一使用该策略。SinglePass显式包含Environments头文件。
- 未变更VersionChecker、LockFreeTimeCheckerComponent选择或默认执行器；关闭时间检查不等于默认绕过多线程checker的mutex。
- 用户显式指定的CheckTimeComponent<true>继续有效；无需修改既有显式false代码。没有将所有显式true强制改为false。
- 之前任务记录中的跨生产者全局FIFO要求已被本次用户指令撤销；后续可采用moodycamel，不据此默默更换现有执行器。

## 验证
测试新增缺省策略接受递减时间戳、缺省版本检查仍拒绝倒退版本，以及默认模板参数/显式true的静态断言；原有显式true/false、mutex/lockfree、多生产者和图测试继续运行。构建：先source build-common/build-env-setting.sh，再 `ninja -j2 -C .../tm_infra/builddir_release test/time_checker_test/time_checker_test test/threaded_handler_test/dedicated_executor_test`，`meson test -C .../tm_infra/builddir_release --no-rebuild`。

Release定向构建成功，最终两项测试2/2通过；私有worktree安装已同步。未合并/推送，reference待推送后同步。

[测试输出](default-time-check-tests.txt)。

代码与记录提交：tm_infra `771ca25`。
