#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bedrock {

uint64_t xxHash64(const uint8_t* data, std::size_t size, uint64_t seed = 0);

inline uint64_t xxHash64(const std::vector<uint8_t>& data, uint64_t seed = 0) {
    return xxHash64(data.data(), data.size(), seed);
}

} // namespace bedrock
