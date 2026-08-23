#pragma once

#include <bedrock/protocol/PacketIO.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

class PacketPayloadReaderError : public std::runtime_error {
public:
    explicit PacketPayloadReaderError(const std::string& message)
        : std::runtime_error(message) {}
};

struct PlayStatusPacket {
    int32_t status = 0;
};

struct PayloadTextPacket {
    uint8_t type = 0;
    bool needsTranslation = false;
    std::string message;
    std::string sourceName;
    std::string xuid;
    std::string platformChatId;
};

struct LevelChunkPacket {
    int32_t chunkX = 0;
    int32_t chunkZ = 0;
    int32_t dimension = 0;
    uint32_t subChunkCount = 0;
    bool cacheEnabled = false;
    uint32_t dataSize = 0;
    std::vector<uint8_t> data;
};

struct StartGamePacket {
    int64_t entityUniqueId = 0;
    uint64_t runtimeEntityId = 0;

    int32_t playerGameMode = 0;
    int32_t gameMode = 0; // alias for old tests/usages

    float positionX = 0.0f;
    float positionY = 0.0f;
    float positionZ = 0.0f;

    // short aliases, like common JS usage
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    float rotationX = 0.0f;
    float rotationY = 0.0f;
};

class PacketPayloadReader {
public:
    static PlayStatusPacket readPlayStatus(const GamePacket& packet);
    static PlayStatusPacket readPlayStatus(const std::vector<uint8_t>& packetOrPayload);

    static PayloadTextPacket readText(const GamePacket& packet);
    static PayloadTextPacket readText(const std::vector<uint8_t>& packetOrPayload);

    static LevelChunkPacket readLevelChunk(const GamePacket& packet);
    static LevelChunkPacket readLevelChunk(const std::vector<uint8_t>& packetOrPayload);

    static StartGamePacket readStartGame(const GamePacket& packet);
    static StartGamePacket readStartGame(const std::vector<uint8_t>& packetOrPayload);
};

} // namespace bedrock
