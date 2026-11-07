| Executor | Hops | interval μs | Version / spin μs | p50 ns | p99 ns | CPU ns/item |
|---|---:|---:|---|---:|---:|---:|
| inline | 4 | 50 | candidate / 0 | 261 | 341 | 5982 |
| dedicated | 4 | 50 | candidate / 0 | 48283 | 58128 | 65305 |
| dedicated | 4 | 50 | candidate / 200 | 2699 | 7061 | 206018 |
| pool | 4 | 50 | candidate / 0 | 17853 | 22466 | 37402 |
| pool | 4 | 50 | candidate / 200 | 5894 | 10406 | 68004 |
| moody | 4 | 50 | candidate / 0 | 23040 | 30254 | 80644 |
| moody | 4 | 50 | candidate / 200 | 2860 | 6869 | 205732 |
| legacy | 4 | 50 | candidate / 0 | 48323 | 58593 | 69694 |
| inline | 4 | 1000 | candidate / 0 | 280 | 366 | 5896 |
| dedicated | 4 | 1000 | candidate / 0 | 60979 | 66148 | 83005 |
| dedicated | 4 | 1000 | candidate / 200 | 64590 | 75004 | 907350 |
| pool | 4 | 1000 | candidate / 0 | 24302 | 26738 | 47701 |
| pool | 4 | 1000 | candidate / 200 | 24733 | 27279 | 252373 |
| moody | 4 | 1000 | candidate / 0 | 51212 | 56089 | 175083 |
| moody | 4 | 1000 | candidate / 200 | 48606 | 54314 | 995468 |
| legacy | 4 | 1000 | candidate / 0 | 63139 | 67306 | 89344 |
