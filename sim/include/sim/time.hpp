// Time, price and quantity conventions for the simulator.
//
//   Time  : int64 nanoseconds after midnight (exchange clock).
//   Price : int64 "price units" = 1/10000 dollar (LOBSTER/ITCH convention).
//           The tick is 100 units ($0.01). Mid prices are carried as 2*mid so
//           they stay integer.
//   Qty   : uint32 shares.
// No floating point touches cash, inventory or PnL; dollars appear only when
// results are formatted for output.
#pragma once

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

#include "ob/types.hpp"

namespace sim {

using Time = std::int64_t;
using Price = ob::Price;
using Qty = ob::Qty;
using OrderId = ob::OrderId;
using Side = ob::Side;

inline constexpr Time kNever = std::numeric_limits<Time>::max();
inline constexpr Time kSecond = 1'000'000'000;
inline constexpr Price kUnitsPerDollar = 10'000;
inline constexpr Price kNoBid = std::numeric_limits<Price>::min();
inline constexpr Price kNoAsk = std::numeric_limits<Price>::max();

// Strategy order ids start here so they can never collide with LOBSTER ids
// (exchange ids are far below 2^40; seeded synthetic orders use 2^62+).
inline constexpr OrderId kStrategyIdBase = OrderId{1} << 40;

constexpr int sign(Side s) noexcept { return s == Side::Buy ? 1 : -1; }

// Exact decimal rendering of v / 10^decimals, e.g. fmt_fixed(1234500, 4) == "123.4500".
inline std::string fmt_fixed(std::int64_t v, int decimals) {
    const bool neg = v < 0;
    // Work in unsigned so INT64_MIN does not overflow.
    std::uint64_t a = neg ? (~static_cast<std::uint64_t>(v) + 1) : static_cast<std::uint64_t>(v);
    std::uint64_t p = 1;
    for (int i = 0; i < decimals; ++i) p *= 10;
    char buf[48];
    if (decimals == 0)
        std::snprintf(buf, sizeof buf, "%s%llu", neg ? "-" : "", static_cast<unsigned long long>(a));
    else
        std::snprintf(buf, sizeof buf, "%s%llu.%0*llu", neg ? "-" : "", static_cast<unsigned long long>(a / p), decimals,
                      static_cast<unsigned long long>(a % p));
    return buf;
}

inline std::string fmt_dollars(std::int64_t units) { return fmt_fixed(units, 4); }
// 2*mid in price units -> dollars with 5 decimals (exact).
inline std::string fmt_mid_x2(std::int64_t mid_x2) { return fmt_fixed(mid_x2 * 5, 5); }
inline std::string fmt_time(Time t) { return fmt_fixed(t, 9); }  // seconds after midnight

}  // namespace sim
