#include <bedrock/world/BedrockBlockRegistry.hpp>

#include <bedrock/protodef/ProtoDefJson.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace bedrock {
namespace {

using JsonValue = ProtoDefValue;

struct CollisionShapeSelection {
    std::vector<int32_t> shapeIds;
    bool stateSpecific = false;
};

struct CollisionShapeData {
    std::unordered_map<std::string, CollisionShapeSelection> blocks;
    std::unordered_map<int32_t, std::vector<BedrockCollisionShape>> shapes;
    std::vector<BedrockCollisionShape> fallbackShapes {
        BedrockCollisionShape {0.5, 0.5, 0.5, 1.0, 1.0, 1.0}
    };
};

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
        if (value.uintValue > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            throw std::runtime_error(
                "minecraft-data " + std::string(context) + " integer is out of range"
            );
        }
        return static_cast<int64_t>(value.uintValue);
    }
    throw std::runtime_error(
        "minecraft-data " + std::string(context) + " must be an integer"
    );
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

std::string stringValue(const JsonValue& value, std::string_view context) {
    requireKind(value, JsonValue::Kind::String, context);
    return value.stringValue;
}

bool boolValue(const JsonValue& value, std::string_view context) {
    requireKind(value, JsonValue::Kind::Bool, context);
    return value.boolValue;
}

uint32_t checkedUInt32(int64_t value, std::string_view context) {
    if (value < 0 || static_cast<uint64_t>(value) > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) + " is out of uint32 range"
        );
    }
    return static_cast<uint32_t>(value);
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

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

BedrockBlockProperty parseStringProperty(std::string value) {
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return BedrockBlockProperty::string(value.substr(1, value.size() - 2));
    }
    if (value == "true") return BedrockBlockProperty::byte(1);
    if (value == "false") return BedrockBlockProperty::byte(0);

    int64_t integer = 0;
    const auto result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        integer
    );
    if (result.ec == std::errc() && result.ptr == value.data() + value.size()) {
        return BedrockBlockProperty::integer(integer);
    }

    return BedrockBlockProperty::string(std::move(value));
}

CollisionShapeData parseCollisionShapes(const JsonValue& root) {
    CollisionShapeData out;
    const auto& blocks = requireField(root, "blocks", "blockCollisionShapes root");
    const auto& shapes = requireField(root, "shapes", "blockCollisionShapes root");
    requireKind(blocks, JsonValue::Kind::Object, "blockCollisionShapes.blocks");
    requireKind(shapes, JsonValue::Kind::Object, "blockCollisionShapes.shapes");

    for (const auto& [name, value] : blocks.objectValue) {
        CollisionShapeSelection selection;
        if (value.kind == JsonValue::Kind::Array) {
            selection.stateSpecific = true;
            selection.shapeIds.reserve(value.arrayValue.size());
            for (const auto& id : value.arrayValue) {
                selection.shapeIds.push_back(checkedInt32(
                    integerValue(id, "collision shape id"),
                    "collision shape id"
                ));
            }
        } else {
            selection.shapeIds.push_back(checkedInt32(
                integerValue(value, "collision shape id"),
                "collision shape id"
            ));
        }
        out.blocks.emplace(name, std::move(selection));
    }

    for (const auto& [idText, value] : shapes.objectValue) {
        int32_t id = 0;
        const auto parsed = std::from_chars(
            idText.data(),
            idText.data() + idText.size(),
            id
        );
        if (parsed.ec != std::errc() || parsed.ptr != idText.data() + idText.size()) {
            throw std::runtime_error("invalid collision shape id: " + idText);
        }

        requireKind(value, JsonValue::Kind::Array, "collision shape");
        std::vector<BedrockCollisionShape> boxes;
        boxes.reserve(value.arrayValue.size());
        for (const auto& boxValue : value.arrayValue) {
            requireKind(boxValue, JsonValue::Kind::Array, "collision shape box");
            if (boxValue.arrayValue.size() != 6) {
                throw std::runtime_error("collision shape box must contain six numbers");
            }
            BedrockCollisionShape box {};
            for (std::size_t i = 0; i < box.size(); ++i) {
                box[i] = numberValue(boxValue.arrayValue[i], "collision shape coordinate");
            }
            boxes.push_back(box);
        }
        out.shapes.emplace(id, std::move(boxes));
    }

    const auto stone = out.blocks.find("stone");
    if (stone != out.blocks.end() && !stone->second.shapeIds.empty()) {
        const auto shape = out.shapes.find(stone->second.shapeIds.front());
        if (shape != out.shapes.end()) out.fallbackShapes = shape->second;
    }

    return out;
}

