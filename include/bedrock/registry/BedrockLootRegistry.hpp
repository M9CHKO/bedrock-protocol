#pragma once

#include <bedrock/protodef/ProtoDefValue.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bedrock {

struct BedrockLootDrop {
    std::string item;
    std::optional<int32_t> metadata;
    double dropChance = 0.0;
    std::vector<std::optional<double>> stackSizeRange;
    std::optional<double> blockAge;
    std::optional<bool> silkTouch;
    std::optional<bool> noSilkTouch;
    std::optional<bool> playerKill;

    std::optional<double> minimumStackSize() const;
    std::optional<double> maximumStackSize() const;
};

using BedrockLootBlockStates = std::unordered_map<std::string, ProtoDefValue>;

struct BedrockBlockLootDefinition {
    std::string block;
    BedrockLootBlockStates states;
    std::vector<BedrockLootDrop> drops;

    // A definition's state conditions must all be present in the supplied
    // full block state. Extra supplied states are allowed.
    bool matchesStates(const BedrockLootBlockStates& blockStates) const;
};

struct BedrockEntityLootDefinition {
    std::string entity;
    std::vector<BedrockLootDrop> drops;
};

class BedrockLootRegistryLoader;

class BedrockLootRegistry {
public:
    // minecraft-data indexes block loot by block name with last-write-wins.
    // Use blockLootVariants or blockLootForStates to retain Bedrock variants.
    const BedrockBlockLootDefinition* blockLootByName(
        std::string_view block
    ) const;
    std::vector<const BedrockBlockLootDefinition*> blockLootVariants(
        std::string_view block
    ) const;
    const BedrockBlockLootDefinition* blockLootForStates(
        std::string_view block,
        const BedrockLootBlockStates& states
    ) const;

    const BedrockEntityLootDefinition* entityLootByName(
        std::string_view entity
    ) const;

    const std::vector<BedrockBlockLootDefinition>& allBlockLoot() const;
    const std::vector<BedrockEntityLootDefinition>& allEntityLoot() const;
    std::size_t blockLootCount() const;
    std::size_t uniqueBlockLootCount() const;
    std::size_t entityLootCount() const;

private:
    friend class BedrockLootRegistryLoader;

    std::vector<BedrockBlockLootDefinition> blockLoot_;
    std::vector<BedrockEntityLootDefinition> entityLoot_;
    std::unordered_map<std::string, std::size_t> blockLootIndexByName_;
    std::unordered_map<std::string, std::vector<std::size_t>>
        blockLootIndicesByName_;
    std::unordered_map<std::string, std::size_t> entityLootIndexByName_;

    static std::string normalizeName(std::string_view name);
};

class BedrockLootRegistryLoader {
public:
    static BedrockLootRegistry loadMinecraftData(
        const std::filesystem::path& blockLootJson,
        const std::filesystem::path& entityLootJson
    );
};

} // namespace bedrock
