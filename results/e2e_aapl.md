| Item | Value |
|---|---|
| CPU | Apple M2 (8 logical cores) |
| OS | Darwin 25.5.0 arm64 |
| Platform | bare metal |
| Compiler | clang 17.0.0 (clang-1700.0.13.5) |
| Flags | `-O3 -DNDEBUG -march=native` |
| Governor | n/a (macOS manages DVFS) |
| Isolated CPUs | n/a (macOS has no core isolation) |
| Timer | cntvct_el0, 41.6667 ns/tick |

324144 messages, ring capacity 65536, producer cpu -1, consumer cpu -1

| Mode | Throughput (M msg/s) | e2e p50 (ns) | e2e p99 (ns) | e2e p99.9 (ns) | match p50 (ns) | match p99 (ns) |
|---|---|---|---|---|---|---|
| paced @ 1000000 msg/s | 1.00 | 167 | 333 | 6542 | 42 | 208 |
| burst (unpaced) | 11.98 | 4511958 | 5797292 | 5807958 | 42 | 167 |
