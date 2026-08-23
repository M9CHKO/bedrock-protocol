#pragma once

#include <bedrock/protocol/VersionedPacketCodec.hpp>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

struct VersionedPlayStatusPacket {
    int32_t status = 0;
};

struct VersionedTextPacket {
    uint8_t type = 0;
    bool needsTranslation = false;
    std::string sourceName;
    std::string message;
    std::string xuid;
    std::string platformChatId;
};

struct VersionedLevelChunkPacket {
    int32_t chunkX = 0;
    int32_t chunkZ = 0;
    int32_t dimension = 0;
    int32_t subChunkCount = 0;
    bool cacheEnabled = false;
    uint32_t dataSize = 0;
    std::vector<uint8_t> data;
};

struct VersionedStartGamePacket {
    int64_t entityId = 0;
    uint64_t runtimeEntityId = 0;
    int32_t playerGameMode = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

class VersionedPayloadCursor {
public:
    explicit VersionedPayloadCursor(const std::vector<uint8_t>& data)
        : data_(data) {}

    std::size_t offset() const {
        return offset_;
    }

    std::size_t remaining() const {
        if (offset_ > data_.size()) {
            return 0;
        }

        return data_.size() - offset_;
    }

    bool eof() const {
        return offset_ >= data_.size();
    }

    uint8_t readU8() {
        require(1, "u8");

        return data_[offset_++];
    }

    bool readBool() {
        return readU8() != 0;
    }

    uint16_t readU16LE() {
        require(2, "u16le");

        uint16_t value =
            static_cast<uint16_t>(data_[offset_]) |
            static_cast<uint16_t>(data_[offset_ + 1] << 8);

        offset_ += 2;
        return value;
    }

    uint32_t readU32LE() {
        require(4, "u32le");

        uint32_t value =
            static_cast<uint32_t>(data_[offset_]) |
            (static_cast<uint32_t>(data_[offset_ + 1]) << 8) |
            (static_cast<uint32_t>(data_[offset_ + 2]) << 16) |
            (static_cast<uint32_t>(data_[offset_ + 3]) << 24);

        offset_ += 4;
        return value;
    }

    uint32_t readU32BE() {
        require(4, "u32be");

        uint32_t value =
            (static_cast<uint32_t>(data_[offset_]) << 24) |
            (static_cast<uint32_t>(data_[offset_ + 1]) << 16) |
            (static_cast<uint32_t>(data_[offset_ + 2]) << 8) |
            static_cast<uint32_t>(data_[offset_ + 3]);

        offset_ += 4;
        return value;
    }

    uint64_t readU64LE() {
        require(8, "u64le");

        uint64_t value = 0;

        for (int i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(data_[offset_ + i]) << (8 * i);
        }

        offset_ += 8;
        return value;
    }

    float readF32LE() {
        uint32_t raw = readU32LE();

        float value = 0.0f;
        std::memcpy(&value, &raw, sizeof(float));

        return value;
    }

    uint32_t readVarUInt() {
        uint32_t value = 0;

        for (int shift = 0; shift <= 28; shift += 7) {
            uint8_t byte = readU8();
            value |= static_cast<uint32_t>(byte & 0x7f) << shift;

            if ((byte & 0x80) == 0) {
                return value;
            }
        }

        throw std::runtime_error("varuint too long");
    }

    uint64_t readVarULong() {
        uint64_t value = 0;

        for (int shift = 0; shift <= 63; shift += 7) {
            uint8_t byte = readU8();
            value |= static_cast<uint64_t>(byte & 0x7f) << shift;

            if ((byte & 0x80) == 0) {
                return value;
            }
        }

        throw std::runtime_error("varulong too long");
    }

    int32_t readVarInt() {
        uint32_t raw = readVarUInt();
        return static_cast<int32_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }

    int64_t readVarLong() {
        uint64_t raw = readVarULong();
        return static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }

