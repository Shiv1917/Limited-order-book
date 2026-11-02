#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include "fuzz_common.hpp"

using namespace ob;
using namespace obtest;

TEST_CASE("differential fuzz: realistic flow", "[fuzz]") {
    const auto seed = GENERATE(range<std::uint64_t>(1, 9));
    SynthConfig cfg;
    cfg.seed = seed;
    const auto r = run_differential(cfg, 20'000, 1);
    INFO(r.error);
    REQUIRE(r.ok);
}

TEST_CASE("differential fuzz: adversarial flow in a narrow band", "[fuzz]") {
    const auto seed = GENERATE(range<std::uint64_t>(100, 108));
    SynthConfig cfg;
    cfg.seed = seed;
    cfg.adversarial = true;
    cfg.band = PriceBand{500, 5, 200};  // tiny band: lots of crossing, band-edge and sweep cases
    cfg.p_aggressive = 0.3;
    cfg.max_qty = 50;
    const auto r = run_differential(cfg, 20'000, 1);
    INFO(r.error);
    REQUIRE(r.ok);
}
