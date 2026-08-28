#pragma once

#include <bedrock/nbt/BedrockNbt.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bedrock {

struct BedrockItemVariation {
    int32_t metadata = 0;
    int32_t id = 0;
    std::string name;
    std::string displayName;
    uint32_t stackSize = 0;
};

struct BedrockItemDefinition {
    int32_t id = 0;
    std::string name;
    std::string displayName;
    uint32_t stackSize = 0;
    std::optional<int32_t> metadata;
    std::optional<int32_t> maxDurability;
    std::vector<std::string> enchantCategories;
    std::vector<std::string> repairWith;
    std::vector<BedrockItemVariation> variations;
};

// Bedrock Itemstates entry used by start_game before 1.21.60 and by the
// dedicated item_registry packet afterward. `version` and `nbt` retain newer
// packet fields without imposing one protocol revision's schema on another.
struct BedrockItemState {
    std::string name;
    int32_t runtimeId = 0;
    bool componentBased = false;
    std::optional<ProtoDefValue> version;
    std::optional<ProtoDefValue> nbt;
};

struct BedrockEnchantmentCost {
    int32_t a = 0;
    int32_t b = 0;

    int32_t atLevel(int32_t level) const;
};

struct BedrockEnchantmentDefinition {
    int16_t id = 0;
    std::string name;
    std::string displayName;
    int32_t maxLevel = 0;
    BedrockEnchantmentCost minCost;
    BedrockEnchantmentCost maxCost;
    bool treasureOnly = false;
    bool curse = false;
    std::vector<std::string> exclude;
    std::string category;
    int32_t weight = 0;
    bool tradeable = false;
    bool discoverable = false;
};

struct BedrockItemEnchantment {
    int16_t id = 0;
    std::optional<std::string> name;
    int16_t level = 0;
};

struct BedrockNamedEnchantment {
    std::string name;
    int16_t level = 0;
};

// Strongly typed representation of the Bedrock protocol Item payload. The
// registry converts this to/from the ProtoDefValue packet shape for a version.
struct BedrockNetworkItem {
    int32_t networkId = 0;
    uint16_t count = 0;
    int32_t metadata = 0;
    std::optional<int32_t> stackId;
    int32_t blockRuntimeId = 0;
    std::optional<NbtDocument> nbt;
    std::vector<std::string> canPlaceOn;
    std::vector<std::string> canDestroy;
    int64_t blockingTick = 0;
};

struct BedrockItemRegistryData;
class BedrockItemRegistry;

class BedrockItem {
public:
    int32_t type = 0;
    uint16_t count = 0;
    int32_t metadata = 0;
    std::optional<NbtDocument> nbt;
    std::optional<int32_t> stackId;

    std::string name;
    std::string displayName;
    uint32_t stackSize = 1;
    std::optional<int32_t> maxDurability;

    bool hasNbtPayload() const;
    bool equals(
        const BedrockItem& other,
        bool matchStackSize = true,
        bool matchNbt = true
    ) const;

    std::optional<std::string> customName() const;
    void setCustomName(std::string value);
    void clearCustomName();

    std::optional<std::vector<std::string>> customLore() const;
    void setCustomLore(std::vector<std::string> value);
    void setCustomLore(std::string value);
    void clearCustomLore();

    int32_t repairCost() const;
    void setRepairCost(int32_t value);

    std::optional<int32_t> durabilityUsed() const;
    void setDurabilityUsed(int32_t value);

    std::vector<BedrockItemEnchantment> enchantments() const;
    void setEnchantments(const std::vector<BedrockNamedEnchantment>& value);
    void clearEnchantments();

    std::vector<std::string> blocksCanPlaceOn() const;
    void setBlocksCanPlaceOn(const std::vector<std::string>& value);
    std::vector<std::string> blocksCanDestroy() const;
    void setBlocksCanDestroy(const std::vector<std::string>& value);

    std::string spawnEggMobName() const;

private:
    friend class BedrockItemRegistry;

    std::shared_ptr<BedrockItemRegistryData> registry_;

    NbtValue& ensureNbtRoot();
};

class BedrockItemRegistryLoader;

class BedrockItemRegistry {
public:
    BedrockItemRegistry();

    const BedrockItemDefinition* itemById(int32_t id) const;
    const BedrockItemDefinition* itemByName(std::string_view name) const;
    const BedrockEnchantmentDefinition* enchantmentById(int16_t id) const;
    const BedrockEnchantmentDefinition* enchantmentByName(std::string_view name) const;

    BedrockItem create(
        int32_t type,
        uint16_t count = 1,
        int32_t metadata = 0,
        std::optional<NbtDocument> nbt = std::nullopt,
        std::optional<int32_t> stackId = std::nullopt,
        bool sentByServer = false
    ) const;

    std::optional<BedrockItem> fromNetwork(
        const BedrockNetworkItem& value,
        std::optional<int32_t> legacyStackId = std::nullopt
    ) const;
    BedrockNetworkItem toNetwork(
        const BedrockItem* item,
        bool serverAuthoritative = true
    ) const;
    BedrockNetworkItem toNetwork(
        const BedrockItem& item,
        bool serverAuthoritative = true
    ) const {
        return toNetwork(&item, serverAuthoritative);
    }

    std::optional<BedrockItem> fromProtoDefValue(
        const ProtoDefValue& value,
        std::optional<int32_t> legacyStackId = std::nullopt
    ) const;
    ProtoDefValue toProtoDefValue(
        const BedrockItem* item,
        bool serverAuthoritative = true
    ) const;
    ProtoDefValue toProtoDefValue(
        const BedrockItem& item,
        bool serverAuthoritative = true
    ) const {
        return toProtoDefValue(&item, serverAuthoritative);
    }

    bool equal(
        const BedrockItem* first,
        const BedrockItem* second,
        bool matchStackSize = true,
        bool matchNbt = true
    ) const;

    // prismarine-registry Bedrock dynamic palette API. Loading replaces the
    // static item indexes while copying known metadata by item name.
    void loadItemStates(const std::vector<BedrockItemState>& itemStates);
    void loadItemStates(const ProtoDefValue& itemStates);
    std::vector<BedrockItemState> writeItemStates() const;
    ProtoDefValue writeItemStatesValue() const;

    int32_t nextStackId() const;
    int32_t currentStackId() const;
    void resetStackIds(int32_t next = 0) const;

    int32_t protocolVersion() const;
    bool usesAuxValue() const;
    std::size_t itemCount() const;
    std::size_t enchantmentCount() const;

private:
    friend class BedrockItemRegistryLoader;

    std::shared_ptr<BedrockItemRegistryData> data_;

    static std::string normalizeName(std::string_view name);
};

class BedrockItemRegistryLoader {
public:
    static BedrockItemRegistry loadMinecraftData(
        const std::filesystem::path& itemsJson,
        const std::filesystem::path& enchantmentsJson,
        int32_t protocolVersion,
        std::optional<bool> usesAuxValue = std::nullopt
    );
};

} // namespace bedrock
