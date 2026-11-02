// Coverage-guided differential fuzzing with libFuzzer (Linux clang):
//   cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DOB_LIBFUZZER=ON -DOB_SANITIZE=address,undefined
//   ./build-fuzz/ob_libfuzzer -max_total_time=300
// Each 8-byte chunk of input decodes to one message on a tiny price band so
// the fuzzer reaches crossing, sweeping and band-edge states quickly.
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "ob/book.hpp"
#include "ref_book.hpp"

using namespace ob;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const BookConfig cfg{PriceBand{100, 2, 32}, 16};
    RefBook ref(cfg);
    Book fast(cfg);
    EventBuffer a, b;
    for (std::size_t off = 0; off + 8 <= size; off += 8) {
        const std::uint8_t* p = data + off;
        const OrderId id = p[1] % 24;  // small id space: duplicates, cancels of filled orders
        const Price px = 96 + p[2] % 72;  // includes off-band and off-tick prices
        const Qty qty = p[3] % 20;        // includes zero
        const Side side = (p[4] & 1) ? Side::Sell : Side::Buy;
        const auto tif = static_cast<TimeInForce>(p[5] % 3);
        Message m;
        switch (p[0] % 4) {
            case 0: m = Message::add(id, side, px, qty, tif); break;
            case 1: m = Message::cancel(id); break;
            case 2: m = Message::modify(id, px, qty); break;
            default: m = Message::market(id, side, qty, tif); break;
        }
        a.clear(), b.clear();
        ref.process(m, a);
        fast.process(m, b);
        if (a.size() != b.size()) std::abort();
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!(a[i] == b[i])) std::abort();
        if (ref.best_bid() != fast.best_bid() || ref.best_ask() != fast.best_ask()) std::abort();
        if (ref.depth(Side::Buy) != fast.depth(Side::Buy) || ref.depth(Side::Sell) != fast.depth(Side::Sell)) std::abort();
    }
    if (!fast.validate()) std::abort();
    return 0;
}
