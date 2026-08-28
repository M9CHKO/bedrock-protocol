#pragma once

#include <bedrock/item/BedrockItem.hpp>
#include <bedrock/registry/BedrockDefaultSkin.hpp>
#include <bedrock/registry/BedrockFeatureRegistry.hpp>
#include <bedrock/registry/BedrockGameplayRegistry.hpp>
#include <bedrock/registry/BedrockLootRegistry.hpp>
#include <bedrock/world/BedrockBlockRegistry.hpp>
#include <bedrock/world/MinecraftDataIndex.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bedrock {

struct BedrockBiomeClimate {
    double temperature = 0.0;
    double humidity = 0.0;
    double altitude = 0.0;
    double weirdness = 0.0;
    double offset = 0.0;
};

struct BedrockBiomeDefinition {
    int32_t id = 0;
    std::string name;
    std::string category;
    std::optional<std::string> precipitation;
    std::optional<bool> hasPrecipitation;
    std::optional<double> depth;
    std::string dimension;
    std::string displayName;
    uint32_t color = 0;
    std::optional<double> rainfall;
    double temperature = 0.0;
    std::optional<double> height;
    std::optional<int32_t> child;
    std::optional<std::string> parent;
    std::vector<BedrockBiomeClimate> climates;

    bool precipitationEnabled() const;
};

class BedrockBiomeRegistryLoader;

class BedrockBiomeRegistry {
public:
    const BedrockBiomeDefinition* biomeById(int32_t id) const;
    const BedrockBiomeDefinition* biomeByName(std::string_view name) const;

    // prismarine-biome compatibility: unknown IDs become an empty biome with
    // the requested ID, color/rainfall/temperature zero, and null height.
    BedrockBiomeDefinition biome(int32_t id) const;

    const std::vector<BedrockBiomeDefinition>& all() const;
    std::size_t biomeCount() const;

private:
    friend class BedrockBiomeRegistryLoader;

    std::vector<BedrockBiomeDefinition> biomes_;
    std::unordered_map<int32_t, std::size_t> biomeIndicesById_;
    std::unordered_map<std::string, std::size_t> biomeIndicesByName_;

    static std::string normalizeName(std::string_view name);
};

class BedrockBiomeRegistryLoader {
public:
    static BedrockBiomeRegistry loadMinecraftData(
        const std::filesystem::path& biomesJson
    );
};

struct BedrockEntityDefinition {
    int32_t id = 0;
    int32_t internalId = 0;
    std::string name;
    std::string displayName;
    std::optional<double> height;
    std::optional<double> width;
    std::optional<double> length;
    std::optional<double> offset;
    std::string type;
    std::optional<std::string> category;
};

class BedrockEntityRegistryLoader;

class BedrockEntityRegistry {
public:
    const BedrockEntityDefinition* entityById(int32_t id) const;

    // minecraft-data/prismarine-registry indexes are last-write-wins. Some
    // Bedrock datasets intentionally contain duplicate names/internal IDs.
    const BedrockEntityDefinition* entityByName(std::string_view name) const;
    const BedrockEntityDefinition* entityByInternalId(int32_t internalId) const;

    std::vector<const BedrockEntityDefinition*> entitiesByName(
        std::string_view name
    ) const;
    std::vector<const BedrockEntityDefinition*> entitiesByInternalId(
        int32_t internalId
    ) const;

    const std::vector<BedrockEntityDefinition>& all() const;
    std::size_t entityCount() const;
    std::size_t uniqueEntityNameCount() const;

private:
    friend class BedrockEntityRegistryLoader;

    std::vector<BedrockEntityDefinition> entities_;
    std::unordered_map<int32_t, std::size_t> entityIndicesById_;
    std::unordered_map<std::string, std::size_t> entityIndexByName_;
    std::unordered_map<int32_t, std::size_t> entityIndexByInternalId_;
    std::unordered_map<std::string, std::vector<std::size_t>> entityIndicesByName_;
    std::unordered_map<int32_t, std::vector<std::size_t>> entityIndicesByInternalId_;

    static std::string normalizeName(std::string_view name);
};

class BedrockEntityRegistryLoader {
public:
    static BedrockEntityRegistry loadMinecraftData(
        const std::filesystem::path& entitiesJson
    );
};

// One version-aware entry point over the static Bedrock registries used by
// prismarine-block, prismarine-item, prismarine-biome and minecraft-data.
class BedrockRegistry {
public:
    BedrockRegistry(
        MinecraftDataVersionInfo version,
        BedrockBlockRegistry blocks,
        BedrockItemRegistry items,
        BedrockBiomeRegistry biomes,
        BedrockEntityRegistry entities,
        BedrockRecipeRegistry recipes = {},
        BedrockWindowRegistry windows = {},
        BedrockInstrumentRegistry instruments = {},
        BedrockAttributeRegistry attributes = {},
        BedrockFeatureRegistry features = {},
        BedrockLootRegistry loot = {},
        std::optional<BedrockDefaultSkin> defaultSkin = std::nullopt
    );

