#include <bedrock/client/BedrockNetworkClient.hpp>
#include <bedrock/protocol/VersionedPayloadReader.hpp>
#include <bedrock/relay/BedrockLiveRelay.hpp>
#include <bedrock/server/BedrockServer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

static bool checkPlayStatusEndianRegression() {
    const auto codec = bedrock::VersionedPacketCodec::forVersion("1.21.100");
    const auto loginSuccess = codec.makePacketByName(
        "play_status",
        {0x00, 0x00, 0x00, 0x00}
    );
    const auto playerSpawn = codec.makePacketByName(
        "play_status",
        {0x00, 0x00, 0x00, 0x03}
    );
    const auto littleEndianPlayerSpawn = codec.makePacketByName(
        "play_status",
        {0x03, 0x00, 0x00, 0x00}
    );

    const bool ok =
        loginSuccess.fullPacket == std::vector<uint8_t>({0x02, 0x00, 0x00, 0x00, 0x00}) &&
        playerSpawn.fullPacket == std::vector<uint8_t>({0x02, 0x00, 0x00, 0x00, 0x03}) &&
        bedrock::VersionedPayloadReader::readPlayStatus(loginSuccess).status == 0 &&
        bedrock::VersionedPayloadReader::readPlayStatus(playerSpawn).status == 3 &&
        bedrock::VersionedPayloadReader::readPlayStatus(littleEndianPlayerSpawn).status != 3;

    if (!ok) {
        std::cerr << "[LIVE-RELAY-SMOKE] play_status i32 big-endian regression\n";
    }
    return ok;
}

static bool checkVersion(const std::string& version) {
    std::atomic<bool> downstreamJoined {false};
    std::atomic<bool> upstreamReady {false};
    std::atomic<bool> gotRelayStatus {false};
    std::atomic<bool> gotError {false};

    bedrock::BedrockServer upstreamServer({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {{"motd", "Live Relay Smoke Upstream"}},
        .maxPlayers = 3,
        .offline = true
    });
    upstreamServer.listen();

    bedrock::BedrockLiveRelayOptions relayOptions;
    relayOptions.server.host = "127.0.0.1";
    relayOptions.server.port = 0;
    relayOptions.server.version = version;
    relayOptions.server.motd = {{"motd", "Live Relay Smoke"}};
    relayOptions.server.maxPlayers = 3;
    relayOptions.server.offline = true;
    relayOptions.upstream.host = "127.0.0.1";
    relayOptions.upstream.port = upstreamServer.boundPort();
    relayOptions.upstream.username = "RelaySmokeUp";
    relayOptions.upstream.version = version;
    relayOptions.upstream.offline = true;
    relayOptions.upstream.connectTimeoutMs = 1000;

    auto relay = bedrock::createRelayServer(std::move(relayOptions));
    relay.onJoin([&](const bedrock::BedrockServerConnection&) {
        downstreamJoined = true;
    });
    relay.onStatus([&](const bedrock::BedrockLiveRelayStatus& status) {
        gotRelayStatus = true;
        if (status.upstreamReady) {
            upstreamReady = true;
        }
    });
    relay.onError([&](const std::string& error) {
        gotError = true;
        std::cerr << "[LIVE-RELAY-SMOKE] " << version << " relay error: " << error << "\n";
    });
    relay.listen();

    auto downstreamClient = bedrock::createNetworkClient({
        .host = "127.0.0.1",
        .port = relay.boundPort(),
        .username = "RelaySmokeDown",
        .version = version,
        .offline = true,
        .connectTimeoutMs = 1000
    });
    downstreamClient.onError([&](const std::string& error) {
        gotError = true;
        std::cerr << "[LIVE-RELAY-SMOKE] " << version << " downstream error: " << error << "\n";
    });

    if (!downstreamClient.connect()) {
        std::cerr << "[LIVE-RELAY-SMOKE] " << version << " downstream connect failed\n";
        relay.close();
        upstreamServer.close();
        return false;
    }

    for (int i = 0; i < 150 &&
         (!downstreamJoined.load() || !upstreamReady.load() || !gotRelayStatus.load()) &&
         !gotError.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    downstreamClient.close();
    relay.close();
    upstreamServer.close();

    if (!downstreamJoined.load()) {
        std::cerr << "[LIVE-RELAY-SMOKE] " << version << " downstream did not join relay\n";
        return false;
    }
    if (!upstreamReady.load()) {
        std::cerr << "[LIVE-RELAY-SMOKE] " << version << " upstream did not become ready\n";
        return false;
    }
    if (gotError.load()) {
        return false;
    }

    std::cout << "[LIVE-RELAY-SMOKE] " << version << " ok\n";
    return true;
}

int main() {
    bool ok = checkPlayStatusEndianRegression();
    ok = checkVersion("1.20.40") && ok;
    ok = checkVersion("1.20.50") && ok;
    ok = checkVersion("1.21.100") && ok;
    return ok ? 0 : 1;
}
