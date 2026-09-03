#include <bedrock/world/BedrockChunk.hpp>

#include <iostream>
#include <vector>

namespace {

bool strictDecoderRejects(const bedrock::BedrockLevelChunkPacket& packet) {
    try {
        (void) bedrock::BedrockLevelChunkCodec::decodeNoCacheColumn(packet);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
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
        v8Column.getSectionAtIndex(-4) == nullptr ||
        v8Column.getSectionAtIndex(-3) == nullptr ||
        v8Column.getSectionAtIndex(-4)->subChunkVersion() != 8 ||
        v8Column.getSectionAtIndex(-3)->subChunkVersion() != 8 ||
        v8Column.getSectionAtIndex(-4)->y() != -4 ||
        v8Column.getSectionAtIndex(-3)->y() != -3 ||
        v8Column.getBlockStateId({.x = 0, .y = -64, .z = 0}) != 333 ||
        v8Column.getBlockStateId({.x = 15, .y = -33, .z = 15}) != 444) {
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
