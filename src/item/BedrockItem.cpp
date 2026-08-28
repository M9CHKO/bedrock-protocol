#include <bedrock/item/BedrockItem.hpp>

#include <bedrock/protodef/ProtoDefJson.hpp>
#include <bedrock/protodef/ProtoDefNbt.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace bedrock {

struct BedrockItemRegistryData {
    int32_t protocolVersion = -1;
    bool usesAuxValue = false;
    std::unordered_map<int32_t, BedrockItemDefinition> itemsById;
    std::unordered_map<std::string, std::size_t> itemIndicesByName;
    std::vector<BedrockItemDefinition> itemsArray;
    std::vector<BedrockItemState> itemStates;
    std::unordered_map<int16_t, BedrockEnchantmentDefinition> enchantmentsById;
    std::unordered_map<std::string, int16_t> enchantmentIdsByName;
    mutable std::atomic<int32_t> nextStackId {0};
};

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

int32_t checkedInt32(int64_t value, std::string_view context) {
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) + " is out of int32 range"
        );
    }
    return static_cast<int32_t>(value);
}

int16_t checkedInt16(int64_t value, std::string_view context) {
    if (value < std::numeric_limits<int16_t>::min() ||
        value > std::numeric_limits<int16_t>::max()) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) + " is out of int16 range"
        );
    }
    return static_cast<int16_t>(value);
}

uint32_t checkedUInt32(int64_t value, std::string_view context) {
    if (value < 0 || static_cast<uint64_t>(value) > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) + " is out of uint32 range"
        );
    }
    return static_cast<uint32_t>(value);
}

std::string stringValue(const JsonValue& value, std::string_view context) {
    requireKind(value, JsonValue::Kind::String, context);
    return value.stringValue;
}

bool boolValue(const JsonValue& value, std::string_view context) {
    requireKind(value, JsonValue::Kind::Bool, context);
    return value.boolValue;
}

std::vector<std::string> stringArray(
    const JsonValue* value,
    std::string_view context
) {
    if (value == nullptr) return {};
    requireKind(*value, JsonValue::Kind::Array, context);
    std::vector<std::string> out;
    out.reserve(value->arrayValue.size());
    for (const auto& item : value->arrayValue) {
        out.push_back(stringValue(item, context));
    }
    return out;
}

bool protoBool(const ProtoDefValue& value) {
    switch (value.kind) {
        case ProtoDefValue::Kind::Bool: return value.boolValue;
        case ProtoDefValue::Kind::Int: return value.intValue != 0;
        case ProtoDefValue::Kind::UInt: return value.uintValue != 0;
        case ProtoDefValue::Kind::String:
            return value.stringValue == "true" || value.stringValue == "1" ||
                value.stringValue == "65535";
        default: return false;
    }
}

int64_t protoInteger(const ProtoDefValue& value, std::string_view context) {
    switch (value.kind) {
        case ProtoDefValue::Kind::Int: return value.intValue;
        case ProtoDefValue::Kind::UInt:
            if (value.uintValue <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return static_cast<int64_t>(value.uintValue);
            }
            break;
        case ProtoDefValue::Kind::Bool: return value.boolValue ? 1 : 0;
        case ProtoDefValue::Kind::String: {
            int64_t out = 0;
            const auto parsed = std::from_chars(
                value.stringValue.data(),
                value.stringValue.data() + value.stringValue.size(),
                out
            );
            if (parsed.ec == std::errc() &&
                parsed.ptr == value.stringValue.data() + value.stringValue.size()) {
                return out;
            }
            break;
        }
        default: break;
    }
    throw std::runtime_error(std::string(context) + " must be an integer");
}

const ProtoDefValue* protoField(const ProtoDefValue& object, std::string_view name) {
    if (object.kind != ProtoDefValue::Kind::Object) return nullptr;
    return object.get(std::string(name));
}

std::vector<std::string> protoStringArray(const ProtoDefValue* value) {
    if (value == nullptr || value->kind != ProtoDefValue::Kind::Array) return {};
    std::vector<std::string> out;
    out.reserve(value->arrayValue.size());
    for (const auto& item : value->arrayValue) {
        if (item.kind == ProtoDefValue::Kind::String) {
            out.push_back(item.stringValue);
        } else if (item.kind == ProtoDefValue::Kind::Array &&
            !item.arrayValue.empty() &&
            item.arrayValue.front().kind == ProtoDefValue::Kind::String) {
            // prismarine-item exposes these packet strings as one-element
            // arrays. Accept that public JS shape as well as decoded strings.
            out.push_back(item.arrayValue.front().stringValue);
        }
    }
    return out;
}

