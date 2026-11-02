// ITCH 5.0 decoding and replay validation on hand-assembled binary frames.
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <vector>

#include "itch.hpp"

using namespace ob;

namespace {
struct FrameBuilder {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> cur;
    void begin(char type, std::uint64_t ts) {
        cur.clear();
        cur.push_back(static_cast<std::uint8_t>(type));
        put(1, 2);   // stock locate
        put(0, 2);   // tracking number
        put(ts, 6);  // timestamp
    }
    void put(std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) cur.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void str(const char* s, int n) {
        for (int i = 0; i < n; ++i) cur.push_back(static_cast<std::uint8_t>(i < static_cast<int>(std::strlen(s)) ? s[i] : ' '));
    }
    void end() {
        bytes.push_back(static_cast<std::uint8_t>(cur.size() >> 8));
        bytes.push_back(static_cast<std::uint8_t>(cur.size()));
        bytes.insert(bytes.end(), cur.begin(), cur.end());
    }
    void stock_directory(const char* sym) {
        begin('R', 1);
        str(sym, 8);
        for (int i = 0; i < 20; ++i) cur.push_back(0);  // remaining R fields
        end();
    }
    void add(std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t price) {
        begin('A', 2);
        put(ref, 8);
        cur.push_back(static_cast<std::uint8_t>(side));
        put(shares, 4);
        str("TEST", 8);
        put(price, 4);
        end();
    }
    void exec(std::uint64_t ref, std::uint32_t shares) {
        begin('E', 3);
        put(ref, 8);
        put(shares, 4);
        put(777, 8);
        end();
    }
    void cancel(std::uint64_t ref, std::uint32_t shares) {
        begin('X', 4);
        put(ref, 8);
        put(shares, 4);
        end();
    }
    void del(std::uint64_t ref) {
        begin('D', 5);
        put(ref, 8);
        end();
    }
    void replace(std::uint64_t ref, std::uint64_t nref, std::uint32_t shares, std::uint32_t price) {
        begin('U', 6);
        put(ref, 8);
        put(nref, 8);
        put(shares, 4);
        put(price, 4);
        end();
    }
};
}  // namespace

TEST_CASE("itch decode + replay validation", "[itch]") {
    FrameBuilder fb;
    fb.stock_directory("TEST");
    fb.add(1, 'S', 100, 1'000'000);  // $100.00
    fb.add(2, 'S', 100, 1'000'000);
    fb.add(3, 'B', 300, 999'900);
    fb.exec(1, 40);       // front of best ask queue -> replayed as aggressor
    fb.exec(2, 10);       // behind order 1 -> flagged, applied as a reduction
    fb.cancel(1, 10);     // 100-40-10 = 50 left
    fb.replace(3, 4, 200, 999'800);
    fb.del(2);
    fb.bytes.push_back(0);  // truncated trailing frame must be ignored
    fb.bytes.push_back(40);

    std::FILE* f = std::tmpfile();
    REQUIRE(f);
    std::fwrite(fb.bytes.data(), 1, fb.bytes.size(), f);
    std::rewind(f);
    itch::FrameReader reader(f);
    itch::ReplayOptions opt;
    opt.symbol = "TEST";
    opt.differential = true;
    opt.half_range_ticks = 1000;
    itch::Replayer rep(opt);
    const std::uint8_t* p;
    std::size_t len;
    itch::Msg m;
    int frames = 0;
    while (reader.next(p, len)) {
        ++frames;
        if (itch::decode(p, len, m)) rep.on_msg(m);
    }
    std::fclose(f);
    REQUIRE(frames == 9);
    const auto& s = rep.stats();
    REQUIRE(s.adds == 3);
    REQUIRE(s.execs == 2);
    REQUIRE(s.exec_checked_ok == 1);
    REQUIRE(s.exec_not_front == 1);
    REQUIRE(s.nf_same_level == 1);
    REQUIRE(s.unknown_refs == 0);
    REQUIRE(s.diff_mismatches == 0);
    const Book& b = *rep.book();
    REQUIRE(b.find(1)->qty == 50);
    REQUIRE(!b.find(2));
    REQUIRE(!b.find(3));
    REQUIRE(b.find(4)->price == 999'800);
    REQUIRE(b.find(4)->qty == 200);
    REQUIRE(rep.stream().msgs.size() == s.engine_msgs);
}