std::pair<std::vector<BedrockCollisionShape>, bool> shapesForState(
    const CollisionShapeData& data,
    const BedrockBlockDefinition& block,
    int32_t stateId
) {
    const auto selectionIt = data.blocks.find(block.name);
    if (selectionIt == data.blocks.end() || selectionIt->second.shapeIds.empty()) {
        return {data.fallbackShapes, false};
    }

    const auto& selection = selectionIt->second;
    std::size_t index = 0;
    bool missing = false;
    if (selection.stateSpecific) {
        const auto metadata = static_cast<int64_t>(stateId) - block.minStateId;
        if (metadata >= 0 && static_cast<uint64_t>(metadata) < selection.shapeIds.size()) {
            index = static_cast<std::size_t>(metadata);
        } else {
            missing = true;
        }
    }

    const auto shape = data.shapes.find(selection.shapeIds[index]);
    if (shape != data.shapes.end()) return {shape->second, missing};

    const auto firstShape = data.shapes.find(selection.shapeIds.front());
    if (firstShape != data.shapes.end()) return {firstShape->second, true};
    return {data.fallbackShapes, true};
}

void appendLe16(std::vector<uint8_t>& out, std::size_t value) {
    if (value > std::numeric_limits<uint16_t>::max()) {
        throw std::runtime_error("block hash NBT string is too long");
    }
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void appendNbtString(std::vector<uint8_t>& out, std::string_view value) {
    appendLe16(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void appendLe32(std::vector<uint8_t>& out, int64_t value) {
    const auto raw = static_cast<uint32_t>(static_cast<int32_t>(value));
    out.push_back(static_cast<uint8_t>(raw & 0xffu));
    out.push_back(static_cast<uint8_t>((raw >> 8u) & 0xffu));
    out.push_back(static_cast<uint8_t>((raw >> 16u) & 0xffu));
    out.push_back(static_cast<uint8_t>((raw >> 24u) & 0xffu));
}

int32_t fnv1a32(const std::vector<uint8_t>& bytes) {
    uint32_t hash = 0x811c9dc5u;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash += (hash << 1u) + (hash << 4u) + (hash << 7u) +
            (hash << 8u) + (hash << 24u);
    }
    return std::bit_cast<int32_t>(hash);
}

double miningFatigueMultiplier(uint32_t level) {
    switch (level) {
        case 0: return 1.0;
        case 1: return 0.3;
        case 2: return 0.09;
        case 3: return 0.0027;
        default: return 8.1e-4;
    }
}

} // namespace

BedrockBlockProperty BedrockBlockProperty::byte(int64_t value) {
    BedrockBlockProperty out;
    out.type = BedrockBlockPropertyType::Byte;
    out.integerValue = value;
    return out;
}

BedrockBlockProperty BedrockBlockProperty::integer(int64_t value) {
    BedrockBlockProperty out;
    out.type = BedrockBlockPropertyType::Int;
    out.integerValue = value;
    return out;
}

BedrockBlockProperty BedrockBlockProperty::string(std::string value) {
    BedrockBlockProperty out;
    out.type = BedrockBlockPropertyType::String;
    out.stringValue = std::move(value);
    return out;
}

bool BedrockBlockProperty::isNumeric() const {
    return type != BedrockBlockPropertyType::String;
}

std::optional<int64_t> BedrockBlockProperty::asInteger() const {
    if (!isNumeric()) return std::nullopt;
    return integerValue;
}

std::optional<std::string_view> BedrockBlockProperty::asString() const {
    if (type != BedrockBlockPropertyType::String) return std::nullopt;
    return stringValue;
}

std::string BedrockBlockProperty::toString() const {
    return isNumeric() ? std::to_string(integerValue) : stringValue;
}

bool BedrockBlockProperty::operator==(const BedrockBlockProperty& other) const {
    if (isNumeric() && other.isNumeric()) return integerValue == other.integerValue;
    if (!isNumeric() && !other.isNumeric()) return stringValue == other.stringValue;
    return false;
}

const BedrockBlockProperty* BedrockBlock::property(std::string_view name) const {
    const auto found = properties.find(std::string(name));
    return found == properties.end() ? nullptr : &found->second;
}

BedrockBlockProperties BedrockBlock::propertiesWithComputedStates() const {
    BedrockBlockProperties out = properties;
    for (const auto& [name, value] : computedStates) {
        out.insert_or_assign(name, value);
    }
    return out;
}

std::optional<bool> BedrockBlock::isWaterlogged() const {
    const auto computed = computedStates.find("waterlogged");
    const BedrockBlockProperty* value = computed == computedStates.end()
        ? property("waterlogged")
        : &computed->second;
    if (value == nullptr) return std::nullopt;
    if (value->isNumeric()) return value->integerValue != 0;
    if (value->stringValue == "true" || value->stringValue == "1") return true;
    if (value->stringValue == "false" || value->stringValue == "0") return false;
    return std::nullopt;
}

bool BedrockBlock::isSign() const {
    return name.find("sign") != std::string::npos;
}

const NbtDocument* BedrockBlock::blockEntity() const {
    return entity.has_value() ? &*entity : nullptr;
}

NbtDocument* BedrockBlock::blockEntity() {
    return entity.has_value() ? &*entity : nullptr;
}

std::vector<std::string> BedrockBlock::getSignText() const {
    if (!isSign()) {
        throw std::logic_error("sign text is only available on sign blocks");
    }
    if (!entity.has_value() || entity->root.type != NbtTagType::Compound) {
        return {""};
    }
    const auto* text = entity->root.find("Text");
    if (text == nullptr || text->type != NbtTagType::String) {
        return {""};
    }
    return {text->stringValue};
}

void BedrockBlock::setSignText(std::string text) {
    if (!isSign()) {
        throw std::logic_error("sign text is only available on sign blocks");
    }
    if (!entity.has_value()) {
        entity = NbtDocument {
            "",
            NbtValue::compound({{
                "id",
                NbtValue::string("Sign")
            }})
        };
    }
    if (entity->root.type != NbtTagType::Compound) {
        throw std::logic_error("sign block entity root must be an NBT compound");
    }
    entity->root.set("Text", NbtValue::string(std::move(text)));
}

void BedrockBlock::setSignText(const std::vector<std::string>& text) {
    std::string joined;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i != 0) joined.push_back('\n');
        joined += text[i];
    }
    setSignText(std::move(joined));
}

