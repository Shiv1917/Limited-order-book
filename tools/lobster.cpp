#include "lobster.hpp"

#include <cstdlib>
#include <fstream>

namespace ob::lobster {

bool parse_row(const char* line, Row& r) {
    char* end;
    r.time = std::strtod(line, &end);
    if (end == line || *end != ',') return false;
    const char* p = end + 1;
    r.type = static_cast<int>(std::strtol(p, &end, 10));
    if (end == p || *end != ',') return false;
    p = end + 1;
    r.id = std::strtoull(p, &end, 10);
    if (end == p || *end != ',') return false;
    p = end + 1;
    r.size = static_cast<std::uint32_t>(std::strtoul(p, &end, 10));
    if (end == p || *end != ',') return false;
    p = end + 1;
    r.price = std::strtoll(p, &end, 10);
    if (end == p || *end != ',') return false;
    p = end + 1;
    r.direction = static_cast<int>(std::strtol(p, &end, 10));
    if (end == p) return false;
    return r.type >= 1 && r.type <= 7 && (r.direction == 1 || r.direction == -1);
}

bool parse_snapshot(const char* line, Snapshot& s) {
    s.v.clear();
    const char* p = line;
    char* end;
    for (;;) {
        const long long x = std::strtoll(p, &end, 10);
        if (end == p) break;
        s.v.push_back(x);
        if (*end != ',') break;
        p = end + 1;
    }
    return !s.v.empty() && s.v.size() % 4 == 0;
}

std::vector<Row> load_messages(const std::string& path, std::string* err) {
    std::vector<Row> rows;
    std::ifstream f(path);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return rows;
    }
    std::string line;
    std::size_t n = 0;
    while (std::getline(f, line)) {
        ++n;
        if (line.empty()) continue;
        Row r;
        if (!parse_row(line.c_str(), r)) {
            if (err) *err = path + ":" + std::to_string(n) + ": malformed row";
            rows.clear();
            return rows;
        }
        rows.push_back(r);
    }
    return rows;
}

std::vector<Snapshot> load_snapshots(const std::string& path, std::string* err) {
    std::vector<Snapshot> snaps;
    std::ifstream f(path);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return snaps;
    }
    std::string line;
    std::size_t n = 0;
    while (std::getline(f, line)) {
        ++n;
        if (line.empty()) continue;
        Snapshot s;
        if (!parse_snapshot(line.c_str(), s)) {
            if (err) *err = path + ":" + std::to_string(n) + ": malformed snapshot";
            snaps.clear();
            return snaps;
        }
        snaps.push_back(std::move(s));
    }
    return snaps;
}

}  // namespace ob::lobster
