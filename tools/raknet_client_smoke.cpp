#include <bedrock/client/RakNetClient.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/server/BedrockServer.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
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
        .timeoutMs = 1000,
        .reliabilityTimeoutMs = 5000
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

    std::atomic<bool> failureConnected {false};
    std::atomic<int> failureCloses {0};
    std::mutex failureMutex;
    std::string failureReason;
    bedrock::RakNetClient failureClient({
        .host = "127.0.0.1",
        .port = server.boundPort(),
        .mtu = 1400,
        .protocolVersion = 11,
        .timeoutMs = 1000,
        .reliabilityTimeoutMs = 5000
    });
    failureClient.onConnected([&]() {
        failureConnected = true;
        auto request = codec.packetCodec().makePacketByName(
            "request_network_settings",
            {0x00, 0x00, 0x02, 0x6e}
        );
        failureClient.sendReliable(codec.encodeMcpePayload(
            {request},
            bedrock::VersionedMcpeCompression::Uncompressed
        ));
    });
    failureClient.onEncapsulated([](const std::vector<uint8_t>&) {
        throw std::runtime_error("client live callback boom");
    });
    failureClient.onClose([&](const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(failureMutex);
            failureReason = reason;
        }
        ++failureCloses;
    });
    if (!failureClient.connect()) {
        std::cerr << "[CLIENT-SMOKE] failure-boundary connect failed: "
                  << failureClient.error() << "\n";
        server.close();
        return 1;
    }
    for (int i = 0; i < 100 && failureCloses.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::string capturedFailureReason;
    {
        std::lock_guard<std::mutex> lock(failureMutex);
        capturedFailureReason = failureReason;
    }
    const std::string expectedFailure =
        "RakNet worker callback failure: client live callback boom";
    if (!failureConnected.load() || failureCloses.load() != 1 ||
        failureClient.connected() || failureClient.error() != expectedFailure ||
        capturedFailureReason != expectedFailure) {
        std::cerr << "[CLIENT-SMOKE] worker exception was not contained: "
                  << "connected=" << failureConnected.load()
                  << " close=" << failureCloses.load()
                  << " active=" << failureClient.connected()
                  << " error=" << failureClient.error()
                  << " reason=" << capturedFailureReason << "\n";
        failureClient.close();
        server.close();
        return 1;
    }
    failureClient.close();

    std::atomic<int> connectCallbackCloses {0};
    std::atomic<bool> connectCallbackEntered {false};
    std::atomic<bool> releaseConnectCallback {false};
    std::atomic<bool> connectCallbackReturned {false};
    std::atomic<bool> connectCallbackResult {true};
    std::mutex connectCallbackMutex;
    std::string connectCallbackReason;
    bedrock::RakNetClient connectCallbackClient({
        .host = "127.0.0.1",
        .port = server.boundPort(),
        .mtu = 1400,
        .protocolVersion = 11,
        .timeoutMs = 1000,
        .reliabilityTimeoutMs = 5000
    });
    connectCallbackClient.onConnected([&]() {
        connectCallbackEntered = true;
        while (!releaseConnectCallback.load()) std::this_thread::yield();
        throw std::runtime_error("connected callback boom");
    });
    connectCallbackClient.onClose([&](const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(connectCallbackMutex);
            connectCallbackReason = reason;
        }
        ++connectCallbackCloses;
    });
    std::thread connectCallbackThread([&]() {
        connectCallbackResult = connectCallbackClient.connect();
        connectCallbackReturned = true;
    });
    for (int attempt = 0;
         attempt < 200 && !connectCallbackEntered.load();
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool returnedBeforeCallback = connectCallbackReturned.load();
    releaseConnectCallback = true;
    connectCallbackThread.join();
    std::string capturedConnectCallbackReason;
    {
        std::lock_guard<std::mutex> lock(connectCallbackMutex);
        capturedConnectCallbackReason = connectCallbackReason;
    }
    const std::string expectedConnectCallbackFailure =
        "RakNet worker callback failure: connected callback boom";
    if (!connectCallbackEntered.load() || returnedBeforeCallback ||
        connectCallbackResult.load() || connectCallbackCloses.load() != 1 ||
        connectCallbackClient.connected() ||
        connectCallbackClient.error() != expectedConnectCallbackFailure ||
        capturedConnectCallbackReason != expectedConnectCallbackFailure) {
        std::cerr << "[CLIENT-SMOKE] connected callback failure was accepted: "
                  << "entered=" << connectCallbackEntered.load()
                  << " early=" << returnedBeforeCallback
                  << " result=" << connectCallbackResult.load()
                  << " close=" << connectCallbackCloses.load()
                  << " active=" << connectCallbackClient.connected()
                  << " error=" << connectCallbackClient.error()
                  << " reason=" << capturedConnectCallbackReason << "\n";
        connectCallbackClient.close();
        server.close();
        return 1;
    }
    connectCallbackClient.close();
    server.close();
    std::cout << "[CLIENT-SMOKE] ok\n";
    return 0;
}
