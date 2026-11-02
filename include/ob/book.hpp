// The fast matching engine: price-time priority over a flat price-level array.
//
//   * Price levels: flat array indexed by (price - base) / tick, one per side.
//     Each level is an intrusive doubly-linked FIFO (head/tail pool indices)
//     plus aggregate quantity and order count.
//   * Best bid/ask: cached level indices. When the best level empties, the
//     next best is found with a hierarchical occupancy bitset (bounded scan).
//   * Orders: index-addressed storage policy (pool or new/delete).
//   * Lookup: OrderId -> index map policy (flat open-addressing or std).
//   * Output: events appended to a caller-supplied pre-allocated EventBuffer.
//
// The class is a template over three policies so that each optimisation step
// in docs/benchmarks.md is literally the same algorithm with one knob turned.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ob/flat_map.hpp"
#include "ob/level_bitset.hpp"
#include "ob/pool.hpp"
#include "ob/types.hpp"

namespace ob {

// "Naive" layout: wide types, cold fields interleaved with hot ones, implicit
// padding. 80 bytes -> spans two cache lines for 3 of every 4 orders.
struct OrderLoose {
    OrderId id = 0;
    Side side = Side::Buy;
    Price price = 0;
    std::uint64_t qty = 0;
    std::uint64_t orig_qty = 0;
    TimeInForce tif = TimeInForce::GTC;
    std::uint64_t timestamp = 0;
    std::uint32_t level = 0;
    std::uint32_t next = kNil;
    std::uint64_t owner = 0;
    std::uint32_t prev = kNil;
};

// Tuned layout: only what matching needs, 32 bytes, 32-byte aligned so an
// order never straddles a cache line and two orders share each line. Price is
// implied by the level index; cold data (timestamps, owner) lives elsewhere.
struct alignas(32) OrderPacked {
    OrderId id = 0;
    std::uint32_t qty = 0;
    std::uint32_t next = kNil;
    std::uint32_t prev = kNil;
    std::uint32_t level = 0;
    Side side = Side::Buy;
};
static_assert(sizeof(OrderPacked) == 32);
static_assert(sizeof(OrderLoose) == 80);

struct Level {
    std::uint32_t head = kNil;
    std::uint32_t tail = kNil;
    std::uint32_t count = 0;
    std::uint64_t total = 0;
};

template <class OrderT, template <class> class StorageT, class IdMapT>
class BasicBook {
public:
    using Order = OrderT;

    explicit BasicBook(const BookConfig& cfg = {})
        : band_(cfg.band), pool_(cfg.max_orders), ids_(cfg.max_orders) {
        for (int s = 0; s < 2; ++s) {
            levels_[s].assign(band_.levels, Level{});
            bits_[s].resize(band_.levels);
            best_[s] = kNil;
        }
    }

    BasicBook(const BasicBook&) = delete;
    BasicBook& operator=(const BasicBook&) = delete;

    void process(const Message& m, EventBuffer& out) {
        switch (m.type) {
            case MsgType::Add: on_add(m, out); return;
            case MsgType::Cancel: on_cancel(m, out); return;
            case MsgType::Modify: on_modify(m, out); return;
            case MsgType::Market: on_market(m, out); return;
        }
    }

    // ---- queries ------------------------------------------------------------
    std::optional<Price> best_bid() const noexcept { return best_price(Side::Buy); }
    std::optional<Price> best_ask() const noexcept { return best_price(Side::Sell); }

    std::optional<Price> best_price(Side s) const noexcept {
        const auto b = best_[idx(s)];
        if (b == kNil) return std::nullopt;
        return band_.to_price(b);
    }

    // Top `n` levels from best to worst.
    std::vector<LevelInfo> depth(Side s, std::size_t n = static_cast<std::size_t>(-1)) const {
        std::vector<LevelInfo> v;
        for (auto b = best_[idx(s)]; b != kNil && v.size() < n; b = next_level(s, b)) {
            const Level& L = levels_[idx(s)][b];
            v.push_back(LevelInfo{band_.to_price(b), L.total, L.count});
        }
        return v;
    }

