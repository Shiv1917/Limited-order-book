// Passive market maker.
//
//   * Quotes the touch (offset_ticks = 0) or k ticks behind it, quote_size each side.
//   * Inventory skew: both quotes move skew_ticks_per_100_shares * position / 100
//     ticks against the position (long -> lower bid and lower, more aggressive, ask).
//   * Never quotes through the opposite touch (bid < best ask, ask > best bid).
//   * Stops quoting a side that could breach max_position; quotes only when the
//     spread is at least min_spread_ticks.
//   * Requotes (cancel + new, which loses queue priority) when the target price
//     moves, at most once per requote_min_ns per side. Tracks pending cancels so
//     it never cancels the same order twice.
//   * Uses OrderApi::position(), which lags the exchange by the return latency.
//   * After a rejected new order (halt, risk limit) a side waits reject_backoff_ns
//     before quoting again, so a halt cannot turn into a reject storm.
//   * At flatten_at: pulls quotes and flattens with IOCs at the touch, then stops.
#pragma once

#include <cstdlib>
#include <set>

#include "sim/params.hpp"
#include "sim/simulator.hpp"

namespace sim {

class MarketMakerStrategy {
public:
    explicit MarketMakerStrategy(const json& p)
        : quote_size_(p.value("quote_size", Qty{100})),
          skew_per_100_(p.value("skew_ticks_per_100_shares", std::int64_t{1})),
          offset_ticks_(p.value("offset_ticks", std::int64_t{0})),
          min_spread_ticks_(p.value("min_spread_ticks", std::int64_t{1})),
          requote_min_(time_or(p, "requote_min_ns", 1'000'000)),
          max_position_(p.value("max_position", std::int64_t{500})),
          start_(time_or(p, "start", 0)),
          flatten_at_(time_or(p, "flatten_at", kNever)),
          flatten_clip_(p.value("flatten_clip", quote_size_)),
          reject_backoff_(time_or(p, "reject_backoff_ns", 100'000'000)) {}

    void on_market_data(const BookView& v, const MdEvent&, OrderApi& api) { decide(v, api); }

    void on_order_event(const BookView& v, const OrderEvent& ev, OrderApi& api) {
        const bool done = ev.type == OrderEventType::Cancelled ||
                          (ev.type == OrderEventType::Fill && ev.leaves == 0) ||
                          (ev.type == OrderEventType::Reject && !ev.for_cancel);
        for (Quote& q : quotes_)
            if (q.id == ev.id && done) {
                q.id = 0;
                if (ev.type == OrderEventType::Reject) q.backoff_until = v.now() + reject_backoff_;
            }
        if (done || (ev.type == OrderEventType::Reject && ev.for_cancel)) cancelling_.erase(ev.id);
        if (ev.id == flatten_id_ && done) flatten_id_ = 0;
        decide(v, api);
    }

    void on_timer(const BookView& v, int, OrderApi& api) {
        timer_at_ = kNever;
        decide(v, api);
    }

private:
    struct Quote {
        OrderId id = 0;
        Price price = 0;
        Time placed = 0;
        Time backoff_until = 0;
    };

    void decide(const BookView& v, OrderApi& api) {
        const Time now = v.now();
        if (now < start_) return;
        const std::int64_t pos = api.position();
        if (now >= flatten_at_) {
            pull(api);
            if (pos != 0 && flatten_id_ == 0 && cancelling_.empty() && v.two_sided()) {
                const Side s = pos > 0 ? Side::Sell : Side::Buy;
                const auto q = static_cast<Qty>(std::min<std::int64_t>(std::llabs(pos), flatten_clip_));
                flatten_id_ = api.send(s, s == Side::Sell ? v.best_bid() : v.best_ask(), q, OrderType::IOC);
            }
            return;
        }
        if (!v.two_sided()) return;  // keep resting quotes; nothing to price against
        const Price tick = v.tick(), bb = v.best_bid(), ba = v.best_ask();
        if ((ba - bb) / tick < min_spread_ticks_) return pull(api);

        const std::int64_t skew = skew_per_100_ * pos / 100;
        Price bid = bb - (offset_ticks_ + skew) * tick;
        Price ask = ba + (offset_ticks_ - skew) * tick;
        bid = std::min(bid, ba - tick);
        ask = std::max(ask, bb + tick);
        const std::int64_t qs = quote_size_;
        manage(v, api, Side::Buy, pos + qs <= max_position_, bid);
        manage(v, api, Side::Sell, -pos + qs <= max_position_, ask);
    }

    void manage(const BookView& v, OrderApi& api, Side s, bool want, Price px) {
        Quote& q = quotes_[static_cast<int>(s)];
        if (!want || !v.valid_price(px)) return cancel(api, q);
        if (q.id == 0) {
            if (v.now() < q.backoff_until) return;
            q.id = api.send(s, px, quote_size_, OrderType::Limit);
            q.price = px;
            q.placed = v.now();
            return;
        }
        if (q.price == px) return;
        if (v.now() - q.placed < requote_min_) {  // throttle; look again when allowed
            const Time at = q.placed + requote_min_;
            if (at < timer_at_) {
                timer_at_ = at;
                api.set_timer(at, 0);
            }
            return;
        }
        cancel(api, q);
        q.id = api.send(s, px, quote_size_, OrderType::Limit);
        q.price = px;
        q.placed = v.now();
    }

    void cancel(OrderApi& api, Quote& q) {
        if (q.id == 0) return;
        if (cancelling_.insert(q.id).second) api.cancel(q.id);
        q.id = 0;
    }

    void pull(OrderApi& api) {
        for (Quote& q : quotes_) cancel(api, q);
    }

    Qty quote_size_;
    std::int64_t skew_per_100_, offset_ticks_, min_spread_ticks_;
    Time requote_min_;
    std::int64_t max_position_;
    Time start_, flatten_at_;
    std::int64_t flatten_clip_;
    Time reject_backoff_;

    Quote quotes_[2];
    std::set<OrderId> cancelling_;
    OrderId flatten_id_ = 0;
    Time timer_at_ = kNever;
};

}  // namespace sim
