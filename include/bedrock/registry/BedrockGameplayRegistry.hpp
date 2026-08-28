#pragma once

#include <bedrock/protodef/ProtoDefValue.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bedrock {

struct BedrockRecipeIngredient {
    std::string name;
    int32_t count = 1;
    std::optional<int32_t> metadata;
};

struct BedrockRecipeOutput {
    std::string name;
    int32_t count = 1;
    std::optional<int32_t> metadata;

    // minecraft-data stores Bedrock item NBT as the native tagged JSON
    // object. Keep it structured so callers can inspect or encode it without
    // a lossy string round-trip.
    std::optional<ProtoDefValue> nbt;
};

struct BedrockRecipeDefinition {
    uint32_t id = 0;
    std::string type;
    std::optional<std::string> name;
    std::vector<BedrockRecipeIngredient> ingredients;
    std::optional<std::vector<std::vector<int32_t>>> input;
    std::vector<BedrockRecipeOutput> output;
    std::optional<double> priority;
};

class BedrockRecipeRegistryLoader;

class BedrockRecipeRegistry {
public:
    const BedrockRecipeDefinition* recipeById(uint32_t id) const;

    // Recipe names are not unique in the Bedrock data. This lookup mirrors
    // minecraft-data's indexes: the last recipe in numeric ID order wins.
    const BedrockRecipeDefinition* recipeByName(std::string_view name) const;
    std::vector<const BedrockRecipeDefinition*> recipesByName(
        std::string_view name
    ) const;
    std::vector<const BedrockRecipeDefinition*> recipesByType(
        std::string_view type
    ) const;

    const std::vector<BedrockRecipeDefinition>& all() const;
    std::size_t recipeCount() const;
    std::size_t uniqueRecipeNameCount() const;

private:
    friend class BedrockRecipeRegistryLoader;

    std::vector<BedrockRecipeDefinition> recipes_;
    std::unordered_map<uint32_t, std::size_t> recipeIndexById_;
    std::unordered_map<std::string, std::size_t> recipeIndexByName_;
    std::unordered_map<std::string, std::vector<std::size_t>> recipeIndicesByName_;
    std::unordered_map<std::string, std::vector<std::size_t>> recipeIndicesByType_;
};

class BedrockRecipeRegistryLoader {
public:
    static BedrockRecipeRegistry loadMinecraftData(
        const std::filesystem::path& recipesJson
    );
};

struct BedrockWindowSlot {
    std::string name;
    uint32_t index = 0;
    std::optional<uint32_t> size;

    uint32_t slotCount() const;
    uint32_t endIndex() const;
};

struct BedrockWindowOpenedWith {
    std::string type;
    int32_t id = 0;
};

struct BedrockWindowDefinition {
    std::string id;
    std::string name;
    std::vector<BedrockWindowSlot> slots;
    std::vector<std::string> properties;
    std::vector<BedrockWindowOpenedWith> openedWith;

    const BedrockWindowSlot* slotByName(std::string_view slotName) const;
};

class BedrockWindowRegistryLoader;

class BedrockWindowRegistry {
public:
    const BedrockWindowDefinition* windowById(std::string_view id) const;
    const BedrockWindowDefinition* windowByName(std::string_view name) const;

    const std::vector<BedrockWindowDefinition>& all() const;
    std::size_t windowCount() const;

private:
    friend class BedrockWindowRegistryLoader;

    std::vector<BedrockWindowDefinition> windows_;
    std::unordered_map<std::string, std::size_t> windowIndexById_;
    std::unordered_map<std::string, std::size_t> windowIndexByName_;
};

class BedrockWindowRegistryLoader {
public:
    static BedrockWindowRegistry loadMinecraftData(
        const std::filesystem::path& windowsJson
    );
};

struct BedrockInstrumentDefinition {
    int32_t id = 0;
    std::string name;
    std::optional<std::string> sound;
};

class BedrockInstrumentRegistryLoader;

class BedrockInstrumentRegistry {
public:
    const BedrockInstrumentDefinition* instrumentById(int32_t id) const;
    const BedrockInstrumentDefinition* instrumentByName(std::string_view name) const;

    const std::vector<BedrockInstrumentDefinition>& all() const;
    std::size_t instrumentCount() const;

private:
    friend class BedrockInstrumentRegistryLoader;

    std::vector<BedrockInstrumentDefinition> instruments_;
    std::unordered_map<int32_t, std::size_t> instrumentIndexById_;
    std::unordered_map<std::string, std::size_t> instrumentIndexByName_;
};

class BedrockInstrumentRegistryLoader {
public:
    static BedrockInstrumentRegistry loadMinecraftData(
        const std::filesystem::path& instrumentsJson
    );
};

struct BedrockAttributeDefinition {
    std::string name;
    std::string resource;
    double defaultValue = 0.0;
    double min = 0.0;
    double max = 0.0;

    double clamp(double value) const;
};

class BedrockAttributeRegistryLoader;

class BedrockAttributeRegistry {
public:
    const BedrockAttributeDefinition* attributeByName(std::string_view name) const;
    const BedrockAttributeDefinition* attributeByResource(
        std::string_view resource
    ) const;

    const std::vector<BedrockAttributeDefinition>& all() const;
    std::size_t attributeCount() const;

private:
    friend class BedrockAttributeRegistryLoader;

    std::vector<BedrockAttributeDefinition> attributes_;
    std::unordered_map<std::string, std::size_t> attributeIndexByName_;
    std::unordered_map<std::string, std::size_t> attributeIndexByResource_;
};

class BedrockAttributeRegistryLoader {
public:
    static BedrockAttributeRegistry loadMinecraftData(
        const std::filesystem::path& attributesJson
    );
};

} // namespace bedrock
