#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

namespace bedrock {

struct MinecraftDataPathSet {
    std::string version;
    std::string blocks;
    std::string blockStates;
    std::string blockCollisionShapes;
    std::string biomes;
    std::string entities;
    std::string enchantments;
    std::string items;
    std::string recipes;
    std::string windows;
    std::string instruments;
    std::string attributes;
    std::string blockLoot;
    std::string entityLoot;
    std::string protocol;
    std::string proto;
    std::string types;
    std::string language;
    std::string steve;
};

class MinecraftDataPathResolver {
public:
    explicit MinecraftDataPathResolver(
        std::filesystem::path path = "data/minecraft-data/dataPaths.json"
    ) {
        std::ifstream file(path);
        if (!file) {
            throw std::runtime_error("failed to open dataPaths.json: " + path.string());
        }

        json_.assign(
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()
        );
    }

    std::optional<MinecraftDataPathSet> findBedrock(const std::string& version) const {
        const std::string bedrockKey = "\"bedrock\"";
        auto bedrockPos = json_.find(bedrockKey);
        if (bedrockPos == std::string::npos) {
            return std::nullopt;
        }

        const std::string versionKey = "\"" + version + "\"";
        auto versionPos = json_.find(versionKey, bedrockPos);
        if (versionPos == std::string::npos) {
            return std::nullopt;
        }

        auto objStart = json_.find('{', versionPos);
        if (objStart == std::string::npos) {
            return std::nullopt;
        }

        auto objEnd = findMatchingBrace(objStart);
        if (objEnd == std::string::npos) {
            return std::nullopt;
        }

        std::string obj = json_.substr(objStart, objEnd - objStart + 1);

        MinecraftDataPathSet out;
        out.version = version;
        out.blocks = stripEdition(readString(obj, "blocks").value_or("bedrock/" + version));
        // These files did not exist for legacy numeric-id Bedrock chunks.
        // Keep an absent category absent instead of guessing blocks.json's
        // directory and manufacturing a path to a non-existent file.
        out.blockStates = stripEdition(readString(obj, "blockStates").value_or(""));
        out.blockCollisionShapes = stripEdition(
            readString(obj, "blockCollisionShapes").value_or("")
        );
        out.biomes = stripEdition(readString(obj, "biomes").value_or("bedrock/" + version));
        out.entities = stripEdition(readString(obj, "entities").value_or("bedrock/" + version));
        out.enchantments = stripEdition(
            readString(obj, "enchantments").value_or("bedrock/" + version)
        );
        out.items = stripEdition(readString(obj, "items").value_or("bedrock/" + version));
        // These gameplay tables are optional in minecraft-data. An absent
        // remap means that the category is unavailable for this version; it
        // must not fall back to a non-existent file in the current directory.
        out.recipes = stripEdition(readString(obj, "recipes").value_or(""));
        out.windows = stripEdition(readString(obj, "windows").value_or(""));
        out.instruments = stripEdition(readString(obj, "instruments").value_or(""));
        out.attributes = stripEdition(readString(obj, "attributes").value_or(""));
        out.blockLoot = stripEdition(readString(obj, "blockLoot").value_or(""));
        out.entityLoot = stripEdition(readString(obj, "entityLoot").value_or(""));
        out.protocol = stripEdition(readString(obj, "protocol").value_or("bedrock/" + version));
        out.proto = stripEdition(readString(obj, "proto").value_or(out.protocol));
        out.types = stripEdition(readString(obj, "types").value_or(out.protocol));
        out.language = stripEdition(readString(obj, "language").value_or("bedrock/" + version));
        // Like loot and the optional gameplay tables, defaultSkin is absent
        // when dataPaths does not explicitly map a steve.json dataset.
        out.steve = stripEdition(readString(obj, "steve").value_or(""));

        return out;
    }

private:
    std::string json_;

    std::size_t findMatchingBrace(std::size_t open) const {
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

            if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0) return i;
            }
        }

        return std::string::npos;
    }

    static std::optional<std::string> readString(
        const std::string& obj,
        const std::string& key
    ) {
        const std::string marker = "\"" + key + "\"";
        auto pos = obj.find(marker);
        if (pos == std::string::npos) return std::nullopt;

        pos = obj.find(':', pos);
        if (pos == std::string::npos) return std::nullopt;

        auto q1 = obj.find('"', pos);
        if (q1 == std::string::npos) return std::nullopt;

        auto q2 = obj.find('"', q1 + 1);
        if (q2 == std::string::npos) return std::nullopt;

        return obj.substr(q1 + 1, q2 - q1 - 1);
    }

    static std::string stripEdition(std::string value) {
        const std::string prefix = "bedrock/";
        if (value.rfind(prefix, 0) == 0) {
            return value.substr(prefix.size());
        }

        // dataPaths contains a few historical cross-edition fallbacks. They
        // are deliberately unavailable in this Bedrock-only library.
        if (value.rfind("pc/", 0) == 0 || value.rfind("java/", 0) == 0) {
            return {};
        }

        return value;
    }
};

} // namespace bedrock
