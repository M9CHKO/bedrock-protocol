#pragma once

#include <bedrock/BinaryStream.hpp>
#include <bedrock/nbt/BedrockNbt.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace bedrock {

enum class ChunkStorageType {
    LocalPersistence = 0,
    NetworkPersistence = 1,
    Runtime = 2
};

enum class ChunkVersion : uint8_t {
    V1_16_210 = 22,
    V1_17_0 = 25,
    V1_17_30 = 29,
    V1_17_40 = 31,
    V1_18_0 = 39
};

struct BlockPosition {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    std::optional<uint8_t> layer;
};

struct BedrockBlockState {
    int32_t stateId = 0;
    std::string name;
    uint32_t count = 0;
    NbtValue states = NbtValue::compound();
    int32_t version = 0;
    bool hasPersistentData = false;
};

struct BedrockBlockStateDescriptor {
    std::string name;
    NbtValue states = NbtValue::compound();
    int32_t version = 0;
};

using BedrockBlockStateResolver = std::function<std::optional<int32_t>(
    const std::string& name,
    const NbtValue& states,
    int32_t version
)>;

using BedrockBlockStateProvider = std::function<std::optional<BedrockBlockStateDescriptor>(
    int32_t stateId
)>;

enum class BlobType : uint8_t {
    ChunkSection = 0,
    Biomes = 1
};

struct BlobEntry {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    BlobType type = BlobType::ChunkSection;
    std::vector<uint8_t> buffer;
};

class BedrockChunkError : public std::runtime_error {
public:
    explicit BedrockChunkError(const std::string& message)
        : std::runtime_error(message) {}
};

class PalettedStorage {
public:
    static constexpr uint32_t StorageSize = 4096;

    explicit PalettedStorage(uint8_t bitsPerBlock = 1);

    uint8_t bitsPerBlock() const;
    uint32_t blocksPerWord() const;
    uint32_t wordsCount() const;

    uint32_t get(uint8_t x, uint8_t y, uint8_t z) const;
    void set(uint8_t x, uint8_t y, uint8_t z, uint32_t value);
    PalettedStorage resize(uint8_t newBitsPerBlock) const;

    void read(BinaryStream& stream);
    void write(BinaryStream& stream) const;

    const std::vector<uint32_t>& words() const;
    std::vector<uint32_t>& words();

    void incrementPalette(std::vector<BedrockBlockState>& palette) const;

private:
    uint8_t bitsPerBlock_ = 1;
    uint32_t blocksPerWord_ = 32;
    uint32_t wordsCount_ = 128;
    uint32_t mask_ = 1;
    std::vector<uint32_t> words_;

    static std::pair<uint32_t, uint8_t> storageIndex(
        uint8_t x,
        uint8_t y,
        uint8_t z,
        uint32_t blocksPerWord,
        uint8_t bitsPerBlock
    );
};

class BedrockSubChunk {
public:
    explicit BedrockSubChunk(int8_t y = 0, uint8_t subChunkVersion = 9);

    static BedrockSubChunk createAir(int8_t y = 0, int32_t airStateId = 0);
    static BedrockSubChunk decode(
        ChunkStorageType format,
        const std::vector<uint8_t>& data,
        int32_t airStateId = 0,
        BedrockBlockStateResolver resolver = {}
    );

    std::vector<uint8_t> encode(
        ChunkStorageType format = ChunkStorageType::Runtime,
        bool compact = true,
        BedrockBlockStateProvider provider = {}
    ) const;

    void decodeFrom(
        ChunkStorageType format,
        BinaryStream& stream,
        int32_t airStateId = 0,
        BedrockBlockStateResolver resolver = {}
    );
    void encodeTo(
        ChunkStorageType format,
        BinaryStream& stream,
        bool compact = true,
        BedrockBlockStateProvider provider = {}
    ) const;

    int8_t y() const;
    void setY(int8_t y);
    uint8_t subChunkVersion() const;
    void setSubChunkVersion(uint8_t version);

