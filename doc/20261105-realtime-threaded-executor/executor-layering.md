# 执行器实现分层

**任务开始时间：** 2026-11-09
**任务结束时间：** 2026-11-09
**状态：** 完成（transport运行验证受沙箱限制）
**分支：** `feature/realtime-threaded-executor`

## 背景
用户要求infra只提供Inline和Dedicated默认实现，SharedPool与其他可选执行器放在basic。沿用已有任务记录。

## 方案
将SharedPool及其专用MPSC队列移到basic命名空间，不保留infra兼容转发；迁移共享池测试和下游引用，保留infra默认执行器测试。用户已明确授权分层调整。

## 测试与验证
已完成：GCC15构建环境，infra Release `ninja -j2` 后 `meson test --no-rebuild` 2/2通过；basic Release定向编译 `threaded_handler_test`、`executor_mechanism_test`、`executor_bench_pool`、`executor_latency_bench`，迁入的两项测试2/2通过。pool 1000消息冒烟 `ok=true`。安装到worktree私有前缀，并清除旧infra两个头文件。

命令均从工作区根目录执行并先source `build-common/build-env-setting.sh`；Ninja目标使用 `test/<子目录>/<目标>` 完整路径。transport只有已有Debug构建目录，先前尝试Release目录失败未计入验证，改用Debug定向验证。未改算法，因此不重复性能对比。

## 兼容性与使用
SharedPool引入路径从 `tm_kit/infra/RealTimeSharedPoolExecutor.hpp` 改为 `tm_kit/basic/RealTimeSharedPoolExecutor.hpp`，四个公开类型 `SharedThreadPoolConfig`、`SharedThreadPool`、`RealTimeSharedPoolExecutor`、`RealTimeSharedPoolExecutorComponent` 从infra迁入basic。没有infra兼容别名。原默认类型准确名称仍为 `DedicatedThreadExecutor`，未随本次层级调整改名。公共执行器协议、选择机制及等待辅助函数保留infra。

## 代码改动
- basic：安装SharedPool及其专用 `RealTimeMPSCQueue.hpp`；迁入handler/机制测试，基准同步引用。
- infra：移除上述两个头文件及安装条目；测试保留Dedicated与time checker。
- transport：接入测试引用basic的共享池类型。
- 不改共享池调度、队列算法或默认执行器行为；旧记录中的infra路径为历史状态，以本节为准。

transport Debug定向构建成功；接入测试启动HTTP监听时中止：`Cannot open acceptor on port 39871`。独立Python探测固定端口39871和系统分配端口0均报 `PermissionError: [Errno 1] Operation not permitted`，确认当前沙箱禁止bind。本轮没有完成transport运行验证，也未把它计为通过。基础库测试已通过；没有新增API或调度行为。

本轮代码提交：tm_basic `7edf028`，tm_transport `332e3c2`；infra删除旧实现与记录另行提交。保持feature分支，未合并/推送；reference按项目流程待推送后同步。

验证输出见下列归档：
- [infra Release](executor-layering-validation/infra-tests.txt)
- [basic Release](executor-layering-validation/basic-tests.txt)
- [pool冒烟](executor-layering-validation/pool-smoke.txt)
- [transport运行受限](executor-layering-validation/transport-test.txt)