    const MinecraftDataVersionInfo& version() const;
    int32_t protocolVersion() const;

    BedrockBlockRegistry& blocks();
    const BedrockBlockRegistry& blocks() const;
    BedrockItemRegistry& items();
    const BedrockItemRegistry& items() const;
    BedrockBiomeRegistry& biomes();
    const BedrockBiomeRegistry& biomes() const;
    BedrockEntityRegistry& entities();
    const BedrockEntityRegistry& entities() const;
    BedrockRecipeRegistry& recipes();
    const BedrockRecipeRegistry& recipes() const;
    BedrockWindowRegistry& windows();
    const BedrockWindowRegistry& windows() const;
    BedrockInstrumentRegistry& instruments();
    const BedrockInstrumentRegistry& instruments() const;
    BedrockAttributeRegistry& attributes();
    const BedrockAttributeRegistry& attributes() const;
    BedrockFeatureRegistry& features();
    const BedrockFeatureRegistry& features() const;
    BedrockLootRegistry& loot();
    const BedrockLootRegistry& loot() const;
    BedrockDefaultSkin* defaultSkin();
    const BedrockDefaultSkin* defaultSkin() const;

    // Dynamic prismarine-registry Bedrock API. Modern releases deliver the
    // item palette in item_registry, while older releases embed it in
    // start_game; both feed the same mutable registry.
    void loadItemStates(const std::vector<BedrockItemState>& itemStates);
    void loadItemStates(const ProtoDefValue& itemStates);
    std::vector<BedrockItemState> writeItemStates() const;
    ProtoDefValue writeItemStatesValue() const;
    void handleStartGame(
        const std::vector<BedrockItemState>& itemStates,
        bool blockNetworkIdsAreHashes
    );
    void handleStartGame(const ProtoDefValue& packet);
    void handleItemRegistry(const ProtoDefValue& packet);

    const BedrockBlockDefinition* blockByType(uint32_t type) const;
    const BedrockBlockDefinition* blockByName(std::string_view name) const;
    const BedrockItemDefinition* itemById(int32_t id) const;
    const BedrockItemDefinition* itemByName(std::string_view name) const;
    const BedrockBiomeDefinition* biomeById(int32_t id) const;
    const BedrockBiomeDefinition* biomeByName(std::string_view name) const;
    const BedrockEntityDefinition* entityById(int32_t id) const;
    const BedrockEntityDefinition* entityByName(std::string_view name) const;
    const BedrockRecipeDefinition* recipeById(uint32_t id) const;
    const BedrockRecipeDefinition* recipeByName(std::string_view name) const;
    const BedrockWindowDefinition* windowById(std::string_view id) const;
    const BedrockWindowDefinition* windowByName(std::string_view name) const;
    const BedrockInstrumentDefinition* instrumentById(int32_t id) const;
    const BedrockInstrumentDefinition* instrumentByName(std::string_view name) const;
    const BedrockAttributeDefinition* attributeByName(std::string_view name) const;
    const BedrockAttributeDefinition* attributeByResource(
        std::string_view resource
    ) const;
    ProtoDefValue supportFeature(std::string_view name) const;
    bool supportsFeature(std::string_view name) const;
    const BedrockBlockLootDefinition* blockLootByName(
        std::string_view block
    ) const;
    const BedrockBlockLootDefinition* blockLootForStates(
        std::string_view block,
        const BedrockLootBlockStates& states
    ) const;
    const BedrockEntityLootDefinition* entityLootByName(
        std::string_view entity
    ) const;
    const BedrockBlockStateDefinition* blockStateByRuntimeId(
        int32_t runtimeId
    ) const;
    const BedrockBlockDefinition* blockByRuntimeId(int32_t runtimeId) const;

private:
    MinecraftDataVersionInfo version_;
    BedrockBlockRegistry blocks_;
    BedrockItemRegistry items_;
    BedrockBiomeRegistry biomes_;
    BedrockEntityRegistry entities_;
    BedrockRecipeRegistry recipes_;
    BedrockWindowRegistry windows_;
    BedrockInstrumentRegistry instruments_;
    BedrockAttributeRegistry attributes_;
    BedrockFeatureRegistry features_;
    BedrockLootRegistry loot_;
    std::optional<BedrockDefaultSkin> defaultSkin_;
};

} // namespace bedrock