ProtoDefValue protoStringArray(const std::vector<std::string>& values) {
    std::vector<ProtoDefValue> out;
    out.reserve(values.size());
    for (const auto& value : values) out.push_back(ProtoDefValue::string(value));
    return ProtoDefValue::array(std::move(out));
}

bool isIntegerTag(const NbtValue& value) {
    return value.type == NbtTagType::Byte || value.type == NbtTagType::Short ||
        value.type == NbtTagType::Int || value.type == NbtTagType::Long;
}

NbtValue& ensureCompoundChild(NbtValue& parent, std::string_view name) {
    if (parent.type != NbtTagType::Compound) {
        throw BedrockNbtError("item NBT parent must be a compound");
    }
    auto* child = parent.find(name);
    if (child == nullptr) {
        parent.set(std::string(name), NbtValue::compound());
        child = parent.find(name);
    }
    if (child->type != NbtTagType::Compound) {
        throw BedrockNbtError("item NBT child must be a compound: " + std::string(name));
    }
    return *child;
}

bool eraseNbtChild(NbtValue& parent, std::string_view name) {
    if (parent.type != NbtTagType::Compound) return false;
    const auto oldSize = parent.compoundValue.size();
    std::erase_if(parent.compoundValue, [&](const NbtNamedValue& value) {
        return value.name == name;
    });
    return parent.compoundValue.size() != oldSize;
}

std::vector<std::string> nbtStringList(const NbtValue* value) {
    if (value == nullptr) return {};
    if (value->type == NbtTagType::String) return {value->stringValue};
    if (value->type != NbtTagType::List || value->listElementType != NbtTagType::String) {
        return {};
    }
    std::vector<std::string> out;
    out.reserve(value->listValue.size());
    for (const auto& item : value->listValue) {
        if (item.type == NbtTagType::String) out.push_back(item.stringValue);
    }
    return out;
}

std::string namespacedBlock(std::string value) {
    if (value.find(':') == std::string::npos) return "minecraft:" + value;
    return value;
}

void setNbtStringList(
    NbtValue& root,
    std::string_view name,
    const std::vector<std::string>& values
) {
    if (values.empty()) {
        eraseNbtChild(root, name);
        return;
    }
    std::vector<NbtValue> list;
    list.reserve(values.size());
    for (const auto& value : values) list.push_back(NbtValue::string(namespacedBlock(value)));
    root.set(std::string(name), NbtValue::list(NbtTagType::String, std::move(list)));
}

std::optional<NbtDocument> protoNbt(
    const ProtoDefValue* wrapper,
    BedrockNbtEncoding encoding
) {
    if (wrapper == nullptr || wrapper->kind != ProtoDefValue::Kind::Object) {
        return std::nullopt;
    }
    const auto* value = wrapper->get("nbt");
    if (value == nullptr || value->kind == ProtoDefValue::Kind::Null) return std::nullopt;
    if (value->kind == ProtoDefValue::Kind::Bytes) {
        BinaryStream stream(value->bytesValue);
        auto document = BedrockNbtCodec::read(stream, encoding);
        if (!stream.eof()) throw BedrockNbtError("item NBT contains trailing bytes");
        return document;
    }
    return protoDefValueToNbtDocument(*value);
}

ProtoDefValue protoNbtWrapper(
    const NbtDocument& document,
    BedrockNbtEncoding encoding
) {
    BinaryStream stream;
    BedrockNbtCodec::write(stream, document, encoding);
    return ProtoDefValue::object({
        {"version", ProtoDefValue::uinteger(1)},
        {"nbt", ProtoDefValue::bytes(stream.buffer())}
    });
}

BedrockEnchantmentCost parseCost(const JsonValue& value, std::string_view context) {
    BedrockEnchantmentCost out;
    out.a = checkedInt32(integerValue(requireField(value, "a", context), context), context);
    out.b = checkedInt32(integerValue(requireField(value, "b", context), context), context);
    return out;
}

} // namespace

int32_t BedrockEnchantmentCost::atLevel(int32_t level) const {
    return a * level + b;
}

bool BedrockItem::hasNbtPayload() const {
    if (!nbt.has_value() || nbt->root.type == NbtTagType::End) return false;
    if (nbt->root.type == NbtTagType::Compound) return !nbt->root.compoundValue.empty();
    return true;
}

bool BedrockItem::equals(
    const BedrockItem& other,
    bool matchStackSize,
    bool matchNbt
) const {
    return type == other.type && metadata == other.metadata &&
        (!matchStackSize || count == other.count) &&
        (!matchNbt || nbt == other.nbt);
}

NbtValue& BedrockItem::ensureNbtRoot() {
    if (!nbt.has_value()) nbt = NbtDocument {{}, NbtValue::compound()};
    if (nbt->root.type != NbtTagType::Compound) {
        throw BedrockNbtError("item NBT root must be a compound");
    }
    return nbt->root;
}

