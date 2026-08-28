#include <bedrock/world/BedrockChunk.hpp>
#include <bedrock/world/SubChunk.hpp>
#include <bedrock/world/WorldScanner.hpp>
#include <bedrock/util/XxHash64.hpp>

#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void writeVarUInt(std::vector<uint8_t>& out, uint32_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7u;
        if (value != 0) byte |= 0x80;
        out.push_back(byte);
    } while (value != 0);
}

static void writeZigZagVarInt(std::vector<uint8_t>& out, int32_t value) {
    const uint32_t encoded =
        (static_cast<uint32_t>(value) << 1u) ^
        static_cast<uint32_t>(value >> 31);
    writeVarUInt(out, encoded);
}

static std::vector<uint8_t> fromHex(const std::string& value) {
    auto nibble = [](char ch) -> uint8_t {
        if (ch >= '0' && ch <= '9') return static_cast<uint8_t>(ch - '0');
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ch >= 'a' && ch <= 'f') return static_cast<uint8_t>(ch - 'a' + 10);
        throw std::runtime_error("invalid hex input");
    };
    if ((value.size() & 1u) != 0) throw std::runtime_error("odd hex input");
    std::vector<uint8_t> out;
    out.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((nibble(value[i]) << 4u) | nibble(value[i + 1])));
    }
    return out;
}

