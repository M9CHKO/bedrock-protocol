#include <bedrock/bedrock.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
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
    options.advanced.username = "RelayOptionsUp";
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
    const bedrock::XboxDeviceCodeInfo testMsaCode {
        .verificationUri = "https://microsoft.com/link",
        .userCode = "ABCD-EFGH",
        .message = "Use ABCD-EFGH to sign in"
    };
    const std::string expectedMsaPrompt =
        "It's your first time joining. Please sign in and reconnect to join "
        "this server:\n\n" + testMsaCode.message;
    const auto expectedMsaDisconnect = encodedPacket(
        version,
        "disconnect",
        bedrock::ProtoDefValue::object({
            {"reason", bedrock::ProtoDefValue::string("unknown")},
            {"hide_disconnect_reason", bedrock::ProtoDefValue::boolean(false)},
            {"message", bedrock::ProtoDefValue::string(expectedMsaPrompt)},
            {"filtered_message", bedrock::ProtoDefValue::string("")}
        })
    );

    ErrorLog errors;
    std::mutex connectionMutex;
    bedrock::BedrockServerConnection upstreamConnection;
    bool hasUpstreamConnection = false;
    std::atomic<bool> customSkinForwarded {false};
    std::atomic<bool> cacheEnabledForwarded {false};
    std::atomic<int> relayTextHandlers {0};
    std::atomic<int> contextualMsaCallbacks {0};
    std::atomic<bool> contextualMsaMismatch {false};
    std::string expectedMsaSessionId;

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

    auto options = relayOptions(upstreamServer.boundPort(), omitParseErrors);
    if (!omitParseErrors) {
        options.onMsaCode = [&](
            const bedrock::XboxDeviceCodeInfo& code,
            bedrock::RelayPlayer& player
        ) {
            ++contextualMsaCallbacks;
            if (code.verificationUri != testMsaCode.verificationUri ||
                code.userCode != testMsaCode.userCode ||
                code.message != testMsaCode.message ||
                player.sessionId() != expectedMsaSessionId) {
                contextualMsaMismatch = true;
            }
        };
    }
    bedrock::Relay relay(std::move(options));
    std::atomic<std::size_t> readyCount {0};
    std::atomic<int> relayJoinEvents {0};
    std::atomic<bool> relayJoinMismatch {false};
    std::atomic<int> relayPlayerLoginEvents {0};
    std::atomic<int> relayPlayerJoinEvents {0};
    std::atomic<int> relayPlayerCloseEvents {0};
    std::atomic<bool> relayPlayerMismatch {false};
    relay.onConnect([&](bedrock::RelayPlayer& player) {
        auto* playerPtr = &player;
        if (player.status() !=
                bedrock::BedrockServerClientStatus::Authenticating ||
            player.profile().has_value() || player.version().has_value()) {
            relayPlayerMismatch = true;
        }
        player.on("login", bedrock::RelayPlayer::PlayerPacketHandler(
            [&, playerPtr](const bedrock::BedrockServerPacketEvent& event) {
                ++relayPlayerLoginEvents;
                const auto profile = playerPtr->profile();
                if (event.packet.name != "login" || !profile ||
                    profile->name != "RelayOptionsDown" ||
                    profile->xuid != "0" ||
                    playerPtr->version() != std::optional<uint32_t>(
                        bedrock::protocolVersionFor(version)
                    ) || relay.live().upstreamStarted()) {
                    relayPlayerMismatch = true;
                }
            }
        ));
        player.on("join", bedrock::RelayPlayer::PlayerVoidHandler(
            [&, playerPtr]() {
                ++relayPlayerJoinEvents;
                // RelayPlayer's internal constructor listener must run first:
                // queued backend packets are released before public join.
                if (!relay.live().downstreamJoined() ||
                    playerPtr->status() !=
                        bedrock::BedrockServerClientStatus::Initializing) {
                    relayPlayerMismatch = true;
                }
            }
        ));
        player.on("close", bedrock::RelayPlayer::PlayerVoidHandler(
            [&, playerPtr]() {
                ++relayPlayerCloseEvents;
                // Node emits Player.close before Relay removes this player and
                // its upstream from the session maps.
                const auto clients = relay.clients();
                const auto exactClient = relay.client(
                    playerPtr->connection.key()
                );
                if (relay.playerCount() != 1 || clients.size() != 1 ||
                    !exactClient || exactClient.get() != playerPtr ||
                    relay.live().sessionCount() != 1 ||
                    playerPtr->status() ==
                        bedrock::BedrockServerClientStatus::Disconnected) {
                    relayPlayerMismatch = true;
                }
            }
        ));
    });
    relay.onStatus([&](const bedrock::BedrockLiveRelayStatus& status) {
        readyCount = status.upstreamReadyCount;
    });
    relay.on("join", [&relay, &readyCount, &relayJoinEvents, &relayJoinMismatch](
        bedrock::RelayPlayer& player,
        bedrock::BedrockNetworkClient& upstream
    ) {
        ++relayJoinEvents;
        const auto expected = relay.live().upstreamShared(player.connection);
        if (!expected || expected.get() != &upstream ||
            upstream.status() !=
                bedrock::BedrockNetworkClientStatus::Authenticating ||
            readyCount.load() != 1) {
            relayJoinMismatch = true;
        }
    });
    relay.onClientbound([&](bedrock::RelayPacketEvent& event) {
        if (event.name == "text") ++relayTextHandlers;
    });
    relay.onError([&](const std::string& message) {
        errors.add("relay", message);
    });
    const auto relayAddress = relay.listen();

    auto downstream = bedrock::createNetworkClient(
        clientOptions(relay.live().boundPort(), marker)
    );
    std::atomic<int> downstreamTextPackets {0};
    std::atomic<int> tickResponses {0};
    std::atomic<bool> downstreamClosed {false};
    std::atomic<int> msaPromptKicks {0};
    downstream.on("text", [&](const bedrock::BedrockNetworkClientPacketEvent&) {
        ++downstreamTextPackets;
    });
    downstream.on("tick_sync", [&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        if (event.packet.fullPacket == expectedTickResponse.fullPacket) {
            ++tickResponses;
        }
    });
    downstream.on("kick", [&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        if (event.packet.fullPacket == expectedMsaDisconnect.fullPacket) {
            ++msaPromptKicks;
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
            relay.clients().size() == 1 &&
            relay.live().sessionCount() == 1 &&
            relay.live().upstreamCount() == 1 &&
            readyCount.load() == 1 &&
            relayJoinEvents.load() == 1 &&
            relayPlayerLoginEvents.load() == 1 &&
            relayPlayerJoinEvents.load() == 1 &&
            upstreamServer.clientCount() == 1 &&
            customSkinForwarded.load() &&
            cacheEnabledForwarded.load();
    });
    ok &= check(connected, "downstream connect failed");
    ok &= check(ready, "relay options/metadata session did not become ready");
    ok &= check(
        relayAddress.host == "127.0.0.1" && relayAddress.port == 0,
        "Relay listen() did not return the configured host/port"
    );
    ok &= check(relay.live().options().enableChunkCaching,
                "enableChunkCaching was not propagated to live relay");
    ok &= check(relay.options().omitParseErrors == omitParseErrors,
                "omitParseErrors option changed during construction");
    ok &= check(!relayJoinMismatch.load(),
                "Relay on(\"join\") exposed the wrong Player/upstream/order");
    ok &= check(!relayPlayerMismatch.load(),
                "RelayPlayer Player facade profile/status/order mismatch");

    std::shared_ptr<bedrock::BedrockNetworkClient> relayUpstream;
    std::function<void(const bedrock::XboxDeviceCodeInfo&)> msaCodeCallback;
    if (ready) {
        const auto downstreamConnection = relay.player().connection;
        const auto clients = relay.clients();
        const auto exactClient = relay.client(downstreamConnection.key());
        ok &= check(
            clients.size() == 1 &&
                clients.find(downstreamConnection.key()) != clients.end() &&
                exactClient && exactClient.get() == &relay.player(),
            "Relay clients snapshot did not expose the exact downstream Player"
        );
        expectedMsaSessionId = bedrock::BedrockLiveRelay::sessionId(
            downstreamConnection
        );
        relayUpstream = relay.live().upstreamShared(downstreamConnection);
        if (relayUpstream) {
            msaCodeCallback = relayUpstream->options().onMsaCode;
        }
    }
    ok &= check(static_cast<bool>(msaCodeCallback),
                "relay upstream did not install onMsaCode routing");

    if (!omitParseErrors && msaCodeCallback) {
        msaCodeCallback(testMsaCode);
        ok &= check(contextualMsaCallbacks.load() == 1,
                    "two-argument Relay onMsaCode callback did not run");
        ok &= check(!contextualMsaMismatch.load(),
                    "Relay onMsaCode received the wrong code/player");
        ok &= check(!downstreamClosed.load() && msaPromptKicks.load() == 0,
                    "custom Relay onMsaCode also ran the default disconnect");
    }

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

        // relay.js disconnects this exact downstream with a sign-in prompt
        // when no onMsaCode callback is configured.
        if (stillRoutes && msaCodeCallback) {
            msaCodeCallback(testMsaCode);
        }
        const bool prompted = stillRoutes && waitFor([&]() {
            return msaPromptKicks.load() == 1 &&
                downstreamClosed.load() &&
                relay.playerCount() == 0 &&
                relay.clients().empty() &&
                relay.live().sessionCount() == 0 &&
                relay.live().upstreamCount() == 0;
        });
        ok &= check(prompted,
                    "missing Relay onMsaCode did not disconnect its session with the JS prompt");
    } else {
        const bool disconnected = ready && waitFor([&]() {
            return downstreamClosed.load() &&
                relay.playerCount() == 0 &&
                relay.clients().empty() &&
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
    ok &= check(relayPlayerCloseEvents.load() == 1 &&
                    !relayPlayerMismatch.load(),
                "RelayPlayer close event/order mismatch");

    downstream.close("relay options smoke complete");
    relay.close("relay options smoke complete");
    upstreamServer.close("relay options smoke complete");
    ok &= check(relay.clients().empty(),
                "Relay retained clients after close");
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