    // Visits every resting order on side s in priority order.
    template <class F>
    void for_each_order(Side s, F&& f) const {
        for (auto b = best_[idx(s)]; b != kNil; b = next_level(s, b))
            for (auto i = levels_[idx(s)][b].head; i != kNil; i = pool_[i].next) {
                const Order& o = pool_[i];
                f(OrderInfo{o.id, o.side, band_.to_price(o.level), static_cast<Qty>(o.qty)});
            }
    }

    std::optional<OrderInfo> find(OrderId id) const {
        const auto i = ids_.find(id);
        if (i == kNil) return std::nullopt;
        const Order& o = pool_[i];
        return OrderInfo{o.id, o.side, band_.to_price(o.level), static_cast<Qty>(o.qty)};
    }

    // Is `id` the first order in the queue at the best price on its side?
    bool is_front_of_best(OrderId id) const {
        const auto i = ids_.find(id);
        if (i == kNil) return false;
        const Order& o = pool_[i];
        return best_[idx(o.side)] == o.level && levels_[idx(o.side)][o.level].head == i;
    }

    std::size_t order_count() const noexcept { return ids_.size(); }
    const PriceBand& band() const noexcept { return band_; }

    // Full invariant check (debug/fuzz only; O(levels + orders)).
    bool validate(std::string* why = nullptr) const;

private:
    static constexpr int idx(Side s) noexcept { return static_cast<int>(s); }

    // Next level away from the touch on resting side s, strictly after `b`.
    std::uint32_t next_level(Side s, std::uint32_t b) const noexcept {
        if (s == Side::Sell) {
            const auto n = bits_[1].next(std::size_t{b} + 1);
            return n == LevelBitset::npos ? kNil : static_cast<std::uint32_t>(n);
        }
        if (b == 0) return kNil;
        const auto p = bits_[0].prev(std::size_t{b} - 1);
        return p == LevelBitset::npos ? kNil : static_cast<std::uint32_t>(p);
    }

    // Does resting level b on the side opposite taker S satisfy the taker's limit?
    template <Side S>
    static bool crosses(std::uint32_t b, std::uint32_t limit) noexcept {
        if constexpr (S == Side::Buy) return b <= limit;
        else return b >= limit;
    }

    void on_add(const Message& m, EventBuffer& out) {
        if (m.qty == 0) [[unlikely]]
            return out.push(make_reject(m.id, m.side, Reason::InvalidQty));
        std::uint32_t lvl;
        if (!band_.to_index(m.price, lvl)) [[unlikely]]
            return out.push(make_reject(m.id, m.side, Reason::InvalidPrice));
        if (ids_.contains(m.id)) [[unlikely]]
            return out.push(make_reject(m.id, m.side, Reason::DuplicateId));
        out.push(make_ack(m.id, m.side, m.price, m.qty, Reason::New));
        if (m.side == Side::Buy) execute<Side::Buy>(m.id, lvl, m.qty, m.tif, m.price, out);
        else execute<Side::Sell>(m.id, lvl, m.qty, m.tif, m.price, out);
    }

    void on_market(const Message& m, EventBuffer& out) {
        if (m.qty == 0) [[unlikely]]
            return out.push(make_reject(m.id, m.side, Reason::InvalidQty));
        if (ids_.contains(m.id)) [[unlikely]]
            return out.push(make_reject(m.id, m.side, Reason::DuplicateId));
        out.push(make_ack(m.id, m.side, 0, m.qty, Reason::New));
        const auto tif = m.tif == TimeInForce::FOK ? TimeInForce::FOK : TimeInForce::IOC;
        if (m.side == Side::Buy) execute<Side::Buy>(m.id, band_.levels - 1, m.qty, tif, 0, out);
        else execute<Side::Sell>(m.id, 0, m.qty, tif, 0, out);
    }

    void on_cancel(const Message& m, EventBuffer& out) {
        const auto i = ids_.find(m.id);
        if (i == kNil) [[unlikely]]
            return out.push(make_reject(m.id, m.side, Reason::UnknownOrder));
        const Order& o = pool_[i];
        const Event ev = make_cancel(m.id, o.side, band_.to_price(o.level), static_cast<Qty>(o.qty), Reason::UserCancel);
        remove(i);
        out.push(ev);
    }

