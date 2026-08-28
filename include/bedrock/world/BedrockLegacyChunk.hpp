#pragma once

#include <bedrock/world/BedrockChunk.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace bedrock {

struct BedrockBiomeColor {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

using BedrockLegacyChunkInitializer = std::function<std::optional<BedrockBlock>(
    int32_t x,
    int32_t y,
    int32_t z
)>;

// Flat 128-block-high chunk used by the Bedrock 0.14 implementation in
// prismarine-chunk.
class BedrockChunk014 {
public:
    static constexpr int32_t Width = 16;
    static constexpr int32_t Length = 16;
    static constexpr int32_t Height = 128;
    static constexpr std::size_t BufferSize = 83200;

    BedrockChunk014();

    void initialize(const BedrockLegacyChunkInitializer& initializer);
    std::optional<BedrockBlock> getBlock(
        const BlockPosition& pos,
        const BedrockBlockRegistry& registry
    ) const;
    void setBlock(const BlockPosition& pos, const BedrockBlock& block);

    uint8_t getBlockType(const BlockPosition& pos) const;
    void setBlockType(const BlockPosition& pos, uint32_t type);
    uint8_t getBlockData(const BlockPosition& pos) const;
    void setBlockData(const BlockPosition& pos, uint8_t data);
    uint8_t getBlockLight(const BlockPosition& pos) const;
    void setBlockLight(const BlockPosition& pos, uint8_t light);
    uint8_t getSkyLight(const BlockPosition& pos) const;
    void setSkyLight(const BlockPosition& pos, uint8_t light);

    BedrockBiomeColor getBiomeColor(const BlockPosition& pos) const;
    void setBiomeColor(const BlockPosition& pos, uint8_t r, uint8_t g, uint8_t b);
    int32_t getBiome(const BlockPosition& pos) const;
    void setBiome(const BlockPosition& pos, int32_t id);
    uint8_t getHeight(const BlockPosition& pos) const;
    void setHeight(const BlockPosition& pos, uint8_t value);

    void load(std::vector<uint8_t> data);
    std::vector<uint8_t> dump() const;
    const std::vector<uint8_t>& data() const noexcept;
    uint16_t getMask() const noexcept;

    void dumpBiomes() const noexcept {}
    void dumpLight() const noexcept {}
    void loadLight() noexcept {}
    void loadBiomes() noexcept {}

private:
    std::vector<uint8_t> data_;
};

// Fixed 16x16x16 subchunk used by Bedrock 1.0. Its byte/nibble offsets mirror
// the historical JavaScript implementation exactly.
class BedrockSubChunk10 {
public:
    static constexpr int32_t Width = 16;
    static constexpr int32_t Length = 16;
    static constexpr int32_t Height = 16;
    static constexpr std::size_t BufferSize = 10241;

    BedrockSubChunk10();

    uint8_t getBlockType(const BlockPosition& pos) const;
    void setBlockType(const BlockPosition& pos, uint32_t type);
    uint8_t getBlockData(const BlockPosition& pos) const;
    void setBlockData(const BlockPosition& pos, uint8_t data);
    uint8_t getBlockLight(const BlockPosition& pos) const;
    void setBlockLight(const BlockPosition& pos, uint8_t light);
    uint8_t getSkyLight(const BlockPosition& pos) const;
    void setSkyLight(const BlockPosition& pos, uint8_t light);

    void load(std::vector<uint8_t> data);
    std::vector<uint8_t> dump() const;
    const std::vector<uint8_t>& data() const noexcept;

private:
    std::vector<uint8_t> data_;
};

// Sixteen fixed subchunks plus the 2D biome/height trailer used by Bedrock 1.0.
class BedrockChunk10 {
public:
    static constexpr int32_t Width = 16;
    static constexpr int32_t Length = 16;
    static constexpr int32_t Height = 256;
    static constexpr std::size_t BufferSize = 164627;

    BedrockChunk10();

    void initialize(const BedrockLegacyChunkInitializer& initializer);
    std::optional<BedrockBlock> getBlock(
        const BlockPosition& pos,
        const BedrockBlockRegistry& registry
    ) const;
    void setBlock(const BlockPosition& pos, const BedrockBlock& block);

    uint8_t getBlockType(const BlockPosition& pos) const;
    void setBlockType(const BlockPosition& pos, uint32_t type);
    uint8_t getBlockData(const BlockPosition& pos) const;
    void setBlockData(const BlockPosition& pos, uint8_t data);
    uint8_t getBlockLight(const BlockPosition& pos) const;
    void setBlockLight(const BlockPosition& pos, uint8_t light);
    uint8_t getSkyLight(const BlockPosition& pos) const;
    void setSkyLight(const BlockPosition& pos, uint8_t light);

    BedrockBiomeColor getBiomeColor(const BlockPosition& pos) const noexcept;
    void setBiomeColor(const BlockPosition& pos, uint8_t r, uint8_t g, uint8_t b) noexcept;
    uint8_t getBiome(const BlockPosition& pos) const;
    void setBiome(const BlockPosition& pos, uint8_t id);
    uint8_t getHeight(const BlockPosition& pos) const;
    void setHeight(const BlockPosition& pos, uint8_t height);

    BedrockSubChunk10* getSection(int32_t sectionY);
    const BedrockSubChunk10* getSection(int32_t sectionY) const;
    const std::array<BedrockSubChunk10, 16>& sections() const noexcept;

    void load(const std::vector<uint8_t>& data);
    std::size_t size() const noexcept;
    std::vector<uint8_t> dump() const;
    uint16_t getMask() const noexcept;

    void dumpBiomes() const noexcept {}
    void dumpLight() const noexcept {}
    void loadLight() noexcept {}
    void loadBiomes() noexcept {}

private:
    std::array<BedrockSubChunk10, 16> sections_;
    std::array<uint8_t, 768> data_ {};
};

using BedrockLegacyChunk014 = BedrockChunk014;
using BedrockLegacySubChunk10 = BedrockSubChunk10;
using BedrockLegacyChunk10 = BedrockChunk10;

} // namespace bedrock