std::string BedrockBlock::signText() const {
    return getSignText().front();
}

bool BedrockBlock::canHarvest(std::optional<uint32_t> heldItemType) const {
    if (!harvestTools.has_value()) return true;
    return heldItemType.has_value() && harvestTools->contains(*heldItemType);
}

std::vector<BlockShape> BedrockBlock::raycastShapes() const {
    std::vector<BlockShape> out;
    out.reserve(shapes.size());
    for (const auto& shape : shapes) {
        const double halfX = shape[3] / 2.0;
        const double halfY = shape[4] / 2.0;
        const double halfZ = shape[5] / 2.0;
        out.push_back(BlockShape {
            shape[0] - halfX,
            shape[1] - halfY,
            shape[2] - halfZ,
            shape[0] + halfX,
            shape[1] + halfY,
            shape[2] + halfZ
        });
    }
    return out;
}

double BedrockBlock::digTime(const BedrockDigTimeOptions& options) const {
    if (options.creative) return 0.0;

    double speed = 1.0;
    if (options.heldItemType.has_value() && options.toolMultiplier > 0.0) {
        speed = options.toolMultiplier;
    }
    if (options.efficiencyLevel > 0 && speed > 1.0) {
        speed += static_cast<double>(
            options.efficiencyLevel * options.efficiencyLevel + 1
        );
    }

    const auto haste = std::max(options.hasteLevel, options.conduitPowerLevel);
    if (haste > 0) speed *= 1.0 + 0.2 * static_cast<double>(haste);
    if (options.miningFatigueLevel > 0) {
        speed *= miningFatigueMultiplier(options.miningFatigueLevel);
    }
    if (options.inWater && options.aquaAffinityLevel == 0) speed /= 5.0;
    if (options.notOnGround) speed /= 5.0;

    if (hardness == -1.0) return std::numeric_limits<double>::infinity();
    if (hardness == 0.0) return 0.0;

    const double harvestMultiplier = canHarvest(options.heldItemType) ? 30.0 : 100.0;
    const double delta = speed / hardness / harvestMultiplier;
    if (delta == 0.0) return std::numeric_limits<double>::infinity();
    if (delta >= 1.0) return 0.0;
    return std::ceil(1.0 / delta) * 50.0;
}

