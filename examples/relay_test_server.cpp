#include <bedrock/bedrock.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

const char* directionName(bedrock::BedrockRelayDirection direction) {
    return direction == bedrock::BedrockRelayDirection::Clientbound
        ? "upstream -> client"
        : "client -> upstream";
}

} // namespace

int main() {
    // One options object, matching `new Relay(options)` in bedrock-protocol.
    // offline applies to both sides; destination.offline is only needed when
    // the upstream server deliberately uses a different auth mode.
    bedrock::RelayOptions options {
        .version = "1.20.40",
        .host = "0.0.0.0",
        .port = 19132,
        .motd = "Bedrock Protocol C++ Relay",
        .offline = false,
        .maxPlayers = 3,
        .enableChunkCaching = false,
        .destination = {
            .host = "cpe.ign.gg",
            .port = 19132
        }
    };

    const bool printPackets = true;
    const auto upstreamHost = options.destination.host;
    const auto upstreamPort = options.destination.port;
    const auto version = options.version;

    bedrock::Relay relay(std::move(options));

    relay.onConnect([](bedrock::RelayPlayer& player) {
        std::cout << "[downstream] connect "
                  << player.connection.address << ":"
                  << player.connection.port << "\n";
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

    relay.on("serverbound", [&](bedrock::RelayPacketEvent& event) {
        if (printPackets) {
            std::cout << "[" << directionName(event.direction) << "] "
                      << event.name << "\n";
        }
    });

    relay.on("clientbound", [&](bedrock::RelayPacketEvent& event) {
        if (printPackets) {
            std::cout << "[" << directionName(event.direction) << "] "
                      << event.name << "\n";
        }
    });

    const auto listener = relay.listen();
    std::cout << "Relay listener: " << listener.host << ":" << listener.port
              << "\n";
    std::cout << "Upstream: " << upstreamHost << ":" << upstreamPort
              << " version=" << version << "\n";
    std::cout << "Join this relay from Minecraft, then watch packet logs here.\n";

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
