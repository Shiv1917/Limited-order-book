#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <random>
#include <sstream>

#include "ob/histogram.hpp"

TEST_CASE("histogram percentiles within relative error", "[histogram]") {
    ob::Histogram h;
    std::vector<std::uint64_t> v;
    std::mt19937_64 rng(3);
    std::lognormal_distribution<double> d(5.0, 1.5);
    for (int i = 0; i < 200'000; ++i) {
        const auto x = static_cast<std::uint64_t>(d(rng));
        v.push_back(x);
        h.record(x);
    }
    std::sort(v.begin(), v.end());
    for (double p : {50.0, 90.0, 99.0, 99.9}) {
        const auto exact = v[static_cast<std::size_t>(std::ceil(p / 100 * static_cast<double>(v.size()))) - 1];
        const auto got = h.percentile(p);
        REQUIRE(static_cast<double>(got) >= static_cast<double>(exact));
        REQUIRE(static_cast<double>(got) <= static_cast<double>(exact) * 1.001 + 1);
    }
    REQUIRE(h.percentile(100) == v.back());
    REQUIRE(h.count() == v.size());
    std::ostringstream os;
    h.print_percentiles(os);
    REQUIRE(os.str().find("Total count = 200000") != std::string::npos);
}

TEST_CASE("histogram exact for small values", "[histogram]") {
    ob::Histogram h;
    for (std::uint64_t i = 1; i <= 1000; ++i) h.record(i);
    REQUIRE(h.percentile(50) == 500);
    REQUIRE(h.percentile(99) == 990);
    REQUIRE(h.min() == 1);
    REQUIRE(h.mean() == Catch::Approx(500.5));
}