const BedrockBlockDefinition* BedrockBlockRegistry::blockByType(uint32_t type) const {
    const auto found = blocksByType_.find(type);
    return found == blocksByType_.end() ? nullptr : &found->second;
}

const BedrockBlockDefinition* BedrockBlockRegistry::blockByName(std::string_view name) const {
    const auto found = blockTypesByName_.find(normalizeName(name));
    return found == blockTypesByName_.end() ? nullptr : blockByType(found->second);
}

const BedrockBlockDefinition* BedrockBlockRegistry::blockByStateId(int32_t stateId) const {
    const auto* state = stateById(stateId);
    return state == nullptr ? nullptr : blockByType(state->type);
}

const BedrockBlockStateDefinition* BedrockBlockRegistry::stateById(int32_t stateId) const {
    if (stateId < 0 || static_cast<std::size_t>(stateId) >= statesById_.size()) {
        return nullptr;
    }
    const auto& value = statesById_[static_cast<std::size_t>(stateId)];
    return value.has_value() ? &*value : nullptr;
}

std::vector<BlockShape> BedrockBlockRegistry::raycastShapesForState(int32_t stateId) const {
    const auto* state = stateById(stateId);
    if (state == nullptr) return {};

    std::vector<BlockShape> out;
    out.reserve(state->shapes.size());
    for (const auto& shape : state->shapes) {
        const double halfX = shape[3] / 2.0;
        const double halfY = shape[4] / 2.0;
        const double halfZ = shape[5] / 2.0;
        out.push_back(BlockShape {
            shape[0] - halfX,
            shape[1] - halfY,
            shape[2] - halfZ,
            shape[0] + halfX,
            shape[1] + halfY,
            shape[2] + halfZ
        });
    }
    return out;
}

std::optional<BedrockBlock> BedrockBlockRegistry::fromStateId(
    int32_t stateId,
    int32_t biomeId
) const {
    const auto* state = stateById(stateId);
    const auto* definition = blockByStateId(stateId);
    if (state == nullptr || definition == nullptr) return std::nullopt;

    BedrockBlock out;
    out.type = definition->id;
    out.metadata = stateId - definition->minStateId;
    out.stateId = stateId;
    out.biomeId = biomeId;
    out.name = definition->name;
    out.displayName = definition->displayName;
    for (const auto& variation : definition->variations) {
        if (variation.metadata == out.metadata) {
            out.displayName = variation.displayName;
            break;
        }
    }
    out.hardness = definition->hardness;
    out.resistance = definition->resistance;
    out.stackSize = definition->stackSize;
    out.diggable = definition->diggable;
    out.material = definition->material;
    out.transparent = definition->transparent;
    out.emitLight = definition->emitLight;
    out.filterLight = definition->filterLight;
    out.defaultState = definition->defaultState;
    out.minStateId = definition->minStateId;
    out.maxStateId = definition->maxStateId;
    out.harvestTools = definition->harvestTools;
    out.drops = definition->drops;
    out.boundingBox = definition->boundingBox;
    out.properties = state->properties;
    out.shapes = state->shapes;
    out.missingStateShape = state->missingStateShape;
    out.hash = state->hash;
    return out;
}

std::optional<BedrockBlock> BedrockBlockRegistry::fromProperties(
    std::string_view name,
    const BedrockBlockProperties& properties,
    int32_t biomeId
) const {
    const auto* block = blockByName(name);
    if (block == nullptr) return std::nullopt;
    return fromProperties(block->id, properties, biomeId);
}

std::optional<BedrockBlock> BedrockBlockRegistry::fromProperties(
    uint32_t type,
    const BedrockBlockProperties& properties,
    int32_t biomeId
) const {
    const auto* block = blockByType(type);
    if (block == nullptr) return std::nullopt;

    for (int64_t stateId = block->minStateId; stateId <= block->maxStateId; ++stateId) {
        const auto* state = stateById(static_cast<int32_t>(stateId));
        if (state == nullptr) continue;

        bool matches = true;
        for (const auto& [name, wanted] : properties) {
            const auto found = state->properties.find(name);
            if (found == state->properties.end() || found->second != wanted) {
                matches = false;
                break;
            }
        }
        if (matches) return fromStateId(static_cast<int32_t>(stateId), biomeId);
    }
    return std::nullopt;
}

