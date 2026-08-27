#include "bedrock/BinaryStream.hpp"

#include <cstring>
#include <limits>
#include <utility>

namespace bedrock {

BinaryStream::BinaryStream() = default;

BinaryStream::BinaryStream(const std::vector<uint8_t>& data)
    : data_(data), offset_(0) {}

BinaryStream::BinaryStream(std::vector<uint8_t>&& data)
    : data_(std::move(data)), offset_(0) {}

const std::vector<uint8_t>& BinaryStream::buffer() const {
    return data_;
}

std::vector<uint8_t>& BinaryStream::buffer() {
    return data_;
}

size_t BinaryStream::offset() const {
    return offset_;
}

size_t BinaryStream::remaining() const {
    if (offset_ > data_.size()) return 0;
    return data_.size() - offset_;
}

bool BinaryStream::eof() const {
    return offset_ >= data_.size();
}

void BinaryStream::reset() {
    offset_ = 0;
}

void BinaryStream::seek(size_t pos) {
    if (pos > data_.size()) {
        throw BinaryStreamError("seek out of range");
    }
    offset_ = pos;
}

void BinaryStream::require(size_t len) const {
    if (remaining() < len) {
        throw BinaryStreamError("not enough bytes in BinaryStream");
    }
}

uint8_t BinaryStream::readU8() {
    require(1);
    return data_[offset_++];
}

int8_t BinaryStream::readI8() {
    return static_cast<int8_t>(readU8());
}

uint16_t BinaryStream::readU16LE() {
    require(2);

    uint16_t v =
        static_cast<uint16_t>(data_[offset_]) |
        static_cast<uint16_t>(static_cast<uint16_t>(data_[offset_ + 1]) << 8);

    offset_ += 2;
    return v;
}

int16_t BinaryStream::readI16LE() {
    return static_cast<int16_t>(readU16LE());
}

uint32_t BinaryStream::readU32LE() {
    require(4);

    uint32_t v =
        static_cast<uint32_t>(data_[offset_]) |
        (static_cast<uint32_t>(data_[offset_ + 1]) << 8) |
        (static_cast<uint32_t>(data_[offset_ + 2]) << 16) |
        (static_cast<uint32_t>(data_[offset_ + 3]) << 24);

    offset_ += 4;
    return v;
}

int32_t BinaryStream::readI32LE() {
    return static_cast<int32_t>(readU32LE());
}

uint64_t BinaryStream::readU64LE() {
    require(8);

    uint64_t v = 0;

    for (int i = 0; i < 8; i++) {
        v |= static_cast<uint64_t>(data_[offset_ + i]) << (8 * i);
    }

    offset_ += 8;
    return v;
}

int64_t BinaryStream::readI64LE() {
    return static_cast<int64_t>(readU64LE());
}

float BinaryStream::readFloatLE() {
    uint32_t raw = readU32LE();

    float v;
    static_assert(sizeof(float) == sizeof(uint32_t), "float size mismatch");
    std::memcpy(&v, &raw, sizeof(float));

    return v;
}

double BinaryStream::readDoubleLE() {
    uint64_t raw = readU64LE();

    double v;
    static_assert(sizeof(double) == sizeof(uint64_t), "double size mismatch");
    std::memcpy(&v, &raw, sizeof(double));

    return v;
}

uint32_t BinaryStream::readVarUInt() {
    uint32_t result = 0;

    for (int shift = 0; shift <= 28; shift += 7) {
        uint8_t byte = readU8();

        result |= static_cast<uint32_t>(byte & 0x7F) << shift;

        if ((byte & 0x80) == 0) {
            return result;
        }
    }

    throw BinaryStreamError("VarUInt too big");
}

int32_t BinaryStream::readVarInt() {
    return static_cast<int32_t>(readVarUInt());
}

uint64_t BinaryStream::readVarULong() {
    uint64_t result = 0;

    for (int shift = 0; shift <= 63; shift += 7) {
        uint8_t byte = readU8();

        result |= static_cast<uint64_t>(byte & 0x7F) << shift;

        if ((byte & 0x80) == 0) {
            return result;
        }
    }

    throw BinaryStreamError("VarULong too big");
}

int64_t BinaryStream::readVarLong() {
    return static_cast<int64_t>(readVarULong());
}

std::string BinaryStream::readString() {
    uint32_t len = readVarUInt();

    if (len > remaining()) {
        throw BinaryStreamError("string length exceeds remaining buffer");
    }

    if (len == 0) {
        return {};
    }

    std::string s(reinterpret_cast<const char*>(&data_[offset_]), len);
    offset_ += len;

    return s;
}

std::vector<uint8_t> BinaryStream::readBytes(size_t len) {
    require(len);

    std::vector<uint8_t> out(
        data_.begin() + static_cast<std::ptrdiff_t>(offset_),
        data_.begin() + static_cast<std::ptrdiff_t>(offset_ + len)
    );

    offset_ += len;
    return out;
}

void BinaryStream::writeU8(uint8_t v) {
    data_.push_back(v);
}

void BinaryStream::writeI8(int8_t v) {
    writeU8(static_cast<uint8_t>(v));
}

void BinaryStream::writeU16LE(uint16_t v) {
    data_.push_back(static_cast<uint8_t>(v & 0xFF));
    data_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void BinaryStream::writeI16LE(int16_t v) {
    writeU16LE(static_cast<uint16_t>(v));
}

void BinaryStream::writeU32LE(uint32_t v) {
    data_.push_back(static_cast<uint8_t>(v & 0xFF));
    data_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    data_.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    data_.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void BinaryStream::writeI32LE(int32_t v) {
    writeU32LE(static_cast<uint32_t>(v));
}

void BinaryStream::writeU64LE(uint64_t v) {
    for (int i = 0; i < 8; i++) {
        data_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void BinaryStream::writeI64LE(int64_t v) {
    writeU64LE(static_cast<uint64_t>(v));
}

void BinaryStream::writeFloatLE(float v) {
    uint32_t raw;
    static_assert(sizeof(float) == sizeof(uint32_t), "float size mismatch");
    std::memcpy(&raw, &v, sizeof(float));
    writeU32LE(raw);
}

void BinaryStream::writeDoubleLE(double v) {
    uint64_t raw;
    static_assert(sizeof(double) == sizeof(uint64_t), "double size mismatch");
    std::memcpy(&raw, &v, sizeof(double));
    writeU64LE(raw);
}

void BinaryStream::writeVarUInt(uint32_t v) {
    while (true) {
        if ((v & ~0x7Fu) == 0) {
            writeU8(static_cast<uint8_t>(v));
            return;
        }

        writeU8(static_cast<uint8_t>((v & 0x7F) | 0x80));
        v >>= 7;
    }
}

void BinaryStream::writeVarInt(int32_t v) {
    writeVarUInt(static_cast<uint32_t>(v));
}

void BinaryStream::writeVarULong(uint64_t v) {
    while (true) {
        if ((v & ~0x7Full) == 0) {
            writeU8(static_cast<uint8_t>(v));
            return;
        }

        writeU8(static_cast<uint8_t>((v & 0x7F) | 0x80));
        v >>= 7;
    }
}

void BinaryStream::writeVarLong(int64_t v) {
    writeVarULong(static_cast<uint64_t>(v));
}

void BinaryStream::writeString(const std::string& s) {
    if (s.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        throw BinaryStreamError("string too large");
    }

    writeVarUInt(static_cast<uint32_t>(s.size()));
    writeBytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

void BinaryStream::writeBytes(const std::vector<uint8_t>& bytes) {
    data_.insert(data_.end(), bytes.begin(), bytes.end());
}

void BinaryStream::writeBytes(const uint8_t* data, size_t len) {
    data_.insert(data_.end(), data, data + len);
}

} // namespace bedrock
