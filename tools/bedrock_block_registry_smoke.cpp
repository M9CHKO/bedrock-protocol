#include <bedrock/world/BedrockBlockRegistry.hpp>
#include <bedrock/world/BedrockChunk.hpp>
#include <bedrock/world/MinecraftDataAssets.hpp>

#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

bool near(double actual, double expected) {
    return std::abs(actual - expected) < 1e-9;
}

bool sameShape(
    const bedrock::BlockShape& actual,
    const bedrock::BlockShape& expected
) {
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!near(actual[i], expected[i])) return false;
    }
    return true;
}

int fail(const std::string& message) {
    std::cerr << "[BEDROCK-BLOCK-REGISTRY-SMOKE] " << message << "\n";
    return 1;
}

} // namespace

int main() {
    bedrock::MinecraftDataAssets assets;
    const auto paths = assets.resolveByVersion("1.21.100");
    if (paths.version.protocol != 827 ||
        paths.blockStatesJson.filename() != "blockStates.json" ||
        paths.blockCollisionShapesJson.filename() != "blockCollisionShapes.json") {
        return fail("minecraft-data path resolution mismatch");
    }
    // Early Bedrock datasets predate blockStates/collision files, but generic
    // path resolution must remain available to protocol and item consumers.
    if (assets.resolveByVersion("0.14").version.protocol != 70) {
        return fail("legacy Bedrock path resolution regression");
    }

    const auto registry = assets.loadBedrockBlockRegistryByVersion("1.21.100");
    if (registry.blockCount() != 1269 || registry.stateCount() != 15285 ||
        !registry.supportsBlockHashes()) {
        return fail("registry size or block-hash feature mismatch");
    }

    const auto* stoneDefinition = registry.blockByName("minecraft:stone");
    if (stoneDefinition == nullptr || stoneDefinition->id != 1 ||
        stoneDefinition->defaultState != 2445 || stoneDefinition->hardness != 1.5 ||
        !stoneDefinition->harvestTools.has_value() ||
        !stoneDefinition->harvestTools->contains(877)) {
        return fail("stone definition mismatch");
    }

    const auto stone = registry.fromStateId(2445, 7);
    if (!stone.has_value() || stone->type != 1 || stone->metadata != 0 ||
        stone->biomeId != 7 || stone->name != "stone" ||
        stone->displayName != "Stone" || stone->hash != -2144268767 ||
        stone->shapes.size() != 1 ||
        !sameShape(stone->shapes.front(), {0.5, 0.5, 0.5, 1, 1, 1}) ||
        stone->canHarvest() || !stone->canHarvest(877) || stone->canHarvest(1)) {
        return fail("stone block parity mismatch");
    }
    const auto stoneRaycastShapes = stone->raycastShapes();
    if (stoneRaycastShapes.size() != 1 ||
        !sameShape(stoneRaycastShapes.front(), bedrock::FullBlockShape)) {
        return fail("stone raycast shape conversion mismatch");
    }

    bedrock::BedrockDigTimeOptions handDig;
    if (!near(stone->digTime(handDig), 7500.0)) {
        return fail("hand dig-time formula mismatch");
    }
    bedrock::BedrockDigTimeOptions pickaxeDig;
    pickaxeDig.heldItemType = 877;
    pickaxeDig.toolMultiplier = 6.0;
    if (!near(stone->digTime(pickaxeDig), 400.0)) {
        return fail("tool dig-time formula mismatch");
    }

    const auto air = registry.fromStateId(12076);
    if (!air.has_value() || air->name != "air" || !air->shapes.empty() ||
        air->boundingBox != "empty" || air->diggable) {
        return fail("air block mismatch");
    }

    const auto bottomSlab = registry.fromStateId(6057);
    const auto topSlab = registry.fromProperties(
        "minecraft:oak_slab",
        {{"minecraft:vertical_half", bedrock::BedrockBlockProperty::string("top")}}
    );
    if (!bottomSlab.has_value() || !topSlab.has_value() ||
        bottomSlab->stateId != 6057 || topSlab->stateId != 6058 ||
        bottomSlab->hash != 1768579957 || topSlab->hash != 1768579957 ||
        bottomSlab->shapes.size() != 1 || topSlab->shapes.size() != 1 ||
        !sameShape(bottomSlab->shapes.front(), {0.5, 0.25, 0.5, 1, 0.5, 1}) ||
        !sameShape(topSlab->shapes.front(), {0.5, 0.75, 0.5, 1, 0.5, 1})) {
        return fail("state-specific slab parity mismatch");
    }
    const auto bottomRaycastShapes = bottomSlab->raycastShapes();
    const auto topRaycastShapes = topSlab->raycastShapes();
    if (bottomRaycastShapes.size() != 1 || topRaycastShapes.size() != 1 ||
        !sameShape(bottomRaycastShapes.front(), {0, 0, 0, 1, 0.5, 1}) ||
        !sameShape(topRaycastShapes.front(), {0, 0.5, 0, 1, 1, 1})) {
        return fail("slab raycast shape conversion mismatch");
    }

    const auto parsedSlab = registry.fromString(
        "minecraft:oak_slab[minecraft:vertical_half=top]"
    );
    const auto parsedCandle = registry.fromString(
        "minecraft:candle[\"lit\":true,\"candles\":0]"
    );
    if (!parsedSlab.has_value() || parsedSlab->stateId != 6058 ||
        !parsedCandle.has_value() || parsedCandle->hash != -326467561) {
        return fail("fromString mismatch");
    }
    const auto* candleLit = parsedCandle->property("lit");
    const auto* candleCount = parsedCandle->property("candles");
    if (candleLit == nullptr || candleCount == nullptr ||
        candleLit->asInteger() != 1 || candleCount->asInteger() != 0) {
        return fail("typed block property mismatch");
    }

    const auto* quartzStairsDefinition = registry.blockByName("minecraft:quartz_stairs");
    const auto defaultQuartzStairs = registry.fromString("minecraft:quartz_stairs");
    if (quartzStairsDefinition == nullptr || quartzStairsDefinition->defaultState != 7116 ||
        !defaultQuartzStairs.has_value() ||
        defaultQuartzStairs->stateId != quartzStairsDefinition->defaultState) {
        return fail("bare block string must resolve the declared default state");
    }

    struct StairStateExpectation {
        int32_t stateId;
        int32_t runtimeHash;
        bedrock::BedrockCollisionShape upperShape;
    };
    const std::array<StairStateExpectation, 4> stairExpectations {{
        {7116, 2067288422, {0.75, 0.75, 0.75, 0.5, 0.5, 0.5}},
        {7117, 2005813599, {0.25, 0.75, 0.25, 0.5, 0.5, 0.5}},
        {7118, -2104729228, {0.25, 0.75, 0.75, 0.5, 0.5, 0.5}},
        {7119, 2128763245, {0.75, 0.75, 0.25, 0.5, 0.5, 0.5}},
    }};
    const bedrock::BedrockCollisionShape stairBase {
        0.5, 0.25, 0.5, 1, 0.5, 1
    };
    for (std::size_t direction = 0; direction < stairExpectations.size(); ++direction) {
        const auto stairs = registry.fromString(
            "minecraft:quartz_stairs[\"upside_down_bit\":false,\"weirdo_direction\":" +
            std::to_string(direction) + "]"
        );
        const auto& expected = stairExpectations[direction];
        if (!stairs.has_value() || stairs->stateId != expected.stateId ||
            bedrock::BedrockBlockRegistry::computeRuntimeHash(
                stairs->name,
                stairs->properties
            ) != expected.runtimeHash ||
            stairs->shapes.size() != 2 ||
            !sameShape(stairs->shapes[0], stairBase) ||
            !sameShape(stairs->shapes[1], expected.upperShape)) {
            return fail("directional stair state/hash/shape mismatch");
        }
    }

    if (bedrock::BedrockBlockRegistry::computeHash("stone", {}) != -2144268767 ||
        bedrock::BedrockBlockRegistry::computeHash(
            "oak_slab",
            {{"minecraft:vertical_half", bedrock::BedrockBlockProperty::string("bottom")}}
        ) != 1768579957) {
        return fail("Node FNV/NBT hash golden mismatch");
    }

    // The generated TSV is optional. MinecraftDataAssets must derive the old
    // compact runtime registry from Bedrock JSON when it is absent.
    const auto runtimeRegistry = assets.loadBlockRuntimeRegistryByProtocol(827);
    if (runtimeRegistry.size() != 15285 ||
        runtimeRegistry.nameOf(2445) != std::optional<std::string>("stone") ||
        runtimeRegistry.runtimeIdOf("stone") != std::optional<uint32_t>(2445) ||
        runtimeRegistry.runtimeIdOf("oak_slab") != std::optional<uint32_t>(6057)) {
        return fail("compact runtime registry fallback mismatch");
    }

    bedrock::BedrockChunkColumn column(0, 0);
    column.setBlockStateId({.x = 2, .y = 0, .z = 0}, 6057);
    column.setBlockStateId({.x = 3, .y = 0, .z = 0}, 2445);
    bedrock::BedrockWorld world;
    world.setLoadedColumn(0, 0, std::move(column), false);

    const auto shapeProvider = [&registry](
        int32_t stateId,
        const bedrock::BlockPosition&
    ) {
        return registry.raycastShapesForState(stateId);
    };
    const auto aboveSlab = world.raycast(
        {0.5, 0.75, 0.5},
        {1, 0, 0},
        5,
        {},
        shapeProvider
    );
    const auto throughSlab = world.raycast(
        {0.5, 0.25, 0.5},
        {1, 0, 0},
        5,
        {},
        shapeProvider
    );
    if (!aboveSlab.has_value() || aboveSlab->position.x != 3 ||
        aboveSlab->stateId != 2445 || !throughSlab.has_value() ||
        throughSlab->position.x != 2 || throughSlab->stateId != 6057) {
        return fail("registry-backed world raycast mismatch");
    }

    std::cout << "[BEDROCK-BLOCK-REGISTRY-SMOKE] ok\n";
    return 0;
}
