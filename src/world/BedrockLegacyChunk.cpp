#include <bedrock/world/BedrockLegacyChunk.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace bedrock {
namespace {

constexpr std::size_t Flat014BlockSize = 16u * 16u * 128u;
constexpr std::size_t Flat014NibbleSize = Flat014BlockSize / 2u;
constexpr std::size_t Flat014HeightOffset =
    Flat014BlockSize + Flat014NibbleSize * 3u;
constexpr std::size_t Flat014BiomeOffset = Flat014HeightOffset + 256u;

constexpr std::size_t Sub10BlockSize = 16u * 16u * 16u;
constexpr std::size_t Sub10NibbleSize = Sub10BlockSize / 2u;

void requireHorizontalPosition(const BlockPosition& pos) {
    if (pos.x < 0 || pos.x >= 16 || pos.z < 0 || pos.z >= 16) {
        throw BedrockChunkError("legacy chunk x/z coordinate outside 0..15");
    }
}

void requireLocalSubChunkPosition(const BlockPosition& pos) {
    requireHorizontalPosition(pos);
    if (pos.y < 0 || pos.y >= 16) {
        throw BedrockChunkError("legacy subchunk y coordinate outside 0..15");
    }
}

void requireNibble(uint8_t value, const char* field) {
    if (value >= 16) {
        throw BedrockChunkError(std::string(field) + " must be below 16");
    }
}

uint8_t readNibble(const std::vector<uint8_t>& data, std::size_t nibbleIndex) {
    const std::size_t byteIndex = nibbleIndex / 2u;
    if (byteIndex >= data.size()) {
        throw BedrockChunkError("legacy chunk nibble offset outside buffer");
    }
    return (nibbleIndex & 1u) == 0
        ? static_cast<uint8_t>(data[byteIndex] & 0x0fu)
        : static_cast<uint8_t>(data[byteIndex] >> 4u);
}

void writeNibble(
    std::vector<uint8_t>& data,
    std::size_t nibbleIndex,
    uint8_t value,
    const char* field
) {
    requireNibble(value, field);
    const std::size_t byteIndex = nibbleIndex / 2u;
    if (byteIndex >= data.size()) {
        throw BedrockChunkError("legacy chunk nibble offset outside buffer");
    }
    if ((nibbleIndex & 1u) == 0) {
        data[byteIndex] = static_cast<uint8_t>((data[byteIndex] & 0xf0u) | value);
    } else {
        data[byteIndex] = static_cast<uint8_t>((data[byteIndex] & 0x0fu) | (value << 4u));
    }
}

uint32_t readU32BE(const std::vector<uint8_t>& data, std::size_t offset) {
    if (offset + 4u > data.size()) {
        throw BedrockChunkError("legacy chunk uint32 offset outside buffer");
    }
    return (static_cast<uint32_t>(data[offset]) << 24u) |
        (static_cast<uint32_t>(data[offset + 1u]) << 16u) |
        (static_cast<uint32_t>(data[offset + 2u]) << 8u) |
        static_cast<uint32_t>(data[offset + 3u]);
}

void writeU32BE(std::vector<uint8_t>& data, std::size_t offset, uint32_t value) {
    if (offset + 4u > data.size()) {
        throw BedrockChunkError("legacy chunk uint32 offset outside buffer");
    }
    data[offset] = static_cast<uint8_t>(value >> 24u);
    data[offset + 1u] = static_cast<uint8_t>(value >> 16u);
    data[offset + 2u] = static_cast<uint8_t>(value >> 8u);
    data[offset + 3u] = static_cast<uint8_t>(value);
}

std::size_t flat014Index(const BlockPosition& pos) {
    return static_cast<std::size_t>(pos.x) + 16u * (
        static_cast<std::size_t>(pos.z) + 16u * static_cast<std::size_t>(pos.y)
    );
}

std::size_t column2dIndex(const BlockPosition& pos) {
    requireHorizontalPosition(pos);
    return static_cast<std::size_t>(pos.z) * 16u + static_cast<std::size_t>(pos.x);
}

std::size_t sub10Index(const BlockPosition& pos) {
    requireLocalSubChunkPosition(pos);
    return 1u + static_cast<std::size_t>(pos.x) * 256u +
        static_cast<std::size_t>(pos.z) * 16u + static_cast<std::size_t>(pos.y);
}

BlockPosition localSubChunkPosition(const BlockPosition& pos) {
    return BlockPosition {
        .x = pos.x,
        .y = pos.y & 0x0f,
        .z = pos.z,
        .layer = pos.layer
    };
}

uint8_t checkedLegacyType(uint32_t type) {
    if (type > std::numeric_limits<uint8_t>::max()) {
        throw BedrockChunkError("legacy block type must fit in one byte");
    }
    return static_cast<uint8_t>(type);
}

} // namespace

