#include <bedrock/registry/BedrockDefaultSkin.hpp>
#include <bedrock/registry/BedrockLootRegistry.hpp>
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
            latestPaths.blockLootDirectory.empty() &&
                latestPaths.entityLootDirectory.empty(),
            "unmapped latest loot must remain unavailable"
        );
        require(
            latestPaths.blockLootJson.empty() && latestPaths.entityLootJson.empty(),
            "unmapped latest loot paths must be empty"
        );
        require(
            latestPaths.steveDirectory == "1.21.70",
            "latest default skin remap mismatch"
        );

        const auto latestLoot = assets.loadBedrockLootRegistryByVersion("1.26.0");
        require(latestLoot.blockLootCount() == 0, "latest block loot leaked");
        require(latestLoot.entityLootCount() == 0, "latest entity loot leaked");

        const auto lootPaths = assets.resolveByVersion("1.18.0");
        require(
            lootPaths.blockLootDirectory == "1.18.0" &&
                lootPaths.entityLootDirectory == "1.18.0",
            "Bedrock loot remap mismatch"
        );
        auto loot = assets.loadBedrockLootRegistryByVersion("1.18.0");
        require(loot.blockLootCount() == 3298, "block loot count mismatch");
        require(loot.uniqueBlockLootCount() == 385, "unique block loot count mismatch");
        require(loot.entityLootCount() == 72, "entity loot count mismatch");
        require(
            loot.allBlockLoot().front().block == "acacia_button",
            "block loot source-order start mismatch"
        );
        require(
            loot.allBlockLoot().back().block == "stained_glass_pane" &&
                loot.allBlockLoot().back().states.at("color").stringValue == "yellow",
            "block loot source-order end mismatch"
        );

        const auto stoneVariants = loot.blockLootVariants("minecraft:stone");
        require(stoneVariants.size() == 7, "stone loot variants were lost");
        const auto* stone = loot.blockLootByName("stone");
        require(stone == stoneVariants.back(), "block index is not last-write-wins");
        require(
            stone->states.at("stone_type").stringValue == "stone" &&
                stone->drops.size() == 2,
            "last stone loot definition mismatch"
        );
        require(
            stone->drops[0].silkTouch == std::optional<bool>(true) &&
                stone->drops[1].noSilkTouch == std::optional<bool>(true),
            "stone silk-touch conditions mismatch"
        );

        bedrock::BedrockLootBlockStates andesiteStates;
        andesiteStates.emplace(
            "stone_type",
            bedrock::ProtoDefValue::string("andesite")
        );
        andesiteStates.emplace("extra_state", bedrock::ProtoDefValue::boolean(true));
        const auto* andesite = loot.blockLootForStates("stone", andesiteStates);
        require(andesite != nullptr, "state-aware stone loot lookup failed");
        require(
            andesite->drops.size() == 1 &&
                andesite->drops.front().metadata == std::optional<int32_t>(5),
            "andesite loot metadata mismatch"
        );
        require(
            loot.blockLootVariants("cobblestone_wall").size() == 1621,
            "large Bedrock state variant set was truncated"
        );

        const auto* mushroom = loot.blockLootByName("brown_mushroom_block");
        require(mushroom != nullptr && mushroom->drops.size() == 2, "mushroom loot missing");
        require(
            mushroom->drops[1].minimumStackSize() == std::optional<double>(0.0) &&
                !mushroom->drops[1].maximumStackSize().has_value(),
            "nullable stack-size bound was not preserved"
        );

        const auto* piglin = loot.entityLootByName("minecraft:zombified_piglin");
        require(piglin != nullptr && piglin->drops.size() == 3, "entity loot lookup failed");
        require(
            piglin->drops.back().item == "gold_ingot" &&
                near(piglin->drops.back().dropChance, 0.025) &&
                piglin->drops.back().playerKill == std::optional<bool>(true),
            "entity player-kill drop condition mismatch"
        );

        auto latestSkin = assets.loadBedrockDefaultSkinByVersion("1.26.0");
        require(latestSkin.has_value(), "latest Bedrock default skin is unavailable");
        require(
            latestSkin->raw().kind == bedrock::ProtoDefValue::Kind::Object &&
                latestSkin->raw().objectValue.size() == 20,
            "default skin raw object was not preserved"
        );
        require(
            latestSkin->skinId() == "persona-ec4b0e51bc40322b-0" &&
                latestSkin->armSize() == "wide" &&
                latestSkin->skinColor() == "#ffb37b62",
            "latest default skin identity mismatch"
        );
        require(
            latestSkin->personaSkin() && !latestSkin->premiumSkin() &&
                !latestSkin->capeOnClassicSkin(),
            "default skin flags mismatch"
        );
        require(
            latestSkin->skinImage().width == 256 &&
                latestSkin->skinImage().height == 256 &&
                latestSkin->skinImage().bytes.size() == 262144 &&
                latestSkin->skinImage().validRgba(),
            "decoded default skin image mismatch"
        );
        require(
            latestSkin->capeImage().width == 0 &&
                latestSkin->capeImage().height == 0 &&
                latestSkin->capeImage().bytes.empty() &&
                latestSkin->capeImage().validRgba(),
            "empty default cape mismatch"
        );
        require(
            latestSkin->animatedImages().size() == 1 &&
                latestSkin->animatedImages().front().expression == 1 &&
                latestSkin->animatedImages().front().frames == 2 &&
                latestSkin->animatedImages().front().type == 1 &&
                latestSkin->animatedImages().front().image.width == 32 &&
                latestSkin->animatedImages().front().image.height == 64 &&
                latestSkin->animatedImages().front().image.bytes.size() == 8192,
            "default skin animation mismatch"
        );
        require(
            latestSkin->personaPieces().size() == 9 &&
                latestSkin->pieceTintColors().size() == 3,
            "persona metadata mismatch"
        );
        require(
            latestSkin->skinGeometryDataJson().find("\"format_version\"") !=
                    std::string::npos &&
                latestSkin->skinGeometryDataJson().find("\"1.14.0\"") !=
                    std::string::npos,
            "decoded skin geometry JSON mismatch"
        );
        require(
            latestSkin->skinResourcePatchJson().find("geometry") != std::string::npos,
            "decoded skin resource patch mismatch"
        );
        require(
            latestSkin->skinGeometryDataEngineVersionBase64() ==
                    std::optional<std::string>("MS4xNC4w") &&
                latestSkin->skinGeometryDataEngineVersion() ==
                    std::optional<std::string>("1.14.0"),
            "decoded geometry engine version mismatch"
        );
        require(
            latestSkin->skinAnimationDataBase64().empty() &&
                latestSkin->skinAnimationData().empty(),
            "empty skin animation data mismatch"
        );
        require(
            latestSkin->field("SkinData") != nullptr &&
                latestSkin->field("SkinData")->stringValue.size() == 349528,
            "raw default skin field access mismatch"
        );

        auto legacySkin = assets.loadBedrockDefaultSkinByVersion("1.18.11");
        require(legacySkin.has_value(), "legacy default skin remap failed");
        require(
            legacySkin->skinId().size() == 63 &&
                legacySkin->skinColor() == "#b37b62",
            "legacy default skin mismatch"
        );
        require(
            !legacySkin->skinGeometryDataEngineVersionBase64().has_value() &&
                !legacySkin->skinGeometryDataEngineVersion().has_value(),
            "new geometry engine field leaked into the legacy skin"
        );

        const auto oldestPaths = assets.resolveByVersion("0.14");
        require(oldestPaths.steveDirectory.empty(), "unmapped old skin path leaked");
        require(
            !assets.loadBedrockDefaultSkinByVersion("0.14").has_value(),
            "unmapped old default skin must be unavailable"
        );

        std::cout << "bedrock loot/default skin smoke ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bedrock loot/default skin smoke failed: " << error.what() << '\n';
        return 1;
    }
}
