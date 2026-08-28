#include <bedrock/registry/BedrockGameplayRegistry.hpp>
#include <bedrock/world/MinecraftDataAssets.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(double first, double second, double epsilon = 1e-9) {
    return std::abs(first - second) <= epsilon;
}

} // namespace

int main() {
    try {
        bedrock::MinecraftDataAssets assets;

        const auto latestPaths = assets.resolveByVersion("1.26.0");
        require(
            latestPaths.recipesDirectory == "1.19.10",
            "latest recipe remap mismatch"
        );
        require(
            latestPaths.windowsDirectory == "1.16.201",
            "latest window remap mismatch"
        );
        require(
            latestPaths.instrumentsDirectory == "1.17.0",
            "latest instrument remap mismatch"
        );
        require(
            latestPaths.attributesDirectory.empty() &&
                latestPaths.attributesJson.empty(),
            "unavailable latest attributes must not fall back to another file"
        );

        auto recipes = assets.loadBedrockRecipeRegistryByVersion("1.26.0");
        require(recipes.recipeCount() == 2434, "latest recipe count mismatch");
        require(
            recipes.uniqueRecipeNameCount() == 2299,
            "latest unique recipe name count mismatch"
        );
        require(
            recipes.all().front().id == 0 && recipes.all().back().id == 2445,
            "numeric recipe source order mismatch"
        );
        require(recipes.recipeById(9) == nullptr, "sparse recipe ID unexpectedly matched");

        const auto* smithing = recipes.recipeById(0);
        require(smithing != nullptr, "smithing recipe lookup failed");
        require(smithing->type == "smithing_table", "smithing recipe type mismatch");
        require(
            smithing->name == std::optional<std::string>(
                "minecraft:smithingtable_diamond_axe_to_netherite_axe"
            ),
            "smithing recipe name mismatch"
        );
        require(smithing->ingredients.size() == 2, "smithing ingredients mismatch");
        require(
            smithing->ingredients[0].name == "diamond_axe" &&
                smithing->ingredients[0].count == 1 &&
                !smithing->ingredients[0].metadata.has_value(),
            "smithing ingredient fields mismatch"
        );
        require(
            smithing->input.has_value() && smithing->input->size() == 1 &&
                (*smithing->input)[0] == std::vector<int32_t>({1, 2}),
            "smithing input matrix mismatch"
        );
        require(
            smithing->output.size() == 1 &&
                smithing->output[0].name == "netherite_axe" &&
                smithing->output[0].metadata == 0,
            "smithing output mismatch"
        );
        require(
            recipes.recipesByType("smithing_table").size() == 9,
            "recipe type index mismatch"
        );

        const auto clayRecipes = recipes.recipesByName("minecraft:clay");
        require(clayRecipes.size() == 2, "duplicate recipe names were lost");
        require(
            clayRecipes[0]->id == 797 && clayRecipes[1]->id == 2326,
            "duplicate recipe source order mismatch"
        );
        require(
            recipes.recipeByName("minecraft:clay")->id == 2326,
            "recipe name index must be last-write-wins"
        );

        const auto* firework = recipes.recipeById(1524);
        require(
            firework != nullptr && !firework->output.empty() &&
                firework->output[0].nbt.has_value() &&
                firework->output[0].nbt->kind ==
                    bedrock::ProtoDefValue::Kind::Object,
            "structured Bedrock recipe NBT was not preserved"
        );
        const auto* furnace = recipes.recipeById(2258);
        require(
            furnace != nullptr && !furnace->output[0].metadata.has_value(),
            "optional recipe output metadata mismatch"
        );

        auto windows = assets.loadBedrockWindowRegistryByProtocol(924);
        require(windows.windowCount() == 14, "Bedrock window count mismatch");
        const auto* inventory = windows.windowById("inventory");
        require(inventory != nullptr, "inventory window ID lookup failed");
        require(inventory->name == "Inventory", "inventory display name mismatch");
        require(
            windows.windowByName("Inventory") == inventory,
            "window name index mismatch"
        );
        const auto* inventorySlots = inventory->slotByName("inventory");
        require(
            inventorySlots != nullptr && inventorySlots->index == 9 &&
                inventorySlots->size == 36 && inventorySlots->slotCount() == 36 &&
                inventorySlots->endIndex() == 45,
            "inventory slot range mismatch"
        );

        auto instruments = assets.loadBedrockInstrumentRegistryByVersion("1.26.0");
        require(
            instruments.instrumentCount() == 16,
            "Bedrock instrument count mismatch"
        );
        const auto* harp = instruments.instrumentById(0);
        require(
            harp != nullptr && harp->name == "harp" &&
                harp->sound == std::optional<std::string>("note.harp"),
            "harp instrument mismatch"
        );
        require(
            instruments.instrumentByName("pling")->id == 15,
            "instrument name index mismatch"
        );

        auto latestAttributes =
            assets.loadBedrockAttributeRegistryByVersion("1.26.0");
        require(
            latestAttributes.attributeCount() == 0,
            "latest registry must mirror missing minecraft-data attributes"
        );

        const auto legacyPaths = assets.resolveByVersion("1.19.62");
        require(
            legacyPaths.attributesDirectory == "1.16.201",
            "legacy attribute remap mismatch"
        );
        auto attributes = assets.loadBedrockAttributeRegistryByVersion("1.19.62");
        require(attributes.attributeCount() == 15, "legacy attribute count mismatch");
        const auto* health = attributes.attributeByName("health");
        require(
            health != nullptr && health->resource == "health" &&
                near(health->defaultValue, 20.0) && near(health->min, 0.0) &&
                near(health->max, 20.0),
            "health attribute mismatch"
        );
        require(
            near(health->clamp(99.0), 20.0),
            "attribute clamp helper mismatch"
        );
        const auto* followRange =
            attributes.attributeByResource("generic.follow_range");
        require(
            followRange != nullptr && followRange->name == "followRange" &&
                near(followRange->max, 2048.0),
            "attribute resource index mismatch"
        );

        const auto earliestPaths = assets.resolveByVersion("1.16.201");
        require(
            earliestPaths.recipesJson.empty() &&
                earliestPaths.instrumentsJson.empty(),
            "missing early gameplay categories must remain unavailable"
        );
        require(
            assets.loadBedrockRecipeRegistryByVersion("1.16.201").recipeCount() == 0,
            "missing early recipes must produce an empty registry"
        );

        std::cout << "bedrock gameplay registry smoke ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bedrock gameplay registry smoke failed: " << error.what() << '\n';
        return 1;
    }
}
