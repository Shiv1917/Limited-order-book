#include "itch.hpp"

#include <algorithm>
#include <cstring>

namespace ob::itch {
namespace {

inline std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
inline std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
inline std::uint64_t be48(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | p[i];
    return v;
}
inline std::uint64_t be64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
void copy_stock(const std::uint8_t* p, char (&out)[9]) {
    std::memcpy(out, p, 8);
    out[8] = '\0';
    for (int i = 7; i >= 0 && out[i] == ' '; --i) out[i] = '\0';
}

}  // namespace

bool decode(const std::uint8_t* p, std::size_t len, Msg& m) {
    if (len < 11) return false;
    m.type = static_cast<char>(p[0]);
    m.locate = be16(p + 1);
    m.ts_ns = be48(p + 5);
    switch (m.type) {
        case 'S':
            if (len < 12) return false;
            m.event = static_cast<char>(p[11]);
            return true;
        case 'R':
            if (len < 39) return false;
            copy_stock(p + 11, m.stock);
            return true;
        case 'A':
        case 'F':
            if (len < 36) return false;
            m.ref = be64(p + 11);
            m.side = static_cast<char>(p[19]);
            m.shares = be32(p + 20);
            copy_stock(p + 24, m.stock);
            m.price = be32(p + 32);
            return true;
        case 'E':
            if (len < 31) return false;
            m.ref = be64(p + 11);
            m.shares = be32(p + 19);
            return true;
        case 'C':
            if (len < 36) return false;
            m.ref = be64(p + 11);
            m.shares = be32(p + 19);
            m.price = be32(p + 32);
            return true;
        case 'X':
            if (len < 23) return false;
            m.ref = be64(p + 11);
            m.shares = be32(p + 19);
            return true;
        case 'D':
            if (len < 19) return false;
            m.ref = be64(p + 11);
            return true;
        case 'U':
            if (len < 35) return false;
            m.ref = be64(p + 11);
            m.new_ref = be64(p + 19);
            m.shares = be32(p + 27);
            m.price = be32(p + 31);
            return true;
        case 'P':
            if (len < 44) return false;
            m.side = static_cast<char>(p[19]);
            m.shares = be32(p + 20);
            m.price = be32(p + 32);
            return true;
        case 'Q':
            if (len < 40) return false;
            m.price = be32(p + 27);
            return true;
        default: return false;
    }
}

FrameReader::FrameReader(std::FILE* f, std::size_t buf_size) : f_(f), buf_(buf_size) {}

bool FrameReader::fill(std::size_t need) {
    if (end_ - pos_ >= need) return true;
    std::memmove(buf_.data(), buf_.data() + pos_, end_ - pos_);
    end_ -= pos_;
    pos_ = 0;
    while (end_ < need) {
        const std::size_t got = std::fread(buf_.data() + end_, 1, buf_.size() - end_, f_);
        if (got == 0) return false;
        end_ += got;
    }
    return true;
}

bool FrameReader::next(const std::uint8_t*& payload, std::size_t& len) {
    if (!fill(2)) return false;
    len = be16(buf_.data() + pos_);
    if (!fill(2 + len)) return false;  // truncated trailing frame (e.g. partial download)
    payload = buf_.data() + pos_ + 2;
    pos_ += 2 + len;
    consumed_ += 2 + len;
    return true;
}

Replayer::Replayer(ReplayOptions opts) : opts_(std::move(opts)) { stream_.symbol = opts_.symbol; }

void Replayer::ensure_book(std::uint32_t first_price) {
    if (book_) return;
    const Price center = (static_cast<Price>(first_price) / opts_.tick) * opts_.tick;
    const Price base = std::max<Price>(0, center - static_cast<Price>(opts_.half_range_ticks) * opts_.tick);
    BookConfig cfg{PriceBand{base, opts_.tick, 2 * opts_.half_range_ticks}, 1u << 20};
    book_ = std::make_unique<Book>(cfg);
    if (opts_.differential) ref_ = std::make_unique<RefBook>(cfg);
    stream_.band = cfg.band;
}

void Replayer::send(const Message& m, std::uint64_t ts) {
    out_.clear();
    book_->process(m, out_);
    ++stats_.engine_msgs;
    for (const Event& e : out_) {
        if (e.type == EventType::Reject) ++stats_.rejects;
        else if (e.type == EventType::Trade) ++stats_.trades;
    }
    if (ref_) {
        ref_out_.clear();
        ref_->process(m, ref_out_);
        const bool same_events = ref_out_.size() == out_.size() && std::equal(out_.begin(), out_.end(), ref_out_.begin());
        if (!same_events || ref_->best_bid() != book_->best_bid() || ref_->best_ask() != book_->best_ask())
            ++stats_.diff_mismatches;
    }
    stream_.msgs.push_back(m);
    stream_.ts_ns.push_back(ts);
    if (stream_.msgs.size() >= opts_.max_messages) done_ = true;
}

void Replayer::reduce(OrderId id, std::uint32_t shares, std::uint64_t ts) {
    const auto info = book_->find(id);
    if (!info) {
        if (out_of_band_.count(id) == 0) ++stats_.unknown_refs;
        return;
    }
    if (shares >= info->qty) send(Message::cancel(id), ts);
    else send(Message::modify(id, info->price, info->qty - shares), ts);
}

bool Replayer::on_msg(const Msg& m) {
    if (done_) return false;
    ++stats_.frames;
    if (m.type == 'S' && m.event == 'Q') stats_.market_open_ts = m.ts_ns;
    if (m.type == 'R') {
        if (!have_locate_ && opts_.symbol == m.stock) {
            stats_.locate = m.locate;
            have_locate_ = true;
        }
        return true;
    }
    if (!have_locate_ || m.locate != stats_.locate || m.type == 'S') return true;
    if (m.ts_ns > opts_.end_ns) {
        done_ = true;
        return false;
    }
    ++stats_.symbol_msgs;
    if (!stats_.first_ts) stats_.first_ts = m.ts_ns;
    stats_.last_ts = m.ts_ns;

    auto add = [&](OrderId ref, Side side, std::uint32_t price, std::uint32_t shares) {
        ensure_book(price);
        if (!book_->band().valid(price)) {
            ++stats_.out_of_band;
            out_of_band_[ref] = side;
            return;
        }
        const auto opp = book_->best_price(opposite(side));
        if (opp && (side == Side::Buy ? static_cast<Price>(price) >= *opp : static_cast<Price>(price) <= *opp))
            ++stats_.crossed_adds;
        send(Message::add(ref, side, price, shares), m.ts_ns);
    };

    switch (m.type) {
        case 'A':
        case 'F':
            ++stats_.adds;
            add(m.ref, m.side == 'B' ? Side::Buy : Side::Sell, m.price, m.shares);
            break;
        case 'E': {
            ++stats_.execs;
            if (!book_) break;
            const auto info = book_->find(m.ref);
            if (!info) {
                if (out_of_band_.count(m.ref) == 0) ++stats_.unknown_refs;
                break;
            }
            if (book_->is_front_of_best(m.ref) && m.shares <= info->qty) {
                // Replay as the aggressor the exchange must have seen.
                const OrderId taker = next_taker_++;
                send(Message::add(taker, opposite(info->side), info->price, m.shares, TimeInForce::IOC), m.ts_ns);
                const bool ok = out_.size() == 2 && out_[1].type == EventType::Trade && out_[1].maker_id == m.ref &&
                                out_[1].qty == m.shares && out_[1].price == info->price;
                ok ? ++stats_.exec_checked_ok : ++stats_.exec_trade_mismatch;
            } else {
                ++stats_.exec_not_front;
                if (stats_.market_open_ts && m.ts_ns >= stats_.market_open_ts &&
                    m.ts_ns < stats_.market_open_ts + 1'000'000'000ull)
                    ++stats_.nf_at_open;
                else if (m.shares > info->qty) ++stats_.nf_oversize;
                else if (book_->best_price(info->side) == info->price) ++stats_.nf_same_level;
                else ++stats_.nf_worse_level;
                reduce(m.ref, m.shares, m.ts_ns);
            }
            break;
        }
        case 'C':  // execution at a price other than the order's (e.g. crosses)
            ++stats_.execs_with_price;
            if (book_) reduce(m.ref, m.shares, m.ts_ns);
            break;
        case 'X':
            ++stats_.partial_cancels;
            if (book_) reduce(m.ref, m.shares, m.ts_ns);
            break;
        case 'D':
            ++stats_.deletes;
            if (!book_) break;
            if (book_->find(m.ref)) send(Message::cancel(m.ref), m.ts_ns);
            else if (!out_of_band_.erase(m.ref)) ++stats_.unknown_refs;
            break;
        case 'U': {
            ++stats_.replaces;
            if (!book_) break;
            Side side;
            if (const auto info = book_->find(m.ref)) {
                side = info->side;
                send(Message::cancel(m.ref), m.ts_ns);
            } else if (auto it = out_of_band_.find(m.ref); it != out_of_band_.end()) {
                side = it->second;
                out_of_band_.erase(it);
            } else {
                ++stats_.unknown_refs;
                break;
            }
            add(m.new_ref, side, m.price, m.shares);  // replace = new order, loses priority
            break;
        }
        case 'P': ++stats_.hidden_trades; break;
        case 'Q': ++stats_.cross_trades; break;
        default: break;
    }
    return !done_;
}

}  // namespace ob::itch
