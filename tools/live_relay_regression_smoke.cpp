#include <bedrock/bedrock.hpp>
#include <bedrock/auth/BedrockAuthJwt.hpp>
#include <bedrock/debug/ProtocolTypeTsvIndex.hpp>
#include <bedrock/generated/GeneratedProtocolTypes.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/protodef/ProtoDefContext.hpp>
#include <bedrock/protodef/ProtoDefDecoder.hpp>
#include <bedrock/protodef/ProtoDefJson.hpp>
#include <bedrock/protodef/ProtoDefNbt.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefReader.hpp>
#include <bedrock/protodef/ProtoDefWriter.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
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

bool packetSequenceEquals(
    const std::vector<bedrock::VersionedGamePacket>& actual,
    const std::vector<bedrock::VersionedGamePacket>& expected,
    std::string& mismatch
) {
    if (actual.size() != expected.size()) {
        mismatch = "packet count " + std::to_string(actual.size()) +
            " != " + std::to_string(expected.size());
        return false;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (actual[index].name != expected[index].name) {
            mismatch = "packet[" + std::to_string(index) + "] name " +
                actual[index].name + " != " + expected[index].name;
            return false;
        }
        if (actual[index].fullPacket == expected[index].fullPacket) continue;
        std::size_t byteIndex = 0;
        while (
            byteIndex < actual[index].fullPacket.size() &&
            byteIndex < expected[index].fullPacket.size() &&
            actual[index].fullPacket[byteIndex] == expected[index].fullPacket[byteIndex]
        ) {
            ++byteIndex;
        }
        mismatch = "packet[" + std::to_string(index) + "] " + actual[index].name +
            " differs at full-packet byte " + std::to_string(byteIndex) +
            " (sizes " + std::to_string(actual[index].fullPacket.size()) +
            " and " + std::to_string(expected[index].fullPacket.size()) + ")";
        return false;
    }
    mismatch.clear();
    return true;
}

std::optional<std::size_t> decodedArrayIndex(
    const std::string& path,
    const std::string& root,
    const std::string& suffix
);
std::optional<int64_t> decodedIntegerValue(std::string text);

