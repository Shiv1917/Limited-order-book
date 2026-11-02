// ob_synth: write a synthetic message stream to an .obm file.
//   ob_synth OUT.obm [--messages N] [--seed S] [--adversarial]
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "msgfile.hpp"
#include "synthetic.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: ob_synth OUT.obm [--messages N] [--seed S] [--adversarial]\n";
        return 2;
    }
    ob::SynthConfig cfg;
    std::size_t n = 1'000'000;
    for (int i = 2; i < argc; ++i) {
        auto arg = [&](const char* name) { return std::strcmp(argv[i], name) == 0 && i + 1 < argc; };
        if (arg("--messages")) n = std::strtoull(argv[++i], nullptr, 10);
        else if (arg("--seed")) cfg.seed = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--adversarial") == 0) cfg.adversarial = true;
    }
    ob::SyntheticFlow gen(cfg);
    ob::MessageStream s;
    s.band = cfg.band;
    s.symbol = "SYNTH";
    s.msgs = gen.generate(n);
    std::string err;
    if (!ob::save_stream(argv[1], s, &err)) {
        std::cerr << err << "\n";
        return 1;
    }
    std::cout << "wrote " << n << " messages to " << argv[1] << "\n";
    return 0;
}