std::optional<std::string> BedrockItem::customName() const {
    if (!nbt.has_value()) return std::nullopt;
    const auto* display = nbt->root.find("display");
    const auto* value = display == nullptr ? nullptr : display->find("Name");
    if (value == nullptr || value->type != NbtTagType::String) return std::nullopt;
    return value->stringValue;
}

void BedrockItem::setCustomName(std::string value) {
    auto& display = ensureCompoundChild(ensureNbtRoot(), "display");
    display.set("Name", NbtValue::string(std::move(value)));
}

void BedrockItem::clearCustomName() {
    if (!nbt.has_value()) return;
    auto* display = nbt->root.find("display");
    if (display == nullptr) return;
    eraseNbtChild(*display, "Name");
    if (display->compoundValue.empty()) eraseNbtChild(nbt->root, "display");
}

std::optional<std::vector<std::string>> BedrockItem::customLore() const {
    if (!nbt.has_value()) return std::nullopt;
    const auto* display = nbt->root.find("display");
    const auto* lore = display == nullptr ? nullptr : display->find("Lore");
    if (lore == nullptr) return std::nullopt;
    return nbtStringList(lore);
}

void BedrockItem::setCustomLore(std::vector<std::string> value) {
    auto& display = ensureCompoundChild(ensureNbtRoot(), "display");
    std::vector<NbtValue> lore;
    lore.reserve(value.size());
    for (auto& line : value) lore.push_back(NbtValue::string(std::move(line)));
    display.set("Lore", NbtValue::list(NbtTagType::String, std::move(lore)));
}

void BedrockItem::setCustomLore(std::string value) {
    setCustomLore(std::vector<std::string> {std::move(value)});
}

void BedrockItem::clearCustomLore() {
    if (!nbt.has_value()) return;
    auto* display = nbt->root.find("display");
    if (display == nullptr) return;
    eraseNbtChild(*display, "Lore");
    if (display->compoundValue.empty()) eraseNbtChild(nbt->root, "display");
}

int32_t BedrockItem::repairCost() const {
    if (!nbt.has_value()) return 0;
    const auto* value = nbt->root.find("RepairCost");
    return value != nullptr && isIntegerTag(*value)
        ? static_cast<int32_t>(value->integerValue)
        : 0;
}

void BedrockItem::setRepairCost(int32_t value) {
    ensureNbtRoot().set("RepairCost", NbtValue::integer(value));
}

std::optional<int32_t> BedrockItem::durabilityUsed() const {
    if (nbt.has_value()) {
        const auto* value = nbt->root.find("Damage");
        if (value != nullptr && isIntegerTag(*value)) {
            return static_cast<int32_t>(value->integerValue);
        }
    }
    return maxDurability.has_value() ? std::optional<int32_t>(0) : std::nullopt;
}

void BedrockItem::setDurabilityUsed(int32_t value) {
    ensureNbtRoot().set("Damage", NbtValue::integer(value));
}

std::vector<BedrockItemEnchantment> BedrockItem::enchantments() const {
    std::vector<BedrockItemEnchantment> out;
    if (!nbt.has_value()) return out;
    const auto* list = nbt->root.find("ench");
    if (list == nullptr || list->type != NbtTagType::List ||
        list->listElementType != NbtTagType::Compound) {
        return out;
    }

    out.reserve(list->listValue.size());
    for (const auto& value : list->listValue) {
        if (value.type != NbtTagType::Compound) continue;
        const auto* id = value.find("id");
        const auto* level = value.find("lvl");
        if (id == nullptr || level == nullptr || !isIntegerTag(*id) ||
            !isIntegerTag(*level)) {
            continue;
        }
        BedrockItemEnchantment enchantment;
        enchantment.id = static_cast<int16_t>(id->integerValue);
        enchantment.level = static_cast<int16_t>(level->integerValue);
        if (registry_) {
            const auto found = registry_->enchantmentsById.find(enchantment.id);
            if (found != registry_->enchantmentsById.end()) {
                enchantment.name = found->second.name;
            }
        }
        out.push_back(std::move(enchantment));
    }
    return out;
}

void BedrockItem::setEnchantments(const std::vector<BedrockNamedEnchantment>& value) {
    if (value.empty()) {
        clearEnchantments();
        return;
    }
    if (!registry_) throw std::runtime_error("item has no Bedrock registry");

    std::vector<NbtValue> list;
    list.reserve(value.size());
    for (const auto& enchantment : value) {
        std::string name = enchantment.name;
        if (name.rfind("minecraft:", 0) == 0) name.erase(0, 10);
        const auto foundId = registry_->enchantmentIdsByName.find(name);
        if (foundId == registry_->enchantmentIdsByName.end()) {
            throw std::runtime_error("unknown Bedrock enchantment: " + enchantment.name);
        }
        list.push_back(NbtValue::compound({
            {"id", NbtValue::shortInteger(foundId->second)},
            {"lvl", NbtValue::shortInteger(enchantment.level)}
        }));
    }
    ensureNbtRoot().set("ench", NbtValue::list(NbtTagType::Compound, std::move(list)));
}

