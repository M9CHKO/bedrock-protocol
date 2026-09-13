#include <bedrock/protocol/GamePacketCodec.hpp>

#include <bedrock/protocol/PacketRegistry.hpp>
#include <bedrock/util/VarInt.hpp>

#include <stdexcept>

namespace bedrock {

uint32_t GamePacketCodec::makeHeader(uint32_t packetId, uint8_t senderSubId, uint8_t targetSubId) {
    return (packetId & 0x3ff)
        | ((static_cast<uint32_t>(senderSubId) & 0x03) << 10)
        | ((static_cast<uint32_t>(targetSubId) & 0x03) << 12);
}

GamePacket GamePacketCodec::decodePacket(const std::vector<uint8_t>& fullPacket) {
    size_t offset = 0;

    const uint32_t header = VarInt::readUnsignedVarInt(fullPacket, offset);

    GamePacket packet;
    packet.packetId = header & 0x3ff;
    packet.senderSubId = static_cast<uint8_t>((header >> 10) & 0x03);
    packet.targetSubId = static_cast<uint8_t>((header >> 12) & 0x03);
    packet.name = PacketRegistry::nameOf(packet.packetId);
    packet.fullPacket = fullPacket;

    if (offset < fullPacket.size()) {
        packet.payload.assign(fullPacket.begin() + static_cast<long>(offset), fullPacket.end());
    }

    return packet;
}

DecodedBatch GamePacketCodec::decodeBatch(const std::vector<uint8_t>& framedPackets) {
    DecodedBatch batch;
    size_t offset = 0;

    while (offset < framedPackets.size()) {
        const uint32_t packetSize = VarInt::readUnsignedVarInt(framedPackets, offset);

        if (packetSize == 0) {
            throw std::runtime_error("GamePacketCodec::decodeBatch: zero packet size");
        }

        if (offset + packetSize > framedPackets.size()) {
            throw std::runtime_error("GamePacketCodec::decodeBatch: packet size outside batch");
        }

        std::vector<uint8_t> fullPacket(
            framedPackets.begin() + static_cast<long>(offset),
            framedPackets.begin() + static_cast<long>(offset + packetSize)
        );

        offset += packetSize;
        batch.packets.push_back(decodePacket(fullPacket));
    }

    return batch;
}

std::vector<uint8_t> GamePacketCodec::encodePacket(
    uint32_t packetId,
    const std::vector<uint8_t>& payload,
    uint8_t senderSubId,
    uint8_t targetSubId
) {
    std::vector<uint8_t> out;

    const uint32_t header = makeHeader(packetId, senderSubId, targetSubId);
    VarInt::writeUnsignedVarInt(out, header);

    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> GamePacketCodec::encodeBatchFromFullPackets(
    const std::vector<std::vector<uint8_t>>& fullPackets
) {
    std::vector<uint8_t> out;

    for (const auto& packet : fullPackets) {
        VarInt::writeUnsignedVarInt(out, static_cast<uint32_t>(packet.size()));
        out.insert(out.end(), packet.begin(), packet.end());
    }

    return out;
}

std::vector<uint8_t> GamePacketCodec::encodeBatch(const std::vector<GamePacket>& packets) {
    std::vector<std::vector<uint8_t>> fullPackets;

    for (const auto& packet : packets) {
        if (!packet.fullPacket.empty()) {
            fullPackets.push_back(packet.fullPacket);
        } else {
            fullPackets.push_back(encodePacket(
                packet.packetId,
                packet.payload,
                packet.senderSubId,
                packet.targetSubId
            ));
        }
    }

    return encodeBatchFromFullPackets(fullPackets);
}

} // namespace bedrock