    std::size_t layerCount() const;
    bool hasLayer(uint8_t layer) const;

    int32_t getBlockStateId(uint8_t layer, uint8_t x, uint8_t y, uint8_t z) const;
    int32_t getBlockStateId(uint8_t x, uint8_t y, uint8_t z) const;
    void setBlockStateId(uint8_t layer, uint8_t x, uint8_t y, uint8_t z, int32_t stateId);
    void setBlockStateId(uint8_t x, uint8_t y, uint8_t z, int32_t stateId);

    uint8_t getBlockLight(uint8_t x, uint8_t y, uint8_t z) const;
    void setBlockLight(uint8_t x, uint8_t y, uint8_t z, uint8_t value);
    uint8_t getSkyLight(uint8_t x, uint8_t y, uint8_t z) const;
    void setSkyLight(uint8_t x, uint8_t y, uint8_t z, uint8_t value);

    const std::vector<BedrockBlockState>& palette(uint8_t layer = 0) const;
    void setPaletteEntryDescriptor(
        uint8_t layer,
        std::size_t paletteIndex,
        BedrockBlockStateDescriptor descriptor
    );
    std::vector<BedrockBlockState> compactedPalette(uint8_t layer = 0) const;
    bool isCompactable(uint8_t layer = 0) const;
    void compact(uint8_t layer = 0);

private:
    int8_t y_ = 0;
    uint8_t subChunkVersion_ = 9;
    int32_t airStateId_ = 0;
    std::vector<std::vector<BedrockBlockState>> palettes_;
    std::vector<PalettedStorage> blocks_;
    PalettedStorage blockLight_ {4};
    PalettedStorage skyLight_ {4};

    void ensureLayer(uint8_t layer);
    void addToPalette(uint8_t layer, int32_t stateId, uint32_t count = 0);
    void loadPalettedBlocks(
        uint8_t layer,
        BinaryStream& stream,
        uint8_t bitsPerBlock,
        ChunkStorageType format,
        const BedrockBlockStateResolver& resolver
    );
    void writeStorage(
        uint8_t layer,
        BinaryStream& stream,
        ChunkStorageType format,
        const BedrockBlockStateProvider& provider
    ) const;

    static uint8_t neededBits(uint32_t value);
    static int32_t readZigZagVarInt(BinaryStream& stream);
    static int32_t readRuntimeSingleStateId(BinaryStream& stream);
    static void writeZigZagVarInt(BinaryStream& stream, int32_t value);
};

class BedrockBiomeSection {
public:
    explicit BedrockBiomeSection(int32_t y = 0);

    static BedrockBiomeSection proxyToPrevious(int32_t y);

    int32_t y() const;
    bool proxy() const;
    void setProxy(bool proxy);

    uint32_t getBiomeId(uint8_t x, uint8_t y, uint8_t z) const;
    void setBiomeId(uint8_t x, uint8_t y, uint8_t z, uint32_t biomeId);

    void readLegacy2D(BinaryStream& stream);
    void exportLegacy2D(BinaryStream& stream) const;

    void read(ChunkStorageType type, BinaryStream& stream);
    void exportTo(ChunkStorageType type, BinaryStream& stream) const;

    const std::vector<uint32_t>& palette() const;

private:
    int32_t y_ = 0;
    bool proxy_ = false;
    PalettedStorage biomes_ {1};
    std::vector<uint32_t> palette_ {0};

    void ensureBiome(uint32_t biomeId);
    static uint8_t neededBits(uint32_t value);
    static uint32_t readRuntimeBiomeId(BinaryStream& stream);
    static void writeRuntimeBiomeId(BinaryStream& stream, uint32_t value);
};

class BedrockBlobStore {
public:
    bool has(uint64_t hash) const;
    const BlobEntry* get(uint64_t hash) const;
    void set(uint64_t hash, BlobEntry entry);
    void erase(uint64_t hash);
    std::size_t size() const;

private:
    std::unordered_map<uint64_t, BlobEntry> entries_;
};

class BedrockChunkColumn;

