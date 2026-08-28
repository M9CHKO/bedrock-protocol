#pragma once

#include <bedrock/chat/BedrockChat.hpp>
#include <bedrock/registry/BedrockRegistry.hpp>
#include <bedrock/world/BlockRuntimeRegistryLoader.hpp>
#include <bedrock/world/MinecraftDataIndex.hpp>
#include <bedrock/world/MinecraftDataPathResolver.hpp>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bedrock {

struct MinecraftDataAssetsPaths {
    MinecraftDataVersionInfo version;

    std::string blocksDirectory;
    std::string blockStatesDirectory;
    std::string blockCollisionShapesDirectory;
    std::string itemsDirectory;
    std::string protocolDirectory;
    std::string typesDirectory;
    std::string biomesDirectory;
    std::string entitiesDirectory;
    std::string enchantmentsDirectory;
    std::string languageDirectory;
    std::string recipesDirectory;
    std::string windowsDirectory;
    std::string instrumentsDirectory;
    std::string attributesDirectory;
    std::string blockLootDirectory;
    std::string entityLootDirectory;
    std::string steveDirectory;

    std::filesystem::path versionDir;
    std::filesystem::path protocolJson;
    std::filesystem::path blocksJson;
    std::filesystem::path blockStatesJson;
    std::filesystem::path blockCollisionShapesJson;
    std::filesystem::path itemsJson;
    std::filesystem::path biomesJson;
    std::filesystem::path entitiesJson;
    std::filesystem::path enchantmentsJson;
    std::filesystem::path languageJson;
    std::filesystem::path recipesJson;
    std::filesystem::path windowsJson;
    std::filesystem::path instrumentsJson;
    std::filesystem::path attributesJson;
    std::filesystem::path blockLootJson;
    std::filesystem::path entityLootJson;
    std::filesystem::path steveJson;
    std::filesystem::path featuresJson;
    std::filesystem::path protocolVersionsJson;
    std::filesystem::path blockRuntimeTsv;
};

class MinecraftDataAssets {
public:
    explicit MinecraftDataAssets(
        std::filesystem::path bedrockDataPath = "data/minecraft-data/bedrock",
        std::filesystem::path generatedRuntimePath = "data/generated/block-runtime/bedrock",
        std::filesystem::path dataPathsPath = "data/minecraft-data/dataPaths.json"
    )
        : index_(std::move(bedrockDataPath)),
          generatedRuntimePath_(std::move(generatedRuntimePath)),
          dataPathsPath_(std::move(dataPathsPath)) {}

    MinecraftDataAssetsPaths resolveByVersion(const std::string& version) const {
        auto info = index_.findByMinecraftVersion(version);
        if (!info.has_value()) {
            throw std::runtime_error("minecraft-data version not found: " + version);
        }

        return makePaths(*info);
    }

    MinecraftDataAssetsPaths resolveByProtocol(int32_t protocol) const {
        auto info = index_.findByProtocol(protocol);
        if (!info.has_value()) {
            throw std::runtime_error("minecraft-data protocol not found: " + std::to_string(protocol));
        }

        return makePaths(*info);
    }

    BlockRuntimeRegistry loadBlockRuntimeRegistryByVersion(const std::string& version) const {
        auto paths = resolveByVersion(version);
        if (std::filesystem::exists(paths.blockRuntimeTsv)) {
            return BlockRuntimeRegistryLoader::loadTsv(paths.blockRuntimeTsv.string());
        }
        return loadBedrockBlockRegistry(paths).toRuntimeRegistry();
    }

    BlockRuntimeRegistry loadBlockRuntimeRegistryByProtocol(int32_t protocol) const {
        auto paths = resolveByProtocol(protocol);
        if (std::filesystem::exists(paths.blockRuntimeTsv)) {
            return BlockRuntimeRegistryLoader::loadTsv(paths.blockRuntimeTsv.string());
        }
        return loadBedrockBlockRegistry(paths).toRuntimeRegistry();
    }

    BedrockBlockRegistry loadBedrockBlockRegistryByVersion(const std::string& version) const {
        return loadBedrockBlockRegistry(resolveByVersion(version));
    }

