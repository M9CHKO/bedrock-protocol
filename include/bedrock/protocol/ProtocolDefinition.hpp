#pragma once

#include <bedrock/Options.hpp>
#include <bedrock/protocol/GeneratedProtocolRegistry.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

class ProtocolDefinition {
public:
    explicit ProtocolDefinition(const ProtocolVersionInfo& info)
        : info_(&info) {}

    static ProtocolDefinition forVersion(const std::string& minecraftVersion) {
        // Generated tables can contain development snapshots and historical
        // schemas. The public protocol contract follows options.Versions from
        // the installed minecraft-data release exactly.
        if (!findVersion(minecraftVersion)) {
            throw std::runtime_error("unsupported protocol version: " + minecraftVersion);
        }
        const auto* info = GeneratedProtocolRegistry::get(minecraftVersion);
        if (!info) {
            throw std::runtime_error("unsupported protocol version: " + minecraftVersion);
        }
        return ProtocolDefinition(*info);
    }

    static bool supportsVersion(const std::string& minecraftVersion) {
        return findVersion(minecraftVersion) != nullptr &&
            GeneratedProtocolRegistry::get(minecraftVersion) != nullptr;
    }

    static std::vector<std::string> versions() {
        return supportedVersionNames();
    }

    const char* minecraftVersion() const {
        return info_->minecraftVersion;
    }

    uint32_t protocolVersion() const {
        return info_->protocolVersion;
    }

    std::size_t packetCount() const {
        return info_->packetCount;
    }

    const ProtocolPacketInfo* packetById(uint32_t packetId) const {
        return GeneratedProtocolRegistry::packetById(*info_, packetId);
    }

    const ProtocolPacketInfo* packetByName(const std::string& packetName) const {
        return GeneratedProtocolRegistry::packetByName(*info_, packetName);
    }

    bool hasPacket(uint32_t packetId) const {
        return packetById(packetId) != nullptr;
    }

    bool hasPacket(const std::string& packetName) const {
        return packetByName(packetName) != nullptr;
    }

    std::string packetName(uint32_t packetId) const {
        const auto* packet = packetById(packetId);
        if (!packet) {
            return "unknown_" + std::to_string(packetId);
        }
        return packet->name;
    }

    uint32_t packetId(const std::string& packetName) const {
        const auto* packet = packetByName(packetName);
        if (!packet) {
            throw std::runtime_error("unknown packet name for version " + std::string(info_->minecraftVersion) + ": " + packetName);
        }
        return packet->id;
    }

    std::string packetParamsType(uint32_t packetId) const {
        const auto* packet = packetById(packetId);
        if (!packet) {
            return "";
        }
        return packet->paramsType ? packet->paramsType : "";
    }

    std::string packetParamsType(const std::string& packetName) const {
        const auto* packet = packetByName(packetName);
        if (!packet) {
            return "";
        }
        return packet->paramsType ? packet->paramsType : "";
    }

    std::vector<ProtocolPacketInfo> packets() const {
        std::vector<ProtocolPacketInfo> out;
        out.reserve(info_->packetCount);

        for (std::size_t i = 0; i < info_->packetCount; ++i) {
            out.push_back(info_->packets[i]);
        }

        return out;
    }

private:
    const ProtocolVersionInfo* info_;
};

} // namespace bedrock
