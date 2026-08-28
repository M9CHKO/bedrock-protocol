#include <bedrock/registry/BedrockFeatureRegistry.hpp>

#include <bedrock/protodef/ProtoDefJson.hpp>

#include <cmath>
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

std::optional<std::string> optionalString(
    const JsonValue* value,
    std::string_view context
) {
    if (value == nullptr || value->kind == JsonValue::Kind::Null) {
        return std::nullopt;
    }
    return stringValue(*value, context);
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

std::pair<std::string, std::string> versionRange(
    const JsonValue& value,
    std::string_view context
) {
    requireKind(value, JsonValue::Kind::Array, context);
    if (value.arrayValue.size() != 2) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) +
            " must contain exactly two versions"
        );
    }
    return {
        stringValue(value.arrayValue[0], std::string(context) + " minimum"),
        stringValue(value.arrayValue[1], std::string(context) + " maximum")
    };
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
        value.substr(value.size() - suffix.size()) == suffix;
}

} // namespace

bool BedrockFeatureDefinition::supported() const {
    switch (value.kind) {
        case ProtoDefValue::Kind::Null: return false;
        case ProtoDefValue::Kind::Bool: return value.boolValue;
        case ProtoDefValue::Kind::Int: return value.intValue != 0;
        case ProtoDefValue::Kind::UInt: return value.uintValue != 0;
        case ProtoDefValue::Kind::Double:
            return value.doubleValue != 0.0 && !std::isnan(value.doubleValue);
        case ProtoDefValue::Kind::String: return !value.stringValue.empty();
        case ProtoDefValue::Kind::Bytes:
        case ProtoDefValue::Kind::Object:
        case ProtoDefValue::Kind::Array:
            return true;
    }
    return false;
}

const MinecraftDataVersionInfo& BedrockFeatureRegistry::version() const {
    return version_;
}

const BedrockFeatureDefinition* BedrockFeatureRegistry::feature(
    std::string_view name
) const {
    const auto found = featureIndexByName_.find(std::string(name));
    return found == featureIndexByName_.end() ? nullptr : &features_[found->second];
}

ProtoDefValue BedrockFeatureRegistry::supportFeature(std::string_view name) const {
    const auto* found = feature(name);
    if (found == nullptr || !found->supported()) {
        return ProtoDefValue::boolean(false);
    }
    return found->value;
}

bool BedrockFeatureRegistry::supportsFeature(std::string_view name) const {
    return truthy(supportFeature(name));
}

std::optional<std::string_view> BedrockFeatureRegistry::featureString(
    std::string_view name
) const {
    const auto* found = feature(name);
    if (found == nullptr || !found->supported() ||
        found->value.kind != ProtoDefValue::Kind::String) {
        return std::nullopt;
    }
    return found->value.stringValue;
}

const std::vector<BedrockFeatureDefinition>& BedrockFeatureRegistry::all() const {
    return features_;
}

std::size_t BedrockFeatureRegistry::featureCount() const {
    return features_.size();
}

bool BedrockFeatureRegistry::isNewerThan(std::string_view otherVersion) const {
    return compare(otherVersion) > 0;
}

bool BedrockFeatureRegistry::isNewerOrEqualTo(
    std::string_view otherVersion
) const {
    return compare(otherVersion) >= 0;
}

bool BedrockFeatureRegistry::isOlderThan(std::string_view otherVersion) const {
    return compare(otherVersion) < 0;
}

bool BedrockFeatureRegistry::isOlderOrEqualTo(
    std::string_view otherVersion
) const {
    return compare(otherVersion) <= 0;
}

bool BedrockFeatureRegistry::isSameVersion(std::string_view otherVersion) const {
    return compare(otherVersion) == 0;
}

int BedrockFeatureRegistry::compare(std::string_view otherVersion) const {
    if (!hasVersion_) {
        throw std::runtime_error("Bedrock feature registry has no selected version");
    }
    const auto found = versionOrderByName_.find(std::string(otherVersion));
    if (found == versionOrderByName_.end()) {
        throw std::runtime_error(
            "Bedrock version not found in protocolVersions.json: " +
            std::string(otherVersion)
        );
    }
    if (versionOrder_ < found->second) return -1;
    if (versionOrder_ > found->second) return 1;
    return 0;
}

bool BedrockFeatureRegistry::truthy(const ProtoDefValue& value) {
    BedrockFeatureDefinition feature;
    feature.value = value;
    return feature.supported();
}

