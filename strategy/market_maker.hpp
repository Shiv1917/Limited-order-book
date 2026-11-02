// A simple inventory-skewed market maker, backtested by injecting its quotes
// into the matching engine alongside replayed market data.
//
// Simulation model (see docs/design.md "Market-making backtest"):
//   * Market messages are replayed in order; aggressive flow in the stream is
//     real aggressor orders reconstructed from ITCH executions, so our quotes
//     fill only if the engine's price-time priority puts them ahead.
//   * The strategy sees the book after every market message and may
//     add/modify/cancel its two quotes. Its actions reach the engine after a
//     configurable delay of `latency_msgs` market messages.
//   * Our orders never cross (post-only behaviour enforced by the strategy).
//   * P&L = cash + inventory * mid, plus maker rebates/fees per share.
// Known simplifications: queue position of our quote is exact within our
// engine but the real market never saw our orders (no market impact), and
// historical cancels/modifies are replayed with their original absolute sizes.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "msgfile.hpp"
#include "ob/book.hpp"

namespace ob::strategy {

struct MMConfig {
    Qty quote_size = 100;
    std::int64_t max_inventory = 500;   // stop quoting the side that would exceed this
    double skew_ticks_per_lot = 1.0;    // reservation price shift per quote_size of inventory
    int min_half_spread_ticks = 0;      // 0 = may join the touch
    bool improve_wide_spread = true;    // step inside the spread when it is >= 3 ticks
    std::size_t latency_msgs = 0;       // our actions land after this many market messages
    double rebate_per_share = 0.0020;   // maker rebate in dollars (negative = fee)
    double taker_fee_per_share = 0.0030;  // if a delayed quote ends up crossing
    std::size_t markout_horizon = 100;  // messages after a fill to measure adverse selection
    std::size_t equity_sample_every = 1000;
    std::uint64_t quote_from_ns = 0;            // only quote inside [from, until) exchange time
    std::uint64_t quote_until_ns = UINT64_MAX;  // (needs stream timestamps; else ignored)
};

struct Fill {
    std::size_t msg_index;
    Side side;  // our side
    Price price;
    Qty qty;
    double mid_at_fill;
};

struct MMResult {
    std::size_t market_msgs = 0, our_msgs = 0, fills = 0, taker_fills = 0;
    // Historical messages the engine rejected because, in the simulation, the
    // order they refer to already traded against our quotes (counterfactual).
    std::size_t rejected_market_msgs = 0;
    std::uint64_t bought = 0, sold = 0;
    std::int64_t inventory = 0, max_abs_inventory = 0;
    double cash = 0, rebates = 0;  // dollars; rebates net of taker fees
    double final_mid = 0;
    double pnl = 0;           // cash + inventory*mid + rebates (dollars)
    double pnl_ex_rebates = 0;
    double max_drawdown = 0;
    double avg_markout_per_share = 0;  // >0 good, <0 adverse selection (dollars/share)
    std::vector<Fill> fill_log;
    std::vector<std::pair<std::uint64_t, double>> equity;  // (timestamp_ns or msg index, pnl)
    std::string summary() const;
};

class MarketMaker {
public:
    MarketMaker(const MMConfig& cfg, const PriceBand& band) : cfg_(cfg), band_(band) {}

    // Decide desired quotes from the current book; appends actions to `out`.
    void on_book(const Book& book, std::vector<Message>& out);
    // Cancel any live quotes (outside the trading window).
    void pull(std::vector<Message>& out);
    // Feed every engine event. Returns true if it was a fill of one of our
    // orders (filling `fill`; `is_maker` false means our order crossed).
    bool on_event(const Event& e, Fill& fill, bool& is_maker);
    bool owns(OrderId id) const { return (id >> 60) == (kIdBase >> 60); }
    std::int64_t inventory() const { return inventory_; }

    // Our ids live in their own namespace: top nibble 0x6 (ITCH takers use
    // 0x8..., LOBSTER seeded liquidity 0x4..., exchange ids are far below).
    static constexpr OrderId kIdBase = OrderId{6} << 60;

private:
    struct Quote {
        OrderId id = 0;
        Price price = 0;
        Qty qty = 0;
        bool live = false;
        bool pending = false;  // action in flight (latency)
    };
    void manage(Quote& q, Side s, bool want, Price px, Qty size, std::vector<Message>& out);

    MMConfig cfg_;
    PriceBand band_;
    Quote bid_, ask_;
    std::int64_t inventory_ = 0;
    OrderId next_id_ = kIdBase;
};

// Runs the backtest over a message stream.
MMResult run_backtest(const MessageStream& stream, const MMConfig& cfg);

}  // namespace ob::strategy