    // Modify semantics (documented in docs/design.md):
    //   same price and qty <= current remaining  -> reduce in place, keeps priority
    //   price change or qty increase             -> cancel/replace: loses priority,
    //                                               may cross and trade immediately.
    void on_modify(const Message& m, EventBuffer& out) {
        const auto i = ids_.find(m.id);
        if (i == kNil) [[unlikely]]
            return out.push(make_reject(m.id, m.side, Reason::UnknownOrder));
        if (m.qty == 0) [[unlikely]]
            return out.push(make_reject(m.id, m.side, Reason::InvalidQty));
        std::uint32_t lvl;
        if (!band_.to_index(m.price, lvl)) [[unlikely]]
            return out.push(make_reject(m.id, m.side, Reason::InvalidPrice));
        Order& o = pool_[i];
        const Side s = o.side;
        if (lvl == o.level && m.qty <= o.qty) {
            levels_[idx(s)][lvl].total -= (o.qty - m.qty);
            o.qty = m.qty;
            return out.push(make_ack(m.id, s, m.price, m.qty, Reason::Modified));
        }
        remove(i);
        out.push(make_ack(m.id, s, m.price, m.qty, Reason::Modified));
        if (s == Side::Buy) execute<Side::Buy>(m.id, lvl, m.qty, TimeInForce::GTC, m.price, out);
        else execute<Side::Sell>(m.id, lvl, m.qty, TimeInForce::GTC, m.price, out);
    }

    template <Side S>
    void execute(OrderId id, std::uint32_t limit, Qty qty, TimeInForce tif, Price px, EventBuffer& out) {
        if (tif == TimeInForce::FOK && !can_fill<S>(limit, qty)) [[unlikely]]
            return out.push(make_cancel(id, S, px, qty, Reason::FokUnfillable));
        qty = match<S>(id, limit, qty, out);
        if (qty == 0) return;
        if (tif != TimeInForce::GTC) return out.push(make_cancel(id, S, px, qty, Reason::Unfilled));
        rest<S>(id, limit, qty);
    }

    template <Side S>
    bool can_fill(std::uint32_t limit, Qty qty) const noexcept {
        constexpr Side O = opposite(S);
        std::uint64_t avail = 0;
        for (auto b = best_[idx(O)]; b != kNil && crosses<S>(b, limit); b = next_level(O, b)) {
            avail += levels_[idx(O)][b].total;
            if (avail >= qty) return true;
        }
        return false;
    }

    // Matches taker against the opposite side; returns the unfilled remainder.
    template <Side S>
    Qty match(OrderId taker, std::uint32_t limit, Qty qty, EventBuffer& out) {
        constexpr Side O = opposite(S);
        constexpr int oi = idx(O);
        while (qty != 0) {
            const std::uint32_t b = best_[oi];
            if (b == kNil || !crosses<S>(b, limit)) break;
            Level& L = levels_[oi][b];
            const Price px = band_.to_price(b);
            while (qty != 0 && L.head != kNil) {
                const std::uint32_t h = L.head;
                Order& o = pool_[h];
                const Qty fill = std::min<Qty>(qty, static_cast<Qty>(o.qty));
                out.push(make_trade(taker, o.id, S, px, fill));
                o.qty -= fill;
                L.total -= fill;
                qty -= fill;
                if (o.qty == 0) {
                    L.head = o.next;
                    if (L.head != kNil) pool_[L.head].prev = kNil;
                    else L.tail = kNil;
                    --L.count;
                    ids_.erase(o.id);
                    pool_.free(h);
                }
            }
            if (L.head == kNil) {
                bits_[oi].clear(b);
                best_[oi] = next_level(O, b);
            }
        }
        return qty;
    }

    template <Side S>
    void rest(OrderId id, std::uint32_t lvl, Qty qty) {
        constexpr int si = idx(S);
        const std::uint32_t i = pool_.alloc();
        Order& o = pool_[i];
        o.id = id;
        o.qty = qty;
        o.level = lvl;
        o.side = S;
        o.next = kNil;
        if constexpr (requires { o.timestamp; }) {  // naive layout carries cold fields
            o.price = band_.to_price(lvl);
            o.orig_qty = qty;
            o.timestamp = ++seq_;
            o.tif = TimeInForce::GTC;
        }
        Level& L = levels_[si][lvl];
        o.prev = L.tail;
        if (L.tail != kNil) {
            pool_[L.tail].next = i;
        } else {
            L.head = i;
            bits_[si].set(lvl);
            auto& best = best_[si];
            if (best == kNil || (S == Side::Buy ? lvl > best : lvl < best)) best = lvl;
        }
        L.tail = i;
        L.total += qty;
        ++L.count;
        ids_.insert(id, i);
    }

