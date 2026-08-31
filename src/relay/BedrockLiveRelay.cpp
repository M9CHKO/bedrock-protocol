#include <bedrock/relay/BedrockLiveRelay.hpp>

#include <bedrock/auth/NativeBedrockAuthflow.hpp>

#include <bedrock/BedrockKeyExchange.hpp>
#include <bedrock/LoginPacket.hpp>
#include <bedrock/Options.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protocol/VersionedPayloadReader.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace bedrock {

namespace {

std::mutex& relayLogMutex() {
    static std::mutex mutex;
    return mutex;
}

void relayLogLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(relayLogMutex());
    std::cout << line << "\n";
}

bool shouldLogRelayPacket(const std::string& direction, const std::string& name) {
    if (name == "player_auth_input" ||
        name == "move_entity" ||
        name == "move_player" ||
        name == "move_entity_delta" ||
        name == "level_chunk" ||
        name == "subchunk" ||
        name == "block_entity_data" ||
        name == "add_entity" ||
        name == "add_item_entity" ||
        name == "player_list" ||
        name == "update_block" ||
        name == "level_event" ||
        name == "text") {
        static std::mutex countersMutex;
        static std::unordered_map<std::string, uint64_t> counters;
        std::lock_guard<std::mutex> lock(countersMutex);
        auto& count = counters[direction + ":" + name];
        ++count;
        return count == 1 || (count % 40) == 0;
    }

    return true;
}

std::vector<std::string> jsonStringArrayField(const std::string& json, const std::string& key) {
    std::vector<std::string> out;
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return out;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return out;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return out;
    ++pos;

    while (pos < json.size()) {
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] != '"') break;
        ++pos;

        std::string value;
        bool escaped = false;
        for (; pos < json.size(); ++pos) {
            const char c = json[pos];
            if (escaped) {
                switch (c) {
                    case '"': value.push_back('"'); break;
                    case '\\': value.push_back('\\'); break;
                    case '/': value.push_back('/'); break;
                    case 'b': value.push_back('\b'); break;
                    case 'f': value.push_back('\f'); break;
                    case 'n': value.push_back('\n'); break;
                    case 'r': value.push_back('\r'); break;
                    case 't': value.push_back('\t'); break;
                    default: value.push_back(c); break;
                }
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') {
                ++pos;
                break;
            }
            value.push_back(c);
        }
        out.push_back(std::move(value));

        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
        if (pos < json.size() && json[pos] == ',') {
            ++pos;
            continue;
        }
        break;
    }
    return out;
}

std::string relayJsonStringOrEmpty(const std::string& json, const std::string& key) {
    try {
        return BedrockKeyExchange::jsonExtractString(json, key);
    } catch (const std::exception&) {
        return {};
    }
}

std::vector<std::string> loginIdentityChain(const std::string& identityJson) {
    auto chainJson = identityJson;
    const auto certificate = relayJsonStringOrEmpty(identityJson, "Certificate");
    if (!certificate.empty()) {
        chainJson = certificate;
    }
    return jsonStringArrayField(chainJson, "chain");
}

BedrockRelayDownstreamProfile downstreamProfileFromLogin(const LoginPacketData& login) {
    BedrockRelayDownstreamProfile profile;
    for (const auto& jwt : loginIdentityChain(login.identity)) {
        std::string payload;
        try {
            payload = BedrockKeyExchange::extractJwtPayloadJson(jwt);
        } catch (const std::exception&) {
            continue;
        }

        if (profile.displayName.empty()) {
            profile.displayName = relayJsonStringOrEmpty(payload, "displayName");
        }
        if (profile.xuid.empty()) {
            profile.xuid = relayJsonStringOrEmpty(payload, "XUID");
        }
        if (profile.identity.empty()) {
            profile.identity = relayJsonStringOrEmpty(payload, "identity");
        }
    }

    if (profile.displayName.empty()) {
        try {
            profile.displayName = BedrockKeyExchange::jsonExtractString(
                BedrockKeyExchange::extractJwtPayloadJson(login.client),
                "ThirdPartyName"
            );
        } catch (const std::exception&) {
        }
    }

    return profile;
}

std::string findFieldValue(
    const std::vector<ProtoDefField>& fields,
    const std::string& path
) {
    for (const auto& field : fields) {
        if (field.path == path) {
            return field.value;
        }
    }
    return {};
}

bool fieldIsTrue(const std::vector<ProtoDefField>& fields, const std::string& path) {
    const auto value = findFieldValue(fields, path);
    return value == "true" || value == "1";
}

const ProtoDefField* findField(
    const std::vector<ProtoDefField>& fields,
    const std::string& path
) {
    for (const auto& field : fields) {
        if (field.path == path) return &field;
    }
    return nullptr;
}

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::optional<int64_t> integerField(
    const std::vector<ProtoDefField>& fields,
    const std::string& path
) {
    const auto* field = findField(fields, path);
    if (!field) return std::nullopt;
    auto text = field->value;
    const auto slash = text.find('/');
    if (slash != std::string::npos) text.resize(slash);
    try {
        std::size_t consumed = 0;
        const auto value = std::stoll(text, &consumed, 10);
        if (consumed == text.size()) return value;
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

std::optional<std::size_t> indexedField(
    const std::string& path,
    const std::string& root,
    const std::string& suffix
) {
    const auto prefix = root + "[";
    if (path.rfind(prefix, 0) != 0) return std::nullopt;
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

std::vector<std::size_t> fieldIndexes(
    const std::vector<ProtoDefField>& fields,
    const std::string& root,
    const std::string& suffix
) {
    std::vector<std::size_t> out;
    for (const auto& field : fields) {
        const auto index = indexedField(field.path, root, suffix);
        if (!index.has_value() ||
            std::find(out.begin(), out.end(), *index) != out.end()) {
            continue;
        }
        out.push_back(*index);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool versionUsesItemRegistry(const std::string& version) {
    try {
        return ProtocolDefinition::forVersion(version).hasPacket("item_registry");
    } catch (const std::exception&) {
        return false;
    }
}

bool isItemBearingPacket(const std::string& name) {
    return name == "inventory_content" ||
        name == "inventory_slot" ||
        name == "mob_equipment" ||
        name == "add_item_entity";
}

bool isItemResourceDiagnosticPacket(const std::string& name) {
    return name == "resource_packs_info" ||
        name == "resource_pack_data_info" ||
        name == "resource_pack_chunk_data" ||
        name == "resource_pack_stack" ||
        name == "resource_pack_client_response" ||
        name == "resource_pack_chunk_request" ||
        name == "start_game" ||
        name == "item_registry" ||
        name == "creative_content" ||
        isItemBearingPacket(name);
}

bool needsStructuredDiagnostic(
    const std::string& version,
    const std::string& name
) {
    if (name == "start_game") {
        return !versionUsesItemRegistry(version);
    }
    return name != "creative_content";
}

std::vector<std::pair<int64_t, std::string>> paletteEntries(
    const std::vector<ProtoDefField>& fields
) {
    struct Entry {
        std::optional<int64_t> runtimeId;
        std::string name;
    };

    std::unordered_map<std::size_t, Entry> byIndex;
    std::vector<std::size_t> indexes;
    for (const auto& field : fields) {
        const auto nameIndex = indexedField(field.path, "itemstates", ".name");
        const auto runtimeIndex = indexedField(
            field.path,
            "itemstates",
            ".runtime_id"
        );
        const auto index = nameIndex.has_value() ? nameIndex : runtimeIndex;
        if (!index.has_value()) continue;

        auto [entry, inserted] = byIndex.try_emplace(*index);
        if (inserted) indexes.push_back(*index);
        if (nameIndex.has_value()) {
            entry->second.name = field.value;
        } else {
            auto text = field.value;
            if (const auto slash = text.find('/'); slash != std::string::npos) {
                text.resize(slash);
            }
            try {
                std::size_t consumed = 0;
                const auto value = std::stoll(text, &consumed, 10);
                if (consumed == text.size()) entry->second.runtimeId = value;
            } catch (const std::exception&) {
            }
        }
    }

    std::sort(indexes.begin(), indexes.end());
    std::vector<std::pair<int64_t, std::string>> out;
    out.reserve(indexes.size());
    for (const auto index : indexes) {
        const auto found = byIndex.find(index);
        if (found != byIndex.end() && found->second.runtimeId.has_value() &&
            !found->second.name.empty()) {
            out.emplace_back(*found->second.runtimeId, found->second.name);
        }
    }
    return out;
}

std::vector<std::string> itemPrefixes(
    const std::string& packetName,
    const std::vector<ProtoDefField>& fields
) {
    std::vector<std::string> out;
    for (const auto& field : fields) {
        if (!endsWith(field.path, ".network_id")) continue;
        const auto prefix = field.path.substr(
            0,
            field.path.size() - std::string(".network_id").size()
        );
        const bool allowed = packetName == "inventory_content"
            ? prefix.rfind("input[", 0) == 0 || prefix == "storage_item"
            : packetName == "inventory_slot"
                ? prefix == "item" || prefix == "storage_item"
                : prefix == "item";
        if (allowed && std::find(out.begin(), out.end(), prefix) == out.end()) {
            out.push_back(prefix);
        }
    }
    return out;
}

bool itemNbtPresent(
    const std::vector<ProtoDefField>& fields,
    const std::string& prefix
) {
    const auto nbtPrefix = prefix + ".extra.nbt";
    for (const auto& field : fields) {
        if (field.path.rfind(nbtPrefix, 0) == 0 && field.type != "void") {
            return true;
        }
    }
    return false;
}

std::string packetFingerprint(const VersionedGamePacket& packet);

float readF32LE(const std::vector<uint8_t>& bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        return 0.0f;
    }
    uint32_t raw =
        static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1]) << 8u) |
        (static_cast<uint32_t>(bytes[offset + 2]) << 16u) |
        (static_cast<uint32_t>(bytes[offset + 3]) << 24u);
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(raw), "float size mismatch");
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

unsigned __int128 readVarUInt128Loose(const std::vector<uint8_t>& bytes, std::size_t& offset) {
    unsigned __int128 value = 0;
    int shift = 0;
    while (offset < bytes.size() && shift < 128) {
        const auto byte = bytes[offset++];
        value |= static_cast<unsigned __int128>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            return value;
        }
        shift += 7;
    }
    return value;
}

