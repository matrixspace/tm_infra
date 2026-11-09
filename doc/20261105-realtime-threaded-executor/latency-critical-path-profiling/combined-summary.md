# Enhanced MPSC node recycling

GCC 15, C++17, `-O3 -pthread`; 5 alternating runs; CPU 20–23 producers, 24–27 consumer. CPU includes coordination; sparse pacing is part of the producer measurement. Counts are separate runs.

| Case | CPU ns/item before→after | wall ns/item before→after | p50 ns before→after | p99 ns before→after | allocations before→after |
|---|---:|---:|---:|---:|---:|
| throughput-p1-b0 | 323 → 287 | 162 → 144 | 0 → 0 | 0 → 0 | 130 → 132 |
| latency-p1-b0 | 2479 → 2622 | 1240 → 1310 | 481 → 523 | 759 → 734 | 0 → 0 |
| throughput-p1-b256 | 412 → 381 | 207 → 191 | 0 → 0 | 0 → 0 | 90 → 9 |
| latency-p1-b256 | 2641 → 2535 | 1320 → 1268 | 533 → 516 | 806 → 749 | 0 → 0 |
| throughput-p4-b0 | 924 → 1006 | 218 → 230 | 0 → 0 | 0 → 0 | 33 → 1133850 |
| latency-p4-b0 | 2947 → 2700 | 636 → 582 | 1453 → 1331 | 2872 → 2718 | 0 → 0 |
| throughput-p4-b256 | 1084 → 933 | 248 → 218 | 0 → 0 | 0 → 0 | 0 → 12 |
| latency-p4-b256 | 2956 → 2806 | 619 → 600 | 1529 → 1443 | 2803 → 2787 | 0 → 0 |
| sparse | 119543 → 119540 | 111011 → 111069 | 695 → 672 | 1024 → 907 | 0 → 0 |
| idle | 31556 → 35667 | 300076262 → 300080938 | 0 → 0 | 0 → 0 | 0 → 0 |

Idle rows show total CPU/wall ns per run (zero items), not ns/item. Allocations are counts from the entire instrumented measurement window, not retained heap. Tail estimates use limited samples on a shared machine.
