// Cash, inventory and PnL accounting. Updated only by fills, all integer.
//
//   cash      price units, includes fees (fee > 0 paid, < 0 rebate)
//   basis     signed cost of the open position, sum of q*p over open lots
//   realized  average-cost realized PnL, excluding fees
// Identity (exact, tested): realized + unrealized - fees == equity - equity_0,
// with unrealized = position*mid - basis and equity = cash + position*mid.
// Partial closes remove basis*closed/|position| (integer division), so the
// realized/unrealized split can be off by < 1 price unit per partial close;
// their sum is always exact.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "sim/time.hpp"

namespace sim {

class Portfolio {
public:
    void on_fill(Side side, Price px, Qty qty, std::int64_t fee) {
        const std::int64_t q = sign(side) * static_cast<std::int64_t>(qty);
        cash_ -= q * px + fee;
        fees_ += fee;
        turnover_ += qty;
        ++fills_;

        if (position_ != 0 && (q > 0) != (position_ > 0)) {  // closes (part of) the position
            const std::int64_t closed = std::min(std::llabs(q), std::llabs(position_));
            const std::int64_t q_close = q > 0 ? closed : -closed;
            const std::int64_t removed = basis_ * closed / std::llabs(position_);
            realized_ += -q_close * px - removed;
            basis_ -= removed;
            position_ += q_close;
            const std::int64_t q_open = q - q_close;  // non-zero only when flipping through zero
            basis_ += q_open * px;
            position_ += q_open;
        } else {
            basis_ += q * px;
            position_ += q;
        }
        max_abs_position_ = std::max(max_abs_position_, std::llabs(position_));
    }

    std::int64_t position() const noexcept { return position_; }
    std::int64_t cash() const noexcept { return cash_; }
    std::int64_t fees() const noexcept { return fees_; }
    std::int64_t realized() const noexcept { return realized_; }
    std::int64_t basis() const noexcept { return basis_; }
    std::int64_t max_abs_position() const noexcept { return max_abs_position_; }
    std::uint64_t turnover() const noexcept { return turnover_; }
    std::uint64_t fills() const noexcept { return fills_; }

    // x2 quantities: multiplied by 2 so a half-tick mid stays integer.
    std::int64_t unrealized_x2(Price mid_x2) const noexcept { return position_ * mid_x2 - 2 * basis_; }
    std::int64_t equity_x2(Price mid_x2) const noexcept { return 2 * cash_ + position_ * mid_x2; }

private:
    std::int64_t cash_ = 0, position_ = 0, basis_ = 0, realized_ = 0, fees_ = 0, max_abs_position_ = 0;
    std::uint64_t turnover_ = 0, fills_ = 0;
};

}  // namespace sim
