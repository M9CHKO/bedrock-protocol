#include <bedrock/debug/ProtocolTypeTsvIndex.hpp>
#include <bedrock/item/BedrockItem.hpp>
#include <bedrock/protodef/ProtoDefEncoder.hpp>
#include <bedrock/protodef/ProtoDefWriter.hpp>
#include <bedrock/world/MinecraftDataAssets.hpp>

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

int fail(const std::string& message) {
    std::cerr << "[BEDROCK-ITEM-SMOKE] " << message << "\n";
    return 1;
}

std::vector<uint8_t> unhex(const std::string& text) {
    auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };

    std::vector<uint8_t> out;
    if ((text.size() & 1u) != 0) return out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const auto high = digit(text[i]);
        const auto low = digit(text[i + 1]);
        if (high < 0 || low < 0) return {};
        out.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return out;
}

const bedrock::ProtoDefValue* field(
    const bedrock::ProtoDefValue& object,
    const std::string& name
) {
    return object.kind == bedrock::ProtoDefValue::Kind::Object
        ? object.get(name)
        : nullptr;
}

} // namespace

int main() {
    bedrock::MinecraftDataAssets assets;
    const auto paths = assets.resolveByVersion("1.21.100");
    if (paths.enchantmentsDirectory != "1.19.1" ||
        paths.enchantmentsJson.filename() != "enchantments.json") {
        return fail("Bedrock enchantment path remap mismatch");
    }

    const auto registry = assets.loadBedrockItemRegistryByProtocol(827);
    if (registry.protocolVersion() != 827 || registry.usesAuxValue() ||
        registry.itemCount() != 1836 || registry.enchantmentCount() != 38) {
        return fail("registry metadata mismatch");
    }

    const auto* swordDefinition = registry.itemByName("minecraft:diamond_sword");
    const auto* sharpness = registry.enchantmentByName("minecraft:sharpness");
    if (swordDefinition == nullptr || swordDefinition->id != 895 ||
        swordDefinition->stackSize != 1 || swordDefinition->maxDurability != 1561 ||
        sharpness == nullptr || sharpness->id != 9 || sharpness->maxLevel != 5 ||
        sharpness->minCost.atLevel(5) != 45 ||
        registry.enchantmentById(17) == nullptr ||
        registry.enchantmentById(17)->name != "unbreaking") {
        return fail("item/enchantment definition mismatch");
    }

    registry.resetStackIds();
    auto sword = registry.create(895, 2);
    auto secondSword = registry.create(895, 1);
    if (sword.stackId != 0 || secondSword.stackId != 1 ||
        registry.currentStackId() != 2 || sword.name != "diamond_sword" ||
        sword.displayName != "Diamond Sword" || sword.stackSize != 1 ||
        sword.maxDurability != 1561 || sword.durabilityUsed() != 0 ||
        !sword.hasNbtPayload()) {
        return fail("constructor/default Damage/stack-id mismatch");
    }

    const auto* defaultDamage = sword.nbt->root.find("Damage");
    if (defaultDamage == nullptr || defaultDamage->type != bedrock::NbtTagType::Int ||
        defaultDamage->integerValue != 0) {
        return fail("explicit Bedrock Damage:0 mismatch");
    }

    sword.setCustomName("Blade");
    sword.setCustomLore(std::vector<std::string> {"one", "two"});
    sword.setRepairCost(3);
    sword.setDurabilityUsed(7);
    sword.setEnchantments({
        {.name = "sharpness", .level = 5},
        {.name = "minecraft:unbreaking", .level = 2}
    });

    const auto lore = sword.customLore();
    const auto enchantments = sword.enchantments();
    if (sword.customName() != std::optional<std::string>("Blade") ||
        !lore.has_value() || *lore != std::vector<std::string>({"one", "two"}) ||
        sword.repairCost() != 3 || sword.durabilityUsed() != 7 ||
        enchantments.size() != 2 || enchantments[0].id != 9 ||
        enchantments[0].name != std::optional<std::string>("sharpness") ||
        enchantments[0].level != 5 || enchantments[1].id != 17 ||
        enchantments[1].name != std::optional<std::string>("unbreaking") ||
        enchantments[1].level != 2) {
        return fail("NBT helper mismatch");
    }

    const auto network = registry.toNetwork(sword);
    if (network.networkId != 895 || network.count != 2 || network.metadata != 0 ||
        network.stackId != 0 || !network.nbt.has_value() ||
        !network.canPlaceOn.empty() || !network.canDestroy.empty()) {
        return fail("typed network Item mismatch");
    }

    const auto proto = registry.toProtoDefValue(sword);
    const auto* extra = field(proto, "extra");
    const auto* nbtWrapper = extra == nullptr ? nullptr : field(*extra, "nbt");
    const auto* encodedNbt = nbtWrapper == nullptr ? nullptr : field(*nbtWrapper, "nbt");
    if (field(proto, "network_id") == nullptr || field(proto, "stack_id") == nullptr ||
        extra == nullptr || encodedNbt == nullptr ||
        encodedNbt->kind != bedrock::ProtoDefValue::Kind::Bytes) {
        return fail("ProtoDef Item shape mismatch");
    }

    bedrock::ProtocolTypeTsvIndex typeIndex;
    const auto itemType = typeIndex.findTypeJson("1.21.100", "Item");
    if (!itemType.has_value()) return fail("Item protocol type missing");
    auto resolver = [&](const std::string& name) {
        return typeIndex.findTypeJson("1.21.100", name);
    };
    bedrock::ProtoDefEncoder encoder(resolver);
    encoder.setVariable("ShieldItemID", 1243);
    bedrock::ProtoDefWriter writer;
    encoder.encode(*itemType, proto, writer);
    const auto encoded = writer.take();
    const auto nodeGolden = unhex(
        "fe0d0200000100008801ffff010a000003060044616d616765070000000a0700"
        "646973706c61790804004e616d650500426c6164650904004c6f726508020000"
        "0003006f6e65030074776f00030a00526570616972436f737403000000090400"
        "656e63680a02000000020200696409000203006c766c05000002020069641100"
        "0203006c766c020000000000000000000000"
    );
    if (encoded != nodeGolden) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string actualHex;
        actualHex.reserve(encoded.size() * 2);
        for (const auto byte : encoded) {
            actualHex.push_back(digits[(byte >> 4) & 0x0f]);
            actualHex.push_back(digits[byte & 0x0f]);
        }
        std::cerr << "actual=" << actualHex << "\n";
        return fail("modern Item Node byte golden mismatch");
    }

    const auto decoded = registry.fromProtoDefValue(proto);
    if (!decoded.has_value() || !decoded->equals(sword) || decoded->stackId != 0 ||
        decoded->enchantments().size() != 2 || decoded->customName() != sword.customName()) {
        return fail("modern Item ProtoDef roundtrip mismatch");
    }

    sword.setBlocksCanPlaceOn({"stone", "minecraft:dirt"});
    sword.setBlocksCanDestroy({"minecraft:oak_log"});
    if (sword.blocksCanPlaceOn() !=
            std::vector<std::string>({"minecraft:stone", "minecraft:dirt"}) ||
        sword.blocksCanDestroy() !=
            std::vector<std::string>({"minecraft:oak_log"})) {
        return fail("CanPlaceOn/CanDestroy normalization mismatch");
    }
    const auto adventureProto = registry.toProtoDefValue(sword);
    const auto* adventureExtra = field(adventureProto, "extra");
    const auto* place = adventureExtra == nullptr
        ? nullptr
        : field(*adventureExtra, "can_place_on");
    if (place == nullptr || place->kind != bedrock::ProtoDefValue::Kind::Array ||
        place->arrayValue.size() != 2 ||
        place->arrayValue[0].kind != bedrock::ProtoDefValue::Kind::String ||
        place->arrayValue[0].stringValue != "minecraft:stone") {
        return fail("packet adventure-list shape mismatch");
    }
    const auto adventureRoundtrip = registry.fromProtoDefValue(adventureProto);
    if (!adventureRoundtrip.has_value() ||
        adventureRoundtrip->blocksCanPlaceOn() != sword.blocksCanPlaceOn() ||
        adventureRoundtrip->blocksCanDestroy() != sword.blocksCanDestroy()) {
        return fail("packet adventure-list roundtrip mismatch");
    }

    const auto zombie = registry.create(1163);
    const auto variation = registry.create(820, 1, 0);
    const auto unknown = registry.create(999999);
    if (zombie.spawnEggMobName() != "zombie" ||
        variation.displayName != "Minecart with Hopper" ||
        unknown.name != "unknown" || unknown.displayName != "unknown" ||
        unknown.stackSize != 1) {
        return fail("spawn egg/variation/unknown item mismatch");
    }

    auto countVariant = sword;
    countVariant.count = 64;
    if (registry.equal(&sword, &countVariant) ||
        !registry.equal(&sword, &countVariant, false) ||
        !registry.equal(nullptr, nullptr) || registry.equal(&sword, nullptr)) {
        return fail("Item.equal parity mismatch");
    }

    const auto emptyProto = registry.toProtoDefValue(nullptr);
    if (field(emptyProto, "network_id") == nullptr ||
        field(emptyProto, "network_id")->intValue != 0 ||
        registry.fromProtoDefValue(emptyProto).has_value()) {
        return fail("null Item serialization mismatch");
    }

    sword.clearEnchantments();
    sword.clearCustomLore();
    sword.clearCustomName();
    sword.setBlocksCanPlaceOn({});
    sword.setBlocksCanDestroy({});
    if (!sword.enchantments().empty() || sword.customLore().has_value() ||
        sword.customName().has_value() || !sword.blocksCanPlaceOn().empty() ||
        !sword.blocksCanDestroy().empty()) {
        return fail("NBT helper clear mismatch");
    }

    const auto legacy = assets.loadBedrockItemRegistryByVersion("0.14");
    if (!legacy.usesAuxValue() || legacy.protocolVersion() != 70 ||
        legacy.itemCount() != 287 || legacy.enchantmentCount() != 0) {
        return fail("legacy Bedrock item registry mismatch");
    }
    legacy.resetStackIds();
    const auto legacySword = legacy.create(276, 5, 3);
    const auto legacyProto = legacy.toProtoDefValue(legacySword);
    const auto* auxiliary = field(legacyProto, "auxiliary_value");
    if (auxiliary == nullptr || auxiliary->intValue != 773 ||
        field(legacyProto, "has_stack_id") != nullptr) {
        return fail("auxiliary_value Item serialization mismatch");
    }
    const auto legacyDecoded = legacy.fromProtoDefValue(legacyProto, 44);
    if (!legacyDecoded.has_value() || legacyDecoded->type != 276 ||
        legacyDecoded->count != 5 || legacyDecoded->metadata != 3 ||
        legacyDecoded->stackId != 44) {
        return fail("auxiliary_value Item roundtrip mismatch");
    }

    std::cout << "[BEDROCK-ITEM-SMOKE] ok\n";
    return 0;
}
