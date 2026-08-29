#include <bedrock/bedrock.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

bedrock::ProtoDefValue tickValue(int64_t request, int64_t response) {
    return bedrock::ProtoDefValue::object({
        {"request_time", bedrock::ProtoDefValue::integer(request)},
        {"response_time", bedrock::ProtoDefValue::integer(response)}
    });
}

bedrock::VersionedGamePacket tickPacket(
    const std::string& version,
    int64_t request,
    int64_t response
) {
    const auto codec = bedrock::VersionedMcpeCodec::forVersion(version);
    const bedrock::ProtoDefPacketEncoder encoder(version);
    return codec.packetCodec().makePacketByName(
        "tick_sync",
        encoder.encodePacket("tick_sync", tickValue(request, response))
    );
}

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 6s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

void updateMaximum(std::atomic<std::size_t>& target, std::size_t value) {
    auto previous = target.load();
    while (previous < value &&
           !target.compare_exchange_weak(previous, value)) {
    }
}

class ErrorLog {
public:
    void add(std::string source, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        errors_.push_back(std::move(source) + ": " + message);
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return errors_.empty();
    }

    std::string text() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream out;
        for (std::size_t i = 0; i < errors_.size(); ++i) {
            if (i != 0) out << "; ";
            out << errors_[i];
        }
        return out.str();
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> errors_;
};

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[LIVE-RELAY-MULTI-SMOKE] " << message << "\n";
    }
    return condition;
}

bedrock::RelayOptions relayOptions(
    uint16_t upstreamPort,
    bool forceSingle
) {
    bedrock::RelayOptions options;
    options.version = "1.21.100";
    options.host = "127.0.0.1";
    options.port = 0;
    options.motd = forceSingle
        ? "Live Relay forceSingle Smoke"
        : "Live Relay Multi-client Smoke";
    options.advanced.username = "RelayMultiUp";
    options.offline = true;
    options.maxPlayers = 4;
    options.batchingInterval = 5;
    options.forceSingle = forceSingle;
    options.destination.host = "127.0.0.1";
    options.destination.port = upstreamPort;
    options.destination.offline = true;
    return options;
}

bedrock::BedrockNetworkClientOptions clientOptions(
    uint16_t relayPort,
    std::string username
) {
    bedrock::BedrockNetworkClientOptions options;
    options.host = "127.0.0.1";
    options.port = relayPort;
    options.username = std::move(username);
    options.profile = options.username;
    options.version = "1.21.100";
    options.offline = true;
    options.connectTimeoutMs = 1500;
    options.batchingIntervalMs = 5;
    return options;
}

