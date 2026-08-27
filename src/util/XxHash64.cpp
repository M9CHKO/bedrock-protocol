#include <bedrock/util/XxHash64.hpp>

#include <bit>
#include <cstring>

namespace bedrock {
namespace {

constexpr uint64_t Prime1 = 11400714785074694791ull;
constexpr uint64_t Prime2 = 14029467366897019727ull;
constexpr uint64_t Prime3 = 1609587929392839161ull;
constexpr uint64_t Prime4 = 9650029242287828579ull;
constexpr uint64_t Prime5 = 2870177450012600261ull;

uint32_t readU32LE(const uint8_t* data) {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    if constexpr (std::endian::native == std::endian::big) {
        value = ((value & 0x000000ffu) << 24u) |
            ((value & 0x0000ff00u) << 8u) |
            ((value & 0x00ff0000u) >> 8u) |
            ((value & 0xff000000u) >> 24u);
    }
    return value;
}

uint64_t readU64LE(const uint8_t* data) {
    uint64_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    if constexpr (std::endian::native == std::endian::big) {
        value = ((value & 0x00000000000000ffull) << 56u) |
            ((value & 0x000000000000ff00ull) << 40u) |
            ((value & 0x0000000000ff0000ull) << 24u) |
            ((value & 0x00000000ff000000ull) << 8u) |
            ((value & 0x000000ff00000000ull) >> 8u) |
            ((value & 0x0000ff0000000000ull) >> 24u) |
            ((value & 0x00ff000000000000ull) >> 40u) |
            ((value & 0xff00000000000000ull) >> 56u);
    }
    return value;
}

uint64_t round(uint64_t accumulator, uint64_t input) {
    accumulator += input * Prime2;
    accumulator = std::rotl(accumulator, 31);
    accumulator *= Prime1;
    return accumulator;
}

uint64_t mergeRound(uint64_t accumulator, uint64_t value) {
    accumulator ^= round(0, value);
    accumulator = accumulator * Prime1 + Prime4;
    return accumulator;
}

} // namespace

uint64_t xxHash64(const uint8_t* data, std::size_t size, uint64_t seed) {
    static constexpr uint8_t EmptyInput = 0;
    if (data == nullptr) {
        if (size != 0) {
            return 0;
        }
        data = &EmptyInput;
    }

    const uint8_t* cursor = data;
    const uint8_t* const end = data + size;
    uint64_t hash = 0;

    if (size >= 32) {
        uint64_t v1 = seed + Prime1 + Prime2;
        uint64_t v2 = seed + Prime2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - Prime1;
        const uint8_t* const limit = end - 32;
        do {
            v1 = round(v1, readU64LE(cursor));
            cursor += 8;
            v2 = round(v2, readU64LE(cursor));
            cursor += 8;
            v3 = round(v3, readU64LE(cursor));
            cursor += 8;
            v4 = round(v4, readU64LE(cursor));
            cursor += 8;
        } while (cursor <= limit);

        hash = std::rotl(v1, 1) + std::rotl(v2, 7) +
            std::rotl(v3, 12) + std::rotl(v4, 18);
        hash = mergeRound(hash, v1);
        hash = mergeRound(hash, v2);
        hash = mergeRound(hash, v3);
        hash = mergeRound(hash, v4);
    } else {
        hash = seed + Prime5;
    }

    hash += size;
    while (cursor + 8 <= end) {
        const uint64_t lane = round(0, readU64LE(cursor));
        hash ^= lane;
        hash = std::rotl(hash, 27) * Prime1 + Prime4;
        cursor += 8;
    }
    if (cursor + 4 <= end) {
        hash ^= static_cast<uint64_t>(readU32LE(cursor)) * Prime1;
        hash = std::rotl(hash, 23) * Prime2 + Prime3;
        cursor += 4;
    }
    while (cursor < end) {
        hash ^= static_cast<uint64_t>(*cursor) * Prime5;
        hash = std::rotl(hash, 11) * Prime1;
        ++cursor;
    }

    hash ^= hash >> 33u;
    hash *= Prime2;
    hash ^= hash >> 29u;
    hash *= Prime3;
    hash ^= hash >> 32u;
    return hash;
}

} // namespace bedrock
