// ob_replay: real-data parser / replayer / validator.
//
//   ob_replay itch    [--input FILE|-] [--symbol AAPL] [--until HH:MM:SS] [--max N] [--diff] [--out F.obm]
//   ob_replay lobster --messages F --orderbook F [--levels N] [--check-every K] [--out F.obm]
//   ob_replay obm     F.obm   (re-run a stored stream through reference + fast book and compare)
//
// Example (streams straight from NASDAQ's public sample without unpacking to disk):
//   curl -s https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz | gunzip -c |
//     ob_replay itch --symbol AAPL --until 16:00:00 --diff --out data/aapl.obm
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

#include "itch.hpp"
#include "lobster.hpp"
#include "msgfile.hpp"
#include "ob/book.hpp"
#include "ref_book.hpp"

using namespace ob;

namespace {

std::string fmt_ts(std::uint64_t ns) {
    const std::uint64_t s = ns / 1'000'000'000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu.%06llu", static_cast<unsigned long long>(s / 3600),
                  static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60),
                  static_cast<unsigned long long>(ns / 1000 % 1'000'000));
    return buf;
}

std::uint64_t parse_hms(const std::string& s) {
    unsigned h = 0, m = 0, sec = 0;
    std::sscanf(s.c_str(), "%u:%u:%u", &h, &m, &sec);
    return (std::uint64_t{h} * 3600 + m * 60 + sec) * 1'000'000'000ull;
}

double pct(std::uint64_t a, std::uint64_t b) { return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; }

void print_top(const Book& b) {
    auto bb = b.depth(Side::Buy, 5), aa = b.depth(Side::Sell, 5);
    std::cout << "  final top of book (5 levels):\n      bid qty      bid px |      ask px     ask qty\n";
    for (std::size_t i = 0; i < std::max(bb.size(), aa.size()); ++i) {
        std::cout << "  ";
        if (i < bb.size()) std::cout << std::setw(11) << bb[i].qty << " " << std::setw(11) << std::fixed << std::setprecision(2) << static_cast<double>(bb[i].price) / 1e4;
        else std::cout << std::setw(23) << "";
        std::cout << " | ";
        if (i < aa.size()) std::cout << std::setw(11) << std::fixed << std::setprecision(2) << static_cast<double>(aa[i].price) / 1e4 << " " << std::setw(11) << aa[i].qty;
        std::cout << "\n";
    }
}

