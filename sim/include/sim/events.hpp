// Event types flowing through the simulator.
//
//   MdEvent     historical market data (one LOBSTER row)       replayer -> exchange, strategy
//   OrderRequest strategy new/cancel, after the entry latency   strategy -> exchange
//   OrderEvent  ack / reject / fill / cancelled                 exchange -> portfolio now,
//                                                               strategy after return latency
//   timers      strategy-owned wakeups
#pragma once

#include <cstdint>

#include "sim/time.hpp"

namespace sim {

enum class MdType : std::uint8_t {
    Add = 1,
    PartialCancel = 2,
    Delete = 3,
    Execute = 4,  // visible execution of a resting order
    Hidden = 5,   // execution against hidden liquidity
    Cross = 6,    // auction / cross trade
    Halt = 7,
};

struct MdEvent {
    Time ts = 0;
    OrderId id = 0;
    Price price = 0;
    Qty qty = 0;
    MdType type = MdType::Add;
    Side side = Side::Buy;  // side of the resting order (LOBSTER direction)
};

enum class OrderType : std::uint8_t { Limit = 0, Market = 1, IOC = 2 };

enum class OrderEventType : std::uint8_t { Ack = 0, Reject = 1, Fill = 2, Cancelled = 3 };

enum class RejectReason : std::uint8_t {
    None = 0,
    InvalidQty,
    InvalidPrice,
    MaxOrderSize,
    MaxPosition,
    MaxOpenOrders,
    SelfTrade,
    Halted,
    UnknownOrder,
    TooLateToCancel,
};

enum class CancelReason : std::uint8_t {
    None = 0,
    User,      // strategy cancel
    Unfilled,  // IOC / market remainder
    Halt,      // halt or cross message: all strategy orders pulled
};

struct OrderRequest {
    OrderId id = 0;
    Price price = 0;  // ignored for Market
    Qty qty = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    bool cancel = false;  // true: cancel request for `id`
    Time sent = 0;        // strategy time the request was made
};

struct OrderEvent {
    Time ts = 0;  // exchange time it happened (strategy receives it later)
    OrderId id = 0;
    Price price = 0;  // fill price / order price
    Qty qty = 0;      // fill qty, cancelled qty, or order qty for acks
    Qty leaves = 0;   // remaining open qty after this event
    OrderEventType type = OrderEventType::Ack;
    Side side = Side::Buy;
    bool maker = false;       // fills: true if our resting order provided liquidity
    bool for_cancel = false;  // rejects: the rejected request was a cancel
    RejectReason reject = RejectReason::None;
    CancelReason cancel = CancelReason::None;
    // Diagnostics (not needed by strategies):
    Time arrived = 0;              // when the order reached the exchange
    std::uint64_t queue_ahead = 0;  // displayed qty ahead of it when it rested
};

const char* to_string(OrderType) noexcept;
const char* to_string(RejectReason) noexcept;
const char* to_string(CancelReason) noexcept;

}  // namespace sim
