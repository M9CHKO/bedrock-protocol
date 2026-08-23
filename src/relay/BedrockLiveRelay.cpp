#include <bedrock/relay/BedrockLiveRelay.hpp>

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
      server_(std::make_unique<BedrockServer>(options_.server)) {}

BedrockLiveRelay::~BedrockLiveRelay() {
    close();
}

void BedrockLiveRelay::listen() {
    if (!closed_.exchange(false)) {
        return;
    }

    server_->onConnect([this](const BedrockServerConnection& connection) {
        for (auto& handler : connectHandlers_) {
            handler(connection);
        }
        emitStatus();
    });

    server_->onJoin([this](const BedrockServerConnection& connection) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            downstream_ = connection;
            downstreamJoined_.store(true);
        }

        for (auto& handler : joinHandlers_) {
            handler(connection);
        }

        std::vector<VersionedGamePacket> queuedClientbound;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queuedClientbound = std::move(pendingClientbound_);
            pendingClientbound_.clear();
        }
        for (const auto& packet : queuedClientbound) {
            forwardClientbound(packet);
        }

        emitStatus();
    });

    server_->onDisconnect([this](const BedrockServerConnection& connection) {
        for (auto& handler : disconnectHandlers_) {
            handler(connection);
        }
        resetRelaySession("downstream disconnected");
        emitStatus();
    });

    server_->onLogin([this](const BedrockServerPacketEvent& event) {
        if (upstreamStarted_.load() || upstream_ || downstreamJoined_.load()) {
            resetRelaySession("downstream reconnect");
        }
        captureDownstreamClientData(event.packet);
        startUpstream();
    });

    server_->onAny([this](const BedrockServerPacketEvent& event) {
        handleDownstreamPacket(event);
    });

    server_->listen();
    listening_.store(true);
    emitStatus();
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

    if (upstream_) {
        upstream_->close(reason);
    }
    if (server_) {
        server_->close();
    }
    if (upstreamThread_.joinable() &&
        upstreamThread_.get_id() != std::this_thread::get_id()) {
        upstreamThread_.join();
    }

    listening_.store(false);
    upstreamReady_.store(false);
    emitStatus();
    closedCv_.notify_all();
}

void BedrockLiveRelay::onConnect(ConnectionHandler handler) {
    connectHandlers_.push_back(std::move(handler));
}

void BedrockLiveRelay::onJoin(ConnectionHandler handler) {
    joinHandlers_.push_back(std::move(handler));
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

bool BedrockLiveRelay::listening() const {
    return listening_.load();
}

bool BedrockLiveRelay::downstreamJoined() const {
    return downstreamJoined_.load();
}

bool BedrockLiveRelay::upstreamStarted() const {
    return upstreamStarted_.load();
}

bool BedrockLiveRelay::upstreamReady() const {
    return upstreamReady_.load();
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
    return upstream_.get();
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
    status.downstreamJoined = downstreamJoined_.load();
    status.upstreamStarted = upstreamStarted_.load();
    status.upstreamReady = upstreamReady_.load();
    status.boundPort = boundPort();

    for (auto& handler : statusHandlers_) {
        handler(status);
    }
}

void BedrockLiveRelay::captureDownstreamClientData(const VersionedGamePacket& packet) {
    if (!options_.forwardDownstreamClientData ||
        upstreamStarted_.load() ||
        !options_.upstream.clientDataJson.empty()) {
        return;
    }

    try {
        auto login = LoginPacketCodec::decode(packet.fullPacket);
        options_.upstream.clientDataJson =
            BedrockKeyExchange::extractJwtPayloadJson(login.client);
        downstreamProfile_ = downstreamProfileFromLogin(login);

        // relay.js assigns this ternary result unconditionally. Keep an empty
        // string rather than falling back to the prior RelayBot identity.
        detail::applyRelayDownstreamIdentity(options_, downstreamProfile_);

        if (options_.logging) {
            std::ostringstream out;
            out << "[relay] downstream profile"
                << " name=" << downstreamProfile_.displayName
                << " xuid=" << downstreamProfile_.xuid;
            relayLogLine(out.str());
        }
    } catch (const std::exception& e) {
        emitError("[relay] failed to forward downstream clientData: " + std::string(e.what()));
    }
}

void BedrockLiveRelay::resetRelaySession(const std::string& reason) {
    if (upstream_) {
        upstream_->close(reason);
    }
    if (upstreamThread_.joinable() &&
        upstreamThread_.get_id() != std::this_thread::get_id()) {
        upstreamThread_.join();
    }
    upstream_.reset();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        downstream_.reset();
        pendingServerbound_.clear();
        pendingPostSpawnServerbound_.clear();
        pendingClientbound_.clear();
        heldClientboundLevelChunks_.clear();
        clientboundChunkReleaseAt_ = {};
        downstreamProfile_ = {};
    }

    downstreamJoined_.store(false);
    upstreamStarted_.store(false);
    upstreamReady_.store(false);
    clientboundStartGameSent_.store(false);
    clientboundPlayerSpawnSeen_.store(false);

    if (options_.logging) {
        relayLogLine("[relay] session reset: " + reason);
    }
}

