// Order-imbalance taker.
//
//   I = (sum bid qty - sum ask qty) / (sum bid qty + sum ask qty) over the top L levels.
//   I > +theta: buy qty with an IOC at the ask; I < -theta: sell at the bid.
//   Exit with an IOC at the touch after hold_ns (a timer); if the exit does not
//   fully fill, retry with a market order. One position at a time, then a
//   cooldown before the next signal.
// Expected to lose after fees: it exists to exercise the taker path, the fee
// model and own_taken bookkeeping. The diagnostics show whether the signal had
// predictive power before costs.
#pragma once

#include <cstdlib>

#include "sim/params.hpp"
#include "sim/simulator.hpp"

namespace sim {

class ImbalanceStrategy {
public:
    explicit ImbalanceStrategy(const json& p)
        : levels_(p.value("levels", 5)),
          theta_(p.value("theta", 0.6)),
          qty_(p.value("qty", Qty{100})),
          hold_(time_or(p, "hold_ns", 10 * kSecond)),
          cooldown_(time_or(p, "cooldown_ns", kSecond)),
          start_(time_or(p, "start", 0)),
          stop_(time_or(p, "stop", kNever)) {}

    void on_market_data(const BookView& v, const MdEvent&, OrderApi& api) {
        if (state_ != State::Flat || v.now() < cooldown_until_ || v.now() < start_ || v.now() >= stop_) return;
        if (!v.two_sided()) return;
        const auto b = static_cast<double>(v.depth_qty(Side::Buy, levels_));
        const auto a = static_cast<double>(v.depth_qty(Side::Sell, levels_));
        if (a + b <= 0) return;
        const double imb = (b - a) / (a + b);
        if (imb > theta_) order_id_ = api.send(Side::Buy, v.best_ask(), qty_, OrderType::IOC);
        else if (imb < -theta_) order_id_ = api.send(Side::Sell, v.best_bid(), qty_, OrderType::IOC);
        else return;
        state_ = State::Entering;
    }

    void on_order_event(const BookView& v, const OrderEvent& ev, OrderApi& api) {
        if (ev.id != order_id_) return;
        const bool done = ev.type == OrderEventType::Cancelled || ev.type == OrderEventType::Reject ||
                          (ev.type == OrderEventType::Fill && ev.leaves == 0);
        if (!done) return;
        order_id_ = 0;
        const std::int64_t pos = api.position();
        if (state_ == State::Entering) {
            if (pos != 0) {
                state_ = State::Holding;
                api.set_timer(v.now() + hold_, 0);
            } else {
                flat(v);
            }
        } else if (state_ == State::Exiting) {
            if (pos != 0) exit(v, api, OrderType::Market);
            else flat(v);
        }
    }

    void on_timer(const BookView& v, int, OrderApi& api) {
        if (state_ == State::Holding) exit(v, api, OrderType::IOC);
    }

private:
    enum class State { Flat, Entering, Holding, Exiting };

    void exit(const BookView& v, OrderApi& api, OrderType type) {
        const std::int64_t pos = api.position();
        const Side s = pos > 0 ? Side::Sell : Side::Buy;
        const Price px = s == Side::Sell ? v.best_bid() : v.best_ask();
        if (type == OrderType::IOC && (px == kNoBid || px == kNoAsk)) type = OrderType::Market;
        order_id_ = api.send(s, px, static_cast<Qty>(std::llabs(pos)), type);
        state_ = State::Exiting;
    }

    void flat(const BookView& v) {
        state_ = State::Flat;
        cooldown_until_ = v.now() + cooldown_;
    }

    int levels_;
    double theta_;
    Qty qty_;
    Time hold_, cooldown_, start_, stop_;
    State state_ = State::Flat;
    OrderId order_id_ = 0;
    Time cooldown_until_ = 0;
};

}  // namespace sim
