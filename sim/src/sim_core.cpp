#include <algorithm>
#include <chrono>
#include <utility>

#include "sim/simulator.hpp"

namespace sim {

const char* to_string(OrderType t) noexcept {
    switch (t) {
        case OrderType::Limit: return "limit";
        case OrderType::Market: return "market";
        case OrderType::IOC: return "ioc";
    }
    return "?";
}

const char* to_string(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None: return "";
        case RejectReason::InvalidQty: return "invalid_qty";
        case RejectReason::InvalidPrice: return "invalid_price";
        case RejectReason::MaxOrderSize: return "max_order_size";
        case RejectReason::MaxPosition: return "max_position";
        case RejectReason::MaxOpenOrders: return "max_open_orders";
        case RejectReason::SelfTrade: return "self_trade";
        case RejectReason::Halted: return "halted";
        case RejectReason::UnknownOrder: return "unknown_order";
        case RejectReason::TooLateToCancel: return "too_late_to_cancel";
    }
    return "?";
}

const char* to_string(CancelReason r) noexcept {
    switch (r) {
        case CancelReason::None: return "";
        case CancelReason::User: return "user";
        case CancelReason::Unfilled: return "unfilled";
        case CancelReason::Halt: return "halt";
    }
    return "?";
}

double wall_now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- market data -------------------------------------------------------------

void build_market_data(std::vector<ob::lobster::Row> rows, std::vector<ob::lobster::Snapshot> snaps,
                       MarketData& out) {
    out.rows = std::move(rows);
    out.snaps = std::move(snaps);
    out.md.clear();
    out.md.reserve(out.rows.size());
    std::int64_t lo = INT64_MAX, hi = 0;
    for (const auto& r : out.rows) {
        MdEvent e;
        e.ts = r.time_ns;
        e.id = r.id;
        e.price = r.price;
        e.qty = r.size;
        e.type = static_cast<MdType>(r.type);
        e.side = r.direction == 1 ? Side::Buy : Side::Sell;
        out.md.push_back(e);
        if (r.type <= 5 && r.price > 0) lo = std::min(lo, r.price), hi = std::max(hi, r.price);
    }
    for (const auto& s : out.snaps)
        for (std::size_t i = 0; i < s.levels(); ++i) {
            if (s.ask_px(i) != ob::lobster::kEmptyAsk) lo = std::min(lo, s.ask_px(i)), hi = std::max(hi, s.ask_px(i));
            if (s.bid_px(i) != ob::lobster::kEmptyBid) lo = std::min(lo, s.bid_px(i)), hi = std::max(hi, s.bid_px(i));
        }
    if (lo > hi) lo = hi = 1'000'000;
    // 1-cent ticks, all observed prices +/- 1024 ticks of room for strategy orders.
    const Price tick = 100, base = std::max<Price>(0, (lo / tick - 1024) * tick);
    out.band = ob::PriceBand{base, tick, static_cast<std::uint32_t>((hi - base) / tick + 1024)};
}

bool load_market_data(const std::string& messages_path, const std::string& book_path, MarketData& out,
                      std::string* err) {
    auto rows = ob::lobster::load_messages(messages_path, err);
    if (rows.empty()) {
        if (err && err->empty()) *err = "no rows in " + messages_path;
        return false;
    }
    std::vector<ob::lobster::Snapshot> snaps;
    if (!book_path.empty()) {
        snaps = ob::lobster::load_snapshots(book_path, err);
        if (snaps.size() != rows.size()) {
            if (err && err->empty()) *err = "message/orderbook row count mismatch";
            return false;
        }
    }
    for (std::size_t i = 1; i < rows.size(); ++i)
        if (rows[i].time_ns < rows[i - 1].time_ns) {
            if (err) *err = messages_path + ": timestamps go backwards at row " + std::to_string(i + 1);
            return false;
        }
    build_market_data(std::move(rows), std::move(snaps), out);
    out.messages_path = messages_path;
    out.book_path = book_path;
    return true;
}

// ---- core --------------------------------------------------------------------

