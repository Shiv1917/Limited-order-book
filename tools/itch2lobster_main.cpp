// ob_itch2lobster: converts one symbol of a NASDAQ ITCH 5.0 feed into a
// LOBSTER-format message file plus an orderbook (snapshot) file, the inputs
// ob-sim expects. Free LOBSTER samples are no longer downloadable, and
// LOBSTER files are themselves built from ITCH, so this is the closest
// honest substitute.
//
//   gunzip -c data/itch_raw/01302019_head600M.gz |
//     ob_itch2lobster --symbol AAPL --levels 10 --out data/AAPL_2019-01-30
//   -> data/AAPL_2019-01-30_message_10.csv, data/AAPL_2019-01-30_orderbook_10.csv
//
// Mapping (LOBSTER type <- ITCH):
//   1 new        <- A, F
//   2 partial    <- X
//   3 delete     <- D (size = remaining qty); U emits 3 (old id) then 1 (new id)
//   4 execution  <- E, and C (execution at another price; logged at the
//                   resting order's price so the level is identifiable)
//   5 hidden     <- P (non-displayed trade; direction = resting side)
//   6 cross      <- Q (opening/closing cross; size 0, direction -1)
// Halts ('H') are not decoded by the ITCH reader and are not emitted.
//
// The book is built from the start of the feed, so liquidity resting before
// --from is known; rows are emitted for ts >= --from. With the default
// --from 0 the file starts with an empty book and needs no seeding.
// The orderbook file is produced by this project's own book, so ob-sim's
// parity check against it is a consistency check of the LOBSTER replay path
// (seeding, id resolution, reductions), not an independent reference; the
// independent real-data check of the book is the ITCH replay in docs/design.md.
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>

#include "itch.hpp"
#include "ob/book.hpp"

using namespace ob;

namespace {

std::uint64_t parse_hms(const std::string& s) {
    unsigned h = 0, m = 0, sec = 0;
    std::sscanf(s.c_str(), "%u:%u:%u", &h, &m, &sec);
    return (std::uint64_t{h} * 3600 + m * 60 + sec) * 1'000'000'000ull;
}

struct Writer {
    std::FILE* msg = nullptr;
    std::FILE* book = nullptr;
    std::size_t levels = 10;
    std::uint64_t rows = 0;

