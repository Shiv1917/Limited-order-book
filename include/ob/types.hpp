// Core message / event types shared by every book implementation.
//
// Prices are integers in "price units" (e.g. 1/10000 of a dollar, as in ITCH
// and LOBSTER). A book is configured with a PriceBand (base, tick, levels);
// limit prices must be on-tick and inside the band, otherwise they are
// rejected. The reference book enforces the same band so that the two
// implementations are observably identical.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <ostream>

namespace ob {

using OrderId = std::uint64_t;
using Price = std::int64_t;
using Qty = std::uint32_t;

inline constexpr std::uint32_t kNil = 0xFFFFFFFFu;  // "null" index for pools/lists

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class TimeInForce : std::uint8_t { GTC = 0, IOC = 1, FOK = 2 };
enum class MsgType : std::uint8_t { Add = 0, Cancel = 1, Modify = 2, Market = 3 };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

// Inbound message. 24 bytes, trivially copyable so streams can be mmapped / memcpy'd.
struct Message {
    OrderId id = 0;
    Price price = 0;  // Add/Modify: limit price. Market/Cancel: ignored (0).
    Qty qty = 0;      // Add/Market: quantity. Modify: new quantity.
    MsgType type = MsgType::Add;
    Side side = Side::Buy;          // Add/Market only
    TimeInForce tif = TimeInForce::GTC;  // Add/Market only (Market is always IOC or FOK)
    std::uint8_t pad_ = 0;

    static constexpr Message add(OrderId id, Side s, Price px, Qty q, TimeInForce tif = TimeInForce::GTC) {
        return Message{id, px, q, MsgType::Add, s, tif, 0};
    }
    static constexpr Message cancel(OrderId id) { return Message{id, 0, 0, MsgType::Cancel, Side::Buy, TimeInForce::GTC, 0}; }
    static constexpr Message modify(OrderId id, Price px, Qty q) {
        return Message{id, px, q, MsgType::Modify, Side::Buy, TimeInForce::GTC, 0};
    }
    static constexpr Message market(OrderId id, Side s, Qty q, TimeInForce tif = TimeInForce::IOC) {
        return Message{id, 0, q, MsgType::Market, s, tif, 0};
    }
    friend bool operator==(const Message&, const Message&) = default;
};
static_assert(sizeof(Message) == 24);

enum class EventType : std::uint8_t { Ack = 0, Trade = 1, Cancel = 2, Reject = 3 };

enum class Reason : std::uint8_t {
    None = 0,
    // Ack
    New,
    Modified,
    // Cancel
    UserCancel,
    Unfilled,       // IOC / market remainder
    FokUnfillable,  // FOK that could not be filled in full
    // Reject
    InvalidQty,
    InvalidPrice,
    DuplicateId,
    UnknownOrder,
};

// Outbound event. 32 bytes.
//   Ack    : id, side, price (0 for market), qty = order quantity, reason New|Modified
//   Trade  : id = taker, maker_id, side = taker side, price = maker (resting) price, qty
//   Cancel : id, side, price (resting price, or 0 for unrested), qty = cancelled qty, reason
//   Reject : id, side (as sent), reason
struct Event {
    OrderId id = 0;
    OrderId maker_id = 0;
    Price price = 0;
    Qty qty = 0;
    EventType type = EventType::Ack;
    Side side = Side::Buy;
    Reason reason = Reason::None;
    std::uint8_t pad_ = 0;

    friend bool operator==(const Event&, const Event&) = default;
};
static_assert(sizeof(Event) == 32);

inline Event make_ack(OrderId id, Side s, Price px, Qty q, Reason r = Reason::New) {
    return Event{id, 0, px, q, EventType::Ack, s, r, 0};
}
inline Event make_trade(OrderId taker, OrderId maker, Side taker_side, Price px, Qty q) {
    return Event{taker, maker, px, q, EventType::Trade, taker_side, Reason::None, 0};
}
inline Event make_cancel(OrderId id, Side s, Price px, Qty q, Reason r) {
    return Event{id, 0, px, q, EventType::Cancel, s, r, 0};
}
inline Event make_reject(OrderId id, Side s, Reason r) { return Event{id, 0, 0, 0, EventType::Reject, s, r, 0}; }

// Valid limit prices are base + k*tick for k in [0, levels).
struct PriceBand {
    Price base = 0;
    Price tick = 1;
    std::uint32_t levels = 1u << 16;

    bool to_index(Price px, std::uint32_t& idx) const noexcept {
        const Price off = px - base;
        if (off < 0) return false;
        const Price q = off / tick;
        if (q * tick != off || q >= static_cast<Price>(levels)) return false;
        idx = static_cast<std::uint32_t>(q);
        return true;
    }
    Price to_price(std::uint32_t idx) const noexcept { return base + static_cast<Price>(idx) * tick; }
    bool valid(Price px) const noexcept {
        std::uint32_t i;
        return to_index(px, i);
    }
};

struct BookConfig {
    PriceBand band{};
    std::uint32_t max_orders = 1u << 20;  // pool capacity hint (pool grows if exceeded)
};

// Aggregated level, used for snapshots / state comparison.
struct LevelInfo {
    Price price = 0;
    std::uint64_t qty = 0;
    std::uint32_t count = 0;
    friend bool operator==(const LevelInfo&, const LevelInfo&) = default;
};

struct OrderInfo {
    OrderId id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
    friend bool operator==(const OrderInfo&, const OrderInfo&) = default;
};

// Pre-allocated output buffer. The engine appends; the caller drains/clears
// between messages. Growth only happens if one message emits more events than
// the initial capacity (e.g. a sweep through thousands of orders).
class EventBuffer {
public:
    explicit EventBuffer(std::size_t cap = 1 << 14) : buf_(new Event[cap]), cap_(cap) {}

    void push(const Event& e) {
        if (n_ == cap_) [[unlikely]]
            grow();
        buf_[n_++] = e;
    }
    void clear() noexcept { n_ = 0; }
    std::size_t size() const noexcept { return n_; }
    bool empty() const noexcept { return n_ == 0; }
    const Event& operator[](std::size_t i) const noexcept { return buf_[i]; }
    const Event* begin() const noexcept { return buf_.get(); }
    const Event* end() const noexcept { return buf_.get() + n_; }

private:
    void grow() {
        std::unique_ptr<Event[]> nb(new Event[cap_ * 2]);
        std::memcpy(nb.get(), buf_.get(), n_ * sizeof(Event));
        buf_ = std::move(nb);
        cap_ *= 2;
    }
    std::unique_ptr<Event[]> buf_;
    std::size_t n_ = 0;
    std::size_t cap_;
};

// ---- printing helpers (tests / tools) -------------------------------------
const char* to_string(Side) noexcept;
const char* to_string(EventType) noexcept;
const char* to_string(Reason) noexcept;
const char* to_string(MsgType) noexcept;
std::ostream& operator<<(std::ostream&, const Event&);
std::ostream& operator<<(std::ostream&, const Message&);
std::ostream& operator<<(std::ostream&, const LevelInfo&);

}  // namespace ob