    BedrockBlockRegistry loadBedrockBlockRegistryByProtocol(int32_t protocol) const {
        return loadBedrockBlockRegistry(resolveByProtocol(protocol));
    }

    BedrockItemRegistry loadBedrockItemRegistryByVersion(const std::string& version) const {
        return loadBedrockItemRegistry(resolveByVersion(version));
    }

    BedrockItemRegistry loadBedrockItemRegistryByProtocol(int32_t protocol) const {
        return loadBedrockItemRegistry(resolveByProtocol(protocol));
    }

    BedrockBiomeRegistry loadBedrockBiomeRegistryByVersion(
        const std::string& version
    ) const {
        return loadBedrockBiomeRegistry(resolveByVersion(version));
    }

    BedrockBiomeRegistry loadBedrockBiomeRegistryByProtocol(int32_t protocol) const {
        return loadBedrockBiomeRegistry(resolveByProtocol(protocol));
    }

    BedrockEntityRegistry loadBedrockEntityRegistryByVersion(
        const std::string& version
    ) const {
        return loadBedrockEntityRegistry(resolveByVersion(version));
    }

    BedrockEntityRegistry loadBedrockEntityRegistryByProtocol(int32_t protocol) const {
        return loadBedrockEntityRegistry(resolveByProtocol(protocol));
    }

    BedrockRecipeRegistry loadBedrockRecipeRegistryByVersion(
        const std::string& version
    ) const {
        return loadBedrockRecipeRegistry(resolveByVersion(version));
    }

    BedrockRecipeRegistry loadBedrockRecipeRegistryByProtocol(
        int32_t protocol
    ) const {
        return loadBedrockRecipeRegistry(resolveByProtocol(protocol));
    }

    BedrockWindowRegistry loadBedrockWindowRegistryByVersion(
        const std::string& version
    ) const {
        return loadBedrockWindowRegistry(resolveByVersion(version));
    }

    BedrockWindowRegistry loadBedrockWindowRegistryByProtocol(
        int32_t protocol
    ) const {
        return loadBedrockWindowRegistry(resolveByProtocol(protocol));
    }

    BedrockInstrumentRegistry loadBedrockInstrumentRegistryByVersion(
        const std::string& version
    ) const {
        return loadBedrockInstrumentRegistry(resolveByVersion(version));
    }

    BedrockInstrumentRegistry loadBedrockInstrumentRegistryByProtocol(
        int32_t protocol
    ) const {
        return loadBedrockInstrumentRegistry(resolveByProtocol(protocol));
    }

    BedrockAttributeRegistry loadBedrockAttributeRegistryByVersion(
        const std::string& version
    ) const {
        return loadBedrockAttributeRegistry(resolveByVersion(version));
    }

    BedrockAttributeRegistry loadBedrockAttributeRegistryByProtocol(
        int32_t protocol
    ) const {
        return loadBedrockAttributeRegistry(resolveByProtocol(protocol));
    }

    BedrockFeatureRegistry loadBedrockFeatureRegistryByVersion(
        const std::string& version
    ) const {
        return loadBedrockFeatureRegistry(resolveByVersion(version));
    }

    BedrockFeatureRegistry loadBedrockFeatureRegistryByProtocol(
        int32_t protocol
    ) const {
        return loadBedrockFeatureRegistry(resolveByProtocol(protocol));
    }

    BedrockLootRegistry loadBedrockLootRegistryByVersion(
        const std::string& version
    ) const {
        return loadBedrockLootRegistry(resolveByVersion(version));
    }

    BedrockLootRegistry loadBedrockLootRegistryByProtocol(
        int32_t protocol
    ) const {
        return loadBedrockLootRegistry(resolveByProtocol(protocol));
    }

    std::optional<BedrockDefaultSkin> loadBedrockDefaultSkinByVersion(
        const std::string& version
    ) const {
        return loadBedrockDefaultSkin(resolveByVersion(version));
    }

    std::optional<BedrockDefaultSkin> loadBedrockDefaultSkinByProtocol(
        int32_t protocol
    ) const {
        return loadBedrockDefaultSkin(resolveByProtocol(protocol));
    }