    std::string readString() {
        uint32_t size = readVarUInt();

        require(size, "string");

        std::string out(
            reinterpret_cast<const char*>(data_.data() + offset_),
            reinterpret_cast<const char*>(data_.data() + offset_ + size)
        );

        offset_ += size;
        return out;
    }

    std::vector<uint8_t> readBytes(std::size_t size) {
        require(size, "bytes");

        std::vector<uint8_t> out(
            data_.begin() + static_cast<std::ptrdiff_t>(offset_),
            data_.begin() + static_cast<std::ptrdiff_t>(offset_ + size)
        );

        offset_ += size;
        return out;
    }

    std::vector<uint8_t> readRemainingBytes() {
        return readBytes(remaining());
    }

private:
    const std::vector<uint8_t>& data_;
    std::size_t offset_ = 0;

    void require(std::size_t size, const char* what) const {
        if (offset_ + size > data_.size()) {
            throw std::runtime_error(std::string("not enough bytes for ") + what);
        }
    }
};

class VersionedPayloadReader {
public:
    static VersionedPlayStatusPacket readPlayStatus(const VersionedGamePacket& packet) {
        requirePacket(packet, "play_status");

        VersionedPayloadCursor cursor(packet.payload);

        VersionedPlayStatusPacket out;
        out.status = static_cast<int32_t>(cursor.readU32BE());

        return out;
    }

    static VersionedTextPacket readText(const VersionedGamePacket& packet) {
        requirePacket(packet, "text");

        VersionedPayloadCursor cursor(packet.payload);

        VersionedTextPacket out;
        out.type = cursor.readU8();
        out.needsTranslation = cursor.readBool();

        // Common Bedrock text variants:
        // 0 raw / 1 chat / 5 tip / 6 system usually contain message directly.
        // 2 translation and some chat variants may include sourceName first.
        if (out.type == 1 || out.type == 7 || out.type == 8) {
            out.sourceName = cursor.readString();
            out.message = cursor.readString();
        } else {
            out.message = cursor.readString();
        }

        if (!cursor.eof()) {
            out.xuid = cursor.readString();
        }

        if (!cursor.eof()) {
            out.platformChatId = cursor.readString();
        }

        return out;
    }

    static VersionedLevelChunkPacket readLevelChunk(const VersionedGamePacket& packet) {
        requirePacket(packet, "level_chunk");

        VersionedPayloadCursor cursor(packet.payload);

        VersionedLevelChunkPacket out;

        out.chunkX = cursor.readVarInt();
        out.chunkZ = cursor.readVarInt();
        out.dimension = cursor.readVarInt();
        out.subChunkCount = cursor.readVarInt();
        out.cacheEnabled = cursor.readBool();

        if (out.cacheEnabled) {
            // Blob-cache enabled format includes blob ids before actual data.
            // For now we skip ids defensively and still read remaining data if present.
            uint32_t blobCount = cursor.readVarUInt();

            for (uint32_t i = 0; i < blobCount; ++i) {
                (void) cursor.readU64LE();
            }
        }

        if (!cursor.eof()) {
            out.dataSize = cursor.readVarUInt();
            out.data = cursor.readBytes(out.dataSize);
        }

        return out;
    }

    static VersionedStartGamePacket readStartGame(const VersionedGamePacket& packet) {
        requirePacket(packet, "start_game");

        VersionedPayloadCursor cursor(packet.payload);

        VersionedStartGamePacket out;

        out.entityId = cursor.readVarLong();
        out.runtimeEntityId = cursor.readVarULong();

        out.playerGameMode = cursor.readVarInt();

        out.x = cursor.readF32LE();
        out.y = cursor.readF32LE();
        out.z = cursor.readF32LE();

        return out;
    }

private:
    static void requirePacket(const VersionedGamePacket& packet, const std::string& expectedName) {
        if (packet.name != expectedName) {
            throw std::runtime_error(
                "wrong packet for payload reader: got " + packet.name + ", expected " + expectedName
            );
        }
    }
};

} // namespace bedrock
