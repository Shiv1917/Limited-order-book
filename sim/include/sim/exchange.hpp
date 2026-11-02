// Exchange simulator: strategy order handling and queue-aware fills.
//
// Strategy orders are kept here, NOT in the replayed ob::Book. Each resting
// strategy order records `after_seq`, the book's arrival sequence number at
// the moment it arrived: every historical order with seq <= after_seq is
// ahead of it in the FIFO, every one with seq > after_seq is behind it. That
// gives the same queue position as inserting the order into the level FIFO,
// while guaranteeing by construction that strategy orders can never change
// the replayed book (no market impact). See docs/sim_design.md.
//
// Fill rules for resting strategy orders, applied in before_md() so the book
// is still in its pre-event state:
//   type 4 (visible execution of order X at price p, qty q, side s):
//       remaining = q * exec_volume_pct / 100
//       1. trade-through (optional): strategy orders on side s priced better
//          than p fill first, best price first, FIFO within a price.
//       2. strategy orders at p with after_seq < seq(X) fill in FIFO order.
//       Orders behind X get nothing.
//   type 1/2/3: never fill anything (cancels ahead improve the queue for free).
//   type 5 (hidden): never fills; counted.
//   type 6/7 (cross/halt): cancel all strategy orders; reject new orders
//       until the next type 1-5 message.
//
// Marketable strategy orders take displayed liquidity without removing it
// from the book; own_taken[(side, price)] remembers how much of a level we
// have already consumed so two back-to-back takes cannot fill against the
// same shares. Historical executions/cancels at the level decrement it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ob/book.hpp"
#include "sim/events.hpp"

namespace sim {

struct RiskLimits {
    std::int64_t max_position = 500;
    Qty max_order_size = 100;
    std::uint32_t max_open_orders = 4;
};

struct FillModelConfig {
    bool trade_through = true;          // fill orders priced through an execution
    std::uint32_t exec_volume_pct = 100;  // share of each execution's volume we may ride on
    bool own_taken = true;              // track liquidity we already took
};

class Exchange {
public:
    // Maps a LOBSTER type 2/3/4 row (id, side, price) to the book order it
    // acts on (handles liquidity seeded from the first snapshot).
    using Resolver = std::function<std::optional<OrderId>(OrderId, Side, Price)>;

    Exchange(const ob::Book& book, Resolver resolve, const RiskLimits& risk, const FillModelConfig& fm)
        : book_(book), resolve_(std::move(resolve)), risk_(risk), fm_(fm) {}

    // Events produced by the last call; the simulator drains and clears it.
    std::vector<OrderEvent>& out() noexcept { return out_; }

    void before_md(const MdEvent& e) {
        if (book_.last_seq() >= (std::uint64_t{1} << 32))
            throw std::runtime_error("book arrival sequence exceeded 2^32; queue position no longer exact");
        switch (e.type) {
            case MdType::Add:
            case MdType::PartialCancel:
            case MdType::Delete: halted_ = false; break;
            case MdType::Execute:
                halted_ = false;
                on_execution(e);
                break;
            case MdType::Hidden:
                halted_ = false;
                hidden_volume_ += e.qty;
                break;
            case MdType::Cross:
            case MdType::Halt:
                ++halts_;
                halted_ = true;
                cancel_all(e.ts, CancelReason::Halt);
                break;
        }
    }

    void after_md(const MdEvent& e) {
        if (!fm_.own_taken) return;
        if (e.type != MdType::PartialCancel && e.type != MdType::Delete && e.type != MdType::Execute) return;
        auto it = own_taken_.find({e.side, e.price});
        if (it == own_taken_.end()) return;
        // The shares we took were the front of the queue; history removing
        // shares at the level removes those first.
        it->second = std::max<std::int64_t>(0, it->second - static_cast<std::int64_t>(e.qty));
        it->second = std::min<std::int64_t>(it->second, static_cast<std::int64_t>(book_.level_qty(e.side, e.price)));
        if (it->second == 0) own_taken_.erase(it);
    }

    void on_request(const OrderRequest& r, Time now) {
        if (r.cancel) on_cancel(r, now);
        else on_new(r, now);
    }

    // ---- state -------------------------------------------------------------
    std::int64_t position() const noexcept { return position_; }
    std::size_t open_orders() const noexcept { return live_.size(); }
    std::uint64_t hidden_volume() const noexcept { return hidden_volume_; }
    std::uint64_t halts() const noexcept { return halts_; }
    std::uint64_t unknown_exec_refs() const noexcept { return unknown_exec_refs_; }
    std::int64_t own_taken(Side s, Price px) const {
        auto it = own_taken_.find({s, px});
        return it == own_taken_.end() ? 0 : it->second;
    }

    struct LiveOrder {
        OrderId id = 0;
        Side side = Side::Buy;
        Price price = 0;
        Qty qty = 0;
        Qty leaves = 0;
        std::uint64_t after_seq = 0;
        std::uint64_t queue_ahead = 0;
        Time arrived = 0;
    };
    const std::map<OrderId, LiveOrder>& live() const noexcept { return live_; }

private:
    using Fifo = std::deque<OrderId>;
    std::map<Price, Fifo>& levels(Side s) { return s == Side::Buy ? bids_ : asks_; }

