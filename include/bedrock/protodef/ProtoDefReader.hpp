#pragma once

#include <bedrock/debug/PacketFieldDecoder.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace bedrock {

class ProtoDefReader {
public:
    explicit ProtoDefReader(PacketFieldCursor& cursor)
        : cursor_(cursor) {}

    std::size_t offset() const {
        return cursor_.offset();
    }

    std::size_t remaining() const {
        return cursor_.remaining();
    }

    const std::vector<uint8_t>& data() const {
        return cursor_.data();
    }

    uint8_t u8() {
        if (remaining() < 1) {
            throw std::runtime_error("not enough bytes for u8 at offset " + std::to_string(offset()));
        }
        return cursor_.u8();
    }

    bool tryU8(uint8_t& out) {
        if (remaining() < 1) return false;
        out = u8();
        return true;
    }

    uint16_t u16le() {
        return cursor_.u16le();
    }

    uint16_t u16be() {
        uint16_t hi = u8();
        uint16_t lo = u8();
        return static_cast<uint16_t>((hi << 8) | lo);
    }

    uint32_t u32le() {
        return cursor_.u32le();
    }

    uint32_t u32be() {
        uint32_t b0 = u8();
        uint32_t b1 = u8();
        uint32_t b2 = u8();
        uint32_t b3 = u8();
        return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
    }

    uint64_t u64le() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(u8()) << (i * 8);
        }
        return v;
    }

    uint64_t u64be() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = (v << 8) | static_cast<uint64_t>(u8());
        }
        return v;
    }

    int64_t i64le() {
        return static_cast<int64_t>(u64le());
    }

    int64_t i64be() {
        return static_cast<int64_t>(u64be());
    }

    uint64_t readU64LE() {
        return u64le();
    }

    uint64_t readU64BE() {
        return u64be();
    }

    float readF32LE() {
        uint32_t raw = u32le();
        float v = 0.0f;
        std::memcpy(&v, &raw, sizeof(v));
        return v;
    }

    float readF32BE() {
        uint32_t raw = u32be();
        float v = 0.0f;
        std::memcpy(&v, &raw, sizeof(v));
        return v;
    }

    double readF64LE() {
        uint64_t raw = u64le();
        double v = 0.0;
        std::memcpy(&v, &raw, sizeof(v));
        return v;
    }

    double readF64BE() {
        uint64_t raw = u64be();
        double v = 0.0;
        std::memcpy(&v, &raw, sizeof(v));
        return v;
    }

    int32_t i32le() {
        return cursor_.i32le();
    }

    int32_t i32be() {
        return static_cast<int32_t>(u32be());
    }

    uint32_t varuint32() {
        return cursor_.varuint32();
    }

    uint64_t varuint64() {
        return cursor_.varuint64();
    }

    int64_t varint64() {
        return static_cast<int64_t>(cursor_.varuint64());
    }

    int32_t zigzag32() {
        return cursor_.zigzag32();
    }

    int64_t zigzag64() {
        return cursor_.zigzag64();
    }

    bool boolean() {
        return cursor_.boolean();
    }

    std::string string() {
        return cursor_.string();
    }

    void skip(std::size_t n) {
        cursor_.skip(n);
    }

    std::vector<uint8_t> readBytes(std::size_t count) {
        if (count > remaining()) {
            throw std::runtime_error(
                "not enough bytes at offset " + std::to_string(offset()) +
                " remaining=" + std::to_string(remaining()) +
                " need=" + std::to_string(count)
            );
        }
        std::vector<uint8_t> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(cursor_.u8());
        }
        return out;
    }

    void rewindTo(std::size_t offset) {
        cursor_.rewindTo(offset);
    }

    std::vector<uint8_t> peekRemainingBytes() {
        const auto before = offset();
        std::vector<uint8_t> out;
        out.reserve(remaining());
        while (remaining() > 0) {
            out.push_back(u8());
        }
        rewindTo(before);
        return out;
    }

private:
    PacketFieldCursor& cursor_;
};

}
