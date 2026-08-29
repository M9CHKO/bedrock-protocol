#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bedrock {

// Kept in the same order as minecraft-data.versions.bedrock. JavaScript
// preserves this insertion order when it builds options.Versions.
struct BedrockVersionEntry {
    std::string_view minecraftVersion;
    uint32_t protocolVersion;
};

inline constexpr std::string_view MIN_VERSION = "1.16.201";
inline constexpr std::string_view CURRENT_VERSION = "1.26.20";

inline constexpr std::array<BedrockVersionEntry, 50> VERSION_ENTRIES {{
    {"1.26.20", 975u},
    {"1.26.10", 944u},
    {"1.26.0", 924u},
    {"1.21.130", 898u},
    {"1.21.124", 860u},
    {"1.21.120", 859u},
    {"1.21.111", 844u},
    {"1.21.100", 827u},
    {"1.21.93", 819u},
    {"1.21.90", 818u},
    {"1.21.80", 800u},
    {"1.21.70", 786u},
    {"1.21.60", 776u},
    {"1.21.50", 766u},
    {"1.21.42", 748u},
    {"1.21.30", 729u},
    {"1.21.20", 712u},
    {"1.21.2", 686u},
    {"1.21.0", 685u},
    {"1.20.80", 671u},
    {"1.20.71", 662u},
    {"1.20.61", 649u},
    {"1.20.50", 630u},
    {"1.20.40", 622u},
    {"1.20.30", 618u},
    {"1.20.15", 594u},
    {"1.20.10", 594u},
    {"1.20.0", 589u},
    {"1.19.80", 582u},
    {"1.19.70", 575u},
    {"1.19.63", 568u},
    {"1.19.62", 567u},
    {"1.19.60", 567u},
    {"1.19.50", 560u},
    {"1.19.40", 557u},
    {"1.19.30", 554u},
    {"1.19.21", 545u},
    {"1.19.20", 544u},
    {"1.19.10", 534u},
    {"1.19.1", 527u},
    {"1.18.30", 503u},
    {"1.18.11", 486u},
    {"1.18.0", 475u},
    {"1.17.40", 471u},
    {"1.17.30", 465u},
    {"1.17.10", 448u},
    {"1.17.0", 440u},
    {"1.16.220", 431u},
    {"1.16.210", 428u},
    {"1.16.201", 422u}
}};

// Runtime counterpart of src/options.js' Versions object.
inline const std::unordered_map<std::string, uint32_t> Versions = [] {
    std::unordered_map<std::string, uint32_t> versions;
    versions.reserve(VERSION_ENTRIES.size());
    for (const auto& entry : VERSION_ENTRIES) {
        versions.emplace(std::string(entry.minecraftVersion), entry.protocolVersion);
    }
    return versions;
}();

inline const BedrockVersionEntry* findVersion(std::string_view minecraftVersion) noexcept {
    for (const auto& entry : VERSION_ENTRIES) {
        if (entry.minecraftVersion == minecraftVersion) {
            return &entry;
        }
    }
    return nullptr;
}

inline uint32_t protocolVersionFor(std::string_view minecraftVersion) {
    const auto* entry = findVersion(minecraftVersion);
    if (!entry) {
        throw std::runtime_error("Unknown version: " + std::string(minecraftVersion));
    }
    return entry->protocolVersion;
}

inline uint32_t validateVersion(std::string_view minecraftVersion) {
    const auto* entry = findVersion(minecraftVersion);
    if (!entry) {
        throw std::runtime_error("Unsupported version " + std::string(minecraftVersion));
    }
    return entry->protocolVersion;
}

inline std::vector<std::string> supportedVersionNames() {
    std::vector<std::string> names;
    names.reserve(VERSION_ENTRIES.size());
    for (const auto& entry : VERSION_ENTRIES) {
        names.emplace_back(entry.minecraftVersion);
    }
    return names;
}

} // namespace bedrock
