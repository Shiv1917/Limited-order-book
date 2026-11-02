// Scripted strategy (test only): issues exactly the actions listed in the
// config, each at its time, so every fill can be predicted on paper.
//
//   "actions": [
//     {"t": "34200.000000005", "side": "buy", "price": 1000000, "qty": 20},             // limit
//     {"t": "34200.000000006", "side": "sell", "qty": 50, "type": "ioc", "price": ...},
//     {"t": "34200.000000009", "cancel": 0}                                              // cancel action #0
//   ]
// Prices are in price units (dollars x 10000), like the LOBSTER files.
#pragma once

#include <vector>

#include "sim/params.hpp"
#include "sim/simulator.hpp"

namespace sim {

class ScriptedStrategy {
public:
    struct Action {
        Time t = 0;
        int cancel_of = -1;  // >= 0: cancel the order sent by that action
        Side side = Side::Buy;
        Price price = 0;
        Qty qty = 0;
        OrderType type = OrderType::Limit;
    };

    explicit ScriptedStrategy(const json& p) {
        for (const auto& a : p.at("actions")) {
            Action x;
            x.t = parse_time(a.at("t"));
            if (a.contains("cancel")) {
                x.cancel_of = a.at("cancel").get<int>();
            } else {
                x.side = parse_side(a.at("side"));
                x.qty = a.at("qty").get<Qty>();
                x.price = a.value("price", Price{0});
                const std::string t = a.value("type", std::string("limit"));
                x.type = t == "ioc" ? OrderType::IOC : t == "market" ? OrderType::Market : OrderType::Limit;
            }
            actions_.push_back(x);
        }
        ids_.assign(actions_.size(), 0);
    }

    void on_start(const BookView&, OrderApi& api) {
        for (std::size_t i = 0; i < actions_.size(); ++i) api.set_timer(actions_[i].t, static_cast<int>(i));
    }
    void on_market_data(const BookView&, const MdEvent&, OrderApi&) {}
    void on_order_event(const BookView&, const OrderEvent& ev, OrderApi&) { received.push_back(ev); }
    void on_timer(const BookView&, int i, OrderApi& api) {
        const Action& a = actions_[static_cast<std::size_t>(i)];
        if (a.cancel_of >= 0) api.cancel(ids_.at(static_cast<std::size_t>(a.cancel_of)));
        else ids_[static_cast<std::size_t>(i)] = api.send(a.side, a.price, a.qty, a.type);
    }

    std::vector<OrderEvent> received;  // as delivered, for tests

private:
    std::vector<Action> actions_;
    std::vector<OrderId> ids_;
};

}  // namespace sim