    void row(const Book& b, std::uint64_t ts, int type, std::uint64_t id, std::uint32_t size, std::int64_t px, int dir) {
        std::fprintf(msg, "%llu.%09llu,%d,%llu,%u,%lld,%d\n", static_cast<unsigned long long>(ts / 1'000'000'000ull),
                     static_cast<unsigned long long>(ts % 1'000'000'000ull), type, static_cast<unsigned long long>(id),
                     size, static_cast<long long>(px), dir);
        const auto asks = b.depth(Side::Sell, levels), bids = b.depth(Side::Buy, levels);
        for (std::size_t i = 0; i < levels; ++i) {
            if (i) std::fputc(',', book);
            if (i < asks.size()) std::fprintf(book, "%lld,%llu", static_cast<long long>(asks[i].price), static_cast<unsigned long long>(asks[i].qty));
            else std::fputs("9999999999,0", book);
            if (i < bids.size()) std::fprintf(book, ",%lld,%llu", static_cast<long long>(bids[i].price), static_cast<unsigned long long>(bids[i].qty));
            else std::fputs(",-9999999999,0", book);
        }
        std::fputc('\n', book);
        ++rows;
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string input = "-", symbol = "AAPL", out;
    std::uint64_t from = 0, until = UINT64_MAX;
    std::size_t levels = 10;
    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* n) { return std::strcmp(argv[i], n) == 0 && i + 1 < argc; };
        if (arg("--input")) input = argv[++i];
        else if (arg("--symbol")) symbol = argv[++i];
        else if (arg("--from")) from = parse_hms(argv[++i]);
        else if (arg("--until")) until = parse_hms(argv[++i]);
        else if (arg("--levels")) levels = std::strtoull(argv[++i], nullptr, 10);
        else if (arg("--out")) out = argv[++i];
        else {
            std::cerr << "unknown option " << argv[i] << "\n";
            return 2;
        }
    }
    if (out.empty()) {
        std::cerr << "usage: ob_itch2lobster [--input F|-] [--symbol AAPL] [--from HH:MM:SS] [--until HH:MM:SS] "
                     "[--levels 10] --out PREFIX\n";
        return 2;
    }
    std::FILE* in = input == "-" ? stdin : std::fopen(input.c_str(), "rb");
    if (!in) {
        std::cerr << "cannot open " << input << "\n";
        return 1;
    }
    const std::string suffix = "_" + std::to_string(levels) + ".csv";
    Writer w;
    w.levels = levels;
    w.msg = std::fopen((out + "_message" + suffix).c_str(), "w");
    w.book = std::fopen((out + "_orderbook" + suffix).c_str(), "w");
    if (!w.msg || !w.book) {
        std::cerr << "cannot create output files with prefix " << out << "\n";
        return 1;
    }

    itch::FrameReader reader(in);
    std::unique_ptr<Book> book;
    EventBuffer ev(1 << 12);
    std::unordered_map<OrderId, Side> out_of_band;
    bool have_locate = false;
    std::uint16_t locate = 0;
    std::uint64_t unknown = 0, oob = 0, skipped_before = 0, last_ts = 0;
    const std::uint8_t* p;
    std::size_t len;
    itch::Msg m;

    auto emit = [&](std::uint64_t ts, int type, std::uint64_t id, std::uint32_t size, std::int64_t px, int dir) {
        if (ts >= from) w.row(*book, ts, type, id, size, px, dir);
        else ++skipped_before;
    };
    auto add = [&](std::uint64_t ts, OrderId id, Side s, std::uint32_t px, std::uint32_t qty) {
        if (!book) {
            const Price tick = 100, half = 1 << 16;
            const Price base = std::max<Price>(0, (static_cast<Price>(px) / tick - half) * tick);
            book = std::make_unique<Book>(BookConfig{PriceBand{base, tick, 2 * half}, 1u << 20});
        }
        if (!book->band().valid(px)) {
            ++oob;
            out_of_band[id] = s;
            return;
        }
        ev.clear();
        book->process(Message::add(id, s, px, qty), ev);
        emit(ts, 1, id, qty, px, s == Side::Buy ? 1 : -1);
    };
    // Reduction of a resting order (X, E, C). Emits after applying, like LOBSTER.
    auto reduce = [&](std::uint64_t ts, int type, OrderId id, std::uint32_t shares) {
        const auto info = book ? book->find(id) : std::nullopt;
        if (!info) {
            if (!out_of_band.count(id)) ++unknown;
            return;
        }
        ev.clear();
        if (shares >= info->qty) book->process(Message::cancel(id), ev);
        else book->process(Message::modify(id, info->price, info->qty - shares), ev);
        emit(ts, type, id, std::min(shares, info->qty), info->price, info->side == Side::Buy ? 1 : -1);
    };
    auto remove = [&](std::uint64_t ts, OrderId id) -> std::optional<OrderInfo> {
        const auto info = book ? book->find(id) : std::nullopt;
        if (!info) {
            if (!out_of_band.erase(id)) ++unknown;
            return std::nullopt;
        }
        ev.clear();
        book->process(Message::cancel(id), ev);
        emit(ts, 3, id, info->qty, info->price, info->side == Side::Buy ? 1 : -1);
        return info;
    };

    while (reader.next(p, len)) {
        if (!itch::decode(p, len, m)) continue;
        if (m.type == 'R') {
            if (!have_locate && symbol == m.stock) locate = m.locate, have_locate = true;
            continue;
        }
        if (!have_locate || m.locate != locate || m.type == 'S') continue;
        if (m.ts_ns > until) break;
        last_ts = m.ts_ns;
        switch (m.type) {
            case 'A':
            case 'F': add(m.ts_ns, m.ref, m.side == 'B' ? Side::Buy : Side::Sell, m.price, m.shares); break;
            case 'E':
            case 'C': reduce(m.ts_ns, 4, m.ref, m.shares); break;
            case 'X': reduce(m.ts_ns, 2, m.ref, m.shares); break;
            case 'D': remove(m.ts_ns, m.ref); break;
            case 'U': {
                Side s;
                if (const auto info = remove(m.ts_ns, m.ref)) s = info->side;
                else if (auto it = out_of_band.find(m.ref); it != out_of_band.end()) s = it->second, out_of_band.erase(it);
                else break;
                add(m.ts_ns, m.new_ref, s, m.price, m.shares);
                break;
            }
            case 'P':
                if (book) emit(m.ts_ns, 5, 0, m.shares, m.price, m.side == 'B' ? 1 : -1);
                break;
            case 'Q':
                if (book) emit(m.ts_ns, 6, 0, 0, m.price, -1);
                break;
            default: break;
        }
    }
    if (in != stdin) std::fclose(in);
    std::fclose(w.msg);
    std::fclose(w.book);
    std::cout << "ob_itch2lobster: " << symbol << " -> " << out << "_{message,orderbook}" << suffix << "\n"
              << "  rows written " << w.rows << " (skipped before --from: " << skipped_before << ")\n"
              << "  last timestamp " << last_ts / 1'000'000'000ull << " s after midnight\n"
              << "  out-of-band orders " << oob << ", unknown refs " << unknown << "\n";
    return w.rows ? 0 : 1;
}
