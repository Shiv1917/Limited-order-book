// ob_bench: per-message latency + throughput for every optimisation stage.
//
//   ob_bench [--stream data/aapl.obm | --synthetic N] [--seed S] [--repeats R]
//            [--cpu C] [--only name,name] [--out-dir DIR]
//
// Method:
//   * The whole message stream is loaded/generated into memory first, so
//     parsing is never measured.
//   * Each version gets an untimed warm-up pass, then R timed repeats on a
//     freshly constructed book (construction is outside the timed region).
//   * Throughput pass: one clock read around the whole stream.
//   * Latency pass: a timestamp before and after every process() call,
//     recorded into an HDR-style histogram (raw ticks, converted at the end).
//   * The calling thread is pinned to --cpu where the OS supports it.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "msgfile.hpp"
#include "ob/book.hpp"
#include "ob/env_info.hpp"
#include "ob/histogram.hpp"
#include "ob/timer.hpp"
#include "ref_book.hpp"
#include "synthetic.hpp"

using namespace ob;

namespace {

struct Result {
    std::string name;
    double mps_median = 0, mps_min = 0, mps_max = 0;  // million messages / second
    Histogram hist;                                    // ticks
};

volatile std::uint64_t g_sink;

template <class B>
Result run_version(const std::string& name, const MessageStream& s, int repeats) {
    Result r;
    r.name = name;
    const BookConfig cfg{s.band, 1u << 20};
    EventBuffer out(1 << 16);
    std::vector<double> mps;

    {  // warm-up: page in code, data and the book arrays
        B book(cfg);
        for (const Message& m : s.msgs) {
            out.clear();
            book.process(m, out);
        }
    }
    for (int rep = 0; rep < repeats; ++rep) {
        {
            B book(cfg);
            std::uint64_t events = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (const Message& m : s.msgs) {
                out.clear();
                book.process(m, out);
                events += out.size();
            }
            const auto t1 = std::chrono::steady_clock::now();
            g_sink = events;
            const double secs = std::chrono::duration<double>(t1 - t0).count();
            mps.push_back(static_cast<double>(s.msgs.size()) / secs / 1e6);
        }
        {
            B book(cfg);
            for (const Message& m : s.msgs) {
                out.clear();
                const std::uint64_t a = Timer::now();
                book.process(m, out);
                const std::uint64_t b = Timer::now();
                r.hist.record(b - a);
            }
        }
    }
    std::sort(mps.begin(), mps.end());
    r.mps_median = mps[mps.size() / 2];
    r.mps_min = mps.front();
    r.mps_max = mps.back();
    return r;
}

std::string ns(const Histogram& h, double p, double scale) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(0) << static_cast<double>(h.percentile(p)) * scale;
    return os.str();
}

