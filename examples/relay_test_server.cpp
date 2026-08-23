#include <bedrock/bedrock.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

struct RelayTestSettings {
    std::string version = "1.20.40";

    std::string listenHost = "0.0.0.0";
    uint16_t listenPort = 19132;
    std::string motd = "Bedrock Protocol C++ Relay";
    int maxPlayers = 3;

    std::string upstreamHost = "cpe.ign.gg";
    uint16_t upstreamPort = 19132;
    std::string upstreamUsername = "RelayBot";
    std::string upstreamProfile = "RelayBot";
    bool upstreamOffline = false;
    bool interactiveAuth = true;

    bool printPackets = true;
    bool clientCacheEnabled = false;
    int32_t chunkRadius = 20;
};

RelayTestSettings relaySettings() {
    RelayTestSettings settings;

    // Change these values and rebuild/run relay-test-server.
    settings.version = "1.20.40";
    settings.listenHost = "0.0.0.0";
    settings.listenPort = 19132;
    settings.upstreamHost = "cpe.ign.gg";
    settings.upstreamPort = 19132;
    settings.upstreamUsername = "StewedV";
    settings.upstreamProfile = "StewedV";
    settings.upstreamOffline = false;
    settings.interactiveAuth = true;

    return settings;
}

const char* directionName(bedrock::BedrockRelayDirection direction) {
    return direction == bedrock::BedrockRelayDirection::Clientbound
        ? "upstream -> client"
        : "client -> upstream";
}

} // namespace

int main() {
    const auto settings = relaySettings();

    bedrock::BedrockLiveRelayOptions options;
    options.server.host = settings.listenHost;
    options.server.port = settings.listenPort;
    options.server.version = settings.version;
    options.server.motd = {{"motd", settings.motd}};
    options.server.maxPlayers = settings.maxPlayers;

    options.upstream.host = settings.upstreamHost;
    options.upstream.port = settings.upstreamPort;
    options.upstream.username = settings.upstreamUsername;
    options.upstream.profile = settings.upstreamProfile;
    options.upstream.version = settings.version;
    options.upstream.offline = settings.upstreamOffline;
    options.upstream.interactiveAuth = settings.interactiveAuth;
    options.upstream.clientCacheEnabled = settings.clientCacheEnabled;
    options.upstream.trackWorld = true;
    options.upstream.chunkRadius = settings.chunkRadius;

    auto relay = bedrock::createRelayServer(std::move(options));

    relay.onConnect([](const bedrock::BedrockServerConnection& connection) {
        std::cout << "[downstream] connect "
                  << connection.address << ":" << connection.port << "\n";
    });

    relay.onJoin([](const bedrock::BedrockServerConnection& connection) {
        std::cout << "[downstream] joined "
                  << connection.address << ":" << connection.port << "\n";
    });

    relay.onError([](const std::string& message) {
        std::cerr << "[relay] " << message << "\n";
    });

    relay.onStatus([](const bedrock::BedrockLiveRelayStatus& status) {
        std::cout << "[relay] listening=" << status.listening
                  << " downstream=" << status.downstreamJoined
                  << " upstream_started=" << status.upstreamStarted
                  << " upstream_ready=" << status.upstreamReady
                  << " port=" << status.boundPort << "\n";
    });

    relay.on("serverbound", [&](bedrock::BedrockRelayPacketEvent& event) {
        if (settings.printPackets) {
            std::cout << "[" << directionName(event.direction) << "] "
                      << event.packet.name << "\n";
        }
    });

    relay.on("clientbound", [&](bedrock::BedrockRelayPacketEvent& event) {
        if (settings.printPackets) {
            std::cout << "[" << directionName(event.direction) << "] "
                      << event.packet.name << "\n";
        }
    });

    std::cout << "Relay listener: " << settings.listenHost << ":" << settings.listenPort << "\n";
    std::cout << "Upstream: " << settings.upstreamHost << ":" << settings.upstreamPort
              << " version=" << settings.version << "\n";
    std::cout << "Join this relay from Minecraft, then watch packet logs here.\n";

    relay.listen();

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
