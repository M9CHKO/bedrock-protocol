#include <bedrock/registry/BedrockGameplayRegistry.hpp>

#include <bedrock/protodef/ProtoDefJson.hpp>

#include <algorithm>
#include <charconv>
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

uint32_t checkedUInt32(int64_t value, std::string_view context) {
    if (value < 0 ||
        static_cast<uint64_t>(value) > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) + " is out of uint32 range"
        );
    }
    return static_cast<uint32_t>(value);
}

uint32_t numericObjectKey(std::string_view value, std::string_view context) {
    uint32_t result = 0;
    const auto parsed = std::from_chars(
        value.data(),
        value.data() + value.size(),
        result
    );
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size()) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) +
            " must be an unsigned integer key"
        );
    }
    return result;
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

std::optional<std::string> optionalString(
    const JsonValue* value,
    std::string_view context
) {
    if (value == nullptr || value->kind == JsonValue::Kind::Null) {
        return std::nullopt;
    }
    return stringValue(*value, context);
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

std::optional<uint32_t> optionalUInt32(
    const JsonValue* value,
    std::string_view context
) {
    if (value == nullptr || value->kind == JsonValue::Kind::Null) {
        return std::nullopt;
    }
    return checkedUInt32(integerValue(*value, context), context);
}

} // namespace

const BedrockRecipeDefinition* BedrockRecipeRegistry::recipeById(uint32_t id) const {
    const auto found = recipeIndexById_.find(id);
    return found == recipeIndexById_.end() ? nullptr : &recipes_[found->second];
}

const BedrockRecipeDefinition* BedrockRecipeRegistry::recipeByName(
    std::string_view name
) const {
    const auto found = recipeIndexByName_.find(std::string(name));
    return found == recipeIndexByName_.end() ? nullptr : &recipes_[found->second];
}

std::vector<const BedrockRecipeDefinition*> BedrockRecipeRegistry::recipesByName(
    std::string_view name
) const {
    std::vector<const BedrockRecipeDefinition*> out;
    const auto found = recipeIndicesByName_.find(std::string(name));
    if (found == recipeIndicesByName_.end()) return out;
    out.reserve(found->second.size());
    for (const auto index : found->second) out.push_back(&recipes_[index]);
    return out;
}

std::vector<const BedrockRecipeDefinition*> BedrockRecipeRegistry::recipesByType(
    std::string_view type
) const {
    std::vector<const BedrockRecipeDefinition*> out;
    const auto found = recipeIndicesByType_.find(std::string(type));
    if (found == recipeIndicesByType_.end()) return out;
    out.reserve(found->second.size());
    for (const auto index : found->second) out.push_back(&recipes_[index]);
    return out;
}

const std::vector<BedrockRecipeDefinition>& BedrockRecipeRegistry::all() const {
    return recipes_;
}

std::size_t BedrockRecipeRegistry::recipeCount() const {
    return recipes_.size();
}

std::size_t BedrockRecipeRegistry::uniqueRecipeNameCount() const {
    return recipeIndexByName_.size();
}

