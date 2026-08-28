#include <bedrock/registry/BedrockLootRegistry.hpp>

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
            "failed to parse minecraft-data JSON " + path.string() + ": " +
            error.what()
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
            "minecraft-data " + std::string(context) +
            " has an unexpected JSON type"
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

std::string stringValue(const JsonValue& value, std::string_view context) {
    requireKind(value, JsonValue::Kind::String, context);
    return value.stringValue;
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

int64_t integerValue(const JsonValue& value, std::string_view context) {
    if (value.kind == JsonValue::Kind::Int) return value.intValue;
    if (value.kind == JsonValue::Kind::UInt &&
        value.uintValue <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return static_cast<int64_t>(value.uintValue);
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

std::optional<bool> optionalBool(
    const JsonValue* value,
    std::string_view context
) {
    if (value == nullptr || value->kind == JsonValue::Kind::Null) {
        return std::nullopt;
    }
    requireKind(*value, JsonValue::Kind::Bool, context);
    return value->boolValue;
}

std::optional<double> optionalNumber(
    const JsonValue* value,
    std::string_view context
) {
    if (value == nullptr || value->kind == JsonValue::Kind::Null) {
        return std::nullopt;
    }
    return numberValue(*value, context);
}

std::optional<int32_t> optionalInt32(
    const JsonValue* value,
    std::string_view context
) {
    if (value == nullptr || value->kind == JsonValue::Kind::Null) {
        return std::nullopt;
    }
    return checkedInt32(integerValue(*value, context), context);
}

bool numericKind(JsonValue::Kind kind) {
    return kind == JsonValue::Kind::Int || kind == JsonValue::Kind::UInt ||
        kind == JsonValue::Kind::Double;
}

bool jsonEqual(const JsonValue& first, const JsonValue& second) {
    if (numericKind(first.kind) && numericKind(second.kind)) {
        return numberValue(first, "loot state") == numberValue(second, "loot state");
    }
    if (first.kind != second.kind) return false;
    switch (first.kind) {
        case JsonValue::Kind::Null: return true;
        case JsonValue::Kind::Bool: return first.boolValue == second.boolValue;
        case JsonValue::Kind::Int: return first.intValue == second.intValue;
        case JsonValue::Kind::UInt: return first.uintValue == second.uintValue;
        case JsonValue::Kind::Double: return first.doubleValue == second.doubleValue;
        case JsonValue::Kind::String: return first.stringValue == second.stringValue;
        case JsonValue::Kind::Bytes: return first.bytesValue == second.bytesValue;
        case JsonValue::Kind::Array:
            if (first.arrayValue.size() != second.arrayValue.size()) return false;
            for (std::size_t i = 0; i < first.arrayValue.size(); ++i) {
                if (!jsonEqual(first.arrayValue[i], second.arrayValue[i])) return false;
            }
            return true;
        case JsonValue::Kind::Object:
            if (first.objectValue.size() != second.objectValue.size()) return false;
            for (const auto& [key, value] : first.objectValue) {
                const auto found = second.objectValue.find(key);
                if (found == second.objectValue.end() ||
                    !jsonEqual(value, found->second)) {
                    return false;
                }
            }
            return true;
    }
    return false;
}

BedrockLootDrop parseDrop(const JsonValue& value, std::string_view context) {
    BedrockLootDrop drop;
    drop.item = stringValue(requireField(value, "item", context), "loot drop.item");
    drop.metadata = optionalInt32(optionalField(value, "metadata"), "loot drop.metadata");
    drop.dropChance = numberValue(
        requireField(value, "dropChance", context),
        "loot drop.dropChance"
    );

    const auto& stackSizeRange = requireKind(
        requireField(value, "stackSizeRange", context),
        JsonValue::Kind::Array,
        "loot drop.stackSizeRange"
    );
    drop.stackSizeRange.reserve(stackSizeRange.arrayValue.size());
    for (const auto& size : stackSizeRange.arrayValue) {
        drop.stackSizeRange.push_back(
            size.kind == JsonValue::Kind::Null
                ? std::nullopt
                : std::optional<double>(numberValue(size, "loot stack size"))
        );
    }

    drop.blockAge = optionalNumber(optionalField(value, "blockAge"), "loot drop.blockAge");
    drop.silkTouch = optionalBool(optionalField(value, "silkTouch"), "loot drop.silkTouch");
    drop.noSilkTouch = optionalBool(
        optionalField(value, "noSilkTouch"),
        "loot drop.noSilkTouch"
    );
    drop.playerKill = optionalBool(
        optionalField(value, "playerKill"),
        "loot drop.playerKill"
    );
    return drop;
}

std::vector<BedrockLootDrop> parseDrops(
    const JsonValue& value,
    std::string_view context
) {
    const auto& drops = requireKind(value, JsonValue::Kind::Array, context);
    std::vector<BedrockLootDrop> out;
    out.reserve(drops.arrayValue.size());
    for (const auto& drop : drops.arrayValue) out.push_back(parseDrop(drop, context));
    return out;
}

} // namespace

std::optional<double> BedrockLootDrop::minimumStackSize() const {
    if (stackSizeRange.empty()) return std::nullopt;
    return stackSizeRange.front();
}

std::optional<double> BedrockLootDrop::maximumStackSize() const {
    if (stackSizeRange.size() < 2) return std::nullopt;
    return stackSizeRange[1];
}

bool BedrockBlockLootDefinition::matchesStates(
    const BedrockLootBlockStates& blockStates
) const {
    for (const auto& [name, expected] : states) {
        const auto found = blockStates.find(name);
        if (found == blockStates.end() || !jsonEqual(expected, found->second)) {
            return false;
        }
    }
    return true;
}

const BedrockBlockLootDefinition* BedrockLootRegistry::blockLootByName(
    std::string_view block
) const {
    const auto found = blockLootIndexByName_.find(normalizeName(block));
    return found == blockLootIndexByName_.end() ? nullptr : &blockLoot_[found->second];
}

std::vector<const BedrockBlockLootDefinition*> BedrockLootRegistry::blockLootVariants(
    std::string_view block
) const {
    std::vector<const BedrockBlockLootDefinition*> out;
    const auto found = blockLootIndicesByName_.find(normalizeName(block));
    if (found == blockLootIndicesByName_.end()) return out;
    out.reserve(found->second.size());
    for (const auto index : found->second) out.push_back(&blockLoot_[index]);
    return out;
}

const BedrockBlockLootDefinition* BedrockLootRegistry::blockLootForStates(
    std::string_view block,
    const BedrockLootBlockStates& states
) const {
    const BedrockBlockLootDefinition* best = nullptr;
    const auto found = blockLootIndicesByName_.find(normalizeName(block));
    if (found == blockLootIndicesByName_.end()) return nullptr;
    for (const auto index : found->second) {
        const auto& candidate = blockLoot_[index];
        if (candidate.matchesStates(states) &&
            (best == nullptr || candidate.states.size() >= best->states.size())) {
            best = &candidate;
        }
    }
    return best;
}

const BedrockEntityLootDefinition* BedrockLootRegistry::entityLootByName(
    std::string_view entity
) const {
    const auto found = entityLootIndexByName_.find(normalizeName(entity));
    return found == entityLootIndexByName_.end() ? nullptr : &entityLoot_[found->second];
}

const std::vector<BedrockBlockLootDefinition>& BedrockLootRegistry::allBlockLoot() const {
    return blockLoot_;
}

const std::vector<BedrockEntityLootDefinition>& BedrockLootRegistry::allEntityLoot() const {
    return entityLoot_;
}

std::size_t BedrockLootRegistry::blockLootCount() const {
    return blockLoot_.size();
}

std::size_t BedrockLootRegistry::uniqueBlockLootCount() const {
    return blockLootIndexByName_.size();
}

std::size_t BedrockLootRegistry::entityLootCount() const {
    return entityLoot_.size();
}

std::string BedrockLootRegistry::normalizeName(std::string_view name) {
    if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
    return std::string(name);
}

BedrockLootRegistry BedrockLootRegistryLoader::loadMinecraftData(
    const std::filesystem::path& blockLootJson,
    const std::filesystem::path& entityLootJson
) {
    BedrockLootRegistry registry;

    if (!blockLootJson.empty()) {
        const auto root = readJsonFile(blockLootJson);
        requireKind(root, JsonValue::Kind::Array, "blockLoot.json root");
        registry.blockLoot_.reserve(root.arrayValue.size());
        for (const auto& value : root.arrayValue) {
            BedrockBlockLootDefinition definition;
            definition.block = BedrockLootRegistry::normalizeName(stringValue(
                requireField(value, "block", "block loot"),
                "block loot.block"
            ));
            definition.drops = parseDrops(
                requireField(value, "drops", "block loot"),
                "block loot.drops"
            );
            if (const auto* states = optionalField(value, "states");
                states != nullptr && states->kind != JsonValue::Kind::Null) {
                requireKind(*states, JsonValue::Kind::Object, "block loot.states");
                definition.states = states->objectValue;
            }

            const auto index = registry.blockLoot_.size();
            const auto name = definition.block;
            registry.blockLoot_.push_back(std::move(definition));
            registry.blockLootIndexByName_.insert_or_assign(name, index);
            registry.blockLootIndicesByName_[name].push_back(index);
        }
    }

    if (!entityLootJson.empty()) {
        const auto root = readJsonFile(entityLootJson);
        requireKind(root, JsonValue::Kind::Array, "entityLoot.json root");
        registry.entityLoot_.reserve(root.arrayValue.size());
        for (const auto& value : root.arrayValue) {
            BedrockEntityLootDefinition definition;
            definition.entity = BedrockLootRegistry::normalizeName(stringValue(
                requireField(value, "entity", "entity loot"),
                "entity loot.entity"
            ));
            definition.drops = parseDrops(
                requireField(value, "drops", "entity loot"),
                "entity loot.drops"
            );

            const auto index = registry.entityLoot_.size();
            const auto name = definition.entity;
            registry.entityLoot_.push_back(std::move(definition));
            registry.entityLootIndexByName_.insert_or_assign(name, index);
        }
    }

    return registry;
}

} // namespace bedrock