static void appendHex(std::vector<uint8_t>& out, const std::string& value) {
    const auto bytes = fromHex(value);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

static std::vector<uint8_t> persistentSubChunkGolden(
    bedrock::BedrockNbtEncoding encoding
) {
    std::vector<uint8_t> out {9, 1, 0xfe, 2};
    for (std::size_t word = 0; word < 128; ++word) {
        const uint32_t value = word == 9 ? 0x00040000u : 0u;
        out.push_back(static_cast<uint8_t>(value));
        out.push_back(static_cast<uint8_t>(value >> 8u));
        out.push_back(static_cast<uint8_t>(value >> 16u));
        out.push_back(static_cast<uint8_t>(value >> 24u));
    }

    if (encoding == bedrock::BedrockNbtEncoding::LittleEndian) {
        out.insert(out.end(), {2, 0, 0, 0});
        appendHex(out,
            "0a00000804006e616d650d006d696e6563726166743a6169720a060073746174657300"
            "03070076657273696f6e213c150100");
        appendHex(out,
            "0a00000804006e616d650f006d696e6563726166743a73746f6e650a06007374617465"
            "73080a0073746f6e655f7479706507006772616e697465010c00706f6c69736865645f"
            "62697401030300616765feffffff0003070076657273696f6e213c150100");
    } else {
        out.push_back(4);
        appendHex(out,
            "0a0008046e616d650d6d696e6563726166743a6169720a067374617465730003077665"
            "7273696f6ec2f0a91100");
        appendHex(out,
            "0a0008046e616d650f6d696e6563726166743a73746f6e650a06737461746573080a73"
            "746f6e655f74797065076772616e697465010c706f6c69736865645f62697401030361"
            "67650300030776657273696f6ec2f0a91100");
    }
    return out;
}

static bedrock::NbtValue stoneStates() {
    return bedrock::NbtValue::compound({
        {"stone_type", bedrock::NbtValue::string("granite")},
        {"polished_bit", bedrock::NbtValue::byte(1)},
        {"age", bedrock::NbtValue::integer(-2)}
    });
}

static bedrock::BedrockBlockStateProvider persistentProvider() {
    return [](int32_t stateId) -> std::optional<bedrock::BedrockBlockStateDescriptor> {
        if (stateId == 0) {
            return bedrock::BedrockBlockStateDescriptor {
                "minecraft:air",
                bedrock::NbtValue::compound(),
                18168865
            };
        }
        if (stateId == 77) {
            return bedrock::BedrockBlockStateDescriptor {
                "minecraft:stone",
                stoneStates(),
                18168865
            };
        }
        return std::nullopt;
    };
}

static std::optional<int32_t> resolvePersistentState(
    const std::string& name,
    const bedrock::NbtValue& states,
    int32_t version
) {
    if (version != 18168865) throw std::runtime_error("persistent version mismatch");
    if (name == "minecraft:air") return 0;
    if (name != "minecraft:stone") return std::nullopt;
    const auto* stoneType = states.find("stone_type");
    const auto* polished = states.find("polished_bit");
    const auto* age = states.find("age");
    if (stoneType == nullptr || stoneType->type != bedrock::NbtTagType::String ||
        stoneType->stringValue != "granite" || polished == nullptr ||
        polished->integerValue != 1 || age == nullptr || age->integerValue != -2) {
        throw std::runtime_error("persistent states mismatch");
    }
    return 77;
}

static bedrock::NbtDocument blockEntityDocument() {
    return {
        {},
        bedrock::NbtValue::compound({
            {"id", bedrock::NbtValue::string("Chest")},
            {"x", bedrock::NbtValue::integer(17)},
            {"y", bedrock::NbtValue::integer(-16)},
            {"z", bedrock::NbtValue::integer(-1)},
            {"CustomName", bedrock::NbtValue::string("Bedrock")}
        })
    };
}

static std::vector<uint8_t> blockEntityGolden(bedrock::BedrockNbtEncoding encoding) {
    return fromHex(encoding == bedrock::BedrockNbtEncoding::LittleEndian
        ? "0a0000080200696405004368657374030100781100000003010079f0ffffff0301007a"
          "ffffffff080a00437573746f6d4e616d650700426564726f636b00"
        : "0a0008026964054368657374030178220301791f03017a01080a437573746f6d4e616d"
          "6507426564726f636b00");
}

static bedrock::NbtDocument entityDocument(int64_t uniqueId = 123) {
    return {
        {},
        bedrock::NbtValue::compound({
            {"UniqueID", bedrock::NbtValue::longInteger(uniqueId)},
            {"identifier", bedrock::NbtValue::string("minecraft:pig")}
        })
    };
}

static std::vector<uint8_t> entityGolden() {
    // Generated by prismarine-nbt writeUncompressed(tag, 'little').
    return fromHex(
        "0a0000040800556e6971756549447b00000000000000080a006964656e746966696572"
        "0d006d696e6563726166743a70696700"
    );
}

static bool checkPersistentSubChunk(
    bedrock::ChunkStorageType storageType,
    bedrock::BedrockNbtEncoding nbtEncoding,
    const char* label
) {
    const auto golden = persistentSubChunkGolden(nbtEncoding);
    const auto decoded = bedrock::BedrockSubChunk::decode(
        storageType,
        golden,
        0,
        resolvePersistentState
    );
    if (decoded.y() != -2 || decoded.getBlockStateId(1, 2, 3) != 77 ||
        decoded.getBlockStateId(0, 0, 0) != 0 || decoded.palette().size() != 2 ||
        decoded.palette()[0].count != 4095 || decoded.palette()[1].count != 1 ||
        !decoded.palette()[1].hasPersistentData ||
        decoded.palette()[1].name != "minecraft:stone" ||
        !(decoded.palette()[1].states == stoneStates())) {
        std::cerr << "[CHUNK-SMOKE] " << label << " persistent decode mismatch\n";
        return false;
    }
    if (decoded.encode(storageType) != golden) {
        std::cerr << "[CHUNK-SMOKE] " << label << " persistent roundtrip mismatch\n";
        return false;
    }

    auto built = bedrock::BedrockSubChunk::createAir(-2, 0);
    built.setBlockStateId(1, 2, 3, 77);
    if (built.encode(storageType, true, persistentProvider()) != golden) {
        std::cerr << "[CHUNK-SMOKE] " << label << " provider encode mismatch\n";
        return false;
    }

    const auto scan = bedrock::SubChunkParser::scanWithStorages(golden, 1, nbtEncoding);
    if (scan.sections.size() != 1 || scan.sections[0].totalBytes != golden.size() ||
        scan.sections[0].storages.size() != 1 ||
        scan.sections[0].storages[0].persistentPalette.size() != 2) {
        std::cerr << "[CHUNK-SMOKE] " << label << " persistent scanner mismatch\n";
        return false;
    }
    const auto& blockTag = scan.sections[0].storages[0].getBlockPersistentState(306);
    const auto* blockName = blockTag.root.find("name");
    if (blockName == nullptr || blockName->stringValue != "minecraft:stone") {
        std::cerr << "[CHUNK-SMOKE] " << label << " persistent scanner block mismatch\n";
        return false;
    }
    return true;
}

int main() {
    if (bedrock::xxHash64(std::vector<uint8_t>{}) != 0xef46db3751d8e999ull ||
        bedrock::xxHash64(std::vector<uint8_t>{'h', 'e', 'l', 'l', 'o'}) !=
            0x26c7827d889f6da3ull) {
        std::cerr << "[CHUNK-SMOKE] xxHash64 Node golden mismatch\n";
        return 1;
    }

    const int32_t stateId = 42;

    std::vector<uint8_t> encoded;
    encoded.push_back(9); // subChunkVersion
    encoded.push_back(1); // storage count
    encoded.push_back(0); // y
    encoded.push_back(1); // palette type: runtime ids + bitsPerBlock 0
    writeZigZagVarInt(encoded, stateId);

    auto sub = bedrock::BedrockSubChunk::decode(
        bedrock::ChunkStorageType::Runtime,
        encoded
    );

    if (sub.getBlockStateId(0, 0, 0) != stateId) {
        std::cerr << "[CHUNK-SMOKE] single-state block mismatch\n";
        return 1;
    }
    if (sub.palette(0).size() != 1) {
        std::cerr << "[CHUNK-SMOKE] single-state palette size mismatch\n";
        return 1;
    }
    if (sub.getPaletteEntry(0, 15, 15, 15).stateId != stateId ||
        sub.getPalette().size() != 1 || sub.getPalette()[0].stateId != stateId) {
        std::cerr << "[CHUNK-SMOKE] JS-compatible palette access mismatch\n";
        return 1;
    }
    auto paletteApiSub = sub;
    paletteApiSub.setBlockStateId(1, 2, 3, 77);
    if (paletteApiSub.getPaletteEntry(0, 1, 2, 3).stateId != 77 ||
        paletteApiSub.getPalette().size() != 2) {
        std::cerr << "[CHUNK-SMOKE] palette entry insertion mismatch\n";
        return 1;
    }
    paletteApiSub.setBlockStateId(1, 2, 3, stateId);
    if (paletteApiSub.palette().size() != 2 || paletteApiSub.getPalette().size() != 1) {
        std::cerr << "[CHUNK-SMOKE] inactive palette filtering mismatch\n";
        return 1;
    }

    auto dumped = sub.encode(bedrock::ChunkStorageType::Runtime);
    if (dumped != encoded) {
        std::cerr << "[CHUNK-SMOKE] single-state roundtrip mismatch\n";
        return 1;
    }

    const auto singleStateScan = bedrock::SubChunkParser::scanWithStorages(encoded, 1);
    if (singleStateScan.sections.size() != 1 ||
        singleStateScan.sections[0].storages[0].getBlockRuntimeId(4095) != stateId) {
        std::cerr << "[CHUNK-SMOKE] single-state scanner mismatch\n";
        return 1;
    }

    bedrock::PalettedStorage threeBitStorage(3);
    threeBitStorage.set(0, 10, 0, 7);
    bedrock::BinaryStream threeBitBytes;
    threeBitBytes.writeU8(7); // runtime palette, 3 bits per block
    threeBitStorage.write(threeBitBytes);
    threeBitBytes.writeVarUInt(16); // zigzag(8 palette entries)
    for (int32_t runtimeId = 100; runtimeId < 108; ++runtimeId) {
        threeBitBytes.writeVarUInt(static_cast<uint32_t>(runtimeId << 1));
    }
    const auto threeBitScan = bedrock::PalettedStorageParser::scanAt(threeBitBytes.buffer(), 0);
    if (threeBitScan.wordCount != 410 || threeBitScan.getBlockRuntimeId(10) != 107 ||
        threeBitScan.getBlockRuntimeId(9) != 100) {
        std::cerr << "[CHUNK-SMOKE] non-divisor bit-width scanner mismatch\n";
        return 1;
    }

    bedrock::PalettedStorage indexedStorage(1);
    indexedStorage.set(1, 2, 3, 1);
    bedrock::PalettedStorageInfo indexedInfo;
    indexedInfo.header = {.raw = 3, .bitsPerBlock = 1, .runtime = true};
    indexedInfo.words = indexedStorage.words();
    indexedInfo.runtimePalette = {0, 77};
    indexedInfo.paletteCount = 2;
    bedrock::SubChunkSection indexedSection;
    indexedSection.version = 9;
    indexedSection.storageCount = 1;
    indexedSection.yIndex = -2;
    indexedSection.storages.push_back(std::move(indexedInfo));
    bedrock::LevelChunkView indexedChunk;
    indexedChunk.header.chunkX = 2;
    indexedChunk.header.chunkZ = -3;
    indexedChunk.subChunks.sections.push_back(std::move(indexedSection));
    bedrock::WorldView indexedWorld;
    indexedWorld.put(std::move(indexedChunk));
    if (indexedWorld.getRuntimeIdAt(33, -30, -45) != 77) {
        std::cerr << "[CHUNK-SMOKE] negative-y world view lookup mismatch\n";
        return 1;
    }
    const auto foundIndexed = bedrock::WorldScanner(indexedWorld)
        .findFirstRuntimeIdInChunk(2, -3, 77);
    if (!foundIndexed.has_value() || foundIndexed->x != 33 ||
        foundIndexed->y != -30 || foundIndexed->z != -45) {
        std::cerr << "[CHUNK-SMOKE] world scanner coordinate mismatch\n";
        return 1;
    }

    if (!checkPersistentSubChunk(
            bedrock::ChunkStorageType::LocalPersistence,
            bedrock::BedrockNbtEncoding::LittleEndian,
            "local")) {
        return 1;
    }
    if (!checkPersistentSubChunk(
            bedrock::ChunkStorageType::NetworkPersistence,
            bedrock::BedrockNbtEncoding::LittleVarInt,
            "network")) {
        return 1;
    }

    bedrock::BedrockChunkColumn column(3, 5);
    column.setBlockStateId({.x = 17, .y = 32, .z = -1}, 77);
    if (column.getBlockStateId({.x = 1, .y = 32, .z = 15}) != 77) {
        std::cerr << "[CHUNK-SMOKE] column local coordinate mismatch\n";
        return 1;
    }
    if (column.getSectionAtIndex(2) == nullptr ||
        column.getSectionAtIndex(2) != column.getSection(32) ||
        column.getSectionAtIndex(-1) != nullptr) {
        std::cerr << "[CHUNK-SMOKE] section index lookup mismatch\n";
        return 1;
    }
    const auto blocks = column.getBlocks();
    const auto& constColumn = column;
    if (blocks.size() != 2 || blocks[0].stateId != 0 || blocks[1].stateId != 77 ||
        &column.getSections() != &constColumn.getSections() ||
        &constColumn.getSections() != &constColumn.sections()) {
        std::cerr << "[CHUNK-SMOKE] CommonChunkColumn block/section API mismatch\n";
        return 1;
    }
    column.setBlockLight({.x = 17, .y = 32, .z = -1}, 12);
    column.setSkyLight({.x = 17, .y = 32, .z = -1}, 15);
    if (column.getBlockLight({.x = 1, .y = 32, .z = 15}) != 12 ||
        column.getSkyLight({.x = 1, .y = 32, .z = 15}) != 15) {
        std::cerr << "[CHUNK-SMOKE] column light lookup mismatch\n";
        return 1;
    }
    try {
        bedrock::BedrockChunkColumn emptyLightColumn;
        (void) emptyLightColumn.getBlockLight({.x = 0, .y = 0, .z = 0});
        std::cerr << "[CHUNK-SMOKE] missing light section was accepted\n";
        return 1;
    } catch (const bedrock::BedrockChunkError&) {
    }

    bedrock::BedrockHeightMap heights {};
    heights[0] = 0x1234;
    heights[255] = 0xabcd;
    column.loadHeights(heights);
    bedrock::BinaryStream heightStream;
    column.writeHeightMap(heightStream);
    const auto* loadedHeights = column.getHeights();
    if (loadedHeights == nullptr || (*loadedHeights)[0] != 0x1234 ||
        (*loadedHeights)[255] != 0xabcd || heightStream.buffer().size() != 512 ||
        heightStream.buffer()[0] != 0x34 || heightStream.buffer()[1] != 0x12 ||
        heightStream.buffer()[510] != 0xcd || heightStream.buffer()[511] != 0xab) {
        std::cerr << "[CHUNK-SMOKE] heightmap little-endian encoding mismatch\n";
        return 1;
    }
    bedrock::BedrockChunkColumn zeroHeightColumn;
    bedrock::BinaryStream zeroHeightStream;
    zeroHeightColumn.writeHeightMap(zeroHeightStream);
    if (zeroHeightColumn.getHeights() == nullptr || zeroHeightStream.buffer().size() != 512 ||
        zeroHeightStream.buffer()[127] != 0) {
        std::cerr << "[CHUNK-SMOKE] default heightmap mismatch\n";
        return 1;
    }

    bedrock::BedrockBiomeSection biome(0);
    biome.setBiomeId(1, 2, 3, 7);
    bedrock::BinaryStream biomeStream;
    biome.exportTo(bedrock::ChunkStorageType::Runtime, biomeStream);
    bedrock::BinaryStream biomeRead(biomeStream.buffer());
    bedrock::BedrockBiomeSection decodedBiome(0);
    decodedBiome.read(bedrock::ChunkStorageType::Runtime, biomeRead);
    if (decodedBiome.getBiomeId(1, 2, 3) != 7) {
        std::cerr << "[CHUNK-SMOKE] biome section roundtrip mismatch\n";
        return 1;
    }

    bedrock::BedrockChunkColumn netColumn(0, 0);
    netColumn.setBounds(-4, 20);
    netColumn.setBlockStateId({.x = 4, .y = -16, .z = 5}, 123);
    netColumn.setBiomeId({.x = 4, .y = -16, .z = 5}, 9);
    netColumn.setBlockEntityNbt({.x = 17, .y = -16, .z = -1}, blockEntityDocument());
    auto payload = netColumn.networkEncodeNoCache();

    bedrock::BedrockChunkColumn decodedColumn(0, 0);
    decodedColumn.setBounds(-4, 20);
    decodedColumn.networkDecodeNoCache(payload, 1);
    if (decodedColumn.getBlockStateId({.x = 4, .y = -16, .z = 5}) != 123) {
        std::cerr << "[CHUNK-SMOKE] network no-cache block mismatch\n";
        return 1;
    }
    if (decodedColumn.getBiomeId({.x = 4, .y = -16, .z = 5}) != 9) {
        std::cerr << "[CHUNK-SMOKE] network no-cache biome mismatch\n";
        return 1;
    }
    const auto* decodedBlockEntity = decodedColumn.getBlockEntity({.x = 1, .y = -16, .z = 15});
    const auto* decodedBlockEntityNbt = decodedColumn.getBlockEntityNbt({.x = 1, .y = -16, .z = 15});
    if (decodedBlockEntity == nullptr ||
        *decodedBlockEntity != blockEntityGolden(bedrock::BedrockNbtEncoding::LittleVarInt) ||
        decodedBlockEntityNbt == nullptr || !(*decodedBlockEntityNbt == blockEntityDocument())) {
        std::cerr << "[CHUNK-SMOKE] network block entity mismatch\n";
        return 1;
    }

    const auto diskBlockEntities = netColumn.diskEncodeBlockEntities();
    if (diskBlockEntities != blockEntityGolden(bedrock::BedrockNbtEncoding::LittleEndian)) {
        std::cerr << "[CHUNK-SMOKE] disk block entity golden mismatch\n";
        return 1;
    }
    bedrock::BedrockChunkColumn diskDecodedColumn(0, 0);
    diskDecodedColumn.diskDecodeBlockEntities(diskBlockEntities);
    if (diskDecodedColumn.blockEntityCount() != 1 ||
        diskDecodedColumn.getBlockEntityNbt({.x = 1, .y = -16, .z = 15}) == nullptr) {
        std::cerr << "[CHUNK-SMOKE] disk block entity decode mismatch\n";
        return 1;
    }

    bedrock::BedrockChunkColumn metadataColumn(0, 0);
    metadataColumn.addBlockEntity(blockEntityDocument());
    const auto sectionBlockEntities = metadataColumn.getSectionBlockEntities(-1);
    if (metadataColumn.blockEntityCount() != 1 || sectionBlockEntities.size() != 1 ||
        !(sectionBlockEntities[0] == blockEntityDocument()) ||
        !metadataColumn.getSectionBlockEntities(0).empty() ||
        metadataColumn.getBlockEntityNbt({.x = 1, .y = -16, .z = 15}) == nullptr ||
        !metadataColumn.moveBlockEntity(
            {.x = 1, .y = -16, .z = 15},
            {.x = 2, .y = 0, .z = 14}
        ) ||
        metadataColumn.getBlockEntityNbt({.x = 1, .y = -16, .z = 15}) != nullptr ||
        metadataColumn.getBlockEntityNbt({.x = 2, .y = 0, .z = 14}) == nullptr ||
        !metadataColumn.getSectionBlockEntities(-1).empty() ||
        metadataColumn.getSectionBlockEntities(0).size() != 1 ||
        metadataColumn.moveBlockEntity(
            {.x = 3, .y = 0, .z = 3},
            {.x = 4, .y = 0, .z = 4}
        )) {
        std::cerr << "[CHUNK-SMOKE] block entity add/move mismatch\n";
        return 1;
    }

    metadataColumn.addEntity(entityDocument());
    const auto entityIt = metadataColumn.getEntities().find("123");
    if (metadataColumn.entityCount() != 1 ||
        entityIt == metadataColumn.getEntities().end() ||
        !(entityIt->second == entityDocument()) ||
        metadataColumn.diskEncodeEntities() != entityGolden()) {
        std::cerr << "[CHUNK-SMOKE] disk entity encode mismatch\n";
        return 1;
    }
    bedrock::BedrockChunkColumn decodedEntityColumn;
    decodedEntityColumn.diskDecodeEntities(entityGolden());
    if (decodedEntityColumn.entityCount() != 1 ||
        decodedEntityColumn.getEntities().find("123") ==
            decodedEntityColumn.getEntities().end()) {
        std::cerr << "[CHUNK-SMOKE] disk entity decode mismatch\n";
        return 1;
    }
    decodedEntityColumn.loadEntities(
        std::vector<bedrock::NbtDocument> {entityDocument(-42)}
    );
    if (decodedEntityColumn.entityCount() != 1 ||
        decodedEntityColumn.getEntities().find("-42") ==
            decodedEntityColumn.getEntities().end() ||
        !decodedEntityColumn.removeEntity("-42") ||
        decodedEntityColumn.removeEntity("-42")) {
        std::cerr << "[CHUNK-SMOKE] entity load/remove mismatch\n";
        return 1;
    }

    const auto subChunkPayload = netColumn.networkEncodeSubChunkNoCache(-1);
    bedrock::BedrockChunkColumn decodedSubChunkColumn(0, 0);
    decodedSubChunkColumn.setBounds(-4, 20);
    decodedSubChunkColumn.networkDecodeSubChunkNoCache(-1, subChunkPayload);
    if (decodedSubChunkColumn.getBlockStateId({.x = 4, .y = -16, .z = 5}) != 123 ||
        decodedSubChunkColumn.blockEntityCount() != 1) {
        std::cerr << "[CHUNK-SMOKE] standalone subchunk decode mismatch\n";
        return 1;
    }

    bedrock::BedrockChunkColumn legacyColumn(2, 3);
    legacyColumn.setBlockStateId({.x = 1, .y = 2, .z = 3}, 55);
    legacyColumn.ensureSection(2).setSubChunkVersion(8);
    legacyColumn.setBiomeId({.x = 1, .y = 0, .z = 3}, 6);
    legacyColumn.setBlockEntityNbt({.x = 17, .y = -16, .z = -1}, blockEntityDocument());
    const auto legacyPayload = legacyColumn.networkEncodeNoCache(false);
    bedrock::BedrockChunkColumn decodedLegacyColumn(2, 3);
    decodedLegacyColumn.networkDecodeNoCache(legacyPayload, 1, false);
    if (decodedLegacyColumn.getBlockStateId({.x = 1, .y = 2, .z = 3}) != 55 ||
        decodedLegacyColumn.getBiomeId({.x = 1, .y = 47, .z = 3}) != 6 ||
        decodedLegacyColumn.blockEntityCount() != 1) {
        std::cerr << "[CHUNK-SMOKE] legacy 2D biome column mismatch\n";
        return 1;
    }

    bedrock::BedrockChunkColumn cachedEncodeColumn(4, 5);
    auto cachedEncodeSection = bedrock::BedrockSubChunk::createAir(0, 0);
    cachedEncodeSection.setSubChunkVersion(8);
    cachedEncodeSection.setBlockStateId(1, 2, 3, 77);
    cachedEncodeColumn.setSection(0, std::move(cachedEncodeSection));
    cachedEncodeColumn.setBiomeId({.x = 1, .y = 0, .z = 3}, 6);
    cachedEncodeColumn.setBlockEntityNbt(
        {.x = 17, .y = -16, .z = -1},
        blockEntityDocument()
    );
    bedrock::BedrockBlobStore encodedCacheStore;
    const auto encodedCache = cachedEncodeColumn.networkEncodeCached(
        encodedCacheStore,
        false,
        persistentProvider()
    );
    if (encodedCache.blobHashes.size() != 2 || encodedCacheStore.size() != 2) {
        std::cerr << "[CHUNK-SMOKE] cached column encode blob count mismatch\n";
        return 1;
    }
    for (const auto encodedHash : encodedCache.blobHashes) {
        const auto* encodedBlob = encodedCacheStore.get(encodedHash);
        if (encodedBlob == nullptr || bedrock::xxHash64(encodedBlob->buffer) != encodedHash) {
            std::cerr << "[CHUNK-SMOKE] cached column xxHash mismatch\n";
            return 1;
        }
    }
    bedrock::BedrockChunkColumn decodedEncodedCache(4, 5);
    const auto encodedCacheMisses = decodedEncodedCache.networkDecodeCached(
        encodedCache.blobHashes,
        encodedCacheStore,
        encodedCache.payload,
        resolvePersistentState
    );
    if (!encodedCacheMisses.empty() ||
        decodedEncodedCache.getBlockStateId({.x = 1, .y = 2, .z = 3}) != 77 ||
        decodedEncodedCache.getBiomeId({.x = 1, .y = 47, .z = 3}) != 6 ||
        decodedEncodedCache.blockEntityCount() != 1) {
        std::cerr << "[CHUNK-SMOKE] cached column encode/decode mismatch\n";
        return 1;
    }

    bedrock::BedrockBlobStore encodedSubChunkStore;
    const auto encodedSubChunk = netColumn.networkEncodeSubChunkCached(
        -1,
        encodedSubChunkStore
    );
    const auto* encodedSubChunkBlob = encodedSubChunkStore.get(encodedSubChunk.blobHash);
    if (encodedSubChunkBlob == nullptr ||
        bedrock::xxHash64(encodedSubChunkBlob->buffer) != encodedSubChunk.blobHash ||
        encodedSubChunk.payload != blockEntityGolden(
            bedrock::BedrockNbtEncoding::LittleVarInt
        )) {
        std::cerr << "[CHUNK-SMOKE] cached subchunk encode mismatch\n";
        return 1;
    }
    std::vector<uint8_t> combinedCachedSubChunk = encodedSubChunkBlob->buffer;
    combinedCachedSubChunk.insert(
        combinedCachedSubChunk.end(),
        encodedSubChunk.payload.begin(),
        encodedSubChunk.payload.end()
    );
    bedrock::BedrockChunkColumn decodedCachedSubChunk(0, 0);
    decodedCachedSubChunk.setBounds(-4, 20);
    decodedCachedSubChunk.networkDecodeSubChunkNoCache(-1, combinedCachedSubChunk);
    if (decodedCachedSubChunk.getBlockStateId({.x = 4, .y = -16, .z = 5}) != 123 ||
        decodedCachedSubChunk.blockEntityCount() != 1) {
        std::cerr << "[CHUNK-SMOKE] cached subchunk decode mismatch\n";
        return 1;
    }

    bedrock::BedrockBlobStore cachedStore;
    const uint64_t sectionHash = 0x1111222233334444ull;
    const uint64_t biomeHash = 0x5555666677778888ull;
    bedrock::BlobEntry cachedSection;
    cachedSection.x = 0;
    cachedSection.y = -2;
    cachedSection.z = 0;
    cachedSection.type = bedrock::BlobType::ChunkSection;
    cachedSection.buffer = persistentSubChunkGolden(bedrock::BedrockNbtEncoding::LittleVarInt);
    cachedStore.set(sectionHash, std::move(cachedSection));
    bedrock::BlobEntry cachedBiomes;
    cachedBiomes.type = bedrock::BlobType::Biomes;
    cachedBiomes.buffer.assign(256, 5);
    cachedStore.set(biomeHash, std::move(cachedBiomes));

    std::vector<uint8_t> cachedPayload {0};
    const auto networkBlockEntity = blockEntityGolden(bedrock::BedrockNbtEncoding::LittleVarInt);
    cachedPayload.insert(cachedPayload.end(), networkBlockEntity.begin(), networkBlockEntity.end());
    bedrock::BedrockChunkColumn cachedColumn(0, 0);
    cachedColumn.setBounds(-4, 20);
    const auto cachedMisses = cachedColumn.networkDecodeCached(
        {sectionHash, biomeHash},
        cachedStore,
        cachedPayload,
        resolvePersistentState
    );
    if (!cachedMisses.empty() ||
        cachedColumn.getBlockStateId({.x = 1, .y = -30, .z = 3}) != 77 ||
        cachedColumn.getBiomeId({.x = 1, .y = -30, .z = 3}) != 5 ||
        cachedColumn.blockEntityCount() != 1) {
        std::cerr << "[CHUNK-SMOKE] cached persistent column mismatch: misses="
                  << cachedMisses.size() << " state="
                  << cachedColumn.getBlockStateId({.x = 1, .y = -30, .z = 3})
                  << " biome="
                  << cachedColumn.getBiomeId({.x = 1, .y = -30, .z = 3})
                  << " entities=" << cachedColumn.blockEntityCount() << '\n';
        return 1;
    }

    auto levelPacket = bedrock::BedrockLevelChunkCodec::encodeNoCacheColumn(netColumn, 0);
    auto packetPayload = bedrock::BedrockLevelChunkCodec::encodePacketPayload(levelPacket);
    auto decodedPacket = bedrock::BedrockLevelChunkCodec::decodePacketPayload(packetPayload);
    auto decodedPacketColumn = bedrock::BedrockLevelChunkCodec::decodeNoCacheColumn(decodedPacket);
    if (decodedPacket.x != 0 || decodedPacket.z != 0 || decodedPacket.cacheEnabled) {
        std::cerr << "[CHUNK-SMOKE] level_chunk packet metadata mismatch\n";
        return 1;
    }
    if (decodedPacketColumn.getBlockStateId({.x = 4, .y = -16, .z = 5}) != 123) {
        std::cerr << "[CHUNK-SMOKE] level_chunk packet column mismatch\n";
        return 1;
    }

    netColumn.setBlockLight({.x = 4, .y = -16, .z = 5}, 9);
    netColumn.setSkyLight({.x = 4, .y = -16, .z = 5}, 14);
    bedrock::BedrockWorld world;
    world.setLoadedColumn(0, 0, netColumn);
    if (world.getBlockStateId({.x = 4, .y = -16, .z = 5}) != 123 ||
        world.getBlockLight({.x = 4, .y = -16, .z = 5}) != 9 ||
        world.getSkyLight({.x = 4, .y = -16, .z = 5}) != 14) {
        std::cerr << "[CHUNK-SMOKE] world block/light lookup mismatch\n";
        return 1;
    }
    world.setBlockLight({.x = 4, .y = -16, .z = 5}, 7);
    world.setSkyLight({.x = 4, .y = -16, .z = 5}, 11);
    world.setBlockLight({.x = 160, .y = 0, .z = 160}, 6);
    world.setSkyLight({.x = 160, .y = 0, .z = 160}, 6);
    if (world.getBlockLight({.x = 4, .y = -16, .z = 5}) != 7 ||
        world.getSkyLight({.x = 4, .y = -16, .z = 5}) != 11 ||
        world.getBlockLight({.x = 160, .y = 0, .z = 160}) != 0 ||
        world.getSkyLight({.x = 160, .y = 0, .z = 160}) != 0) {
        std::cerr << "[CHUNK-SMOKE] world light mutation mismatch\n";
        return 1;
    }
    bedrock::BedrockChunkColumn negativeColumn(-1, -1);
    negativeColumn.setBounds(-4, 20);
    negativeColumn.setBlockStateId({.x = -1, .y = 0, .z = -1}, 88);
    world.setLoadedColumn(-1, -1, negativeColumn);
    if (world.getBlockStateId({.x = -1, .y = 0, .z = -1}) != 88) {
        std::cerr << "[CHUNK-SMOKE] world negative coordinate mismatch\n";
        return 1;
    }

    bedrock::BedrockClientCacheBlobStatus cacheStatus;
    cacheStatus.missing = {0x1122334455667788ull};
    cacheStatus.have = {0x8877665544332211ull};
    auto cacheStatusPayload = bedrock::BedrockLevelChunkCodec::encodeClientCacheBlobStatusPayload(cacheStatus);
    if (cacheStatusPayload.empty()) {
        std::cerr << "[CHUNK-SMOKE] cache status payload empty\n";
        return 1;
    }

    std::vector<uint8_t> missPayload;
    writeVarUInt(missPayload, 1);
    const uint64_t hash = 0x0102030405060708ull;
    for (int i = 0; i < 8; ++i) {
        missPayload.push_back(static_cast<uint8_t>((hash >> (8 * i)) & 0xff));
    }
    writeVarUInt(missPayload, 3);
    missPayload.push_back(1);
    missPayload.push_back(2);
    missPayload.push_back(3);
    auto blobs = bedrock::BedrockLevelChunkCodec::decodeClientCacheMissResponsePayload(missPayload);
    if (blobs.size() != 1 || blobs[0].hash != hash || blobs[0].payload.size() != 3) {
        std::cerr << "[CHUNK-SMOKE] cache miss response mismatch\n";
        return 1;
    }

    std::cout << "[CHUNK-SMOKE] ok\n";
    return 0;
}