BedrockRecipeRegistry BedrockRecipeRegistryLoader::loadMinecraftData(
    const std::filesystem::path& recipesJson
) {
    const auto root = readJsonFile(recipesJson);
    requireKind(root, JsonValue::Kind::Object, "recipes.json root");

    struct SourceRecipe {
        uint32_t id;
        const JsonValue* value;
    };

    std::vector<SourceRecipe> source;
    source.reserve(root.objectValue.size());
    for (const auto& [key, value] : root.objectValue) {
        source.push_back({numericObjectKey(key, "recipe id"), &value});
    }
    std::sort(source.begin(), source.end(), [](const auto& first, const auto& second) {
        return first.id < second.id;
    });

    BedrockRecipeRegistry registry;
    registry.recipes_.reserve(source.size());

    for (const auto& sourceRecipe : source) {
        const auto& value = requireKind(
            *sourceRecipe.value,
            JsonValue::Kind::Object,
            "recipe"
        );

        BedrockRecipeDefinition recipe;
        recipe.id = sourceRecipe.id;
        recipe.type = stringValue(
            requireField(value, "type", "recipe"),
            "recipe.type"
        );
        recipe.name = optionalString(
            &requireField(value, "name", "recipe"),
            "recipe.name"
        );

        const auto& ingredients = requireKind(
            requireField(value, "ingredients", "recipe"),
            JsonValue::Kind::Array,
            "recipe.ingredients"
        );
        recipe.ingredients.reserve(ingredients.arrayValue.size());
        for (const auto& ingredientValue : ingredients.arrayValue) {
            BedrockRecipeIngredient ingredient;
            ingredient.name = stringValue(
                requireField(ingredientValue, "name", "recipe ingredient"),
                "recipe ingredient.name"
            );
            ingredient.count = checkedInt32(
                integerValue(
                    requireField(ingredientValue, "count", "recipe ingredient"),
                    "recipe ingredient.count"
                ),
                "recipe ingredient.count"
            );
            ingredient.metadata = optionalInt32(
                optionalField(ingredientValue, "metadata"),
                "recipe ingredient.metadata"
            );
            recipe.ingredients.push_back(std::move(ingredient));
        }

        if (const auto* input = optionalField(value, "input");
            input != nullptr && input->kind != JsonValue::Kind::Null) {
            requireKind(*input, JsonValue::Kind::Array, "recipe.input");
            std::vector<std::vector<int32_t>> rows;
            rows.reserve(input->arrayValue.size());
            for (const auto& rowValue : input->arrayValue) {
                requireKind(rowValue, JsonValue::Kind::Array, "recipe.input row");
                std::vector<int32_t> row;
                row.reserve(rowValue.arrayValue.size());
                for (const auto& ingredientIndex : rowValue.arrayValue) {
                    row.push_back(checkedInt32(
                        integerValue(ingredientIndex, "recipe.input index"),
                        "recipe.input index"
                    ));
                }
                rows.push_back(std::move(row));
            }
            recipe.input = std::move(rows);
        }

        const auto& output = requireKind(
            requireField(value, "output", "recipe"),
            JsonValue::Kind::Array,
            "recipe.output"
        );
        recipe.output.reserve(output.arrayValue.size());
        for (const auto& outputValue : output.arrayValue) {
            BedrockRecipeOutput item;
            item.name = stringValue(
                requireField(outputValue, "name", "recipe output"),
                "recipe output.name"
            );
            item.count = checkedInt32(
                integerValue(
                    requireField(outputValue, "count", "recipe output"),
                    "recipe output.count"
                ),
                "recipe output.count"
            );
            item.metadata = optionalInt32(
                optionalField(outputValue, "metadata"),
                "recipe output.metadata"
            );
            if (const auto* nbt = optionalField(outputValue, "nbt");
                nbt != nullptr && nbt->kind != JsonValue::Kind::Null) {
                item.nbt = *nbt;
            }
            recipe.output.push_back(std::move(item));
        }

        if (const auto* priority = optionalField(value, "priority");
            priority != nullptr && priority->kind != JsonValue::Kind::Null) {
            recipe.priority = numberValue(*priority, "recipe.priority");
        }

        const auto index = registry.recipes_.size();
        const auto id = recipe.id;
        const auto type = recipe.type;
        const auto name = recipe.name;
        registry.recipes_.push_back(std::move(recipe));
        registry.recipeIndexById_.insert_or_assign(id, index);
        registry.recipeIndicesByType_[type].push_back(index);
        if (name.has_value()) {
            registry.recipeIndexByName_.insert_or_assign(*name, index);
            registry.recipeIndicesByName_[*name].push_back(index);
        }
    }

    return registry;
}

uint32_t BedrockWindowSlot::slotCount() const {
    return size.value_or(1);
}

uint32_t BedrockWindowSlot::endIndex() const {
    const auto count = slotCount();
    if (count > std::numeric_limits<uint32_t>::max() - index) {
        return std::numeric_limits<uint32_t>::max();
    }
    return index + count;
}

const BedrockWindowSlot* BedrockWindowDefinition::slotByName(
    std::string_view slotName
) const {
    const auto found = std::find_if(slots.begin(), slots.end(), [&](const auto& slot) {
        return slot.name == slotName;
    });
    return found == slots.end() ? nullptr : &*found;
}

