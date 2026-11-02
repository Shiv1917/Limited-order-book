// ob_e2e_bench: producer thread -> lock-free SPSC ring -> matching-engine thread.
//
//   ob_e2e_bench [--stream F.obm | --synthetic N] [--rate MSGS_PER_SEC] [--ring 65536]
//                [--producer-cpu A] [--consumer-cpu B]
//
// The producer stamps each message with Timer::now() when it is published;
// the consumer records (time after process() returns) - (publish time). That
// end-to-end latency includes the cross-core hand-off, any queueing in the
// ring and the matching itself. Two modes are always run:
//   * paced  : producer publishes at --rate (default 1M msg/s), so the
//              number reflects hand-off + matching with an idle queue;
//   * burst  : producer publishes as fast as it can; latency then mostly
//              measures queueing, and throughput is the pipeline maximum.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <thread>

#include "msgfile.hpp"
#include "ob/book.hpp"
#include "ob/env_info.hpp"
#include "ob/histogram.hpp"
#include "ob/spsc.hpp"
#include "ob/timer.hpp"
#include "synthetic.hpp"

using namespace ob;

namespace {

struct Envelope {
    Message msg;
    std::uint64_t t_pub;
};
static_assert(sizeof(Envelope) == 32);

struct RunResult {
    Histogram e2e;
    Histogram match;
    double secs = 0;
};

RunResult run(const MessageStream& s, double rate, std::size_t ring_size, int pcpu, int ccpu) {
    RunResult r;
    SpscRing<Envelope> ring(ring_size);
    std::atomic<bool> consumer_ready{false};
    const double tick_ns = Timer::ns_per_tick();
    const std::uint64_t gap_ticks = rate > 0 ? static_cast<std::uint64_t>(1e9 / rate / tick_ns) : 0;

    std::thread consumer([&] {
        if (ccpu >= 0) pin_thread_to_cpu(ccpu);
        Book book(BookConfig{s.band, 1u << 20});
        EventBuffer out(1 << 16);
        consumer_ready.store(true, std::memory_order_release);
        Envelope e;
        for (std::size_t n = 0; n < s.msgs.size();) {
            if (!ring.try_pop(e)) continue;  // busy-poll: lowest latency, burns a core
            out.clear();
            const std::uint64_t a = Timer::now();
            book.process(e.msg, out);
            const std::uint64_t b = Timer::now();
            r.match.record(b - a);
            r.e2e.record(b - e.t_pub);
            ++n;
        }
    });

    if (pcpu >= 0) pin_thread_to_cpu(pcpu);
    while (!consumer_ready.load(std::memory_order_acquire)) {
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t next = Timer::now();
    for (const Message& m : s.msgs) {
        if (gap_ticks) {
            while (Timer::now() < next) {
            }
            next += gap_ticks;
        }
        Envelope e{m, Timer::now()};
        while (!ring.try_push(e)) {
        }  // spin until there is room; latency keeps the original publish stamp
    }
    consumer.join();
    r.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

void report(const char* label, const RunResult& r, std::size_t n) {
    const double sc = Timer::ns_per_tick();
    auto p = [&](const Histogram& h, double q) { return static_cast<double>(h.percentile(q)) * sc; };
    std::cout << "| " << label << " | " << std::fixed << std::setprecision(2) << static_cast<double>(n) / r.secs / 1e6
              << " | " << std::setprecision(0) << p(r.e2e, 50) << " | " << p(r.e2e, 99) << " | " << p(r.e2e, 99.9)
              << " | " << p(r.match, 50) << " | " << p(r.match, 99) << " |\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string stream_path;
    std::size_t synthetic = 1'000'000, ring = 1 << 16;
    double rate = 1e6;
    int pcpu = -1, ccpu = -1;
    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* n) { return std::strcmp(argv[i], n) == 0 && i + 1 < argc; };
        if (arg("--stream")) stream_path = argv[++i];
        else if (arg("--synthetic")) synthetic = std::strtoull(argv[++i], nullptr, 10);
        else if (arg("--rate")) rate = std::atof(argv[++i]);
        else if (arg("--ring")) ring = std::strtoull(argv[++i], nullptr, 10);
        else if (arg("--producer-cpu")) pcpu = std::atoi(argv[++i]);
        else if (arg("--consumer-cpu")) ccpu = std::atoi(argv[++i]);
        else {
            std::cerr << "unknown option " << argv[i] << "\n";
            return 2;
        }
    }
    MessageStream s;
    if (!stream_path.empty()) {
        std::string err;
        if (!load_stream(stream_path, s, &err)) {
            std::cerr << err << "\n";
            return 1;
        }
    } else {
        SynthConfig sc;
        SyntheticFlow gen(sc);
        s.band = sc.band;
        s.msgs = gen.generate(synthetic);
    }
    std::cout << EnvInfo::capture().to_markdown() << "\n"
              << s.msgs.size() << " messages, ring capacity " << ring << ", producer cpu " << pcpu << ", consumer cpu "
              << ccpu << "\n\n"
              << "| Mode | Throughput (M msg/s) | e2e p50 (ns) | e2e p99 (ns) | e2e p99.9 (ns) | match p50 (ns) | match p99 (ns) |\n"
              << "|---|---|---|---|---|---|---|\n";
    run(s, 0, ring, pcpu, ccpu);  // warm-up
    const std::string paced = "paced @ " + std::to_string(static_cast<long>(rate)) + " msg/s";
    report(paced.c_str(), run(s, rate, ring, pcpu, ccpu), s.msgs.size());
    report("burst (unpaced)", run(s, 0, ring, pcpu, ccpu), s.msgs.size());
    return 0;
}