BedrockChunk014::BedrockChunk014()
    : data_(BufferSize, 0) {}

void BedrockChunk014::initialize(const BedrockLegacyChunkInitializer& initializer) {
    if (!initializer) return;
    for (int32_t y = 0; y < Height; ++y) {
        for (int32_t z = 0; z < Length; ++z) {
            for (int32_t x = 0; x < Width; ++x) {
                auto block = initializer(x, y, z);
                if (block.has_value()) {
                    setBlock({.x = x, .y = y, .z = z}, *block);
                }
            }
        }
    }
}

std::optional<BedrockBlock> BedrockChunk014::getBlock(
    const BlockPosition& pos,
    const BedrockBlockRegistry& registry
) const {
    const int32_t stateId = (static_cast<int32_t>(getBlockType(pos)) << 4) |
        static_cast<int32_t>(getBlockData(pos));
    auto block = registry.fromStateId(stateId, getBiome(pos));
    if (block.has_value()) {
        block->light = getBlockLight(pos);
        block->skyLight = getSkyLight(pos);
    }
    return block;
}

void BedrockChunk014::setBlock(const BlockPosition& pos, const BedrockBlock& block) {
    setBlockType(pos, block.type);
    setBlockData(pos, static_cast<uint8_t>(block.metadata));
    setBiome(pos, block.biomeId);
    setSkyLight(pos, block.skyLight);
    setBlockLight(pos, block.light);
}

uint8_t BedrockChunk014::getBlockType(const BlockPosition& pos) const {
    if (pos.y < 0 || pos.y >= Height) return 0;
    requireHorizontalPosition(pos);
    return data_[flat014Index(pos)];
}

void BedrockChunk014::setBlockType(const BlockPosition& pos, uint32_t type) {
    if (pos.y < 0 || pos.y >= Height) return;
    requireHorizontalPosition(pos);
    data_[flat014Index(pos)] = checkedLegacyType(type);
}

uint8_t BedrockChunk014::getBlockData(const BlockPosition& pos) const {
    if (pos.y < 0 || pos.y >= Height) return 0;
    requireHorizontalPosition(pos);
    return readNibble(data_, Flat014BlockSize * 2u + flat014Index(pos));
}

void BedrockChunk014::setBlockData(const BlockPosition& pos, uint8_t value) {
    if (pos.y < 0 || pos.y >= Height) return;
    requireHorizontalPosition(pos);
    writeNibble(data_, Flat014BlockSize * 2u + flat014Index(pos), value, "block data");
}

uint8_t BedrockChunk014::getBlockLight(const BlockPosition& pos) const {
    if (pos.y < 0 || pos.y >= Height) return 0;
    requireHorizontalPosition(pos);
    return readNibble(
        data_,
        (Flat014BlockSize + Flat014NibbleSize) * 2u + flat014Index(pos)
    );
}

void BedrockChunk014::setBlockLight(const BlockPosition& pos, uint8_t light) {
    if (pos.y < 0 || pos.y >= Height) return;
    requireHorizontalPosition(pos);
    writeNibble(
        data_,
        (Flat014BlockSize + Flat014NibbleSize) * 2u + flat014Index(pos),
        light,
        "block light"
    );
}

