#include <bedrock/client/BedrockNetworkClient.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/server/BedrockServer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bedrock {

struct BedrockNetworkClientTestAccess {
    static uint64_t receiveCounter(const BedrockNetworkClient& client) {
        return client.receiveCounter_;
    }
};

} // namespace bedrock

namespace {

bedrock::ProtoDefValue tickValue(int64_t request, int64_t response) {
    return bedrock::ProtoDefValue::object({
        {"request_time", bedrock::ProtoDefValue::integer(request)},
        {"response_time", bedrock::ProtoDefValue::integer(response)}
    });
}

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[SERVER-OUTBOUND-QUEUE-SMOKE] " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    constexpr auto missingCounter = std::numeric_limits<uint64_t>::max();
    const std::string version = "1.20.40";
    const auto codec = bedrock::VersionedMcpeCodec::forVersion(version);
    const bedrock::ProtoDefPacketEncoder encoder(version);

    auto makeTickPacket = [&](int64_t request, int64_t response) {
        return codec.packetCodec().makePacketByName(
            "tick_sync",
            encoder.encodePacket("tick_sync", tickValue(request, response))
        );
    };

    const std::vector<bedrock::VersionedGamePacket> expected {
        makeTickPacket(1, 101),
        makeTickPacket(2, 102),
        makeTickPacket(3, 103),
        makeTickPacket(4, 104),
        makeTickPacket(5, 105)
    };

    std::mutex errorMutex;
    std::string serverError;
    std::string clientError;
    std::atomic<bool> serverJoined {false};

    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {{"motd", "Server Outbound Queue Smoke"}},
        .maxPlayers = 1,
        .offline = true,
        // Keep the first timer tick well outside the synchronous onJoin
        // callback so this test deterministically separates manual and timed
        // flush boundaries even though the C++ scheduler owns its own thread.
        .batchingInterval = 1000
    });
    server.onJoin([&](const bedrock::BedrockServerConnection& connection) {
        serverJoined = true;
        try {
            // Immediate Connection#write boundary.
            server.write(connection, "tick_sync", tickValue(1, 101));

            // Three different queue admission surfaces must become one batch.
            server.queue(connection, "tick_sync", tickValue(2, 102));
            server.sendBuffer(connection, expected[2].fullPacket);
            server.queuePacket(connection, expected[3]);
            server.sendQueued(connection);

            // Leave one packet for the periodic Connection#_tick equivalent.
            server.queue(connection, "tick_sync", tickValue(5, 105));
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(errorMutex);
            serverError = error.what();
        }
    });
    server.listen();

    auto client = bedrock::createNetworkClient({
        .host = "127.0.0.1",
        .port = server.boundPort(),
        .username = "QueueSmoke",
        .version = version,
        .offline = true,
        .connectTimeoutMs = 1000
    });

    std::atomic<bool> clientJoined {false};
    std::atomic<uint64_t> loginSuccessCounter {missingCounter};
    std::atomic<uint64_t> immediateCounter {missingCounter};
    std::atomic<uint64_t> manualBatchCounter {missingCounter};
    std::atomic<uint64_t> timerBatchCounter {missingCounter};
    std::atomic<bool> packetsComplete {false};
    std::mutex packetsMutex;
    std::vector<std::vector<uint8_t>> received;

    client.onJoin([&]() {
        clientJoined = true;
    });
    client.on("play_status", [&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        if (event.packet.payload == std::vector<uint8_t>({0x00, 0x00, 0x00, 0x00})) {
            loginSuccessCounter =
                bedrock::BedrockNetworkClientTestAccess::receiveCounter(client);
        }
    });
    client.on("tick_sync", [&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        std::size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(packetsMutex);
            received.push_back(event.packet.fullPacket);
            count = received.size();
        }
        const auto counter =
            bedrock::BedrockNetworkClientTestAccess::receiveCounter(client);
        if (count == 1) immediateCounter = counter;
        if (count == 4) manualBatchCounter = counter;
        if (count == 5) {
            timerBatchCounter = counter;
            packetsComplete = true;
        }
    });
    client.onError([&](const std::string& error) {
        std::lock_guard<std::mutex> lock(errorMutex);
        clientError = error;
    });

    bool connected = client.connect();
    for (int attempt = 0; attempt < 250; ++attempt) {
        if (packetsComplete.load()) break;
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            if (!serverError.empty() || !clientError.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    client.close("queue smoke complete");
    server.close();

    std::vector<std::vector<uint8_t>> expectedBuffers;
    expectedBuffers.reserve(expected.size());
    for (const auto& packet : expected) {
        expectedBuffers.push_back(packet.fullPacket);
    }

    std::vector<std::vector<uint8_t>> receivedSnapshot;
    {
        std::lock_guard<std::mutex> lock(packetsMutex);
        receivedSnapshot = received;
    }
    std::string serverErrorSnapshot;
    std::string clientErrorSnapshot;
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        serverErrorSnapshot = serverError;
        clientErrorSnapshot = clientError;
    }

    bool ok = true;
    ok &= check(connected, "client connect failed");
    ok &= check(serverJoined.load() && clientJoined.load(), "join lifecycle did not complete");
    ok &= check(serverErrorSnapshot.empty(), "server queue threw: " + serverErrorSnapshot);
    ok &= check(clientErrorSnapshot.empty(), "client decode failed: " + clientErrorSnapshot);
    ok &= check(receivedSnapshot == expectedBuffers, "packet order or raw buffer changed");

    const auto loginCounter = loginSuccessCounter.load();
    ok &= check(loginCounter != missingCounter, "login_success batch was not observed");
    ok &= check(
        immediateCounter.load() == loginCounter + 1,
        "write() was not its own immediate encrypted batch"
    );
    ok &= check(
        manualBatchCounter.load() == loginCounter + 2,
        "queue/sendBuffer/queuePacket did not share one manual batch"
    );
    ok &= check(
        timerBatchCounter.load() == loginCounter + 3,
        "periodic queue timer did not flush exactly one later batch"
    );

    if (!ok) return 1;
    std::cout << "[SERVER-OUTBOUND-QUEUE-SMOKE] ok\n";
    return 0;
}
