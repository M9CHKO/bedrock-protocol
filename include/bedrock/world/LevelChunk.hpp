#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

struct LevelChunkHeader {
    int32_t chunkX = 0;
    int32_t chunkZ = 0;
    int32_t dimension = 0;
    int32_t subChunkCount = 0;
    std::optional<uint16_t> highestSubChunkCount;
    bool cacheEnabled = false;
    std::vector<uint64_t> blobHashes;
    uint32_t dataSize = 0;
    std::vector<uint8_t> data;
    std::size_t headerBytes = 0;
};

class LevelChunkCursor {
public:
    explicit LevelChunkCursor(const std::vector<uint8_t>& data)
        : data_(data) {}

    std::size_t offset() const { return offset_; }
    std::size_t remaining() const { return offset_ <= data_.size() ? data_.size() - offset_ : 0; }
    bool eof() const { return offset_ >= data_.size(); }

    uint8_t u8() {
        require(1, "u8");
        return data_[offset_++];
    }

    bool boolean() {
        return u8() != 0;
    }

    uint16_t u16le() {
        require(2, "u16le");
        const uint16_t value =
            static_cast<uint16_t>(data_[offset_]) |
            static_cast<uint16_t>(static_cast<uint16_t>(data_[offset_ + 1]) << 8u);
        offset_ += 2;
        return value;
    }

    uint64_t u64le() {
        require(8, "u64le");

        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(data_[offset_ + i]) << (8 * i);
        }

        offset_ += 8;
        return value;
    }

    uint32_t uvarint() {
        uint32_t value = 0;

        for (int shift = 0; shift <= 28; shift += 7) {
            uint8_t byte = u8();
            value |= static_cast<uint32_t>(byte & 0x7f) << shift;

            if ((byte & 0x80) == 0) {
                return value;
            }
        }

        throw std::runtime_error("level_chunk uvarint too long");
    }

    int32_t svarint() {
        uint32_t raw = uvarint();
        return static_cast<int32_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }

    std::vector<uint8_t> bytes(std::size_t size) {
        require(size, "bytes");

        std::vector<uint8_t> out(
            data_.begin() + static_cast<std::ptrdiff_t>(offset_),
            data_.begin() + static_cast<std::ptrdiff_t>(offset_ + size)
        );

        offset_ += size;
        return out;
    }

private:
    const std::vector<uint8_t>& data_;
    std::size_t offset_ = 0;

    void require(std::size_t size, const char* what) const {
        if (offset_ + size > data_.size()) {
            throw std::runtime_error(std::string("not enough bytes for level_chunk ") + what);
        }
    }
};

class LevelChunkParser {
public:
    static LevelChunkHeader readHeader(const std::vector<uint8_t>& payload) {
        LevelChunkCursor cursor(payload);

        LevelChunkHeader out;
        out.chunkX = cursor.svarint();
        out.chunkZ = cursor.svarint();
        out.dimension = cursor.svarint();
        out.subChunkCount = static_cast<int32_t>(cursor.uvarint());
        if (out.subChunkCount == -2) {
            out.highestSubChunkCount = cursor.u16le();
        }
        out.cacheEnabled = cursor.boolean();

        if (out.cacheEnabled) {
            uint32_t blobCount = cursor.uvarint();
            out.blobHashes.reserve(blobCount);

            for (uint32_t i = 0; i < blobCount; ++i) {
                out.blobHashes.push_back(cursor.u64le());
            }
        }

        out.dataSize = cursor.uvarint();

        if (out.dataSize > cursor.remaining()) {
            throw std::runtime_error("level_chunk dataSize is larger than remaining payload");
        }

        out.data = cursor.bytes(out.dataSize);
        out.headerBytes = cursor.offset() - out.data.size();

        return out;
    }
};

inline void writeLevelChunkUVarInt(std::vector<uint8_t>& out, uint32_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
    }

    out.push_back(static_cast<uint8_t>(value));
}

inline void writeLevelChunkSVarInt(std::vector<uint8_t>& out, int32_t value) {
    uint32_t raw =
        (static_cast<uint32_t>(value) << 1) ^
        static_cast<uint32_t>(value >> 31);

    writeLevelChunkUVarInt(out, raw);
}

inline void writeLevelChunkU64LE(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xff));
    }
}

} // namespace bedrock
