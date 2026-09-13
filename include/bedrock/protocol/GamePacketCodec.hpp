#pragma once

#include <bedrock/protocol/GamePacket.hpp>

#include <cstdint>
#include <vector>

namespace bedrock {

class GamePacketCodec {
public:
    static uint32_t makeHeader(uint32_t packetId, uint8_t senderSubId = 0, uint8_t targetSubId = 0);

    static GamePacket decodePacket(const std::vector<uint8_t>& fullPacket);
    static DecodedBatch decodeBatch(const std::vector<uint8_t>& framedPackets);

    static std::vector<uint8_t> encodePacket(
        uint32_t packetId,
        const std::vector<uint8_t>& payload = {},
        uint8_t senderSubId = 0,
        uint8_t targetSubId = 0
    );

    static std::vector<uint8_t> encodeBatchFromFullPackets(
        const std::vector<std::vector<uint8_t>>& fullPackets
    );

    static std::vector<uint8_t> encodeBatch(
        const std::vector<GamePacket>& packets
    );
};

} // namespace bedrock
