#include <bedrock/bedrock.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
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

bedrock::VersionedGamePacket encodedPacket(
    const std::string& version,
    const std::string& name,
    const bedrock::ProtoDefValue& value
) {
    const bedrock::ProtoDefPacketEncoder encoder(version);
    const auto codec = bedrock::VersionedMcpeCodec::forVersion(version);
    return codec.packetCodec().makePacketByName(
        name,
        encoder.encodePacket(name, value)
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
        std::cerr << "[LIVE-RELAY-OPTIONS-SMOKE] " << message << "\n";
    }
    return condition;
}

bedrock::RelayOptions relayOptions(
    uint16_t upstreamPort,
    bool omitParseErrors
) {
    bedrock::RelayOptions options;
    options.version = "1.21.100";
    options.host = "127.0.0.1";
    options.port = 0;
    options.motd = "Live Relay Options Smoke";
    options.username = "RelayOptionsUp";
    options.offline = true;
    options.maxPlayers = 2;
    options.batchingInterval = 5;
    options.enableChunkCaching = true;
    options.omitParseErrors = omitParseErrors;
    options.destination.host = "127.0.0.1";
    options.destination.port = upstreamPort;
    options.destination.offline = true;
    return options;
}

bedrock::BedrockNetworkClientOptions clientOptions(
    uint16_t relayPort,
    const std::string& marker
) {
    bedrock::BedrockNetworkClientOptions options;
    options.host = "127.0.0.1";
    options.port = relayPort;
    options.username = "RelayOptionsDown";
    options.profile = options.username;
    options.version = "1.21.100";
    options.offline = true;
    options.connectTimeoutMs = 1500;
    options.batchingIntervalMs = 5;
    // Relay should preserve arbitrary skin/client metadata while replacing
    // only the destination-specific ServerAddress and version gates.
    options.clientDataJson =
        "{\"CustomSkinMarker\":\"" + marker +
        "\",\"SkinAnimationData\":\"\",\"GameVersion\":\"1.21.100\""
        ",\"ClientRandomId\":1,\"PlayFabId\":\"old\",\"UIProfile\":0}";
    return options;
}

