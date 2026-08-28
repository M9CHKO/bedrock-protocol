#include <bedrock/world/BedrockLegacyChunk.hpp>
#include <bedrock/world/MinecraftDataAssets.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int fail(const std::string& message) {
    std::cerr << "[BEDROCK-LEGACY-CHUNK-SMOKE] " << message << "\n";
    return 1;
}

uint32_t fnv1a(const std::vector<uint8_t>& data) {
    uint32_t hash = 0x811c9dc5u;
    for (const uint8_t byte : data) {
        hash ^= byte;
        hash *= 0x01000193u;
    }
    return hash;
}

} // namespace

int main() {
    bedrock::MinecraftDataAssets assets;
    const auto paths014 = assets.resolveByVersion("0.14");
    if (!paths014.blockStatesJson.empty() ||
        !paths014.blockCollisionShapesJson.empty()) {
        return fail("legacy optional block data paths must stay empty");
    }

    const auto blocks014 = assets.loadBedrockBlockRegistryByVersion("0.14");
    const auto blocks10 = assets.loadBedrockBlockRegistryByVersion("1.0");
    if (blocks014.blockCount() != 173 || blocks014.stateCount() != 2768 ||
        blocks10.blockCount() != 212 || blocks10.stateCount() != 3392) {
        return fail("legacy synthesized block-state registry size mismatch");
    }

    const auto registry014 = assets.loadBedrockRegistryByVersion("0.14");
    const auto registry10 = assets.loadBedrockRegistryByVersion("1.0");
    if (registry014.blocks().blockCount() != 173 ||
        registry014.items().itemCount() != 287 ||
        registry10.blocks().blockCount() != 212 ||
        registry10.items().itemCount() != 358 ||
        registry014.biomes().biomeCount() != 0 ||
        registry014.entities().entityCount() != 0) {
        return fail("legacy unified Bedrock registry mismatch");
    }

    const auto* stoneDefinition = blocks014.blockByName("stone");
    const auto granite = blocks014.fromStateId(17, 7);
    const auto stone10 = blocks10.fromStateId(17, 7);
    if (stoneDefinition == nullptr || stoneDefinition->minStateId != 16 ||
        stoneDefinition->maxStateId != 31 || stoneDefinition->defaultState != 16 ||
        stoneDefinition->drops != std::vector<uint32_t>({4}) ||
        stoneDefinition->variations.size() != 7 || !granite.has_value() ||
        granite->type != 1 || granite->metadata != 1 || granite->stateId != 17 ||
        granite->displayName != "Granite" || granite->biomeId != 7 ||
        !stone10.has_value() || stone10->displayName != "Stone") {
        return fail("legacy block metadata/variation parity mismatch");
    }

    const bedrock::BlockPosition first {.x = 1, .y = 2, .z = 3};
    const bedrock::BlockPosition second {.x = 2, .y = 2, .z = 3};

    bedrock::BedrockChunk014 chunk014;
    chunk014.setBlockType(first, 5);
    chunk014.setBlockData(first, 9);
    chunk014.setBlockLight(first, 10);
    chunk014.setSkyLight(first, 11);
    chunk014.setBiome(first, 200);
    chunk014.setBiomeColor(first, 0x12, 0x34, 0x56);
    chunk014.setHeight(first, 77);
    chunk014.setBlockType(second, 6);
    chunk014.setBlockData(second, 4);
    chunk014.setBlockLight(second, 3);
    chunk014.setSkyLight(second, 2);

    const auto dump014 = chunk014.dump();
    const auto color014 = chunk014.getBiomeColor(first);
    if (bedrock::BedrockChunk014::BufferSize != 83200 ||
        dump014.size() != bedrock::BedrockChunk014::BufferSize ||
        fnv1a(dump014) != 4229022984u || chunk014.getBlockType(first) != 5 ||
        chunk014.getBlockData(first) != 9 || chunk014.getBlockLight(first) != 10 ||
        chunk014.getSkyLight(first) != 11 || chunk014.getBiome(first) != -56 ||
        color014.r != 0x12 || color014.g != 0x34 || color014.b != 0x56 ||
        chunk014.getHeight(first) != 77 || chunk014.getBlockData(second) != 4 ||
        chunk014.getBlockLight(second) != 3 || chunk014.getSkyLight(second) != 2 ||
        chunk014.getMask() != 0xffffu) {
        return fail("Bedrock 0.14 byte layout differs from Node golden");
    }

    bedrock::BedrockChunk014 restored014;
    restored014.load(dump014);
    if (restored014.dump() != dump014 ||
        restored014.getBlockType({.x = 1, .y = 128, .z = 3}) != 0) {
        return fail("Bedrock 0.14 load/bounds mismatch");
    }
    bool bad014SizeRejected = false;
    try {
        restored014.load(std::vector<uint8_t>(10));
    } catch (const bedrock::BedrockChunkError&) {
        bad014SizeRejected = true;
    }
    if (!bad014SizeRejected) {
        return fail("Bedrock 0.14 accepted an invalid buffer size");
    }

    auto typedGranite = *granite;
    typedGranite.light = 6;
    typedGranite.skyLight = 13;
    typedGranite.biomeId = 4;
    restored014.setBlock(first, typedGranite);
    const auto readGranite = restored014.getBlock(first, blocks014);
    if (!readGranite.has_value() || readGranite->stateId != 17 ||
        readGranite->displayName != "Granite" || readGranite->light != 6 ||
        readGranite->skyLight != 13 || readGranite->biomeId != 4) {
        return fail("typed Bedrock 0.14 block access mismatch");
    }

    bedrock::BedrockChunk10 chunk10;
    chunk10.setBlockType(first, 5);
    chunk10.setBlockData(first, 9);
    chunk10.setBlockLight(first, 10);
    chunk10.setSkyLight(first, 11);
    chunk10.setBiome(first, 200);
    chunk10.setHeight(first, 77);
    chunk10.setBlockType(second, 6);
    chunk10.setBlockData(second, 4);
    chunk10.setBlockLight(second, 3);
    chunk10.setSkyLight(second, 2);

    const auto dump10 = chunk10.dump();
    if (bedrock::BedrockChunk10::BufferSize != 164627 ||
        chunk10.size() != 164627 || dump10.size() != 164627 || dump10[0] != 16 ||
        fnv1a(dump10) != 1767131590u || chunk10.getBlockType(first) != 5 ||
        chunk10.getBlockData(first) != 9 || chunk10.getBlockLight(first) != 10 ||
        chunk10.getSkyLight(first) != 11 || chunk10.getBiome(first) != 77 ||
        chunk10.getHeight(first) != 77 || chunk10.getBlockData(second) != 4 ||
        chunk10.getBlockLight(second) != 3 || chunk10.getSkyLight(second) != 2 ||
        chunk10.getMask() != 0xffffu) {
        return fail("Bedrock 1.0 byte layout differs from Node golden");
    }

    bedrock::BedrockChunk10 restored10;
    restored10.load(dump10);
    if (restored10.getBlockType(first) != 5 || restored10.getBlockData(first) != 9 ||
        restored10.getBlockLight(first) != 10 || restored10.getSkyLight(first) != 11 ||
        restored10.getBiome(first) != 1 || restored10.getHeight(first) != 1 ||
        fnv1a(restored10.dump()) != 1201211986u ||
        restored10.getBlockType({.x = 1, .y = -1, .z = 3}) != 0 ||
        restored10.getSkyLight({.x = 1, .y = 256, .z = 3}) != 15) {
        return fail("Bedrock 1.0 load/default behavior mismatch");
    }
    bool bad10SizeRejected = false;
    try {
        restored10.load(std::vector<uint8_t> {16});
    } catch (const bedrock::BedrockChunkError&) {
        bad10SizeRejected = true;
    }
    if (!bad10SizeRejected) {
        return fail("Bedrock 1.0 accepted a truncated buffer");
    }

    auto typedStone10 = *stone10;
    typedStone10.light = 7;
    typedStone10.skyLight = 12;
    typedStone10.biomeId = 9;
    restored10.setBlock(first, typedStone10);
    const auto readStone10 = restored10.getBlock(first, blocks10);
    if (!readStone10.has_value() || readStone10->stateId != 17 ||
        readStone10->light != 7 || readStone10->skyLight != 12 ||
        readStone10->biomeId != 9 || restored10.getHeight(first) != 9 ||
        static_cast<uint8_t>(bedrock::ChunkVersion::V0_9_00) != 0 ||
        static_cast<uint8_t>(bedrock::ChunkVersion::V1_16_200) != 21) {
        return fail("typed Bedrock 1.0 block or chunk-version mismatch");
    }

    std::cout << "[BEDROCK-LEGACY-CHUNK-SMOKE] ok\n";
    return 0;
}
