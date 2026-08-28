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
    bool downstreamJoined = false;
    bool upstreamStarted = false;
    bool upstreamReady = false;
    bool clientboundStartGameSent = false;
    bool clientboundPlayerSpawnSeen = false;
    bool closing = false;
    bool upstreamDisconnectRequested = false;
};

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
        bool rejected = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (options_.forceSingle && !sessions_.empty()) {
                rejectedConnections_.insert(id);
                rejected = true;
            } else {
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
        removeRelaySession(session, "downstream disconnected");
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
            alreadyStarted = session->upstreamStarted || session->upstream != nullptr;
        }
        if (alreadyStarted) {
            resetRelaySession(session, "downstream reconnect", true);
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
        resetRelaySession(session, reason, false);
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
        if (session->downstreamJoined && !session->closing) return true;
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
        if (session->upstreamStarted && !session->closing) return true;
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
        if (session->upstreamReady && !session->closing) return true;
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
    resetRelaySession(session, reason, true);
    // Node's upstream `close` listener disconnects the matching downstream
    // Player. Keep that lifecycle while suppressing an `error` event for this
    // intentional close.
    server_->disconnect(connection, "Backend server closed connection");
    emitStatus();
}

std::string BedrockLiveRelay::sessionId(
    const BedrockServerConnection& connection
) {
    return connection.address + ":" + std::to_string(connection.port) + "#" +
        std::to_string(connection.clientGuid);
}

BedrockLiveRelayOptions BedrockLiveRelay::normalizeOptions(BedrockLiveRelayOptions options) {
    (void) validateVersion(options.server.version);

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
        name == "client_to_server_handshake" ||
        name == "resource_pack_client_response";
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
        if (session->closing) continue;
        ++status.downstreamConnections;
        if (session->downstreamJoined) ++status.downstreamJoinedCount;
        if (session->upstreamStarted) ++status.upstreamStartedCount;
        if (session->upstreamReady) ++status.upstreamReadyCount;
    }
    status.downstreamJoined = status.downstreamJoinedCount != 0;
    status.upstreamStarted = status.upstreamStartedCount != 0;
    status.upstreamReady = status.upstreamReadyCount != 0;

    for (auto& handler : statusHandlers_) {
        handler(status);
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
        alreadyStarted = session->upstreamStarted;
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
        if (session->closing) return;
        session->downstreamJoined = true;
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
        .onMsaCode = upstreamOptions.onMsaCode
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

void BedrockLiveRelay::resetRelaySession(
    const std::shared_ptr<Session>& session,
    const std::string& reason,
    bool retainDownstream
) {
    std::shared_ptr<BedrockNetworkClient> upstream;
    std::thread upstreamThread;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!retainDownstream) session->closing = true;
        upstream = session->upstream;
        if (session->upstreamThread.joinable()) {
            upstreamThread = std::move(session->upstreamThread);
        }
        session->upstreamReady = false;
        session->upstreamStarted = false;
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

    if (upstream) upstream->close(reason);
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
        if (retainDownstream && !session->closing) {
            session->upstreamDisconnectRequested = false;
        }
    }

    if (options_.logging) {
        relayLogLine(
            "[relay] session reset " + session->id + ": " + reason
        );
    }
}

void BedrockLiveRelay::removeRelaySession(
    const std::shared_ptr<Session>& session,
    const std::string& reason
) {
    resetRelaySession(session, reason, false);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sessions_.find(session->id);
    if (found != sessions_.end() && found->second == session) {
        sessions_.erase(found);
    }
    if (primarySessionId_ == session->id) {
        primarySessionId_ = sessions_.empty()
            ? std::string()
            : sessions_.begin()->first;
    }
}

