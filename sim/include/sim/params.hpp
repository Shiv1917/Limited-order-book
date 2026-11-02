// Helpers for reading strategy parameters from JSON.
#pragma once

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include "sim/time.hpp"

namespace sim {

using json = nlohmann::json;

// Seconds-after-midnight string with up to 9 decimals ("34200.5"), parsed
// exactly, or an integer number of nanoseconds.
inline Time parse_time(const json& v) {
    if (v.is_number_integer()) return v.get<Time>();
    if (!v.is_string()) throw std::runtime_error("time must be an integer (ns) or a \"seconds.fraction\" string");
    const std::string s = v.get<std::string>();
    Time sec = 0, frac = 0;
    int digits = 0;
    std::size_t i = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) sec = sec * 10 + (s[i] - '0');
    if (i < s.size() && s[i] == '.')
        for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i)
            if (digits < 9) frac = frac * 10 + (s[i] - '0'), ++digits;
    if (i != s.size() || s.empty()) throw std::runtime_error("bad time string: " + s);
    while (digits < 9) frac *= 10, ++digits;
    return sec * kSecond + frac;
}

inline Time time_or(const json& j, const char* key, Time dflt) {
    return j.contains(key) ? parse_time(j.at(key)) : dflt;
}

inline Side parse_side(const json& v) {
    const std::string s = v.get<std::string>();
    if (s == "buy" || s == "B") return Side::Buy;
    if (s == "sell" || s == "S") return Side::Sell;
    throw std::runtime_error("side must be buy or sell");
}

}  // namespace sim
