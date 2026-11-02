// LOBSTER (https://lobsterdata.com) message/orderbook file support.
//
// message file rows : Time, Type, OrderID, Size, Price, Direction
//   Type 1 new limit | 2 partial cancel | 3 delete | 4 visible execution
//        5 hidden execution | 6 cross trade | 7 trading halt
//   Price in 1/10000 dollars; Direction 1 = buy, -1 = sell (side of the
//   *resting* order for executions).
// orderbook file row k = book state after message k:
//   AskPrice1, AskSize1, BidPrice1, BidSize1, ..., up to L levels.
//   Empty levels: ask 9999999999 / bid -9999999999, size 0.
//
// Replay mode "exact": executions are applied as quantity reductions of the
// named resting order, so after every message our book must equal LOBSTER's
// snapshot. Orders that were resting before the file starts are seeded from
// the first snapshot as one synthetic order per level.
#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ob/types.hpp"

namespace ob::lobster {

struct Row {
    double time = 0;
    int type = 0;
    std::uint64_t id = 0;
    std::uint32_t size = 0;
    std::int64_t price = 0;
    int direction = 0;
};

struct Snapshot {
    // levels[i] = {ask_px, ask_sz, bid_px, bid_sz}
    std::vector<std::int64_t> v;
    std::size_t levels() const { return v.size() / 4; }
    std::int64_t ask_px(std::size_t i) const { return v[4 * i]; }
    std::int64_t ask_sz(std::size_t i) const { return v[4 * i + 1]; }
    std::int64_t bid_px(std::size_t i) const { return v[4 * i + 2]; }
    std::int64_t bid_sz(std::size_t i) const { return v[4 * i + 3]; }
};

inline constexpr std::int64_t kEmptyAsk = 9999999999LL;
inline constexpr std::int64_t kEmptyBid = -9999999999LL;

bool parse_row(const char* line, Row& r);
bool parse_snapshot(const char* line, Snapshot& s);
std::vector<Row> load_messages(const std::string& path, std::string* err = nullptr);
std::vector<Snapshot> load_snapshots(const std::string& path, std::string* err = nullptr);

struct ReplayStats {
    std::size_t messages = 0, adds = 0, partial_cancels = 0, deletes = 0, executions = 0, hidden = 0, other = 0;
    std::size_t seeded_levels = 0, unknown_refs = 0, rejects = 0;
    std::size_t checkpoints = 0, l1_mismatches = 0, depth_mismatches = 0;
    std::string first_mismatch;
};

// Converts LOBSTER rows to engine messages and verifies book state against
// the snapshots (every `check_every` messages; 1 = every message).
// `BookT` is any book exposing process/depth/find (RefBook or a BasicBook).
template <class BookT>
class Replayer {
public:
    Replayer(BookT& book, std::size_t check_levels) : book_(book), check_levels_(check_levels) {}

    // Seeds pre-existing liquidity from snapshot 0 with the effect of row 0 undone.
    void seed(const Snapshot& s0, const Row& r0);
    // Applies one row; appends generated engine messages to `sink` if given.
    void apply(const Row& r, std::vector<Message>* sink = nullptr);
    // Compares the top `check_levels` levels to the snapshot.
    bool check(const Snapshot& s, std::size_t row_index);

    ReplayStats stats;

private:
    void send(const Message& m, std::vector<Message>* sink);
    void reduce(std::uint64_t id, Side side, std::int64_t price, std::uint32_t size, std::vector<Message>* sink);

