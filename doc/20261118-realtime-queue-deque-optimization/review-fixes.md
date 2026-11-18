# 实时节点评审修复

**任务开始时间：** 2026-11-18  
**任务结束时间：** 2026-11-18  
**状态：** 完成  
**分支：** `fix/20261118-review-infra`（代码提交 `e139e98`）

评审发现三个 importer 的退出通知可能丢失、UniformSimpleImporter 无法中断已有回调、默认执行器延后释放已消费项，以及测试回调和基准的生命周期问题。

停机标志将在等待所用互斥锁内更新。旧 UniformSimpleImporter 回调保留原签名，新回调可接收停止上下文，后续由传输层重复轮询接入。

## 改动与验证

- 三个队列 importer 的停止标志在同一把锁下更新，解锁后通知。
- `UniformSimpleImporter` 仍支持单参数回调。双参数回调可用 `ImporterStopContext::waitForStop` 和 `stopRequested` 响应退出。已经进入的旧回调按原签名运行至返回。
- 默认执行器保留 deque 批量交接，每个回调结束后立刻销毁队首元素。基准计时覆盖到最后一项回调完成。
- trigger 与 const trigger 测试在读取最终结果前释放 runner 并等待 importer 退出。新增资源逐项释放和回调停止回归检查。

GCC 15，Meson debug。运行 `ninja -j2 -C builddir_debug` 和 `meson test -C builddir_debug --no-rebuild --print-errorlogs`，9/9 通过。执行器吞吐基准参数为 `500000 10000 3`，三轮顺序、数量检查通过。

相关记录：[队列优化](README.md)、[停止线程](../../../tm_basic/doc/20261118-graceful-thread-stop/README.md)。