SimCore::SimCore(const SimConfig& cfg, const MarketData& data)
    : cfg_(cfg), data_(data), latency_(cfg.latency, cfg.seed) {
    const auto hint = static_cast<std::uint32_t>(std::clamp<std::size_t>(data.rows.size() / 4, 1024, 1u << 20));
    book_ = std::make_unique<ob::Book>(ob::BookConfig{data.band, hint});
    replayer_ = std::make_unique<ob::lobster::Replayer<ob::Book>>(*book_, cfg.validate_levels);
    auto* rep = replayer_.get();
    exchange_ = std::make_unique<Exchange>(
        *book_, [rep](OrderId id, Side s, Price px) { return rep->resolve(id, s, px); }, cfg.risk, cfg.fill);
}

void SimCore::violation(const std::string& what) {
    if (stats_.invariant_violations++ == 0) stats_.first_violation = fmt_time(clock_) + ": " + what;
}

void SimCore::begin() {
    if (data_.md.empty()) return;
    if (!data_.snaps.empty()) replayer_->seed(data_.snaps[0], data_.rows[0]);
    clock_ = data_.md.front().ts;
    stats_.first_ts = clock_;
    next_snapshot_ = (clock_ + cfg_.snapshot_ns - 1) / cfg_.snapshot_ns * cfg_.snapshot_ns;
    if (book_->best_bid() && book_->best_ask()) {
        last_mid_x2_ = *book_->best_bid() + *book_->best_ask();
        rec_.mids.emplace_back(clock_, last_mid_x2_);
    }
}

void SimCore::advance(Time t) {
    if (t < clock_) violation("clock went backwards");
    while (next_snapshot_ <= t) {
        PnlRow p;
        p.ts = next_snapshot_;
        p.cash = portfolio_.cash();
        p.position = portfolio_.position();
        p.realized = portfolio_.realized();
        p.fees = portfolio_.fees();
        p.mid_x2 = last_mid_x2_;
        p.unrealized_x2 = portfolio_.unrealized_x2(last_mid_x2_);
        p.equity_x2 = portfolio_.equity_x2(last_mid_x2_);
        rec_.pnl.push_back(p);
        next_snapshot_ += cfg_.snapshot_ns;
    }
    clock_ = t;
}

void SimCore::md_step(std::size_t k) {
    const MdEvent& e = data_.md[k];
    advance(e.ts);
    ++stats_.md_events;
    exchange_->before_md(e);  // fills ride on the pre-event queue state
    drain();
    replayer_->apply(data_.rows[k]);
    if (cfg_.validate_book && !data_.snaps.empty() &&
        (k % cfg_.validate_every == 0 || k + 1 == data_.rows.size()))
        replayer_->check(data_.snaps[k], k);
    exchange_->after_md(e);
    drain();
    if (book_->best_bid() && book_->best_ask()) {
        const Price m = *book_->best_bid() + *book_->best_ask();
        if (m != last_mid_x2_) {
            last_mid_x2_ = m;
            rec_.mids.emplace_back(e.ts, m);
        }
    }
}

bool SimCore::sim_step(const SimEvent& ev) {
    advance(ev.ts);
    ++stats_.sim_events;
    switch (ev.prio) {
        case Prio::OrderArrive: {
            const OrderRequest& r = ev.req;
            if (!r.cancel) {
                tracks_[r.id].arrived = ev.ts;
                rec_.orders.push_back({ev.ts, r.id, "arrived", r.side, r.price, r.qty, to_string(r.type)});
            }
            exchange_->on_request(r, ev.ts);
            if (!r.cancel && r.type == OrderType::Limit) {
                const auto& live = exchange_->live();
                if (auto it = live.find(r.id); it != live.end()) rec_.on_rested(r.id, it->second.leaves, ev.ts);
            }
            drain();
            return false;
        }
        case Prio::Deliver:
            if (ev.oev.type == OrderEventType::Fill)
                strategy_position_ += sign(ev.oev.side) * static_cast<std::int64_t>(ev.oev.qty);
            return true;
        case Prio::Timer: return true;
        case Prio::MarketData: break;
    }
    return false;
}