int cmd_itch(int argc, char** argv) {
    itch::ReplayOptions opt;
    std::string input = "-", out;
    for (int i = 2; i < argc; ++i) {
        auto arg = [&](const char* n) { return std::strcmp(argv[i], n) == 0 && i + 1 < argc; };
        if (arg("--input")) input = argv[++i];
        else if (arg("--symbol")) opt.symbol = argv[++i];
        else if (arg("--until")) opt.end_ns = parse_hms(argv[++i]);
        else if (arg("--max")) opt.max_messages = std::strtoull(argv[++i], nullptr, 10);
        else if (arg("--out")) out = argv[++i];
        else if (std::strcmp(argv[i], "--diff") == 0) opt.differential = true;
        else {
            std::cerr << "unknown option " << argv[i] << "\n";
            return 2;
        }
    }
    std::FILE* f = input == "-" ? stdin : std::fopen(input.c_str(), "rb");
    if (!f) {
        std::cerr << "cannot open " << input << "\n";
        return 1;
    }
    itch::FrameReader reader(f);
    itch::Replayer rep(opt);
    const std::uint8_t* p;
    std::size_t len;
    itch::Msg m;
    const auto t0 = std::chrono::steady_clock::now();
    while (reader.next(p, len)) {
        if (!itch::decode(p, len, m)) continue;
        if (!rep.on_msg(m)) break;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (f != stdin) std::fclose(f);

    const auto& s = rep.stats();
    std::cout << "ITCH 5.0 replay: symbol " << opt.symbol << " (locate " << s.locate << ")\n"
              << "  bytes read           " << reader.bytes() << " in " << secs << " s\n"
              << "  frames decoded       " << s.frames << "\n"
              << "  symbol messages      " << s.symbol_msgs << "  [" << fmt_ts(s.first_ts) << " .. " << fmt_ts(s.last_ts) << "]\n"
              << "  market open (S/Q)    " << (s.market_open_ts ? fmt_ts(s.market_open_ts) : "not reached") << "\n"
              << "  adds / execs / exec-with-price / partial-cancels / deletes / replaces\n      "
              << s.adds << " / " << s.execs << " / " << s.execs_with_price << " / " << s.partial_cancels << " / "
              << s.deletes << " / " << s.replaces << "\n"
              << "  hidden (P) / cross (Q) trades  " << s.hidden_trades << " / " << s.cross_trades << "\n"
              << "  engine messages      " << s.engine_msgs << " (" << s.trades << " trades, " << s.rejects << " rejects)\n"
              << "  out-of-band orders   " << s.out_of_band << "\n"
              << "  crossed adds         " << s.crossed_adds << "\n"
              << "  unknown order refs   " << s.unknown_refs << "\n"
              << "VALIDATION (executions vs our book):\n"
              << "  exec at front of best queue, replayed as aggressor, trade matched: " << s.exec_checked_ok << " ("
              << std::fixed << std::setprecision(3) << pct(s.exec_checked_ok, s.execs) << "%)\n"
              << "  exec matched but trade differed: " << s.exec_trade_mismatch << "\n"
              << "  exec NOT at front of best queue (applied as reduction): " << s.exec_not_front << " ("
              << pct(s.exec_not_front, s.execs) << "%)\n"
              << "      of which: opening cross " << s.nf_at_open << ", same price but queued behind others "
              << s.nf_same_level << ", not at best price " << s.nf_worse_level << ", larger than resting qty "
              << s.nf_oversize << "\n";
    if (opt.differential) std::cout << "  reference-vs-fast mismatches: " << s.diff_mismatches << "\n";
    if (rep.book()) print_top(*rep.book());
    if (!out.empty()) {
        std::string err;
        if (!save_stream(out, rep.stream(), &err)) {
            std::cerr << err << "\n";
            return 1;
        }
        std::cout << "wrote " << rep.stream().msgs.size() << " engine messages to " << out << "\n";
    }
    return (opt.differential && s.diff_mismatches) ? 1 : 0;
}

int cmd_lobster(int argc, char** argv) {
    std::string msg_path, book_path, out;
    std::size_t levels = 10, every = 1;
    for (int i = 2; i < argc; ++i) {
        auto arg = [&](const char* n) { return std::strcmp(argv[i], n) == 0 && i + 1 < argc; };
        if (arg("--messages")) msg_path = argv[++i];
        else if (arg("--orderbook")) book_path = argv[++i];
        else if (arg("--levels")) levels = std::strtoull(argv[++i], nullptr, 10);
        else if (arg("--check-every")) every = std::strtoull(argv[++i], nullptr, 10);
        else if (arg("--out")) out = argv[++i];
        else {
            std::cerr << "unknown option " << argv[i] << "\n";
            return 2;
        }
    }
    std::string err;
    const auto rows = lobster::load_messages(msg_path, &err);
    if (rows.empty()) {
        std::cerr << (err.empty() ? "no rows" : err) << "\n";
        return 1;
    }
    const auto snaps = lobster::load_snapshots(book_path, &err);
    if (snaps.size() != rows.size()) {
        std::cerr << (err.empty() ? "message/orderbook row count mismatch" : err) << "\n";
        return 1;
    }
    // Band: all prices seen, +/- margin, 1-cent ticks.
    std::int64_t lo = INT64_MAX, hi = 0;
    for (const auto& r : rows) lo = std::min(lo, r.price), hi = std::max(hi, r.price);
    for (const auto& s : snaps)
        for (std::size_t i = 0; i < s.levels(); ++i) {
            if (s.ask_px(i) != lobster::kEmptyAsk) lo = std::min(lo, s.ask_px(i)), hi = std::max(hi, s.ask_px(i));
            if (s.bid_px(i) != lobster::kEmptyBid) lo = std::min(lo, s.bid_px(i)), hi = std::max(hi, s.bid_px(i));
        }
    const Price tick = 100, base = std::max<Price>(0, (lo / tick - 1024) * tick);
    const auto nlev = static_cast<std::uint32_t>((hi - base) / tick + 1024);
    const BookConfig cfg{PriceBand{base, tick, nlev}, 1u << 20};
    Book book(cfg);
    lobster::Replayer<Book> rep(book, levels);
    MessageStream stream;
    stream.band = cfg.band;
    rep.seed(snaps[0], rows[0]);
    for (std::size_t k = 0; k < rows.size(); ++k) {
        rep.apply(rows[k], &stream.msgs);
        if (k % every == 0 || k + 1 == rows.size()) rep.check(snaps[k], k);
    }
    const auto& s = rep.stats;
    std::cout << "LOBSTER replay: " << s.messages << " rows (" << s.adds << " adds, " << s.partial_cancels
              << " partial cancels, " << s.deletes << " deletes, " << s.executions << " executions, " << s.hidden
              << " hidden)\n"
              << "  seeded levels " << s.seeded_levels << ", unknown refs " << s.unknown_refs << ", rejects " << s.rejects << "\n"
              << "  checkpoints " << s.checkpoints << ": top-of-book mismatches " << s.l1_mismatches << ", depth("
              << levels << ") mismatches " << s.depth_mismatches << "\n";
    if (!s.first_mismatch.empty()) std::cout << "  first mismatch at " << s.first_mismatch << "\n";
    if (!out.empty() && !save_stream(out, stream, &err)) {
        std::cerr << err << "\n";
        return 1;
    }
    return s.l1_mismatches ? 1 : 0;
}

int cmd_obm(int argc, char** argv) {
    if (argc < 3) return 2;
    MessageStream s;
    std::string err;
    if (!load_stream(argv[2], s, &err)) {
        std::cerr << err << "\n";
        return 1;
    }
    const BookConfig cfg{s.band, 1u << 20};
    RefBook ref(cfg);
    Book fast(cfg);
    EventBuffer a, b;
    std::size_t mism = 0;
    for (const auto& m : s.msgs) {
        a.clear(), b.clear();
        ref.process(m, a);
        fast.process(m, b);
        if (a.size() != b.size() || !std::equal(a.begin(), a.end(), b.begin()) || ref.best_bid() != fast.best_bid() ||
            ref.best_ask() != fast.best_ask())
            ++mism;
    }
    std::cout << s.msgs.size() << " messages (" << s.symbol << "), reference vs fast mismatches: " << mism << "\n";
    print_top(fast);
    return mism ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: ob_replay itch|lobster|obm ...\n";
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "itch") return cmd_itch(argc, argv);
    if (cmd == "lobster") return cmd_lobster(argc, argv);
    if (cmd == "obm") return cmd_obm(argc, argv);
    std::cerr << "unknown command " << cmd << "\n";
    return 2;
}