    void emit(const OrderEvent& ev) { out_.push_back(ev); }

    OrderEvent base_event(const OrderRequest& r, Time now, OrderEventType t) const {
        OrderEvent ev;
        ev.ts = now;
        ev.id = r.id;
        ev.price = r.price;
        ev.qty = r.qty;
        ev.side = r.side;
        ev.type = t;
        ev.arrived = now;
        return ev;
    }

    void reject(const OrderRequest& r, Time now, RejectReason why) {
        OrderEvent ev = base_event(r, now, OrderEventType::Reject);
        ev.reject = why;
        ev.for_cancel = r.cancel;
        emit(ev);
    }

    void apply_fill(Side s, Qty q) {
        position_ += sign(s) * static_cast<std::int64_t>(q);
    }

    // ---- new orders ----------------------------------------------------------
    void on_new(const OrderRequest& r, Time now) {
        if (r.qty == 0) return reject(r, now, RejectReason::InvalidQty);
        if (r.type != OrderType::Market && !book_.band().valid(r.price)) return reject(r, now, RejectReason::InvalidPrice);
        if (halted_) return reject(r, now, RejectReason::Halted);
        if (r.qty > risk_.max_order_size) return reject(r, now, RejectReason::MaxOrderSize);
        if (r.type == OrderType::Limit && live_.size() >= risk_.max_open_orders)
            return reject(r, now, RejectReason::MaxOpenOrders);
        // Worst case: every open order on this side fills, plus this one.
        const std::int64_t q = r.qty;
        if (r.side == Side::Buy ? position_ + open_buy_ + q > risk_.max_position
                                : -position_ + open_sell_ + q > risk_.max_position)
            return reject(r, now, RejectReason::MaxPosition);
        if (would_self_trade(r)) return reject(r, now, RejectReason::SelfTrade);

        OrderEvent ack = base_event(r, now, OrderEventType::Ack);
        ack.leaves = r.qty;
        emit(ack);

        Qty leaves = take(r, now);
        if (leaves == 0) return;
        if (r.type != OrderType::Limit) {
            OrderEvent c = base_event(r, now, OrderEventType::Cancelled);
            c.qty = leaves;
            c.leaves = 0;
            c.cancel = CancelReason::Unfilled;
            done_.insert(r.id);
            return emit(c);
        }
        rest(r, leaves, now);
    }

    bool would_self_trade(const OrderRequest& r) {
        auto& opp = levels(ob::opposite(r.side));
        if (opp.empty()) return false;
        if (r.type == OrderType::Market) return true;
        // Best own price on the other side.
        const Price best = r.side == Side::Buy ? opp.begin()->first : opp.rbegin()->first;
        return r.side == Side::Buy ? r.price >= best : r.price <= best;
    }

    // Walks the opposite side of the historical book; returns leaves.
    Qty take(const OrderRequest& r, Time now) {
        const Side opp = ob::opposite(r.side);
        Qty leaves = r.qty;
        std::vector<std::pair<Price, Qty>> fills;
        book_.for_each_level(opp, [&](const ob::LevelInfo& L) {
            if (r.type != OrderType::Market && (r.side == Side::Buy ? L.price > r.price : L.price < r.price)) return false;
            const std::int64_t used = fm_.own_taken ? own_taken(opp, L.price) : 0;
            const std::int64_t avail = static_cast<std::int64_t>(L.qty) - used;
            if (avail > 0) {
                const Qty f = static_cast<Qty>(std::min<std::int64_t>(avail, leaves));
                fills.emplace_back(L.price, f);
                leaves -= f;
            }
            return leaves > 0;
        });
        Qty open = r.qty;
        for (const auto& [px, f] : fills) {
            if (fm_.own_taken) own_taken_[{opp, px}] += f;
            open -= f;
            apply_fill(r.side, f);
            OrderEvent ev = base_event(r, now, OrderEventType::Fill);
            ev.price = px;
            ev.qty = f;
            ev.leaves = open;
            ev.maker = false;
            emit(ev);
        }
        if (open == 0) done_.insert(r.id);
        return leaves;
    }

    void rest(const OrderRequest& r, Qty leaves, Time now) {
        LiveOrder o;
        o.id = r.id;
        o.side = r.side;
        o.price = r.price;
        o.qty = r.qty;
        o.leaves = leaves;
        o.after_seq = book_.last_seq();
        o.arrived = now;
        o.queue_ahead = book_.level_qty(r.side, r.price);
        auto& fifo = levels(r.side)[r.price];
        for (OrderId id : fifo) o.queue_ahead += live_.at(id).leaves;
        fifo.push_back(r.id);
        live_.emplace(r.id, o);
        (r.side == Side::Buy ? open_buy_ : open_sell_) += leaves;
    }

