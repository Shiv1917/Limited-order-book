// Deliberately simple reference implementation: std::map<Price, std::list<Order>>
// per side plus std::unordered_map for id lookup.
//
// It is the correctness oracle for the differential fuzzer and the "before"
// row of the benchmark table. It must stay obviously correct, so it favours
// clarity over speed everywhere. Its observable behaviour (events, book state,
// validation order, reject reasons) is the specification the fast book meets.
#pragma once

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ob/types.hpp"

namespace ob {

class RefBook {
public:
    explicit RefBook(const BookConfig& cfg = {}) : band_(cfg.band) {}

    void process(const Message& m, EventBuffer& out);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::optional<Price> best_price(Side s) const { return s == Side::Buy ? best_bid() : best_ask(); }
    std::vector<LevelInfo> depth(Side s, std::size_t n = static_cast<std::size_t>(-1)) const;
    std::optional<OrderInfo> find(OrderId id) const;
    std::size_t order_count() const { return index_.size(); }
    const PriceBand& band() const { return band_; }

    template <class F>
    void for_each_order(Side s, F&& f) const {
        auto visit = [&](const auto& book) {
            for (const auto& [px, q] : book)
                for (const auto& o : q) f(OrderInfo{o.id, s, px, o.qty});
        };
        if (s == Side::Buy) visit(bids_);
        else visit(asks_);
    }

private:
    struct RestingOrder {
        OrderId id;
        Qty qty;
    };
    using Queue = std::list<RestingOrder>;
    using Bids = std::map<Price, Queue, std::greater<>>;
    using Asks = std::map<Price, Queue, std::less<>>;
    struct Locator {
        Side side;
        Price price;
        Queue::iterator it;
    };

    void add(const Message& m, bool is_market, EventBuffer& out);
    void cancel(const Message& m, EventBuffer& out);
    void modify(const Message& m, EventBuffer& out);
    // Runs matching for an accepted order; rests the remainder if GTC.
    void execute(OrderId id, Side side, std::optional<Price> limit, Qty qty, TimeInForce tif, Price px,
                 EventBuffer& out);
    std::uint64_t available(Side taker, std::optional<Price> limit, std::uint64_t need) const;
    void erase_order(OrderId id);

    PriceBand band_;
    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Locator> index_;
};

}  // namespace ob