void BedrockItem::clearEnchantments() {
    if (nbt.has_value()) eraseNbtChild(nbt->root, "ench");
}

std::vector<std::string> BedrockItem::blocksCanPlaceOn() const {
    return nbt.has_value() ? nbtStringList(nbt->root.find("CanPlaceOn"))
                           : std::vector<std::string> {};
}

void BedrockItem::setBlocksCanPlaceOn(const std::vector<std::string>& value) {
    if (value.empty() && !nbt.has_value()) return;
    setNbtStringList(ensureNbtRoot(), "CanPlaceOn", value);
}

std::vector<std::string> BedrockItem::blocksCanDestroy() const {
    return nbt.has_value() ? nbtStringList(nbt->root.find("CanDestroy"))
                           : std::vector<std::string> {};
}

void BedrockItem::setBlocksCanDestroy(const std::vector<std::string>& value) {
    if (value.empty() && !nbt.has_value()) return;
    setNbtStringList(ensureNbtRoot(), "CanDestroy", value);
}

std::string BedrockItem::spawnEggMobName() const {
    constexpr std::string_view suffix = "_spawn_egg";
    const auto position = name.find(suffix);
    if (position == std::string::npos) return name;
    std::string out = name;
    out.erase(position, suffix.size());
    return out;
}

BedrockItemRegistry::BedrockItemRegistry()
    : data_(std::make_shared<BedrockItemRegistryData>()) {}

const BedrockItemDefinition* BedrockItemRegistry::itemById(int32_t id) const {
    const auto found = data_->itemsById.find(id);
    return found == data_->itemsById.end() ? nullptr : &found->second;
}

const BedrockItemDefinition* BedrockItemRegistry::itemByName(std::string_view name) const {
    const auto found = data_->itemIndicesByName.find(normalizeName(name));
    return found == data_->itemIndicesByName.end()
        ? nullptr
        : &data_->itemsArray[found->second];
}

const BedrockEnchantmentDefinition* BedrockItemRegistry::enchantmentById(int16_t id) const {
    const auto found = data_->enchantmentsById.find(id);
    return found == data_->enchantmentsById.end() ? nullptr : &found->second;
}

const BedrockEnchantmentDefinition* BedrockItemRegistry::enchantmentByName(
    std::string_view name
) const {
    const auto found = data_->enchantmentIdsByName.find(normalizeName(name));
    return found == data_->enchantmentIdsByName.end()
        ? nullptr
        : enchantmentById(found->second);
}

BedrockItem BedrockItemRegistry::create(
    int32_t type,
    uint16_t count,
    int32_t metadata,
    std::optional<NbtDocument> nbt,
    std::optional<int32_t> stackId,
    bool sentByServer
) const {
    BedrockItem out;
    out.registry_ = data_;
    out.type = type;
    out.count = count;
    out.metadata = metadata;
    out.nbt = std::move(nbt);
    if (!stackId.has_value() && !sentByServer) stackId = nextStackId();
    out.stackId = stackId;

    const auto* definition = itemById(type);
    if (definition == nullptr) {
        out.name = "unknown";
        out.displayName = "unknown";
        out.stackSize = 1;
        return out;
    }

    out.name = definition->name;
    out.displayName = definition->displayName;
    out.stackSize = definition->stackSize;
    out.maxDurability = definition->maxDurability;
    for (const auto& variation : definition->variations) {
        if (variation.metadata == metadata) {
            out.displayName = variation.displayName;
            break;
        }
    }

    if (!sentByServer && out.maxDurability.has_value() &&
        out.durabilityUsed().value_or(0) == 0) {
        out.setDurabilityUsed(0);
    }
    return out;
}

std::optional<BedrockItem> BedrockItemRegistry::fromNetwork(
    const BedrockNetworkItem& value,
    std::optional<int32_t> legacyStackId
) const {
    if (value.networkId == 0) return std::nullopt;
    auto item = create(
        value.networkId,
        value.count,
        value.metadata,
        value.nbt,
        data_->usesAuxValue ? legacyStackId : value.stackId,
        true
    );
    if (!value.canPlaceOn.empty()) item.setBlocksCanPlaceOn(value.canPlaceOn);
    if (!value.canDestroy.empty()) item.setBlocksCanDestroy(value.canDestroy);
    return item;
}

