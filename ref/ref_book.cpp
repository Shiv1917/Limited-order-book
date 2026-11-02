#include "ref_book.hpp"

#include <algorithm>

namespace ob {

void RefBook::process(const Message& m, EventBuffer& out) {
    switch (m.type) {
        case MsgType::Add: add(m, false, out); break;
        case MsgType::Market: add(m, true, out); break;
        case MsgType::Cancel: cancel(m, out); break;
        case MsgType::Modify: modify(m, out); break;
    }
}

void RefBook::add(const Message& m, bool is_market, EventBuffer& out) {
    if (m.qty == 0) return out.push(make_reject(m.id, m.side, Reason::InvalidQty));
    if (!is_market && !band_.valid(m.price)) return out.push(make_reject(m.id, m.side, Reason::InvalidPrice));
    if (index_.count(m.id)) return out.push(make_reject(m.id, m.side, Reason::DuplicateId));

    const Price px = is_market ? 0 : m.price;
    out.push(make_ack(m.id, m.side, px, m.qty, Reason::New));
    TimeInForce tif = m.tif;
    if (is_market && tif == TimeInForce::GTC) tif = TimeInForce::IOC;  // market orders never rest
    execute(m.id, m.side, is_market ? std::nullopt : std::optional<Price>(m.price), m.qty, tif, px, out);
}

std::uint64_t RefBook::available(Side taker, std::optional<Price> limit, std::uint64_t need) const {
    std::uint64_t avail = 0;
    auto sum = [&](const auto& book, auto crosses) {
        for (const auto& [lvl_px, q] : book) {
            if (limit && !crosses(lvl_px)) break;
            for (const auto& o : q) avail += o.qty;
            if (avail >= need) break;
        }
    };
    if (taker == Side::Buy) sum(asks_, [&](Price p) { return p <= *limit; });
    else sum(bids_, [&](Price p) { return p >= *limit; });
    return avail;
}

void RefBook::execute(OrderId id, Side side, std::optional<Price> limit, Qty qty, TimeInForce tif, Price px,
                      EventBuffer& out) {
    if (tif == TimeInForce::FOK && available(side, limit, qty) < qty)
        return out.push(make_cancel(id, side, px, qty, Reason::FokUnfillable));

    auto sweep = [&](auto& book, auto crosses) {
        while (qty > 0 && !book.empty()) {
            auto lvl = book.begin();
            if (limit && !crosses(lvl->first)) break;
            Queue& q = lvl->second;
            while (qty > 0 && !q.empty()) {
                RestingOrder& maker = q.front();
                const Qty fill = std::min(qty, maker.qty);
                out.push(make_trade(id, maker.id, side, lvl->first, fill));
                maker.qty -= fill;
                qty -= fill;
                if (maker.qty == 0) {
                    index_.erase(maker.id);
                    q.pop_front();
                }
            }
            if (q.empty()) book.erase(lvl);
        }
    };
    if (side == Side::Buy) sweep(asks_, [&](Price p) { return p <= *limit; });
    else sweep(bids_, [&](Price p) { return p >= *limit; });

    if (qty == 0) return;
    if (tif != TimeInForce::GTC) return out.push(make_cancel(id, side, px, qty, Reason::Unfilled));

    // Rest the remainder at the back of its price level.
    const Price rest_px = *limit;
    Queue& q = side == Side::Buy ? bids_[rest_px] : asks_[rest_px];
    q.push_back(RestingOrder{id, qty});
    index_[id] = Locator{side, rest_px, std::prev(q.end())};
}

void RefBook::erase_order(OrderId id) {
    auto it = index_.find(id);
    const Locator loc = it->second;
    index_.erase(it);
    auto drop = [&](auto& book) {
        auto lvl = book.find(loc.price);
        lvl->second.erase(loc.it);
        if (lvl->second.empty()) book.erase(lvl);
    };
    if (loc.side == Side::Buy) drop(bids_);
    else drop(asks_);
}

void RefBook::cancel(const Message& m, EventBuffer& out) {
    auto it = index_.find(m.id);
    if (it == index_.end()) return out.push(make_reject(m.id, m.side, Reason::UnknownOrder));
    const Locator loc = it->second;
    const Qty q = loc.it->qty;
    erase_order(m.id);
    out.push(make_cancel(m.id, loc.side, loc.price, q, Reason::UserCancel));
}

void RefBook::modify(const Message& m, EventBuffer& out) {
    auto it = index_.find(m.id);
    if (it == index_.end()) return out.push(make_reject(m.id, m.side, Reason::UnknownOrder));
    if (m.qty == 0) return out.push(make_reject(m.id, m.side, Reason::InvalidQty));
    if (!band_.valid(m.price)) return out.push(make_reject(m.id, m.side, Reason::InvalidPrice));

    Locator& loc = it->second;
    const Side side = loc.side;
    if (m.price == loc.price && m.qty <= loc.it->qty) {
        loc.it->qty = m.qty;  // reduce in place: keeps time priority
        return out.push(make_ack(m.id, side, m.price, m.qty, Reason::Modified));
    }
    // Price change or size increase: cancel/replace, loses priority, may cross.
    erase_order(m.id);
    out.push(make_ack(m.id, side, m.price, m.qty, Reason::Modified));
    execute(m.id, side, m.price, m.qty, TimeInForce::GTC, m.price, out);
}

std::optional<Price> RefBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}
std::optional<Price> RefBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::vector<LevelInfo> RefBook::depth(Side s, std::size_t n) const {
    std::vector<LevelInfo> v;
    auto collect = [&](const auto& book) {
        for (const auto& [px, q] : book) {
            if (v.size() >= n) break;
            LevelInfo li{px, 0, 0};
            for (const auto& o : q) {
                li.qty += o.qty;
                ++li.count;
            }
            v.push_back(li);
        }
    };
    if (s == Side::Buy) collect(bids_);
    else collect(asks_);
    return v;
}

std::optional<OrderInfo> RefBook::find(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    return OrderInfo{id, it->second.side, it->second.price, it->second.it->qty};
}

}  // namespace ob
