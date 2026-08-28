#include <bedrock/registry/BedrockRegistry.hpp>

#include <bedrock/protodef/ProtoDefJson.hpp>

#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace bedrock {
namespace {

using JsonValue = ProtoDefValue;

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open minecraft-data file: " + path.string());
    }
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    );
}

JsonValue readJsonFile(const std::filesystem::path& path) {
    try {
        return ProtoDefJson::parse(readTextFile(path));
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "failed to parse minecraft-data JSON " + path.string() + ": " + error.what()
        );
    }
}

const JsonValue& requireKind(
    const JsonValue& value,
    JsonValue::Kind kind,
    std::string_view context
) {
    if (value.kind != kind) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) + " has an unexpected JSON type"
        );
    }
    return value;
}

const JsonValue& requireField(
    const JsonValue& object,
    std::string_view field,
    std::string_view context
) {
    requireKind(object, JsonValue::Kind::Object, context);
    const auto* value = object.get(std::string(field));
    if (value == nullptr) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) + " is missing field " +
            std::string(field)
        );
    }
    return *value;
}

const JsonValue* optionalField(const JsonValue& object, std::string_view field) {
    if (object.kind != JsonValue::Kind::Object) return nullptr;
    return object.get(std::string(field));
}

int64_t integerValue(const JsonValue& value, std::string_view context) {
    if (value.kind == JsonValue::Kind::Int) return value.intValue;
    if (value.kind == JsonValue::Kind::UInt) {
        if (value.uintValue <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return static_cast<int64_t>(value.uintValue);
        }
    }
    throw std::runtime_error(
        "minecraft-data " + std::string(context) + " must be an integer"
    );
}

int32_t checkedInt32(int64_t value, std::string_view context) {
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) + " is out of int32 range"
        );
    }
    return static_cast<int32_t>(value);
}

uint32_t checkedUInt32(int64_t value, std::string_view context) {
    if (value < 0 ||
        static_cast<uint64_t>(value) > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) + " is out of uint32 range"
        );
    }
    return static_cast<uint32_t>(value);
}

double numberValue(const JsonValue& value, std::string_view context) {
    switch (value.kind) {
        case JsonValue::Kind::Int: return static_cast<double>(value.intValue);
        case JsonValue::Kind::UInt: return static_cast<double>(value.uintValue);
        case JsonValue::Kind::Double: return value.doubleValue;
        default:
            throw std::runtime_error(
                "minecraft-data " + std::string(context) + " must be a number"
            );
    }
}

std::optional<double> optionalNumber(
    const JsonValue* value,
    std::string_view context
) {
    if (value == nullptr || value->kind == JsonValue::Kind::Null) return std::nullopt;
    return numberValue(*value, context);
}

std::string stringValue(const JsonValue& value, std::string_view context) {
    requireKind(value, JsonValue::Kind::String, context);
    return value.stringValue;
}

std::optional<std::string> optionalString(
    const JsonValue* value,
    std::string_view context
) {
    if (value == nullptr || value->kind == JsonValue::Kind::Null) return std::nullopt;
    return stringValue(*value, context);
}

bool boolValue(const JsonValue& value, std::string_view context) {
    requireKind(value, JsonValue::Kind::Bool, context);
    return value.boolValue;
}

} // namespace

bool BedrockBiomeDefinition::precipitationEnabled() const {
    if (hasPrecipitation.has_value()) return *hasPrecipitation;
    if (precipitation.has_value()) return *precipitation != "none";
    return rainfall.value_or(0.0) > 0.0;
}

const BedrockBiomeDefinition* BedrockBiomeRegistry::biomeById(int32_t id) const {
    const auto found = biomeIndicesById_.find(id);
    return found == biomeIndicesById_.end() ? nullptr : &biomes_[found->second];
}

const BedrockBiomeDefinition* BedrockBiomeRegistry::biomeByName(
    std::string_view name
) const {
    const auto found = biomeIndicesByName_.find(normalizeName(name));
    return found == biomeIndicesByName_.end() ? nullptr : &biomes_[found->second];
}

BedrockBiomeDefinition BedrockBiomeRegistry::biome(int32_t id) const {
    if (const auto* found = biomeById(id)) return *found;

    BedrockBiomeDefinition empty;
    empty.id = id;
    empty.rainfall = 0.0;
    return empty;
}