BedrockNetworkItem BedrockItemRegistry::toNetwork(
    const BedrockItem* item,
    bool serverAuthoritative
) const {
    BedrockNetworkItem out;
    if (item == nullptr || item->type == 0) return out;
    out.networkId = item->type;
    out.count = item->count;
    out.metadata = item->metadata;
    if (!data_->usesAuxValue && serverAuthoritative) out.stackId = item->stackId;
    out.blockRuntimeId = 0;
    if (item->hasNbtPayload()) out.nbt = item->nbt;
    out.canPlaceOn = item->blocksCanPlaceOn();
    out.canDestroy = item->blocksCanDestroy();
    out.blockingTick = 0;
    return out;
}

std::optional<BedrockItem> BedrockItemRegistry::fromProtoDefValue(
    const ProtoDefValue& value,
    std::optional<int32_t> legacyStackId
) const {
    if (value.kind != ProtoDefValue::Kind::Object) {
        throw std::runtime_error("Bedrock Item packet value must be an object");
    }
    const auto* networkId = value.get("network_id");
    if (networkId == nullptr) throw std::runtime_error("Bedrock Item is missing network_id");

    BedrockNetworkItem network;
    network.networkId = checkedInt32(protoInteger(*networkId, "network_id"), "network_id");
    if (network.networkId == 0) return std::nullopt;

    if (data_->usesAuxValue) {
        const auto* auxiliary = value.get("auxiliary_value");
        if (auxiliary == nullptr) throw std::runtime_error("Bedrock Item is missing auxiliary_value");
        const auto packed = checkedInt32(protoInteger(*auxiliary, "auxiliary_value"), "auxiliary_value");
        network.count = static_cast<uint8_t>(packed & 0xff);
        network.metadata = packed >> 8;
        network.nbt = protoNbt(value.get("nbt"), BedrockNbtEncoding::LittleVarInt);
        network.canPlaceOn = protoStringArray(value.get("can_place_on"));
        network.canDestroy = protoStringArray(value.get("can_destroy"));
        if (const auto* blocking = value.get("blocking_tick")) {
            network.blockingTick = protoInteger(*blocking, "blocking_tick");
        }
    } else {
        const auto* count = value.get("count");
        const auto* metadata = value.get("metadata");
        if (count == nullptr || metadata == nullptr) {
            throw std::runtime_error("Bedrock Item is missing count or metadata");
        }
        const auto countValue = protoInteger(*count, "count");
        if (countValue < 0 || countValue > std::numeric_limits<uint16_t>::max()) {
            throw std::runtime_error("Bedrock Item count is out of range");
        }
        network.count = static_cast<uint16_t>(countValue);
        network.metadata = checkedInt32(protoInteger(*metadata, "metadata"), "metadata");
        if (const auto* hasStackId = value.get("has_stack_id");
            hasStackId != nullptr && protoBool(*hasStackId)) {
            if (const auto* stackId = value.get("stack_id")) {
                network.stackId = checkedInt32(protoInteger(*stackId, "stack_id"), "stack_id");
            }
        }
        if (const auto* blockRuntimeId = value.get("block_runtime_id")) {
            network.blockRuntimeId = checkedInt32(
                protoInteger(*blockRuntimeId, "block_runtime_id"),
                "block_runtime_id"
            );
        }
        if (const auto* extra = value.get("extra"); extra != nullptr) {
            network.nbt = protoNbt(
                protoField(*extra, "nbt"),
                BedrockNbtEncoding::LittleEndian
            );
            network.canPlaceOn = protoStringArray(protoField(*extra, "can_place_on"));
            network.canDestroy = protoStringArray(protoField(*extra, "can_destroy"));
            if (const auto* blocking = protoField(*extra, "blocking_tick")) {
                network.blockingTick = protoInteger(*blocking, "blocking_tick");
            }
        }
    }
    return fromNetwork(network, legacyStackId);
}