uint8_t BedrockChunk014::getSkyLight(const BlockPosition& pos) const {
    if (pos.y < 0 || pos.y >= Height) return 0;
    requireHorizontalPosition(pos);
    return readNibble(
        data_,
        (Flat014BlockSize + Flat014NibbleSize * 2u) * 2u + flat014Index(pos)
    );
}

void BedrockChunk014::setSkyLight(const BlockPosition& pos, uint8_t light) {
    if (pos.y < 0 || pos.y >= Height) return;
    requireHorizontalPosition(pos);
    writeNibble(
        data_,
        (Flat014BlockSize + Flat014NibbleSize * 2u) * 2u + flat014Index(pos),
        light,
        "sky light"
    );
}

BedrockBiomeColor BedrockChunk014::getBiomeColor(const BlockPosition& pos) const {
    const uint32_t value = readU32BE(data_, Flat014BiomeOffset + column2dIndex(pos) * 4u);
    return {
        static_cast<uint8_t>(value >> 16u),
        static_cast<uint8_t>(value >> 8u),
        static_cast<uint8_t>(value)
    };
}

void BedrockChunk014::setBiomeColor(
    const BlockPosition& pos,
    uint8_t r,
    uint8_t g,
    uint8_t b
) {
    const std::size_t offset = Flat014BiomeOffset + column2dIndex(pos) * 4u;
    const uint32_t old = readU32BE(data_, offset);
    writeU32BE(
        data_,
        offset,
        (old & 0xff000000u) | (static_cast<uint32_t>(r) << 16u) |
            (static_cast<uint32_t>(g) << 8u) | static_cast<uint32_t>(b)
    );
}

int32_t BedrockChunk014::getBiome(const BlockPosition& pos) const {
    const uint32_t value = readU32BE(data_, Flat014BiomeOffset + column2dIndex(pos) * 4u);
    return static_cast<int32_t>(static_cast<int8_t>(value >> 24u));
}

void BedrockChunk014::setBiome(const BlockPosition& pos, int32_t id) {
    const std::size_t offset = Flat014BiomeOffset + column2dIndex(pos) * 4u;
    const uint32_t old = readU32BE(data_, offset);
    writeU32BE(
        data_,
        offset,
        (old & 0x00ffffffu) | (static_cast<uint32_t>(id) & 0xffu) << 24u
    );
}

uint8_t BedrockChunk014::getHeight(const BlockPosition& pos) const {
    return data_[Flat014HeightOffset + column2dIndex(pos)];
}

void BedrockChunk014::setHeight(const BlockPosition& pos, uint8_t value) {
    data_[Flat014HeightOffset + column2dIndex(pos)] = value;
}

void BedrockChunk014::load(std::vector<uint8_t> data) {
    if (data.size() != BufferSize) {
        throw BedrockChunkError(
            "Bedrock 0.14 chunk buffer has size " + std::to_string(data.size()) +
            ", expected " + std::to_string(BufferSize)
        );
    }
    data_ = std::move(data);
}

std::vector<uint8_t> BedrockChunk014::dump() const {
    return data_;
}

const std::vector<uint8_t>& BedrockChunk014::data() const noexcept {
    return data_;
}

uint16_t BedrockChunk014::getMask() const noexcept {
    return 0xffffu;
}

BedrockSubChunk10::BedrockSubChunk10()
    : data_(BufferSize, 0) {}

uint8_t BedrockSubChunk10::getBlockType(const BlockPosition& pos) const {
    return data_[sub10Index(pos)];
}

void BedrockSubChunk10::setBlockType(const BlockPosition& pos, uint32_t type) {
    data_[sub10Index(pos)] = checkedLegacyType(type);
}

uint8_t BedrockSubChunk10::getBlockData(const BlockPosition& pos) const {
    return readNibble(data_, Sub10BlockSize * 2u + sub10Index(pos));
}

void BedrockSubChunk10::setBlockData(const BlockPosition& pos, uint8_t value) {
    writeNibble(data_, Sub10BlockSize * 2u + sub10Index(pos), value, "block data");
}

