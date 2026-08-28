#include <bedrock/world/BedrockChunk.hpp>
#include <bedrock/util/XxHash64.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace bedrock {
namespace {

uint8_t checkedBitsPerBlock(uint8_t bitsPerBlock) {
    if (bitsPerBlock > 32) {
        throw BedrockChunkError("bitsPerBlock must be <= 32");
    }
    return bitsPerBlock == 0 ? 1 : bitsPerBlock;
}

BedrockNbtEncoding persistenceNbtEncoding(ChunkStorageType format) {
    return format == ChunkStorageType::LocalPersistence
        ? BedrockNbtEncoding::LittleEndian
        : BedrockNbtEncoding::LittleVarInt;
}

} // namespace

PalettedStorage::PalettedStorage(uint8_t bitsPerBlock)
    : bitsPerBlock_(checkedBitsPerBlock(bitsPerBlock)),
      blocksPerWord_(32u / bitsPerBlock_),
      wordsCount_((StorageSize + blocksPerWord_ - 1u) / blocksPerWord_),
      mask_(bitsPerBlock_ >= 32 ? 0xffffffffu : ((1u << bitsPerBlock_) - 1u)),
      words_(wordsCount_, 0) {}

uint8_t PalettedStorage::bitsPerBlock() const {
    return bitsPerBlock_;
}

uint32_t PalettedStorage::blocksPerWord() const {
    return blocksPerWord_;
}

uint32_t PalettedStorage::wordsCount() const {
    return wordsCount_;
}

uint32_t PalettedStorage::get(uint8_t x, uint8_t y, uint8_t z) const {
    auto [wordIndex, offset] = storageIndex(x, y, z, blocksPerWord_, bitsPerBlock_);
    if (wordIndex >= words_.size()) {
        throw BedrockChunkError("PalettedStorage get out of range");
    }
    return (words_[wordIndex] >> offset) & mask_;
}

void PalettedStorage::set(uint8_t x, uint8_t y, uint8_t z, uint32_t value) {
    auto [wordIndex, offset] = storageIndex(x, y, z, blocksPerWord_, bitsPerBlock_);
    if (wordIndex >= words_.size()) {
        throw BedrockChunkError("PalettedStorage set out of range");
    }
    words_[wordIndex] &= ~(mask_ << offset);
    words_[wordIndex] |= (value & mask_) << offset;
}

PalettedStorage PalettedStorage::resize(uint8_t newBitsPerBlock) const {
    PalettedStorage next(newBitsPerBlock);
    for (uint8_t x = 0; x < 16; ++x) {
        for (uint8_t y = 0; y < 16; ++y) {
            for (uint8_t z = 0; z < 16; ++z) {
                next.set(x, y, z, get(x, y, z));
            }
        }
    }
    return next;
}

void PalettedStorage::read(BinaryStream& stream) {
    for (auto& word : words_) {
        word = stream.readU32LE();
    }
}

void PalettedStorage::write(BinaryStream& stream) const {
    for (auto word : words_) {
        stream.writeU32LE(word);
    }
}

const std::vector<uint32_t>& PalettedStorage::words() const {
    return words_;
}

std::vector<uint32_t>& PalettedStorage::words() {
    return words_;
}

void PalettedStorage::incrementPalette(std::vector<BedrockBlockState>& palette) const {
    for (uint32_t i = 0; i < StorageSize; ++i) {
        const uint32_t wordIndex = i / blocksPerWord_;
        const uint8_t offset = static_cast<uint8_t>((i % blocksPerWord_) * bitsPerBlock_);
        const uint32_t paletteIndex = (words_[wordIndex] >> offset) & mask_;
        if (paletteIndex >= palette.size()) {
            throw BedrockChunkError("paletted storage index exceeds palette size");
        }
        palette[paletteIndex].count++;
    }
}

std::pair<uint32_t, uint8_t> PalettedStorage::storageIndex(
    uint8_t x,
    uint8_t y,
    uint8_t z,
    uint32_t blocksPerWord,
    uint8_t bitsPerBlock
) {
    x &= 0x0f;
    y &= 0x0f;
    z &= 0x0f;
    const uint32_t blockIndex = (static_cast<uint32_t>(x) << 8u) |
        (static_cast<uint32_t>(z) << 4u) |
        static_cast<uint32_t>(y);
    return {
        blockIndex / blocksPerWord,
        static_cast<uint8_t>((blockIndex % blocksPerWord) * bitsPerBlock)
    };
}

BedrockSubChunk::BedrockSubChunk(int8_t y, uint8_t subChunkVersion)
    : y_(y),
      subChunkVersion_(subChunkVersion) {}

BedrockSubChunk BedrockSubChunk::createAir(int8_t y, int32_t airStateId) {
    BedrockSubChunk out(y, 9);
    out.airStateId_ = airStateId;
    out.ensureLayer(0);
    out.palettes_[0].clear();
    out.blocks_[0] = PalettedStorage(1);
    out.palettes_[0].push_back({airStateId, "minecraft:air", PalettedStorage::StorageSize});
    return out;
}

BedrockSubChunk BedrockSubChunk::decode(
    ChunkStorageType format,
    const std::vector<uint8_t>& data,
    int32_t airStateId,
    BedrockBlockStateResolver resolver
) {
    BinaryStream stream(data);
    BedrockSubChunk sub;
    sub.decodeFrom(format, stream, airStateId, std::move(resolver));
    return sub;
}

std::vector<uint8_t> BedrockSubChunk::encode(
    ChunkStorageType format,
    bool compactStorage,
    BedrockBlockStateProvider provider
) const {
    BinaryStream stream;
    encodeTo(format, stream, compactStorage, std::move(provider));
    return stream.buffer();
}

void BedrockSubChunk::decodeFrom(
    ChunkStorageType format,
    BinaryStream& stream,
    int32_t airStateId,
    BedrockBlockStateResolver resolver
) {
    airStateId_ = airStateId;
    palettes_.clear();
    blocks_.clear();

    subChunkVersion_ = stream.readU8();
    uint8_t storageCount = 1;

    switch (subChunkVersion_) {
        case 1:
            subChunkVersion_ = 8;
            break;
        case 8:
        case 9:
            storageCount = stream.readU8();
            if (subChunkVersion_ >= 9) {
                y_ = static_cast<int8_t>(stream.readU8());
            }
            break;
        default:
            throw BedrockChunkError("unsupported sub chunk version: " + std::to_string(subChunkVersion_));
    }

    if (storageCount == 0 || storageCount > 2) {
        throw BedrockChunkError("expected storage count 1 or 2, got " + std::to_string(storageCount));
    }

    for (uint8_t layer = 0; layer < storageCount; ++layer) {
        const uint8_t paletteType = stream.readU8();
        const bool runtimeIds = (paletteType & 1u) != 0;
        if (!runtimeIds && format == ChunkStorageType::Runtime) {
            throw BedrockChunkError("expected runtime palette while decoding subchunk");
        }
        const uint8_t bitsPerBlock = static_cast<uint8_t>(paletteType >> 1u);
        loadPalettedBlocks(layer, stream, bitsPerBlock, format, resolver);
    }
}

void BedrockSubChunk::encodeTo(
    ChunkStorageType format,
    BinaryStream& stream,
    bool compactStorage,
    BedrockBlockStateProvider provider
) const {
    BedrockSubChunk copy = *this;
    stream.writeU8(copy.subChunkVersion_);
    stream.writeU8(static_cast<uint8_t>(copy.blocks_.size()));
    if (copy.subChunkVersion_ >= 9) {
        stream.writeU8(static_cast<uint8_t>(copy.y_));
    }
    for (uint8_t layer = 0; layer < copy.blocks_.size(); ++layer) {
        if (compactStorage) {
            copy.compact(layer);
        }
        copy.writeStorage(layer, stream, format, provider);
    }
}

int8_t BedrockSubChunk::y() const {
    return y_;
}

void BedrockSubChunk::setY(int8_t y) {
    y_ = y;
}

uint8_t BedrockSubChunk::subChunkVersion() const {
    return subChunkVersion_;
}

void BedrockSubChunk::setSubChunkVersion(uint8_t version) {
    subChunkVersion_ = version;
}

std::size_t BedrockSubChunk::layerCount() const {
    return blocks_.size();
}

bool BedrockSubChunk::hasLayer(uint8_t layer) const {
    return layer < blocks_.size();
}

int32_t BedrockSubChunk::getBlockStateId(uint8_t layer, uint8_t x, uint8_t y, uint8_t z) const {
    if (layer >= blocks_.size()) {
        return airStateId_;
    }
    const uint32_t paletteIndex = blocks_[layer].get(x, y, z);
    if (paletteIndex >= palettes_[layer].size()) {
        throw BedrockChunkError("subchunk palette index out of range");
    }
    return palettes_[layer][paletteIndex].stateId;
}

int32_t BedrockSubChunk::getBlockStateId(uint8_t x, uint8_t y, uint8_t z) const {
    return getBlockStateId(0, x, y, z);
}

void BedrockSubChunk::setBlockStateId(uint8_t layer, uint8_t x, uint8_t y, uint8_t z, int32_t stateId) {
    ensureLayer(layer);
    const uint32_t currentIndex = blocks_[layer].get(x, y, z);
    auto& current = palettes_[layer][currentIndex];
    if (current.stateId == stateId) {
        return;
    }
    if (current.count > 0) {
        current.count--;
    }

    for (uint32_t i = 0; i < palettes_[layer].size(); ++i) {
        auto& entry = palettes_[layer][i];
        if (entry.stateId == stateId) {
            entry.count = std::max(entry.count, 0u) + 1u;
            blocks_[layer].set(x, y, z, i);
            return;
        }
    }

    addToPalette(layer, stateId, 1);
    blocks_[layer].set(x, y, z, static_cast<uint32_t>(palettes_[layer].size() - 1));
}

void BedrockSubChunk::setBlockStateId(uint8_t x, uint8_t y, uint8_t z, int32_t stateId) {
    setBlockStateId(0, x, y, z, stateId);
}

uint8_t BedrockSubChunk::getBlockLight(uint8_t x, uint8_t y, uint8_t z) const {
    return static_cast<uint8_t>(blockLight_.get(x, y, z));
}

void BedrockSubChunk::setBlockLight(uint8_t x, uint8_t y, uint8_t z, uint8_t value) {
    blockLight_.set(x, y, z, value & 0x0f);
}

uint8_t BedrockSubChunk::getSkyLight(uint8_t x, uint8_t y, uint8_t z) const {
    return static_cast<uint8_t>(skyLight_.get(x, y, z));
}