const BedrockWindowDefinition* BedrockWindowRegistry::windowById(
    std::string_view id
) const {
    const auto found = windowIndexById_.find(std::string(id));
    return found == windowIndexById_.end() ? nullptr : &windows_[found->second];
}

const BedrockWindowDefinition* BedrockWindowRegistry::windowByName(
    std::string_view name
) const {
    const auto found = windowIndexByName_.find(std::string(name));
    return found == windowIndexByName_.end() ? nullptr : &windows_[found->second];
}

const std::vector<BedrockWindowDefinition>& BedrockWindowRegistry::all() const {
    return windows_;
}

std::size_t BedrockWindowRegistry::windowCount() const {
    return windows_.size();
}

BedrockWindowRegistry BedrockWindowRegistryLoader::loadMinecraftData(
    const std::filesystem::path& windowsJson
) {
    const auto root = readJsonFile(windowsJson);
    requireKind(root, JsonValue::Kind::Array, "windows.json root");

    BedrockWindowRegistry registry;
    registry.windows_.reserve(root.arrayValue.size());
    for (const auto& value : root.arrayValue) {
        BedrockWindowDefinition window;
        window.id = stringValue(
            requireField(value, "id", "window"),
            "window.id"
        );
        window.name = stringValue(
            requireField(value, "name", "window"),
            "window.name"
        );

        if (const auto* slots = optionalField(value, "slots");
            slots != nullptr && slots->kind != JsonValue::Kind::Null) {
            requireKind(*slots, JsonValue::Kind::Array, "window.slots");
            window.slots.reserve(slots->arrayValue.size());
            for (const auto& slotValue : slots->arrayValue) {
                BedrockWindowSlot slot;
                slot.name = stringValue(
                    requireField(slotValue, "name", "window slot"),
                    "window slot.name"
                );
                slot.index = checkedUInt32(
                    integerValue(
                        requireField(slotValue, "index", "window slot"),
                        "window slot.index"
                    ),
                    "window slot.index"
                );
                slot.size = optionalUInt32(
                    optionalField(slotValue, "size"),
                    "window slot.size"
                );
                window.slots.push_back(std::move(slot));
            }
        }

        if (const auto* properties = optionalField(value, "properties");
            properties != nullptr && properties->kind != JsonValue::Kind::Null) {
            requireKind(*properties, JsonValue::Kind::Array, "window.properties");
            window.properties.reserve(properties->arrayValue.size());
            for (const auto& property : properties->arrayValue) {
                window.properties.push_back(stringValue(property, "window property"));
            }
        }

        if (const auto* openedWith = optionalField(value, "openedWith");
            openedWith != nullptr && openedWith->kind != JsonValue::Kind::Null) {
            requireKind(*openedWith, JsonValue::Kind::Array, "window.openedWith");
            window.openedWith.reserve(openedWith->arrayValue.size());
            for (const auto& openedWithValue : openedWith->arrayValue) {
                BedrockWindowOpenedWith opener;
                opener.type = stringValue(
                    requireField(openedWithValue, "type", "window opener"),
                    "window opener.type"
                );
                if (opener.type != "item" && opener.type != "entity" &&
                    opener.type != "block") {
                    throw std::runtime_error(
                        "minecraft-data window opener.type has an unknown value: " +
                        opener.type
                    );
                }
                opener.id = checkedInt32(
                    integerValue(
                        requireField(openedWithValue, "id", "window opener"),
                        "window opener.id"
                    ),
                    "window opener.id"
                );
                window.openedWith.push_back(std::move(opener));
            }
        }

        const auto index = registry.windows_.size();
        const auto id = window.id;
        const auto name = window.name;
        registry.windows_.push_back(std::move(window));
        registry.windowIndexById_.insert_or_assign(id, index);
        registry.windowIndexByName_.insert_or_assign(name, index);
    }
    return registry;
}

const BedrockInstrumentDefinition* BedrockInstrumentRegistry::instrumentById(
    int32_t id
) const {
    const auto found = instrumentIndexById_.find(id);
    return found == instrumentIndexById_.end() ? nullptr : &instruments_[found->second];
}

