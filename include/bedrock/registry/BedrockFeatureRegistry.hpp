#pragma once

#include <bedrock/protodef/ProtoDefValue.hpp>
#include <bedrock/world/MinecraftDataIndex.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bedrock {

struct BedrockFeatureDefinition {
    std::string name;
    std::string description;

    // Mirrors minecraft-data supportFeature(): boolean features resolve to a
    // Bool and value features retain their JSON value (currently strings).
    // A feature outside its version range resolves to Bool(false).
    ProtoDefValue value = ProtoDefValue::boolean(false);

    bool supported() const;
};

class BedrockFeatureRegistryLoader;

class BedrockFeatureRegistry {
public:
    const MinecraftDataVersionInfo& version() const;

    const BedrockFeatureDefinition* feature(std::string_view name) const;

    // Exact Node-compatible result: unknown, disabled, and falsey feature
    // values return Bool(false); value features return their typed value.
    ProtoDefValue supportFeature(std::string_view name) const;
    bool supportsFeature(std::string_view name) const;
    std::optional<std::string_view> featureString(std::string_view name) const;

    const std::vector<BedrockFeatureDefinition>& all() const;
    std::size_t featureCount() const;

    bool isNewerThan(std::string_view otherVersion) const;
    bool isNewerOrEqualTo(std::string_view otherVersion) const;
    bool isOlderThan(std::string_view otherVersion) const;
    bool isOlderOrEqualTo(std::string_view otherVersion) const;
    bool isSameVersion(std::string_view otherVersion) const;

private:
    friend class BedrockFeatureRegistryLoader;

    MinecraftDataVersionInfo version_;
    int64_t versionOrder_ = 0;
    bool hasVersion_ = false;
    std::vector<BedrockFeatureDefinition> features_;
    std::unordered_map<std::string, std::size_t> featureIndexByName_;
    std::unordered_map<std::string, int64_t> versionOrderByName_;

    int compare(std::string_view otherVersion) const;
    static bool truthy(const ProtoDefValue& value);
};

class BedrockFeatureRegistryLoader {
public:
    static BedrockFeatureRegistry loadMinecraftData(
        const std::filesystem::path& featuresJson,
        const std::filesystem::path& protocolVersionsJson,
        MinecraftDataVersionInfo version
    );
};

} // namespace bedrock