void BedrockSubChunk::setSkyLight(uint8_t x, uint8_t y, uint8_t z, uint8_t value) {
    skyLight_.set(x, y, z, value & 0x0f);
}

const BedrockBlockState& BedrockSubChunk::getPaletteEntry(
    uint8_t layer,
    uint8_t x,
    uint8_t y,
    uint8_t z
) const {
    if (layer >= blocks_.size()) {
        throw BedrockChunkError("palette layer out of range");
    }
    const uint32_t paletteIndex = blocks_[layer].get(x, y, z);
    if (paletteIndex >= palettes_[layer].size()) {
        throw BedrockChunkError("subchunk palette index out of range");
    }
    return palettes_[layer][paletteIndex];
}

std::vector<BedrockBlockState> BedrockSubChunk::getPalette(uint8_t layer) const {
    return compactedPalette(layer);
}

const std::vector<BedrockBlockState>& BedrockSubChunk::palette(uint8_t layer) const {
    if (layer >= palettes_.size()) {
        throw BedrockChunkError("palette layer out of range");
    }
    return palettes_[layer];
}

void BedrockSubChunk::setPaletteEntryDescriptor(
    uint8_t layer,
    std::size_t paletteIndex,
    BedrockBlockStateDescriptor descriptor
) {
    if (layer >= palettes_.size() || paletteIndex >= palettes_[layer].size()) {
        throw BedrockChunkError("palette entry out of range");
    }
    if (descriptor.name.empty()) {
        throw BedrockChunkError("persistent block-state name must not be empty");
    }
    if (descriptor.states.type != NbtTagType::Compound) {
        throw BedrockChunkError("persistent block-state states must be an NBT compound");
    }

    auto& entry = palettes_[layer][paletteIndex];
    entry.name = std::move(descriptor.name);
    entry.states = std::move(descriptor.states);
    entry.version = descriptor.version;
    entry.hasPersistentData = true;
}

std::vector<BedrockBlockState> BedrockSubChunk::compactedPalette(uint8_t layer) const {
    if (layer >= palettes_.size()) {
        return {};
    }
    std::vector<BedrockBlockState> out;
    for (const auto& block : palettes_[layer]) {
        if (block.count > 0) {
            out.push_back(block);
        }
    }
    return out;
}

bool BedrockSubChunk::isCompactable(uint8_t layer) const {
    if (layer >= palettes_.size()) {
        return false;
    }
    uint32_t used = 0;
    for (const auto& block : palettes_[layer]) {
        if (block.count > 0) {
            used++;
        }
    }
    return used < palettes_[layer].size();
}

void BedrockSubChunk::compact(uint8_t layer) {
    if (!isCompactable(layer)) {
        return;
    }

    std::vector<BedrockBlockState> newPalette;
    std::vector<uint32_t> remap;
    remap.reserve(palettes_[layer].size());

    for (const auto& block : palettes_[layer]) {
        if (block.count > 0) {
            newPalette.push_back(block);
        }
        remap.push_back(static_cast<uint32_t>(newPalette.empty() ? 0 : newPalette.size() - 1));
    }

    PalettedStorage newStorage(std::max<uint8_t>(1, neededBits(static_cast<uint32_t>(newPalette.size() - 1))));
    for (uint8_t x = 0; x < 16; ++x) {
        for (uint8_t y = 0; y < 16; ++y) {
            for (uint8_t z = 0; z < 16; ++z) {
                const uint32_t oldIndex = blocks_[layer].get(x, y, z);
                newStorage.set(x, y, z, oldIndex < remap.size() ? remap[oldIndex] : 0);
            }
        }
    }

    palettes_[layer] = std::move(newPalette);
    blocks_[layer] = std::move(newStorage);
}

void BedrockSubChunk::ensureLayer(uint8_t layer) {
    while (palettes_.size() <= layer) {
        palettes_.push_back({});
        blocks_.push_back(PalettedStorage(4));
        addToPalette(static_cast<uint8_t>(palettes_.size() - 1), airStateId_, PalettedStorage::StorageSize);
    }
}

void BedrockSubChunk::addToPalette(uint8_t layer, int32_t stateId, uint32_t count) {
    while (palettes_.size() <= layer) {
        palettes_.push_back({});
        blocks_.push_back(PalettedStorage(4));
    }
    palettes_[layer].push_back({stateId, stateId == airStateId_ ? "minecraft:air" : "", count});
    const uint8_t minBits = neededBits(static_cast<uint32_t>(palettes_[layer].size() - 1));
    if (minBits > blocks_[layer].bitsPerBlock()) {
        blocks_[layer] = blocks_[layer].resize(minBits);
    }
}

void BedrockSubChunk::loadPalettedBlocks(
    uint8_t layer,
    BinaryStream& stream,
    uint8_t bitsPerBlock,
    ChunkStorageType format,
    const BedrockBlockStateResolver& resolver
) {
    if (format == ChunkStorageType::Runtime && bitsPerBlock == 0) {
        while (palettes_.size() <= layer) {
            palettes_.push_back({});
            blocks_.push_back(PalettedStorage(1));
        }
        palettes_[layer].clear();
        blocks_[layer] = PalettedStorage(1);
        const int32_t stateId = readRuntimeSingleStateId(stream);
        addToPalette(layer, stateId, PalettedStorage::StorageSize);
        return;
    }

    while (palettes_.size() <= layer) {
        palettes_.push_back({});
        blocks_.push_back(PalettedStorage(std::max<uint8_t>(1, bitsPerBlock)));
    }

    blocks_[layer] = PalettedStorage(std::max<uint8_t>(1, bitsPerBlock));
    blocks_[layer].read(stream);

    int64_t paletteSizeValue = 0;
    if (format == ChunkStorageType::LocalPersistence) {
        paletteSizeValue = stream.readU32LE();
    } else {
        paletteSizeValue = readZigZagVarInt(stream);
    }

    if (paletteSizeValue < 1 || paletteSizeValue > PalettedStorage::StorageSize) {
        throw BedrockChunkError("invalid subchunk palette size: " + std::to_string(paletteSizeValue));
    }
    const auto paletteSize = static_cast<uint32_t>(paletteSizeValue);
    if (bitsPerBlock < 32 && paletteSize > (1u << std::max<uint8_t>(1, bitsPerBlock))) {
        throw BedrockChunkError("subchunk palette does not fit bitsPerBlock");
    }

    palettes_[layer].clear();
    palettes_[layer].reserve(paletteSize);

    if (format == ChunkStorageType::Runtime) {
        for (uint32_t i = 0; i < paletteSize; ++i) {
            const int32_t stateId = readZigZagVarInt(stream);
            palettes_[layer].push_back({stateId, stateId == airStateId_ ? "minecraft:air" : "", 0});
        }
    } else {
        const auto nbtEncoding = persistenceNbtEncoding(format);
        for (uint32_t i = 0; i < paletteSize; ++i) {
            NbtDocument document;
            try {
                document = BedrockNbtCodec::read(stream, nbtEncoding);
            } catch (const std::exception& error) {
                throw BedrockChunkError(
                    "invalid persistent block-state NBT at palette index " +
                    std::to_string(i) + ": " + error.what()
                );
            }

            if (document.root.type != NbtTagType::Compound) {
                throw BedrockChunkError("persistent block-state NBT root must be a compound");
            }
            const NbtValue* nameTag = document.root.find("name");
            const NbtValue* statesTag = document.root.find("states");
            const NbtValue* versionTag = document.root.find("version");
            if (nameTag == nullptr || nameTag->type != NbtTagType::String) {
                throw BedrockChunkError("persistent block-state NBT is missing string name");
            }
            if (statesTag != nullptr && statesTag->type != NbtTagType::Compound) {
                throw BedrockChunkError("persistent block-state states must be an NBT compound");
            }
            if (versionTag == nullptr || versionTag->type != NbtTagType::Int ||
                versionTag->integerValue < std::numeric_limits<int32_t>::min() ||
                versionTag->integerValue > std::numeric_limits<int32_t>::max()) {
                throw BedrockChunkError("persistent block-state NBT is missing int version");
            }

            NbtValue states = statesTag == nullptr
                ? NbtValue::compound()
                : *statesTag;
            const int32_t version = static_cast<int32_t>(versionTag->integerValue);
            int32_t stateId = airStateId_;
            if (resolver) {
                if (const auto resolved = resolver(nameTag->stringValue, states, version)) {
                    stateId = *resolved;
                }
            }

            BedrockBlockState entry;
            entry.stateId = stateId;
            entry.name = nameTag->stringValue;
            entry.states = std::move(states);
            entry.version = version;
            entry.hasPersistentData = true;
            palettes_[layer].push_back(std::move(entry));
        }
    }
    blocks_[layer].incrementPalette(palettes_[layer]);
}

void BedrockSubChunk::writeStorage(
    uint8_t layer,
    BinaryStream& stream,
    ChunkStorageType format,
    const BedrockBlockStateProvider& provider
) const {
    if (layer >= blocks_.size() || layer >= palettes_.size()) {
        throw BedrockChunkError("writeStorage layer out of range");
    }
    if (palettes_[layer].empty() || palettes_[layer].size() > PalettedStorage::StorageSize) {
        throw BedrockChunkError("invalid palette size while encoding subchunk");
    }

    if (format == ChunkStorageType::Runtime && palettes_[layer].size() == 1) {
        stream.writeU8(1);
        writeZigZagVarInt(stream, palettes_[layer][0].stateId);
        return;
    }

    uint8_t paletteType = static_cast<uint8_t>(blocks_[layer].bitsPerBlock() << 1u);
    if (format == ChunkStorageType::Runtime) {
        paletteType |= 1u;
    }
    stream.writeU8(paletteType);
    blocks_[layer].write(stream);

    if (format == ChunkStorageType::LocalPersistence) {
        stream.writeU32LE(static_cast<uint32_t>(palettes_[layer].size()));
    } else {
        writeZigZagVarInt(stream, static_cast<int32_t>(palettes_[layer].size()));
    }

    if (format == ChunkStorageType::Runtime) {
        for (const auto& block : palettes_[layer]) {
            writeZigZagVarInt(stream, block.stateId);
        }
        return;
    }

    const auto nbtEncoding = persistenceNbtEncoding(format);
    for (const auto& block : palettes_[layer]) {
        BedrockBlockStateDescriptor descriptor;
        if (block.hasPersistentData) {
            descriptor.name = block.name;
            descriptor.states = block.states;
            descriptor.version = block.version;
        } else if (provider) {
            const auto supplied = provider(block.stateId);
            if (!supplied.has_value()) {
                throw BedrockChunkError(
                    "no persistent descriptor for block state " + std::to_string(block.stateId)
                );
            }
            descriptor = *supplied;
        } else {
            throw BedrockChunkError(
                "persistent subchunk encoding requires block-state descriptors; missing state " +
                std::to_string(block.stateId)
            );
        }

        if (descriptor.name.empty()) {
            throw BedrockChunkError("persistent block-state name must not be empty");
        }
        if (descriptor.states.type != NbtTagType::Compound) {
            throw BedrockChunkError("persistent block-state states must be an NBT compound");
        }

        NbtDocument document {
            {},
            NbtValue::compound({
                {"name", NbtValue::string(descriptor.name)},
                {"states", descriptor.states},
                {"version", NbtValue::integer(descriptor.version)}
            })
        };
        BedrockNbtCodec::write(stream, document, nbtEncoding);
    }
}