bool runParsePolicy(bool omitParseErrors) {
    const std::string version = "1.21.100";
    const std::string marker = omitParseErrors
        ? "custom-skin-omit"
        : "custom-skin-disconnect";
    const auto codec = bedrock::VersionedMcpeCodec::forVersion(version);
    const auto malformedText = codec.packetCodec().makePacketByName("text", {});
    const auto expectedCache = encodedPacket(
        version,
        "client_cache_status",
        bedrock::ProtoDefValue::object({
            {"enabled", bedrock::ProtoDefValue::boolean(true)}
        })
    );
    const auto expectedTickResponse = encodedPacket(
        version,
        "tick_sync",
        tickValue(810001, 910001)
    );

    ErrorLog errors;
    std::mutex connectionMutex;
    bedrock::BedrockServerConnection upstreamConnection;
    bool hasUpstreamConnection = false;
    std::atomic<bool> customSkinForwarded {false};
    std::atomic<bool> cacheEnabledForwarded {false};
    std::atomic<int> relayTextHandlers {0};

    bedrock::BedrockServer upstreamServer({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {{"motd", "Relay Options Upstream"}},
        .maxPlayers = 2,
        .offline = true,
        .batchingInterval = 5
    });
    upstreamServer.onConnect([&](const bedrock::BedrockServerConnection& connection) {
        connection.onError([&](const std::string& message) {
            errors.add("upstream player", message);
        });
        std::lock_guard<std::mutex> lock(connectionMutex);
        upstreamConnection = connection;
        hasUpstreamConnection = true;
    });
    upstreamServer.onLogin([&](const bedrock::BedrockServerPacketEvent& event) {
        const auto skinData = upstreamServer.skinData(event.connection);
        const auto* value = skinData ? skinData->get("CustomSkinMarker") : nullptr;
        customSkinForwarded = value &&
            value->kind == bedrock::ProtoDefValue::Kind::String &&
            value->stringValue == marker;
    });
    upstreamServer.on("client_cache_status", [&](const bedrock::BedrockServerPacketEvent& event) {
        if (event.packet.fullPacket == expectedCache.fullPacket) {
            cacheEnabledForwarded = true;
        }
    });
    upstreamServer.on("tick_sync", [&](const bedrock::BedrockServerPacketEvent& event) {
        const auto request = encodedPacket(
            version,
            "tick_sync",
            tickValue(810001, 0)
        );
        if (event.packet.fullPacket != request.fullPacket) return;
        try {
            upstreamServer.write(
                event.connection,
                "tick_sync",
                tickValue(810001, 910001)
            );
        } catch (const std::exception& error) {
            errors.add("upstream tick handler", error.what());
        }
    });
    upstreamServer.listen();

    bedrock::Relay relay(relayOptions(upstreamServer.boundPort(), omitParseErrors));
    std::atomic<std::size_t> readyCount {0};
    relay.onStatus([&](const bedrock::BedrockLiveRelayStatus& status) {
        readyCount = status.upstreamReadyCount;
    });
    relay.onClientbound([&](bedrock::RelayPacketEvent& event) {
        if (event.name == "text") ++relayTextHandlers;
    });
    relay.onError([&](const std::string& message) {
        errors.add("relay", message);
    });
    relay.listen();

    auto downstream = bedrock::createNetworkClient(
        clientOptions(relay.live().boundPort(), marker)
    );
    std::atomic<int> downstreamTextPackets {0};
    std::atomic<int> tickResponses {0};
    std::atomic<bool> downstreamClosed {false};
    downstream.on("text", [&](const bedrock::BedrockNetworkClientPacketEvent&) {
        ++downstreamTextPackets;
    });
    downstream.on("tick_sync", [&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        if (event.packet.fullPacket == expectedTickResponse.fullPacket) {
            ++tickResponses;
        }
    });
    downstream.onClose([&]() {
        downstreamClosed = true;
    });
    downstream.onError([&](const std::string& message) {
        errors.add("downstream", message);
    });

    bool ok = true;
    const bool connected = downstream.connect();
    const bool ready = connected && waitFor([&]() {
        std::lock_guard<std::mutex> lock(connectionMutex);
        return hasUpstreamConnection &&
            relay.playerCount() == 1 &&
            relay.live().sessionCount() == 1 &&
            relay.live().upstreamCount() == 1 &&
            readyCount.load() == 1 &&
            upstreamServer.clientCount() == 1 &&
            customSkinForwarded.load() &&
            cacheEnabledForwarded.load();
    });
    ok &= check(connected, "downstream connect failed");
    ok &= check(ready, "relay options/metadata session did not become ready");
    ok &= check(relay.live().options().enableChunkCaching,
                "enableChunkCaching was not propagated to live relay");
    ok &= check(relay.options().omitParseErrors == omitParseErrors,
                "omitParseErrors option changed during construction");

    bedrock::BedrockServerConnection target;
    {
        std::lock_guard<std::mutex> lock(connectionMutex);
        target = upstreamConnection;
    }
    if (ready) upstreamServer.sendPacket(target, malformedText);

    if (omitParseErrors) {
        // Give the queued clientbound path a chance to process and prove that
        // the malformed packet itself was dropped before user dispatch.
        std::this_thread::sleep_for(250ms);
        const bool sessionSurvived = relay.playerCount() == 1 &&
            relay.live().sessionCount() == 1 &&
            relay.live().upstreamCount() == 1 &&
            upstreamServer.clientCount() == 1 &&
            !downstreamClosed.load();
        ok &= check(sessionSurvived,
                    "omitParseErrors=true disconnected the relay session");
        if (sessionSurvived) {
            downstream.write("tick_sync", tickValue(810001, 0));
        }
        const bool stillRoutes = sessionSurvived && waitFor([&]() {
            return tickResponses.load() == 1;
        });
        ok &= check(stillRoutes,
                    "omitParseErrors=true session stopped routing valid packets");
    } else {
        const bool disconnected = ready && waitFor([&]() {
            return downstreamClosed.load() &&
                relay.playerCount() == 0 &&
                relay.live().sessionCount() == 0 &&
                relay.live().upstreamCount() == 0 &&
                upstreamServer.clientCount() == 0;
        });
        ok &= check(disconnected,
                    "omitParseErrors=false did not disconnect the matching session");
    }

    ok &= check(relayTextHandlers.load() == 0,
                "malformed packet reached a high-level relay handler");
    ok &= check(downstreamTextPackets.load() == 0,
                "malformed packet reached the downstream client");
    ok &= check(customSkinForwarded.load(),
                "custom downstream skin metadata was not forwarded upstream");
    ok &= check(cacheEnabledForwarded.load(),
                "enableChunkCaching=true was not sent to the upstream server");

    downstream.close("relay options smoke complete");
    relay.close("relay options smoke complete");
    upstreamServer.close("relay options smoke complete");
    ok &= check(errors.empty(), "unexpected error callback: " + errors.text());

    if (ok) {
        std::cout << "[LIVE-RELAY-OPTIONS-SMOKE] omitParseErrors="
                  << (omitParseErrors ? "true" : "false") << " ok\n";
    }
    return ok;
}

} // namespace

int main() {
    bool ok = runParsePolicy(true);
    ok = runParsePolicy(false) && ok;
    return ok ? 0 : 1;
}
