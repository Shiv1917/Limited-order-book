// NASDAQ TotalView-ITCH 5.0 support.
//
// Files (e.g. https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/) are a sequence of
// frames: 2-byte big-endian length + message. We decode the order-book
// messages (A/F add, E/C execute, X partial cancel, D delete, U replace) for
// one symbol and drive the matching engine with them.
//
// Validation against the source: ITCH has no book snapshots, but every 'E'
// (execution of a displayed order) tells us which resting order the exchange
// matched. Before applying it we check that, in OUR book, that order is at
// the front of the queue at the best price, then we replay the execution as
// an aggressive IOC order and check that our engine produces exactly one
// trade against that same maker for the same size. This checks price-time
// priority and level bookkeeping against the real exchange's matching.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "msgfile.hpp"
#include "ob/book.hpp"
#include "ref_book.hpp"

namespace ob::itch {

struct Msg {
    char type = 0;
    std::uint16_t locate = 0;
    std::uint64_t ts_ns = 0;
    std::uint64_t ref = 0;
    std::uint64_t new_ref = 0;  // 'U'
    char side = 0;              // 'B' / 'S'
    std::uint32_t shares = 0;
    std::uint32_t price = 0;  // 1e-4 dollars
    char stock[9] = {};       // 'R', 'A', 'F'
    char event = 0;           // 'S' system event code
};

// Decodes one frame payload. Returns false for message types we ignore.
bool decode(const std::uint8_t* p, std::size_t len, Msg& m);

// Buffered frame reader over a FILE* (file or stdin pipe from gunzip).
// Stops cleanly on EOF or a truncated trailing frame.
class FrameReader {
public:
    explicit FrameReader(std::FILE* f, std::size_t buf_size = 1 << 22);
    bool next(const std::uint8_t*& payload, std::size_t& len);
    std::uint64_t bytes() const { return consumed_; }

private:
    bool fill(std::size_t need);
    std::FILE* f_;
    std::vector<std::uint8_t> buf_;
    std::size_t pos_ = 0, end_ = 0;
    std::uint64_t consumed_ = 0;
};

struct ReplayOptions {
    std::string symbol = "AAPL";
    std::uint64_t end_ns = UINT64_MAX;     // stop after this exchange timestamp
    std::size_t max_messages = SIZE_MAX;   // stop after this many engine messages
    std::uint32_t half_range_ticks = 1u << 16;  // price band = first price +/- this
    Price tick = 100;                      // $0.01 in 1e-4 units
    bool differential = false;             // also run the reference book and compare
};

struct ReplayStats {
    std::uint64_t frames = 0, symbol_msgs = 0;
    std::uint64_t adds = 0, execs = 0, execs_with_price = 0, partial_cancels = 0, deletes = 0, replaces = 0;
    std::uint64_t hidden_trades = 0, cross_trades = 0;
    std::uint64_t out_of_band = 0, crossed_adds = 0, unknown_refs = 0, rejects = 0;
    std::uint64_t exec_checked_ok = 0, exec_not_front = 0, exec_trade_mismatch = 0;
    // breakdown of exec_not_front
    std::uint64_t nf_at_open = 0;        // within 1s of the opening cross
    std::uint64_t nf_same_level = 0;     // at best price but behind other orders
    std::uint64_t nf_worse_level = 0;    // not at the best price at all
    std::uint64_t nf_oversize = 0;       // execution larger than our resting qty
    std::uint64_t diff_mismatches = 0;
    std::uint64_t engine_msgs = 0, trades = 0;
    std::uint64_t first_ts = 0, last_ts = 0, market_open_ts = 0;
    std::uint16_t locate = 0;
};

class Replayer {
public:
    explicit Replayer(ReplayOptions opts);
    // Feed one decoded frame. Returns false once the stop condition is hit.
    bool on_msg(const Msg& m);
    bool done() const { return done_; }

    const ReplayStats& stats() const { return stats_; }
    const Book* book() const { return book_.get(); }
    MessageStream& stream() { return stream_; }

private:
    void ensure_book(std::uint32_t first_price);
    void send(const Message& m, std::uint64_t ts);
    void reduce(OrderId id, std::uint32_t shares, std::uint64_t ts);

    ReplayOptions opts_;
    ReplayStats stats_;
    std::unique_ptr<Book> book_;
    std::unique_ptr<RefBook> ref_;
    EventBuffer out_{1 << 14}, ref_out_{1 << 14};
    std::unordered_map<OrderId, Side> out_of_band_;  // orders we could not place (price outside band)
    MessageStream stream_;
    OrderId next_taker_ = OrderId{1} << 63;
    bool done_ = false;
    bool have_locate_ = false;
};

}  // namespace ob::itch