std::optional<BedrockBlock> BedrockBlockRegistry::fromString(
    std::string_view value,
    int32_t biomeId
) const {
    std::string input = trim(value);
    if (input.rfind("minecraft:", 0) == 0) input.erase(0, 10);

    const auto open = input.find('[');
    const std::string name = trim(input.substr(0, open));
    if (name.empty()) return std::nullopt;
    if (open == std::string::npos) return fromProperties(name, {}, biomeId);
    if (input.empty() || input.back() != ']') return std::nullopt;

    BedrockBlockProperties properties;
    std::string_view body(input.data() + open + 1, input.size() - open - 2);
    std::size_t start = 0;
    bool quoted = false;
    for (std::size_t i = 0; i <= body.size(); ++i) {
        if (i < body.size() && body[i] == '"') quoted = !quoted;
        if (i < body.size() && (body[i] != ',' || quoted)) continue;

        const std::string token = trim(body.substr(start, i - start));
        start = i + 1;
        if (token.empty()) continue;

        std::size_t separator = token.find('=');
        std::string key;
        std::string rawValue;
        if (separator != std::string::npos) {
            key = trim(std::string_view(token).substr(0, separator));
            rawValue = trim(std::string_view(token).substr(separator + 1));
        } else if (!token.empty() && token.front() == '"') {
            const auto quote = token.find('"', 1);
            if (quote == std::string::npos) return std::nullopt;
            const auto colon = token.find(':', quote + 1);
            if (colon == std::string::npos) return std::nullopt;
            key = token.substr(1, quote - 1);
            rawValue = trim(std::string_view(token).substr(colon + 1));
        } else {
            return std::nullopt;
        }

        if (key.size() >= 2 && key.front() == '"' && key.back() == '"') {
            key = key.substr(1, key.size() - 2);
        }
        if (key.empty() || rawValue.empty()) return std::nullopt;
        properties.insert_or_assign(std::move(key), parseStringProperty(std::move(rawValue)));
    }

    return fromProperties(name, properties, biomeId);
}

std::size_t BedrockBlockRegistry::blockCount() const {
    return blocksByType_.size();
}

std::size_t BedrockBlockRegistry::stateCount() const {
    return stateCount_;
}

bool BedrockBlockRegistry::supportsBlockHashes() const {
    return blockHashes_;
}

void BedrockBlockRegistry::loadRuntimeIds(bool hashed) {
    stateIdsByRuntimeId_.clear();
    stateIdsByRuntimeId_.reserve(stateCount_);
    hashedRuntimeIds_ = hashed;
    for (std::size_t stateId = 0; stateId < statesById_.size(); ++stateId) {
        const auto& state = statesById_[stateId];
        if (!state.has_value()) continue;
        const auto runtimeId = hashed
            ? computeRuntimeHash(state->name, state->properties)
            : static_cast<int32_t>(stateId);
        stateIdsByRuntimeId_.insert_or_assign(
            runtimeId,
            static_cast<int32_t>(stateId)
        );
    }
}

std::optional<int32_t> BedrockBlockRegistry::stateIdForRuntimeId(
    int32_t runtimeId
) const {
    const auto found = stateIdsByRuntimeId_.find(runtimeId);
    if (found == stateIdsByRuntimeId_.end()) return std::nullopt;
    return found->second;
}

const BedrockBlockStateDefinition* BedrockBlockRegistry::stateByRuntimeId(
    int32_t runtimeId
) const {
    const auto stateId = stateIdForRuntimeId(runtimeId);
    return stateId.has_value() ? stateById(*stateId) : nullptr;
}

const BedrockBlockDefinition* BedrockBlockRegistry::blockByRuntimeId(
    int32_t runtimeId
) const {
    const auto stateId = stateIdForRuntimeId(runtimeId);
    return stateId.has_value() ? blockByStateId(*stateId) : nullptr;
}

std::size_t BedrockBlockRegistry::runtimeIdCount() const {
    return stateIdsByRuntimeId_.size();
}

