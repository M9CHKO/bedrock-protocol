#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

enum class BedrockSubChunkResult : uint8_t {
    Undefined = 0,
    Success = 1,
    ChunkNotFound = 2,
    InvalidDimension = 3,
    PlayerNotFound = 4,
    YIndexOutOfBounds = 5,
    SuccessAllAir = 6
};

enum class BedrockHeightMapType : uint8_t {
    NoData = 0,
    HasData = 1,
    TooHigh = 2,
    TooLow = 3,
    AllCopied = 4
};

struct BedrockSubChunkPacketEntry {
    int8_t dx = 0;
    int8_t dy = 0;
    int8_t dz = 0;
    BedrockSubChunkResult result = BedrockSubChunkResult::Undefined;
    std::vector<uint8_t> payload;
    BedrockHeightMapType heightMapType = BedrockHeightMapType::NoData;
    std::vector<int8_t> heightMap;
    BedrockHeightMapType renderHeightMapType = BedrockHeightMapType::NoData;
    std::vector<int8_t> renderHeightMap;
    std::optional<uint64_t> blobId;
};

struct BedrockSubChunkPacket {
    bool cacheEnabled = false;
    int32_t dimension = 0;
    int32_t originX = 0;
    int32_t originY = 0;
    int32_t originZ = 0;
    std::vector<BedrockSubChunkPacketEntry> entries;
};

class BedrockSubChunkPacketError : public std::runtime_error {
public:
    explicit BedrockSubChunkPacketError(const std::string& message)
        : std::runtime_error(message) {}
};

class BedrockSubChunkPacketCodec {
public:
    // Decode only the fixed packet header. Entries intentionally remain
    // empty so relay queue prioritisation stays off the network hot path.
    // Minecraft 1.18.0 stores cacheEnabled after its single entry, so the
    // header-only result leaves that legacy field false.
    static BedrockSubChunkPacket decodePacketHeader(
        const std::vector<uint8_t>& payload,
        const std::string& minecraftVersion
    );

    static BedrockSubChunkPacket decodePacketPayload(
        const std::vector<uint8_t>& payload,
        const std::string& minecraftVersion
    );

    static std::vector<uint8_t> encodePacketPayload(
        const BedrockSubChunkPacket& packet,
        const std::string& minecraftVersion
    );
};

} // namespace bedrock
