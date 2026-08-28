#include <bedrock/registry/BedrockRegistry.hpp>
#include <bedrock/world/MinecraftDataAssets.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        bedrock::MinecraftDataAssets assets;
        auto registry = assets.loadBedrockRegistryByVersion("1.21.100");

        const auto staticStates = registry.writeItemStates();
        require(staticStates.size() == 1836, "static itemstates count mismatch");
        require(
            staticStates.front().name == "minecraft:air" &&
                staticStates.front().runtimeId == 0 &&
                !staticStates.front().componentBased,
            "static itemstates first entry mismatch"
        );
        require(
            staticStates.back().name == "minecraft:tube_coral_wall_fan" &&
                staticStates.back().runtimeId == 10828 &&
                !staticStates.back().componentBased,
            "static itemstates source order mismatch"
        );
        require(
            registry.blocks().runtimeIdCount() == 0,
            "blocksByRuntimeId must be lazy like prismarine-registry"
        );

        // 1.21.60+ moved itemstates to item_registry. A start_game without
        // that old field still initializes the runtime block index.
        registry.handleStartGame(bedrock::ProtoDefValue::object({
            {
                "block_network_ids_are_hashes",
                bedrock::ProtoDefValue::boolean(true)
            }
        }));
        require(
            registry.blocks().usesHashedRuntimeIds(),
            "hashed runtime block IDs were not selected"
        );
        require(
            registry.blocks().runtimeIdCount() == 15285,
            "hashed blocksByRuntimeId count mismatch"
        );
        require(
            bedrock::BedrockBlockRegistry::computeRuntimeHash(
                "blue_candle",
                {
                    {"candles", bedrock::BedrockBlockProperty::integer(0)},
                    {"lit", bedrock::BedrockBlockProperty::byte(0)}
                }
            ) == 1088625327,
            "typed runtime block hash golden mismatch"
        );
        require(
            registry.blocks().stateIdForRuntimeId(-2144268767) ==
                std::optional<int32_t>(2445),
            "hashed stone runtime ID mismatch"
        );
        const auto* stoneState = registry.blockStateByRuntimeId(-2144268767);
        require(
            stoneState != nullptr && stoneState->stateId == 2445 &&
                stoneState->name == "stone",
            "runtime block-state facade mismatch"
        );
        require(
            registry.blockByRuntimeId(-2144268767) != nullptr &&
                registry.blockByRuntimeId(-2144268767)->name == "stone",
            "runtime block facade mismatch"
        );
        require(
            registry.blockStateByRuntimeId(123456789) == nullptr,
            "unknown runtime block ID matched"
        );

        const auto customNbt = bedrock::ProtoDefValue::object({
            {"components", bedrock::ProtoDefValue::string("custom")}
        });
        registry.handleItemRegistry(bedrock::ProtoDefValue::object({
            {"itemstates", bedrock::ProtoDefValue::array({
                bedrock::ProtoDefValue::object({
                    {"name", bedrock::ProtoDefValue::string("minecraft:stone")},
                    {"runtime_id", bedrock::ProtoDefValue::integer(900)},
                    {"component_based", bedrock::ProtoDefValue::boolean(true)},
                    {"version", bedrock::ProtoDefValue::string("legacy")},
                    {"nbt", bedrock::ProtoDefValue::null()}
                }),
                bedrock::ProtoDefValue::object({
                    {"name", bedrock::ProtoDefValue::string("custom:wand")},
                    {"runtime_id", bedrock::ProtoDefValue::integer(901)},
                    {"component_based", bedrock::ProtoDefValue::boolean(false)},
                    {"version", bedrock::ProtoDefValue::string("data_driven")},
                    {"nbt", customNbt}
                })
            })}
        }));

        require(registry.items().itemCount() == 2, "dynamic item palette did not replace static data");
        const auto* dynamicStone = registry.itemById(900);
        require(
            dynamicStone != nullptr && dynamicStone->name == "stone" &&
                dynamicStone->displayName == "Stone" && dynamicStone->stackSize == 64,
            "known dynamic item did not inherit static metadata"
        );
        const auto* custom = registry.itemByName("custom:wand");
        require(
            custom != nullptr && custom->id == 901 && custom->name == "custom:wand",
            "custom namespace item was not indexed"
        );

        const auto dynamicStates = registry.writeItemStates();
        require(dynamicStates.size() == 2, "dynamic itemstates write count mismatch");
        require(
            dynamicStates[0].name == "minecraft:stone" &&
                dynamicStates[0].runtimeId == 900 &&
                !dynamicStates[0].componentBased,
            "minecraft component_based derivation mismatch"
        );
        require(
            dynamicStates[1].name == "custom:wand" &&
                dynamicStates[1].runtimeId == 901 &&
                dynamicStates[1].componentBased,
            "custom component_based derivation mismatch"
        );
        require(
            dynamicStates[1].version.has_value() &&
                dynamicStates[1].version->stringValue == "data_driven" &&
                dynamicStates[1].nbt.has_value() &&
                dynamicStates[1].nbt->get("components") != nullptr,
            "modern itemstate extension fields were not retained"
        );
        const auto encodedStates = registry.writeItemStatesValue();
        require(
            encodedStates.kind == bedrock::ProtoDefValue::Kind::Array &&
                encodedStates.arrayValue.size() == 2 &&
                encodedStates.arrayValue[1].get("component_based")->boolValue &&
                encodedStates.arrayValue[1].get("version")->stringValue ==
                    "data_driven",
            "itemstates ProtoDef packet shape mismatch"
        );

        // The source indexer keeps duplicate packet entries in itemsArray but
        // resolves name indexes to the final one.
        registry.loadItemStates(std::vector<bedrock::BedrockItemState> {
            {.name = "minecraft:stone", .runtimeId = 1000},
            {.name = "custom:wand", .runtimeId = 1001},
            {.name = "minecraft:stone", .runtimeId = 1002}
        });
        require(registry.items().itemCount() == 3, "duplicate itemstates were lost");
        require(
            registry.itemByName("stone") != nullptr &&
                registry.itemByName("stone")->id == 1002,
            "dynamic item name index is not last-write-wins"
        );
        require(
            registry.itemById(1000) != nullptr && registry.itemById(1002) != nullptr,
            "dynamic item ID index lost a duplicate-name entry"
        );

        registry.loadItemStates(std::vector<bedrock::BedrockItemState> {
            {.name = "custom:first", .runtimeId = 1100},
            {.name = "custom:last", .runtimeId = 1100}
        });
        require(
            registry.itemByName("custom:first") != nullptr &&
                registry.itemByName("custom:first")->name == "custom:first" &&
                registry.itemById(1100) != nullptr &&
                registry.itemById(1100)->name == "custom:last",
            "independent Node name/id indexes were not preserved"
        );

        auto legacy = assets.loadBedrockRegistryByVersion("1.19.50");
        require(
            !legacy.supportsFeature("blockHashes"),
            "legacy feature boundary mismatch"
        );
        legacy.handleStartGame(
            std::vector<bedrock::BedrockItemState> {
                {.name = "minecraft:stone", .runtimeId = 500}
            },
            true
        );
        require(
            !legacy.blocks().usesHashedRuntimeIds(),
            "packet hash flag bypassed the version feature boundary"
        );
        require(
            legacy.blocks().stateIdForRuntimeId(0) == std::optional<int32_t>(0),
            "legacy sequential runtime block ID mismatch"
        );

        std::cout << "bedrock dynamic registry smoke ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bedrock dynamic registry smoke failed: " << error.what() << '\n';
        return 1;
    }
}
