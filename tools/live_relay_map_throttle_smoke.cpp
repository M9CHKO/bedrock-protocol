#include <bedrock/client/BedrockNetworkClient.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/relay/BedrockLiveRelay.hpp>
#include <bedrock/server/BedrockServer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 8s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[LIVE-RELAY-MAP-THROTTLE-SMOKE] " << message << "\n";
    }
    return condition;
}

bedrock::VersionedGamePacket mapPacket(
    const bedrock::VersionedMcpeCodec& codec,
    std::uint32_t sequence
) {
    std::vector<std::uint8_t> payload(1024);
    payload[0] = static_cast<std::uint8_t>(sequence);
    payload[1] = static_cast<std::uint8_t>(sequence >> 8u);
    payload[2] = static_cast<std::uint8_t>(sequence >> 16u);
    payload[3] = static_cast<std::uint8_t>(sequence >> 24u);
    return codec.packetCodec().makePacketByName(
        "clientbound_map_item_data",
        payload
    );
}

std::uint32_t sequenceOf(const bedrock::VersionedGamePacket& packet) {
    return static_cast<std::uint32_t>(packet.payload[0]) |
        (static_cast<std::uint32_t>(packet.payload[1]) << 8u) |
        (static_cast<std::uint32_t>(packet.payload[2]) << 16u) |
        (static_cast<std::uint32_t>(packet.payload[3]) << 24u);
}

} // namespace

