#include <bedrock/relay/LevelChunkRetentionCache.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[LEVEL-CHUNK-RETENTION-CACHE-SMOKE] " << message << "\n";
    }
    return condition;
}

void writeZigZag32(std::vector<uint8_t>& out, int32_t value) {
    const uint32_t raw =
        (static_cast<uint32_t>(value) << 1u) ^
        static_cast<uint32_t>(value >> 31);
    bedrock::VersionedPacketCodec::writeVarUInt(out, raw);
}

bedrock::VersionedGamePacket makeLevelChunk(
    int32_t dimension,
    int32_t x,
    int32_t z,
    uint8_t marker
) {
    std::vector<uint8_t> payload;
    writeZigZag32(payload, x);
    writeZigZag32(payload, z);
    writeZigZag32(payload, dimension);
    // The cache only needs the stable header coordinates. These bytes stand
    // in for the remaining version-specific level_chunk body.
    payload.insert(payload.end(), {1, 0, 1, marker});

    bedrock::VersionedGamePacket packet;
    packet.packetId = 58;
    packet.name = "level_chunk";
    packet.paramsType = "packet_level_chunk";
    packet.payload = payload;
    bedrock::VersionedPacketCodec::writeVarUInt(packet.fullPacket, packet.packetId);
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

    bedrock::LevelChunkRetentionCache bounded(64, 3);
    bounded.configure(true, 128);
    for (int i = 0; i < 100; ++i) bounded.observeLevelChunk(makeLevelChunk(0, i, 0, 1));
    ok &= check(bounded.stats().residentChunks == 3, "tiny packets escaped count budget");
    auto oversized = makeLevelChunk(0, 97, 0, 1);
    oversized.fullPacket.resize(65);
    ok &= check(!bounded.observeLevelChunk(oversized).stored, "oversized packet retained");
    ok &= check(!bounded.contains(0, 97, 0) && bounded.contains(0, 98, 0) &&
        bounded.contains(0, 99, 0), "oversized replacement removed unrelated data or kept stale bytes");
    ok &= check(bounded.stats().skippedOversized == 1, "oversized count missing");
    oversized.fullPacket.clear();
    oversized.payload.resize(65);
    ok &= check(!bounded.observeLevelChunk(oversized).stored, "payload-only packet escaped byte budget");
    bounded.resetSession();
    ok &= check(bounded.stats().skippedOversized == 0 && bounded.stats().residentBytes == 0,
        "session reset did not clear cache counters");

    bedrock::LevelChunkRetentionCache cache(1024);
    const auto ignored = cache.observeLevelChunk(makeLevelChunk(0, 0, 0, 1));
    ok &= check(ignored.recognized, "disabled cache did not recognize level_chunk");
    ok &= check(!ignored.stored, "disabled cache retained a chunk");

    cache.configure(true, 2);
    (void) cache.updatePublisherWindow(0, 0, 32);

    const auto first = makeLevelChunk(0, 1, -2, 0x11);
    const auto stored = cache.observeLevelChunk(first);
    ok &= check(stored.stored && !stored.replaced, "first chunk was not stored");
    ok &= check(cache.contains(0, 1, -2), "stored chunk key is missing");
    ok &= check(
        cache.packetBytes(0, 1, -2) == first.fullPacket,
        "cache did not preserve exact full level_chunk bytes"
    );

    const auto replacement = makeLevelChunk(0, 1, -2, 0x22);
    const auto replaced = cache.observeLevelChunk(replacement);
    ok &= check(replaced.replaced, "newer chunk did not replace the old value");
    ok &= check(
        cache.packetBytes(0, 1, -2) == replacement.fullPacket,
        "replacement bytes were not retained"
    );

    ok &= check(
        cache.observeLevelChunk(makeLevelChunk(0, 2, 2, 3)).stored,
        "chunk on configured radius boundary was rejected"
    );
    ok &= check(
        !cache.observeLevelChunk(makeLevelChunk(0, 3, 0, 4)).stored,
        "chunk beyond configured radius was retained"
    );

    const auto moved = cache.updatePublisherWindow(160, 0, 32);
    ok &= check(
        moved.evictedOutsideRadius == 2,
        "moving publisher did not evict both old chunks"
    );
    ok &= check(cache.stats().residentChunks == 0, "radius eviction left entries");

    // A block coordinate of -1 belongs to chunk -1, not chunk 0.
    cache.configure(true, 1);
    (void) cache.updatePublisherWindow(-1, -1, 16);
    ok &= check(
        cache.observeLevelChunk(makeLevelChunk(0, -2, -2, 5)).stored,
        "negative publisher coordinate was rounded toward zero"
    );
    ok &= check(
        !cache.observeLevelChunk(makeLevelChunk(0, 1, -1, 6)).stored,
        "negative publisher window accepted a chunk two columns away"
    );

    const auto dimensionChange = cache.observeLevelChunk(
        makeLevelChunk(2, -1, -1, 7)
    );
    ok &= check(dimensionChange.dimensionChanged, "dimension change was not detected");
    ok &= check(
        !cache.contains(0, -2, -2) && cache.contains(2, -1, -1),
        "dimension change did not replace the old world's cache"
    );

    const auto onePacket = makeLevelChunk(0, 0, 0, 8);
    bedrock::LevelChunkRetentionCache memoryBounded(onePacket.fullPacket.size());
    memoryBounded.configure(true, 8);
    (void) memoryBounded.updatePublisherWindow(0, 0, 128);
    (void) memoryBounded.observeLevelChunk(onePacket);
    const auto memoryUpdate = memoryBounded.observeLevelChunk(
        makeLevelChunk(0, 1, 0, 9)
    );
    ok &= check(memoryUpdate.evictedForMemory == 1, "memory cap did not evict");
    ok &= check(
        memoryBounded.stats().residentBytes <= onePacket.fullPacket.size(),
        "resident bytes exceeded memory cap"
    );
    ok &= check(
        memoryBounded.contains(0, 0, 0) && !memoryBounded.contains(0, 1, 0),
        "memory eviction did not prefer the farthest chunk"
    );

    cache.configure(false, 2);
    const auto disabledStats = cache.stats();
    ok &= check(
        disabledStats.residentChunks == 0 && disabledStats.residentBytes == 0,
        "disabling retention did not release cached chunk memory"
    );

    if (!ok) return 1;
    std::cout << "level chunk retention cache smoke: OK\n";
    return 0;
}