uint8_t BedrockSubChunk::neededBits(uint32_t value) {
    if (value == 0) {
        return 0;
    }
    uint8_t bits = 0;
    while (value != 0) {
        bits++;
        value >>= 1u;
    }
    return bits;
}

int32_t BedrockSubChunk::readZigZagVarInt(BinaryStream& stream) {
    const uint32_t value = stream.readVarUInt();
    return static_cast<int32_t>((value >> 1u) ^ (0u - (value & 1u)));
}

int32_t BedrockSubChunk::readRuntimeSingleStateId(BinaryStream& stream) {
    return static_cast<int32_t>(stream.readVarUInt() >> 1u);
}

void BedrockSubChunk::writeZigZagVarInt(BinaryStream& stream, int32_t value) {
    const uint32_t encoded =
        (static_cast<uint32_t>(value) << 1u) ^
        static_cast<uint32_t>(value >> 31);
    stream.writeVarUInt(encoded);
}

BedrockBiomeSection::BedrockBiomeSection(int32_t y)
    : y_(y) {}

BedrockBiomeSection BedrockBiomeSection::proxyToPrevious(int32_t y) {
    BedrockBiomeSection section(y);
    section.proxy_ = true;
    return section;
}

int32_t BedrockBiomeSection::y() const {
    return y_;
}

bool BedrockBiomeSection::proxy() const {
    return proxy_;
}

void BedrockBiomeSection::setProxy(bool proxy) {
    proxy_ = proxy;
}

uint32_t BedrockBiomeSection::getBiomeId(uint8_t x, uint8_t y, uint8_t z) const {
    if (palette_.empty()) {
        return 0;
    }
    const uint32_t index = biomes_.get(x, y, z);
    if (index >= palette_.size()) {
        throw BedrockChunkError("biome palette index out of range");
    }
    return palette_[index];
}

void BedrockBiomeSection::setBiomeId(uint8_t x, uint8_t y, uint8_t z, uint32_t biomeId) {
    proxy_ = false;
    ensureBiome(biomeId);
    auto it = std::find(palette_.begin(), palette_.end(), biomeId);
    biomes_.set(
        x,
        y,
        z,
        static_cast<uint32_t>(std::distance(palette_.begin(), it))
    );
}

void BedrockBiomeSection::readLegacy2D(BinaryStream& stream) {
    proxy_ = false;
    for (uint8_t x = 0; x < 16; ++x) {
        for (uint8_t z = 0; z < 16; ++z) {
            const uint32_t biomeId = stream.readU8();
            for (uint8_t y = 0; y < 16; ++y) {
                setBiomeId(x, y, z, biomeId);
            }
        }
    }
}

void BedrockBiomeSection::exportLegacy2D(BinaryStream& stream) const {
    for (uint8_t x = 0; x < 16; ++x) {
        for (uint8_t z = 0; z < 16; ++z) {
            stream.writeU8(static_cast<uint8_t>(getBiomeId(x, 0, z)));
        }
    }
}

void BedrockBiomeSection::read(ChunkStorageType type, BinaryStream& stream) {
    proxy_ = false;
    palette_.clear();

    const uint8_t paletteType = stream.readU8();
    if (paletteType == 0xff) {
        proxy_ = true;
        return;
    }

    const bool runtimeIds = (paletteType & 1u) != 0;
    if (!runtimeIds) {
        throw BedrockChunkError("biome palette type must use runtime IDs");
    }

    const uint8_t bitsPerBlock = static_cast<uint8_t>(paletteType >> 1u);
    if (bitsPerBlock == 0) {
        palette_.push_back(type == ChunkStorageType::LocalPersistence
            ? static_cast<uint32_t>(stream.readI32LE())
            : readRuntimeBiomeId(stream));
        biomes_ = PalettedStorage(1);
        return;
    }

    biomes_ = PalettedStorage(bitsPerBlock);
    biomes_.read(stream);

    uint32_t paletteLength = 0;
    if (type == ChunkStorageType::Runtime) {
        paletteLength = readRuntimeBiomeId(stream);
    } else {
        paletteLength = stream.readU32LE();
    }

    palette_.reserve(paletteLength);
    for (uint32_t i = 0; i < paletteLength; ++i) {
        palette_.push_back(type == ChunkStorageType::Runtime
            ? readRuntimeBiomeId(stream)
            : stream.readU32LE());
    }
}

void BedrockBiomeSection::exportTo(ChunkStorageType type, BinaryStream& stream) const {
    if (proxy_) {
        stream.writeU8(0xff);
        return;
    }

    const uint8_t bitsPerBlock = neededBits(static_cast<uint32_t>(palette_.size() - 1));
    const uint8_t paletteType = static_cast<uint8_t>((bitsPerBlock << 1u) | (type == ChunkStorageType::Runtime ? 1u : 0u));
    stream.writeU8(paletteType);

    if (bitsPerBlock == 0) {
        const uint32_t biome = palette_.empty() ? 0 : palette_[0];
        if (type == ChunkStorageType::LocalPersistence) {
            stream.writeI32LE(static_cast<int32_t>(biome));
        } else {
            writeRuntimeBiomeId(stream, biome);
        }
        return;
    }

    biomes_.write(stream);
    if (type == ChunkStorageType::Runtime) {
        writeRuntimeBiomeId(stream, static_cast<uint32_t>(palette_.size()));
        for (auto biome : palette_) {
            writeRuntimeBiomeId(stream, biome);
        }
    } else {
        stream.writeU32LE(static_cast<uint32_t>(palette_.size()));
        for (auto biome : palette_) {
            stream.writeU32LE(biome);
        }
    }
}

const std::vector<uint32_t>& BedrockBiomeSection::palette() const {
    return palette_;
}

void BedrockBiomeSection::ensureBiome(uint32_t biomeId) {
    if (std::find(palette_.begin(), palette_.end(), biomeId) != palette_.end()) {
        return;
    }
    palette_.push_back(biomeId);
    const uint8_t minBits = neededBits(static_cast<uint32_t>(palette_.size() - 1));
    if (minBits > biomes_.bitsPerBlock()) {
        biomes_ = biomes_.resize(minBits);
    }
}

uint8_t BedrockBiomeSection::neededBits(uint32_t value) {
    if (value == 0) return 0;
    uint8_t bits = 0;
    while (value != 0) {
        bits++;
        value >>= 1u;
    }
    return bits;
}

uint32_t BedrockBiomeSection::readRuntimeBiomeId(BinaryStream& stream) {
    return stream.readVarUInt() >> 1u;
}

void BedrockBiomeSection::writeRuntimeBiomeId(BinaryStream& stream, uint32_t value) {
    stream.writeVarUInt(value << 1u);
}

bool BedrockBlobStore::has(uint64_t hash) const {
    return entries_.find(hash) != entries_.end();
}

const BlobEntry* BedrockBlobStore::get(uint64_t hash) const {
    auto it = entries_.find(hash);
    return it == entries_.end() ? nullptr : &it->second;
}

void BedrockBlobStore::set(uint64_t hash, BlobEntry entry) {
    entries_[hash] = std::move(entry);
}

void BedrockBlobStore::erase(uint64_t hash) {
    entries_.erase(hash);
}

std::size_t BedrockBlobStore::size() const {
    return entries_.size();
}

namespace {

int32_t readZigZag32(BinaryStream& stream) {
    const uint32_t value = stream.readVarUInt();
    return static_cast<int32_t>((value >> 1u) ^ (0u - (value & 1u)));
}

void writeZigZag32(BinaryStream& stream, int32_t value) {
    const uint32_t encoded =
        (static_cast<uint32_t>(value) << 1u) ^
        static_cast<uint32_t>(value >> 31);
    stream.writeVarUInt(encoded);
}

} // namespace

BedrockLevelChunkPacket BedrockLevelChunkCodec::decodePacketPayload(
    const std::vector<uint8_t>& payload
) {
    BinaryStream stream(payload);
    BedrockLevelChunkPacket out;

    out.x = readZigZag32(stream);
    out.z = readZigZag32(stream);
    out.dimension = readZigZag32(stream);
    out.subChunkCount = stream.readVarInt();

    if (out.subChunkCount == -2) {
        out.highestSubChunkCount = stream.readU16LE();
    }

    out.cacheEnabled = stream.readU8() != 0;
    if (out.cacheEnabled) {
        const uint32_t hashCount = stream.readVarUInt();
        out.blobHashes.reserve(hashCount);
        for (uint32_t i = 0; i < hashCount; ++i) {
            out.blobHashes.push_back(stream.readU64LE());
        }
    }

    const uint32_t payloadSize = stream.readVarUInt();
    if (payloadSize > stream.remaining()) {
        throw BedrockChunkError("level_chunk payload size exceeds packet payload");
    }
    out.payload = stream.readBytes(payloadSize);

    if (!stream.eof()) {
        throw BedrockChunkError("level_chunk packet has trailing bytes");
    }

    return out;
}

std::vector<uint8_t> BedrockLevelChunkCodec::encodePacketPayload(
    const BedrockLevelChunkPacket& packet
) {
    BinaryStream stream;

    writeZigZag32(stream, packet.x);
    writeZigZag32(stream, packet.z);
    writeZigZag32(stream, packet.dimension);
    stream.writeVarInt(packet.subChunkCount);

    if (packet.subChunkCount == -2) {
        stream.writeU16LE(packet.highestSubChunkCount.value_or(0));
    }

    stream.writeU8(packet.cacheEnabled ? 1 : 0);
    if (packet.cacheEnabled) {
        stream.writeVarUInt(static_cast<uint32_t>(packet.blobHashes.size()));
        for (auto hash : packet.blobHashes) {
            stream.writeU64LE(hash);
        }
    }

    stream.writeVarUInt(static_cast<uint32_t>(packet.payload.size()));
    stream.writeBytes(packet.payload);
    return stream.buffer();
}

