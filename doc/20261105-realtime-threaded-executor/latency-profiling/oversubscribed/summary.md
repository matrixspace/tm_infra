| Executor | Hops | interval μs | Version / spin μs | p50 ns | p99 ns | CPU ns/item |
|---|---:|---:|---|---:|---:|---:|
| inline | 8 | 0 | baseline / 0 | 128 | 132 | 263 |
| inline | 8 | 0 | candidate / 0 | 95 | 99 | 195 |
| dedicated | 8 | 0 | baseline / 0 | 99049 | 116976 | 206889 |
| dedicated | 8 | 0 | candidate / 0 | 97586 | 116545 | 203255 |
| dedicated | 8 | 0 | candidate / 200 | 38641 | 43315 | 195789 |
| pool | 8 | 0 | baseline / 0 | 13680 | 22787 | 35514 |
| pool | 8 | 0 | candidate / 0 | 14096 | 23812 | 37149 |
| pool | 8 | 0 | candidate / 200 | 10495 | 18428 | 30362 |
| moody | 8 | 0 | baseline / 0 | 44268 | 53850 | 196963 |
| moody | 8 | 0 | candidate / 0 | 52478 | 60462 | 177122 |
| moody | 8 | 0 | candidate / 200 | 40437 | 82548 | 214141 |
