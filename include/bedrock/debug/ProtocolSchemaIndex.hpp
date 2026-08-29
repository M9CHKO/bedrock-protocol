#pragma once

#include <bedrock/world/MinecraftDataAssets.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

struct PacketFieldSchema {
    std::string name;
    std::string type;
};

struct PacketSchemaInfo {
    std::string packetName;
    std::vector<PacketFieldSchema> fields;
};

class ProtocolSchemaIndex {
public:
    explicit ProtocolSchemaIndex(std::filesystem::path protocolJsonPath) {
        std::ifstream file(protocolJsonPath);
        if (!file) {
            throw std::runtime_error("failed to open protocol.json: " + protocolJsonPath.string());
        }

        json_.assign(
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()
        );
    }

    static ProtocolSchemaIndex forVersion(const std::string& version) {
        MinecraftDataAssets assets;
        auto paths = assets.resolveByVersion(version);
        return ProtocolSchemaIndex(paths.protocolJson);
    }

    static ProtocolSchemaIndex forProtocol(int32_t protocol) {
        MinecraftDataAssets assets;
        auto paths = assets.resolveByProtocol(protocol);
        return ProtocolSchemaIndex(paths.protocolJson);
    }

    std::optional<PacketSchemaInfo> findPacket(const std::string& packetName) const {
        const std::string key = "\"packet_" + packetName + "\"";
        std::size_t searchFrom = 0;
        while (true) {
            const auto keyPos = json_.find(key, searchFrom);
            if (keyPos == std::string::npos) return std::nullopt;
            searchFrom = keyPos + key.size();

            // The same string also appears as a switch mapping value. Only a
            // JSON object key may introduce the packet schema.
            auto colon = searchFrom;
            while (colon < json_.size() &&
                   std::isspace(static_cast<unsigned char>(json_[colon]))) {
                ++colon;
            }
            if (colon >= json_.size() || json_[colon] != ':') continue;

            auto arrayStart = colon + 1;
            while (arrayStart < json_.size() &&
                   std::isspace(static_cast<unsigned char>(json_[arrayStart]))) {
                ++arrayStart;
            }
            if (arrayStart >= json_.size() || json_[arrayStart] != '[') {
                continue;
            }

            const auto arrayEnd = findMatchingBracket(arrayStart);
            if (arrayEnd == std::string::npos) return std::nullopt;

            PacketSchemaInfo out;
            out.packetName = packetName;
            const std::string array = json_.substr(
                arrayStart,
                arrayEnd - arrayStart + 1
            );
            out.fields = parseFields(array);
            if (!out.fields.empty()) return out;
        }
    }

private:
    std::string json_;

    std::size_t findMatchingBracket(std::size_t open) const {
        int depth = 0;
        bool inString = false;
        bool escaped = false;

        for (std::size_t i = open; i < json_.size(); ++i) {
            char c = json_[i];

            if (escaped) {
                escaped = false;
                continue;
            }

            if (c == '\\') {
                escaped = true;
                continue;
            }

            if (c == '"') {
                inString = !inString;
                continue;
            }

            if (inString) continue;

            if (c == '[') {
                ++depth;
            } else if (c == ']') {
                --depth;
                if (depth == 0) {
                    return i;
                }
            }
        }

        return std::string::npos;
    }

    static std::vector<PacketFieldSchema> parseFields(const std::string& array) {
        std::vector<PacketFieldSchema> fields;

        std::size_t pos = 0;

        while (true) {
            auto namePos = array.find("\"name\"", pos);
            if (namePos == std::string::npos) break;

            auto name = readStringValue(array, namePos);
            if (!name.has_value()) {
                pos = namePos + 6;
                continue;
            }

            auto typePos = array.find("\"type\"", namePos);
            if (typePos == std::string::npos) {
                pos = namePos + 6;
                continue;
            }

            auto type = readTypeValue(array, typePos).value_or("");

            fields.push_back(PacketFieldSchema{*name, type});
            pos = typePos + 6;
        }

        return fields;
    }

    static std::optional<std::string> readStringValue(
        const std::string& text,
        std::size_t keyPos
    ) {
        auto colon = text.find(':', keyPos);
        if (colon == std::string::npos) return std::nullopt;

        auto q1 = text.find('"', colon);
        if (q1 == std::string::npos) return std::nullopt;

        auto q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) return std::nullopt;

        return text.substr(q1 + 1, q2 - q1 - 1);
    }

    static std::optional<std::string> readTypeValue(
        const std::string& text,
        std::size_t keyPos
    ) {
        auto colon = text.find(':', keyPos);
        if (colon == std::string::npos) return std::nullopt;

        auto pos = colon + 1;
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }

        if (pos >= text.size()) return std::nullopt;

        if (text[pos] == '"') {
            auto q2 = text.find('"', pos + 1);
            if (q2 == std::string::npos) return std::nullopt;
            return text.substr(pos + 1, q2 - pos - 1);
        }

        if (text[pos] == '[') {
            int depth = 0;
            bool inString = false;
            bool escaped = false;

            for (std::size_t i = pos; i < text.size(); ++i) {
                char c = text[i];

                if (escaped) {
                    escaped = false;
                    continue;
                }

                if (c == '\\') {
                    escaped = true;
                    continue;
                }

                if (c == '"') {
                    inString = !inString;
                    continue;
                }

                if (inString) continue;

                if (c == '[') ++depth;
                else if (c == ']') {
                    --depth;
                    if (depth == 0) {
                        return text.substr(pos, i - pos + 1);
                    }
                }
            }
        }

        return std::nullopt;
    }
};

} // namespace bedrock
