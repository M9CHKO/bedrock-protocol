#include <bedrock/bedrock.hpp>
#include <bedrock/auth/BedrockAuthJwt.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/protodef/ProtoDefJson.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
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
        std::cerr << "[LIVE-RELAY-REGRESSION-SMOKE] " << message << "\n";
    }
    return condition;
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

    bool contains(const std::string& text) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& error : errors_) {
            if (error.find(text) != std::string::npos) return true;
        }
        return false;
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

bedrock::ProtoDefValue tickValue(int64_t request, int64_t response) {
    return bedrock::ProtoDefValue::object({
        {"request_time", bedrock::ProtoDefValue::integer(request)},
        {"response_time", bedrock::ProtoDefValue::integer(response)}
    });
}

bedrock::ProtoDefValue timeValue(int32_t value) {
    return bedrock::ProtoDefValue::object({
        {"time", bedrock::ProtoDefValue::integer(value)}
    });
}

bedrock::ProtoDefValue fullMapValue() {
    using Value = bedrock::ProtoDefValue;
    std::vector<Value> pixels(
        16'384,
        Value::uinteger(std::numeric_limits<std::uint32_t>::max())
    );
    return Value::object({
        {"map_id", Value::integer(0)},
        {"update_flags", Value::object({
            {"_value", Value::uinteger(2)},
            {"void", Value::boolean(false)},
            {"texture", Value::boolean(true)},
            {"decoration", Value::boolean(false)},
            {"initialisation", Value::boolean(false)}
        })},
        {"dimension", Value::uinteger(0)},
        {"locked", Value::boolean(false)},
        {"origin", Value::object({
            {"x", Value::integer(0)},
            {"y", Value::integer(0)},
            {"z", Value::integer(0)}
        })},
        {"scale", Value::uinteger(0)},
        {"texture", Value::object({
            {"width", Value::integer(128)},
            {"height", Value::integer(128)},
            {"x_offset", Value::integer(0)},
            {"y_offset", Value::integer(0)},
            {"pixels", Value::array(std::move(pixels))}
        })}
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

std::string signedJwt(
    const bedrock::BedrockClientKeyPair& keys,
    const bedrock::ProtoDefValue& payload
) {
    return bedrock::BedrockAuthJwt::signEs384Jwt(
        keys.privateKeyPem,
        keys.publicKeyDerBase64,
        bedrock::ProtoDefJson::stringify(payload)
    );
}

std::shared_ptr<bedrock::Authflow> privateCaAuthflow(
    std::atomic<int>& calls,
    std::atomic<bool>& receivedClientKey
) {
    const auto root = bedrock::BedrockAuthJwt::generateP384KeyPair();
    const auto profile = bedrock::BedrockAuthJwt::generateP384KeyPair();
    return std::make_shared<bedrock::Authflow>(
        [root, profile, &calls, &receivedClientKey](std::string clientKey) {
            ++calls;
            receivedClientKey = !clientKey.empty() &&
                clientKey != root.publicKeyDerBase64 &&
                clientKey != profile.publicKeyDerBase64;

            const auto rootToken = signedJwt(
                root,
                bedrock::ProtoDefValue::object({
                    {"certificateAuthority", bedrock::ProtoDefValue::boolean(true)},
                    {"identityPublicKey", bedrock::ProtoDefValue::string(
                        profile.publicKeyDerBase64
                    )}
                })
            );
            const auto profileToken = signedJwt(
                profile,
                bedrock::ProtoDefValue::object({
                    {"identityPublicKey", bedrock::ProtoDefValue::string(
                        std::move(clientKey)
                    )},
                    {"extraData", bedrock::ProtoDefValue::object({
                        {"displayName", bedrock::ProtoDefValue::string(
                            "RelayMixedOnline"
                        )},
                        {"identity", bedrock::ProtoDefValue::string(
                            "11111111-2222-3333-4444-555555555555"
                        )},
                        {"XUID", bedrock::ProtoDefValue::string("827100")}
                    })}
                })
            );
            return bedrock::makeReadyAuthflowFuture(
                bedrock::MinecraftBedrockTokenChains {
                    rootToken,
                    profileToken
                }
            );
        }
    );
}

bedrock::RelayOptions relayOptions(
    uint16_t upstreamPort,
    bool upstreamOffline
) {
    bedrock::RelayOptions options;
    options.version = "1.21.100";
    options.host = "127.0.0.1";
    options.port = 0;
    options.motd = "Relay Regression";
    options.offline = true;
    options.maxPlayers = 2;
    options.batchingInterval = 2;
    options.advanced.username = "RelayRegressionUp";
    options.destination.host = "127.0.0.1";
    options.destination.port = upstreamPort;
    options.destination.offline = upstreamOffline;
    return options;
}

bedrock::BedrockNetworkClientOptions downstreamOptions(uint16_t relayPort) {
    bedrock::BedrockNetworkClientOptions options;
    options.host = "127.0.0.1";
    options.port = relayPort;
    options.username = "RelayRegressionDown";
    options.profile = options.username;
    options.version = "1.21.100";
    options.offline = true;
    options.connectTimeoutMs = 2000;
    options.batchingIntervalMs = 2;
    return options;
}

bool checkMixedAuthenticationHandshake() {
    const std::string version = "1.21.100";
    const auto request = encodedPacket(version, "tick_sync", tickValue(827001, 0));
    const auto response = encodedPacket(
        version,
        "tick_sync",
        tickValue(827001, 827002)
    );
    const auto clientbound = encodedPacket(version, "set_time", timeValue(827003));

    ErrorLog errors;
    std::mutex connectionMutex;
    bedrock::BedrockServerConnection upstreamConnection;
    std::atomic<bool> hasUpstreamConnection {false};
    std::atomic<int> upstreamLoggingIn {0};
    std::atomic<int> upstreamLogins {0};
    std::atomic<int> upstreamClientHandshakes {0};
    std::atomic<int> upstreamGameplayPackets {0};

    bedrock::BedrockServer upstream({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {{"motd", "Relay Mixed Auth Upstream"}},
        .maxPlayers = 2,
        .offline = true,
        .batchingInterval = 2
    });
    upstream.onConnect([&](const bedrock::BedrockServerConnection& connection) {
        connection.onError([&](const std::string& message) {
            errors.add("upstream player", message);
        });
        std::lock_guard<std::mutex> lock(connectionMutex);
        upstreamConnection = connection;
        hasUpstreamConnection = true;
    });
    upstream.onLoggingIn([&](const bedrock::BedrockServerLoggingInEvent&) {
        ++upstreamLoggingIn;
    });
    upstream.onLogin([&](const bedrock::BedrockServerPacketEvent&) {
        ++upstreamLogins;
    });
    upstream.onAny([&](const bedrock::BedrockServerPacketEvent& event) {
        if (event.packet.name == "client_to_server_handshake") {
            ++upstreamClientHandshakes;
        }
        if (event.packet.fullPacket == request.fullPacket) {
            ++upstreamGameplayPackets;
            upstream.write(event.connection, "tick_sync", tickValue(827001, 827002));
        }
    });
    upstream.listen();

    std::atomic<int> authflowCalls {0};
    std::atomic<bool> authflowReceivedClientKey {false};
    auto options = relayOptions(upstream.boundPort(), false);
    options.advanced.authflow = privateCaAuthflow(
        authflowCalls,
        authflowReceivedClientKey
    );
    bedrock::Relay relay(std::move(options));
    std::atomic<int> relayJoins {0};
    std::atomic<int> downstreamNegotiationInRelay {0};
    std::atomic<int> upstreamNegotiationInRelay {0};
    relay.onJoin([&](bedrock::RelayPlayer&, bedrock::BedrockNetworkClient&) {
        ++relayJoins;
    });
    relay.onServerbound([&](bedrock::RelayPacketEvent& event) {
        if (event.name == "request_network_settings" ||
            event.name == "login" ||
            event.name == "client_to_server_handshake") {
            ++downstreamNegotiationInRelay;
        }
    });
    relay.onClientbound([&](bedrock::RelayPacketEvent& event) {
        if (event.name == "network_settings" ||
            event.name == "server_to_client_handshake") {
            ++upstreamNegotiationInRelay;
        }
    });
    relay.onError([&](const std::string& message) {
        errors.add("relay", message);
    });
    relay.listen();

    auto downstream = bedrock::createNetworkClient(
        downstreamOptions(relay.live().boundPort())
    );
    std::atomic<int> downstreamNetworkSettings {0};
    std::atomic<int> downstreamServerHandshakes {0};
    std::atomic<int> downstreamTickResponses {0};
    std::atomic<int> downstreamSetTime {0};
    std::atomic<bool> downstreamClosed {false};
    downstream.onAny([&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        if (event.packet.name == "network_settings") {
            ++downstreamNetworkSettings;
        } else if (event.packet.name == "server_to_client_handshake") {
            ++downstreamServerHandshakes;
        } else if (event.packet.fullPacket == response.fullPacket) {
            ++downstreamTickResponses;
        } else if (event.packet.fullPacket == clientbound.fullPacket) {
            ++downstreamSetTime;
        }
    });
    downstream.onClose([&]() { downstreamClosed = true; });
    downstream.onError([&](const std::string& message) {
        errors.add("downstream", message);
    });

    bool ok = true;
    const bool connected = downstream.connect();
    const bool ready = connected && waitFor([&]() {
        return relayJoins.load() == 1 &&
            upstreamLoggingIn.load() == 1 &&
            upstreamLogins.load() == 1 &&
            upstreamClientHandshakes.load() == 1 &&
            relay.live().sessionCount() == 1 &&
            relay.live().upstreamCount() == 1 &&
            upstream.clientCount() == 1;
    });
    ok &= check(connected, "mixed-auth downstream failed to connect");
    ok &= check(ready, "mixed-auth upstream did not finish its own handshake");
    ok &= check(
        !relay.live().options().upstream.offline &&
            relay.live().options().upstream.authflow != nullptr,
        "destination.offline=false was not retained by the upstream client"
    );
    ok &= check(
        authflowCalls.load() == 1 && authflowReceivedClientKey.load(),
        "upstream Authflow did not receive its independently generated key"
    );
    ok &= check(
        downstreamNegotiationInRelay.load() == 0,
        "downstream negotiation packet reached Relay game forwarding"
    );
    ok &= check(
        upstreamNegotiationInRelay.load() == 0,
        "upstream negotiation packet reached downstream Relay handlers"
    );
    ok &= check(
        downstreamNetworkSettings.load() == 1 &&
            downstreamServerHandshakes.load() == 1,
        "downstream received an upstream negotiation packet or missed its own"
    );

    if (ready) {
        downstream.write("tick_sync", tickValue(827001, 0));
    }
    const bool serverboundWorked = ready && waitFor([&]() {
        return upstreamGameplayPackets.load() == 1 &&
            downstreamTickResponses.load() == 1;
    });
    ok &= check(serverboundWorked, "normal serverbound packet stopped after auth");

    bedrock::BedrockServerConnection target;
    {
        std::lock_guard<std::mutex> lock(connectionMutex);
        target = upstreamConnection;
    }
    if (serverboundWorked && hasUpstreamConnection) {
        upstream.write(target, "set_time", timeValue(827003));
    }
    ok &= check(
        serverboundWorked && waitFor([&]() {
            return downstreamSetTime.load() == 1;
        }),
        "normal clientbound packet stopped after auth"
    );
    ok &= check(
        upstreamLoggingIn.load() == 1 && upstreamLogins.load() == 1 &&
            upstreamClientHandshakes.load() == 1,
        "backend observed duplicate downstream login/handshake traffic"
    );
    ok &= check(!downstreamClosed.load(), "mixed-auth session closed unexpectedly");
    ok &= check(
        !errors.contains("Invalid encryption handshake"),
        "backend rejected the upstream encryption handshake"
    );
    ok &= check(errors.empty(), "mixed-auth callback error: " + errors.text());

    downstream.close("mixed-auth regression complete");
    relay.close("mixed-auth regression complete");
    upstream.close("mixed-auth regression complete");
    return ok;
}

bool checkFullMapForwarding() {
    const std::string version = "1.21.100";
    const auto mapPacket = encodedPacket(
        version,
        "clientbound_map_item_data",
        fullMapValue()
    );

    ErrorLog errors;
    std::mutex connectionMutex;
    bedrock::BedrockServerConnection upstreamConnection;
    std::atomic<bool> hasUpstreamConnection {false};
    bedrock::BedrockServer upstream({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {{"motd", "Relay Full Map Upstream"}},
        .maxPlayers = 2,
        .offline = true,
        .batchingInterval = 2
    });
    upstream.onConnect([&](const bedrock::BedrockServerConnection& connection) {
        connection.onError([&](const std::string& message) {
            errors.add("upstream player", message);
        });
        std::lock_guard<std::mutex> lock(connectionMutex);
        upstreamConnection = connection;
        hasUpstreamConnection = true;
    });
    upstream.listen();

    // Keep the default strict/disconnect policy: this packet must parse and
    // travel through the normal structured Relay path.
    bedrock::Relay relay(relayOptions(upstream.boundPort(), true));
    std::atomic<int> joins {0};
    std::atomic<int> relayMapCallbacks {0};
    std::atomic<bool> relayFieldsMismatch {false};
    relay.onJoin([&](bedrock::RelayPlayer&, bedrock::BedrockNetworkClient&) {
        ++joins;
    });
    relay.onClientbound([&](bedrock::RelayPacketEvent& event) {
        if (event.name != "clientbound_map_item_data") return;
        ++relayMapCallbacks;
        const auto* pixels = event.value("texture.pixels");
        bool lastPixelMatches = false;
        if (pixels && pixels->kind == bedrock::ProtoDefValue::Kind::Array &&
            pixels->arrayValue.size() == 16'384) {
            const auto& last = pixels->arrayValue.back();
            lastPixelMatches =
                (last.kind == bedrock::ProtoDefValue::Kind::UInt &&
                    last.uintValue ==
                        std::numeric_limits<std::uint32_t>::max()) ||
                (last.kind == bedrock::ProtoDefValue::Kind::Int &&
                    last.intValue ==
                        std::numeric_limits<std::uint32_t>::max());
        }
        if (event.getUInt("scale", 1) != 0 ||
            event.getInt("texture.width") != 128 ||
            event.getInt("texture.height") != 128 ||
            event.getInt("texture.x_offset", 1) != 0 ||
            event.getInt("texture.y_offset", 1) != 0 ||
            !pixels || pixels->kind != bedrock::ProtoDefValue::Kind::Array ||
            pixels->arrayValue.size() != 16'384 ||
            !lastPixelMatches) {
            relayFieldsMismatch = true;
        }
    });
    relay.onError([&](const std::string& message) {
        errors.add("relay", message);
    });
    relay.listen();

    auto downstream = bedrock::createNetworkClient(
        downstreamOptions(relay.live().boundPort())
    );
    std::atomic<int> downstreamMaps {0};
    std::atomic<bool> downstreamBytesMismatch {false};
    std::atomic<bool> downstreamClosed {false};
    downstream.onAny([&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        if (event.packet.name != "clientbound_map_item_data") return;
        ++downstreamMaps;
        if (event.packet.fullPacket != mapPacket.fullPacket ||
            event.packet.payload != mapPacket.payload) {
            downstreamBytesMismatch = true;
        }
    });
    downstream.onClose([&]() { downstreamClosed = true; });
    downstream.onError([&](const std::string& message) {
        errors.add("downstream", message);
    });

    bool ok = true;
    ok &= check(
        mapPacket.payload.size() == 81'937,
        "full map regression payload is not 81937 bytes"
    );
    const bool connected = downstream.connect();
    const bool ready = connected && waitFor([&]() {
        return joins.load() == 1 && hasUpstreamConnection &&
            relay.live().sessionCount() == 1 &&
            relay.live().upstreamCount() == 1 && upstream.clientCount() == 1;
    });
    ok &= check(connected, "full-map downstream failed to connect");
    ok &= check(ready, "full-map relay did not become ready");

    bedrock::BedrockServerConnection target;
    {
        std::lock_guard<std::mutex> lock(connectionMutex);
        target = upstreamConnection;
    }
    if (ready) upstream.sendPacket(target, mapPacket);
    const bool forwarded = ready && waitFor([&]() {
        return relayMapCallbacks.load() == 1 && downstreamMaps.load() == 1;
    }, 12s);
    ok &= check(forwarded, "full map was not forwarded to downstream");
    ok &= check(!relayFieldsMismatch.load(), "full map decoded fields mismatch");
    ok &= check(!downstreamBytesMismatch.load(), "Relay changed full map bytes");
    ok &= check(!downstreamClosed.load(), "full map closed downstream session");
    ok &= check(
        relay.live().sessionCount() == 1 &&
            relay.live().upstreamCount() == 1 &&
            relay.live().finalSessionResetCount() == 0,
        "full map caused a Relay session reset"
    );
    ok &= check(errors.empty(), "full-map callback error: " + errors.text());

    downstream.close("full-map regression complete");
    relay.close("full-map regression complete");
    upstream.close("full-map regression complete");
    return ok;
}

bool checkForwardRawPolicy() {
    const std::string version = "1.21.100";
    const auto codec = bedrock::VersionedMcpeCodec::forVersion(version);
    const auto unknown = codec.packetCodec().makePacketById(
        0x3feu,
        {0xde, 0xad, 0xbe, 0xef, 0x80, 0x01}
    );
    const auto following = encodedPacket(version, "set_time", timeValue(910002));

    ErrorLog errors;
    std::mutex connectionMutex;
    bedrock::BedrockServerConnection upstreamConnection;
    std::atomic<bool> hasUpstreamConnection {false};
    bedrock::BedrockServer upstream({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {{"motd", "Relay Forward Raw Upstream"}},
        .maxPlayers = 2,
        .offline = true,
        .batchingInterval = 2
    });
    upstream.onConnect([&](const bedrock::BedrockServerConnection& connection) {
        connection.onError([&](const std::string& message) {
            errors.add("upstream player", message);
        });
        std::lock_guard<std::mutex> lock(connectionMutex);
        upstreamConnection = connection;
        hasUpstreamConnection = true;
    });
    upstream.listen();

    auto options = relayOptions(upstream.boundPort(), true);
    options.parseErrorPolicy = bedrock::RelayParseErrorPolicy::ForwardRaw;
    bedrock::Relay relay(std::move(options));
    std::atomic<int> joins {0};
    std::atomic<int> parseErrors {0};
    std::atomic<int> unknownStructuredCallbacks {0};
    std::atomic<bool> parseErrorMismatch {false};
    relay.onJoin([&](bedrock::RelayPlayer&, bedrock::BedrockNetworkClient&) {
        ++joins;
    });
    relay.onParseError([&](const bedrock::RelayParseError& error) {
        ++parseErrors;
        if (error.direction != bedrock::BedrockRelayDirection::Clientbound ||
            error.packetName != unknown.name ||
            error.policy != bedrock::RelayParseErrorPolicy::ForwardRaw ||
            error.message.find("packet schema not found") == std::string::npos) {
            parseErrorMismatch = true;
        }
    });
    relay.onClientbound([&](bedrock::RelayPacketEvent& event) {
        if (event.name == unknown.name) ++unknownStructuredCallbacks;
    });
    relay.onError([&](const std::string& message) {
        errors.add("relay", message);
    });
    relay.listen();

    auto downstream = bedrock::createNetworkClient(
        downstreamOptions(relay.live().boundPort())
    );
    std::atomic<int> unknownPackets {0};
    std::atomic<int> followingPackets {0};
    std::atomic<bool> unknownBytesMismatch {false};
    std::atomic<bool> downstreamClosed {false};
    downstream.onAny([&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        if (event.packet.name == unknown.name) {
            ++unknownPackets;
            if (event.packet.fullPacket != unknown.fullPacket ||
                event.packet.payload != unknown.payload) {
                unknownBytesMismatch = true;
            }
        }
        if (event.packet.fullPacket == following.fullPacket) {
            ++followingPackets;
        }
    });
    downstream.onClose([&]() { downstreamClosed = true; });
    downstream.onError([&](const std::string& message) {
        errors.add("downstream", message);
    });

    bool ok = true;
    const bool connected = downstream.connect();
    const bool ready = connected && waitFor([&]() {
        return joins.load() == 1 && hasUpstreamConnection &&
            relay.live().upstreamCount() == 1;
    });
    ok &= check(connected, "forward_raw downstream failed to connect");
    ok &= check(ready, "forward_raw relay did not become ready");

    bedrock::BedrockServerConnection target;
    {
        std::lock_guard<std::mutex> lock(connectionMutex);
        target = upstreamConnection;
    }
    if (ready) upstream.sendPacket(target, unknown);
    const bool rawForwarded = ready && waitFor([&]() {
        return unknownPackets.load() == 1 && parseErrors.load() == 1;
    });
    ok &= check(rawForwarded, "forward_raw did not forward the unknown packet");
    ok &= check(!unknownBytesMismatch.load(), "forward_raw changed packet bytes");
    ok &= check(!parseErrorMismatch.load(), "parse-error callback data mismatch");
    ok &= check(
        unknownStructuredCallbacks.load() == 0,
        "unknown packet exposed partial structured fields"
    );
    ok &= check(!downstreamClosed.load(), "forward_raw closed the downstream");

    if (rawForwarded) upstream.sendPacket(target, following);
    ok &= check(
        rawForwarded && waitFor([&]() {
            return followingPackets.load() == 1;
        }),
        "valid packet after forward_raw was not delivered"
    );
    ok &= check(parseErrors.load() == 1, "parse error was reported more than once");
    ok &= check(
        relay.live().sessionCount() == 1 &&
            relay.live().upstreamCount() == 1 &&
            upstream.clientCount() == 1 &&
            !downstreamClosed.load(),
        "forward_raw did not preserve the live session"
    );
    ok &= check(errors.empty(), "forward_raw callback error: " + errors.text());

    downstream.close("forward_raw regression complete");
    relay.close("forward_raw regression complete");
    upstream.close("forward_raw regression complete");
    return ok;
}

bool checkDownstreamCloseLifecycle() {
    const std::string version = "1.21.100";
    const auto streamPacket = encodedPacket(version, "set_time", timeValue(710001));
    ErrorLog errors;
    std::mutex connectionMutex;
    bedrock::BedrockServerConnection upstreamConnection;
    std::atomic<bool> hasUpstreamConnection {false};
    std::atomic<int> upstreamDisconnects {0};

    bedrock::BedrockServer upstream({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {{"motd", "Relay Lifecycle Upstream"}},
        .maxPlayers = 2,
        .offline = true,
        .batchingInterval = 1
    });
    upstream.onConnect([&](const bedrock::BedrockServerConnection& connection) {
        connection.onError([&](const std::string& message) {
            errors.add("upstream player", message);
        });
        connection.onClose([&]() { ++upstreamDisconnects; });
        std::lock_guard<std::mutex> lock(connectionMutex);
        upstreamConnection = connection;
        hasUpstreamConnection = true;
    });
    upstream.listen();

    bedrock::Relay relay(relayOptions(upstream.boundPort(), true));
    std::atomic<int> joins {0};
    std::atomic<int> disconnects {0};
    std::atomic<int> callbacks {0};
    std::atomic<int> callbacksAfterDisconnect {0};
    std::atomic<bool> disconnectObserved {false};
    relay.onJoin([&](bedrock::RelayPlayer&, bedrock::BedrockNetworkClient&) {
        ++joins;
    });
    relay.onClientbound([&](bedrock::RelayPacketEvent& event) {
        if (event.packet.fullPacket != streamPacket.fullPacket) return;
        ++callbacks;
        if (disconnectObserved.load()) ++callbacksAfterDisconnect;
    });
    relay.onDisconnect([&](bedrock::RelayPlayer&) {
        disconnectObserved = true;
        ++disconnects;
    });
    relay.onError([&](const std::string& message) {
        errors.add("relay", message);
    });
    relay.listen();

    auto downstream = bedrock::createNetworkClient(
        downstreamOptions(relay.live().boundPort())
    );
    std::atomic<int> downstreamCloses {0};
    downstream.onClose([&]() { ++downstreamCloses; });
    downstream.onError([&](const std::string& message) {
        errors.add("downstream", message);
    });

    bool ok = true;
    const bool connected = downstream.connect();
    const bool ready = connected && waitFor([&]() {
        return joins.load() == 1 && hasUpstreamConnection &&
            relay.live().upstreamCount() == 1 && upstream.clientCount() == 1;
    });
    ok &= check(connected, "lifecycle downstream failed to connect");
    ok &= check(ready, "lifecycle relay did not become ready");

    std::atomic<bool> stopSender {false};
    std::thread sender;
    if (ready) {
        sender = std::thread([&]() {
            while (!stopSender.load()) {
                bedrock::BedrockServerConnection target;
                {
                    std::lock_guard<std::mutex> lock(connectionMutex);
                    target = upstreamConnection;
                }
                try {
                    upstream.sendPacket(target, streamPacket);
                } catch (const std::exception& error) {
                    if (!stopSender.load()) errors.add("backend sender", error.what());
                }
                std::this_thread::sleep_for(1ms);
            }
        });
    }

    const bool streamActive = ready && waitFor([&]() {
        return callbacks.load() >= 5;
    });
    ok &= check(streamActive, "backend packet stream did not become active");
    if (streamActive) downstream.close("lifecycle downstream close");
    const bool closed = streamActive && waitFor([&]() {
        return disconnects.load() == 1 && downstreamCloses.load() == 1 &&
            relay.playerCount() == 0 &&
            relay.live().sessionCount() == 0 &&
            relay.live().upstreamCount() == 0 &&
            upstream.clientCount() == 0;
    });
    const auto callbackCountAtClose = callbacks.load();
    std::this_thread::sleep_for(150ms);
    stopSender = true;
    if (sender.joinable()) sender.join();

    ok &= check(closed, "closing downstream left relay/upstream state alive");
    ok &= check(disconnects.load() == 1, "relay disconnect callback ran more than once");
    ok &= check(downstreamCloses.load() == 1, "downstream close callback ran more than once");
    ok &= check(upstreamDisconnects.load() == 1, "upstream did not close exactly once");
    ok &= check(
        relay.live().finalSessionResetCount() == 1,
        "relay session final reset did not run exactly once"
    );
    ok &= check(
        callbacksAfterDisconnect.load() == 0 &&
            callbacks.load() == callbackCountAtClose,
        "clientbound callback ran after final downstream close"
    );
    ok &= check(errors.empty(), "lifecycle callback error: " + errors.text());

    relay.close("lifecycle regression complete");
    upstream.close("lifecycle regression complete");
    return ok;
}

} // namespace

int main() {
    bool ok = checkMixedAuthenticationHandshake();
    ok = checkFullMapForwarding() && ok;
    ok = checkForwardRawPolicy() && ok;
    ok = checkDownstreamCloseLifecycle() && ok;
    if (ok) std::cout << "[LIVE-RELAY-REGRESSION-SMOKE] OK\n";
    return ok ? 0 : 1;
}