struct BedrockLevelChunkPacket {
    int32_t x = 0;
    int32_t z = 0;
    int32_t dimension = 0;
    int32_t subChunkCount = 0;
    std::optional<uint16_t> highestSubChunkCount;
    bool cacheEnabled = false;
    std::vector<uint64_t> blobHashes;
    std::vector<uint8_t> payload;
};

struct BedrockCacheBlob {
    uint64_t hash = 0;
    std::vector<uint8_t> payload;
};

struct BedrockClientCacheBlobStatus {
    std::vector<uint64_t> missing;
    std::vector<uint64_t> have;
};

struct BedrockEncodedChunkCache {
    std::vector<uint64_t> blobHashes;
    std::vector<uint8_t> payload;
};

struct BedrockEncodedSubChunkCache {
    uint64_t blobHash = 0;
    std::vector<uint8_t> payload;
};

class BedrockLevelChunkCodec {
public:
    static BedrockLevelChunkPacket decodePacketPayload(const std::vector<uint8_t>& payload);
    static std::vector<uint8_t> encodePacketPayload(const BedrockLevelChunkPacket& packet);

    static BedrockChunkColumn decodeNoCacheColumn(
        const BedrockLevelChunkPacket& packet,
        bool useCavesAndCliffsBounds = true
    );

    static BedrockLevelChunkPacket encodeNoCacheColumn(
        const BedrockChunkColumn& column,
        int32_t dimension = 0
    );

    static std::vector<BedrockCacheBlob> decodeClientCacheMissResponsePayload(
        const std::vector<uint8_t>& payload
    );
    static std::vector<uint8_t> encodeClientCacheBlobStatusPayload(
        const BedrockClientCacheBlobStatus& status
    );
};

class BedrockChunkColumn {
public:
    explicit BedrockChunkColumn(int32_t x = 0, int32_t z = 0);

    void setBounds(int32_t minCY, int32_t maxCY);

    int32_t x() const;
    int32_t z() const;
    int32_t minCY() const;
    int32_t maxCY() const;
    int32_t minY() const;
    int32_t maxY() const;
    int32_t worldHeight() const;

    BedrockSubChunk* getSection(int32_t blockY);
    const BedrockSubChunk* getSection(int32_t blockY) const;
    BedrockSubChunk& ensureSection(int32_t blockY);
    BedrockSubChunk& newSection(int32_t sectionY);
    void setSection(int32_t sectionY, BedrockSubChunk section);

    int32_t getBlockStateId(const BlockPosition& pos) const;
    void setBlockStateId(const BlockPosition& pos, int32_t stateId);

    void setBlockEntity(const BlockPosition& pos, std::vector<uint8_t> tag);
    void setBlockEntityNbt(const BlockPosition& pos, NbtDocument tag);
    const std::vector<uint8_t>* getBlockEntity(const BlockPosition& pos) const;
    const NbtDocument* getBlockEntityNbt(const BlockPosition& pos) const;
    void removeBlockEntity(const BlockPosition& pos);
    std::size_t blockEntityCount() const;

    std::vector<uint8_t> diskEncodeBlockEntities() const;
    void diskDecodeBlockEntities(const std::vector<uint8_t>& data);

    const std::vector<std::optional<BedrockSubChunk>>& sections() const;

    uint32_t getBiomeId(const BlockPosition& pos) const;
    void setBiomeId(const BlockPosition& pos, uint32_t biomeId);

    void loadLegacyBiomes(const std::vector<uint8_t>& data);
    std::vector<uint8_t> dumpLegacyBiomes() const;
    void loadBiomes(BinaryStream& stream, ChunkStorageType type);
    void writeBiomes(BinaryStream& stream) const;

