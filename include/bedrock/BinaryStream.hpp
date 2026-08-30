#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <stdexcept>

namespace bedrock {

class BinaryStreamError : public std::runtime_error {
public:
    explicit BinaryStreamError(const std::string& msg)
        : std::runtime_error(msg) {}
};

class BinaryStream {
public:
    BinaryStream();
    explicit BinaryStream(const std::vector<uint8_t>& data);
    explicit BinaryStream(std::vector<uint8_t>&& data);

    // Read directly from an existing buffer without copying it. The source
    // must outlive the returned stream. Any later mutable buffer access or
    // write materializes an owned copy first.
    static BinaryStream view(const std::vector<uint8_t>& data, size_t offset = 0);

    const std::vector<uint8_t>& buffer() const;
    std::vector<uint8_t>& buffer();

    size_t offset() const;
    size_t remaining() const;
    bool eof() const;

    void reset();
    void seek(size_t pos);

    uint8_t readU8();
    int8_t readI8();

    uint16_t readU16LE();
    int16_t readI16LE();

    uint32_t readU32LE();
    int32_t readI32LE();

    uint64_t readU64LE();
    int64_t readI64LE();

    float readFloatLE();
    double readDoubleLE();

    uint32_t readVarUInt();
    int32_t readVarInt();

    uint64_t readVarULong();
    int64_t readVarLong();

    std::string readString();

    std::vector<uint8_t> readBytes(size_t len);

    void writeU8(uint8_t v);
    void writeI8(int8_t v);

    void writeU16LE(uint16_t v);
    void writeI16LE(int16_t v);

    void writeU32LE(uint32_t v);
    void writeI32LE(int32_t v);

    void writeU64LE(uint64_t v);
    void writeI64LE(int64_t v);

    void writeFloatLE(float v);
    void writeDoubleLE(double v);

    void writeVarUInt(uint32_t v);
    void writeVarInt(int32_t v);

    void writeVarULong(uint64_t v);
    void writeVarLong(int64_t v);

    void writeString(const std::string& s);

    void writeBytes(const std::vector<uint8_t>& bytes);
    void writeBytes(const uint8_t* data, size_t len);

private:
    struct ViewTag {};

    BinaryStream(const std::vector<uint8_t>& data, size_t offset, ViewTag);

    std::vector<uint8_t> data_;
    const std::vector<uint8_t>* view_ = nullptr;
    size_t offset_ = 0;

    const std::vector<uint8_t>& readableBuffer() const;
    std::vector<uint8_t>& writableBuffer();
    void require(size_t len) const;
};

} // namespace bedrock
