| Executor | Hops | interval μs | Version / spin μs | p50 ns | p99 ns | CPU ns/item |
|---|---:|---:|---|---:|---:|---:|
| inline | 8 | 0 | baseline / 0 | 233 | 241 | 440 |
| inline | 8 | 0 | candidate / 0 | 151 | 166 | 285 |
| dedicated | 8 | 0 | baseline / 0 | 104520 | 114415 | 236589 |
| dedicated | 8 | 0 | candidate / 0 | 104686 | 115181 | 236467 |
| dedicated | 8 | 0 | candidate / 200 | 3366 | 3984 | 31906 |
| pool | 8 | 0 | baseline / 0 | 9566 | 14753 | 25213 |
| pool | 8 | 0 | candidate / 0 | 9874 | 15938 | 26177 |
| pool | 8 | 0 | candidate / 200 | 7473 | 10603 | 22286 |
| moody | 8 | 0 | baseline / 0 | 36470 | 42231 | 189258 |
| moody | 8 | 0 | candidate / 0 | 36550 | 44530 | 155351 |
| moody | 8 | 0 | candidate / 200 | 4074 | 8542 | 39357 |
| inline | 8 | 50 | baseline / 0 | 406 | 452 | 6471 |
| inline | 8 | 50 | candidate / 0 | 391 | 451 | 6256 |
| dedicated | 8 | 50 | baseline / 0 | 107902 | 117012 | 245768 |
| dedicated | 8 | 50 | candidate / 0 | 107833 | 117428 | 245684 |
| dedicated | 8 | 50 | candidate / 200 | 3977 | 4610 | 893573 |
| pool | 8 | 50 | baseline / 0 | 25557 | 37911 | 67778 |
| pool | 8 | 50 | candidate / 0 | 25911 | 39574 | 71293 |
| pool | 8 | 50 | candidate / 200 | 6421 | 9943 | 136047 |
| moody | 8 | 50 | baseline / 0 | 85878 | 99225 | 486710 |
| moody | 8 | 50 | candidate / 0 | 86774 | 99208 | 392855 |
| moody | 8 | 50 | candidate / 200 | 4705 | 9393 | 895661 |
