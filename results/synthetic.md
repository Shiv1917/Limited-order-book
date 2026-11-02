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

2000000 messages from synthetic, seed 42: 1004860 add, 718505 cancel, 157661 modify, 118974 market. 5 repeats after 1 warm-up.

## Results

| Version | Throughput (M msg/s, median [min-max]) | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |
|---|---|---|---|---|---|
| Baseline (std::map + std::list) | 17.36 [17.24-17.40] | 83 | 167 | 375 | 59708 |
| + flat levels (intrusive lists, new/delete) | 21.80 [21.68-22.00] | 42 | 125 | 292 | 39375 |
| + pool allocator | 28.95 [28.80-29.24] | 42 | 125 | 208 | 37917 |
| + flat hash map | 29.28 [28.62-29.35] | 42 | 167 | 250 | 63000 |
| + cache tuning (32B order) | 29.85 [29.76-30.62] | 42 | 167 | 250 | 50875 |

Latency = time inside process() per message, including event emission. Resolution: 42 ns/tick.
wrote histograms to results/synthetic