    // Unlinks order i from its level, erases its id and frees the slot.
    void remove(std::uint32_t i) {
        Order& o = pool_[i];
        const int si = idx(o.side);
        Level& L = levels_[si][o.level];
        if (o.prev != kNil) pool_[o.prev].next = o.next;
        else L.head = o.next;
        if (o.next != kNil) pool_[o.next].prev = o.prev;
        else L.tail = o.prev;
        L.total -= o.qty;
        --L.count;
        if (L.head == kNil) {
            bits_[si].clear(o.level);
            if (best_[si] == o.level) best_[si] = next_level(o.side, o.level);
        }
        ids_.erase(o.id);
        pool_.free(i);
    }

    PriceBand band_;
    StorageT<OrderT> pool_;
    IdMapT ids_;
    std::vector<Level> levels_[2];
    LevelBitset bits_[2];
    std::uint32_t best_[2];
    std::uint64_t seq_ = 0;
};

template <class OrderT, template <class> class StorageT, class IdMapT>
bool BasicBook<OrderT, StorageT, IdMapT>::validate(std::string* why) const {
    auto fail = [&](const std::string& msg) {
        if (why) *why = msg;
        return false;
    };
    std::size_t total_orders = 0;
    for (int s = 0; s < 2; ++s) {
        std::uint32_t expect_best = kNil;
        for (std::uint32_t b = 0; b < band_.levels; ++b) {
            const Level& L = levels_[s][b];
            const bool occupied = L.head != kNil;
            if (occupied != bits_[s].test(b)) return fail("bitset mismatch at level " + std::to_string(b));
            if (!occupied) {
                if (L.tail != kNil || L.count || L.total) return fail("empty level has residue");
                continue;
            }
            if (s == 0 || expect_best == kNil) {
                if (s == 0) expect_best = b;  // bids: highest occupied (keeps overwriting)
                else expect_best = b;         // asks: lowest occupied (first hit)
            }
            std::uint64_t tot = 0;
            std::uint32_t cnt = 0;
            std::uint32_t prev = kNil;
            for (auto i = L.head; i != kNil; i = pool_[i].next) {
                const Order& o = pool_[i];
                if (o.prev != prev) return fail("broken prev link");
                if (o.level != b || idx(o.side) != s) return fail("order level/side mismatch");
                if (o.qty == 0) return fail("zero-qty resting order");
                if (ids_.find(o.id) != i) return fail("id map mismatch");
                tot += o.qty;
                ++cnt;
                prev = i;
            }
            if (prev != L.tail) return fail("tail mismatch");
            if (tot != L.total || cnt != L.count) return fail("level aggregate mismatch");
            total_orders += cnt;
        }
        if (best_[s] != expect_best) return fail("best cache wrong on side " + std::to_string(s));
    }
    if (total_orders != ids_.size()) return fail("id map size mismatch");
    if (total_orders != pool_.live()) return fail("pool live count mismatch");
    return true;
}

// ---- optimisation stages (see docs/benchmarks.md) ---------------------------
using BookFlatLevels = BasicBook<OrderLoose, HeapStorage, StdIdMap>;  // flat levels + intrusive lists
using BookPool = BasicBook<OrderLoose, PoolStorage, StdIdMap>;        // + pool allocator
using BookFlatHash = BasicBook<OrderLoose, PoolStorage, FlatIdMap>;   // + open-addressing hash map
using Book = BasicBook<OrderPacked, PoolStorage, FlatIdMap>;          // + cache-tuned 32B order

extern template class BasicBook<OrderLoose, HeapStorage, StdIdMap>;
extern template class BasicBook<OrderLoose, PoolStorage, StdIdMap>;
extern template class BasicBook<OrderLoose, PoolStorage, FlatIdMap>;
extern template class BasicBook<OrderPacked, PoolStorage, FlatIdMap>;

}  // namespace ob
