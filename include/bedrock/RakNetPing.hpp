#pragma once

#include <bedrock/server/ServerAdvertisement.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

class RakTimeout : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct RakNetPongInfo {
    bool ok = false;
    bool timedOut = false;

    std::string host;
    uint16_t port = 19132;

    int64_t pingTime = 0;
    uint64_t serverGuid = 0;

    std::string rawMotd;
    std::optional<ServerAdvertisement> advertisement;

    std::string edition;
    std::string motd;
    int protocolVersion = -1;
    std::string gameVersion;
    int onlinePlayers = -1;
    int maxPlayers = -1;
    std::string serverId;
    std::string subMotd;
    std::string gameMode;
    int gameModeNumeric = -1;
    int ipv4Port = -1;
    int ipv6Port = -1;

    std::string error;
};

class RakNetPinger {
public:
    static RakNetPongInfo ping(
        const std::string& host,
        uint16_t port,
        int timeoutMs = 3000
    );

private:
    static std::vector<uint8_t> buildUnconnectedPing();
    static RakNetPongInfo parseUnconnectedPong(
        const std::string& host,
        uint16_t port,
        const std::vector<uint8_t>& data
    );
};

} // namespace bedrock
