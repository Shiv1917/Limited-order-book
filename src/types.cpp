#include "ob/types.hpp"

#include <ostream>

namespace ob {

const char* to_string(Side s) noexcept { return s == Side::Buy ? "BUY" : "SELL"; }

const char* to_string(EventType t) noexcept {
    switch (t) {
        case EventType::Ack: return "ACK";
        case EventType::Trade: return "TRADE";
        case EventType::Cancel: return "CANCEL";
        case EventType::Reject: return "REJECT";
    }
    return "?";
}

const char* to_string(Reason r) noexcept {
    switch (r) {
        case Reason::None: return "-";
        case Reason::New: return "new";
        case Reason::Modified: return "modified";
        case Reason::UserCancel: return "user-cancel";
        case Reason::Unfilled: return "unfilled";
        case Reason::FokUnfillable: return "fok-unfillable";
        case Reason::InvalidQty: return "invalid-qty";
        case Reason::InvalidPrice: return "invalid-price";
        case Reason::DuplicateId: return "duplicate-id";
        case Reason::UnknownOrder: return "unknown-order";
    }
    return "?";
}

const char* to_string(MsgType t) noexcept {
    switch (t) {
        case MsgType::Add: return "ADD";
        case MsgType::Cancel: return "CANCEL";
        case MsgType::Modify: return "MODIFY";
        case MsgType::Market: return "MARKET";
    }
    return "?";
}

std::ostream& operator<<(std::ostream& os, const Event& e) {
    os << to_string(e.type) << "{id=" << e.id << " " << to_string(e.side);
    if (e.type == EventType::Trade) os << " maker=" << e.maker_id;
    os << " px=" << e.price << " qty=" << e.qty << " reason=" << to_string(e.reason) << "}";
    return os;
}

std::ostream& operator<<(std::ostream& os, const Message& m) {
    static const char* tifs[] = {"GTC", "IOC", "FOK"};
    os << to_string(m.type) << "{id=" << m.id << " " << to_string(m.side) << " px=" << m.price << " qty=" << m.qty
       << " " << tifs[static_cast<int>(m.tif) % 3] << "}";
    return os;
}

std::ostream& operator<<(std::ostream& os, const LevelInfo& l) {
    return os << "{px=" << l.price << " qty=" << l.qty << " n=" << l.count << "}";
}

}  // namespace ob
