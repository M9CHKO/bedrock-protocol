#include <bedrock/relay/ChunkPublisherRetention.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[CHUNK-PUBLISHER-RETENTION-SMOKE] " << message << "\n";
    }
    return condition;
}

uint32_t readVarUInt(const std::vector<uint8_t>& bytes, std::size_t& offset) {
    return bedrock::VersionedPacketCodec::readVarUInt(bytes, offset);
}

bedrock::VersionedGamePacket makePublisherPacket(uint32_t radiusBlocks) {
    std::vector<uint8_t> payload;
    // BlockCoordinates: x=-1 (zigzag 1), y=64, z=300 (zigzag 600).
    bedrock::VersionedPacketCodec::writeVarUInt(payload, 1);
    bedrock::VersionedPacketCodec::writeVarUInt(payload, 64);
    bedrock::VersionedPacketCodec::writeVarUInt(payload, 600);
    bedrock::VersionedPacketCodec::writeVarUInt(payload, radiusBlocks);
    // Empty saved_chunks array (lu32 count).
    payload.insert(payload.end(), {0, 0, 0, 0});

    bedrock::VersionedGamePacket packet;
    packet.packetId = 0x79;
    packet.name = "network_chunk_publisher_update";
    packet.paramsType = "packet_network_chunk_publisher_update";
    packet.payload = payload;
    // A two-byte synthetic header verifies that the helper preserves it.
    packet.fullPacket = {0xf9, 0x01};
    packet.fullPacket.insert(
        packet.fullPacket.end(),
        payload.begin(),
        payload.end()
    );
    return packet;
}

} // namespace

int main() {
    bool ok = true;

    auto packet = makePublisherPacket(160);
    const auto originalTail = std::vector<uint8_t>(
        packet.payload.end() - 4,
        packet.payload.end()
    );
    const auto result = bedrock::retainPublishedChunks(packet, 512);
    ok &= check(result.recognized, "publisher packet was not recognized");
    ok &= check(result.rewritten, "larger configured radius was not applied");
    ok &= check(result.originalRadiusBlocks == 160, "server radius was misread");
    ok &= check(result.effectiveRadiusBlocks == 512, "effective radius is wrong");
    ok &= check(
        packet.fullPacket.size() >= 2 &&
            packet.fullPacket[0] == 0xf9 && packet.fullPacket[1] == 0x01,
        "original packet header was not preserved"
    );
    ok &= check(
        std::vector<uint8_t>(packet.payload.end() - 4, packet.payload.end()) ==
            originalTail,
        "saved_chunks bytes changed"
    );

    std::size_t offset = 0;
    (void) readVarUInt(packet.payload, offset);
    (void) readVarUInt(packet.payload, offset);
    (void) readVarUInt(packet.payload, offset);
    ok &= check(
        readVarUInt(packet.payload, offset) == 512,
        "rewritten packet does not contain the configured radius"
    );

    auto noOpPacket = makePublisherPacket(320);
    const auto noOpBytes = noOpPacket.fullPacket;
    const auto noOp = bedrock::retainPublishedChunks(noOpPacket, 160);
    ok &= check(!noOp.rewritten, "helper reduced the server radius");
    ok &= check(noOpPacket.fullPacket == noOpBytes, "no-op packet bytes changed");

    auto malformed = makePublisherPacket(160);
    malformed.payload = {0x80};
    malformed.fullPacket = {0x79, 0x80};
    const auto malformedBytes = malformed.fullPacket;
    const auto malformedResult = bedrock::retainPublishedChunks(malformed, 512);
    ok &= check(!malformedResult.rewritten, "malformed packet was rewritten");
    ok &= check(
        malformed.fullPacket == malformedBytes,
        "malformed packet was not left byte-for-byte intact"
    );

    if (!ok) return 1;
    std::cout << "chunk publisher retention smoke: OK\n";
    return 0;
}