bool hasFlag128(unsigned __int128 flags, int bit) {
    return (flags & (static_cast<unsigned __int128>(1) << bit)) != 0;
}

std::string playerAuthInputSummary(const VersionedGamePacket& packet) {
    if (packet.payload.size() < 33) {
        return packetFingerprint(packet);
    }

    // packet_player_auth_input starts with:
    // pitch, yaw, position(vec3f), move_vector(vec2f), head_yaw, input_data.
    std::size_t offset = 0;
    const auto pitch = readF32LE(packet.payload, offset); offset += 4;
    const auto yaw = readF32LE(packet.payload, offset); offset += 4;
    const auto x = readF32LE(packet.payload, offset); offset += 4;
    const auto y = readF32LE(packet.payload, offset); offset += 4;
    const auto z = readF32LE(packet.payload, offset); offset += 4;
    offset += 8; // move_vector
    const auto headYaw = readF32LE(packet.payload, offset); offset += 4;
    const auto inputFlags = readVarUInt128Loose(packet.payload, offset);

    struct FlagName {
        int bit;
        const char* name;
    };
    static constexpr FlagName flags[] = {
        {32, "start_gliding"},
        {33, "stop_gliding"},
        {34, "item_interact"},
        {35, "block_action"},
        {36, "item_stack_request"},
        {39, "missed_swing"},
        {54, "start_using_item"},
        {55, "camera_relative_movement_enabled"},
        {58, "start_spin_attack"},
        {59, "stop_spin_attack"}
    };

    std::ostringstream out;
    out << packetFingerprint(packet)
        << " pos=(" << x << "," << y << "," << z << ")"
        << " pitch=" << pitch
        << " yaw=" << yaw
        << " head_yaw=" << headYaw
        << " input_bytes=" << offset
        << " flags=[";
    bool any = false;
    for (const auto& flag : flags) {
        if (!hasFlag128(inputFlags, flag.bit)) {
            continue;
        }
        if (any) out << ",";
        out << flag.name;
        any = true;
    }
    out << "]";
    return out.str();
}

uint64_t fnv1a64(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto byte : bytes) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string packetFingerprint(const VersionedGamePacket& packet) {
    std::ostringstream out;
    out << " full=" << packet.fullPacket.size()
        << " payload=" << packet.payload.size()
        << " hash=0x" << std::hex << fnv1a64(packet.fullPacket) << std::dec;
    return out.str();
}

std::string packetSummary(const std::string& version, const VersionedGamePacket& packet) {
    try {
        if (packet.name == "level_chunk") {
            const auto chunk = VersionedPayloadReader::readLevelChunk(packet);
            std::ostringstream out;
            out << " x=" << chunk.chunkX
                << " z=" << chunk.chunkZ
                << " dim=" << chunk.dimension
                << " subchunks=" << chunk.subChunkCount
                << " cache=" << (chunk.cacheEnabled ? 1 : 0)
                << " bytes=" << packet.payload.size();
            return out.str();
        }

        if (packet.name == "request_chunk_radius") {
            VersionedPayloadCursor cursor(packet.payload);
            std::ostringstream out;
            out << " radius=" << cursor.readVarInt();
            if (!cursor.eof()) {
                out << " max=" << static_cast<int>(cursor.readU8());
            }
            return out.str();
        }

        if (packet.name == "subchunk_request") {
            ProtoDefPacketDecoder decoder(version);
            const auto fields = decoder.decodePacket(packet.name, packet.payload);
            std::ostringstream out;
            out << " dim=" << findFieldValue(fields, "dimension")
                << " origin=(" << findFieldValue(fields, "origin.x")
                << "," << findFieldValue(fields, "origin.y")
                << "," << findFieldValue(fields, "origin.z") << ")"
                << " bytes=" << packet.payload.size();
            return out.str();
        }

        if (packet.name == "player_auth_input") {
            static std::atomic<uint64_t> authLogCounter {0};
            const auto count = ++authLogCounter;
            if ((count % 40) != 1) {
                return {};
            }

            (void)version;
            return playerAuthInputSummary(packet);
        }

        if (packet.name == "item_stack_request" ||
            packet.name == "inventory_transaction" ||
            packet.name == "interact" ||
            packet.name == "animate" ||
            packet.name == "mob_equipment" ||
            packet.name == "player_action") {
            return packetFingerprint(packet);
        }
    } catch (const std::exception& e) {
        return std::string(" decode_error=") + e.what() +
            packetFingerprint(packet);
    }

    return {};
}

} // namespace

enum class RelaySessionLifecycle {
    Open,
    Resetting,
    Closing,
    Closed
};

enum class RelayDownstreamPhase {
    Negotiating,
    Game,
    Closed
};

enum class RelayUpstreamPhase {
    Idle,
    Negotiating,
    Game,
    Closed
};

struct BedrockLiveRelay::Session {
    mutable std::mutex mutex;
    std::string id;
    BedrockServerConnection downstream;
    BedrockNetworkClientOptions upstreamOptions;
    std::shared_ptr<BedrockNetworkClient> upstream;
    std::thread upstreamThread;
    std::vector<VersionedGamePacket> pendingServerbound;
    std::vector<VersionedGamePacket> pendingPostSpawnServerbound;
    std::vector<VersionedGamePacket> pendingClientbound;
    std::vector<VersionedGamePacket> heldClientboundLevelChunks;
    std::chrono::steady_clock::time_point clientboundChunkReleaseAt {};
    BedrockRelayDownstreamProfile downstreamProfile;
    RelaySessionLifecycle lifecycle = RelaySessionLifecycle::Open;
    RelayDownstreamPhase downstreamPhase = RelayDownstreamPhase::Negotiating;
    RelayUpstreamPhase upstreamPhase = RelayUpstreamPhase::Idle;
    bool clientboundStartGameSent = false;
    bool clientboundPlayerSpawnSeen = false;
    bool upstreamDisconnectRequested = false;
    uint64_t diagnosticSequence = 0;
    std::size_t diagnosticItemRegistryCount = 0;
    bool diagnosticStartGameSeen = false;
    bool diagnosticItemBearingSeen = false;
    ProtoDefVariableStorePtr diagnosticVariables = makeProtoDefVariableStore();
    std::unordered_map<int64_t, std::string> diagnosticItemsByRuntimeId;
};

