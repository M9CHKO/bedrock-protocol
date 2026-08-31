#pragma once

#include <bedrock/protocol/VersionedPacketCodec.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bedrock {

struct ChunkPublisherRetentionResult {
    bool recognized = false;
    bool rewritten = false;
    uint32_t originalRadiusBlocks = 0;
    uint32_t effectiveRadiusBlocks = 0;
};

// NetworkChunkPublisherUpdate tells Bedrock which already-delivered chunks
// should remain visible. Its position is three varints and its radius is the
// following varint, measured in blocks. Rewriting only that field avoids
// decoding or re-encoding any registry-dependent packet data.
inline ChunkPublisherRetentionResult retainPublishedChunks(
    VersionedGamePacket& packet,
    uint32_t minimumRadiusBlocks
) noexcept {
    ChunkPublisherRetentionResult result;
    if (packet.name != "network_chunk_publisher_update") {
        return result;
    }
    result.recognized = true;

    try {
        auto readVarUInt = [&packet](std::size_t& offset, uint32_t& value) {
            value = 0;
            uint32_t shift = 0;
            for (int byteIndex = 0; byteIndex < 5; ++byteIndex) {
                if (offset >= packet.payload.size()) return false;
                const uint8_t byte = packet.payload[offset++];
                value |= static_cast<uint32_t>(byte & 0x7fu) << shift;
                if ((byte & 0x80u) == 0) return true;
                shift += 7;
            }
            return false;
        };

        std::size_t offset = 0;
        uint32_t ignored = 0;
        if (!readVarUInt(offset, ignored) ||
            !readVarUInt(offset, ignored) ||
            !readVarUInt(offset, ignored)) {
            return result;
        }

        const std::size_t radiusStart = offset;
        uint32_t serverRadiusBlocks = 0;
        if (!readVarUInt(offset, serverRadiusBlocks)) {
            return result;
        }
        result.originalRadiusBlocks = serverRadiusBlocks;
        result.effectiveRadiusBlocks = std::max(
            serverRadiusBlocks,
            minimumRadiusBlocks
        );
        if (result.effectiveRadiusBlocks == serverRadiusBlocks) {
            return result;
        }

        std::vector<uint8_t> encodedRadius;
        VersionedPacketCodec::writeVarUInt(
            encodedRadius,
            result.effectiveRadiusBlocks
        );

        const auto originalPayload = packet.payload;
        std::vector<uint8_t> rewrittenPayload;
        rewrittenPayload.reserve(
            originalPayload.size() - (offset - radiusStart) +
            encodedRadius.size()
        );
        rewrittenPayload.insert(
            rewrittenPayload.end(),
            originalPayload.begin(),
            originalPayload.begin() + static_cast<std::ptrdiff_t>(radiusStart)
        );
        rewrittenPayload.insert(
            rewrittenPayload.end(),
            encodedRadius.begin(),
            encodedRadius.end()
        );
        rewrittenPayload.insert(
            rewrittenPayload.end(),
            originalPayload.begin() + static_cast<std::ptrdiff_t>(offset),
            originalPayload.end()
        );

        std::vector<uint8_t> rewrittenFullPacket;
        const bool fullPacketEndsWithPayload =
            packet.fullPacket.size() >= originalPayload.size() &&
            std::equal(
                originalPayload.rbegin(),
                originalPayload.rend(),
                packet.fullPacket.rbegin()
            );
        if (fullPacketEndsWithPayload) {
            const std::size_t headerSize =
                packet.fullPacket.size() - originalPayload.size();
            rewrittenFullPacket.reserve(headerSize + rewrittenPayload.size());
            rewrittenFullPacket.insert(
                rewrittenFullPacket.end(),
                packet.fullPacket.begin(),
                packet.fullPacket.begin() +
                    static_cast<std::ptrdiff_t>(headerSize)
            );
        } else {
            VersionedPacketCodec::writeVarUInt(
                rewrittenFullPacket,
                packet.packetId
            );
            rewrittenFullPacket.reserve(
                rewrittenFullPacket.size() + rewrittenPayload.size()
            );
        }
        rewrittenFullPacket.insert(
            rewrittenFullPacket.end(),
            rewrittenPayload.begin(),
            rewrittenPayload.end()
        );

        packet.payload = std::move(rewrittenPayload);
        packet.fullPacket = std::move(rewrittenFullPacket);
        result.rewritten = true;
        return result;
    } catch (...) {
        // Allocation or malformed-packet failures must never disturb relay
        // forwarding. The original packet remains untouched until assignment.
        return result;
    }
}

} // namespace bedrock
