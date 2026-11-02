// LOBSTER parser + exact replay against a hand-built fixture whose first
// snapshot contains liquidity that predates the message file.
#include <catch2/catch_test_macros.hpp>

#include "lobster.hpp"
#include "ob/book.hpp"
#include "ref_book.hpp"

using namespace ob;

namespace {
const std::string dir = OB_TEST_DATA_DIR;

template <class B>
lobster::ReplayStats replay(std::vector<lobster::Snapshot> snaps) {
    const auto rows = lobster::load_messages(dir + "/TEST_message_2.csv");
    REQUIRE(rows.size() == 8);
    REQUIRE(snaps.size() == rows.size());
    B book(BookConfig{PriceBand{990000, 100, 512}, 64});
    lobster::Replayer<B> rep(book, 2);
    rep.seed(snaps[0], rows[0]);
    for (std::size_t k = 0; k < rows.size(); ++k) {
        rep.apply(rows[k]);
        rep.check(snaps[k], k);
    }
    return rep.stats;
}
}  // namespace

TEST_CASE("lobster row parsing", "[lobster]") {
    lobster::Row r;
    REQUIRE(lobster::parse_row("34200.004241176,1,16113575,18,5853300,1", r));
    REQUIRE(r.type == 1);
    REQUIRE(r.id == 16113575);
    REQUIRE(r.size == 18);
    REQUIRE(r.price == 5853300);
    REQUIRE(r.direction == 1);
    REQUIRE_FALSE(lobster::parse_row("34200.1,9,1,1,1,1", r));  // bad type
    REQUIRE_FALSE(lobster::parse_row("garbage", r));
    lobster::Snapshot s;
    REQUIRE(lobster::parse_snapshot("5859400,200,5853300,18", s));
    REQUIRE(s.levels() == 1);
    REQUIRE(s.bid_sz(0) == 18);
}

TEST_CASE("lobster exact replay matches every snapshot", "[lobster]") {
    const auto snaps = lobster::load_snapshots(dir + "/TEST_orderbook_2.csv");
    for (const auto& st : {replay<Book>(snaps), replay<RefBook>(snaps)}) {
        INFO(st.first_mismatch);
        REQUIRE(st.seeded_levels == 2);
        REQUIRE(st.unknown_refs == 0);
        REQUIRE(st.rejects == 0);
        REQUIRE(st.checkpoints == 8);
        REQUIRE(st.depth_mismatches == 0);
        REQUIRE(st.hidden == 1);
    }
}

TEST_CASE("lobster replay detects a corrupted snapshot", "[lobster]") {
    auto snaps = lobster::load_snapshots(dir + "/TEST_orderbook_2.csv");
    snaps[3].v[3] = 151;  // bid size at level 1 off by one
    const auto st = replay<Book>(snaps);
    REQUIRE(st.l1_mismatches == 1);
    REQUIRE(st.first_mismatch == "row 3 level 1");
}
