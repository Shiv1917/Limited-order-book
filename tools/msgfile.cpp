#include "msgfile.hpp"

#include <cstdio>
#include <cstring>
#include <memory>

namespace ob {
namespace {
constexpr char kMagic[8] = {'O', 'B', 'M', 'S', 'G', '0', '1', '\0'};
struct Header {
    char magic[8];
    std::uint64_t count;
    std::int64_t base;
    std::int64_t tick;
    std::uint32_t levels;
    std::uint32_t has_ts;
    char symbol[16];
};
struct FileCloser {
    void operator()(std::FILE* f) const { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;
bool set_err(std::string* err, const std::string& m) {
    if (err) *err = m;
    return false;
}
}  // namespace

bool save_stream(const std::string& path, const MessageStream& s, std::string* err) {
    File f(std::fopen(path.c_str(), "wb"));
    if (!f) return set_err(err, "cannot open " + path);
    Header h{};
    std::memcpy(h.magic, kMagic, 8);
    h.count = s.msgs.size();
    h.base = s.band.base;
    h.tick = s.band.tick;
    h.levels = s.band.levels;
    h.has_ts = s.ts_ns.size() == s.msgs.size() ? 1 : 0;
    std::strncpy(h.symbol, s.symbol.c_str(), sizeof(h.symbol) - 1);
    if (std::fwrite(&h, sizeof h, 1, f.get()) != 1) return set_err(err, "write failed");
    if (!s.msgs.empty() && std::fwrite(s.msgs.data(), sizeof(Message), s.msgs.size(), f.get()) != s.msgs.size())
        return set_err(err, "write failed");
    if (h.has_ts && !s.ts_ns.empty() &&
        std::fwrite(s.ts_ns.data(), sizeof(std::uint64_t), s.ts_ns.size(), f.get()) != s.ts_ns.size())
        return set_err(err, "write failed");
    return true;
}

bool load_stream(const std::string& path, MessageStream& s, std::string* err) {
    File f(std::fopen(path.c_str(), "rb"));
    if (!f) return set_err(err, "cannot open " + path);
    Header h{};
    if (std::fread(&h, sizeof h, 1, f.get()) != 1 || std::memcmp(h.magic, kMagic, 8) != 0)
        return set_err(err, "not an .obm file: " + path);
    s.band = PriceBand{h.base, h.tick, h.levels};
    h.symbol[sizeof(h.symbol) - 1] = '\0';
    s.symbol = h.symbol;
    s.msgs.resize(h.count);
    if (h.count && std::fread(s.msgs.data(), sizeof(Message), h.count, f.get()) != h.count)
        return set_err(err, "truncated file");
    s.ts_ns.clear();
    if (h.has_ts) {
        s.ts_ns.resize(h.count);
        if (h.count && std::fread(s.ts_ns.data(), sizeof(std::uint64_t), h.count, f.get()) != h.count)
            return set_err(err, "truncated timestamps");
    }
    return true;
}

}  // namespace ob