// Cost of a back-to-back pair of timestamp reads, in ticks.
Histogram timer_overhead() {
    Histogram h;
    for (int i = 0; i < 1'000'000; ++i) {
        const auto a = Timer::now();
        const auto b = Timer::now();
        h.record(b - a);
    }
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    std::string stream_path, only, out_dir;
    std::size_t synthetic = 0;
    std::uint64_t seed = 42;
    int repeats = 5, cpu = -1;
    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* n) { return std::strcmp(argv[i], n) == 0 && i + 1 < argc; };
        if (arg("--stream")) stream_path = argv[++i];
        else if (arg("--synthetic")) synthetic = std::strtoull(argv[++i], nullptr, 10);
        else if (arg("--seed")) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (arg("--repeats")) repeats = std::atoi(argv[++i]);
        else if (arg("--cpu")) cpu = std::atoi(argv[++i]);
        else if (arg("--only")) only = argv[++i];
        else if (arg("--out-dir")) out_dir = argv[++i];
        else {
            std::cerr << "unknown option " << argv[i] << "\n";
            return 2;
        }
    }
    if (repeats < 1) repeats = 1;

    MessageStream s;
    std::string source;
    if (!stream_path.empty()) {
        std::string err;
        if (!load_stream(stream_path, s, &err)) {
            std::cerr << err << "\n";
            return 1;
        }
        source = stream_path + " (" + s.symbol + ")";
    } else {
        if (!synthetic) synthetic = 2'000'000;
        SynthConfig sc;
        sc.seed = seed;
        SyntheticFlow gen(sc);
        s.band = sc.band;
        s.symbol = "SYNTH";
        s.msgs = gen.generate(synthetic);
        source = "synthetic, seed " + std::to_string(seed);
    }

    const bool pinned = cpu >= 0 && pin_thread_to_cpu(cpu);
    const EnvInfo env = EnvInfo::capture();
    const double scale = Timer::ns_per_tick();
    const Histogram tov = timer_overhead();

    std::cout << "## Environment\n\n" << env.to_markdown() << "| Pinned | "
              << (cpu < 0 ? "no (--cpu not given)" : pinned ? "cpu " + std::to_string(cpu) : "requested but unsupported on this OS")
              << " |\n| Timer pair overhead | p50 " << static_cast<double>(tov.percentile(50)) * scale << " ns, p99 "
              << static_cast<double>(tov.percentile(99)) * scale << " ns |\n\n";

    std::size_t counts[4] = {};
    for (const auto& m : s.msgs) ++counts[static_cast<int>(m.type)];
    std::cout << "## Workload\n\n" << s.msgs.size() << " messages from " << source << ": " << counts[0] << " add, "
              << counts[1] << " cancel, " << counts[2] << " modify, " << counts[3] << " market. " << repeats
              << " repeats after 1 warm-up.\n\n";

    auto want = [&](const std::string& n) { return only.empty() || ("," + only + ",").find("," + n + ",") != std::string::npos; };
    std::vector<Result> results;
    if (want("baseline")) results.push_back(run_version<RefBook>("Baseline (std::map + std::list)", s, repeats));
    if (want("flat")) results.push_back(run_version<BookFlatLevels>("+ flat levels (intrusive lists, new/delete)", s, repeats));
    if (want("pool")) results.push_back(run_version<BookPool>("+ pool allocator", s, repeats));
    if (want("hash")) results.push_back(run_version<BookFlatHash>("+ flat hash map", s, repeats));
    if (want("cache")) results.push_back(run_version<Book>("+ cache tuning (32B order)", s, repeats));

    std::cout << "## Results\n\n| Version | Throughput (M msg/s, median [min-max]) | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |\n"
              << "|---|---|---|---|---|---|\n";
    for (const auto& r : results) {
        std::cout << "| " << r.name << " | " << std::fixed << std::setprecision(2) << r.mps_median << " ["
                  << r.mps_min << "-" << r.mps_max << "] | " << ns(r.hist, 50, scale) << " | " << ns(r.hist, 99, scale)
                  << " | " << ns(r.hist, 99.9, scale) << " | " << std::setprecision(0)
                  << static_cast<double>(r.hist.max()) * scale << " |\n";
    }
    std::cout << "\nLatency = time inside process() per message, including event emission. Resolution: "
              << scale << " ns/tick.\n";

    if (!out_dir.empty()) {
        std::filesystem::create_directories(out_dir);
        for (const auto& r : results) {
            std::string file = r.name;
            for (char& c : file)
                if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
            std::ofstream f(out_dir + "/" + file + ".hgrm");
            f << "# " << r.name << "\n";
            r.hist.print_percentiles(f, 1.0 / scale);
        }
        std::ofstream summary(out_dir + "/summary.csv");
        summary << "version,mps_median,p50_ns,p99_ns,p999_ns,max_ns\n";
        for (const auto& r : results)
            summary << '"' << r.name << "\"," << r.mps_median << "," << static_cast<double>(r.hist.percentile(50)) * scale << ","
                    << static_cast<double>(r.hist.percentile(99)) * scale << ","
                    << static_cast<double>(r.hist.percentile(99.9)) * scale << "," << static_cast<double>(r.hist.max()) * scale << "\n";
        std::cout << "wrote histograms to " << out_dir << "\n";
    }
    return 0;
}
