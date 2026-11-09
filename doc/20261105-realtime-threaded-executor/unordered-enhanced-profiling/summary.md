# Enhanced moodycamel final comparison (BLOCK_SIZE=256)

GCC 15, C++17, `-O3 -pthread`; 5 alternating runs; CPU 20–23 producers, 24–27 consumer. CPU includes coordination; sparse pacing is part of the producer measurement. Counts are separate runs.

| Case | CPU ns/item before→after | wall ns/item before→after | p50 ns before→after | p99 ns before→after | allocations before→after |
|---|---:|---:|---:|---:|---:|
| throughput-p1-b0 | 254 → 278 | 127 → 152 | 0 → 0 | 0 → 0 | 23 → 3649 |
| latency-p1-b0 | 2265 → 2328 | 1131 → 1164 | 347 → 452 | 684 → 613 | 0 → 0 |
| throughput-p1-b256 | 315 → 375 | 158 → 195 | 0 → 0 | 0 → 0 | 115 → 3276 |
| latency-p1-b256 | 2588 → 2765 | 1295 → 1382 | 440 → 519 | 745 → 3566 | 0 → 0 |
| throughput-p4-b0 | 1031 → 147 | 227 → 66 | 0 → 0 | 0 → 0 | 26254 → 15256 |
| latency-p4-b0 | 2604 → 2840 | 548 → 619 | 1324 → 1477 | 2504 → 3235 | 0 → 0 |
| throughput-p4-b256 | 1073 → 417 | 246 → 137 | 0 → 0 | 0 → 0 | 3337 → 15317 |
| latency-p4-b256 | 2707 → 2775 | 584 → 596 | 1295 → 1399 | 2561 → 3203 | 0 → 0 |
| sparse | 119508 → 119426 | 111018 → 111010 | 653 → 956 | 992 → 1706 | 0 → 0 |
| idle | 35557 → 37166 | 300078640 → 300076664 | 0 → 0 | 0 → 0 | 0 → 0 |

Idle rows show total CPU/wall ns per run (zero items), not ns/item. Allocations are counts from the entire instrumented measurement window, not retained heap. Tail estimates use limited samples on a shared machine.