bool BedrockBlockRegistry::usesHashedRuntimeIds() const {
    return hashedRuntimeIds_;
}

BlockRuntimeRegistry BedrockBlockRegistry::toRuntimeRegistry() const {
    BlockRuntimeRegistry out;
    for (std::size_t stateId = 0; stateId < statesById_.size(); ++stateId) {
        if (statesById_[stateId].has_value()) {
            out.add(static_cast<uint32_t>(stateId), statesById_[stateId]->name);
        }
    }
    // Keep name -> runtime ID deterministic and useful for callers: the JS
    // registry's canonical ID for a block name is its default state.
    for (const auto& [type, block] : blocksByType_) {
        (void) type;
        if (block.defaultState >= 0) {
            out.add(static_cast<uint32_t>(block.defaultState), block.name);
        }
    }
    return out;
}

int32_t BedrockBlockRegistry::computeHash(
    std::string_view name,
    const BedrockBlockProperties& properties
) {
    const std::string namespaced = name.find(':') == std::string_view::npos
        ? "minecraft:" + std::string(name)
        : std::string(name);

    std::vector<uint8_t> nbt;
    nbt.reserve(32 + namespaced.size() + properties.size() * 16);
    nbt.push_back(10); // unnamed root TAG_Compound
    appendLe16(nbt, 0);
    nbt.push_back(8); // TAG_String name
    appendNbtString(nbt, "name");
    appendNbtString(nbt, namespaced);
    nbt.push_back(10); // TAG_Compound states
    appendNbtString(nbt, "states");
    for (const auto& [propertyName, value] : properties) {
        (void) value;
        // prismarine-nbt receives primitive values inside nbt.comp here and
        // historically writes each one as a named TAG_End.
        nbt.push_back(0);
        appendNbtString(nbt, propertyName);
    }
    nbt.push_back(0); // end states
    nbt.push_back(0); // end root

    return fnv1a32(nbt);
}

int32_t BedrockBlockRegistry::computeRuntimeHash(
    std::string_view name,
    const BedrockBlockProperties& properties
) {
    const std::string namespaced = name.find(':') == std::string_view::npos
        ? "minecraft:" + std::string(name)
        : std::string(name);

    std::vector<uint8_t> nbt;
    nbt.reserve(32 + namespaced.size() + properties.size() * 24);
    nbt.push_back(10); // unnamed root TAG_Compound
    appendLe16(nbt, 0);
    nbt.push_back(8); // TAG_String name
    appendNbtString(nbt, "name");
    appendNbtString(nbt, namespaced);
    nbt.push_back(10); // TAG_Compound states
    appendNbtString(nbt, "states");
    for (const auto& [propertyName, value] : properties) {
        switch (value.type) {
            case BedrockBlockPropertyType::Byte:
                nbt.push_back(1);
                appendNbtString(nbt, propertyName);
                nbt.push_back(static_cast<uint8_t>(value.integerValue));
                break;
            case BedrockBlockPropertyType::Int:
                nbt.push_back(3);
                appendNbtString(nbt, propertyName);
                appendLe32(nbt, value.integerValue);
                break;
            case BedrockBlockPropertyType::String:
                nbt.push_back(8);
                appendNbtString(nbt, propertyName);
                appendNbtString(nbt, value.stringValue);
                break;
        }
    }
    nbt.push_back(0); // end states
    nbt.push_back(0); // end root
    return fnv1a32(nbt);
}

std::string BedrockBlockRegistry::normalizeName(std::string_view name) {
    if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
    return std::string(name);
}