void BedrockLiveRelay::startUpstream(const std::shared_ptr<Session>& session) {
    BedrockNetworkClientOptions upstreamOptions;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing || session->upstreamStarted) return;
        session->upstreamStarted = true;
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
        server_->disconnect(
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
            session->upstreamStarted = false;
        }
        if (!closed_.load()) {
            emitError("[upstream] " + std::string(error.what()));
        }
        emitStatus();
        return;
    }
    if (closed_.load()) {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->upstreamStarted = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing) {
            session->upstreamStarted = false;
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
    upstream->onError([this, weakSession](const std::string& message) {
        const auto session = weakSession.lock();
        if (!session) return;
        bool requestDisconnect = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!session->closing && !session->upstreamDisconnectRequested) {
                session->upstreamDisconnectRequested = true;
                requestDisconnect = true;
            }
        }
        if (!closed_.load()) emitError("[upstream] " + message);
        if (requestDisconnect && !closed_.load()) {
            server_->disconnect(session->downstream, "Server error: " + message);
        }
    });

    upstream->onClose([this, weakSession](const std::string& reason) {
        const auto session = weakSession.lock();
        if (!session) return;
        bool requestDisconnect = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->upstreamReady = false;
            if (!session->closing && !session->upstreamDisconnectRequested) {
                session->upstreamDisconnectRequested = true;
                requestDisconnect = true;
            }
        }
        if (requestDisconnect && !closed_.load()) {
            emitError("[upstream closed] " + reason);
        }
        if (requestDisconnect && !closed_.load()) {
            server_->disconnect(
                session->downstream,
                "Backend server closed connection"
            );
        }
        emitStatus();
    });

    upstream->onAny([this, weakSession](
        const BedrockNetworkClientPacketEvent& event
    ) {
        const auto session = weakSession.lock();
        if (session) handleUpstreamPacket(session, event.packet);
    });

    upstream->onJoin([this, weakSession, upstream]() {
        const auto session = weakSession.lock();
        if (!session) return;
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
            if (session->closing || session->upstream != upstream) return;
            session->upstreamReady = true;
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
            stillReady = !session->closing &&
                session->upstreamReady &&
                session->upstream == upstream;
        }
        if (!stillReady) return;

        // relay.js emits only after client_cache_status, ds.upstream
        // publication and flushUpQueue(). Copy the listener snapshot so a
        // callback registered from this emission joins only the next event.
        const auto handlers = upstreamJoinHandlers_;
        for (auto& handler : handlers) {
            handler(session->downstream, upstream);
        }
    });

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing) {
            session->upstreamStarted = false;
            return;
        }
        session->upstream = upstream;
        session->upstreamThread = std::thread(
            [this, weakSession, upstream]() {
                try {
                    (void) upstream->run();
                } catch (const std::exception& error) {
                    const auto session = weakSession.lock();
                    if (session && !closed_.load()) {
                        emitError("[upstream] " + std::string(error.what()));
                        server_->disconnect(
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

void BedrockLiveRelay::handleUpstreamPacket(
    const std::shared_ptr<Session>& session,
    const VersionedGamePacket& packet
) {
    if (options_.logging && shouldLogRelayPacket("Backend -> Proxy", packet.name)) {
        std::ostringstream out;
        out << "* Backend -> Proxy " << session->id << " " << packet.name
            << packetSummary(options_.server.version, packet);
        relayLogLine(out.str());
    }

    if (options_.skipClientboundHandshake &&
        isClientboundHandshakePacket(packet.name)) {
        return;
    }

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

    bool downstreamJoined = false;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing) return;
        downstreamJoined = session->downstreamJoined;
    }
    if (options_.filterDownstreamHandshakePackets &&
        !downstreamJoined &&
        isDownstreamHandshakePacket(event.packet.name)) {
        return;
    }

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
                upstreamReady = session->upstream && session->upstreamReady;
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
            if (!session->clientboundPlayerSpawnSeen) {
                session->pendingPostSpawnServerbound.push_back(candidate);
                continue;
            }
        }

        std::shared_ptr<BedrockNetworkClient> upstream;
        bool upstreamReady = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            upstream = session->upstream;
            upstreamReady = session->upstreamReady;
        }
        if (candidate.name == "resource_pack_client_response" && upstream) {
            if (options_.logging) {
                std::ostringstream out;
                out << "* Proxy -> Backend " << candidate.name
                    << packetFingerprint(candidate);
                relayLogLine(out.str());
            }
            upstream->sendPacket(candidate);
            continue;
        }

        if (!upstream || !upstreamReady) {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!session->closing) {
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
    BedrockServerConnection downstream;
    std::vector<VersionedGamePacket> heldChunks;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing) return;
        if (!session->downstreamJoined) {
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
    server_->queuePacket(downstream, packet, options_.clientboundCompression);

    if (!heldChunks.empty()) {
        if (options_.logging) {
            std::ostringstream out;
            out << "* Proxy -> Client batch level_chunk x" << heldChunks.size();
            relayLogLine(out.str());
        }
        server_->queuePackets(downstream, heldChunks, options_.clientboundCompression);
    }
}

void BedrockLiveRelay::forwardServerbound(
    const std::shared_ptr<Session>& session,
    const VersionedGamePacket& packet,
    bool immediate
) {
    std::shared_ptr<BedrockNetworkClient> upstream;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->closing) return;
        if (!session->upstream || !session->upstreamReady) {
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

    std::lock_guard<std::mutex> dispatchLock(handlerDispatchMutex_);
    auto& handlers = direction == BedrockRelayDirection::Clientbound
        ? clientboundHandlers_
        : serverboundHandlers_;

    for (auto& handler : handlers) {
        handler(event);
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
