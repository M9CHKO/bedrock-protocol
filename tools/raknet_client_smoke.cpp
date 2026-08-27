#include <bedrock/client/RakNetClient.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/server/BedrockServer.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    std::atomic<bool> connected {false};
    std::atomic<bool> gotNetworkSettings {false};
    std::atomic<int> serverDisconnects {0};

    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.20.40",
        .motd = {{"motd", "RakNet Client Smoke"}},
        .maxPlayers = 3
    });
    server.onDisconnect([&](const bedrock::BedrockServerConnection&) {
        ++serverDisconnects;
    });
    server.listen();

    bedrock::RakNetClient client({
        // raknet-native maps ::1 to IPv4 loopback for its AF_INET client.
        .host = "::1",
        .port = server.boundPort(),
        .mtu = 1400,
        .protocolVersion = 11,
        .timeoutMs = 1000
    });

    auto codec = bedrock::VersionedMcpeCodec::forVersion("1.20.40");

    client.onConnected([&]() {
        connected = true;
        auto request = codec.packetCodec().makePacketByName(
            "request_network_settings",
            {0x00, 0x00, 0x02, 0x6e}
        );
        client.sendReliable(codec.encodeMcpePayload(
            {request},
            bedrock::VersionedMcpeCompression::Uncompressed
        ));
    });

    client.onEncapsulated([&](const std::vector<uint8_t>& payload) {
        if (payload.empty() || payload[0] != 0xfe) {
            return;
        }

        auto decoded = codec.decodeMcpePayload(payload);
        for (const auto& packet : decoded.batch.packets) {
            if (packet.name == "network_settings") {
                gotNetworkSettings = true;
            }
        }
    });

    if (!client.connect()) {
        std::cerr << "[CLIENT-SMOKE] connect failed: " << client.error() << "\n";
        server.close();
        return 1;
    }

    for (int i = 0; i < 50 && !gotNetworkSettings.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!connected.load()) {
        std::cerr << "[CLIENT-SMOKE] did not emit connected\n";
        client.close();
        server.close();
        return 1;
    }

    if (!gotNetworkSettings.load()) {
        std::cerr << "[CLIENT-SMOKE] did not receive network_settings\n";
        client.close();
        server.close();
        return 1;
    }

    client.close();
    for (int i = 0;
         i < 50 &&
         (serverDisconnects.load() != 1 || server.clientCount() != 0);
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (serverDisconnects.load() != 1 || server.clientCount() != 0) {
        std::cerr << "[CLIENT-SMOKE] close did not notify the RakNet peer\n";
        server.close();
        return 1;
    }
    server.close();
    std::cout << "[CLIENT-SMOKE] ok\n";
    return 0;
}