const std::vector<BedrockBiomeDefinition>& BedrockBiomeRegistry::all() const {
    return biomes_;
}

std::size_t BedrockBiomeRegistry::biomeCount() const {
    return biomes_.size();
}

std::string BedrockBiomeRegistry::normalizeName(std::string_view name) {
    if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
    return std::string(name);
}

BedrockBiomeRegistry BedrockBiomeRegistryLoader::loadMinecraftData(
    const std::filesystem::path& biomesJson
) {
    const auto root = readJsonFile(biomesJson);
    requireKind(root, JsonValue::Kind::Array, "biomes.json root");

    BedrockBiomeRegistry registry;
    registry.biomes_.reserve(root.arrayValue.size());

    for (const auto& value : root.arrayValue) {
        BedrockBiomeDefinition biome;
        biome.id = checkedInt32(
            integerValue(requireField(value, "id", "biome"), "biome.id"),
            "biome.id"
        );
        biome.name = BedrockBiomeRegistry::normalizeName(
            stringValue(requireField(value, "name", "biome"), "biome.name")
        );
        biome.category = stringValue(
            requireField(value, "category", "biome"),
            "biome.category"
        );
        biome.precipitation = optionalString(
            optionalField(value, "precipitation"),
            "biome.precipitation"
        );
        if (const auto* hasPrecipitation = optionalField(value, "has_precipitation")) {
            biome.hasPrecipitation = boolValue(
                *hasPrecipitation,
                "biome.has_precipitation"
            );
        }
        biome.depth = optionalNumber(optionalField(value, "depth"), "biome.depth");
        biome.dimension = stringValue(
            requireField(value, "dimension", "biome"),
            "biome.dimension"
        );
        biome.displayName = stringValue(
            requireField(value, "displayName", "biome"),
            "biome.displayName"
        );
        biome.color = checkedUInt32(
            integerValue(requireField(value, "color", "biome"), "biome.color"),
            "biome.color"
        );
        biome.rainfall = optionalNumber(
            optionalField(value, "rainfall"),
            "biome.rainfall"
        );
        biome.temperature = numberValue(
            requireField(value, "temperature", "biome"),
            "biome.temperature"
        );
        biome.height = optionalNumber(optionalField(value, "height"), "biome.height");

        if (const auto* child = optionalField(value, "child")) {
            biome.child = checkedInt32(integerValue(*child, "biome.child"), "biome.child");
        }
        biome.parent = optionalString(optionalField(value, "parent"), "biome.parent");

        if (const auto* climates = optionalField(value, "climates")) {
            requireKind(*climates, JsonValue::Kind::Array, "biome.climates");
            biome.climates.reserve(climates->arrayValue.size());
            for (const auto& climateValue : climates->arrayValue) {
                BedrockBiomeClimate climate;
                climate.temperature = numberValue(
                    requireField(climateValue, "temperature", "biome climate"),
                    "biome climate.temperature"
                );
                climate.humidity = numberValue(
                    requireField(climateValue, "humidity", "biome climate"),
                    "biome climate.humidity"
                );
                climate.altitude = numberValue(
                    requireField(climateValue, "altitude", "biome climate"),
                    "biome climate.altitude"
                );
                climate.weirdness = numberValue(
                    requireField(climateValue, "weirdness", "biome climate"),
                    "biome climate.weirdness"
                );
                climate.offset = numberValue(
                    requireField(climateValue, "offset", "biome climate"),
                    "biome climate.offset"
                );
                biome.climates.push_back(climate);
            }
        }

        if (registry.biomeIndicesById_.contains(biome.id)) {
            throw std::runtime_error(
                "duplicate Bedrock biome id: " + std::to_string(biome.id)
            );
        }

        const auto id = biome.id;
        const auto name = biome.name;
        const auto index = registry.biomes_.size();
        registry.biomes_.push_back(std::move(biome));
        registry.biomeIndicesById_.emplace(id, index);
        registry.biomeIndicesByName_.insert_or_assign(name, index);
    }

    return registry;
}

const BedrockEntityDefinition* BedrockEntityRegistry::entityById(int32_t id) const {
    const auto found = entityIndicesById_.find(id);
    return found == entityIndicesById_.end() ? nullptr : &entities_[found->second];
}