uint8_t BedrockSubChunk10::getBlockLight(const BlockPosition& pos) const {
    return readNibble(
        data_,
        (Sub10BlockSize + Sub10NibbleSize * 2u) * 2u + sub10Index(pos)
    );
}

void BedrockSubChunk10::setBlockLight(const BlockPosition& pos, uint8_t light) {
    writeNibble(
        data_,
        (Sub10BlockSize + Sub10NibbleSize * 2u) * 2u + sub10Index(pos),
        light,
        "block light"
    );
}

uint8_t BedrockSubChunk10::getSkyLight(const BlockPosition& pos) const {
    return readNibble(
        data_,
        (Sub10BlockSize + Sub10NibbleSize) * 2u + sub10Index(pos)
    );
}

void BedrockSubChunk10::setSkyLight(const BlockPosition& pos, uint8_t light) {
    writeNibble(
        data_,
        (Sub10BlockSize + Sub10NibbleSize) * 2u + sub10Index(pos),
        light,
        "sky light"
    );
}

void BedrockSubChunk10::load(std::vector<uint8_t> data) {
    if (data.size() != BufferSize) {
        throw BedrockChunkError(
            "Bedrock 1.0 subchunk buffer has size " + std::to_string(data.size()) +
            ", expected " + std::to_string(BufferSize)
        );
    }
    data_ = std::move(data);
}

std::vector<uint8_t> BedrockSubChunk10::dump() const {
    return data_;
}

const std::vector<uint8_t>& BedrockSubChunk10::data() const noexcept {
    return data_;
}

BedrockChunk10::BedrockChunk10() {
    std::fill_n(data_.begin(), 256, static_cast<uint8_t>(1));
}

void BedrockChunk10::initialize(const BedrockLegacyChunkInitializer& initializer) {
    if (!initializer) return;
    for (int32_t y = 0; y < Height; ++y) {
        for (int32_t z = 0; z < Length; ++z) {
            for (int32_t x = 0; x < Width; ++x) {
                auto block = initializer(x, y, z);
                if (block.has_value()) {
                    setBlock({.x = x, .y = y, .z = z}, *block);
                }
            }
        }
    }
}

std::optional<BedrockBlock> BedrockChunk10::getBlock(
    const BlockPosition& pos,
    const BedrockBlockRegistry& registry
) const {
    const int32_t stateId = (static_cast<int32_t>(getBlockType(pos)) << 4) |
        static_cast<int32_t>(getBlockData(pos));
    auto block = registry.fromStateId(stateId, getBiome(pos));
    if (block.has_value()) {
        block->light = getBlockLight(pos);
        block->skyLight = getSkyLight(pos);
    }
    return block;
}

void BedrockChunk10::setBlock(const BlockPosition& pos, const BedrockBlock& block) {
    setBlockType(pos, block.type);
    setBlockData(pos, static_cast<uint8_t>(block.metadata));
    setBiome(pos, static_cast<uint8_t>(block.biomeId));
    setSkyLight(pos, block.skyLight);
    setBlockLight(pos, block.light);
}

uint8_t BedrockChunk10::getBlockType(const BlockPosition& pos) const {
    const auto* section = getSection(pos.y >> 4);
    return section == nullptr ? 0 : section->getBlockType(localSubChunkPosition(pos));
}

void BedrockChunk10::setBlockType(const BlockPosition& pos, uint32_t type) {
    auto* section = getSection(pos.y >> 4);
    if (section != nullptr) section->setBlockType(localSubChunkPosition(pos), type);
}

uint8_t BedrockChunk10::getBlockData(const BlockPosition& pos) const {
    const auto* section = getSection(pos.y >> 4);
    return section == nullptr ? 0 : section->getBlockData(localSubChunkPosition(pos));
}

void BedrockChunk10::setBlockData(const BlockPosition& pos, uint8_t value) {
    auto* section = getSection(pos.y >> 4);
    if (section != nullptr) section->setBlockData(localSubChunkPosition(pos), value);
}

uint8_t BedrockChunk10::getBlockLight(const BlockPosition& pos) const {
    const auto* section = getSection(pos.y >> 4);
    return section == nullptr ? 0 : section->getBlockLight(localSubChunkPosition(pos));
}