    BedrockRegistry loadBedrockRegistryByVersion(const std::string& version) const {
        return loadBedrockRegistry(resolveByVersion(version));
    }

    BedrockRegistry loadBedrockRegistryByProtocol(int32_t protocol) const {
        return loadBedrockRegistry(resolveByProtocol(protocol));
    }

    BedrockLanguage loadBedrockLanguageByVersion(const std::string& version) const {
        return loadBedrockLanguage(resolveByVersion(version));
    }

    BedrockLanguage loadBedrockLanguageByProtocol(int32_t protocol) const {
        return loadBedrockLanguage(resolveByProtocol(protocol));
    }

    BedrockChat loadBedrockChatByVersion(const std::string& version) const {
        return BedrockChat(loadBedrockLanguageByVersion(version));
    }

    BedrockChat loadBedrockChatByProtocol(int32_t protocol) const {
        return BedrockChat(loadBedrockLanguageByProtocol(protocol));
    }

private:
    MinecraftDataIndex index_;
    std::filesystem::path generatedRuntimePath_;
    std::filesystem::path dataPathsPath_;

    MinecraftDataAssetsPaths makePaths(const MinecraftDataVersionInfo& info) const {
        MinecraftDataAssetsPaths paths;
        paths.version = info;

        MinecraftDataPathSet remap;
        bool hasRemap = false;
        if (std::filesystem::exists(dataPathsPath_)) {
            MinecraftDataPathResolver resolver(dataPathsPath_);
            auto found = resolver.findBedrock(info.directory);
            if (found.has_value()) {
                remap = *found;
                hasRemap = true;
            }
        }

        if (!hasRemap) {
            const auto optionalDirectory = [&](std::string_view fileName) {
                return std::filesystem::exists(
                    index_.versionPath(info.directory) / fileName
                ) ? info.directory : std::string{};
            };

            remap.version = info.directory;
            remap.blocks = info.directory;
            remap.blockStates = optionalDirectory("blockStates.json");
            remap.blockCollisionShapes = optionalDirectory("blockCollisionShapes.json");
            remap.items = info.directory;
            remap.protocol = info.directory;
            remap.types = info.directory;
            remap.biomes = info.directory;
            remap.entities = info.directory;
            remap.enchantments = info.directory;
            remap.language = info.directory;

            remap.recipes = optionalDirectory("recipes.json");
            remap.windows = optionalDirectory("windows.json");
            remap.instruments = optionalDirectory("instruments.json");
            remap.attributes = optionalDirectory("attributes.json");
            remap.blockLoot = optionalDirectory("blockLoot.json");
            remap.entityLoot = optionalDirectory("entityLoot.json");
            remap.steve = optionalDirectory("steve.json");
        }

        paths.blocksDirectory = remap.blocks;
        paths.blockStatesDirectory = remap.blockStates;
        paths.blockCollisionShapesDirectory = remap.blockCollisionShapes;
        paths.itemsDirectory = remap.items;
        paths.protocolDirectory = remap.protocol;
        paths.typesDirectory = remap.types;
        paths.biomesDirectory = remap.biomes;
        paths.entitiesDirectory = remap.entities;
        paths.enchantmentsDirectory = remap.enchantments;
        paths.languageDirectory = remap.language;
        paths.recipesDirectory = remap.recipes;
        paths.windowsDirectory = remap.windows;
        paths.instrumentsDirectory = remap.instruments;
        paths.attributesDirectory = remap.attributes;
        paths.blockLootDirectory = remap.blockLoot;
        paths.entityLootDirectory = remap.entityLoot;
        paths.steveDirectory = remap.steve;

        paths.versionDir = index_.versionPath(info.directory);
        paths.protocolJson = index_.protocolPath(remap.protocol);
        if (!std::filesystem::exists(paths.protocolJson)) {
            paths.protocolJson.clear();
            paths.protocolDirectory.clear();
        }
        paths.blocksJson = index_.blocksPath(remap.blocks);
        paths.blockStatesJson = remap.blockStates.empty()
            ? std::filesystem::path{}
            : index_.versionPath(remap.blockStates) / "blockStates.json";
        paths.blockCollisionShapesJson = remap.blockCollisionShapes.empty()
            ? std::filesystem::path{}
            : index_.versionPath(remap.blockCollisionShapes) / "blockCollisionShapes.json";
        paths.itemsJson = index_.itemsPath(remap.items);
        paths.biomesJson = remap.biomes.empty()
            ? std::filesystem::path{}
            : index_.versionPath(remap.biomes) / "biomes.json";
        paths.entitiesJson = remap.entities.empty()
            ? std::filesystem::path{}
            : index_.versionPath(remap.entities) / "entities.json";
        paths.enchantmentsJson = remap.enchantments.empty()
            ? std::filesystem::path{}
            : index_.versionPath(remap.enchantments) / "enchantments.json";
        paths.languageJson = remap.language.empty()
            ? std::filesystem::path{}
            : index_.versionPath(remap.language) / "language.json";
        paths.recipesJson = remap.recipes.empty()
            ? std::filesystem::path{}
            : index_.versionPath(remap.recipes) / "recipes.json";
        paths.windowsJson = remap.windows.empty()
            ? std::filesystem::path{}
            : index_.versionPath(remap.windows) / "windows.json";
        paths.instrumentsJson = remap.instruments.empty()
            ? std::filesystem::path{}
            : index_.versionPath(remap.instruments) / "instruments.json";
        paths.attributesJson = remap.attributes.empty()
            ? std::filesystem::path{}
            : index_.versionPath(remap.attributes) / "attributes.json";
        paths.blockLootJson = remap.blockLoot.empty()
            ? std::filesystem::path{}
            : index_.versionPath(remap.blockLoot) / "blockLoot.json";
        paths.entityLootJson = remap.entityLoot.empty()
            ? std::filesystem::path{}
            : index_.versionPath(remap.entityLoot) / "entityLoot.json";
        paths.steveJson = remap.steve.empty()
            ? std::filesystem::path{}
            : index_.versionPath(remap.steve) / "steve.json";
        paths.featuresJson = index_.basePath() / "common" / "features.json";
        paths.protocolVersionsJson =
            index_.basePath() / "common" / "protocolVersions.json";
        paths.blockRuntimeTsv = generatedRuntimePath_ / (remap.blocks + ".tsv");

        if (!std::filesystem::exists(paths.versionDir)) {
            throw std::runtime_error("minecraft-data version dir missing: " + paths.versionDir.string());
        }

        return paths;
    }

