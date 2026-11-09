| Executor | Hops | interval μs | Version / spin μs | p50 ns | p99 ns | CPU ns/item |
|---|---:|---:|---|---:|---:|---:|
| inline | 8 | 0 | baseline / 0 | 144 | 155 | 302 |
| inline | 8 | 0 | candidate / 0 | 128 | 155 | 259 |
| dedicated | 8 | 0 | baseline / 0 | 96793 | 111239 | 201238 |
| dedicated | 8 | 0 | candidate / 0 | 99519 | 117065 | 207385 |
| dedicated | 8 | 0 | candidate / 200 | 415093 | 424072 | 2062271 |
| pool | 8 | 0 | baseline / 0 | 12456 | 23555 | 34219 |
| pool | 8 | 0 | candidate / 0 | 12979 | 27412 | 36351 |
| pool | 8 | 0 | candidate / 200 | 10168 | 19080 | 29370 |
| moody | 8 | 0 | baseline / 0 | 46097 | 57109 | 201239 |
| moody | 8 | 0 | candidate / 0 | 51544 | 59909 | 175206 |
| moody | 8 | 0 | candidate / 200 | 433661 | 445947 | 2156473 |
| inline | 8 | 50 | baseline / 0 | 351 | 387 | 6560 |
| inline | 8 | 50 | candidate / 0 | 347 | 376 | 6362 |
| dedicated | 8 | 50 | baseline / 0 | 124573 | 136761 | 254983 |
| dedicated | 8 | 50 | candidate / 0 | 125925 | 137441 | 257575 |
| dedicated | 8 | 50 | candidate / 200 | 311836 | 352133 | 1965051 |
| pool | 8 | 50 | baseline / 0 | 26437 | 39338 | 64693 |
| pool | 8 | 50 | candidate / 0 | 25751 | 37745 | 64097 |
| pool | 8 | 50 | candidate / 200 | 7935 | 9405 | 140387 |
| moody | 8 | 50 | baseline / 0 | 63399 | 74991 | 275053 |
| moody | 8 | 50 | candidate / 0 | 79385 | 91951 | 299327 |
| moody | 8 | 50 | candidate / 200 | 329508 | 370274 | 2053816 |
