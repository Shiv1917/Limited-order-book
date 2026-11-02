// Compact binary container for pre-converted message streams (.obm).
// Layout: header | Message[count] | uint64 timestamp_ns[count]
// Lets benchmarks load real data straight into memory with no parsing cost.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ob/types.hpp"

namespace ob {

struct MessageStream {
    PriceBand band{};
    std::string symbol;
    std::vector<Message> msgs;
    std::vector<std::uint64_t> ts_ns;  // optional, same length as msgs when present
};

bool save_stream(const std::string& path, const MessageStream& s, std::string* err = nullptr);
bool load_stream(const std::string& path, MessageStream& s, std::string* err = nullptr);

}  // namespace ob