    static BedrockBlockRegistry loadBedrockBlockRegistry(
        const MinecraftDataAssetsPaths& paths
    ) {
        const auto features = loadBedrockFeatureRegistry(paths);
        return loadBedrockBlockRegistry(paths, features);
    }

    static BedrockBlockRegistry loadBedrockBlockRegistry(
        const MinecraftDataAssetsPaths& paths,
        const BedrockFeatureRegistry& features
    ) {
        return BedrockBlockRegistryLoader::loadMinecraftData(
            paths.blocksJson,
            paths.blockStatesJson,
            paths.blockCollisionShapesJson,
            features.supportsFeature("blockHashes")
        );
    }

    static BedrockItemRegistry loadBedrockItemRegistry(
        const MinecraftDataAssetsPaths& paths
    ) {
        const auto features = loadBedrockFeatureRegistry(paths);
        return loadBedrockItemRegistry(paths, features);
    }

    static BedrockItemRegistry loadBedrockItemRegistry(
        const MinecraftDataAssetsPaths& paths,
        const BedrockFeatureRegistry& features
    ) {
        // minecraft-data defines itemSerializeUsesAuxValue only for the
        // 1.16.201+ feature era. Keep the protocol fallback for older Bedrock
        // datasets, whose packet Item shape also uses auxiliary_value.
        const std::optional<bool> usesAuxValue =
            features.isNewerOrEqualTo("1.16.201")
                ? std::optional<bool>(features.supportsFeature(
                      "itemSerializeUsesAuxValue"
                  ))
                : std::nullopt;
        return BedrockItemRegistryLoader::loadMinecraftData(
            paths.itemsJson,
            paths.enchantmentsJson,
            paths.version.protocol,
            usesAuxValue
        );
    }

