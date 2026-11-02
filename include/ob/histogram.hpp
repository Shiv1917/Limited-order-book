// HDR-style latency histogram (log-linear buckets, constant relative error).
//
// Same bucketing scheme as Gil Tene's HdrHistogram: values are grouped into
// power-of-two "buckets", each split into 2^(sub_bits-1) linear sub-buckets, so
// every recorded value is reproduced within a relative error of 2^-(sub_bits-1)
// (sub_bits = 11 -> < 0.1%, i.e. 3 significant digits). Recording is a
// clz, a shift and an increment: cheap enough to call per message.
// A small self-contained implementation keeps the build dependency-free; the
// percentile output format matches HdrHistogram's so its plotters can be used.
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <ostream>
#include <vector>

namespace ob {

class Histogram {
public:
    explicit Histogram(unsigned sub_bits = 11, unsigned max_bits = 40)
        : sub_bits_(sub_bits), counts_(static_cast<std::size_t>(max_bits - sub_bits + 1) << sub_bits, 0) {}

    void record(std::uint64_t v) noexcept {
        std::size_t i = index_of(v);
        if (i >= counts_.size()) i = counts_.size() - 1;
        ++counts_[i];
        ++total_;
        max_ = std::max(max_, v);
        min_ = std::min(min_, v);
        sum_ += static_cast<double>(v);
    }

    void merge(const Histogram& o) {
        for (std::size_t i = 0; i < counts_.size() && i < o.counts_.size(); ++i) counts_[i] += o.counts_[i];
        total_ += o.total_;
        max_ = std::max(max_, o.max_);
        min_ = std::min(min_, o.min_);
        sum_ += o.sum_;
    }

    void reset() {
        std::fill(counts_.begin(), counts_.end(), 0);
        total_ = 0;
        max_ = 0;
        min_ = UINT64_MAX;
        sum_ = 0;
    }

    std::uint64_t count() const noexcept { return total_; }
    std::uint64_t max() const noexcept { return max_; }
    std::uint64_t min() const noexcept { return total_ ? min_ : 0; }
    double mean() const noexcept { return total_ ? sum_ / static_cast<double>(total_) : 0.0; }

    // Value at percentile p in [0, 100] (upper edge of the containing bucket,
    // clamped to the observed max, like HdrHistogram).
    std::uint64_t percentile(double p) const noexcept {
        if (total_ == 0) return 0;
        const auto target = static_cast<std::uint64_t>(std::ceil(p / 100.0 * static_cast<double>(total_)));
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= std::max<std::uint64_t>(target, 1)) return std::min(highest_equivalent(i), max_);
        }
        return max_;
    }

    // HdrHistogram-style percentile distribution (value, percentile, count, 1/(1-p)).
    void print_percentiles(std::ostream& os, double scale = 1.0, int ticks_per_half = 5) const {
        os << "       Value     Percentile TotalCount 1/(1-Percentile)\n\n";
        if (total_ == 0) return;
        double pct = 0.0;
        for (;;) {
            const std::uint64_t v = percentile(pct);
            std::uint64_t cnt = 0;
            for (std::size_t i = 0; i < counts_.size() && lowest_equivalent(i) <= v; ++i) cnt += counts_[i];
            os.setf(std::ios::fixed);
            os.precision(3);
            os.width(12);
            os << static_cast<double>(v) / scale << " ";
            os.precision(12);
            os.width(14);
            os << pct / 100.0 << " ";
            os.width(10);
            os << cnt << " ";
            if (pct < 100.0) {
                os.precision(2);
                os.width(14);
                os << 1.0 / (1.0 - pct / 100.0);
            }
            os << "\n";
            if (pct >= 100.0 || cnt >= total_) break;
            // Halve the remaining distance to 100% every `ticks_per_half` steps.
            const double remaining = 100.0 - pct;
            const double step = remaining / 2.0 / ticks_per_half;
            pct += std::max(step, 1e-7);
            if (100.0 - pct < 1e-6) pct = 100.0;
        }
        os << "#[Mean = " << mean() / scale << ", Max = " << static_cast<double>(max_) / scale
           << ", Total count = " << total_ << "]\n";
    }

private:
    // Values below 2^sub_bits are stored exactly. Above that, a value with most
    // significant bit m lands in region k = m - sub_bits + 1 and keeps its top
    // sub_bits bits: index = k*half + (v >> k), half = 2^(sub_bits-1).
    std::size_t index_of(std::uint64_t v) const noexcept {
        const unsigned msb = 63u - static_cast<unsigned>(std::countl_zero(v | 1));
        if (msb < sub_bits_) return static_cast<std::size_t>(v);
        const unsigned k = msb - sub_bits_ + 1;
        return (static_cast<std::size_t>(k) << (sub_bits_ - 1)) + static_cast<std::size_t>(v >> k);
    }
    std::uint64_t lowest_equivalent(std::size_t i) const noexcept {
        const std::size_t half = std::size_t{1} << (sub_bits_ - 1);
        if (i < 2 * half) return i;
        const std::size_t k = i / half - 1;
        return static_cast<std::uint64_t>(i - k * half) << k;
    }
    std::uint64_t highest_equivalent(std::size_t i) const noexcept {
        const std::size_t half = std::size_t{1} << (sub_bits_ - 1);
        if (i < 2 * half) return i;
        const std::size_t k = i / half - 1;
        return lowest_equivalent(i) + (std::uint64_t{1} << k) - 1;
    }

    unsigned sub_bits_;
    std::vector<std::uint64_t> counts_;
    std::uint64_t total_ = 0;
    std::uint64_t max_ = 0;
    std::uint64_t min_ = UINT64_MAX;
    double sum_ = 0;
};

}  // namespace ob
