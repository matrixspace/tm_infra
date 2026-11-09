| Executor | Hops | interval μs | Version / spin μs | p50 ns | p99 ns | CPU ns/item |
|---|---:|---:|---|---:|---:|---:|
| inline | 8 | 0 | candidate / 0 | 106 | 110 | 205 |
| dedicated | 8 | 0 | candidate / 0 | 81970 | 91089 | 165198 |
| dedicated | 8 | 0 | candidate / 200 | 38653 | 107265 | 219712 |
| pool | 8 | 0 | candidate / 0 | 13954 | 28593 | 37193 |
| pool | 8 | 0 | candidate / 200 | 11662 | 23088 | 34457 |
| moody | 8 | 0 | candidate / 0 | 52228 | 63002 | 185171 |
| moody | 8 | 0 | candidate / 200 | 40481 | 111794 | 223856 |
| legacy | 8 | 0 | candidate / 0 | 81558 | 92264 | 170728 |