const BedrockEntityDefinition* BedrockEntityRegistry::entityByName(
    std::string_view name
) const {
    const auto found = entityIndexByName_.find(normalizeName(name));
    return found == entityIndexByName_.end() ? nullptr : &entities_[found->second];
}

const BedrockEntityDefinition* BedrockEntityRegistry::entityByInternalId(
    int32_t internalId
) const {
    const auto found = entityIndexByInternalId_.find(internalId);
    return found == entityIndexByInternalId_.end() ? nullptr : &entities_[found->second];
}

std::vector<const BedrockEntityDefinition*> BedrockEntityRegistry::entitiesByName(
    std::string_view name
) const {
    std::vector<const BedrockEntityDefinition*> out;
    const auto found = entityIndicesByName_.find(normalizeName(name));
    if (found == entityIndicesByName_.end()) return out;
    out.reserve(found->second.size());
    for (const auto index : found->second) out.push_back(&entities_[index]);
    return out;
}

std::vector<const BedrockEntityDefinition*> BedrockEntityRegistry::entitiesByInternalId(
    int32_t internalId
) const {
    std::vector<const BedrockEntityDefinition*> out;
    const auto found = entityIndicesByInternalId_.find(internalId);
    if (found == entityIndicesByInternalId_.end()) return out;
    out.reserve(found->second.size());
    for (const auto index : found->second) out.push_back(&entities_[index]);
    return out;
}

const std::vector<BedrockEntityDefinition>& BedrockEntityRegistry::all() const {
    return entities_;
}

std::size_t BedrockEntityRegistry::entityCount() const {
    return entities_.size();
}

std::size_t BedrockEntityRegistry::uniqueEntityNameCount() const {
    return entityIndexByName_.size();
}

std::string BedrockEntityRegistry::normalizeName(std::string_view name) {
    if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
    return std::string(name);
}

BedrockEntityRegistry BedrockEntityRegistryLoader::loadMinecraftData(
    const std::filesystem::path& entitiesJson
) {
    const auto root = readJsonFile(entitiesJson);
    requireKind(root, JsonValue::Kind::Array, "entities.json root");

    BedrockEntityRegistry registry;
    registry.entities_.reserve(root.arrayValue.size());

    for (const auto& value : root.arrayValue) {
        BedrockEntityDefinition entity;
        entity.id = checkedInt32(
            integerValue(requireField(value, "id", "entity"), "entity.id"),
            "entity.id"
        );
        entity.internalId = checkedInt32(
            integerValue(
                requireField(value, "internalId", "entity"),
                "entity.internalId"
            ),
            "entity.internalId"
        );
        entity.name = BedrockEntityRegistry::normalizeName(
            stringValue(requireField(value, "name", "entity"), "entity.name")
        );
        entity.displayName = stringValue(
            requireField(value, "displayName", "entity"),
            "entity.displayName"
        );
        entity.height = optionalNumber(optionalField(value, "height"), "entity.height");
        entity.width = optionalNumber(optionalField(value, "width"), "entity.width");
        entity.length = optionalNumber(optionalField(value, "length"), "entity.length");
        entity.offset = optionalNumber(optionalField(value, "offset"), "entity.offset");
        entity.type = stringValue(
            requireField(value, "type", "entity"),
            "entity.type"
        );
        entity.category = optionalString(
            optionalField(value, "category"),
            "entity.category"
        );

        if (registry.entityIndicesById_.contains(entity.id)) {
            throw std::runtime_error(
                "duplicate Bedrock entity id: " + std::to_string(entity.id)
            );
        }

        const auto id = entity.id;
        const auto internalId = entity.internalId;
        const auto name = entity.name;
        const auto index = registry.entities_.size();
        registry.entities_.push_back(std::move(entity));
        registry.entityIndicesById_.emplace(id, index);
        registry.entityIndexByName_.insert_or_assign(name, index);
        registry.entityIndexByInternalId_.insert_or_assign(internalId, index);
        registry.entityIndicesByName_[name].push_back(index);
        registry.entityIndicesByInternalId_[internalId].push_back(index);
    }

    return registry;
}