ProtoDefValue BedrockItemRegistry::toProtoDefValue(
    const BedrockItem* item,
    bool serverAuthoritative
) const {
    const auto network = toNetwork(item, serverAuthoritative);
    if (network.networkId == 0) {
        return ProtoDefValue::object({
            {"network_id", ProtoDefValue::integer(0)}
        });
    }

    const bool hasNbt = network.nbt.has_value();
    if (data_->usesAuxValue) {
        std::unordered_map<std::string, ProtoDefValue> fields {
            {"network_id", ProtoDefValue::integer(network.networkId)},
            {"auxiliary_value", ProtoDefValue::integer(
                network.metadata * 256 + (network.count & 0xff)
            )},
            {"has_nbt", ProtoDefValue::string(hasNbt ? "true" : "false")},
            {"can_place_on", protoStringArray(network.canPlaceOn)},
            {"can_destroy", protoStringArray(network.canDestroy)},
            {"blocking_tick", ProtoDefValue::integer(network.blockingTick)}
        };
        if (hasNbt) {
            fields["nbt"] = protoNbtWrapper(
                *network.nbt,
                BedrockNbtEncoding::LittleVarInt
            );
        }
        return ProtoDefValue::object(std::move(fields));
    }

    std::unordered_map<std::string, ProtoDefValue> extra {
        {"has_nbt", ProtoDefValue::string(hasNbt ? "true" : "false")},
        {"can_place_on", protoStringArray(network.canPlaceOn)},
        {"can_destroy", protoStringArray(network.canDestroy)},
        {"blocking_tick", ProtoDefValue::integer(network.blockingTick)}
    };
    if (hasNbt) {
        extra["nbt"] = protoNbtWrapper(
            *network.nbt,
            BedrockNbtEncoding::LittleEndian
        );
    }

    std::unordered_map<std::string, ProtoDefValue> fields {
        {"network_id", ProtoDefValue::integer(network.networkId)},
        {"count", ProtoDefValue::uinteger(network.count)},
        {"metadata", ProtoDefValue::integer(network.metadata)},
        {"has_stack_id", ProtoDefValue::uinteger(network.stackId.has_value() ? 1 : 0)},
        {"block_runtime_id", ProtoDefValue::integer(network.blockRuntimeId)},
        {"extra", ProtoDefValue::object(std::move(extra))}
    };
    if (network.stackId.has_value()) {
        fields["stack_id"] = ProtoDefValue::integer(*network.stackId);
    }
    return ProtoDefValue::object(std::move(fields));
}

bool BedrockItemRegistry::equal(
    const BedrockItem* first,
    const BedrockItem* second,
    bool matchStackSize,
    bool matchNbt
) const {
    if (first == nullptr || second == nullptr) return first == second;
    return first->equals(*second, matchStackSize, matchNbt);
}

void BedrockItemRegistry::loadItemStates(
    const std::vector<BedrockItemState>& itemStates
) {
    std::unordered_map<int32_t, BedrockItemDefinition> itemsById;
    std::unordered_map<std::string, std::size_t> itemIndicesByName;
    std::vector<BedrockItemDefinition> itemsArray;
    std::vector<BedrockItemState> normalizedStates;
    itemsById.reserve(itemStates.size());
    itemIndicesByName.reserve(itemStates.size());
    itemsArray.reserve(itemStates.size());
    normalizedStates.reserve(itemStates.size());

    for (const auto& supplied : itemStates) {
        BedrockItemState state = supplied;
        state.name = normalizeName(state.name);

        BedrockItemDefinition definition;
        const auto oldName = data_->itemIndicesByName.find(state.name);
        if (oldName != data_->itemIndicesByName.end()) {
            definition = data_->itemsArray[oldName->second];
        }
        definition.name = state.name;
        definition.id = state.runtimeId;

        // buildIndexFromArray is last-write-wins for duplicate IDs/names,
        // while itemsArray retains every entry in packet order.
        itemsById.insert_or_assign(definition.id, definition);
        itemIndicesByName.insert_or_assign(definition.name, itemsArray.size());
        itemsArray.push_back(std::move(definition));
        normalizedStates.push_back(std::move(state));
    }

    data_->itemsById = std::move(itemsById);
    data_->itemIndicesByName = std::move(itemIndicesByName);
    data_->itemsArray = std::move(itemsArray);
    data_->itemStates = std::move(normalizedStates);
}

void BedrockItemRegistry::loadItemStates(const ProtoDefValue& itemStates) {
    if (itemStates.kind != ProtoDefValue::Kind::Array) {
        throw std::runtime_error("Bedrock itemstates must be an array");
    }

    std::vector<BedrockItemState> parsed;
    parsed.reserve(itemStates.arrayValue.size());
    for (const auto& value : itemStates.arrayValue) {
        if (value.kind != ProtoDefValue::Kind::Object) {
            throw std::runtime_error("Bedrock itemstate must be an object");
        }
        BedrockItemState state;
        state.name = stringValue(
            requireField(value, "name", "itemstate"),
            "itemstate.name"
        );
        state.runtimeId = checkedInt32(
            integerValue(
                requireField(value, "runtime_id", "itemstate"),
                "itemstate.runtime_id"
            ),
            "itemstate.runtime_id"
        );
        if (const auto* componentBased = optionalField(value, "component_based")) {
            state.componentBased = boolValue(
                *componentBased,
                "itemstate.component_based"
            );
        }
        if (const auto* version = optionalField(value, "version")) {
            state.version = *version;
        }
        if (const auto* nbt = optionalField(value, "nbt")) state.nbt = *nbt;
        parsed.push_back(std::move(state));
    }
    loadItemStates(parsed);
}