std::optional<bedrock::VersionedGamePacket> capturedItemRegistry() {
    const char* path = std::getenv("BEDROCK_REAL_ITEM_REGISTRY");
    if (path == nullptr || *path == '\0') return std::nullopt;

    std::ifstream stream(path, std::ios::binary);
    std::vector<uint8_t> bytes {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
    if (!stream.is_open() || bytes.empty()) {
        throw std::runtime_error("could not read captured item_registry");
    }

    const std::string version = "1.21.100";
    auto packet = bedrock::VersionedMcpeCodec::forVersion(version)
        .packetCodec()
        .decodeFullPacket(bytes);
    if (packet.name != "item_registry") {
        throw std::runtime_error("capture is not item_registry");
    }
    return packet;
}

std::unordered_map<std::string, int64_t> itemRuntimeIdsByName(
    const bedrock::VersionedGamePacket& packet,
    std::size_t* fieldCount = nullptr
) {
    const auto fields = bedrock::ProtoDefPacketDecoder("1.21.100")
        .decodePacketStrict(packet.name, packet.payload);
    if (fieldCount != nullptr) *fieldCount = fields.size();
    std::unordered_map<std::size_t, std::string> names;
    std::unordered_map<std::size_t, int64_t> runtimeIds;
    for (const auto& field : fields) {
        if (const auto index = decodedArrayIndex(field.path, "itemstates", ".name")) {
            names[*index] = field.value;
        } else if (const auto index = decodedArrayIndex(
                       field.path,
                       "itemstates",
                       ".runtime_id"
                   )) {
            if (const auto id = decodedIntegerValue(field.value)) {
                runtimeIds[*index] = *id;
            }
        }
    }
    std::unordered_map<std::string, int64_t> byName;
    for (const auto& [index, id] : runtimeIds) {
        if (const auto found = names.find(index); found != names.end()) {
            byName[found->second] = id;
        }
    }
    return byName;
}

bool checkCapturedItemRegistry() {
    const auto captured = capturedItemRegistry();
    if (!captured.has_value()) return true;
    const auto& packet = *captured;
    const auto& bytes = packet.fullPacket;

    const auto validationStarted = std::chrono::steady_clock::now();
    bedrock::ProtoDefPacketDecoder("1.21.100")
        .validatePacketStrict(packet.name, packet.payload);
    const auto validationElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - validationStarted
    ).count();
    const auto started = std::chrono::steady_clock::now();
    std::size_t fieldCount = 0;
    const auto byName = itemRuntimeIdsByName(packet, &fieldCount);
    const auto decodeElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started
    ).count();
    std::cerr << "[captured-registry] bytes=" << bytes.size()
              << " entries=" << byName.size()
              << " fields=" << fieldCount
              << " validation_ms=" << validationElapsed
              << " decode_ms=" << decodeElapsed
              << "\n";
    bool ok = check(byName.size() == 1'836, "captured registry count mismatch");
    ok &= check(
        validationElapsed < 2'000 && decodeElapsed < 2'000,
        "captured registry decode regressed above two seconds"
    );
    const std::unordered_map<std::string, int64_t> expected {
        {"minecraft:firework_rocket", 552},
        {"minecraft:diamond_sword", 340},
        {"minecraft:netherite_sword", 640},
        {"minecraft:sand", 12},
        {"minecraft:shulker_box", 810}
    };
    for (const auto& [name, id] : expected) {
        ok &= check(
            byName.find(name) != byName.end() && byName.at(name) == id,
            "captured registry runtime ID mismatch for " + name
        );
    }
    return ok;
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

bedrock::ProtoDefValue noisyMapValue(int64_t mapId, uint32_t seed) {
    using Value = bedrock::ProtoDefValue;
    std::vector<Value> pixels;
    pixels.reserve(16'384);
    uint32_t state = seed;
    for (std::size_t index = 0; index < 16'384; ++index) {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        pixels.push_back(Value::uinteger(state));
    }
    return Value::object({
        {"map_id", Value::integer(mapId)},
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

std::vector<uint8_t> unhex(const std::string& value) {
    const auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    if ((value.size() & 1u) != 0) return {};
    std::vector<uint8_t> result;
    result.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        const int high = digit(value[index]);
        const int low = digit(value[index + 1]);
        if (high < 0 || low < 0) return {};
        result.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return result;
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

bedrock::VersionedGamePacket minimalStrictPacket(
    const std::string& version,
    const std::string& name
) {
    bedrock::ProtocolTypeTsvIndex typeIndex;
    const auto rootType = typeIndex.findTypeJson(version, "packet_" + name);
    if (!rootType.has_value()) {
        throw std::runtime_error("missing packet schema for " + version + ":" + name);
    }

    std::vector<uint8_t> zeroes(1024 * 1024, 0);
    bedrock::PacketFieldCursor cursor(zeroes);
    bedrock::ProtoDefReader reader(cursor);
    bedrock::ProtoDefContext context;
    std::vector<bedrock::ProtoDefField> fields;
    bedrock::ProtoDefDecoder decoder([&](const std::string& typeName) {
        auto resolved = typeIndex.findTypeJson(version, typeName);
        if (resolved.has_value()) return resolved;
        return bedrock::generatedProtocolTypeJson(version, typeName);
    });
    decoder.decode(*rootType, reader, "", fields, context);
    zeroes.resize(reader.offset());

    bedrock::ProtoDefPacketDecoder strict(version);
    (void) strict.decodePacketStrict(name, zeroes);
    return bedrock::VersionedMcpeCodec::forVersion(version)
        .packetCodec()
        .makePacketByName(name, zeroes);
}

std::string decodedFieldValue(
    const std::vector<bedrock::ProtoDefField>& fields,
    const std::string& path
) {
    for (const auto& field : fields) {
        if (field.path == path) return field.value;
    }
    return {};
}

bool pathEndsWith(const std::string& path, const std::string& suffix) {
    return path.size() >= suffix.size() &&
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::optional<std::size_t> decodedArrayIndex(
    const std::string& path,
    const std::string& root,
    const std::string& suffix
) {
    const auto prefix = root + "[";
    if (path.rfind(prefix, 0) != 0 || !pathEndsWith(path, suffix)) {
        return std::nullopt;
    }
    const auto close = path.find(']', prefix.size());
    if (close == std::string::npos || path.substr(close + 1) != suffix) {
        return std::nullopt;
    }
    try {
        return static_cast<std::size_t>(std::stoull(
            path.substr(prefix.size(), close - prefix.size())
        ));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<int64_t> decodedIntegerValue(std::string text) {
    if (const auto slash = text.find('/'); slash != std::string::npos) {
        text.resize(slash);
    }
    try {
        std::size_t consumed = 0;
        const auto value = std::stoll(text, &consumed, 10);
        if (consumed == text.size()) return value;
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

bedrock::ProtoDefValue inventoryContainerValue() {
    using Value = bedrock::ProtoDefValue;
    return Value::object({
        {"container_id", Value::string("inventory")},
        {"dynamic_container_id", Value::null()}
    });
}

bedrock::ProtoDefValue emptyPacketNbtValue() {
    using Value = bedrock::ProtoDefValue;
    return Value::object({
        {"type", Value::string("compound")},
        {"name", Value::string("")},
        {"value", Value::object({})}
    });
}

int32_t itemStateVersion(const std::optional<bedrock::ProtoDefValue>& version) {
    if (!version.has_value()) return 2;
    const auto& value = *version;
    if (value.kind == bedrock::ProtoDefValue::Kind::String) {
        if (value.stringValue == "legacy") return 0;
        if (value.stringValue == "data_driven") return 1;
        if (value.stringValue == "none") return 2;
    }
    if (value.kind == bedrock::ProtoDefValue::Kind::Int) {
        return static_cast<int32_t>(value.intValue);
    }
    if (value.kind == bedrock::ProtoDefValue::Kind::UInt) {
        return static_cast<int32_t>(value.uintValue);
    }
    throw std::runtime_error("unsupported item-state version fixture");
}

bedrock::VersionedGamePacket itemRegistryPacket(
    const std::string& version,
    const std::vector<bedrock::BedrockItemState>& states
) {
    bedrock::ProtoDefWriter payload;
    payload.varuint32(static_cast<uint32_t>(states.size()));
    for (const auto& state : states) {
        payload.string(state.name);
        payload.u16le(static_cast<uint16_t>(state.runtimeId));
        payload.boolValue(state.componentBased);
        payload.zigzag32(itemStateVersion(state.version));
        bedrock::writeProtoDefNbt(
            payload,
            state.nbt.value_or(emptyPacketNbtValue()),
            bedrock::BedrockNbtEncoding::LittleVarInt
        );
    }
    return bedrock::VersionedMcpeCodec::forVersion(version)
        .packetCodec()
        .makePacketByName("item_registry", payload.take());
}

bool checkItemRegistryVersionBoundary() {
    const std::string beforeVersion = "1.21.50";
    const std::string afterVersion = "1.21.60";
    const auto beforeStart = minimalStrictPacket(beforeVersion, "start_game");
    const auto afterStart = minimalStrictPacket(afterVersion, "start_game");
    const auto beforeFields = bedrock::ProtoDefPacketDecoder(beforeVersion)
        .decodePacketStrict("start_game", beforeStart.payload);
    const auto afterFields = bedrock::ProtoDefPacketDecoder(afterVersion)
        .decodePacketStrict("start_game", afterStart.payload);
    const auto hasPath = [](const auto& fields, const std::string& path) {
        for (const auto& field : fields) {
            if (field.path == path) return true;
        }
        return false;
    };

    bool ok = true;
    ok &= check(
        !bedrock::ProtocolDefinition::forVersion(beforeVersion)
             .hasPacket("item_registry") &&
            hasPath(beforeFields, "itemstates.$count"),
        "1.21.50 must carry itemstates in start_game"
    );
    ok &= check(
        bedrock::ProtocolDefinition::forVersion(afterVersion)
                .hasPacket("item_registry") &&
            !hasPath(afterFields, "itemstates.$count"),
        "1.21.60 must move itemstates out of start_game"
    );

    const std::vector<bedrock::BedrockItemState> states {{
        .name = "minecraft:firework_rocket",
        .runtimeId = 552,
        .componentBased = false,
        .version = bedrock::ProtoDefValue::string("none"),
        .nbt = emptyPacketNbtValue()
    }};
    const auto registry = itemRegistryPacket(afterVersion, states);
    const auto registryFields = bedrock::ProtoDefPacketDecoder(afterVersion)
        .decodePacketStrict("item_registry", registry.payload);
    const bedrock::ProtoDefField* runtimeId = nullptr;
    for (const auto& field : registryFields) {
        if (field.path == "itemstates[0].runtime_id") {
            runtimeId = &field;
            break;
        }
    }
    const auto decoded = bedrock::VersionedMcpeCodec::forVersion(afterVersion)
        .packetCodec()
        .decodeFullPacket(registry.fullPacket);
    ok &= check(
        runtimeId != nullptr && runtimeId->type == "li16" &&
            runtimeId->size == 2 && runtimeId->value == "552",
        "1.21.60 item_registry runtime_id is not signed little-endian 16-bit"
    );
    ok &= check(
        decoded.name == "item_registry" &&
            decoded.fullPacket == registry.fullPacket,
        "1.21.60 item_registry full-packet identity failed"
    );
    return ok;
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

bool checkMapFloodAndItemOrdering() {
    using Value = bedrock::ProtoDefValue;
    const std::string version = "1.21.100";
    const auto startGame = minimalStrictPacket(version, "start_game");

    bedrock::MinecraftDataAssets assets;
    const auto items = assets.loadBedrockItemRegistryByProtocol(827);
    const auto blocks = assets.loadBedrockBlockRegistryByProtocol(827);
    const auto capturedRegistry = capturedItemRegistry();
    std::unordered_map<std::string, int64_t> expectedItemIds {
        {"minecraft:firework_rocket", 1192},
        {"minecraft:diamond_sword", 895},
        {"minecraft:netherite_sword", 900},
        {"minecraft:sand", 59},
        {"minecraft:shulker_box", 9471}
    };
    if (capturedRegistry.has_value()) {
        expectedItemIds = itemRuntimeIdsByName(*capturedRegistry);
    }
    const auto itemId = [&](const std::string& name) -> int64_t {
        const auto found = expectedItemIds.find(name);
        if (found == expectedItemIds.end()) {
            throw std::runtime_error("missing item registry definition: " + name);
        }
        return found->second;
    };
    const auto blockRuntimeId = [&](const std::string& name) -> int32_t {
        const auto* definition = blocks.blockByName(name);
        if (definition == nullptr) {
            throw std::runtime_error("missing block definition: " + name);
        }
        const auto block = blocks.fromStateId(definition->defaultState);
        return block && block->hash.has_value()
            ? *block->hash
            : definition->defaultState;
    };

    items.resetStackIds(100);
    auto sand = items.create(59, 64);
    auto shulker = items.create(9471, 1);
    auto firework = items.create(
        1192,
        16,
        0,
        bedrock::NbtDocument {
            "",
            bedrock::NbtValue::compound({{
                "Fireworks",
                bedrock::NbtValue::compound({{
                    "Flight",
                    bedrock::NbtValue::byte(3)
                }})
            }})
        }
    );
    auto sword = items.create(895, 1);
    sword.setCustomName("Relay Regression Blade");
    sword.setDurabilityUsed(73);
    sword.setEnchantments({
        {.name = "sharpness", .level = 5},
        {.name = "unbreaking", .level = 3}
    });
    auto netheriteSword = items.create(900, 1);
    netheriteSword.setCustomName("Relay Regression Netherite");
    netheriteSword.setDurabilityUsed(101);
    netheriteSword.setEnchantments({
        {.name = "unbreaking", .level = 3}
    });

    auto sandValue = items.toProtoDefValue(sand);
    sandValue.objectValue["block_runtime_id"] = Value::integer(
        blockRuntimeId("minecraft:sand")
    );
    auto shulkerValue = items.toProtoDefValue(shulker);
    auto fireworkValue = items.toProtoDefValue(firework);
    auto swordValue = items.toProtoDefValue(sword);
    auto netheriteSwordValue = items.toProtoDefValue(netheriteSword);
    sandValue.objectValue["network_id"] = Value::integer(itemId("minecraft:sand"));
    shulkerValue.objectValue["network_id"] = Value::integer(
        itemId("minecraft:shulker_box")
    );
    fireworkValue.objectValue["network_id"] = Value::integer(
        itemId("minecraft:firework_rocket")
    );
    swordValue.objectValue["network_id"] = Value::integer(
        itemId("minecraft:diamond_sword")
    );
    netheriteSwordValue.objectValue["network_id"] = Value::integer(
        itemId("minecraft:netherite_sword")
    );
    const auto emptyItem = items.toProtoDefValue(nullptr);

    const auto registryStates = items.writeItemStates();
    const auto expectedRegistryCount = registryStates.size();
    if (expectedRegistryCount < 1'000) {
        throw std::runtime_error("full item registry fixture is unexpectedly small");
    }

    std::vector<bedrock::BedrockItemState> selectedStates;
    std::vector<Value> selectedStateValues;
    const auto allItemStateValues = items.writeItemStatesValue();
    for (std::size_t index = 0; index < registryStates.size(); ++index) {
        const auto id = registryStates[index].runtimeId;
        if (id != 59 && id != 895 && id != 900 && id != 1192 && id != 9471) {
            continue;
        }
        selectedStates.push_back(registryStates[index]);
        auto state = allItemStateValues.arrayValue[index];
        if (state.get("version") == nullptr) {
            state.objectValue["version"] = Value::string("none");
        }
        if (state.get("nbt") == nullptr) {
            state.objectValue["nbt"] = emptyPacketNbtValue();
        }
        selectedStateValues.push_back(std::move(state));
    }
    if (selectedStates.size() != 5) {
        throw std::runtime_error("item registry control entries are incomplete");
    }
    const auto schemaReferenceRegistry = encodedPacket(
        version,
        "item_registry",
        Value::object({
            {"itemstates", Value::array(std::move(selectedStateValues))}
        })
    );
    if (itemRegistryPacket(version, selectedStates).fullPacket !=
        schemaReferenceRegistry.fullPacket) {
        throw std::runtime_error("full item registry fixture disagrees with schema encoder");
    }
    auto registry = itemRegistryPacket(version, registryStates);
    if (capturedRegistry.has_value()) registry = *capturedRegistry;
    const auto registryDecodeStarted = std::chrono::steady_clock::now();
    const auto registryFields = bedrock::ProtoDefPacketDecoder(version)
        .decodePacketStrict("item_registry", registry.payload);
    const auto registryDecodeElapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - registryDecodeStarted
        );
    const auto creativeContent = encodedPacket(
        version,
        "creative_content",
        Value::object({
            {"groups", Value::array({
                Value::object({
                    {"category", Value::string("construction")},
                    {"name", Value::string("Relay blocks")},
                    {"icon_item", sandValue}
                }),
                Value::object({
                    {"category", Value::string("items")},
                    {"name", Value::string("Relay items")},
                    {"icon_item", fireworkValue}
                })
            })},
            {"items", Value::array({
                Value::object({
                    {"entry_id", Value::uinteger(1)},
                    {"item", sandValue},
                    {"group_index", Value::uinteger(0)}
                }),
                Value::object({
                    {"entry_id", Value::uinteger(2)},
                    {"item", fireworkValue},
                    {"group_index", Value::uinteger(1)}
                }),
                Value::object({
                    {"entry_id", Value::uinteger(3)},
                    {"item", swordValue},
                    {"group_index", Value::uinteger(1)}
                })
            })}
        })
    );

    std::vector<Value> inventory(36, emptyItem);
    inventory[0] = sandValue;
    inventory[1] = shulkerValue;
    inventory[2] = fireworkValue;
    inventory[3] = swordValue;
    const auto inventoryContent = encodedPacket(
        version,
        "inventory_content",
        Value::object({
            {"window_id", Value::string("inventory")},
            {"input", Value::array(std::move(inventory))},
            {"container", inventoryContainerValue()},
            {"storage_item", emptyItem}
        })
    );
    const auto inventorySlot = encodedPacket(
        version,
        "inventory_slot",
        Value::object({
            {"window_id", Value::string("inventory")},
            {"slot", Value::uinteger(4)},
            {"container", inventoryContainerValue()},
            {"storage_item", emptyItem},
            {"item", netheriteSwordValue}
        })
    );
    const auto mobEquipment = encodedPacket(
        version,
        "mob_equipment",
        Value::object({
            {"runtime_entity_id", Value::uinteger(0x100000001ull)},
            {"item", swordValue},
            {"slot", Value::uinteger(3)},
            {"selected_slot", Value::uinteger(3)},
            {"window_id", Value::string("inventory")}
        })
    );
    const auto addItemEntity = encodedPacket(
        version,
        "add_item_entity",
        Value::object({
            {"entity_id_self", Value::integer(0x100000002ll)},
            {"runtime_entity_id", Value::uinteger(0x200000002ull)},
            {"item", fireworkValue},
            {"position", Value::object({
                {"x", Value::floating(1.25)},
                {"y", Value::floating(70.5)},
                {"z", Value::floating(-4.75)}
            })},
            {"velocity", Value::object({
                {"x", Value::floating(0.0)},
                {"y", Value::floating(0.1)},
                {"z", Value::floating(0.0)}
            })},
            {"metadata", Value::array({})},
            {"is_from_fishing", Value::boolean(false)}
        })
    );

    std::vector<bedrock::VersionedGamePacket> maps;
    maps.reserve(6);
    for (int index = 0; index < 6; ++index) {
        maps.push_back(encodedPacket(
            version,
            "clientbound_map_item_data",
            noisyMapValue(
                10'000 + index,
                0x9e3779b9u ^ static_cast<uint32_t>(index * 0x85ebca6bu)
            )
        ));
    }
    ErrorLog errors;
    std::mutex connectionMutex;
    bedrock::BedrockServerConnection upstreamConnection;
    std::atomic<bool> hasUpstreamConnection {false};
    bedrock::BedrockServer upstream({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {{"motd", "Relay Map Flood Upstream"}},
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

    // A serverbound-only handler must not force structured decoding in the
    // opposite direction. Relay still validates every clientbound packet
    // strictly, without building 6 x 16,384 unused pixel objects.
    auto options = relayOptions(upstream.boundPort(), true);
    options.itemResourceDiagnostics = true;
    bedrock::Relay relay(std::move(options));
    std::atomic<int> joins {0};
    std::atomic<int> parseErrors {0};
    std::atomic<int> serverboundCallbacks {0};
    std::mutex diagnosticsMutex;
    std::vector<std::string> diagnostics;
    relay.onJoin([&](bedrock::RelayPlayer&, bedrock::BedrockNetworkClient&) {
        ++joins;
    });
    relay.onServerbound([&](bedrock::RelayPacketEvent&) {
        ++serverboundCallbacks;
    });
    relay.onParseError([&](const bedrock::RelayParseError&) {
        ++parseErrors;
    });
    relay.onError([&](const std::string& message) {
        errors.add("relay", message);
    });
    relay.onDiagnostic([&](const std::string& message) {
        std::lock_guard<std::mutex> lock(diagnosticsMutex);
        diagnostics.push_back(message);
    });
    relay.listen();

    auto downstream = bedrock::createNetworkClient(
        downstreamOptions(relay.live().boundPort())
    );
    std::atomic<int> registries {0};
    std::atomic<int> startGames {0};
    std::atomic<int> receivedMaps {0};
    std::atomic<int> slots {0};
    std::atomic<int> itemBearingPackets {0};
    std::atomic<int64_t> registrySentAtNs {0};
    std::atomic<int64_t> registryDeliveryMs {-1};
    std::atomic<bool> bytesMismatch {false};
    std::atomic<bool> missingRuntimeId {false};
    std::atomic<bool> downstreamDecodeFailed {false};
    std::atomic<bool> downstreamClosed {false};
    auto downstreamVariables = bedrock::makeProtoDefVariableStore();
    std::mutex downstreamStateMutex;
    std::unordered_map<int64_t, std::string> downstreamRegistry;
    std::vector<bedrock::VersionedGamePacket> observed;
    downstream.onAny([&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        const bool tracked =
            event.packet.name == "start_game" ||
            event.packet.name == "item_registry" ||
            event.packet.name == "creative_content" ||
            event.packet.name == "inventory_content" ||
            event.packet.name == "clientbound_map_item_data" ||
            event.packet.name == "inventory_slot" ||
            event.packet.name == "mob_equipment" ||
            event.packet.name == "add_item_entity";
        if (tracked) {
            std::lock_guard<std::mutex> lock(downstreamStateMutex);
            observed.push_back(event.packet);
        }

        if (event.packet.name == "start_game") {
            ++startGames;
            if (event.packet.fullPacket != startGame.fullPacket) {
                bytesMismatch = true;
            }
            return;
        }
        if (event.packet.name == "item_registry") {
            const auto sentAt = registrySentAtNs.load();
            if (sentAt != 0) {
                const auto receivedAt =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()
                    ).count();
                registryDeliveryMs = (receivedAt - sentAt) / 1'000'000;
            }
            ++registries;
            if (event.packet.fullPacket != registry.fullPacket) {
                bytesMismatch = true;
            }
        }
        if (event.packet.name == "clientbound_map_item_data") {
            const int index = receivedMaps.fetch_add(1);
            if (index < 0 || static_cast<std::size_t>(index) >= maps.size() ||
                event.packet.fullPacket !=
                    maps[static_cast<std::size_t>(index)].fullPacket) {
                bytesMismatch = true;
            }
            return;
        }

        const bool itemBearing =
            event.packet.name == "inventory_content" ||
            event.packet.name == "inventory_slot" ||
            event.packet.name == "mob_equipment" ||
            event.packet.name == "add_item_entity";
        if (itemBearing) ++itemBearingPackets;
        if (event.packet.name == "inventory_slot") ++slots;

        if (event.packet.name == "item_registry" || itemBearing) {
            try {
                bedrock::ProtoDefPacketDecoder decoder(version, downstreamVariables);
                const auto fields = decoder.decodePacketStrict(
                    event.packet.name,
                    event.packet.payload
                );
                std::lock_guard<std::mutex> lock(downstreamStateMutex);
                if (event.packet.name == "item_registry") {
                    std::unordered_map<std::size_t, std::string> names;
                    std::unordered_map<std::size_t, int64_t> runtimeIds;
                    for (const auto& field : fields) {
                        if (const auto index = decodedArrayIndex(
                                field.path,
                                "itemstates",
                                ".name"
                            ); index.has_value()) {
                            names[*index] = field.value;
                            continue;
                        }
                        const auto index = decodedArrayIndex(
                            field.path,
                            "itemstates",
                            ".runtime_id"
                        );
                        if (!index.has_value()) continue;
                        if (const auto runtimeId = decodedIntegerValue(field.value)) {
                            runtimeIds[*index] = *runtimeId;
                        }
                    }
                    for (const auto& [index, runtimeId] : runtimeIds) {
                        const auto name = names.find(index);
                        if (name != names.end()) {
                            downstreamRegistry[runtimeId] = name->second;
                        }
                    }
                } else {
                    for (const auto& field : fields) {
                        if (!pathEndsWith(field.path, ".network_id")) continue;
                        const auto networkId = decodedIntegerValue(field.value);
                        if (networkId.has_value() && *networkId != 0 &&
                            downstreamRegistry.find(*networkId) == downstreamRegistry.end()) {
                            missingRuntimeId = true;
                        }
                    }
                }
            } catch (const std::exception&) {
                downstreamDecodeFailed = true;
            }
        }
    });
    downstream.onClose([&]() { downstreamClosed = true; });
    downstream.onError([&](const std::string& message) {
        errors.add("downstream", message);
    });

    bool ok = true;
    ok &= check(
        registryDecodeElapsed < 2s,
        "full 1.21.100 item_registry strict decode exceeded two seconds"
    );
    const bool connected = downstream.connect();
    const bool ready = connected && waitFor([&]() {
        return joins.load() == 1 && hasUpstreamConnection &&
            relay.live().upstreamCount() == 1;
    });
    ok &= check(connected, "map-flood downstream failed to connect");
    ok &= check(ready, "map-flood relay did not become ready");

    if (ready) {
        downstream.write("tick_sync", tickValue(827100, 0));
    }
    const bool serverboundHandlerReady = ready && waitFor([&]() {
        return serverboundCallbacks.load() > 0;
    });
    ok &= check(
        serverboundHandlerReady,
        "serverbound-only regression handler was not exercised"
    );

    bedrock::BedrockServerConnection target;
    {
        std::lock_guard<std::mutex> lock(connectionMutex);
        target = upstreamConnection;
    }
    if (ready) upstream.sendPacket(target, startGame);
    const bool startGameReady = ready && waitFor([&]() {
        return startGames.load() == 1;
    });
    ok &= check(startGameReady, "start_game was not delivered before item_registry");

    if (startGameReady) {
        registrySentAtNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count();
        upstream.sendPacket(target, registry);
    }
    const bool paletteReady = ready && waitFor([&]() {
        return registries.load() == 1;
    }, 60s);
    ok &= check(paletteReady, "item_registry was not delivered before items");
    if (paletteReady) {
        ok &= check(
            registryDeliveryMs.load() >= 0 && registryDeliveryMs.load() < 6'000,
            "full 1.21.100 item_registry relay delivery exceeded six seconds"
        );
    }
    std::cerr << "[item-registry-regression] entries=" << expectedRegistryCount
              << " bytes=" << registry.fullPacket.size()
              << " fields=" << registryFields.size()
              << " strict_decode_ms=" << registryDecodeElapsed.count()
              << " relay_delivery_ms=" << registryDeliveryMs.load()
              << "\n";

    if (paletteReady) {
        upstream.queuePacket(target, creativeContent);
        upstream.queuePacket(target, inventoryContent);
        upstream.queuePackets(target, maps);
        upstream.queuePacket(target, inventorySlot);
        upstream.queuePacket(target, mobEquipment);
        upstream.queuePacket(target, addItemEntity);
        upstream.sendQueued(target);
    }
    std::vector<bedrock::VersionedGamePacket> expected {
        startGame,
        registry,
        creativeContent,
        inventoryContent
    };
    expected.insert(expected.end(), maps.begin(), maps.end());
    expected.push_back(inventorySlot);
    expected.push_back(mobEquipment);
    expected.push_back(addItemEntity);
    const bool floodDelivered = paletteReady && waitFor([&]() {
        std::lock_guard<std::mutex> lock(downstreamStateMutex);
        return observed.size() == expected.size() &&
            receivedMaps.load() == static_cast<int>(maps.size()) &&
            slots.load() == 1 && itemBearingPackets.load() == 4;
    }, 20s);
    ok &= check(floodDelivered, "map flood or following inventory item stalled");
    if (floodDelivered) {
        std::lock_guard<std::mutex> lock(downstreamStateMutex);
        for (std::size_t index = 0; index < expected.size(); ++index) {
            if (observed[index].name != expected[index].name ||
                observed[index].fullPacket != expected[index].fullPacket) {
                bytesMismatch = true;
                break;
            }
        }
        ok &= check(
            downstreamRegistry.size() == expectedRegistryCount,
            "downstream item runtime-ID table is incomplete"
        );
        for (const auto& [name, id] : expectedItemIds) {
            ok &= check(
                downstreamRegistry.find(id) != downstreamRegistry.end(),
                "downstream item runtime-ID table is missing " + name + "=" +
                    std::to_string(id)
            );
        }
    }
    ok &= check(!bytesMismatch.load(), "map/item flood changed packet bytes or order");
    ok &= check(!missingRuntimeId.load(), "item packet referenced an unknown runtime ID");
    ok &= check(!downstreamDecodeFailed.load(), "diagnostic downstream decode failed");
    ok &= check(parseErrors.load() == 0, "map/item flood raised a parse error");
    ok &= check(!downstreamClosed.load(), "map flood closed downstream session");
    ok &= check(
        relay.live().sessionCount() == 1 &&
            relay.live().upstreamCount() == 1 &&
            relay.live().finalSessionResetCount() == 0,
        "map flood reset the Relay session"
    );
    {
        std::lock_guard<std::mutex> lock(diagnosticsMutex);
        std::ostringstream joined;
        for (const auto& line : diagnostics) joined << line << '\n';
        const auto text = joined.str();
        ok &= check(
            text.find("item_registry count=" +
                std::to_string(expectedRegistryCount)) !=
                std::string::npos &&
            text.find("name=minecraft:firework_rocket present=true runtime_id=" +
                std::to_string(itemId("minecraft:firework_rocket"))) !=
                std::string::npos &&
            text.find("name=minecraft:diamond_sword present=true runtime_id=" +
                std::to_string(itemId("minecraft:diamond_sword"))) !=
                std::string::npos &&
            text.find("name=minecraft:netherite_sword present=true runtime_id=" +
                std::to_string(itemId("minecraft:netherite_sword"))) !=
                std::string::npos &&
            text.find("name=minecraft:sand present=true runtime_id=" +
                std::to_string(itemId("minecraft:sand"))) !=
                std::string::npos &&
            text.find("name=minecraft:shulker_box present=true runtime_id=" +
                std::to_string(itemId("minecraft:shulker_box"))) !=
                std::string::npos &&
            text.find("registry_present=false") == std::string::npos,
            "item/resource diagnostics did not prove palette consistency"
        );
    }
    ok &= check(errors.empty(), "map-flood callback error: " + errors.text());

    downstream.close("map-flood regression complete");
    relay.close("map-flood regression complete");
    upstream.close("map-flood regression complete");
    return ok;
}

bool checkResourcePackServerboundOrdering() {
    using Value = bedrock::ProtoDefValue;
    const std::string version = "1.21.100";
    const std::string packId = "11111111-2222-3333-4444-555555555555";
    const std::string packVersion = "1.0.0";
    const std::string packReference = packId + "_" + packVersion;
    const std::string secretContentKey = "do-not-log-content-key";
    const std::string cdnUrl =
        "https://cdn.example.invalid/pack.mcpack?sig=relay-secret-token";

    const auto zeroPacksInfo = encodedPacket(
        version,
        "resource_packs_info",
        Value::object({
            {"must_accept", Value::boolean(false)},
            {"has_addons", Value::boolean(false)},
            {"has_scripts", Value::boolean(false)},
            {"disable_vibrant_visuals", Value::boolean(false)},
            {"world_template", Value::object({
                {"uuid", Value::string("00000000-0000-0000-0000-000000000000")},
                {"version", Value::string("")}
            })},
            {"texture_packs", Value::array({})}
        })
    );
    const auto cdnPacksInfo = encodedPacket(
        version,
        "resource_packs_info",
        Value::object({
            {"must_accept", Value::boolean(true)},
            {"has_addons", Value::boolean(true)},
            {"has_scripts", Value::boolean(false)},
            {"disable_vibrant_visuals", Value::boolean(false)},
            {"world_template", Value::object({
                {"uuid", Value::string("00000000-0000-0000-0000-000000000000")},
                {"version", Value::string("")}
            })},
            {"texture_packs", Value::array({
                Value::object({
                    {"uuid", Value::string(packId)},
                    {"version", Value::string(packVersion)},
                    {"size", Value::uinteger(6)},
                    {"content_key", Value::string(secretContentKey)},
                    {"sub_pack_name", Value::string("")},
                    {"content_identity", Value::string(packId)},
                    {"has_scripts", Value::boolean(false)},
                    {"addon_pack", Value::boolean(false)},
                    {"rtx_enabled", Value::boolean(false)},
                    {"cdn_url", Value::string(cdnUrl)}
                })
            })}
        })
    );
    const auto dataInfo = encodedPacket(
        version,
        "resource_pack_data_info",
        Value::object({
            {"pack_id", Value::string(packId)},
            {"max_chunk_size", Value::uinteger(4)},
            {"chunk_count", Value::uinteger(2)},
            {"size", Value::uinteger(6)},
            {"hash", Value::bytes({0x10, 0x20, 0x30, 0x40})},
            {"is_premium", Value::boolean(false)},
            {"pack_type", Value::string("cached")}
        })
    );
    const auto chunkData = encodedPacket(
        version,
        "resource_pack_chunk_data",
        Value::object({
            {"pack_id", Value::string(packId)},
            {"chunk_index", Value::uinteger(0)},
            {"progress", Value::uinteger(4)},
            {"payload", Value::bytes({0xde, 0xad, 0xbe, 0xef})}
        })
    );
    const auto packStack = encodedPacket(
        version,
        "resource_pack_stack",
        Value::object({
            {"must_accept", Value::boolean(true)},
            {"behavior_packs", Value::array({})},
            {"resource_packs", Value::array({
                Value::object({
                    {"uuid", Value::string(packId)},
                    {"version", Value::string(packVersion)},
                    {"name", Value::string("")}
                })
            })},
            {"game_version", Value::string(version)},
            {"experiments", Value::array({})},
            {"experiments_previously_used", Value::boolean(false)},
            {"has_editor_packs", Value::boolean(false)}
        })
    );
    const std::vector<bedrock::VersionedGamePacket> expectedClientbound {
        zeroPacksInfo,
        cdnPacksInfo,
        dataInfo,
        chunkData,
        packStack
    };

    const auto haveAllPacks = encodedPacket(
        version,
        "resource_pack_client_response",
        Value::object({
            {"response_status", Value::string("have_all_packs")},
            {"resourcepackids", Value::array({Value::string(packReference)})}
        })
    );
    const auto chunkRequest = encodedPacket(
        version,
        "resource_pack_chunk_request",
        Value::object({
            {"pack_id", Value::string(packId)},
            {"chunk_index", Value::uinteger(0)}
        })
    );
    const auto completed = encodedPacket(
        version,
        "resource_pack_client_response",
        Value::object({
            {"response_status", Value::string("completed")},
            {"resourcepackids", Value::array({Value::string(packReference)})}
        })
    );
    const std::vector<bedrock::VersionedGamePacket> expectedServerbound {
        haveAllPacks,
        chunkRequest,
        completed
    };

    ErrorLog errors;
    std::mutex connectionMutex;
    bedrock::BedrockServerConnection upstreamConnection;
    std::atomic<bool> hasUpstreamConnection {false};
    std::atomic<bool> captureServerbound {false};
    std::mutex serverboundMutex;
    std::vector<bedrock::VersionedGamePacket> receivedServerbound;
    bedrock::BedrockServer upstream({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {{"motd", "Relay Resource Pack Ordering Upstream"}},
        .maxPlayers = 2,
        .offline = true,
        // Make the old immediate-response bypass deterministic: a queued
        // chunk request must not be overtaken while this batch is pending.
        .batchingInterval = 100
    });
    upstream.onConnect([&](const bedrock::BedrockServerConnection& connection) {
        std::lock_guard<std::mutex> lock(connectionMutex);
        upstreamConnection = connection;
        hasUpstreamConnection = true;
    });
    upstream.onAny([&](const bedrock::BedrockServerPacketEvent& event) {
        if (!captureServerbound.load() ||
            (event.packet.name != "resource_pack_chunk_request" &&
             event.packet.name != "resource_pack_client_response")) {
            return;
        }
        std::lock_guard<std::mutex> lock(serverboundMutex);
        receivedServerbound.push_back(event.packet);
    });
    upstream.listen();

    auto options = relayOptions(upstream.boundPort(), true);
    options.batchingInterval = 100;
    options.itemResourceDiagnostics = true;
    bedrock::Relay relay(std::move(options));
    std::atomic<int> joins {0};
    std::mutex diagnosticsMutex;
    std::vector<std::string> diagnostics;
    relay.onJoin([&](bedrock::RelayPlayer&, bedrock::BedrockNetworkClient&) {
        ++joins;
    });
    relay.onDiagnostic([&](const std::string& message) {
        std::lock_guard<std::mutex> lock(diagnosticsMutex);
        diagnostics.push_back(message);
    });
    relay.onError([&](const std::string& message) {
        errors.add("relay", message);
    });
    relay.listen();

    auto clientOptions = downstreamOptions(relay.live().boundPort());
    clientOptions.batchingIntervalMs = 100;
    clientOptions.autoResourcePackResponses = false;
    auto downstream = bedrock::createNetworkClient(std::move(clientOptions));
    std::atomic<bool> captureClientbound {false};
    std::mutex clientboundMutex;
    std::vector<bedrock::VersionedGamePacket> receivedClientbound;
    downstream.onAny([&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        if (!captureClientbound.load() ||
            (event.packet.name != "resource_packs_info" &&
             event.packet.name != "resource_pack_data_info" &&
             event.packet.name != "resource_pack_chunk_data" &&
             event.packet.name != "resource_pack_stack")) {
            return;
        }
        std::lock_guard<std::mutex> lock(clientboundMutex);
        receivedClientbound.push_back(event.packet);
    });
    downstream.onError([&](const std::string& message) {
        errors.add("downstream", message);
    });

    bool ok = true;
    const bool connected = downstream.connect();
    const bool ready = connected && waitFor([&]() {
        return joins.load() == 1 && relay.live().upstreamReady() &&
            hasUpstreamConnection.load();
    });
    ok &= check(connected, "resource-pack ordering downstream failed to connect");
    ok &= check(ready, "resource-pack ordering relay did not become ready");

    bedrock::BedrockServerConnection target;
    {
        std::lock_guard<std::mutex> lock(connectionMutex);
        target = upstreamConnection;
    }
    if (ready) {
        captureClientbound = true;
        upstream.queuePackets(target, expectedClientbound);
        upstream.sendQueued(target);
    }
    const bool clientboundDelivered = ready && waitFor([&]() {
        std::lock_guard<std::mutex> lock(clientboundMutex);
        return receivedClientbound.size() == expectedClientbound.size();
    });
    ok &= check(
        clientboundDelivered,
        "resource-pack clientbound variants were not delivered"
    );
    if (clientboundDelivered) {
        std::lock_guard<std::mutex> lock(clientboundMutex);
        std::string mismatch;
        ok &= check(
            packetSequenceEquals(receivedClientbound, expectedClientbound, mismatch),
            "Relay changed resource-pack clientbound bytes or order: " + mismatch
        );
    }

    if (ready) {
        captureServerbound = true;
        for (const auto& packet : expectedServerbound) {
            downstream.sendBuffer(packet.fullPacket);
        }
        downstream.sendQueued();
    }
    const bool serverboundDelivered = ready && waitFor([&]() {
        std::lock_guard<std::mutex> lock(serverboundMutex);
        return receivedServerbound.size() == expectedServerbound.size();
    });
    ok &= check(
        serverboundDelivered,
        "resource-pack serverbound variants were not delivered"
    );
    if (serverboundDelivered) {
        std::lock_guard<std::mutex> lock(serverboundMutex);
        std::string mismatch;
        ok &= check(
            packetSequenceEquals(receivedServerbound, expectedServerbound, mismatch),
            "Relay changed resource-pack serverbound bytes or order: " + mismatch
        );
    }

    {
        std::lock_guard<std::mutex> lock(diagnosticsMutex);
        std::ostringstream joined;
        for (const auto& line : diagnostics) joined << line << '\n';
        const auto text = joined.str();
        ok &= check(
            text.find("resource_packs_info count=0") != std::string::npos &&
                text.find("resource_packs_info count=1") != std::string::npos &&
                text.find("uuid=" + packId) != std::string::npos &&
                text.find("version=" + packVersion) != std::string::npos &&
                text.find("has_cdn_url=true") != std::string::npos &&
                text.find("have_all_packs") != std::string::npos &&
                text.find("completed") != std::string::npos,
            "resource-pack diagnostics omitted negotiation metadata"
        );
        ok &= check(
            text.find(secretContentKey) == std::string::npos &&
                text.find("relay-secret-token") == std::string::npos,
            "resource-pack diagnostics leaked content_key or CDN token"
        );
    }
    ok &= check(errors.empty(), "resource-pack ordering error: " + errors.text());

    downstream.close("resource-pack ordering regression complete");
    relay.close("resource-pack ordering regression complete");
    upstream.close("resource-pack ordering regression complete");
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
    bool ok = checkCapturedItemRegistry();
    ok = checkItemRegistryVersionBoundary() && ok;
    ok = checkMixedAuthenticationHandshake() && ok;
    ok = checkFullMapForwarding() && ok;
    ok = checkMapFloodAndItemOrdering() && ok;
    ok = checkResourcePackServerboundOrdering() && ok;
    ok = checkForwardRawPolicy() && ok;
    ok = checkDownstreamCloseLifecycle() && ok;
    if (ok) std::cout << "[LIVE-RELAY-REGRESSION-SMOKE] OK\n";
    return ok ? 0 : 1;
}
