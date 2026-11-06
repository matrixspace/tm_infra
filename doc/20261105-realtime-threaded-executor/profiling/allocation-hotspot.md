# Dedicated 基线分配热点采样

Release 独立后端二进制，四生产者、每生产者 1000000 条小消息；CPU 20–27，producer 20–23、consumer 24–27。

等价复现命令：

```sh
perf record --no-buildid-cache --call-graph dwarf -F 997 -e cycles:u \
  -o /tmp/executor-dedicated-before.data -- taskset -c 20-27 \
  /tmp/executor-before/executor_bench_dedicated --executor dedicated \
  --messages 1000000 --producers 4 --payload 0 \
  --producer-cpus 20,21,22,23 --consumer-cpus 24,25,26,27
perf report -i /tmp/executor-dedicated-before.data --stdio --no-children
```

约 4K 个 cycles:u 样本，丢样 0。self 样本占比 malloc 22.78%、_int_free 11.51%、_int_malloc 10.42%、cfree 3.47%，合计约 48%。此为全进程用户态采样占比，不能等同精确逐消息耗时。原始 perf.data 留在本机临时目录，避免提交包含本机路径的二进制符号数据。