void BedrockLiveRelay::startUpstream() {
    bool expected = false;
    if (!upstreamStarted_.compare_exchange_strong(expected, true)) {
        return;
    }

    upstream_ = std::make_unique<BedrockNetworkClient>(options_.upstream);

    upstream_->onError([this](const std::string& message) {
        emitError("[upstream] " + message);
    });

    upstream_->onClose([this](const std::string& reason) {
        if (!closed_.load()) {
            emitError("[upstream closed] " + reason);
        }
        upstreamReady_.store(false);
        emitStatus();
    });

    upstream_->onAny([this](const BedrockNetworkClientPacketEvent& event) {
        handleUpstreamPacket(event.packet);
    });

    upstream_->onJoin([this]() {
        try {
            upstream_->write("client_cache_status", ProtoDefValue::object({
                {"enabled", ProtoDefValue::boolean(options_.enableChunkCaching)}
            }));
        } catch (const std::exception& e) {
            emitError("[upstream] failed to send client_cache_status: " + std::string(e.what()));
        }

        upstreamReady_.store(true);
        emitStatus();

        std::vector<VersionedGamePacket> queued;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queued = std::move(pendingServerbound_);
            pendingServerbound_.clear();
        }
        for (const auto& packet : queued) {
            forwardServerbound(packet);
        }
    });

    upstreamThread_ = std::thread([this]() {
        upstream_->run();
    });
    emitStatus();
}

void BedrockLiveRelay::handleUpstreamPacket(const VersionedGamePacket& packet) {
    if (options_.logging && shouldLogRelayPacket("Backend -> Proxy", packet.name)) {
        std::ostringstream out;
        out << "* Backend -> Proxy " << packet.name
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

    auto packets = applyHandlers(BedrockRelayDirection::Clientbound, packet);
    for (const auto& candidate : packets) {
        forwardClientbound(candidate);
    }

    if (isPlayerSpawn) {
        std::vector<VersionedGamePacket> queued;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            clientboundPlayerSpawnSeen_.store(true);
            queued = std::move(pendingPostSpawnServerbound_);
            pendingPostSpawnServerbound_.clear();
        }
        for (const auto& queuedPacket : queued) {
            forwardServerbound(queuedPacket);
        }
    }
}

