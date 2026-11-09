# Enhanced MPSC node recycling

GCC 15, C++17, `-O3 -pthread`; 5 alternating runs; CPU 20–23 producers, 24–27 consumer. CPU includes coordination; sparse pacing is part of the producer measurement. Counts are separate runs.

| Case | CPU ns/item before→after | wall ns/item before→after | p50 ns before→after | p99 ns before→after | allocations before→after |
|---|---:|---:|---:|---:|---:|
| throughput-p1-b0 | 600 → 338 | 301 → 170 | 0 → 0 | 0 → 0 | 91 → 147 |
| latency-p1-b0 | 2724 → 2536 | 1362 → 1266 | 599 → 501 | 794 → 781 | 0 → 0 |
| throughput-p1-b256 | 665 → 452 | 333 → 227 | 0 → 0 | 0 → 0 | 22 → 99 |
| latency-p1-b256 | 2698 → 2757 | 1347 → 1378 | 604 → 524 | 824 → 800 | 0 → 0 |
| throughput-p4-b0 | 1144 → 815 | 267 → 196 | 0 → 0 | 0 → 0 | 18 → 9 |
| latency-p4-b0 | 3074 → 3000 | 670 → 655 | 1560 → 1495 | 2861 → 2831 | 0 → 0 |
| throughput-p4-b256 | 1225 → 898 | 287 → 212 | 0 → 0 | 0 → 0 | 7 → 0 |
| latency-p4-b256 | 3090 → 2981 | 670 → 647 | 1591 → 1511 | 2881 → 2845 | 0 → 0 |
| sparse | 119572 → 119624 | 110983 → 111069 | 889 → 696 | 1135 → 1039 | 0 → 0 |
| idle | 33489 → 32731 | 300077921 → 300076394 | 0 → 0 | 0 → 0 | 0 → 0 |

Idle rows show total CPU/wall ns per run (zero items), not ns/item. Allocations are counts from the entire instrumented measurement window, not retained heap. Tail estimates use limited samples on a shared machine.
