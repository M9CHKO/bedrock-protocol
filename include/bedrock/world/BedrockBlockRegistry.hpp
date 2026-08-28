#pragma once

#include <bedrock/nbt/BedrockNbt.hpp>
#include <bedrock/world/BlockRuntimeRegistry.hpp>
#include <bedrock/world/WorldIterators.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bedrock {

enum class BedrockBlockPropertyType {
    Byte,
    Int,
    String
};

struct BedrockBlockProperty {
    BedrockBlockPropertyType type = BedrockBlockPropertyType::Int;
    int64_t integerValue = 0;
    std::string stringValue;

    static BedrockBlockProperty byte(int64_t value);
    static BedrockBlockProperty integer(int64_t value);
    static BedrockBlockProperty string(std::string value);

    bool isNumeric() const;
    std::optional<int64_t> asInteger() const;
    std::optional<std::string_view> asString() const;
    std::string toString() const;

    // JavaScript represents both Bedrock byte and int properties as Number.
    // Preserve that comparison behavior while still exposing the source type.
    bool operator==(const BedrockBlockProperty& other) const;
    bool operator!=(const BedrockBlockProperty& other) const {
        return !(*this == other);
    }
};

using BedrockBlockProperties = std::map<std::string, BedrockBlockProperty>;

// minecraft-data Bedrock collision boxes use center xyz + size xyz. This is
// deliberately distinct in meaning from BlockShape, whose coordinates are
// min xyz + max xyz for ray intersection.
using BedrockCollisionShape = std::array<double, 6>;

struct BedrockBlockVariation {
    int32_t metadata = 0;
    std::string displayName;
};

struct BedrockBlockDefinition {
    uint32_t id = 0;
    std::string name;
    std::string displayName;
    double hardness = 0.0;
    double resistance = 0.0;
    uint32_t stackSize = 0;
    bool diggable = false;
    std::string material;
    bool transparent = true;
    uint8_t emitLight = 0;
    uint8_t filterLight = 0;
    int32_t defaultState = 0;
    int32_t minStateId = 0;
    int32_t maxStateId = 0;
    std::optional<std::unordered_set<uint32_t>> harvestTools;
    std::vector<uint32_t> drops;
    std::vector<BedrockBlockVariation> variations;
    std::string boundingBox = "empty";
};

struct BedrockBlockStateDefinition {
    int32_t stateId = 0;
    uint32_t type = 0;
    std::string name;
    BedrockBlockProperties properties;
    int32_t version = 0;
    std::vector<BedrockCollisionShape> shapes;
    bool missingStateShape = false;
    std::optional<int32_t> hash;
};

struct BedrockDigTimeOptions {
    std::optional<uint32_t> heldItemType;
    bool creative = false;
    bool inWater = false;
    bool notOnGround = false;

    // Tool multipliers are caller-supplied because minecraft-data currently
    // redirects Bedrock materials to Java data, which this library excludes.
    double toolMultiplier = 1.0;
    uint32_t efficiencyLevel = 0;
    uint32_t hasteLevel = 0;
    uint32_t conduitPowerLevel = 0;
    uint32_t miningFatigueLevel = 0;
    uint32_t aquaAffinityLevel = 0;
};

struct BedrockBlock {
    uint32_t type = 0;
    int32_t metadata = 0;
    int32_t stateId = 0;
    int32_t biomeId = 0;
    uint8_t light = 0;
    uint8_t skyLight = 0;
    std::optional<WorldBlockPosition> position;
    std::optional<NbtDocument> entity;

    std::string name;
    std::string displayName;
    double hardness = 0.0;
    double resistance = 0.0;
    uint32_t stackSize = 0;
    bool diggable = false;
    std::string material;
    bool transparent = true;
    uint8_t emitLight = 0;
    uint8_t filterLight = 0;
    int32_t defaultState = 0;
    int32_t minStateId = 0;
    int32_t maxStateId = 0;
    std::optional<std::unordered_set<uint32_t>> harvestTools;
    std::vector<uint32_t> drops;
    std::string boundingBox = "empty";