namespace {

bool relaySessionAcceptsPackets(const auto& session) {
    return session.lifecycle == RelaySessionLifecycle::Open;
}

bool relaySessionIsClosing(const auto& session) {
    return session.lifecycle == RelaySessionLifecycle::Closing ||
        session.lifecycle == RelaySessionLifecycle::Closed;
}

bool relayUpstreamStarted(const auto& session) {
    return session.upstreamPhase == RelayUpstreamPhase::Negotiating ||
        session.upstreamPhase == RelayUpstreamPhase::Game;
}

bool relayUpstreamReady(const auto& session) {
    return session.upstreamPhase == RelayUpstreamPhase::Game;
}

} // namespace

void detail::applyRelayDownstreamIdentity(
    BedrockLiveRelayOptions& options,
    const BedrockRelayDownstreamProfile& profile
) {
    options.upstream.username = options.useDownstreamDisplayNameForUpstreamUsername
        ? profile.displayName
        : profile.xuid;
    options.upstream.profile = options.upstream.username;
}

BedrockLiveRelay::BedrockLiveRelay(BedrockLiveRelayOptions options)
    : options_(normalizeOptions(std::move(options))),
      baseUpstreamOptions_(options_.upstream),
      server_(std::make_unique<BedrockServer>(options_.server)) {}

BedrockLiveRelay::~BedrockLiveRelay() {
    close();
}

ServerListenResult BedrockLiveRelay::listen() {
    if (!closed_.exchange(false)) {
        return {
            .host = options_.server.host,
            .port = options_.server.port
        };
    }

    server_->onConnect([this](const BedrockServerConnection& connection) {
        const auto id = sessionId(connection);
        std::shared_ptr<Session> session;
        std::vector<std::shared_ptr<Session>> replacedSessions;
        bool rejected = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (options_.forceSingle && !sessions_.empty()) {
                if (options_.replaceExisting) {
                    replacedSessions.reserve(sessions_.size());
                    for (const auto& [existingId, existingSession] : sessions_) {
                        (void) existingId;
                        replacedSessions.push_back(existingSession);
                    }
                    sessions_.clear();
                    primarySessionId_.clear();
                } else {
                    rejectedConnections_.insert(id);
                    rejected = true;
                }
            }
            if (!rejected) {
                session = std::make_shared<Session>();
                session->id = id;
                session->downstream = connection;
                session->upstreamOptions = baseUpstreamOptions_;
                sessions_[id] = session;
                if (primarySessionId_.empty()) primarySessionId_ = id;
            }
        }

        if (rejected) {
            if (options_.logging) {
                relayLogLine("[relay] dropping connection as single client relay: " + id);
            }
            server_->closeConnection(connection);
            emitStatus();
            return;
        }

        for (const auto& replaced : replacedSessions) {
            const std::string reason =
                "replaced by a new downstream connection";
            (void) resetRelaySession(replaced, reason, false);
            server_->closeConnection(replaced->downstream);
            for (auto& handler : disconnectHandlers_) {
                handler(replaced->downstream);
            }
            if (options_.logging) {
                relayLogLine(
                    "[relay] replaced stale session " + replaced->id +
                    " with " + id
                );
            }
        }

        // RelayPlayer installs its own one-shot join listener in the
        // constructor, before Relay emits `connect`. Register this internal
        // boundary before public connect handlers can add their Player join
        // listeners, preserving that ordering for queued clientbound packets.
        std::weak_ptr<Session> weakSession = session;
        connection.onJoin([this, weakSession]() {
            const auto joinedSession = weakSession.lock();
            if (joinedSession) handleDownstreamJoin(joinedSession);
        });

        for (auto& handler : connectHandlers_) {
            handler(connection);
        }
        emitStatus();
    });

    server_->onDisconnect([this](const BedrockServerConnection& connection) {
        const auto id = sessionId(connection);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto rejected = rejectedConnections_.find(id);
            if (rejected != rejectedConnections_.end()) {
                rejectedConnections_.erase(rejected);
                return;
            }
        }

        const auto session = findSession(connection);
        if (!session) return;
        if (!removeRelaySession(session, "downstream disconnected")) {
            return;
        }
        for (auto& handler : disconnectHandlers_) {
            handler(connection);
        }
        emitStatus();
    });

    server_->onLogin([this](const BedrockServerPacketEvent& event) {
        const auto session = findSession(event.connection);
        if (!session) return;
        bool alreadyStarted = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->downstreamPhase = RelayDownstreamPhase::Negotiating;
            alreadyStarted = relayUpstreamStarted(*session) ||
                session->upstream != nullptr;
        }
        if (alreadyStarted) {
            (void) resetRelaySession(session, "downstream reconnect", true);
        }
        captureDownstreamClientData(session, event.packet);
        startUpstream(session);
    });

    server_->onAny([this](const BedrockServerPacketEvent& event) {
        handleDownstreamPacket(event);
    });

    const auto address = server_->listen();
    listening_.store(true);
    emitStatus();
    return address;
}

int BedrockLiveRelay::run() {
    listen();
    std::unique_lock<std::mutex> lock(mutex_);
    closedCv_.wait(lock, [this]() {
        return closed_.load();
    });
    return 0;
}

void BedrockLiveRelay::close(const std::string& reason) {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true)) {
        return;
    }

    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions.reserve(sessions_.size());
        for (const auto& [id, session] : sessions_) {
            (void) id;
            sessions.push_back(session);
        }
    }
    for (const auto& session : sessions) {
        (void) resetRelaySession(session, reason, false);
    }
    if (server_) {
        server_->close();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.clear();
        rejectedConnections_.clear();
        primarySessionId_.clear();
    }

    listening_.store(false);
    emitStatus();
    closedCv_.notify_all();
}

void BedrockLiveRelay::onConnect(ConnectionHandler handler) {
    connectHandlers_.push_back(std::move(handler));
}

void BedrockLiveRelay::onJoin(ConnectionHandler handler) {
    joinHandlers_.push_back(std::move(handler));
}

void BedrockLiveRelay::onUpstreamJoin(UpstreamJoinHandler handler) {
    upstreamJoinHandlers_.push_back(std::move(handler));
}

void BedrockLiveRelay::onDisconnect(ConnectionHandler handler) {
    disconnectHandlers_.push_back(std::move(handler));
}

void BedrockLiveRelay::onClientbound(PacketHandler handler) {
    clientboundHandlers_.push_back(std::move(handler));
}

void BedrockLiveRelay::onServerbound(PacketHandler handler) {
    serverboundHandlers_.push_back(std::move(handler));
}

void BedrockLiveRelay::on(const std::string& direction, PacketHandler handler) {
    if (direction == "clientbound") {
        onClientbound(std::move(handler));
        return;
    }
    if (direction == "serverbound") {
        onServerbound(std::move(handler));
        return;
    }
    throw std::runtime_error("unknown relay direction: " + direction);
}

void BedrockLiveRelay::onError(ErrorHandler handler) {
    errorHandlers_.push_back(std::move(handler));
}

void BedrockLiveRelay::onStatus(StatusHandler handler) {
    statusHandlers_.push_back(std::move(handler));
}

void BedrockLiveRelay::onDiagnostic(DiagnosticHandler handler) {
    diagnosticHandlers_.push_back(std::move(handler));
}

void BedrockLiveRelay::onMsaCode(MsaCodeHandler handler) {
    std::lock_guard<std::mutex> lock(msaCodeHandlersMutex_);
    msaCodeHandlers_.push_back(std::move(handler));
}

bool BedrockLiveRelay::listening() const {
    return listening_.load();
}

bool BedrockLiveRelay::downstreamJoined() const {
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, session] : sessions_) {
            (void) id;
            sessions.push_back(session);
        }
    }
    for (const auto& session : sessions) {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (relaySessionAcceptsPackets(*session) &&
            session->downstreamPhase == RelayDownstreamPhase::Game) return true;
    }
    return false;
}

bool BedrockLiveRelay::upstreamStarted() const {
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, session] : sessions_) {
            (void) id;
            sessions.push_back(session);
        }
    }
    for (const auto& session : sessions) {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (relaySessionAcceptsPackets(*session) &&
            relayUpstreamStarted(*session)) return true;
    }
    return false;
}