void BedrockChunk10::setBlockLight(const BlockPosition& pos, uint8_t light) {
    auto* section = getSection(pos.y >> 4);
    if (section != nullptr) section->setBlockLight(localSubChunkPosition(pos), light);
}

uint8_t BedrockChunk10::getSkyLight(const BlockPosition& pos) const {
    const auto* section = getSection(pos.y >> 4);
    return section == nullptr ? 15 : section->getSkyLight(localSubChunkPosition(pos));
}

void BedrockChunk10::setSkyLight(const BlockPosition& pos, uint8_t light) {
    auto* section = getSection(pos.y >> 4);
    if (section != nullptr) section->setSkyLight(localSubChunkPosition(pos), light);
}

BedrockBiomeColor BedrockChunk10::getBiomeColor(const BlockPosition&) const noexcept {
    return {};
}

void BedrockChunk10::setBiomeColor(
    const BlockPosition&,
    uint8_t,
    uint8_t,
    uint8_t
) noexcept {}

uint8_t BedrockChunk10::getBiome(const BlockPosition& pos) const {
    return data_[column2dIndex(pos)];
}

void BedrockChunk10::setBiome(const BlockPosition& pos, uint8_t id) {
    data_[column2dIndex(pos)] = id;
}

uint8_t BedrockChunk10::getHeight(const BlockPosition& pos) const {
    // This intentionally shares the biome byte, matching the installed
    // prismarine-chunk 1.0 implementation.
    return data_[column2dIndex(pos)];
}

void BedrockChunk10::setHeight(const BlockPosition& pos, uint8_t height) {
    data_[column2dIndex(pos)] = height;
}

BedrockSubChunk10* BedrockChunk10::getSection(int32_t sectionY) {
    if (sectionY < 0 || sectionY >= static_cast<int32_t>(sections_.size())) return nullptr;
    return &sections_[static_cast<std::size_t>(sectionY)];
}

const BedrockSubChunk10* BedrockChunk10::getSection(int32_t sectionY) const {
    if (sectionY < 0 || sectionY >= static_cast<int32_t>(sections_.size())) return nullptr;
    return &sections_[static_cast<std::size_t>(sectionY)];
}

const std::array<BedrockSubChunk10, 16>& BedrockChunk10::sections() const noexcept {
    return sections_;
}

void BedrockChunk10::load(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        throw BedrockChunkError("Bedrock 1.0 chunk buffer is empty");
    }
    const std::size_t sectionCount = data.front();
    const std::size_t required = 1u + sectionCount * BedrockSubChunk10::BufferSize;
    if (sectionCount > sections_.size() || required > data.size()) {
        throw BedrockChunkError(
            "Bedrock 1.0 chunk buffer has size " + std::to_string(data.size()) +
            ", expected at least " + std::to_string(required)
        );
    }

    std::size_t offset = 1;
    for (std::size_t i = 0; i < sectionCount; ++i) {
        sections_[i].load(std::vector<uint8_t>(
            data.begin() + static_cast<std::ptrdiff_t>(offset),
            data.begin() + static_cast<std::ptrdiff_t>(offset + BedrockSubChunk10::BufferSize)
        ));
        offset += BedrockSubChunk10::BufferSize;
    }
    // prismarine-chunk 1.0 intentionally ignores the trailing biome/height
    // payload while loading.
}

std::size_t BedrockChunk10::size() const noexcept {
    return BufferSize;
}

std::vector<uint8_t> BedrockChunk10::dump() const {
    std::vector<uint8_t> out;
    out.reserve(BufferSize);
    out.push_back(static_cast<uint8_t>(sections_.size()));
    for (const auto& section : sections_) {
        const auto bytes = section.dump();
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    out.insert(out.end(), data_.begin(), data_.end());
    out.push_back(0); // border block count
    out.push_back(0); // signed-varint extra-data count
    return out;
}

uint16_t BedrockChunk10::getMask() const noexcept {
    return 0xffffu;
}

} // namespace bedrock
