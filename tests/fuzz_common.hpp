// Differential fuzzing: every message goes to the reference book and to every
// fast-book stage; events and full book state must match after each message.
#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "book_types.hpp"
#include "synthetic.hpp"

namespace obtest {

struct DiffResult {
    bool ok = true;
    std::size_t messages = 0;
    std::string error;
    std::size_t trades = 0;
    std::size_t by_reason[16] = {};  // event counts per Reason (coverage report)
};

template <class Fast>
bool same_state(const RefBook& ref, const Fast& fast, std::string& why) {
    if (ref.best_bid() != fast.best_bid() || ref.best_ask() != fast.best_ask()) {
        why = "top of book differs";
        return false;
    }
    if (ref.order_count() != fast.order_count()) {
        why = "order count differs";
        return false;
    }
    if (full_state(ref) != full_state(fast)) {
        why = "queue contents differ";
        return false;
    }
    if (ref.depth(Side::Buy) != fast.depth(Side::Buy) || ref.depth(Side::Sell) != fast.depth(Side::Sell)) {
        why = "depth differs";
        return false;
    }
    return true;
}

// `full_every`: how often to compare full queue state (top of book and event
// streams are compared on every message).
inline DiffResult run_differential(const SynthConfig& cfg, std::size_t n, std::size_t full_every = 1) {
    DiffResult res;
    SyntheticFlow gen(cfg);
    const BookConfig bc{cfg.band, 256};
    RefBook ref(bc);
    BookFlatLevels s1(bc);
    BookPool s2(bc);
    BookFlatHash s3(bc);
    Book s4(bc);
    EventBuffer er, e1, e2, e3, e4;
    std::vector<Event> want;

    auto fail = [&](const Message& m, const std::string& what) {
        std::ostringstream os;
        os << "seed " << cfg.seed << " msg #" << res.messages << " " << m << ": " << what;
        res.ok = false;
        res.error = os.str();
    };

    for (std::size_t i = 0; i < n; ++i) {
        const Message m = gen.next();
        res.messages = i;
        er.clear(), e1.clear(), e2.clear(), e3.clear(), e4.clear();
        ref.process(m, er);
        s1.process(m, e1);
        s2.process(m, e2);
        s3.process(m, e3);
        s4.process(m, e4);
        want.assign(er.begin(), er.end());
        for (const Event& e : want) {
            if (e.type == EventType::Trade) ++res.trades;
            else ++res.by_reason[static_cast<int>(e.reason)];
        }
        auto evs = [](const EventBuffer& b) { return std::vector<Event>(b.begin(), b.end()); };
        const char* names[] = {"flat-levels", "pool", "flat-hash", "packed"};
        const std::vector<Event> got[] = {evs(e1), evs(e2), evs(e3), evs(e4)};
        for (int k = 0; k < 4; ++k)
            if (got[k] != want) {
                fail(m, std::string("events differ for ") + names[k] + "\nref:\n" + dump(want) + "fast:\n" + dump(got[k]));
                return res;
            }
        std::string why;
        const bool full = full_every != 0 && i % full_every == 0;
        auto cmp = [&](const auto& fast, const char* nm) {
            if (full) {
                if (!same_state(ref, fast, why)) return fail(m, std::string(nm) + ": " + why), false;
            } else if (ref.best_bid() != fast.best_bid() || ref.best_ask() != fast.best_ask()) {
                return fail(m, std::string(nm) + ": top of book differs"), false;
            }
            return true;
        };
        if (!cmp(s1, "flat-levels") || !cmp(s2, "pool") || !cmp(s3, "flat-hash") || !cmp(s4, "packed")) return res;
        if (full && i % (full_every * 64) == 0 && !s4.validate(&why)) {
            fail(m, "invariant: " + why);
            return res;
        }
    }
    res.messages = n;
    return res;
}

}  // namespace obtest
