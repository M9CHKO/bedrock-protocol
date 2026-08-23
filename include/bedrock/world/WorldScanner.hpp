#pragma once

#include <bedrock/world/BedrockChunk.hpp>
#include <bedrock/world/WorldView.hpp>
#include <bedrock/world/BlockRuntimeRegistry.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace bedrock {

class WorldScanner {
public:
    explicit WorldScanner(const WorldView& world)
        : world_(world) {}

    std::size_t countRuntimeIdInChunk(
        int32_t chunkX,
        int32_t chunkZ,
        uint32_t runtimeId,
        std::size_t storageIndex = 0
    ) const {
        const auto& chunk = world_.get(chunkX, chunkZ);

        std::size_t count = 0;

        for (std::size_t sub = 0; sub < chunk.subChunks.sections.size(); ++sub) {
            const auto& section = chunk.subChunks.sections[sub];

            if (storageIndex >= section.storages.size()) {
                continue;
            }

            const auto& storage = section.storages[storageIndex];

            for (std::size_t i = 0; i < 4096; ++i) {
                if (storage.getBlockRuntimeId(i) == runtimeId) {
                    ++count;
                }
            }
        }

        return count;
    }

    std::size_t countBlockNameInChunk(
        int32_t chunkX,
        int32_t chunkZ,
        const BlockRuntimeRegistry& registry,
        const std::string& blockName,
        std::size_t storageIndex = 0
    ) const {
        auto runtimeId = registry.runtimeIdOf(blockName);
        if (!runtimeId.has_value()) {
            return 0;
        }

        return countRuntimeIdInChunk(
            chunkX,
            chunkZ,
            *runtimeId,
            storageIndex
        );
    }

    std::optional<BlockPosition> findFirstRuntimeIdInChunk(
        int32_t chunkX,
        int32_t chunkZ,
        uint32_t runtimeId,
        std::size_t storageIndex = 0
    ) const {
        const auto& chunk = world_.get(chunkX, chunkZ);

        for (std::size_t sub = 0; sub < chunk.subChunks.sections.size(); ++sub) {
            const auto& section = chunk.subChunks.sections[sub];

            if (storageIndex >= section.storages.size()) {
                continue;
            }

            const auto& storage = section.storages[storageIndex];

            for (std::size_t i = 0; i < 4096; ++i) {
                if (storage.getBlockRuntimeId(i) != runtimeId) {
                    continue;
                }

                const int32_t localX = static_cast<int32_t>(i & 0x0f);
                const int32_t localZ = static_cast<int32_t>((i >> 4) & 0x0f);
                const int32_t localY = static_cast<int32_t>((i >> 8) & 0x0f);

                return BlockPosition{
                    chunkX * 16 + localX,
                    static_cast<int32_t>(sub * 16) + localY,
                    chunkZ * 16 + localZ
                };
            }
        }

        return std::nullopt;
    }

    std::optional<BlockPosition> findFirstBlockNameInChunk(
        int32_t chunkX,
        int32_t chunkZ,
        const BlockRuntimeRegistry& registry,
        const std::string& blockName,
        std::size_t storageIndex = 0
    ) const {
        auto runtimeId = registry.runtimeIdOf(blockName);
        if (!runtimeId.has_value()) {
            return std::nullopt;
        }

        return findFirstRuntimeIdInChunk(
            chunkX,
            chunkZ,
            *runtimeId,
            storageIndex
        );
    }

private:
    const WorldView& world_;
};

} // namespace bedrock