BedrockBlockRegistry BedrockBlockRegistryLoader::loadMinecraftData(
    const std::filesystem::path& blocksJson,
    const std::filesystem::path& blockStatesJson,
    const std::filesystem::path& blockCollisionShapesJson,
    bool blockHashes
) {
    const auto blocksRoot = readJsonFile(blocksJson);
    requireKind(blocksRoot, JsonValue::Kind::Array, "blocks.json root");

    std::optional<JsonValue> statesRoot;
    if (!blockStatesJson.empty() && std::filesystem::exists(blockStatesJson)) {
        statesRoot = readJsonFile(blockStatesJson);
        requireKind(*statesRoot, JsonValue::Kind::Array, "blockStates.json root");
    }

    CollisionShapeData collision;
    if (!blockCollisionShapesJson.empty() &&
        std::filesystem::exists(blockCollisionShapesJson)) {
        collision = parseCollisionShapes(readJsonFile(blockCollisionShapesJson));
    }

    BedrockBlockRegistry registry;
    registry.blockHashes_ = blockHashes;

    for (const auto& value : blocksRoot.arrayValue) {
        BedrockBlockDefinition block;
        block.id = checkedUInt32(
            integerValue(requireField(value, "id", "block"), "block.id"),
            "block.id"
        );
        block.name = BedrockBlockRegistry::normalizeName(
            stringValue(requireField(value, "name", "block"), "block.name")
        );
        block.displayName = stringValue(
            requireField(value, "displayName", "block"),
            "block.displayName"
        );
        const auto& hardness = requireField(value, "hardness", "block");
        // Legacy Bedrock data uses null for unbreakable/special blocks. In
        // prismarine-block's numeric dig formula it behaves like zero.
        block.hardness = hardness.kind == JsonValue::Kind::Null
            ? 0.0
            : numberValue(hardness, "block.hardness");
        if (const auto* resistance = optionalField(value, "resistance")) {
            block.resistance = numberValue(*resistance, "block.resistance");
        }
        block.stackSize = checkedUInt32(
            integerValue(requireField(value, "stackSize", "block"), "block.stackSize"),
            "block.stackSize"
        );
        block.diggable = boolValue(requireField(value, "diggable", "block"), "block.diggable");
        if (const auto* material = optionalField(value, "material")) {
            block.material = stringValue(*material, "block.material");
        }
        block.transparent = boolValue(
            requireField(value, "transparent", "block"),
            "block.transparent"
        );
        block.emitLight = static_cast<uint8_t>(checkedUInt32(
            integerValue(requireField(value, "emitLight", "block"), "block.emitLight"),
            "block.emitLight"
        ));
        block.filterLight = static_cast<uint8_t>(checkedUInt32(
            integerValue(requireField(value, "filterLight", "block"), "block.filterLight"),
            "block.filterLight"
        ));
        const auto* defaultState = optionalField(value, "defaultState");
        const auto* minStateId = optionalField(value, "minStateId");
        const auto* maxStateId = optionalField(value, "maxStateId");
        if (defaultState != nullptr && minStateId != nullptr && maxStateId != nullptr) {
            block.defaultState = checkedInt32(
                integerValue(*defaultState, "block.defaultState"),
                "block.defaultState"
            );
            block.minStateId = checkedInt32(
                integerValue(*minStateId, "block.minStateId"),
                "block.minStateId"
            );
            block.maxStateId = checkedInt32(
                integerValue(*maxStateId, "block.maxStateId"),
                "block.maxStateId"
            );
        } else {
            // minecraft-data's legacy Bedrock index assigns every numeric
            // block id a 16-value metadata range when blockStates.json did
            // not exist yet.
            block.minStateId = checkedInt32(
                static_cast<int64_t>(block.id) << 4,
                "legacy block.minStateId"
            );
            block.maxStateId = block.minStateId + 15;
            block.defaultState = block.minStateId;
        }
        block.boundingBox = stringValue(
            requireField(value, "boundingBox", "block"),
            "block.boundingBox"
        );

        if (const auto* tools = optionalField(value, "harvestTools")) {
            requireKind(*tools, JsonValue::Kind::Object, "block.harvestTools");
            block.harvestTools.emplace();
            for (const auto& [toolText, enabled] : tools->objectValue) {
                if (enabled.kind == JsonValue::Kind::Bool && !enabled.boolValue) continue;
                uint32_t tool = 0;
                const auto parsed = std::from_chars(
                    toolText.data(),
                    toolText.data() + toolText.size(),
                    tool
                );
                if (parsed.ec != std::errc() || parsed.ptr != toolText.data() + toolText.size()) {
                    throw std::runtime_error("invalid block harvest tool id: " + toolText);
                }
                block.harvestTools->insert(tool);
            }
        }

        const auto& drops = requireField(value, "drops", "block");
        requireKind(drops, JsonValue::Kind::Array, "block.drops");
        block.drops.reserve(drops.arrayValue.size());
        for (const auto& drop : drops.arrayValue) {
            const JsonValue* dropId = &drop;
            if (drop.kind == JsonValue::Kind::Object) {
                dropId = &requireField(drop, "drop", "legacy block drop");
                if (dropId->kind == JsonValue::Kind::Object) {
                    dropId = &requireField(*dropId, "id", "legacy block drop item");
                }
            }
            block.drops.push_back(checkedUInt32(
                integerValue(*dropId, "block drop"),
                "block drop"
            ));
        }

        if (const auto* variations = optionalField(value, "variations")) {
            requireKind(*variations, JsonValue::Kind::Array, "block.variations");
            block.variations.reserve(variations->arrayValue.size());
            for (const auto& variation : variations->arrayValue) {
                block.variations.push_back(BedrockBlockVariation {
                    checkedInt32(
                        integerValue(
                            requireField(variation, "metadata", "block variation"),
                            "block variation.metadata"
                        ),
                        "block variation.metadata"
                    ),
                    stringValue(
                        requireField(variation, "displayName", "block variation"),
                        "block variation.displayName"
                    )
                });
            }
        }

        const auto id = block.id;
        const auto name = block.name;
        if (!registry.blocksByType_.emplace(id, std::move(block)).second) {
            throw std::runtime_error("duplicate Bedrock block type id: " + std::to_string(id));
        }
        // minecraft-data's name index is last-write-wins; legacy Bedrock has
        // duplicate names such as flowing/still water under distinct ids.
        registry.blockTypesByName_.insert_or_assign(name, id);
    }

    if (!statesRoot.has_value()) {
        int32_t highestStateId = -1;
        for (const auto& [_, block] : registry.blocksByType_) {
            highestStateId = std::max(highestStateId, block.maxStateId);
        }
        registry.statesById_.resize(static_cast<std::size_t>(highestStateId) + 1u);
        for (const auto& [_, block] : registry.blocksByType_) {
            for (int32_t stateId = block.minStateId; stateId <= block.maxStateId; ++stateId) {
                BedrockBlockStateDefinition state;
                state.stateId = stateId;
                state.type = block.id;
                state.name = block.name;
                registry.statesById_[static_cast<std::size_t>(stateId)] = std::move(state);
                ++registry.stateCount_;
            }
        }
        return registry;
    }

    registry.statesById_.resize(statesRoot->arrayValue.size());
    for (std::size_t stateId = 0; stateId < statesRoot->arrayValue.size(); ++stateId) {
        const auto& value = statesRoot->arrayValue[stateId];
        BedrockBlockStateDefinition state;
        state.stateId = checkedInt32(static_cast<int64_t>(stateId), "block state id");
        state.name = BedrockBlockRegistry::normalizeName(
            stringValue(requireField(value, "name", "block state"), "block state.name")
        );
        state.version = checkedInt32(
            integerValue(requireField(value, "version", "block state"), "block state.version"),
            "block state.version"
        );

        const auto& properties = requireField(value, "states", "block state");
        requireKind(properties, JsonValue::Kind::Object, "block state.states");
        for (const auto& [name, descriptor] : properties.objectValue) {
            const auto type = stringValue(
                requireField(descriptor, "type", "block state property"),
                "block state property.type"
            );
            const auto& propertyValue = requireField(
                descriptor,
                "value",
                "block state property"
            );
            if (type == "byte") {
                state.properties.emplace(name, BedrockBlockProperty::byte(
                    integerValue(propertyValue, "block state byte property")
                ));
            } else if (type == "int") {
                state.properties.emplace(name, BedrockBlockProperty::integer(
                    integerValue(propertyValue, "block state int property")
                ));
            } else if (type == "string") {
                state.properties.emplace(name, BedrockBlockProperty::string(
                    stringValue(propertyValue, "block state string property")
                ));
            } else {
                throw std::runtime_error("unsupported Bedrock block property type: " + type);
            }
        }

        const auto* block = registry.blockByName(state.name);
        if (block == nullptr) {
            throw std::runtime_error(
                "block state references unknown Bedrock block: " + state.name
            );
        }
        state.type = block->id;
        auto [shapes, missing] = shapesForState(collision, *block, state.stateId);
        state.shapes = std::move(shapes);
        state.missingStateShape = missing;
        if (blockHashes) {
            state.hash = BedrockBlockRegistry::computeHash(state.name, state.properties);
        }

        registry.statesById_[stateId] = std::move(state);
        ++registry.stateCount_;
    }

    return registry;
}

} // namespace bedrock
