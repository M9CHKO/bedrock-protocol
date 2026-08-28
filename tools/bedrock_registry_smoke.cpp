#include <bedrock/registry/BedrockRegistry.hpp>
#include <bedrock/world/MinecraftDataAssets.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(double first, double second, double epsilon = 1e-6) {
    return std::abs(first - second) <= epsilon;
}

} // namespace

int main() {
    try {
        bedrock::MinecraftDataAssets assets;

        const auto paths = assets.resolveByVersion("1.21.100");
        require(paths.version.protocol == 827, "latest Bedrock protocol remap mismatch");
        require(
            paths.biomesDirectory == "1.21.60",
            "latest Bedrock biome directory remap mismatch"
        );
        require(
            paths.entitiesDirectory == "1.21.80",
            "latest Bedrock entity directory remap mismatch"
        );
        require(
            paths.biomesJson.filename() == "biomes.json",
            "biomes.json path was not resolved"
        );
        require(
            paths.entitiesJson.filename() == "entities.json",
            "entities.json path was not resolved"
        );

        auto biomes = assets.loadBedrockBiomeRegistryByVersion("1.21.100");
        require(biomes.biomeCount() == 87, "latest Bedrock biome count mismatch");
        require(biomes.all().size() == 87, "biome array view mismatch");

        const auto* plains = biomes.biomeById(64);
        require(plains != nullptr, "plains biome id lookup failed");
        require(plains->name == "plains", "plains biome name mismatch");
        require(plains->category == "overworld", "plains biome category mismatch");
        require(plains->dimension == "overworld", "plains biome dimension mismatch");
        require(plains->color == 4501493, "plains biome color mismatch");
        require(near(plains->temperature, 0.800000011920929), "plains temperature mismatch");
        require(
            plains->hasPrecipitation.has_value() && *plains->hasPrecipitation,
            "latest precipitation flag mismatch"
        );
        require(plains->precipitationEnabled(), "precipitation helper mismatch");
        require(!plains->precipitation.has_value(), "legacy precipitation leaked into latest data");
        require(!plains->rainfall.has_value(), "legacy rainfall leaked into latest data");
        require(
            biomes.biomeByName("minecraft:plains") == plains,
            "namespaced biome lookup mismatch"
        );
        require(biomes.biomeByName("missing") == nullptr, "unknown biome name matched");

        const auto missingBiome = biomes.biome(9999);
        require(missingBiome.id == 9999, "unknown biome did not retain its id");
        require(missingBiome.name.empty(), "unknown biome name must be empty");
        require(missingBiome.color == 0, "unknown biome color must be zero");
        require(
            missingBiome.rainfall.has_value() && *missingBiome.rainfall == 0.0,
            "unknown biome rainfall must match prismarine-biome"
        );
        require(missingBiome.temperature == 0.0, "unknown biome temperature must be zero");
        require(!missingBiome.height.has_value(), "unknown biome height must be null");

        auto entities = assets.loadBedrockEntityRegistryByProtocol(827);
        require(entities.entityCount() == 122, "latest Bedrock entity count mismatch");
        require(
            entities.uniqueEntityNameCount() == 115,
            "latest unique Bedrock entity name count mismatch"
        );

        const auto* chicken = entities.entityById(0);
        require(chicken != nullptr, "chicken entity id lookup failed");
        require(chicken->internalId == 10, "chicken internal id mismatch");
        require(chicken->name == "chicken", "chicken name mismatch");
        require(chicken->displayName == "Chicken", "chicken display name mismatch");
        require(chicken->height.has_value() && near(*chicken->height, 0.7), "chicken height mismatch");
        require(chicken->width.has_value() && near(*chicken->width, 0.4), "chicken width mismatch");

        const auto* llama = entities.entityByName("minecraft:llama");
        require(llama != nullptr, "llama entity name lookup failed");
        require(
            llama->id == 20 && llama->displayName == "Trader Llama",
            "entityByName must preserve Node last-write-wins indexing"
        );
        const auto llamas = entities.entitiesByName("llama");
        require(llamas.size() == 2, "duplicate llama definitions were lost");
        require(llamas[0]->id == 19 && llamas[1]->id == 20, "llama source order mismatch");

        const auto* internalMinecart = entities.entityByInternalId(98);
        require(
            internalMinecart != nullptr && internalMinecart->id == 93,
            "internal entity id must use last-write-wins lookup"
        );
        require(
            entities.entitiesByInternalId(98).size() == 3,
            "duplicate internal entity ids were lost"
        );
        require(entities.entityById(2)->width == std::nullopt, "nullable entity width mismatch");
        require(entities.entityByName("missing") == nullptr, "unknown entity name matched");

        bool foundMissingCategory = false;
        for (const auto& entity : entities.all()) {
            if (!entity.category.has_value()) {
                foundMissingCategory = true;
                break;
            }
        }
        require(foundMissingCategory, "optional entity category was not preserved");

        auto legacyBiomes = assets.loadBedrockBiomeRegistryByVersion("1.18.11");
        require(legacyBiomes.biomeCount() == 82, "legacy Bedrock biome count mismatch");
        const auto* legacyPlains = legacyBiomes.biomeByName("plains");
        require(legacyPlains != nullptr && legacyPlains->id == 1, "legacy plains lookup failed");
        require(
            legacyPlains->precipitation == std::optional<std::string>("rain"),
            "legacy precipitation value mismatch"
        );
        require(!legacyPlains->hasPrecipitation.has_value(), "new flag leaked into legacy data");
        require(
            legacyPlains->rainfall.has_value() && near(*legacyPlains->rainfall, 0.4),
            "legacy rainfall mismatch"
        );
        require(
            legacyPlains->depth.has_value() && near(*legacyPlains->depth, 0.125),
            "legacy biome depth mismatch"
        );
        require(legacyPlains->child == 129, "legacy biome child relation mismatch");
        require(legacyPlains->precipitationEnabled(), "legacy precipitation helper mismatch");

        const auto* basaltDeltas = legacyBiomes.biomeByName("basalt_deltas");
        require(
            basaltDeltas != nullptr && basaltDeltas->climates.size() == 1,
            "legacy biome climate metadata mismatch"
        );
        require(
            near(basaltDeltas->climates.front().offset, 0.175),
            "legacy biome climate offset mismatch"
        );

        auto registry = assets.loadBedrockRegistryByVersion("1.21.100");
        require(registry.protocolVersion() == 827, "unified registry protocol mismatch");
        require(
            registry.version().minecraftVersion == "1.21.100",
            "unified registry Minecraft version mismatch"
        );
        require(registry.blocks().blockCount() > 0, "unified block registry is empty");
        require(registry.items().itemCount() == 1836, "unified item registry mismatch");
        require(registry.biomes().biomeCount() == 87, "unified biome registry mismatch");
        require(registry.entities().entityCount() == 122, "unified entity registry mismatch");
        require(registry.recipes().recipeCount() == 2434, "unified recipe registry mismatch");
        require(registry.windows().windowCount() == 14, "unified window registry mismatch");
        require(
            registry.instruments().instrumentCount() == 16,
            "unified instrument registry mismatch"
        );
        require(
            registry.attributes().attributeCount() == 0,
            "unavailable latest attributes leaked into the unified registry"
        );
        require(registry.features().featureCount() == 16, "unified feature registry mismatch");
        require(
            registry.loot().blockLootCount() == 0 &&
                registry.loot().entityLootCount() == 0,
            "unavailable latest loot leaked into the unified registry"
        );
        require(registry.defaultSkin() != nullptr, "unified default skin is unavailable");
        require(
            registry.defaultSkin()->skinId() == "persona-ec4b0e51bc40322b-0",
            "unified default skin mismatch"
        );
        require(registry.supportsFeature("blockHashes"), "feature facade lookup failed");
        require(
            registry.blocks().supportsBlockHashes(),
            "block registry did not consume the blockHashes feature"
        );
        require(
            !registry.items().usesAuxValue(),
            "item registry did not consume the auxiliary-value feature"
        );
        require(registry.blockByName("minecraft:stone") != nullptr, "block facade lookup failed");
        require(registry.itemByName("minecraft:diamond_sword") != nullptr, "item facade lookup failed");
        require(registry.biomeByName("plains") != nullptr, "biome facade lookup failed");
        require(registry.entityByName("chicken") != nullptr, "entity facade lookup failed");
        require(registry.recipeById(0) != nullptr, "recipe facade lookup failed");
        require(registry.windowById("inventory") != nullptr, "window facade lookup failed");
        require(registry.instrumentByName("harp") != nullptr, "instrument facade lookup failed");
        require(
            registry.attributeByName("health") == nullptr,
            "attribute facade must reflect the selected version"
        );
        require(registry.blockLootByName("stone") == nullptr, "loot facade leaked data");
        require(
            registry.entityLootByName("zombie") == nullptr,
            "entity loot facade leaked data"
        );

        std::cout << "bedrock registry smoke ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bedrock registry smoke failed: " << error.what() << '\n';
        return 1;
    }
}
