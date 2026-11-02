// Helpers shared by the book tests.
#pragma once

#include <sstream>
#include <string>
#include <vector>

#include "ob/book.hpp"
#include "ref_book.hpp"

namespace obtest {

using namespace ob;

inline BookConfig test_config() { return BookConfig{PriceBand{10'000, 100, 1024}, 1024}; }

// Price helper: tick index -> price in the test band.
inline Price P(int ticks) { return 10'000 + 100 * static_cast<Price>(ticks); }

template <class B>
struct Harness {
    B book{test_config()};
    EventBuffer out{256};

    std::vector<Event> send(const Message& m) {
        out.clear();
        book.process(m, out);
        return std::vector<Event>(out.begin(), out.end());
    }
};

// Snapshot of everything observable about a book: every resting order in
// priority order, per side.
template <class B>
std::vector<OrderInfo> full_state(const B& b) {
    std::vector<OrderInfo> v;
    b.for_each_order(Side::Buy, [&](const OrderInfo& o) { v.push_back(o); });
    v.push_back(OrderInfo{0, Side::Buy, -1, 0});  // separator
    b.for_each_order(Side::Sell, [&](const OrderInfo& o) { v.push_back(o); });
    return v;
}

inline std::string dump(const std::vector<Event>& evs) {
    std::ostringstream os;
    for (const auto& e : evs) os << "  " << e << "\n";
    return os.str();
}

}  // namespace obtest