BedrockChunkColumn BedrockLevelChunkCodec::decodeNoCacheColumn(
    const BedrockLevelChunkPacket& packet,
    bool useCavesAndCliffsBounds
) {
    if (packet.cacheEnabled) {
        throw BedrockChunkError("decodeNoCacheColumn requires cacheEnabled=false");
    }

    BedrockChunkColumn column(packet.x, packet.z);
    if (useCavesAndCliffsBounds) {
        column.setBounds(-4, 20);
    }
    column.networkDecodeNoCache(
        packet.payload,
        packet.subChunkCount,
        useCavesAndCliffsBounds
    );
    return column;
}

BedrockLevelChunkPacket BedrockLevelChunkCodec::encodeNoCacheColumn(
    const BedrockChunkColumn& column,
    int32_t dimension
) {
    BedrockLevelChunkPacket packet;
    packet.x = column.x();
    packet.z = column.z();
    packet.dimension = dimension;
    packet.cacheEnabled = false;
    packet.payload = column.networkEncodeNoCache();

    int32_t count = 0;
    for (const auto& section : column.sections()) {
        if (section.has_value()) {
            count++;
        }
    }
    packet.subChunkCount = count;
    return packet;
}

std::vector<BedrockCacheBlob> BedrockLevelChunkCodec::decodeClientCacheMissResponsePayload(
    const std::vector<uint8_t>& payload
) {
    BinaryStream stream(payload);
    const uint32_t count = stream.readVarUInt();
    std::vector<BedrockCacheBlob> blobs;
    blobs.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        BedrockCacheBlob blob;
        blob.hash = stream.readU64LE();
        const uint32_t size = stream.readVarUInt();
        if (size > stream.remaining()) {
            throw BedrockChunkError("cache blob payload exceeds packet size");
        }
        blob.payload = stream.readBytes(size);
        blobs.push_back(std::move(blob));
    }

    if (!stream.eof()) {
        throw BedrockChunkError("client_cache_miss_response has trailing bytes");
    }

    return blobs;
}

std::vector<uint8_t> BedrockLevelChunkCodec::encodeClientCacheBlobStatusPayload(
    const BedrockClientCacheBlobStatus& status
) {
    BinaryStream stream;
    stream.writeVarUInt(static_cast<uint32_t>(status.missing.size()));
    stream.writeVarUInt(static_cast<uint32_t>(status.have.size()));
    for (auto hash : status.missing) {
        stream.writeU64LE(hash);
    }
    for (auto hash : status.have) {
        stream.writeU64LE(hash);
    }
    return stream.buffer();
}

BedrockChunkColumn::BedrockChunkColumn(int32_t x, int32_t z)
    : x_(x),
      z_(z) {
    setBounds(0, 16);
}

void BedrockChunkColumn::setBounds(int32_t minCY, int32_t maxCY) {
    if (maxCY <= minCY) {
        throw BedrockChunkError("maxCY must be greater than minCY");
    }
    minCY_ = minCY;
    maxCY_ = maxCY;
    minY_ = minCY_ * 16;
    maxY_ = maxCY_ * 16;
    worldHeight_ = maxY_ + std::abs(minY_);
    co_ = std::abs(minCY_);
    sections_.clear();
    sections_.resize(static_cast<std::size_t>(maxCY_ - minCY_));
    biomes_.clear();
    biomes_.reserve(static_cast<std::size_t>(maxCY_ - minCY_));
    for (int32_t y = minCY_; y < maxCY_; ++y) {
        biomes_.push_back(BedrockBiomeSection(y));
    }
}

int32_t BedrockChunkColumn::x() const { return x_; }
int32_t BedrockChunkColumn::z() const { return z_; }
int32_t BedrockChunkColumn::minCY() const { return minCY_; }
int32_t BedrockChunkColumn::maxCY() const { return maxCY_; }
int32_t BedrockChunkColumn::minY() const { return minY_; }
int32_t BedrockChunkColumn::maxY() const { return maxY_; }
int32_t BedrockChunkColumn::worldHeight() const { return worldHeight_; }

void BedrockChunkColumn::initialize(
    const BedrockBlockRegistry& registry,
    const BedrockChunkInitializer& initializer
) {
    if (!initializer) {
        throw BedrockChunkError("chunk initializer must not be empty");
    }
    for (int32_t y = minY_; y < maxY_; ++y) {
        for (int32_t z = 0; z < 16; ++z) {
            for (int32_t x = 0; x < 16; ++x) {
                auto block = initializer(x, y, z);
                if (block.has_value()) {
                    setBlock({.x = x, .y = y, .z = z}, *block, registry);
                }
            }
        }
    }
}

BedrockSubChunk* BedrockChunkColumn::getSection(int32_t blockY) {
    const int32_t index = sectionIndexForBlockY(blockY);
    if (index < 0 || static_cast<std::size_t>(index) >= sections_.size() || !sections_[index].has_value()) {
        return nullptr;
    }
    return &sections_[index].value();
}

const BedrockSubChunk* BedrockChunkColumn::getSection(int32_t blockY) const {
    const int32_t index = sectionIndexForBlockY(blockY);
    if (index < 0 || static_cast<std::size_t>(index) >= sections_.size() || !sections_[index].has_value()) {
        return nullptr;
    }
    return &sections_[index].value();
}

BedrockSubChunk* BedrockChunkColumn::getSectionAtIndex(int32_t sectionY) {
    const int32_t index = co_ + sectionY;
    if (index < 0 || static_cast<std::size_t>(index) >= sections_.size() ||
        !sections_[index].has_value()) {
        return nullptr;
    }
    return &sections_[index].value();
}

const BedrockSubChunk* BedrockChunkColumn::getSectionAtIndex(int32_t sectionY) const {
    const int32_t index = co_ + sectionY;
    if (index < 0 || static_cast<std::size_t>(index) >= sections_.size() ||
        !sections_[index].has_value()) {
        return nullptr;
    }
    return &sections_[index].value();
}

BedrockSubChunk& BedrockChunkColumn::ensureSection(int32_t blockY) {
    const int32_t sectionY = blockY >> 4;
    const int32_t index = co_ + sectionY;
    if (index < 0 || static_cast<std::size_t>(index) >= sections_.size()) {
        throw BedrockChunkError("block Y outside chunk bounds");
    }
    if (!sections_[index].has_value()) {
        sections_[index] = BedrockSubChunk(static_cast<int8_t>(sectionY), 9);
    }
    return sections_[index].value();
}

BedrockSubChunk& BedrockChunkColumn::newSection(int32_t sectionY) {
    const int32_t index = co_ + sectionY;
    if (index < 0 || static_cast<std::size_t>(index) >= sections_.size()) {
        throw BedrockChunkError("section Y outside chunk bounds");
    }
    sections_[index] = BedrockSubChunk(static_cast<int8_t>(sectionY), 9);
    return sections_[index].value();
}

void BedrockChunkColumn::setSection(int32_t sectionY, BedrockSubChunk section) {
    const int32_t index = co_ + sectionY;
    if (index < 0 || static_cast<std::size_t>(index) >= sections_.size()) {
        throw BedrockChunkError("section Y outside chunk bounds");
    }
    section.setY(static_cast<int8_t>(sectionY));
    sections_[index] = std::move(section);
}

int32_t BedrockChunkColumn::getBlockStateId(const BlockPosition& pos) const {
    const auto* section = getSection(pos.y);
    if (!section) {
        return 0;
    }
    const uint8_t layer = pos.layer.value_or(0);
    return section->getBlockStateId(layer, localCoord(pos.x), localCoord(pos.y), localCoord(pos.z));
}

void BedrockChunkColumn::setBlockStateId(const BlockPosition& pos, int32_t stateId) {
    auto& section = ensureSection(pos.y);
    const uint8_t layer = pos.layer.value_or(0);
    section.setBlockStateId(layer, localCoord(pos.x), localCoord(pos.y), localCoord(pos.z), stateId);
}

std::vector<BedrockBlockState> BedrockChunkColumn::getBlocks() const {
    std::unordered_map<int32_t, BedrockBlockState> byStateId;
    for (const auto& section : sections_) {
        if (!section.has_value()) {
            continue;
        }
        for (auto block : section->getPalette()) {
            byStateId.insert_or_assign(block.stateId, std::move(block));
        }
    }

    std::vector<BedrockBlockState> blocks;
    blocks.reserve(byStateId.size());
    for (auto& [stateId, block] : byStateId) {
        (void) stateId;
        blocks.push_back(std::move(block));
    }
    std::sort(blocks.begin(), blocks.end(), [](const auto& left, const auto& right) {
        return left.stateId < right.stateId;
    });
    return blocks;
}

std::optional<BedrockBlock> BedrockChunkColumn::getBlock(
    const BlockPosition& pos,
    const BedrockBlockRegistry& registry,
    bool full
) const {
    const auto* section = getSection(pos.y);
    if (section == nullptr) {
        const auto* air = registry.blockByName("air");
        return air == nullptr
            ? std::nullopt
            : registry.fromStateId(air->defaultState, 0);
    }

    const uint8_t layer = pos.layer.value_or(0);
    const int32_t stateId = section->getBlockStateId(
        layer,
        localCoord(pos.x),
        localCoord(pos.y),
        localCoord(pos.z)
    );
    auto block = registry.fromStateId(stateId, static_cast<int32_t>(getBiomeId(pos)));
    if (!block.has_value()) return std::nullopt;

    if (!pos.layer.has_value() && section->hasLayer(1)) {
        const int32_t superimposedStateId = section->getBlockStateId(
            1,
            localCoord(pos.x),
            localCoord(pos.y),
            localCoord(pos.z)
        );
        auto superimposed = registry.fromStateId(
            superimposedStateId,
            static_cast<int32_t>(getBiomeId(pos))
        );
        if (superimposed.has_value()) {
            if (superimposed->name.find("water") != std::string::npos) {
                block->computedStates.insert_or_assign(
                    "waterlogged",
                    BedrockBlockProperty::byte(1)
                );
            }
            block->superimposed = std::make_shared<BedrockBlock>(std::move(*superimposed));
        }
    }

    if (full) {
        block->light = section->getBlockLight(
            localCoord(pos.x),
            localCoord(pos.y),
            localCoord(pos.z)
        );
        block->skyLight = section->getSkyLight(
            localCoord(pos.x),
            localCoord(pos.y),
            localCoord(pos.z)
        );
        if (const auto* entity = getBlockEntityNbt(pos)) {
            block->entity = *entity;
        }
    }
    return block;
}

