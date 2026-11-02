// Low-overhead timestamps for latency measurement.
//
//   x86-64 : rdtsc (invariant TSC), lfence-serialised.
//   AArch64: cntvct_el0 virtual counter (24 MHz on Apple Silicon -> 41.7 ns
//            resolution; 1 GHz on Graviton3 / recent Neoverse).
//   other  : std::chrono::steady_clock.
// Ticks are converted to nanoseconds with a one-off calibration against
// steady_clock. Benchmarks record raw ticks and convert at report time.
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace ob {

struct Timer {
    static inline std::uint64_t now() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
        std::uint32_t lo, hi;
        asm volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi)::"memory");
        return (std::uint64_t{hi} << 32) | lo;
#elif defined(__aarch64__)
        std::uint64_t v;
        asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)::"memory");
        return v;
#else
        return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

    static const char* source() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
        return "rdtsc";
#elif defined(__aarch64__)
        return "cntvct_el0";
#else
        return "steady_clock";
#endif
    }

    // Nanoseconds per tick, measured once (~50 ms).
    static double ns_per_tick() {
        static const double v = [] {
#if defined(__aarch64__)
            std::uint64_t freq;
            asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
            if (freq) return 1e9 / static_cast<double>(freq);
#endif
            using clk = std::chrono::steady_clock;
            const auto t0 = clk::now();
            const auto c0 = now();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto t1 = clk::now();
            const auto c1 = now();
            const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            return ns / static_cast<double>(c1 - c0);
        }();
        return v;
    }
};

}  // namespace ob
