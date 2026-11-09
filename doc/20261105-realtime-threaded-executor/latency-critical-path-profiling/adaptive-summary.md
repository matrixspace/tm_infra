# Enhanced MPSC node recycling

GCC 15, C++17, `-O3 -pthread`; 5 alternating runs; CPU 20–23 producers, 24–27 consumer. CPU includes coordination; sparse pacing is part of the producer measurement. Counts are separate runs.

| Case | CPU ns/item before→after | wall ns/item before→after | p50 ns before→after | p99 ns before→after | allocations before→after |
|---|---:|---:|---:|---:|---:|
| throughput-p1-b0 | 322 → 303 | 161 → 152 | 0 → 0 | 0 → 0 | 19 → 111 |
| latency-p1-b0 | 2469 → 2625 | 1235 → 1311 | 483 → 512 | 752 → 731 | 0 → 0 |
| throughput-p1-b256 | 451 → 406 | 226 → 203 | 0 → 0 | 0 → 0 | 80 → 94 |
| latency-p1-b256 | 2727 → 2757 | 1363 → 1377 | 521 → 540 | 830 → 765 | 0 → 0 |
| throughput-p4-b0 | 811 → 1012 | 192 → 224 | 0 → 0 | 0 → 0 | 22240 → 19099 |
| latency-p4-b0 | 2938 → 2797 | 623 → 606 | 1514 → 1367 | 2836 → 2706 | 0 → 0 |
| throughput-p4-b256 | 984 → 973 | 230 → 225 | 0 → 0 | 0 → 0 | 3791 → 9962 |
| latency-p4-b256 | 2999 → 2886 | 662 → 603 | 1569 → 1487 | 2886 → 2774 | 0 → 0 |
| sparse | 119471 → 119585 | 110973 → 111018 | 675 → 672 | 1019 → 972 | 0 → 0 |
| idle | 35642 → 37705 | 300080773 → 300079530 | 0 → 0 | 0 → 0 | 0 → 0 |

Idle rows show total CPU/wall ns per run (zero items), not ns/item. Allocations are counts from the entire instrumented measurement window, not retained heap. Tail estimates use limited samples on a shared machine.