void BedrockChunkColumn::setBlock(const BlockPosition& pos, const BedrockBlock& block) {
    auto& section = ensureSection(pos.y);
    const uint8_t x = localCoord(pos.x);
    const uint8_t y = localCoord(pos.y);
    const uint8_t z = localCoord(pos.z);

    if (pos.layer.has_value()) {
        section.setBlockStateId(*pos.layer, x, y, z, block.stateId);
    } else {
        section.setBlockStateId(0, x, y, z, block.stateId);
        if (block.superimposed) {
            section.setBlockStateId(1, x, y, z, block.superimposed->stateId);
        }
    }
    section.setBlockLight(x, y, z, block.light);
    section.setSkyLight(x, y, z, block.skyLight);
    if (block.entity.has_value()) {
        setBlockEntityNbt(pos, *block.entity);
    }
}

void BedrockChunkColumn::setBlock(
    const BlockPosition& pos,
    const BedrockBlock& block,
    const BedrockBlockRegistry& registry
) {
    if (getSection(pos.y) == nullptr) {
        const auto* air = registry.blockByName("air");
        if (air == nullptr) {
            throw BedrockChunkError("Bedrock block registry does not contain air");
        }
        const int32_t sectionY = pos.y >> 4;
        setSection(
            sectionY,
            BedrockSubChunk::createAir(static_cast<int8_t>(sectionY), air->defaultState)
        );
    }
    setBlock(pos, block);
}

uint8_t BedrockChunkColumn::getBlockLight(const BlockPosition& pos) const {
    const auto* section = getSection(pos.y);
    if (!section) {
        throw BedrockChunkError("block-light section is not loaded");
    }
    return section->getBlockLight(localCoord(pos.x), localCoord(pos.y), localCoord(pos.z));
}

void BedrockChunkColumn::setBlockLight(const BlockPosition& pos, uint8_t value) {
    auto* section = getSection(pos.y);
    if (!section) {
        throw BedrockChunkError("block-light section is not loaded");
    }
    section->setBlockLight(localCoord(pos.x), localCoord(pos.y), localCoord(pos.z), value);
}

uint8_t BedrockChunkColumn::getSkyLight(const BlockPosition& pos) const {
    const auto* section = getSection(pos.y);
    if (!section) {
        throw BedrockChunkError("sky-light section is not loaded");
    }
    return section->getSkyLight(localCoord(pos.x), localCoord(pos.y), localCoord(pos.z));
}

void BedrockChunkColumn::setSkyLight(const BlockPosition& pos, uint8_t value) {
    auto* section = getSection(pos.y);
    if (!section) {
        throw BedrockChunkError("sky-light section is not loaded");
    }
    section->setSkyLight(localCoord(pos.x), localCoord(pos.y), localCoord(pos.z), value);
}

void BedrockChunkColumn::setBlockEntity(const BlockPosition& pos, std::vector<uint8_t> tag) {
    const std::string key = blockEntityKey(pos);
    blockEntities_[key] = std::move(tag);

    try {
        BinaryStream stream(blockEntities_[key]);
        auto document = BedrockNbtCodec::read(stream, BedrockNbtEncoding::LittleVarInt);
        if (!stream.eof()) {
            throw BedrockChunkError("block entity contains trailing bytes");
        }
        blockEntityNbt_[key] = std::move(document);
    } catch (const std::exception&) {
        // Keep the backward-compatible raw tag even when the caller supplied
        // an opaque or not-yet-complete value.
        blockEntityNbt_.erase(key);
    }
}

void BedrockChunkColumn::setBlockEntityNbt(const BlockPosition& pos, NbtDocument tag) {
    const std::string key = blockEntityKey(pos);
    BinaryStream stream;
    BedrockNbtCodec::write(stream, tag, BedrockNbtEncoding::LittleVarInt);
    blockEntities_[key] = stream.buffer();
    blockEntityNbt_[key] = std::move(tag);
}

void BedrockChunkColumn::addBlockEntity(NbtDocument tag) {
    const BlockPosition pos = blockEntityPosition(tag);
    setBlockEntityNbt(pos, std::move(tag));
}

const std::vector<uint8_t>* BedrockChunkColumn::getBlockEntity(const BlockPosition& pos) const {
    auto it = blockEntities_.find(blockEntityKey(pos));
    return it == blockEntities_.end() ? nullptr : &it->second;
}

const NbtDocument* BedrockChunkColumn::getBlockEntityNbt(const BlockPosition& pos) const {
    auto it = blockEntityNbt_.find(blockEntityKey(pos));
    return it == blockEntityNbt_.end() ? nullptr : &it->second;
}

void BedrockChunkColumn::removeBlockEntity(const BlockPosition& pos) {
    const std::string key = blockEntityKey(pos);
    blockEntities_.erase(key);
    blockEntityNbt_.erase(key);
}

bool BedrockChunkColumn::moveBlockEntity(
    const BlockPosition& pos,
    const BlockPosition& newPos
) {
    const std::string oldKey = blockEntityKey(pos);
    const std::string newKey = blockEntityKey(newPos);
    if (oldKey == newKey) {
        return blockEntities_.find(oldKey) != blockEntities_.end() ||
            blockEntityNbt_.find(oldKey) != blockEntityNbt_.end();
    }

    std::optional<std::vector<uint8_t>> raw;
    std::optional<NbtDocument> document;
    if (auto it = blockEntities_.find(oldKey); it != blockEntities_.end()) {
        raw = std::move(it->second);
        blockEntities_.erase(it);
    }
    if (auto it = blockEntityNbt_.find(oldKey); it != blockEntityNbt_.end()) {
        document = std::move(it->second);
        blockEntityNbt_.erase(it);
    }
    if (!raw.has_value() && !document.has_value()) {
        return false;
    }

    blockEntities_.erase(newKey);
    blockEntityNbt_.erase(newKey);
    if (raw.has_value()) {
        blockEntities_[newKey] = std::move(*raw);
    }
    if (document.has_value()) {
        blockEntityNbt_[newKey] = std::move(*document);
    }
    return true;
}

std::size_t BedrockChunkColumn::blockEntityCount() const {
    return blockEntities_.size();
}

std::vector<NbtDocument> BedrockChunkColumn::getSectionBlockEntities(
    int32_t sectionY
) const {
    std::vector<std::string> keys;
    keys.reserve(blockEntityNbt_.size());
    for (const auto& item : blockEntityNbt_) {
        const auto firstComma = item.first.find(',');
        const auto secondComma = firstComma == std::string::npos
            ? std::string::npos
            : item.first.find(',', firstComma + 1);
        if (firstComma == std::string::npos || secondComma == std::string::npos) {
            throw BedrockChunkError("invalid block-entity position key");
        }
        const int32_t y = std::stoi(item.first.substr(
            firstComma + 1,
            secondComma - firstComma - 1
        ));
        if ((y >> 4) == sectionY) {
            keys.push_back(item.first);
        }
    }
    std::sort(keys.begin(), keys.end());

    std::vector<NbtDocument> found;
    found.reserve(keys.size());
    for (const auto& key : keys) {
        found.push_back(blockEntityNbt_.at(key));
    }
    return found;
}

std::vector<uint8_t> BedrockChunkColumn::diskEncodeBlockEntities() const {
    BinaryStream stream;
    encodeBlockEntities(stream, BedrockNbtEncoding::LittleEndian);
    return stream.buffer();
}

void BedrockChunkColumn::diskDecodeBlockEntities(const std::vector<uint8_t>& data) {
    BinaryStream stream(data);
    decodeBlockEntities(stream, BedrockNbtEncoding::LittleEndian);
}

void BedrockChunkColumn::addEntity(NbtDocument entityTag) {
    const std::string key = entityKey(entityTag);
    entities_[key] = std::move(entityTag);
}

bool BedrockChunkColumn::removeEntity(std::string_view id) {
    return entities_.erase(std::string(id)) != 0;
}

BedrockEntityMap& BedrockChunkColumn::getEntities() {
    return entities_;
}

const BedrockEntityMap& BedrockChunkColumn::getEntities() const {
    return entities_;
}

void BedrockChunkColumn::loadEntities(BedrockEntityMap entities) {
    entities_ = std::move(entities);
}

void BedrockChunkColumn::loadEntities(std::vector<NbtDocument> entities) {
    entities_.clear();
    for (auto& entity : entities) {
        addEntity(std::move(entity));
    }
}

std::size_t BedrockChunkColumn::entityCount() const {
    return entities_.size();
}

std::vector<uint8_t> BedrockChunkColumn::diskEncodeEntities() const {
    std::vector<std::string> keys;
    keys.reserve(entities_.size());
    for (const auto& item : entities_) {
        keys.push_back(item.first);
    }
    std::sort(keys.begin(), keys.end());

    BinaryStream stream;
    for (const auto& key : keys) {
        BedrockNbtCodec::write(
            stream,
            entities_.at(key),
            BedrockNbtEncoding::LittleEndian
        );
    }
    return stream.buffer();
}

void BedrockChunkColumn::diskDecodeEntities(const std::vector<uint8_t>& data) {
    BinaryStream stream(data);
    while (!stream.eof() &&
        stream.buffer()[stream.offset()] == static_cast<uint8_t>(NbtTagType::Compound)) {
        try {
            addEntity(BedrockNbtCodec::read(stream, BedrockNbtEncoding::LittleEndian));
        } catch (const std::exception& error) {
            throw BedrockChunkError("invalid entity NBT: " + std::string(error.what()));
        }
    }
}

void BedrockChunkColumn::loadHeights(BedrockHeightMap heights) {
    heights_ = std::move(heights);
}

BedrockHeightMap* BedrockChunkColumn::getHeights() {
    return heights_.has_value() ? &*heights_ : nullptr;
}

const BedrockHeightMap* BedrockChunkColumn::getHeights() const {
    return heights_.has_value() ? &*heights_ : nullptr;
}

void BedrockChunkColumn::writeHeightMap(BinaryStream& stream) {
    if (!heights_.has_value()) {
        heights_ = BedrockHeightMap {};
    }
    for (const uint16_t height : *heights_) {
        stream.writeU16LE(height);
    }
}

std::vector<std::optional<BedrockSubChunk>>& BedrockChunkColumn::getSections() {
    return sections_;
}

const std::vector<std::optional<BedrockSubChunk>>& BedrockChunkColumn::getSections() const {
    return sections_;
}

const std::vector<std::optional<BedrockSubChunk>>& BedrockChunkColumn::sections() const {
    return sections_;
}

uint32_t BedrockChunkColumn::getBiomeId(const BlockPosition& pos) const {
    const int32_t sectionY = pos.y >> 4;
    int32_t index = co_ + sectionY;
    if (index < 0 || static_cast<std::size_t>(index) >= biomes_.size()) {
        return 0;
    }
    while (index > 0 && biomes_[static_cast<std::size_t>(index)].proxy()) {
        --index;
    }
    const auto& section = biomes_[index];
    return section.getBiomeId(localCoord(pos.x), localCoord(pos.y), localCoord(pos.z));
}

