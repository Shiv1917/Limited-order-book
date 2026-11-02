// Probe strategy (validation layer 5): every `period` buy `qty` at the best
// bid, cancel it `hold` later. Deliberately simple so an independent Python
// model (ref_py/fill_model.py) can replay the same day and the fills can be
// compared line by line.
#pragma once

#include <deque>

#include "sim/params.hpp"
#include "sim/simulator.hpp"

namespace sim {

class ProbeStrategy {
public:
    explicit ProbeStrategy(const json& p)
        : period_(time_or(p, "period_ns", 10 * kSecond)),
          hold_(time_or(p, "hold_ns", 5 * kSecond)),
          start_(time_or(p, "start", 0)),
          stop_(time_or(p, "stop", kNever)),
          qty_(p.value("qty", Qty{100})) {}

    void on_start(const BookView& v, OrderApi& api) { api.set_timer(std::max(start_, v.now()), kSend); }
    void on_market_data(const BookView&, const MdEvent&, OrderApi&) {}
    void on_order_event(const BookView&, const OrderEvent&, OrderApi&) {}
    void on_timer(const BookView& v, int id, OrderApi& api) {
        if (id == kCancel) {
            api.cancel(open_.front());
            open_.pop_front();
            return;
        }
        if (v.now() >= stop_) return;
        if (v.has_bid()) {
            open_.push_back(api.send(Side::Buy, v.best_bid(), qty_, OrderType::Limit));
            api.set_timer(v.now() + hold_, kCancel);
        }
        api.set_timer(v.now() + period_, kSend);
    }

private:
    static constexpr int kSend = 0, kCancel = 1;
    Time period_, hold_, start_, stop_;
    Qty qty_;
    std::deque<OrderId> open_;
};

}  // namespace sim
