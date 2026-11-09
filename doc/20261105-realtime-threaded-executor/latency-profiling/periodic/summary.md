| Executor | Hops | interval μs | Version / spin μs | p50 ns | p99 ns | CPU ns/item |
|---|---:|---:|---|---:|---:|---:|
| inline | 4 | 50 | baseline / 0 | 271 | 286 | 6329 |
| inline | 4 | 50 | candidate / 0 | 266 | 290 | 5990 |
| dedicated | 4 | 50 | baseline / 0 | 47950 | 57113 | 65396 |
| dedicated | 4 | 50 | candidate / 0 | 48395 | 60022 | 67097 |
| dedicated | 4 | 50 | candidate / 200 | 2554 | 3723 | 205767 |
| pool | 4 | 50 | baseline / 0 | 18145 | 22130 | 34825 |
| pool | 4 | 50 | candidate / 0 | 18018 | 22416 | 34323 |
| pool | 4 | 50 | candidate / 200 | 5379 | 6260 | 65198 |
| moody | 4 | 50 | baseline / 0 | 20357 | 24304 | 88671 |
| moody | 4 | 50 | candidate / 0 | 23460 | 30394 | 80869 |
| moody | 4 | 50 | candidate / 200 | 2475 | 5221 | 205474 |
| inline | 4 | 1000 | baseline / 0 | 276 | 316 | 5931 |
| inline | 4 | 1000 | candidate / 0 | 276 | 315 | 5886 |
| dedicated | 4 | 1000 | baseline / 0 | 61606 | 65396 | 83834 |
| dedicated | 4 | 1000 | candidate / 0 | 61101 | 77388 | 83164 |
| dedicated | 4 | 1000 | candidate / 200 | 57933 | 64354 | 900309 |
| pool | 4 | 1000 | baseline / 0 | 24474 | 26328 | 47471 |
| pool | 4 | 1000 | candidate / 0 | 24423 | 26843 | 47024 |
| pool | 4 | 1000 | candidate / 200 | 24990 | 27033 | 251778 |
| moody | 4 | 1000 | baseline / 0 | 50139 | 55827 | 221656 |
| moody | 4 | 1000 | candidate / 0 | 49677 | 58174 | 172275 |
| moody | 4 | 1000 | candidate / 200 | 49850 | 55552 | 973350 |
