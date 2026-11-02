## Environment

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
| Pinned | no (--cpu not given) |
| Timer pair overhead | p50 0 ns, p99 41.6667 ns |

## Workload

324144 messages from data/aapl.obm (AAPL): 197312 add, 125930 cancel, 902 modify, 0 market. 7 repeats after 1 warm-up.

## Results

| Version | Throughput (M msg/s, median [min-max]) | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |
|---|---|---|---|---|---|
| Baseline (std::map + std::list) | 11.16 [11.04-11.20] | 83 | 208 | 250 | 395125 |
| + flat levels (intrusive lists, new/delete) | 18.27 [17.06-18.41] | 83 | 125 | 208 | 23500 |
| + pool allocator | 26.77 [26.40-27.12] | 42 | 167 | 208 | 33375 |
| + flat hash map | 34.02 [33.59-34.75] | 42 | 167 | 250 | 32542 |
| + cache tuning (32B order) | 41.57 [36.17-43.39] | 42 | 167 | 208 | 19917 |

Latency = time inside process() per message, including event emission. Resolution: 42 ns/tick.
wrote histograms to results/aapl