bool BedrockLiveRelay::upstreamReady() const {
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, session] : sessions_) {
            (void) id;
            sessions.push_back(session);
        }
    }
    for (const auto& session : sessions) {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (relaySessionAcceptsPackets(*session) &&
            relayUpstreamReady(*session)) return true;
    }
    return false;
}

uint16_t BedrockLiveRelay::boundPort() const {
    return server_ ? server_->boundPort() : 0;
}

const BedrockLiveRelayOptions& BedrockLiveRelay::options() const {
    return options_;
}

BedrockServer& BedrockLiveRelay::server() {
    return *server_;
}

BedrockNetworkClient* BedrockLiveRelay::upstream() {
    return upstreamShared().get();
}

BedrockNetworkClient* BedrockLiveRelay::upstream(
    const BedrockServerConnection& connection
) {
    return upstreamShared(connection).get();
}

std::shared_ptr<BedrockNetworkClient> BedrockLiveRelay::upstreamShared() {
    const auto session = primarySession();
    if (!session) return nullptr;
    std::lock_guard<std::mutex> lock(session->mutex);
    return session->upstream;
}

std::shared_ptr<BedrockNetworkClient> BedrockLiveRelay::upstreamShared(
    const BedrockServerConnection& connection
) {
    const auto session = findSession(connection);
    if (!session) return nullptr;
    std::lock_guard<std::mutex> lock(session->mutex);
    return session->upstream;
}

std::size_t BedrockLiveRelay::sessionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

std::size_t BedrockLiveRelay::upstreamCount() const {
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, session] : sessions_) {
            (void) id;
            sessions.push_back(session);
        }
    }
    std::size_t count = 0;
    for (const auto& session : sessions) {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->upstream) ++count;
    }
    return count;
}

void BedrockLiveRelay::closeUpstreamConnection(
    const BedrockServerConnection& connection,
    const std::string& reason
) {
    const auto session = findSession(connection);
    if (!session) {
        throw std::runtime_error(
            "unable to close non-open connection " + sessionId(connection)
        );
    }
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!session->upstream) {
            throw std::runtime_error(
                "unable to close non-open connection " + session->id
            );
        }
    }
    (void) resetRelaySession(session, reason, false);
    // Node's upstream `close` listener disconnects the matching downstream
    // Player. The relay session is already closed before the protocol-level
    // disconnect is queued, so no backend packet can race that delay.
    server_->disconnect(connection, "Backend server closed connection");
    emitStatus();
}

void BedrockLiveRelay::disconnectDownstream(
    const BedrockServerConnection& connection,
    const std::string& reason
) {
    const auto session = findSession(connection);
    if (!session) return;
    (void) resetRelaySession(session, reason, false);
    server_->disconnect(connection, reason);
    emitStatus();
}

uint64_t BedrockLiveRelay::finalSessionResetCount() const noexcept {
    return finalSessionResetCount_.load();
}

std::string BedrockLiveRelay::sessionId(
    const BedrockServerConnection& connection
) {
    return connection.address + ":" + std::to_string(connection.port) + "#" +
        std::to_string(connection.clientGuid);
}

BedrockLiveRelayOptions BedrockLiveRelay::normalizeOptions(BedrockLiveRelayOptions options) {
    (void) validateVersion(options.server.version);

    // These packets belong to transport/authentication sessions, not the game
    // packet bridge. Keep the old switches source-compatible but never allow
    // either side's key exchange to be injected into the other side.
    options.filterDownstreamHandshakePackets = true;
    options.skipClientboundHandshake = true;

    // An omitted destination version inherits the relay server version. An
    // explicit pseudo-version is not part of options.Versions and is rejected.
    if (options.upstream.version.empty()) {
        options.upstream.version = options.server.version;
    } else {
        (void) validateVersion(options.upstream.version);
    }
    if (options.upstream.version != options.server.version) {
        throw std::runtime_error("live relay currently requires matching server/upstream versions");
    }

    // JS Relay lets the backend drive the resource-pack exchange. The local
    // server only performs login/encryption and then forwards backend
    // resource_packs_info/resource_pack_stack to the downstream client.
    options.server.autoResourcePacks = false;
    options.skipClientboundResourcePacks = false;

    options.upstream.autoResourcePackResponses = false;
    options.upstream.autoInitPlayer = false;
    if (options.upstream.profile.empty()) {
        options.upstream.profile = options.upstream.username.empty()
            ? std::string("RelayBot")
            : options.upstream.username;
    }
    if (options.upstream.username.empty()) {
        options.upstream.username = options.upstream.profile;
    }
    return options;
}

bool BedrockLiveRelay::isDownstreamHandshakePacket(const std::string& name) {
    return name == "request_network_settings" ||
        name == "login" ||
        name == "client_to_server_handshake";
}

bool BedrockLiveRelay::isClientboundResourcePackPacket(const std::string& name) {
    return name == "resource_packs_info" ||
        name == "resource_pack_stack" ||
        name == "resource_pack_data_info" ||
        name == "resource_pack_chunk_data";
}

bool BedrockLiveRelay::isClientboundHandshakePacket(const std::string& name) {
    return name == "network_settings" ||
        name == "server_to_client_handshake";
}

bool BedrockLiveRelay::isPlayStatusLoginSuccess(const VersionedGamePacket& packet) {
    if (packet.name != "play_status" || packet.payload.size() < 4) {
        return false;
    }

    return VersionedPayloadReader::readPlayStatus(packet).status == 0;
}

bool BedrockLiveRelay::isPlayStatusPlayerSpawn(
    const std::string& version,
    const VersionedGamePacket& packet
) {
    if (packet.name != "play_status") {
        return false;
    }

    (void)version;
    if (packet.payload.size() < 4) {
        return false;
    }

    return VersionedPayloadReader::readPlayStatus(packet).status == 3;
}

void BedrockLiveRelay::emitError(const std::string& message) {
    for (auto& handler : errorHandlers_) {
        handler(message);
    }
}

void BedrockLiveRelay::emitStatus() {
    BedrockLiveRelayStatus status;
    status.listening = listening_.load();
    status.boundPort = boundPort();

    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions.reserve(sessions_.size());
        for (const auto& [id, session] : sessions_) {
            (void) id;
            sessions.push_back(session);
        }
    }
    for (const auto& session : sessions) {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session)) continue;
        ++status.downstreamConnections;
        if (session->downstreamPhase == RelayDownstreamPhase::Game) {
            ++status.downstreamJoinedCount;
        }
        if (relayUpstreamStarted(*session)) ++status.upstreamStartedCount;
        if (relayUpstreamReady(*session)) ++status.upstreamReadyCount;
    }
    status.downstreamJoined = status.downstreamJoinedCount != 0;
    status.upstreamStarted = status.upstreamStartedCount != 0;
    status.upstreamReady = status.upstreamReadyCount != 0;

    for (auto& handler : statusHandlers_) {
        handler(status);
    }
}

void BedrockLiveRelay::emitDiagnostic(const std::string& message) {
    if (!options_.itemResourceDiagnostics) return;
    const auto line = "[relay-diagnostic] " + message;
    relayLogLine(line);
    const auto handlers = diagnosticHandlers_;
    for (auto& handler : handlers) {
        handler(line);
    }
}

bool BedrockLiveRelay::emitMsaCode(
    const XboxDeviceCodeInfo& code,
    const BedrockServerConnection& connection
) {
    std::vector<MsaCodeHandler> handlers;
    {
        std::lock_guard<std::mutex> lock(msaCodeHandlersMutex_);
        handlers = msaCodeHandlers_;
    }
    for (auto& handler : handlers) {
        handler(code, connection);
    }
    return !handlers.empty();
}

std::shared_ptr<BedrockLiveRelay::Session> BedrockLiveRelay::findSession(
    const BedrockServerConnection& connection
) const {
    const auto id = sessionId(connection);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sessions_.find(id);
    return found == sessions_.end() ? nullptr : found->second;
}

std::shared_ptr<BedrockLiveRelay::Session> BedrockLiveRelay::primarySession() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (primarySessionId_.empty()) return nullptr;
    const auto found = sessions_.find(primarySessionId_);
    return found == sessions_.end() ? nullptr : found->second;
}

