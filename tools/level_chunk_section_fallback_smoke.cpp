#include <bedrock/world/BedrockChunk.hpp>

#include <iostream>
#include <vector>

namespace {

struct SchematicCacheProbe {
    bool known = false;
    bool semanticAir = false;
    int32_t runtimeId = 0;
};

SchematicCacheProbe probeSchematicCache(
    const bedrock::BedrockChunkColumn& column,
    const bedrock::BlockPosition& position
) {
    const auto* section = column.getSection(position.y);
    if (section == nullptr) return {};
    const int32_t runtimeId = column.getBlockStateId(position);
    return {
        true,
        runtimeId == column.airStateId(),
        runtimeId
    };
}

bool strictDecoderRejects(const bedrock::BedrockLevelChunkPacket& packet) {
    try {
        (void) bedrock::BedrockLevelChunkCodec::decodeNoCacheColumn(packet);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

bool checkZeroStorageRecovery(uint8_t version) {
    const int32_t emptySectionY = version == 8 ? 0 : -4;
    const int32_t nextSectionY = emptySectionY + 1;
    const int32_t airStateId = version == 8 ? 608 : 609;
    const int32_t populatedStateId = version == 8 ? 808 : 909;

    bedrock::BedrockLevelChunkPacket packet;
    packet.x = version == 8 ? 31 : -31;
    packet.z = version == 8 ? -17 : 17;
    packet.dimension = 0;
    packet.subChunkCount = 2;
    packet.cacheEnabled = false;
    packet.payload = {version, 0};
    if (version == 9) {
        packet.payload.push_back(static_cast<uint8_t>(emptySectionY));
    }

    auto populated = bedrock::BedrockSubChunk::createAir(
        nextSectionY,
        populatedStateId
    );
    populated.setSubChunkVersion(version);
    const auto populatedBytes = populated.encode(
        bedrock::ChunkStorageType::Runtime
    );
    packet.payload.insert(
        packet.payload.end(),
        populatedBytes.begin(),
        populatedBytes.end()
    );

    // Force the full decoder onto its recovery path after it has consumed both
    // block sections. The original packet remains byte-for-byte untouched.
    packet.payload.push_back(0);
    const auto originalPayload = packet.payload;
    if (!strictDecoderRejects(packet)) {
        std::cerr << "[LEVEL-CHUNK-FALLBACK] strict decoder accepted zero-storage "
                  << (version == 8 ? "v8" : "v9") << " legacy trailer\n";
        return false;
    }

    const auto column =
        bedrock::BedrockLevelChunkCodec::decodeNoCacheBlockSectionsFallback(
            packet,
            airStateId
        );
    const auto* empty = column.getSectionAtIndex(emptySectionY);
    const auto* next = column.getSectionAtIndex(nextSectionY);
    const auto emptySnapshot = probeSchematicCache(
        column,
        {.x = 15, .y = emptySectionY * 16 + 15, .z = 15}
    );
    const auto populatedSnapshot = probeSchematicCache(
        column,
        {.x = 0, .y = nextSectionY * 16, .z = 0}
    );
    if (packet.payload != originalPayload || empty == nullptr || next == nullptr ||
        column.airStateId() != airStateId ||
        empty->subChunkVersion() != version || empty->y() != emptySectionY ||
        empty->layerCount() != 0 || empty->hasLayer(0) ||
        empty->getBlockStateId(15, 15, 15) != airStateId ||
        next->subChunkVersion() != version || next->y() != nextSectionY ||
        next->getBlockStateId(0, 0, 0) != populatedStateId ||
        column.getBlockStateId(
            {.x = 15, .y = emptySectionY * 16 + 15, .z = 15}
        ) != airStateId ||
        column.getBlockStateId(
            {.x = 0, .y = nextSectionY * 16, .z = 0}
        ) != populatedStateId ||
        !emptySnapshot.known || !emptySnapshot.semanticAir ||
        emptySnapshot.runtimeId != airStateId || !populatedSnapshot.known ||
        populatedSnapshot.semanticAir ||
        populatedSnapshot.runtimeId != populatedStateId) {
        std::cerr << "[LEVEL-CHUNK-FALLBACK] zero-storage "
                  << (version == 8 ? "v8" : "v9")
                  << " alignment/Y/schematic-cache mismatch\n";
        return false;
    }
    return true;
}

bool checkStrictZeroStorage(uint8_t version) {
    const int32_t sectionY = version == 8 ? 0 : -4;
    const int32_t airStateId = version == 8 ? 1608 : 1609;
    const int32_t placedStateId = version == 8 ? 1808 : 1809;

    bedrock::BedrockChunkColumn trailerSource(0, 0, airStateId);
    trailerSource.setBounds(-4, 20);
    const auto validTrailer = trailerSource.networkEncodeNoCache(true);

    bedrock::BedrockLevelChunkPacket packet;
    packet.x = version == 8 ? 41 : -41;
    packet.z = version == 8 ? -29 : 29;
    packet.dimension = 0;
    packet.subChunkCount = 1;
    packet.cacheEnabled = false;
    packet.payload = {version, 0};
    if (version == 9) {
        packet.payload.push_back(static_cast<uint8_t>(sectionY));
    }
    packet.payload.insert(
        packet.payload.end(),
        validTrailer.begin(),
        validTrailer.end()
    );

    auto column = bedrock::BedrockLevelChunkCodec::decodeNoCacheColumn(
        packet,
        true,
        airStateId
    );
    const auto* empty = column.getSectionAtIndex(sectionY);
    const int32_t baseY = sectionY * 16;
    const int32_t missingY = version == 8 ? -64 : 0;
    if (empty == nullptr || empty->layerCount() != 0 ||
        empty->airStateId() != airStateId || column.airStateId() != airStateId ||
        column.getBlockStateId({.x = 0, .y = baseY, .z = 0}) != airStateId ||
        column.getBlockStateId({.x = 0, .y = missingY, .z = 0}) != airStateId) {
        std::cerr << "[LEVEL-CHUNK-FALLBACK] strict zero-storage "
                  << (version == 8 ? "v8" : "v9")
                  << " custom-air decode mismatch\n";
        return false;
    }

    constexpr uint8_t changedX = 3;
    constexpr uint8_t changedY = 5;
    constexpr uint8_t changedZ = 7;
    column.setBlockStateId(
        {
            .x = changedX,
            .y = baseY + changedY,
            .z = changedZ
        },
        placedStateId
    );

    std::size_t customAirCells = 0;
    for (uint8_t x = 0; x < 16; ++x) {
        for (uint8_t y = 0; y < 16; ++y) {
            for (uint8_t z = 0; z < 16; ++z) {
                const int32_t actual = column.getBlockStateId({
                    .x = x,
                    .y = baseY + y,
                    .z = z
                });
                if (x == changedX && y == changedY && z == changedZ) {
                    if (actual != placedStateId) return false;
                } else if (actual == airStateId) {
                    ++customAirCells;
                } else {
                    return false;
                }
            }
        }
    }
    if (customAirCells != 4095) {
        std::cerr << "[LEVEL-CHUNK-FALLBACK] strict zero-storage materialization "
                  << (version == 8 ? "v8" : "v9") << " mismatch\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!checkStrictZeroStorage(8) || !checkStrictZeroStorage(9)) {
        return 1;
    }
    if (!checkZeroStorageRecovery(8) || !checkZeroStorageRecovery(9)) {
        return 1;
    }

    // A final all-air v8 section has only the two-byte header. Do not require
    // the v9-only signed-Y byte when no later payload follows it.
    bedrock::BedrockLevelChunkPacket finalV8Empty;
    finalV8Empty.x = 0;
    finalV8Empty.z = 0;
    finalV8Empty.dimension = 0;
    finalV8Empty.subChunkCount = 1;
    finalV8Empty.cacheEnabled = false;
    finalV8Empty.payload = {8, 0};
    const auto finalV8Column =
        bedrock::BedrockLevelChunkCodec::decodeNoCacheBlockSectionsFallback(
            finalV8Empty
        );
    const auto* finalV8Section = finalV8Column.getSectionAtIndex(0);
    if (finalV8Section == nullptr || finalV8Section->layerCount() != 0 ||
        finalV8Section->y() != 0 ||
        finalV8Column.getBlockStateId({.x = 0, .y = 0, .z = 0}) != 0) {
        std::cerr << "[LEVEL-CHUNK-FALLBACK] final two-byte v8 air section mismatch\n";
        return 1;
    }

    bedrock::BedrockLevelChunkPacket packet;
    packet.x = -7;
    packet.z = 12;
    packet.dimension = 0;
    packet.subChunkCount = 2;
    packet.cacheEnabled = false;

    const auto bottom = bedrock::BedrockSubChunk::createAir(-4, 111).encode(
        bedrock::ChunkStorageType::Runtime
    );
    const auto top = bedrock::BedrockSubChunk::createAir(19, 222).encode(
        bedrock::ChunkStorageType::Runtime
    );
    packet.payload.insert(packet.payload.end(), bottom.begin(), bottom.end());
    packet.payload.insert(packet.payload.end(), top.begin(), top.end());

    // A leading zero is valid legacy 2D-biome data but invalid as the first
    // modern runtime biome palette. The strict full-column decoder must reject
    // it while the recovery decoder intentionally stops after the two blocks.
    packet.payload.push_back(0);
    const auto originalPayload = packet.payload;
    if (!strictDecoderRejects(packet)) {
        std::cerr << "[LEVEL-CHUNK-FALLBACK] strict decoder accepted legacy trailer\n";
        return 1;
    }

    const auto column =
        bedrock::BedrockLevelChunkCodec::decodeNoCacheBlockSectionsFallback(packet);
    if (packet.payload != originalPayload) {
        std::cerr << "[LEVEL-CHUNK-FALLBACK] source payload was modified\n";
        return 1;
    }
    if (column.x() != -7 || column.z() != 12 ||
        column.minCY() != -4 || column.maxCY() != 20 ||
        column.minY() != -64 || column.maxY() != 320 ||
        column.getSectionAtIndex(-4) == nullptr ||
        column.getSectionAtIndex(19) == nullptr ||
        column.getSectionAtIndex(-4)->y() != -4 ||
        column.getSectionAtIndex(19)->y() != 19 ||
        column.getBlockStateId({.x = 0, .y = -64, .z = 0}) != 111 ||
        column.getBlockStateId({.x = 15, .y = 319, .z = 15}) != 222) {
        std::cerr << "[LEVEL-CHUNK-FALLBACK] signed-Y/runtime-ID recovery mismatch\n";
        return 1;
    }

    bedrock::BedrockLevelChunkPacket v8Packet;
    v8Packet.x = 23;
    v8Packet.z = -9;
    v8Packet.dimension = 0;
    v8Packet.subChunkCount = 2;
    v8Packet.cacheEnabled = false;

    auto v8Bottom = bedrock::BedrockSubChunk::createAir(7, 333);
    v8Bottom.setSubChunkVersion(8);
    auto v8Next = bedrock::BedrockSubChunk::createAir(11, 444);
    v8Next.setSubChunkVersion(8);
    const auto v8BottomBytes = v8Bottom.encode(bedrock::ChunkStorageType::Runtime);
    const auto v8NextBytes = v8Next.encode(bedrock::ChunkStorageType::Runtime);
    v8Packet.payload.insert(
        v8Packet.payload.end(),
        v8BottomBytes.begin(),
        v8BottomBytes.end()
    );
    v8Packet.payload.insert(
        v8Packet.payload.end(),
        v8NextBytes.begin(),
        v8NextBytes.end()
    );
    v8Packet.payload.push_back(0);
    const auto originalV8Payload = v8Packet.payload;
    if (!strictDecoderRejects(v8Packet)) {
        std::cerr << "[LEVEL-CHUNK-FALLBACK] strict decoder accepted v8 legacy trailer\n";
        return 1;
    }

    const auto v8Column =
        bedrock::BedrockLevelChunkCodec::decodeNoCacheBlockSectionsFallback(v8Packet);
    if (v8Packet.payload != originalV8Payload) {
        std::cerr << "[LEVEL-CHUNK-FALLBACK] v8 source payload was modified\n";
        return 1;
    }
    if (v8Column.x() != 23 || v8Column.z() != -9 ||
        v8Column.getSectionAtIndex(0) == nullptr ||
        v8Column.getSectionAtIndex(1) == nullptr ||
        v8Column.getSectionAtIndex(0)->subChunkVersion() != 8 ||
        v8Column.getSectionAtIndex(1)->subChunkVersion() != 8 ||
        v8Column.getSectionAtIndex(0)->y() != 0 ||
        v8Column.getSectionAtIndex(1)->y() != 1 ||
        v8Column.getBlockStateId({.x = 0, .y = 0, .z = 0}) != 333 ||
        v8Column.getBlockStateId({.x = 15, .y = 31, .z = 15}) != 444) {
        std::cerr << "[LEVEL-CHUNK-FALLBACK] sequential v8 Y normalization mismatch\n";
        return 1;
    }

    bedrock::BedrockLevelChunkPacket malformed = packet;
    malformed.subChunkCount = 1;
    malformed.payload = {9, 1, 0};
    bool malformedRejected = false;
    try {
        (void) bedrock::BedrockLevelChunkCodec::decodeNoCacheBlockSectionsFallback(
            malformed
        );
    } catch (const bedrock::BedrockChunkError&) {
        malformedRejected = true;
    }
    if (!malformedRejected) {
        std::cerr << "[LEVEL-CHUNK-FALLBACK] malformed v8/v9 section was accepted\n";
        return 1;
    }

    std::cout << "[LEVEL-CHUNK-FALLBACK] OK\n";
    return 0;
}