    BookT& book_;
    std::size_t check_levels_;
    EventBuffer out_{1 << 12};
    std::unordered_map<std::uint64_t, OrderId> seeded_;  // key: (side<<63)|price -> synthetic id
    OrderId next_synth_ = (OrderId{1} << 62);
};

// ---- implementation ---------------------------------------------------------

template <class BookT>
void Replayer<BookT>::send(const Message& m, std::vector<Message>* sink) {
    out_.clear();
    book_.process(m, out_);
    for (const Event& e : out_)
        if (e.type == EventType::Reject) ++stats.rejects;
    if (sink) sink->push_back(m);
}

template <class BookT>
void Replayer<BookT>::seed(const Snapshot& s0, const Row& r0) {
    auto key = [](Side s, std::int64_t px) {
        return (static_cast<std::uint64_t>(s) << 63) ^ static_cast<std::uint64_t>(px);
    };
    // Undo row 0 on the aggregated levels.
    std::unordered_map<std::uint64_t, std::int64_t> adjust;
    const Side s = r0.direction == 1 ? Side::Buy : Side::Sell;
    if (r0.type == 1) adjust[key(s, r0.price)] -= r0.size;
    else if (r0.type == 2 || r0.type == 3 || r0.type == 4) adjust[key(s, r0.price)] += r0.size;

    auto seed_level = [&](Side side, std::int64_t px, std::int64_t sz) {
        if (px == kEmptyAsk || px == kEmptyBid) return;
        sz += adjust[key(side, px)];
        adjust.erase(key(side, px));
        if (sz <= 0) return;
        const OrderId id = next_synth_++;
        seeded_[key(side, px)] = id;
        send(Message::add(id, side, px, static_cast<Qty>(sz)), nullptr);
        ++stats.seeded_levels;
    };
    for (std::size_t i = 0; i < s0.levels(); ++i) {
        seed_level(Side::Sell, s0.ask_px(i), s0.ask_sz(i));
        seed_level(Side::Buy, s0.bid_px(i), s0.bid_sz(i));
    }
    // A deletion/execution in row 0 at a level not shown after it (it emptied).
    for (auto& [k, sz] : adjust) {
        if (sz <= 0) continue;
        const Side side = (k >> 63) ? Side::Sell : Side::Buy;
        const auto px = static_cast<std::int64_t>(k & ~(std::uint64_t{1} << 63));
        const OrderId id = next_synth_++;
        seeded_[k] = id;
        send(Message::add(id, side, px, static_cast<Qty>(sz)), nullptr);
        ++stats.seeded_levels;
    }
}

template <class BookT>
void Replayer<BookT>::reduce(std::uint64_t id, Side side, std::int64_t price, std::uint32_t size,
                             std::vector<Message>* sink) {
    OrderId target = id;
    auto info = book_.find(target);
    if (!info) {
        // Order predates the file: take it out of the seeded aggregate at that level.
        const std::uint64_t k = (static_cast<std::uint64_t>(side) << 63) ^ static_cast<std::uint64_t>(price);
        auto it = seeded_.find(k);
        if (it == seeded_.end() || !(info = book_.find(it->second))) {
            ++stats.unknown_refs;
            return;
        }
        target = it->second;
    }
    if (size >= info->qty) send(Message::cancel(target), sink);
    else send(Message::modify(target, info->price, info->qty - size), sink);  // decrease keeps priority
}

template <class BookT>
void Replayer<BookT>::apply(const Row& r, std::vector<Message>* sink) {
    ++stats.messages;
    const Side side = r.direction == 1 ? Side::Buy : Side::Sell;
    switch (r.type) {
        case 1:
            ++stats.adds;
            send(Message::add(r.id, side, r.price, r.size), sink);
            break;
        case 2:
            ++stats.partial_cancels;
            reduce(r.id, side, r.price, r.size, sink);
            break;
        case 3: {
            ++stats.deletes;
            auto info = book_.find(r.id);
            if (info) send(Message::cancel(r.id), sink);
            else reduce(r.id, side, r.price, r.size, sink);
            break;
        }
        case 4:
            ++stats.executions;
            reduce(r.id, side, r.price, r.size, sink);
            break;
        case 5: ++stats.hidden; break;  // hidden liquidity: not in the visible book
        default: ++stats.other; break;  // cross trades, halts
    }
}

template <class BookT>
bool Replayer<BookT>::check(const Snapshot& s, std::size_t row_index) {
    ++stats.checkpoints;
    const std::size_t n = std::min(check_levels_, s.levels());
    const auto asks = book_.depth(Side::Sell, n);
    const auto bids = book_.depth(Side::Buy, n);
    bool ok = true;
    for (std::size_t i = 0; i < n; ++i) {
        const bool a_ok = i < asks.size() ? (asks[i].price == s.ask_px(i) && static_cast<std::int64_t>(asks[i].qty) == s.ask_sz(i))
                                         : (s.ask_px(i) == kEmptyAsk || s.ask_sz(i) == 0);
        const bool b_ok = i < bids.size() ? (bids[i].price == s.bid_px(i) && static_cast<std::int64_t>(bids[i].qty) == s.bid_sz(i))
                                         : (s.bid_px(i) == kEmptyBid || s.bid_sz(i) == 0);
        if (!(a_ok && b_ok)) {
            if (i == 0) ++stats.l1_mismatches;
            ++stats.depth_mismatches;
            if (stats.first_mismatch.empty())
                stats.first_mismatch = "row " + std::to_string(row_index) + " level " + std::to_string(i + 1);
            ok = false;
            break;
        }
    }
    return ok;
}

}  // namespace ob::lobster