void BedrockLiveRelay::captureDownstreamClientData(
    const std::shared_ptr<Session>& session,
    const VersionedGamePacket& packet
) {
    bool alreadyStarted = false;
    bool hasClientData = false;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        alreadyStarted = relayUpstreamStarted(*session);
        hasClientData = !session->upstreamOptions.clientDataJson.empty();
    }
    if (!options_.forwardDownstreamClientData ||
        alreadyStarted || hasClientData) {
        return;
    }

    try {
        auto login = LoginPacketCodec::decode(packet.fullPacket);
        auto clientDataJson = BedrockKeyExchange::extractJwtPayloadJson(login.client);
        auto downstreamProfile = downstreamProfileFromLogin(login);
        BedrockRelayDownstreamProfile profileSnapshot;

        // relay.js assigns this ternary result unconditionally. Keep an empty
        // string rather than falling back to the prior RelayBot identity.
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->upstreamOptions.clientDataJson = std::move(clientDataJson);
            session->downstreamProfile = std::move(downstreamProfile);
            session->upstreamOptions.username =
                options_.useDownstreamDisplayNameForUpstreamUsername
                    ? session->downstreamProfile.displayName
                    : session->downstreamProfile.xuid;
            session->upstreamOptions.profile = session->upstreamOptions.username;
            profileSnapshot = session->downstreamProfile;
        }

        // Preserve the original single-session inspection surface for the
        // first active Player while all actual connection work uses the
        // session-local options above.
        bool publishPrimary = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            publishPrimary = primarySessionId_ == session->id;
        }
        if (publishPrimary) {
            std::lock_guard<std::mutex> lock(session->mutex);
            options_.upstream = session->upstreamOptions;
        }

        if (options_.logging) {
            std::ostringstream out;
            out << "[relay] downstream profile"
                << " session=" << session->id
                << " name=" << profileSnapshot.displayName
                << " xuid=" << profileSnapshot.xuid;
            relayLogLine(out.str());
        }
    } catch (const std::exception& e) {
        emitError("[relay] failed to forward downstream clientData: " + std::string(e.what()));
    }
}

void BedrockLiveRelay::handleDownstreamJoin(
    const std::shared_ptr<Session>& session
) {
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session)) return;
        session->downstreamPhase = RelayDownstreamPhase::Game;
    }

    for (auto& handler : joinHandlers_) {
        handler(session->downstream);
    }

    std::vector<VersionedGamePacket> queuedClientbound;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        queuedClientbound = std::move(session->pendingClientbound);
        session->pendingClientbound.clear();
    }
    for (const auto& packet : queuedClientbound) {
        forwardClientbound(session, packet);
    }

    emitStatus();
}

void BedrockLiveRelay::resolveUpstreamRealm(
    BedrockNetworkClientOptions& upstreamOptions
) {
    if (!static_cast<bool>(options_.realms)) return;

    NativeBedrockAuthflowOptions authOptions {
        .username = upstreamOptions.username,
        .profilesFolder = upstreamOptions.profilesFolder,
        .authTitle = upstreamOptions.authTitle,
        .deviceType = upstreamOptions.deviceType,
        .flow = upstreamOptions.flow,
        .forceRefresh = upstreamOptions.forceRefresh,
        .msalConfig = upstreamOptions.msalConfig,
        .password = upstreamOptions.password,
        .onMsaCode = upstreamOptions.onMsaCode,
        .httpClientFactory = upstreamOptions.httpClientFactory
    };
    validateNativeBedrockAuthflowOptions(authOptions);
    upstreamOptions.profilesFolder = authOptions.profilesFolder;
    upstreamOptions.authTitle = authOptions.authTitle;
    upstreamOptions.deviceType = authOptions.deviceType;
    upstreamOptions.flow = authOptions.flow;

    validateNativeBedrockAuthflowPresence(authOptions);
    const auto effectiveCacheRoot = initializeNativeBedrockAuthCacheRoot(
        authOptions.profilesFolder
    );
    auto runtime = createNativeBedrockAuthflow(
        authOptions,
        effectiveCacheRoot
    );
    upstreamOptions.authflow = std::move(runtime.authflow);
    auto address = resolveBedrockRealmAddress(
        upstreamOptions.authflow,
        options_.realms
    );
    upstreamOptions.host = std::move(address.host);
    upstreamOptions.port = address.port;
}

bool BedrockLiveRelay::resetRelaySession(
    const std::shared_ptr<Session>& session,
    const std::string& reason,
    bool retainDownstream
) {
    std::shared_ptr<BedrockNetworkClient> upstream;
    std::thread upstreamThread;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (retainDownstream) {
            if (session->lifecycle != RelaySessionLifecycle::Open) {
                return false;
            }
            session->lifecycle = RelaySessionLifecycle::Resetting;
        } else {
            if (relaySessionIsClosing(*session)) {
                return false;
            }
            session->lifecycle = RelaySessionLifecycle::Closing;
            session->downstreamPhase = RelayDownstreamPhase::Closed;
            ++finalSessionResetCount_;
        }
        upstream = session->upstream;
        if (session->upstreamThread.joinable()) {
            upstreamThread = std::move(session->upstreamThread);
        }
        session->upstreamPhase = RelayUpstreamPhase::Closed;
        // Suppress the upstream close callback caused by this intentional
        // reset. Unexpected backend closes still request a downstream
        // disconnect in the callback below.
        session->upstreamDisconnectRequested = true;
        session->pendingServerbound.clear();
        session->pendingPostSpawnServerbound.clear();
        session->pendingClientbound.clear();
        session->heldClientboundLevelChunks.clear();
        session->clientboundChunkReleaseAt = {};
        session->clientboundStartGameSent = false;
        session->clientboundPlayerSpawnSeen = false;
        if (retainDownstream) {
            session->upstreamOptions = baseUpstreamOptions_;
            session->downstreamProfile = {};
        }
    }

    // Packet handlers are serialized through this mutex. Marking the session
    // first makes newly arriving packets stop at applyHandlers(); taking the
    // mutex here drains any callback that was already in progress.
    {
        std::lock_guard<std::recursive_mutex> dispatchLock(
            handlerDispatchMutex_
        );
    }

    // A downstream departure is a Bedrock session departure, not merely a
    // local transport teardown. Notify the backend before RakNet shutdown so
    // account/session state is released immediately and a prompt reconnect
    // cannot be held behind the previous login. BedrockNetworkClient::close()
    // intentionally sends no game packet; disconnect() sends the protocol
    // packet synchronously and then performs the same bounded RakNet close.
    if (upstream) {
        try {
            upstream->disconnect(reason, true);
        } catch (const std::exception& error) {
            // Teardown must still complete if a version-specific disconnect
            // encoder fails before RakNet shutdown.
            emitError(
                "[upstream disconnect] " + std::string(error.what())
            );
            upstream->close(reason);
        }
    }
    if (upstreamThread.joinable()) {
        if (upstreamThread.get_id() == std::this_thread::get_id()) {
            upstreamThread.detach();
        } else {
            upstreamThread.join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->upstream == upstream) session->upstream.reset();
        if (retainDownstream &&
            session->lifecycle == RelaySessionLifecycle::Resetting) {
            session->lifecycle = RelaySessionLifecycle::Open;
            session->upstreamPhase = RelayUpstreamPhase::Idle;
            session->upstreamDisconnectRequested = false;
        } else if (!retainDownstream &&
                   session->lifecycle == RelaySessionLifecycle::Closing) {
            session->lifecycle = RelaySessionLifecycle::Closed;
        }
    }

    if (options_.logging) {
        relayLogLine(
            "[relay] session reset " + session->id + ": " + reason
        );
    }
    return true;
}

bool BedrockLiveRelay::removeRelaySession(
    const std::shared_ptr<Session>& session,
    const std::string& reason
) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sessions_.find(session->id);
        if (found == sessions_.end() || found->second != session) {
            return false;
        }
        sessions_.erase(found);
        if (primarySessionId_ == session->id) {
            primarySessionId_ = sessions_.empty()
                ? std::string()
                : sessions_.begin()->first;
        }
    }
    (void) resetRelaySession(session, reason, false);
    return true;
}