BedrockRegistry::BedrockRegistry(
    MinecraftDataVersionInfo version,
    BedrockBlockRegistry blocks,
    BedrockItemRegistry items,
    BedrockBiomeRegistry biomes,
    BedrockEntityRegistry entities,
    BedrockRecipeRegistry recipes,
    BedrockWindowRegistry windows,
    BedrockInstrumentRegistry instruments,
    BedrockAttributeRegistry attributes,
    BedrockFeatureRegistry features,
    BedrockLootRegistry loot,
    std::optional<BedrockDefaultSkin> defaultSkin
) : version_(std::move(version)),
    blocks_(std::move(blocks)),
    items_(std::move(items)),
    biomes_(std::move(biomes)),
    entities_(std::move(entities)),
    recipes_(std::move(recipes)),
    windows_(std::move(windows)),
    instruments_(std::move(instruments)),
    attributes_(std::move(attributes)),
    features_(std::move(features)),
    loot_(std::move(loot)),
    defaultSkin_(std::move(defaultSkin)) {}

const MinecraftDataVersionInfo& BedrockRegistry::version() const {
    return version_;
}

int32_t BedrockRegistry::protocolVersion() const {
    return version_.protocol;
}

BedrockBlockRegistry& BedrockRegistry::blocks() { return blocks_; }
const BedrockBlockRegistry& BedrockRegistry::blocks() const { return blocks_; }
BedrockItemRegistry& BedrockRegistry::items() { return items_; }
const BedrockItemRegistry& BedrockRegistry::items() const { return items_; }
BedrockBiomeRegistry& BedrockRegistry::biomes() { return biomes_; }
const BedrockBiomeRegistry& BedrockRegistry::biomes() const { return biomes_; }
BedrockEntityRegistry& BedrockRegistry::entities() { return entities_; }
const BedrockEntityRegistry& BedrockRegistry::entities() const { return entities_; }
BedrockRecipeRegistry& BedrockRegistry::recipes() { return recipes_; }
const BedrockRecipeRegistry& BedrockRegistry::recipes() const { return recipes_; }
BedrockWindowRegistry& BedrockRegistry::windows() { return windows_; }
const BedrockWindowRegistry& BedrockRegistry::windows() const { return windows_; }
BedrockInstrumentRegistry& BedrockRegistry::instruments() { return instruments_; }
const BedrockInstrumentRegistry& BedrockRegistry::instruments() const {
    return instruments_;
}
BedrockAttributeRegistry& BedrockRegistry::attributes() { return attributes_; }
const BedrockAttributeRegistry& BedrockRegistry::attributes() const {
    return attributes_;
}
BedrockFeatureRegistry& BedrockRegistry::features() { return features_; }
const BedrockFeatureRegistry& BedrockRegistry::features() const { return features_; }
BedrockLootRegistry& BedrockRegistry::loot() { return loot_; }
const BedrockLootRegistry& BedrockRegistry::loot() const { return loot_; }
BedrockDefaultSkin* BedrockRegistry::defaultSkin() {
    return defaultSkin_ ? &*defaultSkin_ : nullptr;
}
const BedrockDefaultSkin* BedrockRegistry::defaultSkin() const {
    return defaultSkin_ ? &*defaultSkin_ : nullptr;
}

void BedrockRegistry::loadItemStates(
    const std::vector<BedrockItemState>& itemStates
) {
    items_.loadItemStates(itemStates);
}

void BedrockRegistry::loadItemStates(const ProtoDefValue& itemStates) {
    items_.loadItemStates(itemStates);
}

std::vector<BedrockItemState> BedrockRegistry::writeItemStates() const {
    return items_.writeItemStates();
}

ProtoDefValue BedrockRegistry::writeItemStatesValue() const {
    return items_.writeItemStatesValue();
}

void BedrockRegistry::handleStartGame(
    const std::vector<BedrockItemState>& itemStates,
    bool blockNetworkIdsAreHashes
) {
    loadItemStates(itemStates);
    blocks_.loadRuntimeIds(
        features_.supportsFeature("blockHashes") && blockNetworkIdsAreHashes
    );
}

