// Latency model: fixed delays plus optional seeded jitter.
//
//   order arrival = decided_at [+ feed if triggered by market data] + proc
//                   + to_exchange + jitter, clamped to >= previous arrival
//   delivery      = exchange_ts + from_exchange + jitter,
//                   clamped to >= previous delivery
// The clamps keep each stream FIFO: a cancel sent after a new order always
// arrives after it, and acks/fills reach the strategy in exchange order.
//
// Determinism: std::mt19937_64's output sequence is fixed by the standard,
// but std::uniform_int_distribution is not (libstdc++ and libc++ differ), so
// jitter is drawn with a plain modulo of the raw engine output.
#pragma once

#include <algorithm>
#include <cstdint>
#include <random>

#include "sim/time.hpp"

namespace sim {

struct LatencyConfig {
    Time feed_ns = 20'000;
    Time proc_ns = 10'000;
    Time to_exchange_ns = 50'000;
    Time from_exchange_ns = 50'000;
    Time jitter_max_ns = 0;  // 0 = no jitter; else uniform in [0, max]
};

class LatencyModel {
public:
    LatencyModel(const LatencyConfig& cfg, std::uint64_t seed) : cfg_(cfg), rng_(seed) {}

    Time order_arrival(Time decided_at, bool md_triggered) {
        Time t = decided_at + (md_triggered ? cfg_.feed_ns : 0) + cfg_.proc_ns + cfg_.to_exchange_ns + jitter();
        t = std::max(t, last_in_);
        last_in_ = t;
        return t;
    }

    Time delivery(Time exchange_ts) {
        Time t = exchange_ts + cfg_.from_exchange_ns + jitter();
        t = std::max(t, last_out_);
        last_out_ = t;
        return t;
    }

    const LatencyConfig& config() const noexcept { return cfg_; }

private:
    Time jitter() {
        if (cfg_.jitter_max_ns <= 0) return 0;
        return static_cast<Time>(rng_() % static_cast<std::uint64_t>(cfg_.jitter_max_ns + 1));
    }
    LatencyConfig cfg_;
    std::mt19937_64 rng_;
    Time last_in_ = 0, last_out_ = 0;
};

}  // namespace sim