bool checkMultiClientRouting() {
    const std::string version = "1.21.100";
    const auto requestA = tickPacket(version, 410001, 0);
    const auto responseA = tickPacket(version, 410001, 510001);
    const auto requestB = tickPacket(version, 420002, 0);
    const auto responseB = tickPacket(version, 420002, 520002);
    const auto requestB2 = tickPacket(version, 430003, 0);
    const auto responseB2 = tickPacket(version, 430003, 530003);

    ErrorLog errors;
    std::mutex routeMutex;
    std::string upstreamRouteA;
    std::string upstreamRouteB;
    std::string upstreamRouteB2;
    std::atomic<int> upstreamRequests {0};

    bedrock::BedrockServer upstreamServer({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {{"motd", "Relay Multi-client Upstream"}},
        .maxPlayers = 4,
        .offline = true,
        .batchingInterval = 5
    });
    upstreamServer.onConnect([&](const bedrock::BedrockServerConnection& connection) {
        connection.onError([&](const std::string& message) {
            errors.add("upstream player", message);
        });
    });
    upstreamServer.on("tick_sync", [&](const bedrock::BedrockServerPacketEvent& event) {
        try {
            const auto route = bedrock::BedrockLiveRelay::sessionId(event.connection);
            if (event.packet.fullPacket == requestA.fullPacket) {
                {
                    std::lock_guard<std::mutex> lock(routeMutex);
                    upstreamRouteA = route;
                }
                ++upstreamRequests;
                upstreamServer.write(event.connection, "tick_sync", tickValue(410001, 510001));
            } else if (event.packet.fullPacket == requestB.fullPacket) {
                {
                    std::lock_guard<std::mutex> lock(routeMutex);
                    upstreamRouteB = route;
                }
                ++upstreamRequests;
                upstreamServer.write(event.connection, "tick_sync", tickValue(420002, 520002));
            } else if (event.packet.fullPacket == requestB2.fullPacket) {
                {
                    std::lock_guard<std::mutex> lock(routeMutex);
                    upstreamRouteB2 = route;
                }
                ++upstreamRequests;
                upstreamServer.write(event.connection, "tick_sync", tickValue(430003, 530003));
            }
        } catch (const std::exception& error) {
            errors.add("upstream tick handler", error.what());
        }
    });
    upstreamServer.listen();

    bedrock::Relay relay(relayOptions(upstreamServer.boundPort(), false));
    std::atomic<std::size_t> statusConnections {0};
    std::atomic<std::size_t> statusJoined {0};
    std::atomic<std::size_t> statusStarted {0};
    std::atomic<std::size_t> statusReady {0};
    std::atomic<std::size_t> maximumConnections {0};
    std::atomic<int> connectCalls {0};
    std::atomic<int> joinCalls {0};
    std::atomic<int> joinMismatches {0};
    std::atomic<int> disconnectCalls {0};
    std::atomic<int> playerHandlerHits {0};
    std::atomic<int> playerHandlerMismatches {0};
    std::atomic<int> globalHandlerHits {0};
    std::mutex downstreamMutex;
    std::unordered_map<std::string, bedrock::BedrockServerConnection> downstreamConnections;
    std::unordered_map<std::string, std::string> packetSessions;
    std::mutex joinMutex;
    std::unordered_set<std::string> joinedSessions;

    const auto isCustomRequest = [&](const bedrock::VersionedGamePacket& packet) {
        return packet.fullPacket == requestA.fullPacket ||
            packet.fullPacket == requestB.fullPacket ||
            packet.fullPacket == requestB2.fullPacket;
    };

    relay.onConnect([&](bedrock::RelayPlayer& player) {
        ++connectCalls;
        const auto id = player.sessionId();
        {
            std::lock_guard<std::mutex> lock(downstreamMutex);
            downstreamConnections[id] = player.connection;
        }
        player.onServerbound([&, id](bedrock::RelayPacketEvent& event) {
            if (!isCustomRequest(event.packet)) return;
            ++playerHandlerHits;
            if (event.sessionId != id) ++playerHandlerMismatches;
        });
    });
    relay.onJoin([&](
        bedrock::RelayPlayer& player,
        bedrock::BedrockNetworkClient& upstream
    ) {
        ++joinCalls;
        const auto expected = relay.live().upstreamShared(player.connection);
        if (!expected || expected.get() != &upstream ||
            upstream.status() !=
                bedrock::BedrockNetworkClientStatus::Authenticating ||
            statusReady.load() == 0) {
            ++joinMismatches;
        }
        std::lock_guard<std::mutex> lock(joinMutex);
        joinedSessions.insert(player.sessionId());
    });
    relay.onDisconnect([&](bedrock::RelayPlayer&) {
        ++disconnectCalls;
    });
    relay.onServerbound([&](bedrock::RelayPacketEvent& event) {
        if (!isCustomRequest(event.packet)) return;
        ++globalHandlerHits;
        std::lock_guard<std::mutex> lock(downstreamMutex);
        if (event.packet.fullPacket == requestA.fullPacket) {
            packetSessions["a"] = event.sessionId;
        } else if (event.packet.fullPacket == requestB.fullPacket) {
            packetSessions["b"] = event.sessionId;
        } else if (event.packet.fullPacket == requestB2.fullPacket) {
            packetSessions["b2"] = event.sessionId;
        }
    });
    relay.onStatus([&](const bedrock::BedrockLiveRelayStatus& status) {
        statusConnections = status.downstreamConnections;
        statusJoined = status.downstreamJoinedCount;
        statusStarted = status.upstreamStartedCount;
        statusReady = status.upstreamReadyCount;
        updateMaximum(maximumConnections, status.downstreamConnections);
    });
    relay.onError([&](const std::string& message) {
        errors.add("relay", message);
    });
    relay.listen();

    auto clientA = bedrock::createNetworkClient(
        clientOptions(relay.live().boundPort(), "RelayMultiA")
    );
    auto clientB = bedrock::createNetworkClient(
        clientOptions(relay.live().boundPort(), "RelayMultiB")
    );
    std::atomic<int> clientAOwnResponses {0};
    std::atomic<int> clientACrossResponses {0};
    std::atomic<int> clientBOwnResponses {0};
    std::atomic<int> clientBCrossResponses {0};

    clientA.on("tick_sync", [&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        if (event.packet.fullPacket == responseA.fullPacket) {
            ++clientAOwnResponses;
        } else if (event.packet.fullPacket == responseB.fullPacket ||
                   event.packet.fullPacket == responseB2.fullPacket) {
            ++clientACrossResponses;
        }
    });
    clientB.on("tick_sync", [&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        if (event.packet.fullPacket == responseB.fullPacket ||
            event.packet.fullPacket == responseB2.fullPacket) {
            ++clientBOwnResponses;
        } else if (event.packet.fullPacket == responseA.fullPacket) {
            ++clientBCrossResponses;
        }
    });
    clientA.onError([&](const std::string& message) {
        errors.add("downstream A", message);
    });
    clientB.onError([&](const std::string& message) {
        errors.add("downstream B", message);
    });

    bool ok = true;
    const bool connectedA = clientA.connect();
    const bool firstReady = waitFor([&]() {
        return relay.playerCount() == 1 &&
            relay.live().sessionCount() == 1 &&
            statusReady.load() == 1 &&
            upstreamServer.clientCount() == 1;
    });
    const bool connectedB = firstReady && clientB.connect();
    const bool bothReady = connectedB && waitFor([&]() {
        return relay.playerCount() == 2 &&
            relay.live().sessionCount() == 2 &&
            relay.live().upstreamCount() == 2 &&
            upstreamServer.clientCount() == 2 &&
            statusConnections.load() == 2 &&
            statusJoined.load() == 2 &&
            statusStarted.load() == 2 &&
            statusReady.load() == 2;
    });

    ok &= check(connectedA, "first downstream connect failed");
    ok &= check(firstReady, "first relay session did not become ready");
    ok &= check(connectedB, "second downstream connect failed");
    ok &= check(bothReady, "two independent relay sessions did not become ready");
    ok &= check(connectCalls.load() == 2, "connect callback count is not two");
    ok &= check(joinCalls.load() == 2, "Relay join callback count is not two");
    ok &= check(joinMismatches.load() == 0,
                "Relay join did not expose the matching ready upstream boundary");
    ok &= check(relay.players().size() == 2, "players() did not expose two active players");

    std::vector<bedrock::BedrockServerConnection> capturedConnections;
    {
        std::lock_guard<std::mutex> lock(downstreamMutex);
        for (const auto& [id, connection] : downstreamConnections) {
            (void) id;
            capturedConnections.push_back(connection);
        }
    }
    ok &= check(capturedConnections.size() == 2, "did not capture both RelayPlayer connections");
    for (const auto& connection : capturedConnections) {
        ok &= check(relay.player(connection) != nullptr, "player(connection) lookup failed");
        std::lock_guard<std::mutex> lock(joinMutex);
        ok &= check(
            joinedSessions.contains(bedrock::BedrockLiveRelay::sessionId(connection)),
            "Relay join callback missed an accepted session"
        );
    }

    if (bothReady) {
        clientA.write("tick_sync", tickValue(410001, 0));
        clientB.write("tick_sync", tickValue(420002, 0));
    }
    const bool firstResponses = bothReady && waitFor([&]() {
        return clientAOwnResponses.load() == 1 &&
            clientBOwnResponses.load() == 1 &&
            upstreamRequests.load() >= 2;
    });
    ok &= check(firstResponses, "initial per-session tick responses were not routed");

    std::string downstreamSessionA;
    std::string downstreamSessionB;
    {
        std::lock_guard<std::mutex> lock(downstreamMutex);
        downstreamSessionA = packetSessions["a"];
        downstreamSessionB = packetSessions["b"];
    }
    std::string upstreamA;
    std::string upstreamB;
    {
        std::lock_guard<std::mutex> lock(routeMutex);
        upstreamA = upstreamRouteA;
        upstreamB = upstreamRouteB;
    }
    ok &= check(!downstreamSessionA.empty() && !downstreamSessionB.empty(),
                "relay packet events did not expose session ids");
    ok &= check(downstreamSessionA != downstreamSessionB,
                "both downstream packets used one relay session");
    ok &= check(!upstreamA.empty() && !upstreamB.empty() && upstreamA != upstreamB,
                "both downstreams were forwarded through one upstream connection");

    bedrock::BedrockServerConnection connectionA;
    bedrock::BedrockServerConnection connectionB;
    {
        std::lock_guard<std::mutex> lock(downstreamMutex);
        const auto foundA = downstreamConnections.find(downstreamSessionA);
        const auto foundB = downstreamConnections.find(downstreamSessionB);
        if (foundA != downstreamConnections.end()) connectionA = foundA->second;
        if (foundB != downstreamConnections.end()) connectionB = foundB->second;
    }

    // Trigger the no-callback Relay device-code policy on A. It must remove
    // only A's downstream/upstream pair while B keeps routing.
    auto upstreamClientA = relay.live().upstreamShared(connectionA);
    std::function<void(const bedrock::XboxDeviceCodeInfo&)> msaCodeA;
    if (upstreamClientA) msaCodeA = upstreamClientA->options().onMsaCode;
    ok &= check(static_cast<bool>(msaCodeA),
                "first upstream did not install Relay onMsaCode fallback");
    if (msaCodeA) {
        msaCodeA(bedrock::XboxDeviceCodeInfo {
            .message = "multi-client device code"
        });
    }
    const bool oneRemains = waitFor([&]() {
        return relay.playerCount() == 1 &&
            relay.live().sessionCount() == 1 &&
            relay.live().upstreamCount() == 1 &&
            upstreamServer.clientCount() == 1 &&
            statusConnections.load() == 1 &&
            statusReady.load() == 1 &&
            disconnectCalls.load() == 1;
    });
    if (!oneRemains) {
        std::ostringstream state;
        state << "session-scoped device-code disconnect did not converge: players="
              << relay.playerCount()
              << " sessions=" << relay.live().sessionCount()
              << " upstreams=" << relay.live().upstreamCount()
              << " upstream-server-clients=" << upstreamServer.clientCount()
              << " status-connections=" << statusConnections.load()
              << " status-ready=" << statusReady.load()
              << " disconnect-callbacks=" << disconnectCalls.load();
        ok &= check(false, state.str());
    }

    if (!downstreamSessionA.empty() && !downstreamSessionB.empty()) {
        ok &= check(relay.player(connectionA) == nullptr,
                    "disconnected RelayPlayer remained active");
        ok &= check(relay.player(connectionB) != nullptr,
                    "remaining RelayPlayer lookup was lost");
    }

    if (oneRemains) {
        clientB.write("tick_sync", tickValue(430003, 0));
    }
    const bool remainingResponse = oneRemains && waitFor([&]() {
        return clientBOwnResponses.load() == 2 && upstreamRequests.load() == 3;
    });
    ok &= check(remainingResponse, "remaining session stopped forwarding after peer close");

    std::string downstreamSessionB2;
    std::string upstreamB2;
    {
        std::lock_guard<std::mutex> lock(downstreamMutex);
        downstreamSessionB2 = packetSessions["b2"];
    }
    {
        std::lock_guard<std::mutex> lock(routeMutex);
        upstreamB2 = upstreamRouteB2;
    }
    ok &= check(downstreamSessionB2 == downstreamSessionB,
                "remaining downstream changed relay session");
    ok &= check(upstreamB2 == upstreamB,
                "remaining downstream changed upstream connection");
    ok &= check(clientACrossResponses.load() == 0 && clientBCrossResponses.load() == 0,
                "a client received another session's response");
    ok &= check(playerHandlerHits.load() == 3 && playerHandlerMismatches.load() == 0,
                "RelayPlayer packet handlers were not session-scoped");
    ok &= check(globalHandlerHits.load() == 3,
                "global relay handler did not observe all custom packets");
    ok &= check(maximumConnections.load() == 2,
                "aggregate relay status did not expose two connections");

    clientB.close("multi-client smoke complete");
    (void) waitFor([&]() {
        return relay.playerCount() == 0 && relay.live().sessionCount() == 0;
    }, 2s);
    relay.close("multi-client smoke complete");
    upstreamServer.close("multi-client smoke complete");

    ok &= check(errors.empty(), "unexpected error callback: " + errors.text());
    if (ok) std::cout << "[LIVE-RELAY-MULTI-SMOKE] multi-client routing ok\n";
    return ok;
}