void BedrockChunkColumn::setBiomeId(const BlockPosition& pos, uint32_t biomeId) {
    const int32_t sectionY = pos.y >> 4;
    const int32_t index = co_ + sectionY;
    if (index < 0 || static_cast<std::size_t>(index) >= biomes_.size()) {
        throw BedrockChunkError("biome Y outside chunk bounds");
    }
    if (biomes_[index].proxy() && index > 0) {
        biomes_[index] = biomes_[static_cast<std::size_t>(index - 1)];
        biomes_[index].setProxy(false);
    }
    biomes_[index].setBiomeId(localCoord(pos.x), localCoord(pos.y), localCoord(pos.z), biomeId);
}

void BedrockChunkColumn::loadLegacyBiomes(const std::vector<uint8_t>& data) {
    BinaryStream stream(data);
    if (biomes_.empty()) {
        biomes_.push_back(BedrockBiomeSection(0));
    }
    biomes_[0].readLegacy2D(stream);
    for (std::size_t i = 1; i < biomes_.size(); ++i) {
        biomes_[i] = BedrockBiomeSection::proxyToPrevious(
            minCY_ + static_cast<int32_t>(i)
        );
    }
}

std::vector<uint8_t> BedrockChunkColumn::dumpLegacyBiomes() const {
    BinaryStream stream;
    if (biomes_.empty()) {
        BedrockBiomeSection().exportLegacy2D(stream);
    } else {
        biomes_[0].exportLegacy2D(stream);
    }
    return stream.buffer();
}

void BedrockChunkColumn::loadBiomes(BinaryStream& stream, ChunkStorageType type) {
    biomes_.clear();
    BedrockBiomeSection last(minCY_);
    bool hasLast = false;
    for (int32_t y = minCY_; y < maxCY_ && !stream.eof(); ++y) {
        const auto oldOffset = stream.offset();
        const uint8_t next = stream.readU8();
        stream.seek(oldOffset);

        if (next == 0xff) {
            stream.readU8();
            if (!hasLast) {
                throw BedrockChunkError("proxy biome section has no previous section");
            }
            auto proxy = BedrockBiomeSection::proxyToPrevious(y);
            biomes_.push_back(proxy);
            continue;
        }

        BedrockBiomeSection section(y);
        section.read(type, stream);
        last = section;
        hasLast = true;
        biomes_.push_back(section);
    }

    while (biomes_.size() < static_cast<std::size_t>(maxCY_ - minCY_)) {
        int32_t y = minCY_ + static_cast<int32_t>(biomes_.size());
        biomes_.push_back(hasLast ? BedrockBiomeSection::proxyToPrevious(y) : BedrockBiomeSection(y));
    }
}

void BedrockChunkColumn::writeBiomes(BinaryStream& stream) const {
    for (std::size_t i = 0; i < biomes_.size(); ++i) {
        biomes_[i].exportTo(ChunkStorageType::Runtime, stream);
    }
}

void BedrockChunkColumn::networkDecodeNoCache(const std::vector<uint8_t>& payload, int32_t sectionCount) {
    networkDecodeNoCache(
        payload,
        sectionCount,
        minCY_ < 0 || maxCY_ > 16
    );
}

void BedrockChunkColumn::networkDecodeNoCache(
    const std::vector<uint8_t>& payload,
    int32_t sectionCount,
    bool use3DBiomes
) {
    BinaryStream stream(payload);

    if (sectionCount != -1 && sectionCount != -2) {
        sections_.clear();
        sections_.resize(static_cast<std::size_t>(maxCY_ - minCY_));
        for (int32_t i = 0; i < sectionCount; ++i) {
            BedrockSubChunk section(static_cast<int8_t>(i), 9);
            section.decodeFrom(ChunkStorageType::Runtime, stream);
            setSection(section.y(), section);
        }
    }

    if (use3DBiomes) {
        loadBiomes(stream, ChunkStorageType::Runtime);
    } else {
        if (stream.remaining() < 256) {
            throw BedrockChunkError("legacy biome data is truncated");
        }
        loadLegacyBiomes(stream.readBytes(256));
    }

    if (!stream.eof()) {
        const uint32_t encodedLength = stream.readVarUInt();
        const int32_t borderBlocksLength = static_cast<int32_t>(
            (encodedLength >> 1u) ^ (0u - (encodedLength & 1u))
        );
        if (borderBlocksLength < 0 || static_cast<std::size_t>(borderBlocksLength) > stream.remaining()) {
            throw BedrockChunkError("invalid border block length");
        }
        auto borderBlocks = stream.readBytes(static_cast<std::size_t>(borderBlocksLength));
        if (!borderBlocks.empty()) {
            throw BedrockChunkError("border blocks are not supported yet");
        }
    }

    decodeBlockEntities(stream, BedrockNbtEncoding::LittleVarInt);
}

std::vector<uint8_t> BedrockChunkColumn::networkEncodeNoCache() const {
    return networkEncodeNoCache(minCY_ < 0 || maxCY_ > 16);
}

std::vector<uint8_t> BedrockChunkColumn::networkEncodeNoCache(bool use3DBiomes) const {
    BinaryStream stream;
    for (const auto& section : sections_) {
        if (section.has_value()) {
            section->encodeTo(ChunkStorageType::Runtime, stream, true);
        }
    }
    if (use3DBiomes) {
        writeBiomes(stream);
    } else {
        stream.writeBytes(dumpLegacyBiomes());
    }
    stream.writeVarUInt(0);
    encodeBlockEntities(stream, BedrockNbtEncoding::LittleVarInt);
    return stream.buffer();
}

void BedrockChunkColumn::networkDecodeSubChunkNoCache(
    int32_t sectionY,
    const std::vector<uint8_t>& payload
) {
    BinaryStream stream(payload);
    BedrockSubChunk section(static_cast<int8_t>(sectionY), 9);
    section.decodeFrom(ChunkStorageType::Runtime, stream);
    const int32_t decodedY = section.y();
    setSection(decodedY, std::move(section));
    decodeBlockEntities(stream, BedrockNbtEncoding::LittleVarInt);
}

std::vector<uint8_t> BedrockChunkColumn::networkEncodeSubChunkNoCache(
    int32_t sectionY,
    bool compactStorage
) const {
    const int32_t index = co_ + sectionY;
    if (index < 0 || static_cast<std::size_t>(index) >= sections_.size() ||
        !sections_[static_cast<std::size_t>(index)].has_value()) {
        throw BedrockChunkError("subchunk section is not loaded");
    }

    BinaryStream stream;
    sections_[static_cast<std::size_t>(index)]->encodeTo(
        ChunkStorageType::Runtime,
        stream,
        compactStorage
    );
    encodeBlockEntities(stream, BedrockNbtEncoding::LittleVarInt, sectionY);
    return stream.buffer();
}

BedrockEncodedChunkCache BedrockChunkColumn::networkEncodeCached(
    BedrockBlobStore& blobStore,
    bool use3DBiomes,
    BedrockBlockStateProvider provider
) const {
    BedrockEncodedChunkCache encoded;

    if (!use3DBiomes) {
        for (const auto& section : sections_) {
            if (!section.has_value()) {
                continue;
            }
            auto buffer = section->encode(
                ChunkStorageType::NetworkPersistence,
                true,
                provider
            );
            const uint64_t hash = xxHash64(buffer);
            BlobEntry entry;
            entry.x = x_;
            entry.y = section->y();
            entry.z = z_;
            entry.type = BlobType::ChunkSection;
            entry.buffer = std::move(buffer);
            blobStore.set(hash, std::move(entry));
            encoded.blobHashes.push_back(hash);
        }

        auto biomeBuffer = dumpLegacyBiomes();
        const uint64_t biomeHash = xxHash64(biomeBuffer);
        BlobEntry biomeEntry;
        biomeEntry.x = x_;
        biomeEntry.z = z_;
        biomeEntry.type = BlobType::Biomes;
        biomeEntry.buffer = std::move(biomeBuffer);
        blobStore.set(biomeHash, std::move(biomeEntry));
        encoded.blobHashes.push_back(biomeHash);
    } else {
        BinaryStream biomeStream;
        writeBiomes(biomeStream);
        auto biomeBuffer = biomeStream.buffer();
        const uint64_t biomeHash = xxHash64(biomeBuffer);
        BlobEntry biomeEntry;
        biomeEntry.x = x_;
        biomeEntry.z = z_;
        biomeEntry.type = BlobType::Biomes;
        biomeEntry.buffer = std::move(biomeBuffer);
        blobStore.set(biomeHash, std::move(biomeEntry));
        encoded.blobHashes.push_back(biomeHash);
    }

    BinaryStream payload;
    payload.writeVarUInt(0);
    if (!use3DBiomes) {
        encodeBlockEntities(payload, BedrockNbtEncoding::LittleVarInt);
    }
    encoded.payload = payload.buffer();
    return encoded;
}

BedrockEncodedChunkCache BedrockChunkColumn::networkEncodeCached(
    BedrockBlobStore& blobStore,
    BedrockBlockStateProvider provider
) const {
    return networkEncodeCached(
        blobStore,
        minCY_ < 0 || maxCY_ > 16,
        std::move(provider)
    );
}

BedrockEncodedSubChunkCache BedrockChunkColumn::networkEncodeSubChunkCached(
    int32_t sectionY,
    BedrockBlobStore& blobStore,
    bool compactStorage
) const {
    const int32_t index = co_ + sectionY;
    if (index < 0 || static_cast<std::size_t>(index) >= sections_.size() ||
        !sections_[static_cast<std::size_t>(index)].has_value()) {
        throw BedrockChunkError("subchunk section is not loaded");
    }

    auto terrain = sections_[static_cast<std::size_t>(index)]->encode(
        ChunkStorageType::Runtime,
        compactStorage
    );
    BedrockEncodedSubChunkCache encoded;
    encoded.blobHash = xxHash64(terrain);

    BlobEntry entry;
    entry.x = x_;
    entry.y = sectionY;
    entry.z = z_;
    entry.type = BlobType::ChunkSection;
    entry.buffer = std::move(terrain);
    blobStore.set(encoded.blobHash, std::move(entry));

    BinaryStream payload;
    encodeBlockEntities(
        payload,
        BedrockNbtEncoding::LittleVarInt,
        sectionY
    );
    encoded.payload = payload.buffer();
    return encoded;
}