BedrockFeatureRegistry BedrockFeatureRegistryLoader::loadMinecraftData(
    const std::filesystem::path& featuresJson,
    const std::filesystem::path& protocolVersionsJson,
    MinecraftDataVersionInfo version
) {
    const auto versionsRoot = readJsonFile(protocolVersionsJson);
    requireKind(
        versionsRoot,
        JsonValue::Kind::Array,
        "bedrock/common/protocolVersions.json root"
    );

    struct VersionRecord {
        std::string minecraftVersion;
        std::string majorVersion;
        int64_t order = 0;
    };

    std::vector<VersionRecord> versionRecords;
    versionRecords.reserve(versionsRoot.arrayValue.size());
    for (std::size_t index = 0; index < versionsRoot.arrayValue.size(); ++index) {
        const auto& value = versionsRoot.arrayValue[index];
        VersionRecord record;
        record.minecraftVersion = stringValue(
            requireField(value, "minecraftVersion", "Bedrock protocol version"),
            "Bedrock protocol version.minecraftVersion"
        );
        record.majorVersion = stringValue(
            requireField(value, "majorVersion", "Bedrock protocol version"),
            "Bedrock protocol version.majorVersion"
        );
        if (const auto* dataVersion = optionalField(value, "dataVersion")) {
            record.order = integerValue(
                *dataVersion,
                "Bedrock protocol version.dataVersion"
            );
        } else {
            record.order = -static_cast<int64_t>(index);
        }
        versionRecords.push_back(std::move(record));
    }

    BedrockFeatureRegistry registry;
    registry.version_ = std::move(version);

    std::unordered_map<std::string, std::string> newestVersionByMajor;
    std::unordered_map<std::string, std::string> oldestVersionByMajor;
    for (const auto& record : versionRecords) {
        registry.versionOrderByName_.insert_or_assign(
            record.minecraftVersion,
            record.order
        );
        newestVersionByMajor.try_emplace(record.majorVersion, record.minecraftVersion);
        oldestVersionByMajor.insert_or_assign(record.majorVersion, record.minecraftVersion);
    }

    // minecraft-data Version accepts a major alias when an explicit .0
    // release exists, for example 1.20 means 1.20.0.
    for (const auto& record : versionRecords) {
        if (endsWith(record.minecraftVersion, ".0")) {
            registry.versionOrderByName_.insert_or_assign(
                record.majorVersion,
                record.order
            );
        }
    }

    const auto selected = registry.versionOrderByName_.find(
        registry.version_.minecraftVersion
    );
    if (selected == registry.versionOrderByName_.end()) {
        throw std::runtime_error(
            "selected Bedrock version is missing from protocolVersions.json: " +
            registry.version_.minecraftVersion
        );
    }
    registry.versionOrder_ = selected->second;
    registry.hasVersion_ = true;

    const auto resolveBoundary = [&](std::string boundary, bool minimum) {
        if (!endsWith(boundary, "_major")) return boundary;
        const auto major = boundary.substr(0, boundary.size() - 6);
        const auto& table = minimum ? oldestVersionByMajor : newestVersionByMajor;
        const auto found = table.find(major);
        if (found == table.end()) {
            throw std::runtime_error(
                "Bedrock major version not found in protocolVersions.json: " + major
            );
        }
        return found->second;
    };

    const auto versionInRange = [&](std::string minimum, std::string maximum) {
        minimum = resolveBoundary(std::move(minimum), true);
        if (maximum == "latest") {
            const auto found = registry.versionOrderByName_.find(minimum);
            if (found == registry.versionOrderByName_.end()) {
                throw std::runtime_error(
                    "Bedrock feature minimum version not found: " + minimum
                );
            }
            return registry.versionOrder_ >= found->second;
        }
        maximum = resolveBoundary(std::move(maximum), false);
        const auto minFound = registry.versionOrderByName_.find(minimum);
        const auto maxFound = registry.versionOrderByName_.find(maximum);
        if (minFound == registry.versionOrderByName_.end() ||
            maxFound == registry.versionOrderByName_.end()) {
            throw std::runtime_error(
                "Bedrock feature version range is missing from protocolVersions.json: " +
                minimum + ".." + maximum
            );
        }
        return registry.versionOrder_ >= minFound->second &&
            registry.versionOrder_ <= maxFound->second;
    };

    const auto featuresRoot = readJsonFile(featuresJson);
    requireKind(
        featuresRoot,
        JsonValue::Kind::Array,
        "bedrock/common/features.json root"
    );
    registry.features_.reserve(featuresRoot.arrayValue.size());

    for (const auto& value : featuresRoot.arrayValue) {
        BedrockFeatureDefinition feature;
        feature.name = stringValue(
            requireField(value, "name", "Bedrock feature"),
            "Bedrock feature.name"
        );
        feature.description = optionalString(
            optionalField(value, "description"),
            "Bedrock feature.description"
        ).value_or("");

        if (const auto* values = optionalField(value, "values")) {
            requireKind(*values, JsonValue::Kind::Array, "Bedrock feature.values");
            for (const auto& candidate : values->arrayValue) {
                bool matches = false;
                if (const auto exactVersion = optionalString(
                        optionalField(candidate, "version"),
                        "Bedrock feature value.version"
                    )) {
                    matches = versionInRange(*exactVersion, *exactVersion);
                } else {
                    const auto [minimum, maximum] = versionRange(
                        requireField(candidate, "versions", "Bedrock feature value"),
                        "Bedrock feature value.versions"
                    );
                    matches = versionInRange(minimum, maximum);
                }
                if (matches) {
                    feature.value = requireField(
                        candidate,
                        "value",
                        "Bedrock feature value"
                    );
                }
            }
        } else if (const auto exactVersion = optionalString(
                       optionalField(value, "version"),
                       "Bedrock feature.version"
                   )) {
            feature.value = ProtoDefValue::boolean(
                versionInRange(*exactVersion, *exactVersion)
            );
        } else {
            const auto [minimum, maximum] = versionRange(
                requireField(value, "versions", "Bedrock feature"),
                "Bedrock feature.versions"
            );
            feature.value = ProtoDefValue::boolean(
                versionInRange(minimum, maximum)
            );
        }

        const auto index = registry.features_.size();
        const auto name = feature.name;
        registry.features_.push_back(std::move(feature));
        registry.featureIndexByName_.insert_or_assign(name, index);
    }

    return registry;
}

} // namespace bedrock