void BedrockLiveRelay::startUpstream(const std::shared_ptr<Session>& session) {
    BedrockNetworkClientOptions upstreamOptions;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session) ||
            relayUpstreamStarted(*session)) return;
        session->upstreamPhase = RelayUpstreamPhase::Negotiating;
        upstreamOptions = session->upstreamOptions;
    }

    std::weak_ptr<Session> weakSession = session;
    auto configuredMsaCode = upstreamOptions.onMsaCode;
    upstreamOptions.onMsaCode = [
        this,
        weakSession,
        configuredMsaCode = std::move(configuredMsaCode)
    ](const XboxDeviceCodeInfo& code) {
        const auto activeSession = weakSession.lock();
        if (!activeSession || closed_.load()) return;
        {
            std::lock_guard<std::mutex> lock(activeSession->mutex);
            if (!relaySessionAcceptsPackets(*activeSession) ||
                activeSession->upstreamPhase == RelayUpstreamPhase::Idle) {
                return;
            }
        }

        // A direct low-level upstream callback remains authoritative. The
        // contextual Relay callback is the JS-facing high-level path.
        if (configuredMsaCode) {
            configuredMsaCode(code);
            return;
        }
        if (emitMsaCode(code, activeSession->downstream)) {
            return;
        }

        // relay.js disconnects only the Player whose upstream authentication
        // requested a code when no callback was supplied.
        disconnectDownstream(
            activeSession->downstream,
            "It's your first time joining. Please sign in and reconnect to "
            "join this server:\n\n" + code.message
        );
    };

    try {
        resolveUpstreamRealm(upstreamOptions);
    } catch (const std::exception& error) {
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (relaySessionAcceptsPackets(*session)) {
                session->upstreamPhase = RelayUpstreamPhase::Idle;
            }
        }
        if (!closed_.load()) {
            emitError("[upstream] " + std::string(error.what()));
        }
        emitStatus();
        return;
    }
    if (closed_.load()) {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->upstreamPhase = RelayUpstreamPhase::Closed;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session)) {
            session->upstreamPhase = RelayUpstreamPhase::Closed;
            return;
        }
        session->upstreamOptions = upstreamOptions;
    }
    bool publishPrimary = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        publishPrimary = primarySessionId_ == session->id;
    }
    if (publishPrimary) options_.upstream = upstreamOptions;

    auto upstream = std::make_shared<BedrockNetworkClient>(upstreamOptions);
    auto* upstreamIdentity = upstream.get();
    upstream->onError([this, weakSession, upstreamIdentity](const std::string& message) {
        const auto session = weakSession.lock();
        if (!session) return;
        bool requestDisconnect = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (relaySessionAcceptsPackets(*session) &&
                session->upstream.get() == upstreamIdentity &&
                !session->upstreamDisconnectRequested) {
                session->upstreamDisconnectRequested = true;
                requestDisconnect = true;
            }
        }
        if (requestDisconnect && !closed_.load()) {
            emitError("[upstream] " + message);
            disconnectDownstream(
                session->downstream,
                "Server error: " + message
            );
        }
    });

    upstream->onClose([this, weakSession, upstreamIdentity](const std::string& reason) {
        const auto session = weakSession.lock();
        if (!session) return;
        bool requestDisconnect = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->upstream.get() == upstreamIdentity &&
                relaySessionAcceptsPackets(*session)) {
                session->upstreamPhase = RelayUpstreamPhase::Idle;
            }
            if (relaySessionAcceptsPackets(*session) &&
                session->upstream.get() == upstreamIdentity &&
                !session->upstreamDisconnectRequested) {
                session->upstreamDisconnectRequested = true;
                requestDisconnect = true;
            }
        }
        if (requestDisconnect && !closed_.load()) {
            emitError("[upstream closed] " + reason);
            disconnectDownstream(
                session->downstream,
                "Backend server closed connection"
            );
        }
        emitStatus();
    });

    upstream->onAny([this, weakSession, upstreamIdentity](
        const BedrockNetworkClientPacketEvent& event
    ) {
        const auto session = weakSession.lock();
        if (!session) return;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!relaySessionAcceptsPackets(*session) ||
                session->upstream.get() != upstreamIdentity) return;
        }
        handleUpstreamPacket(session, event.packet);
    });

    upstream->onJoin([this, weakSession, upstream]() {
        const auto session = weakSession.lock();
        if (!session) return;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!relaySessionAcceptsPackets(*session) ||
                session->upstream != upstream ||
                session->upstreamPhase != RelayUpstreamPhase::Negotiating) {
                return;
            }
        }
        try {
            upstream->write("client_cache_status", ProtoDefValue::object({
                {"enabled", ProtoDefValue::boolean(options_.enableChunkCaching)}
            }));
        } catch (const std::exception& e) {
            emitError(
                "[upstream] failed to send client_cache_status: " +
                std::string(e.what())
            );
        }

        std::vector<VersionedGamePacket> queued;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!relaySessionAcceptsPackets(*session) ||
                session->upstream != upstream) return;
            session->upstreamPhase = RelayUpstreamPhase::Game;
            queued = std::move(session->pendingServerbound);
            session->pendingServerbound.clear();
        }
        emitStatus();
        for (const auto& packet : queued) {
            // RelayPlayer#flushUpQueue uses Client#write, not queue.
            forwardServerbound(session, packet, true);
        }

        bool stillReady = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            stillReady = relaySessionAcceptsPackets(*session) &&
                relayUpstreamReady(*session) &&
                session->upstream == upstream;
        }
        if (!stillReady) return;

        // relay.js emits only after client_cache_status, ds.upstream
        // publication and flushUpQueue(). Copy the listener snapshot so a
        // callback registered from this emission joins only the next event.
        const auto handlers = upstreamJoinHandlers_;
        std::lock_guard<std::recursive_mutex> dispatchLock(
            handlerDispatchMutex_
        );
        for (auto& handler : handlers) {
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                if (!relaySessionAcceptsPackets(*session) ||
                    session->upstream != upstream) return;
            }
            handler(session->downstream, upstream);
        }
    });

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session)) {
            session->upstreamPhase = RelayUpstreamPhase::Closed;
            return;
        }
        session->upstream = upstream;
        session->upstreamThread = std::thread(
            [this, weakSession, upstream]() {
                try {
                    (void) upstream->run();
                } catch (const std::exception& error) {
                    const auto session = weakSession.lock();
                    bool active = false;
                    if (session) {
                        std::lock_guard<std::mutex> lock(session->mutex);
                        active = relaySessionAcceptsPackets(*session) &&
                            session->upstream == upstream;
                    }
                    if (active && !closed_.load()) {
                        emitError("[upstream] " + std::string(error.what()));
                        disconnectDownstream(
                            session->downstream,
                            "Server error: " + std::string(error.what())
                        );
                    }
                }
            }
        );
    }
    emitStatus();
}