void BedrockRegistry::handleStartGame(const ProtoDefValue& packet) {
    if (packet.kind != ProtoDefValue::Kind::Object) {
        throw std::runtime_error("Bedrock start_game value must be an object");
    }
    if (const auto* itemStates = packet.get("itemstates")) {
        loadItemStates(*itemStates);
    }

    bool blockNetworkIdsAreHashes = false;
    if (const auto* value = packet.get("block_network_ids_are_hashes")) {
        if (value->kind != ProtoDefValue::Kind::Bool) {
            throw std::runtime_error(
                "Bedrock start_game.block_network_ids_are_hashes must be a bool"
            );
        }
        blockNetworkIdsAreHashes = value->boolValue;
    }
    blocks_.loadRuntimeIds(
        features_.supportsFeature("blockHashes") && blockNetworkIdsAreHashes
    );
}

void BedrockRegistry::handleItemRegistry(const ProtoDefValue& packet) {
    if (packet.kind != ProtoDefValue::Kind::Object) {
        throw std::runtime_error("Bedrock item_registry value must be an object");
    }
    const auto* itemStates = packet.get("itemstates");
    if (itemStates == nullptr) {
        throw std::runtime_error("Bedrock item_registry is missing itemstates");
    }
    loadItemStates(*itemStates);
}

const BedrockBlockDefinition* BedrockRegistry::blockByType(uint32_t type) const {
    return blocks_.blockByType(type);
}

const BedrockBlockDefinition* BedrockRegistry::blockByName(std::string_view name) const {
    return blocks_.blockByName(name);
}

const BedrockItemDefinition* BedrockRegistry::itemById(int32_t id) const {
    return items_.itemById(id);
}

const BedrockItemDefinition* BedrockRegistry::itemByName(std::string_view name) const {
    return items_.itemByName(name);
}

const BedrockBiomeDefinition* BedrockRegistry::biomeById(int32_t id) const {
    return biomes_.biomeById(id);
}

const BedrockBiomeDefinition* BedrockRegistry::biomeByName(std::string_view name) const {
    return biomes_.biomeByName(name);
}

const BedrockEntityDefinition* BedrockRegistry::entityById(int32_t id) const {
    return entities_.entityById(id);
}

const BedrockEntityDefinition* BedrockRegistry::entityByName(std::string_view name) const {
    return entities_.entityByName(name);
}

const BedrockRecipeDefinition* BedrockRegistry::recipeById(uint32_t id) const {
    return recipes_.recipeById(id);
}

const BedrockRecipeDefinition* BedrockRegistry::recipeByName(
    std::string_view name
) const {
    return recipes_.recipeByName(name);
}

const BedrockWindowDefinition* BedrockRegistry::windowById(
    std::string_view id
) const {
    return windows_.windowById(id);
}

const BedrockWindowDefinition* BedrockRegistry::windowByName(
    std::string_view name
) const {
    return windows_.windowByName(name);
}

const BedrockInstrumentDefinition* BedrockRegistry::instrumentById(int32_t id) const {
    return instruments_.instrumentById(id);
}

const BedrockInstrumentDefinition* BedrockRegistry::instrumentByName(
    std::string_view name
) const {
    return instruments_.instrumentByName(name);
}

const BedrockAttributeDefinition* BedrockRegistry::attributeByName(
    std::string_view name
) const {
    return attributes_.attributeByName(name);
}

const BedrockAttributeDefinition* BedrockRegistry::attributeByResource(
    std::string_view resource
) const {
    return attributes_.attributeByResource(resource);
}

ProtoDefValue BedrockRegistry::supportFeature(std::string_view name) const {
    return features_.supportFeature(name);
}

bool BedrockRegistry::supportsFeature(std::string_view name) const {
    return features_.supportsFeature(name);
}

const BedrockBlockLootDefinition* BedrockRegistry::blockLootByName(
    std::string_view block
) const {
    return loot_.blockLootByName(block);
}

const BedrockBlockLootDefinition* BedrockRegistry::blockLootForStates(
    std::string_view block,
    const BedrockLootBlockStates& states
) const {
    return loot_.blockLootForStates(block, states);
}

const BedrockEntityLootDefinition* BedrockRegistry::entityLootByName(
    std::string_view entity
) const {
    return loot_.entityLootByName(entity);
}

const BedrockBlockStateDefinition* BedrockRegistry::blockStateByRuntimeId(
    int32_t runtimeId
) const {
    return blocks_.stateByRuntimeId(runtimeId);
}

const BedrockBlockDefinition* BedrockRegistry::blockByRuntimeId(
    int32_t runtimeId
) const {
    return blocks_.blockByRuntimeId(runtimeId);
}

} // namespace bedrock