    // ---- cancels ---------------------------------------------------------------
    void on_cancel(const OrderRequest& r, Time now) {
        auto it = live_.find(r.id);
        if (it == live_.end())
            return reject(r, now, done_.count(r.id) ? RejectReason::TooLateToCancel : RejectReason::UnknownOrder);
        const LiveOrder o = it->second;
        unlink(it);
        OrderEvent ev;
        ev.ts = now;
        ev.id = o.id;
        ev.price = o.price;
        ev.qty = o.leaves;
        ev.leaves = 0;
        ev.side = o.side;
        ev.type = OrderEventType::Cancelled;
        ev.cancel = CancelReason::User;
        ev.arrived = o.arrived;
        ev.queue_ahead = o.queue_ahead;
        emit(ev);
    }

    void cancel_all(Time now, CancelReason why) {
        while (!live_.empty()) {
            auto it = live_.begin();
            const LiveOrder o = it->second;
            unlink(it);
            OrderEvent ev;
            ev.ts = now;
            ev.id = o.id;
            ev.price = o.price;
            ev.qty = o.leaves;
            ev.side = o.side;
            ev.type = OrderEventType::Cancelled;
            ev.cancel = why;
            ev.arrived = o.arrived;
            ev.queue_ahead = o.queue_ahead;
            emit(ev);
        }
    }

    void unlink(std::map<OrderId, LiveOrder>::iterator it) {
        const LiveOrder& o = it->second;
        auto& lv = levels(o.side);
        auto L = lv.find(o.price);
        auto& fifo = L->second;
        fifo.erase(std::find(fifo.begin(), fifo.end(), o.id));
        if (fifo.empty()) lv.erase(L);
        (o.side == Side::Buy ? open_buy_ : open_sell_) -= o.leaves;
        done_.insert(o.id);
        live_.erase(it);
    }

    // ---- passive fills from historical executions -----------------------------
    void on_execution(const MdEvent& e) {
        std::uint64_t x_seq = 0;  // unknown executed order: assume it is at the very front
        if (const auto target = resolve_(e.id, e.side, e.price)) {
            if (const auto q = book_.queue_entry(*target)) x_seq = q->seq;
        } else {
            ++unknown_exec_refs_;
        }
        std::int64_t remaining = static_cast<std::int64_t>(e.qty) * fm_.exec_volume_pct / 100;
        auto& lv = levels(e.side);
        if (lv.empty() || remaining <= 0) return;

        // 1. Trade-through: our orders priced better than the execution.
        if (fm_.trade_through) {
            std::vector<Price> better;
            for (const auto& [px, fifo] : lv)
                if (e.side == Side::Buy ? px > e.price : px < e.price) better.push_back(px);
            if (e.side == Side::Buy) std::reverse(better.begin(), better.end());  // best (highest) bid first
            for (Price px : better) {
                fill_level(e.side, px, remaining, e.ts, std::numeric_limits<std::uint64_t>::max());
                if (remaining == 0) return;
            }
        }
        // 2. Our orders at the execution price that arrived before X.
        fill_level(e.side, e.price, remaining, e.ts, x_seq);
    }

    // Fills FIFO orders at one of our levels whose after_seq < seq_limit.
    void fill_level(Side s, Price px, std::int64_t& remaining, Time ts, std::uint64_t seq_limit) {
        auto& lv = levels(s);
        auto L = lv.find(px);
        if (L == lv.end()) return;
        std::vector<OrderId> ids(L->second.begin(), L->second.end());  // copy: unlink edits the FIFO
        for (OrderId id : ids) {
            if (remaining == 0) return;
            auto it = live_.find(id);
            LiveOrder& o = it->second;
            if (o.after_seq >= seq_limit) return;  // FIFO: everyone after is behind X too
            const Qty f = static_cast<Qty>(std::min<std::int64_t>(remaining, o.leaves));
            remaining -= f;
            o.leaves -= f;
            (s == Side::Buy ? open_buy_ : open_sell_) -= f;
            apply_fill(s, f);
            OrderEvent ev;
            ev.ts = ts;
            ev.id = o.id;
            ev.price = o.price;
            ev.qty = f;
            ev.leaves = o.leaves;
            ev.side = s;
            ev.type = OrderEventType::Fill;
            ev.maker = true;
            ev.arrived = o.arrived;
            ev.queue_ahead = o.queue_ahead;
            emit(ev);
            if (o.leaves == 0) unlink(it);
        }
    }

    const ob::Book& book_;
    Resolver resolve_;
    RiskLimits risk_;
    FillModelConfig fm_;
    std::vector<OrderEvent> out_;

    std::map<OrderId, LiveOrder> live_;
    std::map<Price, Fifo> bids_, asks_;
    std::set<OrderId> done_;
    std::map<std::pair<Side, Price>, std::int64_t> own_taken_;

    std::int64_t position_ = 0, open_buy_ = 0, open_sell_ = 0;
    bool halted_ = false;
    std::uint64_t hidden_volume_ = 0, halts_ = 0, unknown_exec_refs_ = 0;
};

}  // namespace sim