    BedrockBlockProperties properties;
    BedrockBlockProperties computedStates;
    std::vector<BedrockCollisionShape> shapes;
    bool missingStateShape = false;
    std::optional<int32_t> hash;
    std::shared_ptr<BedrockBlock> superimposed;

    const BedrockBlockProperty* property(std::string_view name) const;
    BedrockBlockProperties propertiesWithComputedStates() const;
    std::optional<bool> isWaterlogged() const;
    bool isSign() const;

    const NbtDocument* blockEntity() const;
    NbtDocument* blockEntity();
    std::vector<std::string> getSignText() const;
    void setSignText(std::string text);
    void setSignText(const std::vector<std::string>& text);
    std::string signText() const;

    bool canHarvest(std::optional<uint32_t> heldItemType = std::nullopt) const;
    std::vector<BlockShape> raycastShapes() const;

    // Returns milliseconds, zero for instant breaking, or infinity for an
    // unbreakable block. The formula mirrors prismarine-block.
    double digTime(const BedrockDigTimeOptions& options = {}) const;
};

class BedrockBlockRegistryLoader;

class BedrockBlockRegistry {
public:
    const BedrockBlockDefinition* blockByType(uint32_t type) const;
    const BedrockBlockDefinition* blockByName(std::string_view name) const;
    const BedrockBlockDefinition* blockByStateId(int32_t stateId) const;
    const BedrockBlockStateDefinition* stateById(int32_t stateId) const;
    std::vector<BlockShape> raycastShapesForState(int32_t stateId) const;

    std::optional<BedrockBlock> fromStateId(
        int32_t stateId,
        int32_t biomeId = 0
    ) const;
    std::optional<BedrockBlock> fromProperties(
        std::string_view name,
        const BedrockBlockProperties& properties,
        int32_t biomeId = 0
    ) const;
    std::optional<BedrockBlock> fromProperties(
        uint32_t type,
        const BedrockBlockProperties& properties,
        int32_t biomeId = 0
    ) const;
    std::optional<BedrockBlock> fromString(
        std::string_view value,
        int32_t biomeId = 0
    ) const;

    std::size_t blockCount() const;
    std::size_t stateCount() const;
    bool supportsBlockHashes() const;

    // prismarine-registry's blocksByRuntimeId index. It remains empty until
    // loadRuntimeIds/BedrockRegistry::handleStartGame is called.
    void loadRuntimeIds(bool hashed);
    std::optional<int32_t> stateIdForRuntimeId(int32_t runtimeId) const;
    const BedrockBlockStateDefinition* stateByRuntimeId(int32_t runtimeId) const;
    const BedrockBlockDefinition* blockByRuntimeId(int32_t runtimeId) const;
    std::size_t runtimeIdCount() const;
    bool usesHashedRuntimeIds() const;

    BlockRuntimeRegistry toRuntimeRegistry() const;

    // Matches prismarine-block's little-endian NBT/FNV-1a behavior, including
    // its historical property-name-only compound encoding quirk.
    static int32_t computeHash(
        std::string_view name,
        const BedrockBlockProperties& properties
    );

    // prismarine-registry hashes the typed blockStates descriptors when it
    // builds blocksByRuntimeId. This deliberately differs from Block::hash,
    // whose historical primitive-state encoding omits property values.
    static int32_t computeRuntimeHash(
        std::string_view name,
        const BedrockBlockProperties& properties
    );

private:
    friend class BedrockBlockRegistryLoader;

    std::unordered_map<uint32_t, BedrockBlockDefinition> blocksByType_;
    std::unordered_map<std::string, uint32_t> blockTypesByName_;
    std::vector<std::optional<BedrockBlockStateDefinition>> statesById_;
    std::size_t stateCount_ = 0;
    bool blockHashes_ = false;
    std::unordered_map<int32_t, int32_t> stateIdsByRuntimeId_;
    bool hashedRuntimeIds_ = false;

    static std::string normalizeName(std::string_view name);
};

class BedrockBlockRegistryLoader {
public:
    static BedrockBlockRegistry loadMinecraftData(
        const std::filesystem::path& blocksJson,
        const std::filesystem::path& blockStatesJson,
        const std::filesystem::path& blockCollisionShapesJson,
        bool blockHashes = true
    );
};

} // namespace bedrock