std::vector<BedrockItemState> BedrockItemRegistry::writeItemStates() const {
    std::vector<BedrockItemState> out;
    out.reserve(data_->itemsArray.size());
    for (std::size_t index = 0; index < data_->itemsArray.size(); ++index) {
        const auto& item = data_->itemsArray[index];
        BedrockItemState state;
        state.name = item.name.find(':') == std::string::npos
            ? "minecraft:" + item.name
            : item.name;
        state.runtimeId = item.id;
        const auto separator = state.name.find(':');
        state.componentBased = separator != std::string::npos &&
            state.name.substr(0, separator) != "minecraft";
        if (index < data_->itemStates.size()) {
            state.version = data_->itemStates[index].version;
            state.nbt = data_->itemStates[index].nbt;
        }
        out.push_back(std::move(state));
    }
    return out;
}

ProtoDefValue BedrockItemRegistry::writeItemStatesValue() const {
    std::vector<ProtoDefValue> values;
    const auto states = writeItemStates();
    values.reserve(states.size());
    for (const auto& state : states) {
        std::unordered_map<std::string, ProtoDefValue> fields {
            {"name", ProtoDefValue::string(state.name)},
            {"runtime_id", ProtoDefValue::integer(state.runtimeId)},
            {"component_based", ProtoDefValue::boolean(state.componentBased)}
        };
        if (state.version.has_value()) fields.emplace("version", *state.version);
        if (state.nbt.has_value()) fields.emplace("nbt", *state.nbt);
        values.push_back(ProtoDefValue::object(std::move(fields)));
    }
    return ProtoDefValue::array(std::move(values));
}

int32_t BedrockItemRegistry::nextStackId() const {
    return data_->nextStackId.fetch_add(1, std::memory_order_relaxed);
}

int32_t BedrockItemRegistry::currentStackId() const {
    return data_->nextStackId.load(std::memory_order_relaxed);
}

void BedrockItemRegistry::resetStackIds(int32_t next) const {
    data_->nextStackId.store(next, std::memory_order_relaxed);
}

int32_t BedrockItemRegistry::protocolVersion() const {
    return data_->protocolVersion;
}

bool BedrockItemRegistry::usesAuxValue() const {
    return data_->usesAuxValue;
}

std::size_t BedrockItemRegistry::itemCount() const {
    return data_->itemsArray.size();
}

std::size_t BedrockItemRegistry::enchantmentCount() const {
    return data_->enchantmentsById.size();
}

std::string BedrockItemRegistry::normalizeName(std::string_view name) {
    if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
    return std::string(name);
}

