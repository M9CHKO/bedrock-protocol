#include <bedrock/world/BedrockBlockRegistry.hpp>
#include <bedrock/world/BedrockChunk.hpp>
#include <bedrock/world/MinecraftDataAssets.hpp>

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int fail(const std::string& message) {
    std::cerr << "[BEDROCK-BLOCK-ENTITY-SMOKE] " << message << "\n";
    return 1;
}

} // namespace

int main() {
    bedrock::MinecraftDataAssets assets;
    const auto registry = assets.loadBedrockBlockRegistryByVersion("1.21.100");

    const auto* signDefinition = registry.blockByName("standing_sign");
    if (signDefinition == nullptr || signDefinition->defaultState != 9286) {
        return fail("standing sign registry mismatch");
    }
    auto sign = registry.fromStateId(signDefinition->defaultState);
    if (!sign.has_value() || !sign->isSign() ||
        sign->getSignText() != std::vector<std::string>({""}) ||
        sign->blockEntity() != nullptr) {
        return fail("empty Bedrock sign mismatch");
    }

    sign->setSignText(std::vector<std::string> {"Hello", "World", "!", "?"});
    auto* signEntity = sign->blockEntity();
    const auto* signId = signEntity == nullptr ? nullptr : signEntity->root.find("id");
    const auto* signText = signEntity == nullptr ? nullptr : signEntity->root.find("Text");
    if (signEntity == nullptr || signEntity->root.type != bedrock::NbtTagType::Compound ||
        signId == nullptr || signId->type != bedrock::NbtTagType::String ||
        signId->stringValue != "Sign" || signText == nullptr ||
        signText->type != bedrock::NbtTagType::String ||
        signText->stringValue != "Hello\nWorld\n!\n?" ||
        sign->getSignText() != std::vector<std::string>({"Hello\nWorld\n!\n?"}) ||
        sign->signText() != "Hello\nWorld\n!\n?") {
        return fail("Bedrock sign NBT mismatch");
    }

    signEntity->root.set("CustomField", bedrock::NbtValue::integer(17));
    sign->setSignText("Updated");
    const auto* customField = sign->entity->root.find("CustomField");
    if (customField == nullptr || customField->integerValue != 17 ||
        sign->signText() != "Updated") {
        return fail("sign text update did not preserve block-entity fields");
    }

    auto stone = registry.fromStateId(2445);
    if (!stone.has_value() || stone->isSign()) {
        return fail("stone registry mismatch");
    }
    bool nonSignRejected = false;
    try {
        stone->setSignText("invalid");
    } catch (const std::logic_error&) {
        nonSignRejected = true;
    }
    if (!nonSignRejected) {
        return fail("non-sign block accepted sign text");
    }

    stone->properties.insert_or_assign(
        "waterlogged",
        bedrock::BedrockBlockProperty::byte(0)
    );
    stone->computedStates.insert_or_assign(
        "waterlogged",
        bedrock::BedrockBlockProperty::byte(1)
    );
    const auto mergedProperties = stone->propertiesWithComputedStates();
    if (stone->isWaterlogged() != true ||
        mergedProperties.at("waterlogged").asInteger() != 1 ||
        stone->property("waterlogged")->asInteger() != 0) {
        return fail("computed block state merge mismatch");
    }

    bedrock::BedrockChunkColumn column(0, 0);
    const bedrock::BlockPosition signPosition {.x = 2, .y = 20, .z = 3};
    const auto missingSectionBlock = column.getBlock(signPosition, registry);
    const auto* airDefinition = registry.blockByName("air");
    if (!missingSectionBlock.has_value() || airDefinition == nullptr ||
        missingSectionBlock->stateId != airDefinition->defaultState ||
        missingSectionBlock->name != "air") {
        return fail("missing section did not resolve to registry air");
    }

    bedrock::BedrockChunkColumn initializedColumn(0, 0);
    initializedColumn.setBounds(-4, 20);
    std::size_t initializeCalls = 0;
    int32_t minimumInitializedY = initializedColumn.maxY();
    int32_t maximumInitializedY = initializedColumn.minY();
    initializedColumn.initialize(
        registry,
        [&](int32_t x, int32_t y, int32_t z) -> std::optional<bedrock::BedrockBlock> {
            ++initializeCalls;
            minimumInitializedY = std::min(minimumInitializedY, y);
            maximumInitializedY = std::max(maximumInitializedY, y);
            if (x == 1 && y == -64 && z == 2) {
                return stone;
            }
            return std::nullopt;
        }
    );
    const auto initializedStone = initializedColumn.getBlock(
        {.x = 1, .y = -64, .z = 2},
        registry
    );
    if (initializeCalls != 16u * 16u * 384u || minimumInitializedY != -64 ||
        maximumInitializedY != 319 || !initializedStone.has_value() ||
        initializedStone->stateId != stone->stateId ||
        initializedColumn.getSectionAtIndex(-4) == nullptr) {
        return fail("CommonChunkColumn initialize bounds mismatch");
    }

    sign->light = 9;
    sign->skyLight = 14;
    column.setBlock(signPosition, *sign, registry);
    column.setBiomeId(signPosition, 7);

    const auto fullSign = column.getBlock(signPosition, registry);
    const auto compactSign = column.getBlock(signPosition, registry, false);
    const auto neighboringAir = column.getBlock(
        {.x = 3, .y = 20, .z = 3},
        registry
    );
    if (!fullSign.has_value() || fullSign->stateId != sign->stateId ||
        fullSign->light != 9 || fullSign->skyLight != 14 ||
        fullSign->biomeId != 7 || fullSign->signText() != "Updated" ||
        !compactSign.has_value() || compactSign->entity.has_value() ||
        compactSign->light != 0 || compactSign->skyLight != 0 ||
        !neighboringAir.has_value() ||
        neighboringAir->stateId != airDefinition->defaultState) {
        return fail("typed CommonChunkColumn block access mismatch");
    }

    auto layeredStone = registry.fromStateId(2445);
    auto water = registry.fromStateId(8860);
    if (!layeredStone.has_value() || !water.has_value() || water->name != "water") {
        return fail("layered block fixtures are unavailable");
    }
    layeredStone->superimposed = std::make_shared<bedrock::BedrockBlock>(*water);
    const bedrock::BlockPosition layeredPosition {.x = 5, .y = 20, .z = 3};
    column.setBlock(layeredPosition, *layeredStone, registry);

    const auto combined = column.getBlock(layeredPosition, registry);
    auto layerOnePosition = layeredPosition;
    layerOnePosition.layer = 1;
    const auto layerOne = column.getBlock(layerOnePosition, registry);
    if (!combined.has_value() || !combined->superimposed ||
        combined->superimposed->stateId != water->stateId ||
        combined->isWaterlogged() != true || !layerOne.has_value() ||
        layerOne->stateId != water->stateId || layerOne->superimposed) {
        return fail("Bedrock superimposed layer mismatch");
    }

    bedrock::BedrockWorld world;
    world.setLoadedColumn(0, 0, std::move(column), false);
    const auto positionedSign = world.getBlock(signPosition, registry);
    if (!positionedSign.has_value() || !positionedSign->position.has_value() ||
        positionedSign->position->x != signPosition.x ||
        positionedSign->position->y != signPosition.y ||
        positionedSign->position->z != signPosition.z) {
        return fail("typed world block position mismatch");
    }

    std::size_t updateCount = 0;
    bedrock::BedrockWorldBlockSnapshot oldSnapshot;
    bedrock::BedrockWorldBlockSnapshot newSnapshot;
    world.onBlockUpdate([&](const auto& oldBlock, const auto& newBlock) {
        ++updateCount;
        oldSnapshot = oldBlock;
        newSnapshot = newBlock;
    });

    auto replacement = registry.fromStateId(2445);
    replacement->light = 4;
    replacement->skyLight = 12;
    world.setBlock(signPosition, *replacement, registry);
    const auto replaced = world.getBlock(signPosition, registry);
    if (updateCount != 1 || oldSnapshot.stateId != sign->stateId ||
        newSnapshot.stateId != replacement->stateId ||
        !replaced.has_value() || replaced->stateId != replacement->stateId ||
        replaced->light != 4 || replaced->skyLight != 12 ||
        !replaced->entity.has_value()) {
        return fail("typed world setBlock/update or entity-preservation mismatch");
    }

    std::cout << "[BEDROCK-BLOCK-ENTITY-SMOKE] ok\n";
    return 0;
}