void SimCore::drain() {
    auto& out = exchange_->out();
    for (const OrderEvent& ev : out) {
        switch (ev.type) {
            case OrderEventType::Ack:
                rec_.orders.push_back({ev.ts, ev.id, "acked", ev.side, ev.price, ev.qty, ""});
                break;
            case OrderEventType::Reject:
                ++rec_.rejects;
                if (ev.for_cancel) ++rec_.cancel_rejects;
                rec_.orders.push_back({ev.ts, ev.id, "rejected", ev.side, ev.price, ev.qty, to_string(ev.reject)});
                break;
            case OrderEventType::Cancelled:
                rec_.on_order_done(ev.id);
                rec_.orders.push_back({ev.ts, ev.id, "cancelled", ev.side, ev.price, ev.qty, to_string(ev.cancel)});
                break;
            case OrderEventType::Fill: {
                const std::int64_t q = ev.qty;
                const std::int64_t fee = ev.maker ? -cfg_.maker_rebate * q : cfg_.taker_fee * q;
                portfolio_.on_fill(ev.side, ev.price, ev.qty, fee);
                rec_.fills.push_back({ev.ts, ev.id, ev.side, ev.price, ev.qty, ev.maker, fee, ev.queue_ahead,
                                      last_mid_x2_, ev.arrived});
                rec_.orders.push_back({ev.ts, ev.id, ev.leaves ? "partial" : "filled", ev.side, ev.price, ev.qty,
                                       ev.maker ? "maker" : "taker"});
                if (ev.maker) {
                    rec_.passive_filled_shares += ev.qty;
                    rec_.on_maker_fill(ev.id, ev.ts);
                } else {
                    rec_.taker_filled_shares += ev.qty;
                }
                if (ev.leaves == 0) rec_.on_order_done(ev.id);
                auto& tr = tracks_[ev.id];
                tr.filled += ev.qty;
                if (tr.filled > tr.qty) violation("order filled more than its quantity");
                if (tr.arrived < 0 || ev.ts < tr.arrived) violation("fill before the order arrived");
                if (tr.qty - tr.filled != ev.leaves) violation("leaves inconsistent with fills");
                break;
            }
        }
        SimEvent d;
        d.ts = latency_.delivery(ev.ts);
        d.prio = Prio::Deliver;
        d.oev = ev;
        queue_.push(d);
    }
    out.clear();
    if (portfolio_.position() != exchange_->position()) violation("portfolio and exchange positions differ");
}

void SimCore::finish(double wall_seconds) {
    if (rec_.pnl.empty() || rec_.pnl.back().ts != clock_) {  // closing snapshot at the last event
        next_snapshot_ = clock_;
        advance(clock_);
    }
    stats_.last_ts = clock_;
    stats_.replay = replayer_->stats;
    stats_.hidden_volume = exchange_->hidden_volume();
    stats_.halts = exchange_->halts();
    stats_.unknown_exec_refs = exchange_->unknown_exec_refs();
    stats_.wall_seconds = wall_seconds;
}

// ---- strategy API --------------------------------------------------------------

OrderId SimCore::api_send(Side side, Price price, Qty qty, OrderType type) {
    if (closed_) {
        ++stats_.ignored_after_close;
        return 0;
    }
    OrderRequest r;
    r.id = next_id_++;
    r.side = side;
    r.price = type == OrderType::Market ? 0 : price;
    r.qty = qty;
    r.type = type;
    r.sent = clock_;
    SimEvent ev;
    ev.ts = latency_.order_arrival(clock_, md_triggered_);
    ev.prio = Prio::OrderArrive;
    ev.req = r;
    queue_.push(ev);
    ++rec_.orders_sent;
    rec_.orders.push_back({clock_, r.id, "sent", side, r.price, qty, to_string(type)});
    tracks_[r.id].qty = qty;
    return r.id;
}

void SimCore::api_cancel(OrderId id) {
    if (closed_) {
        ++stats_.ignored_after_close;
        return;
    }
    OrderRequest r;
    r.id = id;
    r.cancel = true;
    r.sent = clock_;
    SimEvent ev;
    ev.ts = latency_.order_arrival(clock_, md_triggered_);
    ev.prio = Prio::OrderArrive;
    ev.req = r;
    queue_.push(ev);
    ++rec_.cancels_sent;
    rec_.orders.push_back({clock_, id, "cancel_sent", Side::Buy, 0, 0, ""});
}

void SimCore::api_timer(Time at, int timer_id) {
    if (closed_) return;
    SimEvent ev;
    ev.ts = std::max(at, clock_);
    ev.prio = Prio::Timer;
    ev.timer_id = timer_id;
    queue_.push(ev);
}

}  // namespace sim
