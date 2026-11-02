// Minimal SHA-256 (FIPS 180-4) for output/input file fingerprints.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sim {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t len);
    std::string hex();  // finalizes

private:
    void block(const std::uint8_t* p);
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t n_ = 0;
    std::uint64_t bits_ = 0;
};

std::string sha256_hex(const std::string& data);
std::string sha256_file(const std::string& path);  // "" if unreadable

}  // namespace sim