void BedrockLiveRelay::diagnosePacket(
    const std::shared_ptr<Session>& session,
    BedrockRelayDirection direction,
    const VersionedGamePacket& packet
) {
    if (!options_.itemResourceDiagnostics ||
        !isItemResourceDiagnosticPacket(packet.name)) {
        return;
    }

    const auto& version = direction == BedrockRelayDirection::Clientbound
        ? options_.server.version
        : baseUpstreamOptions_.version;
    ProtoDefVariableStorePtr variables;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session)) return;
        variables = session->diagnosticVariables;
    }

    std::vector<ProtoDefField> fields;
    std::string decodeError;
    if (needsStructuredDiagnostic(version, packet.name)) {
        try {
            ProtoDefPacketDecoder decoder(version, variables);
            fields = decoder.decodePacketStrict(packet.name, packet.payload);
        } catch (const std::exception& error) {
            decodeError = error.what();
        }
    }
    const auto entries = paletteEntries(fields);

    uint64_t sequence = 0;
    std::size_t registryOccurrence = 0;
    bool startGameSeen = false;
    bool itemBearingSeenBefore = false;
    std::unordered_map<int64_t, std::string> itemsByRuntimeId;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session)) return;
        sequence = ++session->diagnosticSequence;
        itemBearingSeenBefore = session->diagnosticItemBearingSeen;

        if (direction == BedrockRelayDirection::Clientbound &&
            packet.name == "start_game") {
            session->diagnosticStartGameSeen = true;
            if (!entries.empty()) {
                session->diagnosticItemsByRuntimeId.clear();
                for (const auto& [runtimeId, name] : entries) {
                    session->diagnosticItemsByRuntimeId[runtimeId] = name;
                }
            }
        }
        if (direction == BedrockRelayDirection::Clientbound &&
            packet.name == "item_registry") {
            registryOccurrence = ++session->diagnosticItemRegistryCount;
            if (!entries.empty()) {
                session->diagnosticItemsByRuntimeId.clear();
                for (const auto& [runtimeId, name] : entries) {
                    session->diagnosticItemsByRuntimeId[runtimeId] = name;
                }
            }
        }
        startGameSeen = session->diagnosticStartGameSeen;
        if (direction == BedrockRelayDirection::Clientbound &&
            isItemBearingPacket(packet.name)) {
            session->diagnosticItemBearingSeen = true;
        }
        itemsByRuntimeId = session->diagnosticItemsByRuntimeId;
    }

    std::ostringstream header;
    header << "session=" << session->id
           << " direction="
           << (direction == BedrockRelayDirection::Clientbound
                   ? "clientbound"
                   : "serverbound")
           << " sequence=" << sequence
           << " packet=" << packet.name
           << packetFingerprint(packet);
    emitDiagnostic(header.str());

    if (!decodeError.empty()) {
        emitDiagnostic(
            "session=" + session->id + " packet=" + packet.name +
            " diagnostic_decode_error=" + decodeError
        );
        return;
    }

    if (packet.name == "start_game") {
        emitDiagnostic(
            "session=" + session->id +
            " start_game item_palette_source=" +
            (versionUsesItemRegistry(version)
                ? "item_registry"
                : "start_game")
        );
        return;
    }

    if (packet.name == "resource_packs_info") {
        const auto indexes = fieldIndexes(fields, "texture_packs", ".uuid");
        emitDiagnostic(
            "session=" + session->id + " resource_packs_info count=" +
            std::to_string(indexes.size())
        );
        for (const auto index : indexes) {
            const auto base = "texture_packs[" + std::to_string(index) + "]";
            const bool hasCdn = !findFieldValue(fields, base + ".cdn_url").empty();
            emitDiagnostic(
                "session=" + session->id + " resource_pack index=" +
                std::to_string(index) + " uuid=" +
                findFieldValue(fields, base + ".uuid") + " version=" +
                findFieldValue(fields, base + ".version") + " size=" +
                findFieldValue(fields, base + ".size") + " has_cdn_url=" +
                (hasCdn ? "true" : "false")
            );
        }
        return;
    }

    if (packet.name == "resource_pack_stack") {
        for (const auto root : {std::string("behavior_packs"), std::string("resource_packs")}) {
            const auto indexes = fieldIndexes(fields, root, ".uuid");
            for (const auto index : indexes) {
                const auto base = root + "[" + std::to_string(index) + "]";
                emitDiagnostic(
                    "session=" + session->id + " resource_pack_stack list=" +
                    root + " index=" + std::to_string(index) + " uuid=" +
                    findFieldValue(fields, base + ".uuid") + " version=" +
                    findFieldValue(fields, base + ".version") + " subpack=" +
                    findFieldValue(fields, base + ".name")
                );
            }
        }
        return;
    }

    if (packet.name == "resource_pack_client_response") {
        emitDiagnostic(
            "session=" + session->id + " resource_pack_client_response status=" +
            findFieldValue(fields, "response_status")
        );
        for (const auto index : fieldIndexes(fields, "resourcepackids", "")) {
            emitDiagnostic(
                "session=" + session->id + " resource_pack_client_response index=" +
                std::to_string(index) + " id=" + findFieldValue(
                    fields,
                    "resourcepackids[" + std::to_string(index) + "]"
                )
            );
        }
        return;
    }

    if (packet.name == "resource_pack_chunk_request") {
        emitDiagnostic(
            "session=" + session->id + " resource_pack_chunk_request uuid=" +
            findFieldValue(fields, "pack_id") + " chunk_index=" +
            findFieldValue(fields, "chunk_index")
        );
        return;
    }

    if (packet.name == "resource_pack_data_info") {
        emitDiagnostic(
            "session=" + session->id + " resource_pack_data_info uuid=" +
            findFieldValue(fields, "pack_id") + " max_chunk_size=" +
            findFieldValue(fields, "max_chunk_size") + " chunk_count=" +
            findFieldValue(fields, "chunk_count") + " size=" +
            findFieldValue(fields, "size")
        );
        return;
    }

    if (packet.name == "resource_pack_chunk_data") {
        std::size_t payloadSize = 0;
        if (const auto* payload = findField(fields, "payload")) {
            if (payload->structuredValue.has_value() &&
                payload->structuredValue->kind == ProtoDefValue::Kind::Bytes) {
                payloadSize = payload->structuredValue->bytesValue.size();
            }
        }
        emitDiagnostic(
            "session=" + session->id + " resource_pack_chunk_data uuid=" +
            findFieldValue(fields, "pack_id") + " chunk_index=" +
            findFieldValue(fields, "chunk_index") + " progress=" +
            findFieldValue(fields, "progress") + " payload_size=" +
            std::to_string(payloadSize)
        );
        return;
    }

    if (packet.name == "item_registry") {
        emitDiagnostic(
            "session=" + session->id + " item_registry count=" +
            std::to_string(entries.size()) + " occurrence=" +
            std::to_string(registryOccurrence) + " after_start_game=" +
            (startGameSeen ? "true" : "false") + " before_first_item=" +
            (!itemBearingSeenBefore ? "true" : "false")
        );
        static constexpr const char* targets[] = {
            "minecraft:firework_rocket",
            "minecraft:diamond_sword",
            "minecraft:netherite_sword",
            "minecraft:sand",
            "minecraft:shulker_box"
        };
        for (const auto* target : targets) {
            std::optional<int64_t> runtimeId;
            for (const auto& [candidateId, name] : entries) {
                if (name == target) {
                    runtimeId = candidateId;
                    break;
                }
            }
            emitDiagnostic(
                "session=" + session->id + " item_registry name=" + target +
                " present=" + (runtimeId.has_value() ? "true" : "false") +
                (runtimeId.has_value()
                    ? " runtime_id=" + std::to_string(*runtimeId)
                    : std::string())
            );
        }
        return;
    }

    if (isItemBearingPacket(packet.name)) {
        const auto window = findFieldValue(fields, "window_id");
        const auto packetSlot = findFieldValue(fields, "slot");
        for (const auto& prefix : itemPrefixes(packet.name, fields)) {
            const auto networkId = integerField(fields, prefix + ".network_id");
            if (!networkId.has_value() || *networkId == 0) continue;
            const auto known = itemsByRuntimeId.find(*networkId);
            std::ostringstream item;
            item << "session=" << session->id
                 << " packet=" << packet.name;
            if (!window.empty()) item << " window=" << window;
            if (!packetSlot.empty()) item << " slot=" << packetSlot;
            if (const auto inputIndex = indexedField(prefix + ".network_id", "input", ".network_id")) {
                item << " item_index=" << *inputIndex;
            }
            item << " network_id=" << *networkId
                 << " block_runtime_id="
                 << findFieldValue(fields, prefix + ".block_runtime_id")
                 << " count=" << findFieldValue(fields, prefix + ".count")
                 << " nbt_present=" << (itemNbtPresent(fields, prefix) ? "true" : "false")
                 << " registry_present="
                 << (known != itemsByRuntimeId.end() ? "true" : "false");
            if (known != itemsByRuntimeId.end()) item << " registry_name=" << known->second;
            emitDiagnostic(item.str());
        }
    }
}