std::vector<uint64_t> BedrockChunkColumn::networkDecodeCached(
    const std::vector<uint64_t>& blobHashes,
    const BedrockBlobStore& blobStore,
    const std::vector<uint8_t>& payload,
    BedrockBlockStateResolver resolver
) {
    if (!payload.empty()) {
        BinaryStream stream(payload);
        const uint32_t encodedLength = stream.readVarUInt();
        const int32_t borderBlocksLength = static_cast<int32_t>(
            (encodedLength >> 1u) ^ (0u - (encodedLength & 1u))
        );
        if (borderBlocksLength < 0 || static_cast<std::size_t>(borderBlocksLength) > stream.remaining()) {
            throw BedrockChunkError("invalid cached border block length");
        }
        auto borderBlocks = stream.readBytes(static_cast<std::size_t>(borderBlocksLength));
        if (!borderBlocks.empty()) {
            throw BedrockChunkError("border blocks are not supported yet");
        }
        decodeBlockEntities(stream, BedrockNbtEncoding::LittleVarInt);
    }

    std::vector<uint64_t> misses;
    for (auto hash : blobHashes) {
        if (!blobStore.has(hash)) {
            misses.push_back(hash);
        }
    }
    if (!misses.empty()) {
        return misses;
    }

    sections_.clear();
    sections_.resize(static_cast<std::size_t>(maxCY_ - minCY_));

    bool hasSectionBlobs = false;
    for (auto hash : blobHashes) {
        const auto* entry = blobStore.get(hash);
        if (entry != nullptr && entry->type == BlobType::ChunkSection) {
            hasSectionBlobs = true;
            break;
        }
    }

    for (auto hash : blobHashes) {
        const auto* entry = blobStore.get(hash);
        if (!entry) {
            misses.push_back(hash);
            continue;
        }
        if (entry->type == BlobType::Biomes) {
            if (hasSectionBlobs) {
                loadLegacyBiomes(entry->buffer);
            } else {
                BinaryStream stream(entry->buffer);
                loadBiomes(stream, ChunkStorageType::NetworkPersistence);
            }
        } else if (entry->type == BlobType::ChunkSection) {
            BinaryStream stream(entry->buffer);
            BedrockSubChunk section(static_cast<int8_t>(entry->y), 8);
            section.decodeFrom(
                ChunkStorageType::NetworkPersistence,
                stream,
                0,
                resolver
            );
            if (!stream.eof()) {
                throw BedrockChunkError("cached chunk-section blob has trailing bytes");
            }
            setSection(section.y(), std::move(section));
        } else {
            throw BedrockChunkError("unknown cached blob type");
        }
    }

    return misses;
}

void BedrockChunkColumn::decodeBlockEntities(
    BinaryStream& stream,
    BedrockNbtEncoding encoding
) {
    while (!stream.eof() &&
        stream.buffer()[stream.offset()] == static_cast<uint8_t>(NbtTagType::Compound)) {
        const std::size_t start = stream.offset();
        NbtDocument document;
        try {
            document = BedrockNbtCodec::read(stream, encoding);
        } catch (const std::exception& error) {
            throw BedrockChunkError("invalid block-entity NBT: " + std::string(error.what()));
        }
        const std::size_t end = stream.offset();
        const BlockPosition pos = blockEntityPosition(document);
        const std::string key = blockEntityKey(pos);
        blockEntityNbt_[key] = document;

        if (encoding == BedrockNbtEncoding::LittleVarInt) {
            const auto& buffer = stream.buffer();
            blockEntities_[key] = std::vector<uint8_t>(
                buffer.begin() + static_cast<std::ptrdiff_t>(start),
                buffer.begin() + static_cast<std::ptrdiff_t>(end)
            );
        } else {
            BinaryStream networkTag;
            BedrockNbtCodec::write(
                networkTag,
                document,
                BedrockNbtEncoding::LittleVarInt
            );
            blockEntities_[key] = networkTag.buffer();
        }
    }
}

void BedrockChunkColumn::encodeBlockEntities(
    BinaryStream& stream,
    BedrockNbtEncoding encoding,
    std::optional<int32_t> sectionY
) const {
    std::vector<std::string> keys;
    keys.reserve(blockEntities_.size());
    for (const auto& item : blockEntities_) {
        if (sectionY.has_value()) {
            const auto firstComma = item.first.find(',');
            const auto secondComma = firstComma == std::string::npos
                ? std::string::npos
                : item.first.find(',', firstComma + 1);
            if (firstComma == std::string::npos || secondComma == std::string::npos) {
                throw BedrockChunkError("invalid block-entity position key");
            }
            const int32_t y = std::stoi(item.first.substr(
                firstComma + 1,
                secondComma - firstComma - 1
            ));
            if ((y >> 4) != *sectionY) {
                continue;
            }
        }
        keys.push_back(item.first);
    }
    std::sort(keys.begin(), keys.end());

    for (const auto& key : keys) {
        const auto rawIt = blockEntities_.find(key);
        if (encoding == BedrockNbtEncoding::LittleVarInt && rawIt != blockEntities_.end()) {
            stream.writeBytes(rawIt->second);
            continue;
        }

        auto documentIt = blockEntityNbt_.find(key);
        if (documentIt != blockEntityNbt_.end()) {
            BedrockNbtCodec::write(stream, documentIt->second, encoding);
            continue;
        }

        if (rawIt == blockEntities_.end()) {
            throw BedrockChunkError("block-entity storage is inconsistent");
        }
        BinaryStream rawStream(rawIt->second);
        const auto document = BedrockNbtCodec::read(
            rawStream,
            BedrockNbtEncoding::LittleVarInt
        );
        if (!rawStream.eof()) {
            throw BedrockChunkError("raw block entity contains trailing bytes");
        }
        BedrockNbtCodec::write(stream, document, encoding);
    }
}

BlockPosition BedrockChunkColumn::blockEntityPosition(const NbtDocument& tag) {
    if (tag.root.type != NbtTagType::Compound) {
        throw BedrockChunkError("block-entity NBT root must be a compound");
    }

    auto coordinate = [&tag](const char* name) -> int32_t {
        const NbtValue* value = tag.root.find(name);
        if (value == nullptr ||
            (value->type != NbtTagType::Byte && value->type != NbtTagType::Short &&
             value->type != NbtTagType::Int && value->type != NbtTagType::Long) ||
            value->integerValue < std::numeric_limits<int32_t>::min() ||
            value->integerValue > std::numeric_limits<int32_t>::max()) {
            throw BedrockChunkError(std::string("block-entity NBT is missing int ") + name);
        }
        return static_cast<int32_t>(value->integerValue);
    };

    return {
        .x = coordinate("x"),
        .y = coordinate("y"),
        .z = coordinate("z")
    };
}

std::string BedrockChunkColumn::entityKey(const NbtDocument& tag) {
    if (tag.root.type != NbtTagType::Compound) {
        throw BedrockChunkError("entity NBT root must be a compound");
    }
    const NbtValue* uniqueId = tag.root.find("UniqueID");
    if (uniqueId == nullptr ||
        (uniqueId->type != NbtTagType::Byte && uniqueId->type != NbtTagType::Short &&
         uniqueId->type != NbtTagType::Int && uniqueId->type != NbtTagType::Long)) {
        throw BedrockChunkError("entity NBT is missing integer UniqueID");
    }
    return std::to_string(uniqueId->integerValue);
}

int32_t BedrockChunkColumn::sectionIndexForBlockY(int32_t y) const {
    return co_ + (y >> 4);
}

uint8_t BedrockChunkColumn::localCoord(int32_t value) {
    return static_cast<uint8_t>(value & 0x0f);
}

std::string BedrockChunkColumn::blockEntityKey(const BlockPosition& pos) {
    return std::to_string(pos.x & 0x0f) + "," +
        std::to_string(pos.y) + "," +
        std::to_string(pos.z & 0x0f);
}

BedrockWorld::BedrockWorld(BedrockWorldOptions options)
    : options_(std::move(options)) {}

void BedrockWorld::onColumnLoad(ColumnHandler handler) {
    loadHandlers_.push_back(std::move(handler));
}

void BedrockWorld::onColumnUnload(ColumnHandler handler) {
    unloadHandlers_.push_back(std::move(handler));
}

void BedrockWorld::onBlockUpdate(BlockUpdateHandler handler) {
    blockUpdateHandlers_.push_back(std::move(handler));
}

void BedrockWorld::onBlockUpdate(
    const BlockPosition& pos,
    BlockUpdateHandler handler
) {
    positionedBlockUpdateHandlers_[blockKey(pos)].push_back(std::move(handler));
}

void BedrockWorld::onDoneSaving(VoidHandler handler) {
    doneSavingHandlers_.push_back(std::move(handler));
}

bool BedrockWorld::hasColumn(int32_t chunkX, int32_t chunkZ) const {
    return columns_.find(key(chunkX, chunkZ)) != columns_.end();
}

BedrockChunkColumn* BedrockWorld::getLoadedColumn(int32_t chunkX, int32_t chunkZ) {
    auto it = columns_.find(key(chunkX, chunkZ));
    return it == columns_.end() ? nullptr : &it->second;
}

const BedrockChunkColumn* BedrockWorld::getLoadedColumn(int32_t chunkX, int32_t chunkZ) const {
    auto it = columns_.find(key(chunkX, chunkZ));
    return it == columns_.end() ? nullptr : &it->second;
}

BedrockChunkColumn* BedrockWorld::getLoadedColumnAt(const BlockPosition& pos) {
    return getLoadedColumn(chunkCoord(pos.x), chunkCoord(pos.z));
}

const BedrockChunkColumn* BedrockWorld::getLoadedColumnAt(const BlockPosition& pos) const {
    return getLoadedColumn(chunkCoord(pos.x), chunkCoord(pos.z));
}

BedrockChunkColumn* BedrockWorld::getColumn(int32_t chunkX, int32_t chunkZ) {
    if (auto* loaded = getLoadedColumn(chunkX, chunkZ)) {
        return loaded;
    }

    std::optional<BedrockChunkColumn> column;
    bool loadedFromStorage = false;
    if (options_.loadColumn) {
        column = options_.loadColumn(chunkX, chunkZ);
        loadedFromStorage = column.has_value();
    }
    if (!column.has_value() && options_.chunkGenerator) {
        column = options_.chunkGenerator(chunkX, chunkZ);
    }
    if (!column.has_value()) {
        return nullptr;
    }

    setLoadedColumn(
        chunkX,
        chunkZ,
        std::move(*column),
        !loadedFromStorage
    );
    return getLoadedColumn(chunkX, chunkZ);
}

BedrockChunkColumn* BedrockWorld::getColumnAt(const BlockPosition& pos) {
    return getColumn(chunkCoord(pos.x), chunkCoord(pos.z));
}