const BedrockInstrumentDefinition* BedrockInstrumentRegistry::instrumentByName(
    std::string_view name
) const {
    const auto found = instrumentIndexByName_.find(std::string(name));
    return found == instrumentIndexByName_.end() ? nullptr : &instruments_[found->second];
}

const std::vector<BedrockInstrumentDefinition>& BedrockInstrumentRegistry::all() const {
    return instruments_;
}

std::size_t BedrockInstrumentRegistry::instrumentCount() const {
    return instruments_.size();
}

BedrockInstrumentRegistry BedrockInstrumentRegistryLoader::loadMinecraftData(
    const std::filesystem::path& instrumentsJson
) {
    const auto root = readJsonFile(instrumentsJson);
    requireKind(root, JsonValue::Kind::Array, "instruments.json root");

    BedrockInstrumentRegistry registry;
    registry.instruments_.reserve(root.arrayValue.size());
    for (const auto& value : root.arrayValue) {
        BedrockInstrumentDefinition instrument;
        instrument.id = checkedInt32(
            integerValue(
                requireField(value, "id", "instrument"),
                "instrument.id"
            ),
            "instrument.id"
        );
        instrument.name = stringValue(
            requireField(value, "name", "instrument"),
            "instrument.name"
        );
        instrument.sound = optionalString(
            optionalField(value, "sound"),
            "instrument.sound"
        );

        const auto index = registry.instruments_.size();
        const auto id = instrument.id;
        const auto name = instrument.name;
        registry.instruments_.push_back(std::move(instrument));
        registry.instrumentIndexById_.insert_or_assign(id, index);
        registry.instrumentIndexByName_.insert_or_assign(name, index);
    }
    return registry;
}

double BedrockAttributeDefinition::clamp(double value) const {
    return std::clamp(value, min, max);
}

const BedrockAttributeDefinition* BedrockAttributeRegistry::attributeByName(
    std::string_view name
) const {
    const auto found = attributeIndexByName_.find(std::string(name));
    return found == attributeIndexByName_.end() ? nullptr : &attributes_[found->second];
}

const BedrockAttributeDefinition* BedrockAttributeRegistry::attributeByResource(
    std::string_view resource
) const {
    const auto found = attributeIndexByResource_.find(std::string(resource));
    return found == attributeIndexByResource_.end()
        ? nullptr
        : &attributes_[found->second];
}

const std::vector<BedrockAttributeDefinition>& BedrockAttributeRegistry::all() const {
    return attributes_;
}

std::size_t BedrockAttributeRegistry::attributeCount() const {
    return attributes_.size();
}

BedrockAttributeRegistry BedrockAttributeRegistryLoader::loadMinecraftData(
    const std::filesystem::path& attributesJson
) {
    const auto root = readJsonFile(attributesJson);
    requireKind(root, JsonValue::Kind::Array, "attributes.json root");

    BedrockAttributeRegistry registry;
    registry.attributes_.reserve(root.arrayValue.size());
    for (const auto& value : root.arrayValue) {
        BedrockAttributeDefinition attribute;
        attribute.name = stringValue(
            requireField(value, "name", "attribute"),
            "attribute.name"
        );
        attribute.resource = stringValue(
            requireField(value, "resource", "attribute"),
            "attribute.resource"
        );
        attribute.defaultValue = numberValue(
            requireField(value, "default", "attribute"),
            "attribute.default"
        );
        attribute.min = numberValue(
            requireField(value, "min", "attribute"),
            "attribute.min"
        );
        attribute.max = numberValue(
            requireField(value, "max", "attribute"),
            "attribute.max"
        );
        if (attribute.min > attribute.max) {
            throw std::runtime_error(
                "minecraft-data attribute min is greater than max: " + attribute.name
            );
        }

        const auto index = registry.attributes_.size();
        const auto name = attribute.name;
        const auto resource = attribute.resource;
        registry.attributes_.push_back(std::move(attribute));
        registry.attributeIndexByName_.insert_or_assign(name, index);
        registry.attributeIndexByResource_.insert_or_assign(resource, index);
    }
    return registry;
}

} // namespace bedrock
