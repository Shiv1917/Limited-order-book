// Read-only view of the replayed historical book, as the strategy sees it.
// It wraps a const reference: there is no way to mutate the book through it.
// Strategy orders are never in this book (see docs/sim_design.md), so the
// view shows exactly what history showed.
#pragma once

#include "ob/book.hpp"
#include "sim/time.hpp"

namespace sim {

class BookView {
public:
    BookView(const ob::Book& book, const Time& clock) : book_(book), clock_(clock) {}

    Time now() const noexcept { return clock_; }
    Price tick() const noexcept { return book_.band().tick; }
    bool valid_price(Price px) const noexcept { return book_.band().valid(px); }

    Price best_bid() const noexcept { return book_.best_bid().value_or(kNoBid); }
    Price best_ask() const noexcept { return book_.best_ask().value_or(kNoAsk); }
    bool has_bid() const noexcept { return book_.best_bid().has_value(); }
    bool has_ask() const noexcept { return book_.best_ask().has_value(); }
    bool two_sided() const noexcept { return has_bid() && has_ask(); }

    // Level 0 is the touch. Missing levels give px = kNoBid/kNoAsk, qty = 0.
    Price bid_px(int level) const { return level_info(Side::Buy, level).price; }
    Price ask_px(int level) const { return level_info(Side::Sell, level).price; }
    Qty bid_qty(int level) const { return static_cast<Qty>(level_info(Side::Buy, level).qty); }
    Qty ask_qty(int level) const { return static_cast<Qty>(level_info(Side::Sell, level).qty); }

    // Sum of displayed quantity over the top `levels` levels of one side.
    std::uint64_t depth_qty(Side s, int levels) const {
        std::uint64_t sum = 0;
        int n = 0;
        book_.for_each_level(s, [&](const ob::LevelInfo& L) {
            sum += L.qty;
            return ++n < levels;
        });
        return sum;
    }

    // 2 * mid (integer). 0 if the book is not two-sided.
    Price mid_x2() const noexcept { return two_sided() ? best_bid() + best_ask() : 0; }

private:
    ob::LevelInfo level_info(Side s, int level) const {
        ob::LevelInfo out{s == Side::Buy ? kNoBid : kNoAsk, 0, 0};
        int n = 0;
        book_.for_each_level(s, [&](const ob::LevelInfo& L) {
            if (n++ == level) {
                out = L;
                return false;
            }
            return true;
        });
        return out;
    }
    const ob::Book& book_;
    const Time& clock_;
};

}  // namespace sim
