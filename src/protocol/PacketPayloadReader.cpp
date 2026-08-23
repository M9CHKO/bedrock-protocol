#include <bedrock/protocol/PacketPayloadReader.hpp>

#include <cstring>
#include <limits>

namespace bedrock {
namespace {

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& data)
        : data_(data) {}

    size_t remaining() const {
        return data_.size() - offset_;
    }

    size_t offset() const {
        return offset_;
    }

    bool eof() const {
        return offset_ >= data_.size();
    }

    uint8_t u8() {
        require(1, "u8");
        return data_[offset_++];
    }

    bool boolean() {
        return u8() != 0;
    }

    int32_t i32le() {
        require(4, "i32le");
        uint32_t v =
            static_cast<uint32_t>(data_[offset_]) |
            (static_cast<uint32_t>(data_[offset_ + 1]) << 8) |
            (static_cast<uint32_t>(data_[offset_ + 2]) << 16) |
            (static_cast<uint32_t>(data_[offset_ + 3]) << 24);
        offset_ += 4;
        return static_cast<int32_t>(v);
    }

    float f32le() {
        require(4, "f32le");
        uint32_t bits =
            static_cast<uint32_t>(data_[offset_]) |
            (static_cast<uint32_t>(data_[offset_ + 1]) << 8) |
            (static_cast<uint32_t>(data_[offset_ + 2]) << 16) |
            (static_cast<uint32_t>(data_[offset_ + 3]) << 24);
        offset_ += 4;

        float value = 0.0f;
        static_assert(sizeof(value) == sizeof(bits), "float must be 32-bit");
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    uint32_t uvarint() {
        uint32_t value = 0;
        int shift = 0;

        for (int i = 0; i < 5; ++i) {
            uint8_t b = u8();
            value |= static_cast<uint32_t>(b & 0x7f) << shift;

            if ((b & 0x80) == 0) {
                return value;
            }

            shift += 7;
        }

        throw PacketPayloadReaderError("uvarint too long");
    }

    int32_t svarint() {
        uint32_t raw = uvarint();
        return static_cast<int32_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }

    uint64_t uvarlong() {
        uint64_t value = 0;
        int shift = 0;

        for (int i = 0; i < 10; ++i) {
            uint8_t b = u8();
            value |= static_cast<uint64_t>(b & 0x7f) << shift;

            if ((b & 0x80) == 0) {
                return value;
            }

            shift += 7;
        }

        throw PacketPayloadReaderError("uvarlong too long");
    }

    int64_t svarlong() {
        uint64_t raw = uvarlong();
        return static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }

    std::string string() {
        uint32_t len = uvarint();
        require(len, "string");

        std::string out;
        out.resize(len);

        if (len > 0) {
            std::memcpy(out.data(), data_.data() + offset_, len);
        }

        offset_ += len;
        return out;
    }

    std::vector<uint8_t> bytes(size_t len) {
        require(len, "bytes");

        std::vector<uint8_t> out(
            data_.begin() + static_cast<std::ptrdiff_t>(offset_),
            data_.begin() + static_cast<std::ptrdiff_t>(offset_ + len)
        );

        offset_ += len;
        return out;
    }

private:
    void require(size_t len, const char* what) const {
        if (len > remaining()) {
            throw PacketPayloadReaderError(std::string("not enough bytes for ") + what);
        }
    }

    const std::vector<uint8_t>& data_;
    size_t offset_ = 0;
};

std::vector<uint8_t> payloadFromPacketOrPayload(
    const std::vector<uint8_t>& packetOrPayload,
    uint32_t packetId
) {
    if (!packetOrPayload.empty() && packetOrPayload[0] == static_cast<uint8_t>(packetId)) {
        return std::vector<uint8_t>(packetOrPayload.begin() + 1, packetOrPayload.end());
    }

    return packetOrPayload;
}

const std::vector<uint8_t>& payloadFromGamePacket(const GamePacket& packet) {
    return packet.payload;
}

} // namespace

PlayStatusPacket PacketPayloadReader::readPlayStatus(const GamePacket& packet) {
    return readPlayStatus(payloadFromGamePacket(packet));
}

PlayStatusPacket PacketPayloadReader::readPlayStatus(const std::vector<uint8_t>& packetOrPayload) {
    auto payload = payloadFromPacketOrPayload(packetOrPayload, 2);
    Reader r(payload);

    PlayStatusPacket out;
    out.status = r.i32le();

    return out;
}

PayloadTextPacket PacketPayloadReader::readText(const GamePacket& packet) {
    return readText(payloadFromGamePacket(packet));
}

PayloadTextPacket PacketPayloadReader::readText(const std::vector<uint8_t>& packetOrPayload) {
    auto payload = payloadFromPacketOrPayload(packetOrPayload, 9);
    Reader r(payload);

    PayloadTextPacket out;
    out.type = r.u8();
    out.needsTranslation = r.boolean();

    // Common Bedrock text layout for raw/chat-like messages:
    // type + needsTranslation + message + xuid + platformChatId.
    // Some message types also contain sourceName before message.
    if (out.type == 1 || out.type == 7 || out.type == 8) {
        out.sourceName = r.string();
    }

    out.message = r.string();

    if (!r.eof()) {
        try {
            out.xuid = r.string();
        } catch (...) {
            return out;
        }
    }

    if (!r.eof()) {
        try {
            out.platformChatId = r.string();
        } catch (...) {
            return out;
        }
    }

    return out;
}

LevelChunkPacket PacketPayloadReader::readLevelChunk(const GamePacket& packet) {
    return readLevelChunk(payloadFromGamePacket(packet));
}

LevelChunkPacket PacketPayloadReader::readLevelChunk(const std::vector<uint8_t>& packetOrPayload) {
    auto payload = payloadFromPacketOrPayload(packetOrPayload, 58);
    Reader r(payload);

    LevelChunkPacket out;
    out.chunkX = r.svarint();
    out.chunkZ = r.svarint();
    out.dimension = r.svarint();
    out.subChunkCount = r.uvarint();
    out.cacheEnabled = r.boolean();
    out.dataSize = r.uvarint();

    if (out.dataSize > r.remaining()) {
        throw PacketPayloadReaderError("level_chunk dataSize is larger than remaining payload");
    }

    out.data = r.bytes(out.dataSize);

    return out;
}

StartGamePacket PacketPayloadReader::readStartGame(const GamePacket& packet) {
    return readStartGame(payloadFromGamePacket(packet));
}

StartGamePacket PacketPayloadReader::readStartGame(const std::vector<uint8_t>& packetOrPayload) {
    auto payload = payloadFromPacketOrPayload(packetOrPayload, 11);
    Reader r(payload);

    StartGamePacket out;

    out.entityUniqueId = r.svarlong();
    out.runtimeEntityId = r.uvarlong();

    out.playerGameMode = r.svarint();
    out.gameMode = out.playerGameMode;

    out.positionX = r.f32le();
    out.positionY = r.f32le();
    out.positionZ = r.f32le();

    out.x = out.positionX;
    out.y = out.positionY;
    out.z = out.positionZ;

    out.rotationX = r.f32le();
    out.rotationY = r.f32le();

    return out;
}

} // namespace bedrock
