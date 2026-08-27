#pragma once

#include <bedrock/protocol/GamePacket.hpp>

#include <cstdint>
#include <vector>

namespace bedrock {

enum class CompressionMode {
    AlwaysDeflate,
    Threshold,
    Never,
    AlwaysSnappy,
    SnappyThreshold
};

class BatchCodec {
public:
    // framedPackets = [packetSize varuint][gamePacket][packetSize varuint][gamePacket]...
    static std::vector<uint8_t> encodeFramedBatch(
        const std::vector<std::vector<uint8_t>>& fullPackets
    );

    static DecodedBatch decodeFramedBatch(
        const std::vector<uint8_t>& framedPackets
    );

    // compressionPacket:
    // 0x00 + deflateRaw(framedPackets)
    // 0x01 + snappyRaw(framedPackets)
    // 0xff + framedPackets
    static std::vector<uint8_t> encodeCompressionPacket(
        const std::vector<uint8_t>& framedPackets,
        CompressionMode mode = CompressionMode::AlwaysDeflate,
        size_t threshold = 256
    );

    static std::vector<uint8_t> decodeCompressionPacket(
        const std::vector<uint8_t>& compressionPacket,
        uint8_t* compressionHeaderOut = nullptr
    );

    // MCPE payload после NetworkSettings:
    // 0xfe + compressionPacket
    static std::vector<uint8_t> wrapMcpe(
        const std::vector<uint8_t>& compressionPacket
    );

    static std::vector<uint8_t> unwrapMcpe(
        const std::vector<uint8_t>& mcpePayload
    );

    static std::vector<uint8_t> deflateRaw(
        const std::vector<uint8_t>& input
    );

    static std::vector<uint8_t> inflateRaw(
        const std::vector<uint8_t>& input
    );
};

} // namespace bedrock