BedrockItemRegistry BedrockItemRegistryLoader::loadMinecraftData(
    const std::filesystem::path& itemsJson,
    const std::filesystem::path& enchantmentsJson,
    int32_t protocolVersion,
    std::optional<bool> usesAuxValue
) {
    const auto itemsRoot = readJsonFile(itemsJson);
    requireKind(itemsRoot, JsonValue::Kind::Array, "items.json root");

    BedrockItemRegistry registry;
    registry.data_->protocolVersion = protocolVersion;
    // Prefer minecraft-data's Bedrock feature boundary. The protocol fallback
    // preserves direct loader compatibility when no feature registry is
    // available (the modern Item shape starts with protocol 440 / 1.17.0).
    registry.data_->usesAuxValue = usesAuxValue.value_or(
        protocolVersion >= 0 && protocolVersion < 440
    );

    for (const auto& value : itemsRoot.arrayValue) {
        BedrockItemDefinition item;
        item.id = checkedInt32(
            integerValue(requireField(value, "id", "item"), "item.id"),
            "item.id"
        );
        item.name = BedrockItemRegistry::normalizeName(
            stringValue(requireField(value, "name", "item"), "item.name")
        );
        item.displayName = stringValue(
            requireField(value, "displayName", "item"),
            "item.displayName"
        );
        item.stackSize = checkedUInt32(
            integerValue(requireField(value, "stackSize", "item"), "item.stackSize"),
            "item.stackSize"
        );
        if (const auto* metadata = optionalField(value, "metadata")) {
            item.metadata = checkedInt32(integerValue(*metadata, "item.metadata"), "item.metadata");
        }
        if (const auto* durability = optionalField(value, "maxDurability")) {
            item.maxDurability = checkedInt32(
                integerValue(*durability, "item.maxDurability"),
                "item.maxDurability"
            );
        } else if (const auto* durability = optionalField(value, "durability")) {
            item.maxDurability = checkedInt32(
                integerValue(*durability, "item.durability"),
                "item.durability"
            );
        }
        item.enchantCategories = stringArray(
            optionalField(value, "enchantCategories"),
            "item.enchantCategories"
        );
        item.repairWith = stringArray(optionalField(value, "repairWith"), "item.repairWith");

        if (const auto* variations = optionalField(value, "variations")) {
            requireKind(*variations, JsonValue::Kind::Array, "item.variations");
            item.variations.reserve(variations->arrayValue.size());
            for (const auto& variationValue : variations->arrayValue) {
                BedrockItemVariation variation;
                variation.metadata = checkedInt32(
                    integerValue(
                        requireField(variationValue, "metadata", "item variation"),
                        "item variation.metadata"
                    ),
                    "item variation.metadata"
                );
                variation.id = item.id;
                if (const auto* id = optionalField(variationValue, "id")) {
                    variation.id = checkedInt32(
                        integerValue(*id, "item variation.id"),
                        "item variation.id"
                    );
                }
                variation.name = item.name;
                if (const auto* name = optionalField(variationValue, "name")) {
                    variation.name = BedrockItemRegistry::normalizeName(
                        stringValue(*name, "item variation.name")
                    );
                }
                variation.displayName = stringValue(
                    requireField(variationValue, "displayName", "item variation"),
                    "item variation.displayName"
                );
                variation.stackSize = item.stackSize;
                if (const auto* stackSize = optionalField(variationValue, "stackSize")) {
                    variation.stackSize = checkedUInt32(
                        integerValue(*stackSize, "item variation.stackSize"),
                        "item variation.stackSize"
                    );
                }
                item.variations.push_back(std::move(variation));
            }
        }

        const auto id = item.id;
        const auto name = item.name;
        registry.data_->itemsArray.push_back(item);
        registry.data_->itemStates.push_back(BedrockItemState {
            .name = name,
            .runtimeId = id
        });
        if (!registry.data_->itemsById.emplace(id, std::move(item)).second) {
            throw std::runtime_error("duplicate Bedrock item id: " + std::to_string(id));
        }
        registry.data_->itemIndicesByName.insert_or_assign(
            name,
            registry.data_->itemsArray.size() - 1
        );
    }

    if (std::filesystem::exists(enchantmentsJson)) {
        const auto enchantmentsRoot = readJsonFile(enchantmentsJson);
        requireKind(
            enchantmentsRoot,
            JsonValue::Kind::Array,
            "enchantments.json root"
        );
        for (const auto& value : enchantmentsRoot.arrayValue) {
            BedrockEnchantmentDefinition enchantment;
            enchantment.id = checkedInt16(
                integerValue(requireField(value, "id", "enchantment"), "enchantment.id"),
                "enchantment.id"
            );
            enchantment.name = BedrockItemRegistry::normalizeName(stringValue(
                requireField(value, "name", "enchantment"),
                "enchantment.name"
            ));
            enchantment.displayName = stringValue(
                requireField(value, "displayName", "enchantment"),
                "enchantment.displayName"
            );
            enchantment.maxLevel = checkedInt32(
                integerValue(
                    requireField(value, "maxLevel", "enchantment"),
                    "enchantment.maxLevel"
                ),
                "enchantment.maxLevel"
            );
            enchantment.minCost = parseCost(
                requireField(value, "minCost", "enchantment"),
                "enchantment.minCost"
            );
            enchantment.maxCost = parseCost(
                requireField(value, "maxCost", "enchantment"),
                "enchantment.maxCost"
            );
            enchantment.treasureOnly = boolValue(
                requireField(value, "treasureOnly", "enchantment"),
                "enchantment.treasureOnly"
            );
            enchantment.curse = boolValue(
                requireField(value, "curse", "enchantment"),
                "enchantment.curse"
            );
            enchantment.exclude = stringArray(
                optionalField(value, "exclude"),
                "enchantment.exclude"
            );
            enchantment.category = stringValue(
                requireField(value, "category", "enchantment"),
                "enchantment.category"
            );
            enchantment.weight = checkedInt32(
                integerValue(
                    requireField(value, "weight", "enchantment"),
                    "enchantment.weight"
                ),
                "enchantment.weight"
            );
            enchantment.tradeable = boolValue(
                requireField(value, "tradeable", "enchantment"),
                "enchantment.tradeable"
            );
            enchantment.discoverable = boolValue(
                requireField(value, "discoverable", "enchantment"),
                "enchantment.discoverable"
            );

            const auto id = enchantment.id;
            const auto name = enchantment.name;
            if (!registry.data_->enchantmentsById.emplace(id, std::move(enchantment)).second) {
                throw std::runtime_error(
                    "duplicate Bedrock enchantment id: " + std::to_string(id)
                );
            }
            registry.data_->enchantmentIdsByName.insert_or_assign(name, id);
        }
    }

    return registry;
}

} // namespace bedrock