int main() {
    const std::string version = "1.21.100";
    const auto codec = bedrock::VersionedMcpeCodec::forVersion(version);
    const bedrock::ProtoDefPacketEncoder encoder(version);
    const auto gameplay = codec.packetCodec().makePacketByName(
        "tick_sync",
        encoder.encodePacket(
            "tick_sync",
            bedrock::ProtoDefValue::object({
                {"request_time", bedrock::ProtoDefValue::integer(41)},
                {"response_time", bedrock::ProtoDefValue::integer(42)}
            })
        )
    );

    std::mutex upstreamMutex;
    bedrock::BedrockServerConnection upstreamConnection;
    bool hasUpstreamConnection = false;
    std::mutex errorMutex;
    std::vector<std::string> errors;

    bedrock::BedrockServer upstream({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {{"motd", "Map Throttle Upstream"}},
        .maxPlayers = 1,
        .offline = true,
        .batchingInterval = 10
    });
    upstream.onJoin([&](const bedrock::BedrockServerConnection& connection) {
        std::lock_guard<std::mutex> lock(upstreamMutex);
        upstreamConnection = connection;
        hasUpstreamConnection = true;
    });
    upstream.listen();

    bedrock::BedrockLiveRelayOptions relayOptions;
    relayOptions.server.host = "127.0.0.1";
    relayOptions.server.port = 0;
    relayOptions.server.version = version;
    relayOptions.server.motd = {{"motd", "Map Throttle Relay"}};
    relayOptions.server.maxPlayers = 1;
    relayOptions.server.offline = true;
    relayOptions.server.batchingInterval = 10;
    relayOptions.upstream.host = "127.0.0.1";
    relayOptions.upstream.port = upstream.boundPort();
    relayOptions.upstream.username = "MapThrottleUp";
    relayOptions.upstream.version = version;
    relayOptions.upstream.offline = true;
    relayOptions.upstream.connectTimeoutMs = 1500;
    relayOptions.throttleMapItemData = true;
    relayOptions.mapFlushIntervalMs = 20;
    relayOptions.mapPacketsPerFlush = 2;
    relayOptions.mapBytesPerFlush = 64u * 1024u;
    relayOptions.maxMapQueuePackets = 4096;
    relayOptions.maxMapQueueBytes = 64u * 1024u * 1024u;
    relayOptions.maxPacketsPerBatch = 4;
    relayOptions.maxBatchPayloadBytes = 64u * 1024u;

    bedrock::BedrockLiveRelay relay(std::move(relayOptions));
    relay.onError([&](const std::string& error) {
        std::lock_guard<std::mutex> lock(errorMutex);
        errors.push_back(error);
    });
    relay.listen();

    auto downstream = bedrock::createNetworkClient({
        .host = "127.0.0.1",
        .port = relay.boundPort(),
        .username = "MapThrottleDown",
        .version = version,
        .offline = true,
        .connectTimeoutMs = 1500,
        .batchingIntervalMs = 10
    });
    std::mutex observedMutex;
    std::vector<std::uint32_t> observed;
    std::atomic<bool> gameplayReceived {false};
    std::atomic<std::size_t> mapsBeforeGameplay {0};
    downstream.on(
        "clientbound_map_item_data",
        [&](const bedrock::BedrockNetworkClientPacketEvent& event) {
            std::lock_guard<std::mutex> lock(observedMutex);
            if (observed.size() < 64) {
                observed.push_back(sequenceOf(event.packet));
            }
        }
    );
    downstream.on(
        "tick_sync",
        [&](const bedrock::BedrockNetworkClientPacketEvent&) {
            std::lock_guard<std::mutex> lock(observedMutex);
            mapsBeforeGameplay = observed.size();
            gameplayReceived = true;
        }
    );
    downstream.onError([&](const std::string& error) {
        std::lock_guard<std::mutex> lock(errorMutex);
        errors.push_back(error);
    });

    bool ok = check(downstream.connect(), "downstream connect failed");
    ok &= check(waitFor([&]() {
        std::lock_guard<std::mutex> lock(upstreamMutex);
        return hasUpstreamConnection && relay.upstreamReady();
    }), "relay session did not become ready");

    bedrock::BedrockServerConnection target;
    {
        std::lock_guard<std::mutex> lock(upstreamMutex);
        target = upstreamConnection;
    }
    std::vector<bedrock::VersionedGamePacket> burst;
    burst.reserve(65);
    for (std::uint32_t sequence = 0; sequence < 64; ++sequence) {
        burst.push_back(mapPacket(codec, sequence));
        if (sequence == 7) burst.push_back(gameplay);
    }
    upstream.sendPackets(target, burst);

    ok &= check(waitFor([&]() {
        std::lock_guard<std::mutex> lock(observedMutex);
        return gameplayReceived.load() && observed.size() == 64;
    }), "map queue did not drain or gameplay packet was lost");

    std::vector<std::uint32_t> observedSnapshot;
    {
        std::lock_guard<std::mutex> lock(observedMutex);
        observedSnapshot = observed;
    }
    ok &= check(
        gameplayReceived.load() && mapsBeforeGameplay.load() <= 2,
        "queued gameplay was not prioritized ahead of map batches"
    );
    ok &= check(observedSnapshot.size() == 64, "missing or duplicate maps");
    for (std::size_t i = 0; i < observedSnapshot.size(); ++i) {
        ok &= check(observedSnapshot[i] == i,
            "map order changed at " + std::to_string(i));
        if (!ok) break;
    }
    const auto drained = relay.mapQueueStats();
    ok &= check(
        drained.received == 64 && drained.enqueued == 64 &&
            drained.forwarded == 64 && drained.queuedPackets == 0,
        "map queue counters did not finish cleanly"
    );

    // Close the downstream while a second backlog exists. This exercises
    // session-local cancellation and scheduler teardown rather than waiting
    // for stale maps to reach a future login.
    std::vector<bedrock::VersionedGamePacket> pending;
    pending.reserve(256);
    for (std::uint32_t sequence = 0; sequence < 256; ++sequence) {
        pending.push_back(mapPacket(codec, 1000 + sequence));
    }
    upstream.sendPackets(target, pending);
    ok &= check(waitFor([&]() {
        return relay.mapQueueStats().queuedPackets >= 16;
    }), "disconnect backlog did not enter the map queue");
    downstream.close("map backlog lifecycle test");
    ok &= check(waitFor([&]() {
        return relay.sessionCount() == 0;
    }), "disconnect did not clear the relay session backlog");

    relay.close("map throttle smoke complete");
    upstream.close("map throttle smoke complete");
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        ok &= check(errors.empty(), errors.empty()
            ? std::string {}
            : "unexpected relay/client error: " + errors.front());
    }

    if (!ok) return 1;
    std::cout << "[LIVE-RELAY-MAP-THROTTLE-SMOKE] ok\n";
    return 0;
}
