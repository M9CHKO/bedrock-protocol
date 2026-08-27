#pragma once

#include <bedrock/world/LevelChunkView.hpp>

#include <cstdint>
#include <map>
#include <stdexcept>
#include <utility>
#include <cmath>

namespace bedrock {

struct ChunkPos {
    int32_t x = 0;
    int32_t z = 0;

    bool operator<(const ChunkPos& other) const {
        if (x != other.x) return x < other.x;
        return z < other.z;
    }
};

class WorldView {
public:
    void put(LevelChunkView chunk) {
        ChunkPos pos{chunk.header.chunkX, chunk.header.chunkZ};
        chunks_[pos] = std::move(chunk);
    }

    bool has(int32_t chunkX, int32_t chunkZ) const {
        return chunks_.find({chunkX, chunkZ}) != chunks_.end();
    }

    std::size_t size() const {
        return chunks_.size();
    }

    const LevelChunkView& get(int32_t chunkX, int32_t chunkZ) const {
        auto it = chunks_.find({chunkX, chunkZ});
        if (it == chunks_.end()) {
            throw std::runtime_error("chunk not found");
        }

        return it->second;
    }

    uint32_t getRuntimeId(
        int32_t chunkX,
        int32_t chunkZ,
        std::size_t subChunkIndex,
        std::size_t storageIndex,
        std::size_t blockIndex
    ) const {
        return get(chunkX, chunkZ).getRuntimeId(
            subChunkIndex,
            storageIndex,
            blockIndex
        );
    }

    uint32_t getRuntimeIdAt(
        int32_t x,
        int32_t y,
        int32_t z,
        std::size_t storageIndex = 0
    ) const {
        const int32_t chunkX = floorDiv16(x);
        const int32_t chunkZ = floorDiv16(z);

        const int32_t localX = floorMod16(x);
        const int32_t localZ = floorMod16(z);

        const int32_t sectionY = floorDiv16(y);
        const std::size_t localY = static_cast<std::size_t>(floorMod16(y));

        const std::size_t blockIndex =
            (static_cast<std::size_t>(localX) * 16 * 16) +
            (static_cast<std::size_t>(localZ) * 16) +
            localY;

        const auto& chunk = get(chunkX, chunkZ);
        for (std::size_t index = 0; index < chunk.subChunks.sections.size(); ++index) {
            const auto& section = chunk.subChunks.sections[index];
            const int32_t decodedY = section.version >= 9
                ? static_cast<int32_t>(section.yIndex)
                : static_cast<int32_t>(index);
            if (decodedY != sectionY) {
                continue;
            }
            return chunk.getRuntimeId(index, storageIndex, blockIndex);
        }

        throw std::runtime_error("subchunk not found for block y");
    }

    void clear() {
        chunks_.clear();
    }

private:
    static int32_t floorDiv16(int32_t value) {
        if (value >= 0) {
            return value / 16;
        }

        return -(((-value) + 15) / 16);
    }

    static int32_t floorMod16(int32_t value) {
        int32_t mod = value % 16;
        if (mod < 0) mod += 16;
        return mod;
    }

    std::map<ChunkPos, LevelChunkView> chunks_;
};

} // namespace bedrock
