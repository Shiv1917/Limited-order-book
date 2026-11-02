// Long-running differential fuzzer CLI.
//   ob_fuzz_diff --seeds 1000 --messages 100000 [--start-seed 1] [--adversarial-only]
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "fuzz_common.hpp"

int main(int argc, char** argv) {
    std::uint64_t seeds = 50, start = 1;
    std::size_t messages = 50'000;
    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* name) { return std::strcmp(argv[i], name) == 0 && i + 1 < argc; };
        if (arg("--seeds")) seeds = std::strtoull(argv[++i], nullptr, 10);
        else if (arg("--messages")) messages = std::strtoull(argv[++i], nullptr, 10);
        else if (arg("--start-seed")) start = std::strtoull(argv[++i], nullptr, 10);
        else {
            std::cerr << "usage: " << argv[0] << " [--seeds N] [--messages N] [--start-seed S]\n";
            return 2;
        }
    }
    std::size_t total = 0, trades = 0;
    std::size_t by_reason[16] = {};
    for (std::uint64_t s = start; s < start + seeds; ++s) {
        ob::SynthConfig cfg;
        cfg.seed = s;
        if (s % 2) {  // alternate realistic and adversarial/narrow-band configurations
            cfg.adversarial = true;
            cfg.band = ob::PriceBand{500, 5, 64 + static_cast<std::uint32_t>(s % 256)};
            cfg.p_aggressive = 0.3;
            cfg.max_qty = 40;
        }
        // Full state compare every message for small runs, sparser for long ones.
        const auto r = obtest::run_differential(cfg, messages, messages > 100'000 ? 16 : 1);
        total += r.messages;
        trades += r.trades;
        for (int k = 0; k < 16; ++k) by_reason[k] += r.by_reason[k];
        if (!r.ok) {
            std::cerr << "MISMATCH: " << r.error << "\n";
            return 1;
        }
    }
    std::cout << "ok: " << seeds << " seeds, " << total << " messages, all books identical\n";
    std::cout << "coverage: trades=" << trades;
    for (int k = 1; k <= static_cast<int>(ob::Reason::UnknownOrder); ++k)
        std::cout << " " << ob::to_string(static_cast<ob::Reason>(k)) << "=" << by_reason[k];
    std::cout << "\n";
    return 0;
}
