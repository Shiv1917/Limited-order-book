// Priority queue of simulator-generated events, keyed (ts, priority, seq).
//
// Historical market data is NOT in this queue: it streams from a vector and
// the loop compares its timestamp with top(). Market data has priority 0, so
// at equal timestamps it always runs before anything here.
#pragma once

#include <cstdint>
#include <queue>
#include <vector>

#include "sim/events.hpp"

namespace sim {

enum class Prio : std::uint8_t { MarketData = 0, OrderArrive = 1, Deliver = 2, Timer = 3 };

struct SimEvent {
    Time ts = 0;
    std::uint64_t seq = 0;
    Prio prio = Prio::Timer;
    OrderRequest req{};  // OrderArrive
    OrderEvent oev{};    // Deliver
    int timer_id = 0;    // Timer
};

class EventQueue {
public:
    void push(SimEvent e) {
        e.seq = next_seq_++;
        heap_.push(e);
    }
    bool empty() const noexcept { return heap_.empty(); }
    std::size_t size() const noexcept { return heap_.size(); }
    const SimEvent& top() const { return heap_.top(); }
    Time top_ts() const noexcept { return heap_.empty() ? kNever : heap_.top().ts; }
    SimEvent pop() {
        SimEvent e = heap_.top();
        heap_.pop();
        return e;
    }

private:
    struct Later {
        bool operator()(const SimEvent& a, const SimEvent& b) const noexcept {
            if (a.ts != b.ts) return a.ts > b.ts;
            if (a.prio != b.prio) return a.prio > b.prio;
            return a.seq > b.seq;  // global counter: no two events ever compare equal
        }
    };
    std::priority_queue<SimEvent, std::vector<SimEvent>, Later> heap_;
    std::uint64_t next_seq_ = 0;
};

}  // namespace sim
