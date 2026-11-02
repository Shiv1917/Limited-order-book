// Does nothing. Used by `ob-sim check` and as the no-impact baseline.
#pragma once

#include "sim/simulator.hpp"

namespace sim {

struct NullStrategy {
    void on_market_data(const BookView&, const MdEvent&, OrderApi&) {}
    void on_order_event(const BookView&, const OrderEvent&, OrderApi&) {}
    void on_timer(const BookView&, int, OrderApi&) {}
};

}  // namespace sim