    void networkDecodeNoCache(const std::vector<uint8_t>& payload, int32_t sectionCount);
    void networkDecodeNoCache(
        const std::vector<uint8_t>& payload,
        int32_t sectionCount,
        bool use3DBiomes
    );
    std::vector<uint8_t> networkEncodeNoCache() const;
    std::vector<uint8_t> networkEncodeNoCache(bool use3DBiomes) const;
    void networkDecodeSubChunkNoCache(int32_t sectionY, const std::vector<uint8_t>& payload);
    std::vector<uint8_t> networkEncodeSubChunkNoCache(
        int32_t sectionY,
        bool compact = true
    ) const;
    BedrockEncodedChunkCache networkEncodeCached(
        BedrockBlobStore& blobStore,
        bool use3DBiomes,
        BedrockBlockStateProvider provider = {}
    ) const;
    BedrockEncodedChunkCache networkEncodeCached(
        BedrockBlobStore& blobStore,
        BedrockBlockStateProvider provider = {}
    ) const;
    BedrockEncodedSubChunkCache networkEncodeSubChunkCached(
        int32_t sectionY,
        BedrockBlobStore& blobStore,
        bool compact = true
    ) const;
    std::vector<uint64_t> networkDecodeCached(
        const std::vector<uint64_t>& blobHashes,
        const BedrockBlobStore& blobStore,
        const std::vector<uint8_t>& payload,
        BedrockBlockStateResolver resolver = {}
    );

private:
    int32_t x_ = 0;
    int32_t z_ = 0;
    int32_t minCY_ = 0;
    int32_t maxCY_ = 16;
    int32_t minY_ = 0;
    int32_t maxY_ = 256;
    int32_t worldHeight_ = 256;
    int32_t co_ = 0;
    std::vector<std::optional<BedrockSubChunk>> sections_;
    std::vector<BedrockBiomeSection> biomes_;
    std::unordered_map<std::string, std::vector<uint8_t>> blockEntities_;
    std::unordered_map<std::string, NbtDocument> blockEntityNbt_;

    int32_t sectionIndexForBlockY(int32_t y) const;
    void decodeBlockEntities(BinaryStream& stream, BedrockNbtEncoding encoding);
    void encodeBlockEntities(
        BinaryStream& stream,
        BedrockNbtEncoding encoding,
        std::optional<int32_t> sectionY = std::nullopt
    ) const;
    static BlockPosition blockEntityPosition(const NbtDocument& tag);
    static uint8_t localCoord(int32_t value);
    static std::string blockEntityKey(const BlockPosition& pos);
};

struct BedrockWorldColumnEntry {
    int32_t chunkX = 0;
    int32_t chunkZ = 0;
    BedrockChunkColumn column;
};

class BedrockWorld {
public:
    using ColumnHandler = std::function<void(const BedrockWorldColumnEntry&)>;

    void onColumnLoad(ColumnHandler handler);
    void onColumnUnload(ColumnHandler handler);

    bool hasColumn(int32_t chunkX, int32_t chunkZ) const;
    BedrockChunkColumn* getLoadedColumn(int32_t chunkX, int32_t chunkZ);
    const BedrockChunkColumn* getLoadedColumn(int32_t chunkX, int32_t chunkZ) const;
    BedrockChunkColumn* getLoadedColumnAt(const BlockPosition& pos);
    const BedrockChunkColumn* getLoadedColumnAt(const BlockPosition& pos) const;

    void setLoadedColumn(int32_t chunkX, int32_t chunkZ, BedrockChunkColumn column);
    void unloadColumn(int32_t chunkX, int32_t chunkZ);
    std::vector<BedrockWorldColumnEntry> getColumns() const;

    int32_t getBlockStateId(const BlockPosition& pos) const;
    void setBlockStateId(const BlockPosition& pos, int32_t stateId);
    uint32_t getBiomeId(const BlockPosition& pos) const;
    void setBiomeId(const BlockPosition& pos, uint32_t biomeId);

    std::size_t columnCount() const;

private:
    std::unordered_map<std::string, BedrockChunkColumn> columns_;
    std::vector<ColumnHandler> loadHandlers_;
    std::vector<ColumnHandler> unloadHandlers_;

    static std::string key(int32_t chunkX, int32_t chunkZ);
    static int32_t chunkCoord(int32_t blockCoord);
};

} // namespace bedrock
