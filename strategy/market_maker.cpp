#include "market_maker.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <sstream>

namespace ob::strategy {
namespace {
constexpr double kPxScale = 1e4;  // price units per dollar (ITCH/LOBSTER convention)
}

void MarketMaker::manage(Quote& q, Side s, bool want, Price px, Qty size, std::vector<Message>& out) {
    if (q.pending) return;  // one action in flight per quote
    if (!want) {
        if (q.live) {
            out.push_back(Message::cancel(q.id));
            q.pending = true;
        }
        return;
    }
    if (!q.live) {
        q.id = next_id_++;
        out.push_back(Message::add(q.id, s, px, size));
        q.pending = true;
    } else if (q.price != px) {
        out.push_back(Message::modify(q.id, px, size));  // reprice: loses queue position
        q.pending = true;
    }
}

void MarketMaker::on_book(const Book& book, std::vector<Message>& out) {
    const auto bb = book.best_bid(), ba = book.best_ask();
    if (!bb || !ba) return;
    const Price tick = band_.tick;
    const double mid = 0.5 * static_cast<double>(*bb + *ba);
    const double lots = static_cast<double>(inventory_) / static_cast<double>(cfg_.quote_size);
    const double reservation = mid - cfg_.skew_ticks_per_lot * lots * static_cast<double>(tick);
    const double half = static_cast<double>(cfg_.min_half_spread_ticks * tick);

    auto align_down = [&](double p) {
        const auto k = static_cast<Price>(std::floor((p - static_cast<double>(band_.base)) / static_cast<double>(tick)));
        return band_.base + k * tick;
    };
    auto align_up = [&](double p) {
        const auto k = static_cast<Price>(std::ceil((p - static_cast<double>(band_.base)) / static_cast<double>(tick)));
        return band_.base + k * tick;
    };

    Price touch_bid = *bb, touch_ask = *ba;
    const bool wide = (*ba - *bb) >= 3 * tick;
    if (cfg_.improve_wide_spread && wide) {
        if (!(bid_.live && bid_.price == *bb)) touch_bid = *bb + tick;
        if (!(ask_.live && ask_.price == *ba)) touch_ask = *ba - tick;
    }
    // Never cross: bid strictly below the ask and vice versa (post-only).
    Price bid_px = std::min(align_down(reservation - half), touch_bid);
    Price ask_px = std::max(align_up(reservation + half), touch_ask);
    bid_px = std::min(bid_px, *ba - tick);
    ask_px = std::max(ask_px, *bb + tick);

    const bool want_bid = inventory_ + static_cast<std::int64_t>(cfg_.quote_size) <= cfg_.max_inventory &&
                          band_.valid(bid_px);
    const bool want_ask = -inventory_ + static_cast<std::int64_t>(cfg_.quote_size) <= cfg_.max_inventory &&
                          band_.valid(ask_px);
    manage(bid_, Side::Buy, want_bid, bid_px, cfg_.quote_size, out);
    manage(ask_, Side::Sell, want_ask, ask_px, cfg_.quote_size, out);
}

void MarketMaker::pull(std::vector<Message>& out) {
    manage(bid_, Side::Buy, false, 0, 0, out);
    manage(ask_, Side::Sell, false, 0, 0, out);
}

bool MarketMaker::on_event(const Event& e, Fill& fill, bool& is_maker) {
    auto quote_for = [&](OrderId id) -> Quote* {
        if (bid_.id == id && id) return &bid_;
        if (ask_.id == id && id) return &ask_;
        return nullptr;
    };
    if (e.type == EventType::Trade) {
        Quote* q = nullptr;
        Side our_side;
        if (owns(e.maker_id)) {
            q = quote_for(e.maker_id);
            our_side = opposite(e.side);
            is_maker = true;
        } else if (owns(e.id)) {
            q = quote_for(e.id);
            our_side = e.side;
            is_maker = false;
        } else {
            return false;
        }
        if (q) {
            q->qty -= std::min(q->qty, e.qty);
            if (q->qty == 0) q->live = false;
        }
        inventory_ += our_side == Side::Buy ? static_cast<std::int64_t>(e.qty) : -static_cast<std::int64_t>(e.qty);
        fill = Fill{0, our_side, e.price, e.qty, 0.0};
        return true;
    }
    Quote* q = quote_for(e.id);
    if (!q || !owns(e.id)) return false;
    switch (e.type) {
        case EventType::Ack:
            q->live = true;
            q->pending = false;
            q->price = e.price;
            q->qty = e.qty;
            break;
        case EventType::Cancel:
            q->live = false;
            q->pending = false;
            break;
        case EventType::Reject:
            q->pending = false;
            if (e.reason == Reason::UnknownOrder) q->live = false;
            break;
        default: break;
    }
    return false;
}

MMResult run_backtest(const MessageStream& stream, const MMConfig& cfg) {
    MMResult r;
    Book book(BookConfig{stream.band, 1u << 20});
    MarketMaker mm(cfg, stream.band);
    EventBuffer out(1 << 14);
    std::deque<std::pair<std::size_t, Message>> inflight;
    std::vector<Message> actions;
    std::deque<Fill> markout_queue;
    double markout_sum = 0;
    std::uint64_t markout_shares = 0;
    double peak = 0;
    double mid = 0;

    auto equity = [&] {
        return r.cash + static_cast<double>(mm.inventory()) * mid / kPxScale + r.rebates;
    };
    auto handle_events = [&](std::size_t i, bool ours) {
        for (const Event& e : out) {
            if (!ours && e.type == EventType::Reject) ++r.rejected_market_msgs;
            Fill f{};
            bool maker = false;
            if (!mm.on_event(e, f, maker)) continue;
            const double px = static_cast<double>(f.price) / kPxScale;
            const double q = static_cast<double>(f.qty);
            if (f.side == Side::Buy) {
                r.cash -= px * q;
                r.bought += f.qty;
            } else {
                r.cash += px * q;
                r.sold += f.qty;
            }
            if (maker) r.rebates += cfg.rebate_per_share * q;
            else {
                r.rebates -= cfg.taker_fee_per_share * q;
                ++r.taker_fills;
            }
            ++r.fills;
            f.msg_index = i;
            f.mid_at_fill = mid;
            r.fill_log.push_back(f);
            markout_queue.push_back(f);
            r.max_abs_inventory = std::max(r.max_abs_inventory, std::abs(mm.inventory()));
        }
    };
    auto send_ours = [&](std::size_t i, const Message& m) {
        out.clear();
        book.process(m, out);
        ++r.our_msgs;
        handle_events(i, true);
    };

    for (std::size_t i = 0; i < stream.msgs.size(); ++i) {
        while (!inflight.empty() && inflight.front().first <= i) {
            send_ours(i, inflight.front().second);
            inflight.pop_front();
        }
        out.clear();
        book.process(stream.msgs[i], out);
        ++r.market_msgs;
        handle_events(i, false);

        if (auto bb = book.best_bid(), ba = book.best_ask(); bb && ba) mid = 0.5 * static_cast<double>(*bb + *ba);

        actions.clear();
        const bool has_ts = stream.ts_ns.size() == stream.msgs.size();
        const bool in_window =
            !has_ts || (stream.ts_ns[i] >= cfg.quote_from_ns && stream.ts_ns[i] < cfg.quote_until_ns);
        if (in_window) mm.on_book(book, actions);
        else mm.pull(actions);
        for (const Message& a : actions) {
            if (cfg.latency_msgs == 0) send_ours(i, a);
            else inflight.emplace_back(i + cfg.latency_msgs, a);
        }

        // Markouts: mid move in our favour `markout_horizon` messages after each fill.
        while (!markout_queue.empty() && markout_queue.front().msg_index + cfg.markout_horizon <= i) {
            const Fill& f = markout_queue.front();
            const double dir = f.side == Side::Buy ? 1.0 : -1.0;
            markout_sum += dir * (mid - static_cast<double>(f.price)) / kPxScale * static_cast<double>(f.qty);
            markout_shares += f.qty;
            markout_queue.pop_front();
        }

        const double eq = equity();
        peak = std::max(peak, eq);
        r.max_drawdown = std::max(r.max_drawdown, peak - eq);
        if (i % cfg.equity_sample_every == 0)
            r.equity.emplace_back(stream.ts_ns.size() == stream.msgs.size() ? stream.ts_ns[i] : i, eq);
    }
    r.inventory = mm.inventory();
    r.final_mid = mid / kPxScale;
    r.pnl = equity();
    r.pnl_ex_rebates = r.pnl - r.rebates;
    r.avg_markout_per_share = markout_shares ? markout_sum / static_cast<double>(markout_shares) : 0.0;
    return r;
}

std::string MMResult::summary() const {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(2);
    os << "market messages      " << market_msgs << " (rejected " << rejected_market_msgs << ")\n"
       << "strategy messages    " << our_msgs << "\n"
       << "fills                " << fills << " (" << taker_fills << " as taker)\n"
       << "volume               bought " << bought << ", sold " << sold << "\n"
       << "final inventory      " << inventory << " (max |inv| " << max_abs_inventory << ")\n"
       << "final mid            $" << final_mid << "\n"
       << "P&L (mark-to-mid)    $" << pnl << "\n"
       << "  of which spread/MTM $" << pnl_ex_rebates << ", rebates/fees $" << rebates << "\n"
       << "max drawdown         $" << max_drawdown << "\n";
    os.precision(4);
    os << "avg markout          $" << avg_markout_per_share << " / share (negative = adverse selection)\n";
    return os.str();
}

}  // namespace ob::strategy