bool checkForceSingle() {
    const std::string version = "1.21.100";
    const auto request = tickPacket(version, 610001, 0);
    const auto response = tickPacket(version, 610001, 710001);
    ErrorLog errors;

    bedrock::BedrockServer upstreamServer({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {{"motd", "Relay forceSingle Upstream"}},
        .maxPlayers = 4,
        .offline = true,
        .batchingInterval = 5
    });
    upstreamServer.onConnect([&](const bedrock::BedrockServerConnection& connection) {
        connection.onError([&](const std::string& message) {
            errors.add("forceSingle upstream player", message);
        });
    });
    upstreamServer.on("tick_sync", [&](const bedrock::BedrockServerPacketEvent& event) {
        if (event.packet.fullPacket != request.fullPacket) return;
        try {
            upstreamServer.write(event.connection, "tick_sync", tickValue(610001, 710001));
        } catch (const std::exception& error) {
            errors.add("forceSingle tick handler", error.what());
        }
    });
    upstreamServer.listen();

    bedrock::Relay relay(relayOptions(upstreamServer.boundPort(), true));
    std::atomic<int> connectCalls {0};
    std::atomic<int> firstPlayerPackets {0};
    std::atomic<std::size_t> statusConnections {0};
    std::atomic<std::size_t> statusReady {0};
    std::atomic<std::size_t> maximumConnections {0};
    relay.onConnect([&](bedrock::RelayPlayer& player) {
        ++connectCalls;
        player.onServerbound([&](bedrock::RelayPacketEvent& event) {
            if (event.packet.fullPacket == request.fullPacket) ++firstPlayerPackets;
        });
    });
    relay.onStatus([&](const bedrock::BedrockLiveRelayStatus& status) {
        statusConnections = status.downstreamConnections;
        statusReady = status.upstreamReadyCount;
        updateMaximum(maximumConnections, status.downstreamConnections);
    });
    relay.onError([&](const std::string& message) {
        errors.add("forceSingle relay", message);
    });
    relay.listen();

    auto first = bedrock::createNetworkClient(
        clientOptions(relay.live().boundPort(), "RelaySingleA")
    );
    auto second = bedrock::createNetworkClient(
        clientOptions(relay.live().boundPort(), "RelaySingleB")
    );
    std::atomic<int> firstResponses {0};
    std::atomic<bool> secondClosed {false};
    first.on("tick_sync", [&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        if (event.packet.fullPacket == response.fullPacket) ++firstResponses;
    });
    first.onError([&](const std::string& message) {
        errors.add("forceSingle first downstream", message);
    });
    second.onClose([&]() {
        secondClosed = true;
    });
    // A transport rejected by forceSingle may report either close or a
    // handshake error depending on which side observes the raw drop first.
    second.onError([](const std::string&) {});

    bool ok = true;
    const bool connectedFirst = first.connect();
    const bool firstReady = connectedFirst && waitFor([&]() {
        return relay.playerCount() == 1 &&
            relay.live().sessionCount() == 1 &&
            relay.live().upstreamCount() == 1 &&
            upstreamServer.clientCount() == 1 &&
            statusConnections.load() == 1 &&
            statusReady.load() == 1;
    });
    bool secondConnectResult = false;
    if (firstReady) secondConnectResult = second.connect();
    (void) secondConnectResult;
    const bool rejectionSettled = firstReady && waitFor([&]() {
        return relay.playerCount() == 1 &&
            relay.live().sessionCount() == 1 &&
            relay.live().upstreamCount() == 1 &&
            relay.live().server().clientCount() == 1 &&
            upstreamServer.clientCount() == 1 &&
            connectCalls.load() == 1;
    }, 4s);
    const bool secondTransportClosed = rejectionSettled && waitFor([&]() {
        return secondClosed.load() ||
            second.status() == bedrock::BedrockNetworkClientStatus::Disconnected;
    }, 4s);

    ok &= check(connectedFirst, "forceSingle first downstream connect failed");
    ok &= check(firstReady, "forceSingle first relay session did not become ready");
    ok &= check(rejectionSettled, "forceSingle rejection did not leave exactly one session");
    ok &= check(connectCalls.load() == 1, "forceSingle emitted connect for rejected transport");
    ok &= check(maximumConnections.load() == 1,
                "forceSingle status exposed more than one accepted connection");
    ok &= check(secondTransportClosed,
                "forceSingle second transport was not closed");

    if (rejectionSettled) {
        first.write("tick_sync", tickValue(610001, 0));
    }
    const bool firstStillRoutes = rejectionSettled && waitFor([&]() {
        return firstResponses.load() == 1 && firstPlayerPackets.load() == 1;
    });
    ok &= check(firstStillRoutes,
                "forceSingle rejection interrupted the accepted session");

    second.close("forceSingle smoke complete");
    first.close("forceSingle smoke complete");
    relay.close("forceSingle smoke complete");
    upstreamServer.close("forceSingle smoke complete");
    ok &= check(errors.empty(), "unexpected forceSingle error callback: " + errors.text());
    if (ok) std::cout << "[LIVE-RELAY-MULTI-SMOKE] forceSingle ok\n";
    return ok;
}

} // namespace

int main() {
    bool ok = checkMultiClientRouting();
    ok = checkForceSingle() && ok;
    return ok ? 0 : 1;
}