void BedrockLiveRelay::handleDownstreamPacket(const BedrockServerPacketEvent& event) {
    if (options_.logging && shouldLogRelayPacket("Client -> Proxy", event.packet.name)) {
        std::ostringstream out;
        out << "* Client -> Proxy " << event.packet.name
            << packetSummary(options_.upstream.version, event.packet);
        relayLogLine(out.str());
    }

    if (options_.filterDownstreamHandshakePackets &&
        !downstreamJoined_.load() &&
        isDownstreamHandshakePacket(event.packet.name)) {
        return;
    }

    if (!options_.forwardServerbound) {
        return;
    }

    auto packets = applyHandlers(BedrockRelayDirection::Serverbound, event.packet);
    for (const auto& candidate : packets) {
        if (candidate.name == "client_cache_status") {
            ProtoDefPacketEncoder encoder(options_.upstream.version);
            auto payload = encoder.encodePacket("client_cache_status", ProtoDefValue::object({
                {"enabled", ProtoDefValue::boolean(options_.enableChunkCaching)}
            }));
            VersionedMcpeCodec codec = VersionedMcpeCodec::forVersion(options_.upstream.version);
            auto forced = codec.packetCodec().makePacketByName("client_cache_status", payload);

            if (!upstream_ || !upstreamReady_.load()) {
                // bedrock-protocol's Relay.flushUpQueue intentionally drops
                // cached client_cache_status packets. It sends one forced
                // value when the upstream client joins, then forwards later
                // live client_cache_status packets as needed.
                continue;
            }
            forwardServerbound(forced);
            continue;
        }

        if (candidate.name == "request_chunk_radius" &&
            !clientboundPlayerSpawnSeen_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingPostSpawnServerbound_.push_back(candidate);
            continue;
        }

        if (candidate.name == "resource_pack_client_response" && upstream_) {
            if (options_.logging) {
                std::ostringstream out;
                out << "* Proxy -> Backend " << candidate.name
                    << packetFingerprint(candidate);
                relayLogLine(out.str());
            }
            upstream_->sendPacket(candidate);
            continue;
        }

        if (!upstream_ || !upstreamReady_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingServerbound_.push_back(candidate);
            continue;
        }
        forwardServerbound(candidate);
    }
}

void BedrockLiveRelay::forwardClientbound(const VersionedGamePacket& packet) {
    std::optional<BedrockServerConnection> downstream;
    std::vector<VersionedGamePacket> heldChunks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        downstream = downstream_;
        if (!downstream.has_value()) {
            pendingClientbound_.push_back(packet);
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (options_.queueClientboundLevelChunksUntilStartGame &&
            packet.name == "level_chunk" &&
            (!clientboundStartGameSent_.load() || now < clientboundChunkReleaseAt_)) {
            heldClientboundLevelChunks_.push_back(packet);
            return;
        }

        if (packet.name == "start_game") {
            clientboundStartGameSent_.store(true);
            clientboundChunkReleaseAt_ = now + std::chrono::milliseconds(500);
        }

        if (clientboundStartGameSent_.load() &&
            now >= clientboundChunkReleaseAt_ &&
            !heldClientboundLevelChunks_.empty()) {
            heldChunks = std::move(heldClientboundLevelChunks_);
            heldClientboundLevelChunks_.clear();
        }
    }
    if (options_.logging && shouldLogRelayPacket("Proxy -> Client", packet.name)) {
        std::ostringstream out;
        out << "* Proxy -> Client " << packet.name
            << packetSummary(options_.server.version, packet);
        relayLogLine(out.str());
    }
    server_->sendPacket(*downstream, packet, options_.clientboundCompression);

    if (!heldChunks.empty()) {
        if (options_.logging) {
            std::ostringstream out;
            out << "* Proxy -> Client batch level_chunk x" << heldChunks.size();
            relayLogLine(out.str());
        }
        server_->sendPackets(*downstream, heldChunks, options_.clientboundCompression);
    }
}

void BedrockLiveRelay::forwardServerbound(const VersionedGamePacket& packet) {
    if (!upstream_ || !upstreamReady_.load()) {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingServerbound_.push_back(packet);
        return;
    }
    if (options_.logging && shouldLogRelayPacket("Proxy -> Backend", packet.name)) {
        std::ostringstream out;
        out << "* Proxy -> Backend " << packet.name
            << packetSummary(options_.upstream.version, packet);
        relayLogLine(out.str());
    }
    upstream_->sendPacket(packet);
}

std::vector<VersionedGamePacket> BedrockLiveRelay::applyHandlers(
    BedrockRelayDirection direction,
    const VersionedGamePacket& packet
) {
    BedrockRelayPacketEvent event;
    event.direction = direction;
    event.packet = packet;

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
