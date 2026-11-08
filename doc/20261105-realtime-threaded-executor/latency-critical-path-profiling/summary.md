# Enhanced MPSC node recycling

GCC 15, C++17, `-O3 -pthread`; 5 alternating runs; CPU 20–23 producers, 24–27 consumer. CPU includes coordination; sparse pacing is part of the producer measurement. Counts are separate runs.

| Case | CPU ns/item before→after | wall ns/item before→after | p50 ns before→after | p99 ns before→after | allocations before→after |
|---|---:|---:|---:|---:|---:|
| throughput-p1-b0 | 337 → 241 | 169 → 121 | 0 → 0 | 0 → 0 | 181 → 206 |
| latency-p1-b0 | 2470 → 2466 | 1234 → 1233 | 494 → 462 | 757 → 738 | 0 → 0 |
| throughput-p1-b256 | 436 → 329 | 219 → 165 | 0 → 0 | 0 → 0 | 76 → 18 |
| latency-p1-b256 | 2657 → 2394 | 1328 → 1230 | 526 → 481 | 820 → 703 | 0 → 0 |
| throughput-p4-b0 | 817 → 1002 | 199 → 222 | 0 → 0 | 0 → 0 | 23360 → 11 |
| latency-p4-b0 | 3017 → 2678 | 653 → 576 | 1516 → 1281 | 2862 → 2607 | 0 → 0 |
| throughput-p4-b256 | 1053 → 913 | 241 → 212 | 0 → 0 | 0 → 0 | 8939 → 8779 |
| latency-p4-b256 | 3009 → 2716 | 648 → 586 | 1562 → 1367 | 2915 → 2654 | 0 → 0 |
| sparse | 119532 → 119467 | 110995 → 111013 | 746 → 668 | 1115 → 1003 | 0 → 0 |
| idle | 34738 → 35998 | 300078979 → 300078081 | 0 → 0 | 0 → 0 | 0 → 0 |

Idle rows show total CPU/wall ns per run (zero items), not ns/item. Allocations are counts from the entire instrumented measurement window, not retained heap. Tail estimates use limited samples on a shared machine.

Subsequent 90-run randomized dual-baseline audit found bimodal four-producer small-message timings. Consult p4-noise-control.jsonl and p4-noise-summary.json before interpreting the five-run median for throughput-p4-b0 as a regression.
