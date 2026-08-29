#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bedrock {

inline constexpr std::string_view GENERATED_PROTOCOL_TYPES_VERSION = "1.26.20";

std::optional<std::string> generatedProtocolTypeJson(
    const std::string& version,
    const std::string& name
);

// Compatibility lookup for callers that explicitly want the latest schema.
std::optional<std::string> generatedProtocolTypeJson(const std::string& name);

std::vector<std::string> generatedProtocolTypeVersions();

} // namespace bedrock
