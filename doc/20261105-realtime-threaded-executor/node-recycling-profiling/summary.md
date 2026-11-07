# Enhanced MPSC node recycling

GCC 15, C++17, `-O3 -pthread`; five alternating runs; CPU 20–23 producers, 24–27 consumer. CPU includes coordination; sparse pacing is part of the producer measurement. Counts are separate runs.

| Case | CPU ns/item before→after | wall ns/item before→after | p50 ns before→after | p99 ns before→after | allocations before→after |
|---|---:|---:|---:|---:|---:|
| throughput-p1-b0 | 596 → 543 | 299 → 272 | 0 → 0 | 0 → 0 | 1000000 → 177 |
| latency-p1-b0 | 2679 → 2729 | 1339 → 1364 | 479 → 605 | 1418 → 799 | 10000 → 0 |
| throughput-p1-b256 | 876 → 620 | 605 → 311 | 0 → 0 | 0 → 0 | 1000000 → 23 |
| latency-p1-b256 | 2855 → 2818 | 1426 → 1418 | 769 → 627 | 937 → 852 | 10000 → 0 |
| throughput-p4-b0 | 1099 → 1131 | 252 → 264 | 0 → 0 | 0 → 0 | 4000000 → 874664 |
| latency-p4-b0 | 3073 → 3152 | 662 → 673 | 1495 → 1619 | 3935 → 3031 | 40000 → 0 |
| throughput-p4-b256 | 1180 → 1219 | 367 → 279 | 0 → 0 | 0 → 0 | 4000000 → 357799 |
| latency-p4-b256 | 4319 → 3105 | 958 → 676 | 2294 → 1605 | 7162 → 2972 | 40000 → 0 |
| sparse | 119543 → 119720 | 110959 → 110991 | 775 → 930 | 2034 → 1143 | 3000 → 0 |
| idle | 35065 → 36816 | 300078919 → 300081584 | 0 → 0 | 0 → 0 | 0 → 0 |

Idle rows show total CPU/wall ns per run (zero items), not ns/item. Allocations are counts from the entire instrumented measurement window, not retained heap. Tail estimates use limited samples on a shared machine.