void BedrockWorld::setLoadedColumn(
    int32_t chunkX,
    int32_t chunkZ,
    BedrockChunkColumn column,
    bool save
) {
    columns_[key(chunkX, chunkZ)] = std::move(column);
    BedrockWorldColumnEntry entry;
    entry.chunkX = chunkX;
    entry.chunkZ = chunkZ;
    entry.column = columns_.at(key(chunkX, chunkZ));
    for (auto& handler : loadHandlers_) {
        handler(entry);
    }
    if (save && options_.saveColumn) {
        queueSaving(chunkX, chunkZ);
    }
}

void BedrockWorld::setColumn(
    int32_t chunkX,
    int32_t chunkZ,
    BedrockChunkColumn column,
    bool save
) {
    setLoadedColumn(chunkX, chunkZ, std::move(column), save);
}

void BedrockWorld::unloadColumn(int32_t chunkX, int32_t chunkZ) {
    const std::string columnKey = key(chunkX, chunkZ);
    if (columns_.find(columnKey) == columns_.end()) {
        return;
    }
    if (options_.saveColumn && savingQueue_.find(columnKey) != savingQueue_.end()) {
        unloadQueue_[columnKey] = {chunkX, chunkZ};
        return;
    }
    forceUnloadColumn(chunkX, chunkZ);
}

void BedrockWorld::queueSaving(int32_t chunkX, int32_t chunkZ) {
    if (!options_.saveColumn || !hasColumn(chunkX, chunkZ)) {
        return;
    }
    savingQueue_[key(chunkX, chunkZ)] = {chunkX, chunkZ};
}

void BedrockWorld::saveAt(const BlockPosition& pos) {
    queueSaving(chunkCoord(pos.x), chunkCoord(pos.z));
}

void BedrockWorld::saveNow() {
    if (savingQueue_.empty() || !options_.saveColumn) {
        return;
    }

    const auto queue = savingQueue_;
    for (const auto& item : queue) {
        const auto columnIt = columns_.find(item.first);
        if (columnIt == columns_.end()) {
            continue;
        }
        options_.saveColumn(item.second.first, item.second.second, columnIt->second);
    }
    for (const auto& item : queue) {
        savingQueue_.erase(item.first);
    }

    const auto unloads = unloadQueue_;
    for (const auto& item : unloads) {
        forceUnloadColumn(item.second.first, item.second.second);
    }
    for (auto& handler : doneSavingHandlers_) {
        handler();
    }
}

void BedrockWorld::waitSaving() {
    saveNow();
}

std::size_t BedrockWorld::savingQueueSize() const {
    return savingQueue_.size();
}

std::size_t BedrockWorld::unloadQueueSize() const {
    return unloadQueue_.size();
}

std::vector<BedrockWorldColumnEntry> BedrockWorld::getColumns() const {
    std::vector<BedrockWorldColumnEntry> out;
    out.reserve(columns_.size());
    for (const auto& item : columns_) {
        const auto comma = item.first.find(',');
        BedrockWorldColumnEntry entry;
        entry.chunkX = std::stoi(item.first.substr(0, comma));
        entry.chunkZ = std::stoi(item.first.substr(comma + 1));
        entry.column = item.second;
        out.push_back(std::move(entry));
    }
    return out;
}

std::optional<BedrockWorldBlockSnapshot> BedrockWorld::getBlock(
    const BlockPosition& pos
) const {
    const auto* column = getLoadedColumnAt(pos);
    if (!column) {
        return std::nullopt;
    }

    BedrockWorldBlockSnapshot block;
    block.position = pos;
    block.stateId = column->getBlockStateId(pos);
    block.biomeId = column->getBiomeId(pos);
    if (column->getSection(pos.y) != nullptr) {
        block.blockLight = column->getBlockLight(pos);
        block.skyLight = column->getSkyLight(pos);
    }
    if (const auto* entity = column->getBlockEntityNbt(pos)) {
        block.blockEntity = *entity;
    }
    return block;
}

std::optional<BedrockBlock> BedrockWorld::getBlock(
    const BlockPosition& pos,
    const BedrockBlockRegistry& registry,
    bool full
) const {
    const auto* column = getLoadedColumnAt(pos);
    if (column == nullptr) return std::nullopt;
    auto block = column->getBlock(pos, registry, full);
    if (block.has_value()) {
        block->position = WorldBlockPosition {pos.x, pos.y, pos.z};
    }
    return block;
}

void BedrockWorld::setBlock(const BlockPosition& pos, const BedrockBlock& block) {
    auto* column = getLoadedColumnAt(pos);
    if (column == nullptr) return;
    const auto oldBlock = getBlock(pos);
    column->setBlock(pos, block);
    saveAt(pos);
    emitBlockUpdate(*oldBlock, *getBlock(pos));
}

void BedrockWorld::setBlock(
    const BlockPosition& pos,
    const BedrockBlock& block,
    const BedrockBlockRegistry& registry
) {
    auto* column = getLoadedColumnAt(pos);
    if (column == nullptr) return;
    const auto oldBlock = getBlock(pos);
    column->setBlock(pos, block, registry);
    saveAt(pos);
    emitBlockUpdate(*oldBlock, *getBlock(pos));
}

int32_t BedrockWorld::getBlockStateId(const BlockPosition& pos) const {
    const auto* column = getLoadedColumnAt(pos);
    return column ? column->getBlockStateId(pos) : 0;
}

void BedrockWorld::setBlockStateId(const BlockPosition& pos, int32_t stateId) {
    auto* column = getLoadedColumnAt(pos);
    if (!column) {
        return;
    }
    const auto oldBlock = getBlock(pos);
    column->setBlockStateId(pos, stateId);
    saveAt(pos);
    emitBlockUpdate(*oldBlock, *getBlock(pos));
}

uint8_t BedrockWorld::getBlockLight(const BlockPosition& pos) const {
    const auto* column = getLoadedColumnAt(pos);
    return column ? column->getBlockLight(pos) : 0;
}

void BedrockWorld::setBlockLight(const BlockPosition& pos, uint8_t value) {
    auto* column = getLoadedColumnAt(pos);
    if (!column) {
        return;
    }
    const auto oldBlock = getBlock(pos);
    column->setBlockLight(pos, value);
    saveAt(pos);
    emitBlockUpdate(*oldBlock, *getBlock(pos));
}

uint8_t BedrockWorld::getSkyLight(const BlockPosition& pos) const {
    const auto* column = getLoadedColumnAt(pos);
    return column ? column->getSkyLight(pos) : 0;
}

void BedrockWorld::setSkyLight(const BlockPosition& pos, uint8_t value) {
    auto* column = getLoadedColumnAt(pos);
    if (!column) {
        return;
    }
    const auto oldBlock = getBlock(pos);
    column->setSkyLight(pos, value);
    saveAt(pos);
    emitBlockUpdate(*oldBlock, *getBlock(pos));
}

uint32_t BedrockWorld::getBiomeId(const BlockPosition& pos) const {
    const auto* column = getLoadedColumnAt(pos);
    return column ? column->getBiomeId(pos) : 0;
}

void BedrockWorld::setBiomeId(const BlockPosition& pos, uint32_t biomeId) {
    auto* column = getLoadedColumnAt(pos);
    if (!column) {
        return;
    }
    const auto oldBlock = getBlock(pos);
    column->setBiomeId(pos, biomeId);
    saveAt(pos);
    emitBlockUpdate(*oldBlock, *getBlock(pos));
}

std::optional<BedrockWorldRaycastHit> BedrockWorld::raycast(
    const WorldVec3& from,
    const WorldVec3& direction,
    double range,
    BedrockWorldRaycastMatcher matcher,
    BedrockWorldBlockShapeProvider shapeProvider
) const {
    RaycastIterator iterator(from, direction, range);
    while (const auto step = iterator.next()) {
        const BlockPosition position {
            .x = step->x,
            .y = step->y,
            .z = step->z
        };
        const auto* column = getLoadedColumnAt(position);
        if (!column) {
            continue;
        }
        const int32_t stateId = column->getBlockStateId(position);
        if (matcher ? !matcher(stateId, position) : stateId == 0) {
            continue;
        }

        const std::vector<BlockShape> shapes = shapeProvider
            ? shapeProvider(stateId, position)
            : std::vector<BlockShape> {FullBlockShape};
        const auto intersection = iterator.intersect(
            shapes,
            WorldVec3 {
                static_cast<double>(position.x),
                static_cast<double>(position.y),
                static_cast<double>(position.z)
            }
        );
        if (!intersection.has_value()) {
            continue;
        }
        return BedrockWorldRaycastHit {
            position,
            stateId,
            intersection->face,
            intersection->pos
        };
    }
    return std::nullopt;
}

std::size_t BedrockWorld::columnCount() const {
    return columns_.size();
}

std::string BedrockWorld::key(int32_t chunkX, int32_t chunkZ) {
    return std::to_string(chunkX) + "," + std::to_string(chunkZ);
}

std::string BedrockWorld::blockKey(const BlockPosition& pos) {
    return std::to_string(pos.x) + "," + std::to_string(pos.y) + "," +
        std::to_string(pos.z);
}

int32_t BedrockWorld::chunkCoord(int32_t blockCoord) {
    if (blockCoord >= 0) {
        return blockCoord / 16;
    }
    return -(((-blockCoord) + 15) / 16);
}

void BedrockWorld::emitBlockUpdate(
    const BedrockWorldBlockSnapshot& oldBlock,
    const BedrockWorldBlockSnapshot& newBlock
) {
    for (auto& handler : blockUpdateHandlers_) {
        handler(oldBlock, newBlock);
    }
    auto it = positionedBlockUpdateHandlers_.find(blockKey(newBlock.position));
    if (it == positionedBlockUpdateHandlers_.end()) {
        return;
    }
    for (auto& handler : it->second) {
        handler(oldBlock, newBlock);
    }
}

void BedrockWorld::forceUnloadColumn(int32_t chunkX, int32_t chunkZ) {
    const std::string columnKey = key(chunkX, chunkZ);
    auto it = columns_.find(columnKey);
    if (it == columns_.end()) {
        unloadQueue_.erase(columnKey);
        savingQueue_.erase(columnKey);
        return;
    }

    BedrockWorldColumnEntry entry;
    entry.chunkX = chunkX;
    entry.chunkZ = chunkZ;
    entry.column = it->second;
    columns_.erase(it);
    unloadQueue_.erase(columnKey);
    savingQueue_.erase(columnKey);
    for (auto& handler : unloadHandlers_) {
        handler(entry);
    }
}

} // namespace bedrock
