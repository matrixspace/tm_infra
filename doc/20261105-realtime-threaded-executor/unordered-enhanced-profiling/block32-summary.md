# Enhanced MPSC node recycling

GCC 15, C++17, `-O3 -pthread`; 5 alternating runs; CPU 20–23 producers, 24–27 consumer. CPU includes coordination; sparse pacing is part of the producer measurement. Counts are separate runs.

| Case | CPU ns/item before→after | wall ns/item before→after | p50 ns before→after | p99 ns before→after | allocations before→after |
|---|---:|---:|---:|---:|---:|
| throughput-p1-b0 | 251 → 260 | 126 → 159 | 0 → 0 | 0 → 0 | 18 → 31000 |
| latency-p1-b0 | 2340 → 2446 | 1176 → 1258 | 477 → 492 | 740 → 1060 | 0 → 0 |
| throughput-p1-b256 | 333 → 505 | 167 → 288 | 0 → 0 | 0 → 0 | 103 → 30658 |
| latency-p1-b256 | 2530 → 2451 | 1264 → 1223 | 506 → 500 | 765 → 1047 | 0 → 0 |
| throughput-p4-b0 | 803 → 194 | 189 → 80 | 0 → 0 | 0 → 0 | 6088 → 124705 |
| latency-p4-b0 | 2655 → 2881 | 561 → 659 | 1275 → 1440 | 2559 → 3272 | 0 → 0 |
| throughput-p4-b256 | 1060 → 574 | 243 → 189 | 0 → 0 | 0 → 0 | 2898 → 124609 |
| latency-p4-b256 | 2678 → 3083 | 572 → 660 | 1339 → 1535 | 2534 → 3748 | 0 → 0 |
| sparse | 119392 → 119510 | 110954 → 111027 | 656 → 972 | 1036 → 1675 | 0 → 0 |
| idle | 32721 → 37284 | 300074575 → 300077501 | 0 → 0 | 0 → 0 | 0 → 0 |

Idle rows show total CPU/wall ns per run (zero items), not ns/item. Allocations are counts from the entire instrumented measurement window, not retained heap. Tail estimates use limited samples on a shared machine.
