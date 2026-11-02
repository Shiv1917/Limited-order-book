// Synthetic order-flow generator.
//
// Produces a realistic-ish message mix (adds clustered near a random-walking
// mid, heavy cancel churn, some modifies, marketable/IOC/FOK/market orders).
// It runs every message through a RefBook so cancels/modifies target orders
// that are actually live. In `adversarial` mode it also injects invalid
// messages (zero qty, off-band / off-tick prices, duplicate ids, cancels of
// filled orders, modifies of unknown ids) for the differential fuzzer.
#pragma once

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "ob/types.hpp"
#include "ref_book.hpp"

namespace ob {

struct SynthConfig {
    std::uint64_t seed = 1;
    PriceBand band{1'000'000, 100, 4096};  // $100.00 .. $140.95 at 1-cent ticks (price units 1e-4)
    double p_add = 0.50;
    double p_cancel = 0.36;
    double p_modify = 0.08;   // remainder after add+cancel+modify -> market orders
    double p_aggressive = 0.08;  // fraction of adds priced through the touch
    double p_ioc = 0.04;
    double p_fok = 0.02;
    double depth_decay = 0.25;  // geometric distance-from-mid parameter (levels)
    Qty max_qty = 500;
    bool adversarial = false;
};

class SyntheticFlow {
public:
    explicit SyntheticFlow(const SynthConfig& cfg)
        : cfg_(cfg), rng_(cfg.seed), book_(BookConfig{cfg.band, 1u << 16}), mid_(cfg.band.levels / 2) {}

    Message next() {
        Message m = draw();
        out_.clear();
        book_.process(m, out_);
        if ((m.type == MsgType::Add || m.type == MsgType::Modify) && book_.find(m.id)) {
            if (m.type == MsgType::Add) live_.push_back(m.id);
        }
        return m;
    }

    std::vector<Message> generate(std::size_t n) {
        std::vector<Message> v;
        v.reserve(n);
        for (std::size_t i = 0; i < n; ++i) v.push_back(next());
        return v;
    }

    const RefBook& book() const { return book_; }

private:
    double u() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); }
    std::uint32_t uint(std::uint32_t lo, std::uint32_t hi) {
        return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng_);
    }
    Side side() { return u() < 0.5 ? Side::Buy : Side::Sell; }
    Qty qty() {
        // Mostly small, occasionally large (sweeps).
        if (u() < 0.05) return uint(cfg_.max_qty, cfg_.max_qty * 8);
        return uint(1, cfg_.max_qty);
    }

    void walk_mid() {
        if (u() < 0.02) {
            const int step = u() < 0.5 ? -1 : 1;
            const std::int64_t m = static_cast<std::int64_t>(mid_) + step;
            const std::int64_t lo = 64, hi = static_cast<std::int64_t>(cfg_.band.levels) - 64;
            mid_ = static_cast<std::uint32_t>(std::clamp(m, lo, hi));
        }
    }

    std::uint32_t level_for(Side s, bool aggressive) {
        std::geometric_distribution<int> geo(cfg_.depth_decay);
        const int d = geo(rng_) + 1;
        std::int64_t lvl;
        if (aggressive) lvl = s == Side::Buy ? mid_ + d : static_cast<std::int64_t>(mid_) - d;
        else lvl = s == Side::Buy ? static_cast<std::int64_t>(mid_) - d : mid_ + d;
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(lvl, 0, cfg_.band.levels - 1));
    }

    // Picks a live order id (lazily dropping ones that were filled).
    bool pick_live(OrderId& id) {
        while (!live_.empty()) {
            const std::size_t i = uint(0, static_cast<std::uint32_t>(live_.size() - 1));
            if (book_.find(live_[i])) {
                id = live_[i];
                return true;
            }
            live_[i] = live_.back();
            live_.pop_back();
        }
        return false;
    }

    Message draw() {
        walk_mid();
        if (cfg_.adversarial && u() < 0.06) return invalid();

        const double r = u();
        OrderId target;
        if (r < cfg_.p_add || live_.empty()) {
            const Side s = side();
            const bool aggr = u() < cfg_.p_aggressive;
            TimeInForce tif = TimeInForce::GTC;
            const double t = u();
            if (t < cfg_.p_fok) tif = TimeInForce::FOK;
            else if (t < cfg_.p_fok + cfg_.p_ioc) tif = TimeInForce::IOC;
            return Message::add(++next_id_, s, cfg_.band.to_price(level_for(s, aggr)), qty(), tif);
        }
        if (r < cfg_.p_add + cfg_.p_cancel) {
            if (pick_live(target)) return Message::cancel(target);
            return Message::cancel(++next_id_);  // nothing live: harmless reject
        }
        if (r < cfg_.p_add + cfg_.p_cancel + cfg_.p_modify) {
            if (!pick_live(target)) return Message::cancel(++next_id_);
            const OrderInfo o = *book_.find(target);
            const double k = u();
            if (k < 0.5)  // shrink: keeps priority
                return Message::modify(target, o.price, std::max<Qty>(1, o.qty / 2));
            if (k < 0.75)  // grow: loses priority
                return Message::modify(target, o.price, o.qty + uint(1, 100));
            // reprice by a few ticks (may cross the spread)
            std::uint32_t lvl;
            cfg_.band.to_index(o.price, lvl);
            const std::int64_t nl =
                std::clamp<std::int64_t>(static_cast<std::int64_t>(lvl) + static_cast<int>(uint(0, 6)) - 3, 0,
                                         cfg_.band.levels - 1);
            return Message::modify(target, cfg_.band.to_price(static_cast<std::uint32_t>(nl)), o.qty);
        }
        return Message::market(++next_id_, side(), qty(), u() < 0.2 ? TimeInForce::FOK : TimeInForce::IOC);
    }

    Message invalid() {
        OrderId live_id = 0;
        const bool have = pick_live(live_id);
        switch (uint(0, 6)) {
            case 0: return Message::add(++next_id_, side(), cfg_.band.to_price(mid_), 0);  // zero qty
            case 1: return Message::add(++next_id_, side(), cfg_.band.base - cfg_.band.tick, 10);  // below band
            case 2: return Message::add(++next_id_, side(), cfg_.band.to_price(mid_) + 1, 10);  // off tick
            case 3:
                if (have) return Message::add(live_id, side(), cfg_.band.to_price(mid_), 10);  // duplicate id
                return Message::add(++next_id_, side(), cfg_.band.to_price(cfg_.band.levels), 10);  // above band
            case 4: return Message::cancel(next_id_ > 0 ? uint(1, static_cast<std::uint32_t>(next_id_)) : 1);  // maybe filled
            case 5: return Message::modify(next_id_ + 1000, cfg_.band.to_price(mid_), 10);  // unknown id
            default:
                if (have) return Message::modify(live_id, cfg_.band.to_price(mid_), 0);  // modify to zero
                return Message::market(++next_id_, side(), 0);
        }
    }

    SynthConfig cfg_;
    std::mt19937_64 rng_;
    RefBook book_;
    EventBuffer out_{1 << 12};
    std::uint32_t mid_;
    OrderId next_id_ = 0;
    std::vector<OrderId> live_;
};

}  // namespace ob
