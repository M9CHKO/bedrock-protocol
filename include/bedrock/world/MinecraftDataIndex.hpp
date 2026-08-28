#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

struct MinecraftDataVersionInfo {
    std::string directory;
    std::string minecraftVersion;
    int32_t protocol = -1;
    std::string majorVersion;
    std::string releaseType;
};

class MinecraftDataIndex {
public:
    explicit MinecraftDataIndex(std::filesystem::path basePath = "data/minecraft-data/bedrock")
        : basePath_(std::move(basePath)) {
        loadManifest();
        parseManifest();
    }

    const std::filesystem::path& basePath() const {
        return basePath_;
    }

    const std::vector<MinecraftDataVersionInfo>& versions() const {
        return versions_;
    }

    std::filesystem::path versionPath(const std::string& directory) const {
        return basePath_ / directory;
    }

    std::filesystem::path protocolPath(const std::string& directory) const {
        return versionPath(directory) / "protocol.json";
    }

    std::filesystem::path blocksPath(const std::string& directory) const {
        return versionPath(directory) / "blocks.json";
    }

    std::filesystem::path itemsPath(const std::string& directory) const {
        return versionPath(directory) / "items.json";
    }

    std::optional<MinecraftDataVersionInfo> findByMinecraftVersion(const std::string& version) const {
        for (const auto& info : versions_) {
            if (info.minecraftVersion == version || info.directory == version) {
                return info;
            }
        }

        return std::nullopt;
    }

    std::optional<MinecraftDataVersionInfo> findByProtocol(int32_t protocol) const {
        std::optional<MinecraftDataVersionInfo> result;
        for (const auto& info : versions_) {
            if (info.protocol == protocol) {
                // The manifest is oldest-to-newest. minecraft-data resolves a
                // duplicate Bedrock protocol to the newest non-snapshot
                // release (for example 594 -> 1.20.15), so keep the last hit.
                result = info;
            }
        }
        return result;
    }

private:
    std::filesystem::path basePath_;
    std::string manifest_;
    std::vector<MinecraftDataVersionInfo> versions_;

    void loadManifest() {
        auto path = basePath_ / "manifest.json";

        std::ifstream file(path);
        if (!file) {
            throw std::runtime_error("failed to open minecraft-data manifest: " + path.string());
        }

        manifest_.assign(
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()
        );
    }

    static std::optional<std::string> readStringField(
        const std::string& object,
        const std::string& key
    ) {
        const std::string marker = "\"" + key + "\"";
        auto pos = object.find(marker);
        if (pos == std::string::npos) return std::nullopt;

        pos = object.find(':', pos);
        if (pos == std::string::npos) return std::nullopt;

        pos = object.find('"', pos);
        if (pos == std::string::npos) return std::nullopt;

        auto end = object.find('"', pos + 1);
        if (end == std::string::npos) return std::nullopt;

        return object.substr(pos + 1, end - pos - 1);
    }

    static std::optional<int32_t> readIntField(
        const std::string& object,
        const std::string& key
    ) {
        const std::string marker = "\"" + key + "\"";
        auto pos = object.find(marker);
        if (pos == std::string::npos) return std::nullopt;

        pos = object.find(':', pos);
        if (pos == std::string::npos) return std::nullopt;

        ++pos;
        while (pos < object.size() && (object[pos] == ' ' || object[pos] == '\n' || object[pos] == '\r' || object[pos] == '\t')) {
            ++pos;
        }

        auto end = pos;
        while (end < object.size() && object[end] >= '0' && object[end] <= '9') {
            ++end;
        }

        if (end == pos) return std::nullopt;

        return std::stoi(object.substr(pos, end - pos));
    }

    void parseManifest() {
        versions_.clear();

        std::size_t searchPos = 0;

        while (true) {
            auto dirPos = manifest_.find("\"directory\"", searchPos);
            if (dirPos == std::string::npos) {
                break;
            }

            auto objStart = manifest_.rfind('{', dirPos);
            auto objEnd = manifest_.find('}', dirPos);

            if (objStart == std::string::npos || objEnd == std::string::npos || objEnd <= objStart) {
                break;
            }

            std::string object = manifest_.substr(objStart, objEnd - objStart + 1);

            MinecraftDataVersionInfo info;

            auto directory = readStringField(object, "directory");
            auto minecraftVersion = readStringField(object, "minecraftVersion");
            auto protocol = readIntField(object, "protocol");

            if (directory && minecraftVersion && protocol) {
                info.directory = *directory;
                info.minecraftVersion = *minecraftVersion;
                info.protocol = *protocol;

                auto majorVersion = readStringField(object, "majorVersion");
                auto releaseType = readStringField(object, "releaseType");

                if (majorVersion) info.majorVersion = *majorVersion;
                if (releaseType) info.releaseType = *releaseType;

                versions_.push_back(info);
            }

            searchPos = objEnd + 1;
        }
    }
};

} // namespace bedrock