void BedrockLiveRelay::handleUpstreamPacket(
    const std::shared_ptr<Session>& session,
    const VersionedGamePacket& packet
) {
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session)) return;
    }
    if (options_.logging && shouldLogRelayPacket("Backend -> Proxy", packet.name)) {
        std::ostringstream out;
        out << "* Backend -> Proxy " << session->id << " " << packet.name
            << packetSummary(options_.server.version, packet);
        relayLogLine(out.str());
    }

    // This callback's source is the independently authenticated upstream
    // client. It has already consumed these packets in its own negotiation
    // state machine; they are never part of the downstream game stream.
    if (isClientboundHandshakePacket(packet.name)) {
        return;
    }

    diagnosePacket(session, BedrockRelayDirection::Clientbound, packet);

    const bool isPlayerSpawn = isPlayStatusPlayerSpawn(options_.server.version, packet);

    if (options_.skipClientboundLoginSuccess && isPlayStatusLoginSuccess(packet)) {
        return;
    }

    if (options_.skipClientboundResourcePacks &&
        isClientboundResourcePackPacket(packet.name)) {
        return;
    }

    if (!options_.forwardClientbound) {
        return;
    }

    auto packets = applyHandlers(
        session,
        BedrockRelayDirection::Clientbound,
        packet
    );
    for (const auto& candidate : packets) {
        forwardClientbound(session, candidate);
    }

    if (isPlayerSpawn) {
        std::vector<VersionedGamePacket> queued;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!relaySessionAcceptsPackets(*session)) return;
            session->clientboundPlayerSpawnSeen = true;
            queued = std::move(session->pendingPostSpawnServerbound);
            session->pendingPostSpawnServerbound.clear();
        }
        for (const auto& queuedPacket : queued) {
            forwardServerbound(session, queuedPacket);
        }
    }
}

void BedrockLiveRelay::handleDownstreamPacket(const BedrockServerPacketEvent& event) {
    const auto session = findSession(event.connection);
    if (!session) return;

    if (options_.logging && shouldLogRelayPacket("Client -> Proxy", event.packet.name)) {
        std::ostringstream out;
        out << "* Client -> Proxy " << session->id << " " << event.packet.name
            << packetSummary(baseUpstreamOptions_.version, event.packet);
        relayLogLine(out.str());
    }

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session)) return;
    }
    // Source and direction are authoritative here: these packets were emitted
    // by the local downstream server session. onJoin() runs while processing
    // client_to_server_handshake, before onAny(), so downstreamJoined cannot
    // safely identify this boundary.
    if (isDownstreamHandshakePacket(event.packet.name)) {
        return;
    }

    diagnosePacket(session, BedrockRelayDirection::Serverbound, event.packet);

    if (!options_.forwardServerbound) {
        return;
    }

    auto packets = applyHandlers(
        session,
        BedrockRelayDirection::Serverbound,
        event.packet
    );
    for (const auto& candidate : packets) {
        if (candidate.name == "client_cache_status") {
            ProtoDefPacketEncoder encoder(baseUpstreamOptions_.version);
            auto payload = encoder.encodePacket("client_cache_status", ProtoDefValue::object({
                {"enabled", ProtoDefValue::boolean(options_.enableChunkCaching)}
            }));
            VersionedMcpeCodec codec = VersionedMcpeCodec::forVersion(baseUpstreamOptions_.version);
            auto forced = codec.packetCodec().makePacketByName("client_cache_status", payload);

            bool upstreamReady = false;
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                upstreamReady = relaySessionAcceptsPackets(*session) &&
                    session->upstream && relayUpstreamReady(*session);
            }
            if (!upstreamReady) {
                // bedrock-protocol's Relay.flushUpQueue intentionally drops
                // cached client_cache_status packets. It sends one forced
                // value when the upstream client joins, then forwards later
                // live client_cache_status packets as needed.
                continue;
            }
            forwardServerbound(session, forced);
            continue;
        }

        if (candidate.name == "request_chunk_radius") {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!relaySessionAcceptsPackets(*session)) return;
            if (!session->clientboundPlayerSpawnSeen) {
                session->pendingPostSpawnServerbound.push_back(candidate);
                continue;
            }
        }

        std::shared_ptr<BedrockNetworkClient> upstream;
        bool upstreamReady = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!relaySessionAcceptsPackets(*session)) return;
            upstream = session->upstream;
            upstreamReady = relayUpstreamReady(*session);
        }
        if (!upstream || !upstreamReady) {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (relaySessionAcceptsPackets(*session)) {
                session->pendingServerbound.push_back(candidate);
            }
            continue;
        }
        forwardServerbound(session, candidate);
    }
}

void BedrockLiveRelay::forwardClientbound(
    const std::shared_ptr<Session>& session,
    const VersionedGamePacket& packet
) {
    std::lock_guard<std::recursive_mutex> dispatchLock(handlerDispatchMutex_);
    BedrockServerConnection downstream;
    std::vector<VersionedGamePacket> heldChunks;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session)) return;
        if (session->downstreamPhase != RelayDownstreamPhase::Game) {
            session->pendingClientbound.push_back(packet);
            return;
        }
        downstream = session->downstream;

        const auto now = std::chrono::steady_clock::now();
        if (options_.queueClientboundLevelChunksUntilStartGame &&
            packet.name == "level_chunk" &&
            (!session->clientboundStartGameSent ||
             now < session->clientboundChunkReleaseAt)) {
            session->heldClientboundLevelChunks.push_back(packet);
            return;
        }

        if (packet.name == "start_game") {
            session->clientboundStartGameSent = true;
            session->clientboundChunkReleaseAt =
                now + std::chrono::milliseconds(500);
        }

        if (session->clientboundStartGameSent &&
            now >= session->clientboundChunkReleaseAt &&
            !session->heldClientboundLevelChunks.empty()) {
            heldChunks = std::move(session->heldClientboundLevelChunks);
            session->heldClientboundLevelChunks.clear();
        }
    }
    if (options_.logging && shouldLogRelayPacket("Proxy -> Client", packet.name)) {
        std::ostringstream out;
        out << "* Proxy -> Client " << session->id << " " << packet.name
            << packetSummary(options_.server.version, packet);
        relayLogLine(out.str());
    }
    // relay.js routes live upstream packets through Player#queue so packets
    // observed in one batching interval share one downstream MCPE batch.
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session) ||
            session->downstreamPhase != RelayDownstreamPhase::Game) return;
        server_->queuePacket(
            downstream,
            packet,
            options_.clientboundCompression
        );
    }

    if (!heldChunks.empty()) {
        if (options_.logging) {
            std::ostringstream out;
            out << "* Proxy -> Client batch level_chunk x" << heldChunks.size();
            relayLogLine(out.str());
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session) ||
            session->downstreamPhase != RelayDownstreamPhase::Game) return;
        server_->queuePackets(
            downstream,
            heldChunks,
            options_.clientboundCompression
        );
    }
}

void BedrockLiveRelay::forwardServerbound(
    const std::shared_ptr<Session>& session,
    const VersionedGamePacket& packet,
    bool immediate
) {
    std::lock_guard<std::recursive_mutex> dispatchLock(handlerDispatchMutex_);
    std::shared_ptr<BedrockNetworkClient> upstream;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session)) return;
        if (!session->upstream || !relayUpstreamReady(*session)) {
            session->pendingServerbound.push_back(packet);
            return;
        }
        upstream = session->upstream;
    }
    if (!upstream) {
        return;
    }
    if (options_.logging && shouldLogRelayPacket("Proxy -> Backend", packet.name)) {
        std::ostringstream out;
        out << "* Proxy -> Backend " << session->id << " " << packet.name
            << packetSummary(baseUpstreamOptions_.version, packet);
        relayLogLine(out.str());
    }
    if (immediate) {
        upstream->sendPacket(packet);
    } else {
        upstream->sendBuffer(packet.fullPacket);
    }
}

std::vector<VersionedGamePacket> BedrockLiveRelay::applyHandlers(
    const std::shared_ptr<Session>& session,
    BedrockRelayDirection direction,
    const VersionedGamePacket& packet
) {
    BedrockRelayPacketEvent event;
    event.direction = direction;
    event.sessionId = session->id;
    event.packet = packet;

    std::lock_guard<std::recursive_mutex> dispatchLock(handlerDispatchMutex_);
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session)) return {};
    }
    auto& handlers = direction == BedrockRelayDirection::Clientbound
        ? clientboundHandlers_
        : serverboundHandlers_;

    for (auto& handler : handlers) {
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!relaySessionAcceptsPackets(*session)) return {};
        }
        handler(event);
    }

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!relaySessionAcceptsPackets(*session)) return {};
    }

    if (event.canceled) {
        return {};
    }
    if (!event.replacements.empty()) {
        return std::move(event.replacements);
    }
    return { std::move(event.packet) };
}

} // namespace bedrock