    static BedrockBiomeRegistry loadBedrockBiomeRegistry(
        const MinecraftDataAssetsPaths& paths
    ) {
        if (paths.biomesJson.empty() || !std::filesystem::exists(paths.biomesJson)) {
            return {};
        }
        return BedrockBiomeRegistryLoader::loadMinecraftData(paths.biomesJson);
    }

    static BedrockEntityRegistry loadBedrockEntityRegistry(
        const MinecraftDataAssetsPaths& paths
    ) {
        if (paths.entitiesJson.empty() || !std::filesystem::exists(paths.entitiesJson)) {
            return {};
        }
        return BedrockEntityRegistryLoader::loadMinecraftData(paths.entitiesJson);
    }

    static BedrockRecipeRegistry loadBedrockRecipeRegistry(
        const MinecraftDataAssetsPaths& paths
    ) {
        if (paths.recipesJson.empty()) return {};
        return BedrockRecipeRegistryLoader::loadMinecraftData(paths.recipesJson);
    }

    static BedrockWindowRegistry loadBedrockWindowRegistry(
        const MinecraftDataAssetsPaths& paths
    ) {
        if (paths.windowsJson.empty()) return {};
        return BedrockWindowRegistryLoader::loadMinecraftData(paths.windowsJson);
    }

    static BedrockInstrumentRegistry loadBedrockInstrumentRegistry(
        const MinecraftDataAssetsPaths& paths
    ) {
        if (paths.instrumentsJson.empty()) return {};
        return BedrockInstrumentRegistryLoader::loadMinecraftData(
            paths.instrumentsJson
        );
    }

    static BedrockAttributeRegistry loadBedrockAttributeRegistry(
        const MinecraftDataAssetsPaths& paths
    ) {
        if (paths.attributesJson.empty()) return {};
        return BedrockAttributeRegistryLoader::loadMinecraftData(
            paths.attributesJson
        );
    }

    static BedrockFeatureRegistry loadBedrockFeatureRegistry(
        const MinecraftDataAssetsPaths& paths
    ) {
        return BedrockFeatureRegistryLoader::loadMinecraftData(
            paths.featuresJson,
            paths.protocolVersionsJson,
            paths.version
        );
    }

    static BedrockLootRegistry loadBedrockLootRegistry(
        const MinecraftDataAssetsPaths& paths
    ) {
        return BedrockLootRegistryLoader::loadMinecraftData(
            paths.blockLootJson,
            paths.entityLootJson
        );
    }

    static std::optional<BedrockDefaultSkin> loadBedrockDefaultSkin(
        const MinecraftDataAssetsPaths& paths
    ) {
        if (paths.steveJson.empty()) return std::nullopt;
        return BedrockDefaultSkinLoader::loadMinecraftData(paths.steveJson);
    }

    static BedrockLanguage loadBedrockLanguage(const MinecraftDataAssetsPaths& paths) {
        if (paths.languageJson.empty() || !std::filesystem::exists(paths.languageJson)) {
            return {};
        }
        return BedrockLanguageLoader::loadMinecraftData(paths.languageJson);
    }

    static BedrockRegistry loadBedrockRegistry(const MinecraftDataAssetsPaths& paths) {
        auto features = loadBedrockFeatureRegistry(paths);
        auto blocks = loadBedrockBlockRegistry(paths, features);
        auto items = loadBedrockItemRegistry(paths, features);
        auto biomes = loadBedrockBiomeRegistry(paths);
        auto entities = loadBedrockEntityRegistry(paths);
        auto recipes = loadBedrockRecipeRegistry(paths);
        auto windows = loadBedrockWindowRegistry(paths);
        auto instruments = loadBedrockInstrumentRegistry(paths);
        auto attributes = loadBedrockAttributeRegistry(paths);
        auto loot = loadBedrockLootRegistry(paths);
        auto defaultSkin = loadBedrockDefaultSkin(paths);
        return BedrockRegistry(
            paths.version,
            std::move(blocks),
            std::move(items),
            std::move(biomes),
            std::move(entities),
            std::move(recipes),
            std::move(windows),
            std::move(instruments),
            std::move(attributes),
            std::move(features),
            std::move(loot),
            std::move(defaultSkin)
        );
    }
};

} // namespace bedrock
