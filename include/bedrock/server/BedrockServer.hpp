#pragma once

#include <bedrock/BedrockEncryption.hpp>
#include <bedrock/BedrockKeyExchange.hpp>
#include <bedrock/LoginPacket.hpp>
#include <bedrock/Options.hpp>
#include <bedrock/auth/BedrockAuthJwt.hpp>
#include <bedrock/debug/PacketFieldDecoder.hpp>
#include <bedrock/debug/ProtocolTypeTsvIndex.hpp>
#include <bedrock/generated/GeneratedProtocolTypes.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/protodef/ProtoDefContext.hpp>
#include <bedrock/protodef/ProtoDefDecoder.hpp>
#include <bedrock/protodef/ProtoDefField.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/protodef/ProtoDefReader.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>
#include <bedrock/server/BedrockLoginVerifier.hpp>
#include <bedrock/server/RakNetServer.hpp>
#include <bedrock/server/ServerAdvertisement.hpp>

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bedrock {

struct BedrockServerTestAccess;

struct BedrockServerOptions {
    std::string host = "0.0.0.0";
    uint16_t port = 19132;
    std::string version = "1.26.0";
    ServerAdvertisementObject motd;
    int maxPlayers = 3;
    std::function<ServerAdvertisement&()> advertisementFn;
    bool offline = false;
    bool autoLogin = true;
    // The JavaScript server completes login at client_to_server_handshake and
    // does not run a resource-pack exchange.  Keep the previous empty-pack
    // flow only as an explicit C++ extension.
    bool autoResourcePacks = false;
    uint16_t compressionThreshold = 512;
    std::string compressionAlgorithm = "deflate";
    int compressionLevel = 7;
};

class BedrockServer;
BedrockServer createServer(BedrockServerOptions options);

class BedrockUnhandledPlayerError : public std::runtime_error {
public:
    explicit BedrockUnhandledPlayerError(const std::string& message)
        : std::runtime_error(message) {}
};

class BedrockUndefinedPlayerBufferError : public std::runtime_error {
public:
    BedrockUndefinedPlayerBufferError()
        : std::runtime_error(
            "Cannot read properties of undefined (reading 'byteLength')"
          ) {}
};

class BedrockServerPlayerEventState {
public:
    using ErrorHandler = std::function<void(const std::string&)>;
    using CloseHandler = std::function<void()>;

    void onError(ErrorHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        errorHandlers_.push_back(std::move(handler));
    }

    void onClose(CloseHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        closeHandlers_.push_back(std::move(handler));
    }

private:
    friend class BedrockServer;
    friend struct BedrockServerTestAccess;

    mutable std::mutex mutex_;
    std::vector<ErrorHandler> errorHandlers_;
    std::vector<CloseHandler> closeHandlers_;

    std::vector<ErrorHandler> errorSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return errorHandlers_;
    }

    std::vector<CloseHandler> closeSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closeHandlers_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        errorHandlers_.clear();
        closeHandlers_.clear();
    }
};

enum class BedrockServerClientStatus {
    Disconnected,
    Connecting,
    Authenticating,
    Initializing,
    Initialized
};

struct BedrockServerConnection {
    using ErrorHandler = BedrockServerPlayerEventState::ErrorHandler;
    using CloseHandler = BedrockServerPlayerEventState::CloseHandler;

    std::string address;
    uint16_t port = 0;
    uint64_t clientGuid = 0;
    int mtu = 1400;
    RakNetServerPeer peer;
    std::shared_ptr<BedrockServerPlayerEventState> playerEvents;

    // Player is represented by this shared connection view in the C++ API.
    // Copies retain the same EventEmitter-like listener state.
    void onError(ErrorHandler handler) const {
        if (!playerEvents) {
            throw std::logic_error("player event state is not initialized");
        }
        playerEvents->onError(std::move(handler));
    }

    void onClose(CloseHandler handler) const {
        if (!playerEvents) {
            throw std::logic_error("player event state is not initialized");
        }
        playerEvents->onClose(std::move(handler));
    }
};

struct BedrockServerPacketEvent {
    BedrockServerConnection connection;
    VersionedGamePacket packet;
};

class BedrockServer {
public:
    using ConnectionHandler = std::function<void(const BedrockServerConnection&)>;
    using PacketHandler = std::function<void(const BedrockServerPacketEvent&)>;
    using StatusHandler = std::function<void(
        const BedrockServerConnection&,
        BedrockServerClientStatus
    )>;

    explicit BedrockServer(BedrockServerOptions options = {})
        : options_(normalizeOptions(std::move(options))),
          advertisement_(makeAdvertisement(options_)),
          raknet_(makeRakNetOptions(options_, advertisement_.toString())),
          mcpeCodec_(VersionedMcpeCodec::forVersion(options_.version)) {}

    ~BedrockServer() {
        // RakNetServer is declared before the Player/session state and would
        // otherwise be destroyed after those callback targets. Stop and join
        // its worker while every captured map/mutex is still alive.
        closing_ = true;
        raknet_.close();
        stopPlayerCloseScheduler();
    }

    void onConnect(ConnectionHandler handler) {
        connectHandlers_.push_back(std::move(handler));
    }

    void onJoin(ConnectionHandler handler) {
        joinHandlers_.push_back(std::move(handler));
    }

    // Authenticated login lifecycle event. Unlike on("login"), this mirrors
    // serverPlayer.js's explicit login event and therefore fires only after a
    // valid login has initialized the encrypted session.
    void onLogin(PacketHandler handler) {
        loginHandlers_.push_back(std::move(handler));
    }

    void onSpawn(ConnectionHandler handler) {
        spawnHandlers_.push_back(std::move(handler));
    }

    void onDisconnect(ConnectionHandler handler) {
        std::lock_guard<std::mutex> lock(playerLifecycleMutex_);
        disconnectHandlers_.push_back(std::move(handler));
    }

    void onStatus(StatusHandler handler) {
        statusHandlers_.push_back(std::move(handler));
    }

    void onAny(PacketHandler handler) {
        anyPacketHandlers_.push_back(std::move(handler));
    }

    void on(const std::string& packetName, PacketHandler handler) {
        packetHandlers_[packetName].push_back(std::move(handler));
    }

    ServerAdvertisement& getAdvertisement() {
        if (options_.advertisementFn) {
            return options_.advertisementFn();
        }

        advertisement_.playersOnline = clientCount_.load();
        return advertisement_;
    }

    void listen() {
        closing_ = false;
        // RakServer's constructor asks Server#getAdvertisement for the initial
        // offline response before the backend starts listening.
        raknet_.setAdvertisement(getAdvertisement().toString());
        raknet_.setAdvertisementProvider([this]() {
            return getAdvertisement().toString();
        });
        raknet_.onOpenConnection([this](const RakNetServerPeer& peer) {
            if (closing_.load()) {
                return;
            }
            BedrockServerConnection connection;
            connection.address = peer.address;
            connection.port = peer.port;
            connection.clientGuid = peer.clientGuid;
            connection.mtu = peer.mtu;
            connection.peer = peer;
            connection.playerEvents = std::make_shared<BedrockServerPlayerEventState>();

            const auto key = connectionKey(peer);
            {
                std::lock_guard<std::mutex> lock(playerLifecycleMutex_);
                closedPlayers_.erase(playerKey(connection));
                detachedPlayerStatuses_.erase(playerKey(connection));
            }
            {
                std::lock_guard<std::mutex> lock(serverStateMutex_);
                if (closing_.load()) {
                    return;
                }
                connections_[key] = connection;
                auto session = std::make_shared<SessionState>();
                // Player copies the server compression level in its
                // constructor. Subsequent Server#setCompressor calls do not
                // mutate this per-player snapshot.
                session->compressionLevel = options_.compressionLevel;
                sessions_[key] = std::move(session);
                ++clientCount_;
            }

            for (auto& handler : connectHandlers_) {
                handler(connection);
            }
        });
        raknet_.onEncapsulated([this](const RakNetServerPeer& peer, const std::vector<uint8_t>& payload) {
            handleEncapsulated(peer, payload);
        });
        raknet_.onCloseConnection([this](const RakNetServerPeer& peer) {
            handlePeerClosed(peer);
        });
        raknet_.listen();
    }

    void close(const std::string& disconnectReason = "Server closed") {
        closing_ = true;
        const auto ownDeadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(60);

        // server.js sends the MCPE disconnects first, then immediately drops
        // its logical client table/count before its 60 ms backend grace.
        std::vector<BedrockServerConnection> clients;
        {
            std::lock_guard<std::mutex> lock(serverStateMutex_);
            clients.reserve(connections_.size());
            for (const auto& entry : connections_) {
                clients.push_back(entry.second);
            }
        }
        for (const auto& client : clients) {
            disconnect(client, disconnectReason);
        }

        std::vector<std::pair<BedrockServerConnection, std::shared_ptr<SessionState>>>
            detached;
        {
            std::lock_guard<std::mutex> lock(serverStateMutex_);
            detached.reserve(clients.size());
            for (const auto& client : clients) {
                const auto session = sessions_.find(connectionKey(client.peer));
                if (session != sessions_.end()) {
                    detached.emplace_back(client, session->second);
                }
            }
            connections_.clear();
            sessions_.clear();
            clientCount_ = 0;
        }
        std::vector<std::pair<std::string, BedrockServerClientStatus>> detachedStatuses;
        detachedStatuses.reserve(detached.size());
        for (const auto& entry : detached) {
            {
                std::lock_guard<std::mutex> sessionLock(entry.second->mutex);
                detachedStatuses.emplace_back(
                    playerKey(entry.first),
                    entry.second->status
                );
            }
        }
        {
            std::lock_guard<std::mutex> lock(playerLifecycleMutex_);
            for (const auto& entry : detachedStatuses) {
                if (closedPlayers_.find(entry.first) == closedPlayers_.end()) {
                    detachedPlayerStatuses_[entry.first] = entry.second;
                }
            }
        }

        const bool calledFromWorker = raknet_.onWorkerThread();
        raknet_.closeAfter(std::chrono::milliseconds(60));

        // A JS close invoked from an event handler yields at sleep(60); never
        // block the RakNet loop or join it from itself.  External callers keep
        // the existing synchronous C++ contract and observe the full 60 ms.
        if (!calledFromWorker) {
            const auto now = std::chrono::steady_clock::now();
            if (now < ownDeadline) {
                std::this_thread::sleep_until(ownDeadline);
            }
        }
    }

    bool listening() const {
        return raknet_.listening();
    }

    uint16_t boundPort() const {
        return raknet_.boundPort();
    }

    int clientCount() const {
        return clientCount_.load();
    }

    const BedrockServerOptions& options() const {
        return options_;
    }

    // Mirrors Server#setCompressor. The method defaults intentionally differ
    // from defaultOptions, just as they do in server.js.
    void setCompressor(
        const std::string& algorithm,
        int level = 1,
        uint16_t threshold = 256
    ) {
        if (algorithm == "none") {
            options_.compressionAlgorithm = "none";
            options_.compressionLevel = 0;
            return;
        }
        if (algorithm == "deflate" || algorithm == "snappy") {
            options_.compressionAlgorithm = algorithm;
            options_.compressionLevel = level;
            options_.compressionThreshold = threshold;
            return;
        }

        throw std::runtime_error("Unknown compression algorithm: " + algorithm);
    }

    BedrockServerClientStatus status(const BedrockServerConnection& connection) const {
        const auto session = sessionSnapshot(connection);
        if (session) {
            std::lock_guard<std::mutex> lock(session->mutex);
            return session->status;
        }
        std::lock_guard<std::mutex> lock(playerLifecycleMutex_);
        const auto detached = detachedPlayerStatuses_.find(playerKey(connection));
        return detached == detachedPlayerStatuses_.end()
            ? BedrockServerClientStatus::Disconnected
            : detached->second;
    }

    std::optional<BedrockLoginVerificationResult> loginVerification(
        const BedrockServerConnection& connection
    ) const {
        const auto session = sessionSnapshot(connection);
        if (!session) {
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        return session->loginVerification;
    }

    std::optional<ProtoDefValue> userData(
        const BedrockServerConnection& connection
    ) const {
        const auto login = loginVerification(connection);
        return login ? std::optional<ProtoDefValue>(login->userData) : std::nullopt;
    }

    std::optional<ProtoDefValue> skinData(
        const BedrockServerConnection& connection
    ) const {
        const auto login = loginVerification(connection);
        return login ? std::optional<ProtoDefValue>(login->skinData) : std::nullopt;
    }

    std::optional<BedrockLoginProfile> profile(
        const BedrockServerConnection& connection
    ) const {
        const auto login = loginVerification(connection);
        return login ? std::optional<BedrockLoginProfile>(login->profile) : std::nullopt;
    }

    std::optional<uint32_t> clientVersion(
        const BedrockServerConnection& connection
    ) const {
        const auto login = loginVerification(connection);
        return login ? std::optional<uint32_t>(login->version) : std::nullopt;
    }

    // Server.js exposes the same three helpers directly, but unlike
    // Connection it does not validate string names: a missing lookup becomes
    // undefined/NaN and every relational comparison is false.
    bool versionLessThan(const std::string& version) const {
        const auto* target = findVersion(version);
        return target && configuredServerProtocolVersion() < target->protocolVersion;
    }

    bool versionLessThan(uint32_t protocolVersion) const {
        return configuredServerProtocolVersion() < protocolVersion;
    }

    bool versionGreaterThan(const std::string& version) const {
        const auto* target = findVersion(version);
        return target && configuredServerProtocolVersion() > target->protocolVersion;
    }

    bool versionGreaterThan(uint32_t protocolVersion) const {
        return configuredServerProtocolVersion() > protocolVersion;
    }

    bool versionGreaterThanOrEqualTo(const std::string& version) const {
        const auto* target = findVersion(version);
        return target && configuredServerProtocolVersion() >= target->protocolVersion;
    }

    bool versionGreaterThanOrEqualTo(uint32_t protocolVersion) const {
        return configuredServerProtocolVersion() >= protocolVersion;
    }

    // Connection.js compares against Player.options.protocolVersion, which is
    // the configured server protocol even before the player sends login.  The
    // connection argument keeps this API aligned with the other Player-view
    // accessors (status/userData/profile) without confusing it with the
    // client-reported Player.version.
    bool versionLessThan(
        const BedrockServerConnection& connection,
        const std::string& version
    ) const {
        const auto target = playerComparisonProtocolVersion(version);
        return target && configuredPlayerProtocolVersion(connection) < *target;
    }

    bool versionLessThan(
        const BedrockServerConnection& connection,
        uint32_t protocolVersion
    ) const {
        return configuredPlayerProtocolVersion(connection) < protocolVersion;
    }

    bool versionGreaterThan(
        const BedrockServerConnection& connection,
        const std::string& version
    ) const {
        const auto target = playerComparisonProtocolVersion(version);
        return target && configuredPlayerProtocolVersion(connection) > *target;
    }

    bool versionGreaterThan(
        const BedrockServerConnection& connection,
        uint32_t protocolVersion
    ) const {
        return configuredPlayerProtocolVersion(connection) > protocolVersion;
    }

    bool versionGreaterThanOrEqualTo(
        const BedrockServerConnection& connection,
        const std::string& version
    ) const {
        const auto target = playerComparisonProtocolVersion(version);
        return target && configuredPlayerProtocolVersion(connection) >= *target;
    }

    bool versionGreaterThanOrEqualTo(
        const BedrockServerConnection& connection,
        uint32_t protocolVersion
    ) const {
        return configuredPlayerProtocolVersion(connection) >= protocolVersion;
    }

    // Present in connection.js even though the current index.d.ts omits it.
    bool versionLessThanOrEqualTo(
        const BedrockServerConnection& connection,
        const std::string& version
    ) const {
        const auto target = playerComparisonProtocolVersion(version);
        return target && configuredPlayerProtocolVersion(connection) <= *target;
    }

    bool versionLessThanOrEqualTo(
        const BedrockServerConnection& connection,
        uint32_t protocolVersion
    ) const {
        return configuredPlayerProtocolVersion(connection) <= protocolVersion;
    }

    void send(
        const BedrockServerConnection& connection,
        const std::string& packetName,
        const ProtoDefValue& value,
        VersionedMcpeCompression compression = VersionedMcpeCompression::Automatic
    ) {
        ProtoDefPacketEncoder encoder(options_.version);
        auto payload = encoder.encodePacket(packetName, value);
        sendPacket(
            connection,
            mcpeCodec_.packetCodec().makePacketByName(packetName, payload),
            compression
        );
    }

    void sendPacket(
        const BedrockServerConnection& connection,
        const VersionedGamePacket& packet,
        VersionedMcpeCompression compression = VersionedMcpeCompression::Automatic
    ) {
        sendPackets(connection, std::vector<VersionedGamePacket>{packet}, compression);
    }

    void sendPackets(
        const BedrockServerConnection& connection,
        const std::vector<VersionedGamePacket>& packets,
        VersionedMcpeCompression compression = VersionedMcpeCompression::Automatic
    ) {
        if (packets.empty()) {
            return;
        }

        // connection.js drops writes once its native connection is closed.
        // In particular, a retained Player/close-listener snapshot must not
        // manufacture a new plaintext session after Server#close cleared it.
        const auto session = sessionSnapshot(connection);
        if (!session) {
            return;
        }
        std::vector<uint8_t> mcpe;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->status == BedrockServerClientStatus::Disconnected) {
                return;
            }
            if (session->encryptionEnabled && session->hasEncryptionKeys) {
                if (!session->encryptStream) {
                    throw std::runtime_error("server encrypt stream is not initialized");
                }
                // encryption.js receives the raw framed batch from Framer and
                // always deflates it itself. Its wire format is independent of
                // threshold, compressor selection, compressionReady, and the
                // low-level compression override accepted by this C++ method.
                auto compressionPacket = mcpeCodec_.encodeEncryptedCompressionPacket(
                    packets,
                    session->compressionLevel
                );
                auto aesPlaintext = BedrockEncryption::makeAesPlaintext(
                    compressionPacket,
                    session->sendCounter++,
                    session->encryptionKeys.secretKeyBytes
                );
                auto encryptedOnly = session->encryptStream->process(aesPlaintext);
                mcpe.reserve(1 + encryptedOnly.size());
                mcpe.push_back(0xfe);
                mcpe.insert(mcpe.end(), encryptedOnly.begin(), encryptedOnly.end());
            } else {
                auto compressionPacket = compression == VersionedMcpeCompression::Automatic
                    ? mcpeCodec_.encodeCompressionPacket(
                        packets,
                        options_.compressionAlgorithm,
                        options_.compressionLevel,
                        options_.compressionThreshold
                    )
                    : mcpeCodec_.encodeCompressionPacket(
                        packets,
                        compression,
                        options_.compressionLevel
                    );
                if (compression == VersionedMcpeCompression::Automatic &&
                    mcpeCodec_.compressorInPacketHeader() &&
                    !session->compressionReady) {
                    if (compressionPacket.empty()) {
                        throw std::runtime_error("modern compression packet missing mode byte");
                    }
                    compressionPacket.erase(compressionPacket.begin());
                }

                mcpe.reserve(1 + compressionPacket.size());
                mcpe.push_back(0xfe);
                mcpe.insert(mcpe.end(), compressionPacket.begin(), compressionPacket.end());
            }
        }

        if (hasWritablePlayer(connection)) {
            raknet_.sendReliable(connection.peer, mcpe);
        }
    }

    void disconnect(
        const BedrockServerConnection& connection,
        const std::string& reason = "Server closed",
        bool hide = false
    ) {
        if (!hasWritablePlayer(connection) ||
            status(connection) == BedrockServerClientStatus::Disconnected) {
            return;
        }
        if (!mcpeCodec_.definition().hasPacket("disconnect")) {
            return;
        }

        send(connection, "disconnect", ProtoDefValue::object({
            {"reason", ProtoDefValue::string("unknown")},
            // serverPlayer.js writes hide_disconnect_screen even though the
            // protocol field is named hide_disconnect_reason.  Node ProtoDef
            // consequently writes its false default.  Our encoder requires
            // every schema field explicitly, so encode that same wire value
            // and intentionally ignore `hide`.
            {"hide_disconnect_reason", ProtoDefValue::boolean(false)},
            {"message", ProtoDefValue::string(reason)},
            {"filtered_message", ProtoDefValue::string("")}
        }));
        schedulePlayerClose(connection, std::chrono::milliseconds(100));
    }

    void sendDisconnectStatus(
        const BedrockServerConnection& connection,
        const std::string& playStatus
    ) {
        if (!hasWritablePlayer(connection) ||
            status(connection) == BedrockServerClientStatus::Disconnected) {
            return;
        }
        if (!mcpeCodec_.definition().hasPacket("play_status")) {
            return;
        }

        send(connection, "play_status", ProtoDefValue::object({
            {"status", ProtoDefValue::string(playStatus)}
        }));
        closePlayer(connection);
    }

private:
    struct CreateServerTag {};

    static std::filesystem::path playerProtocolTypeBasePath() {
        // Development builds can use the exact per-version TSV schemas even
        // when CTest's working directory is the build tree. Installed builds
        // still fall back to the embedded generated schema registry.
        auto sourceRoot = std::filesystem::path(__FILE__).parent_path();
        for (int i = 0; i < 3; ++i) {
            sourceRoot = sourceRoot.parent_path();
        }
        return sourceRoot / "data/generated/protocol-types/bedrock";
    }

    BedrockServer(CreateServerTag, BedrockServerOptions options)
        : BedrockServer(std::move(options)) {
        listen();
    }

    friend BedrockServer createServer(BedrockServerOptions options);
    friend struct BedrockServerTestAccess;

    BedrockServerOptions options_;
    ServerAdvertisement advertisement_;
    RakNetServer raknet_;
    VersionedMcpeCodec mcpeCodec_;
    ProtocolTypeTsvIndex playerProtocolTypes_ {playerProtocolTypeBasePath()};
    std::vector<ConnectionHandler> connectHandlers_;
    std::vector<ConnectionHandler> joinHandlers_;
    std::vector<ConnectionHandler> spawnHandlers_;
    std::vector<ConnectionHandler> disconnectHandlers_;
    std::vector<PacketHandler> loginHandlers_;
    std::vector<StatusHandler> statusHandlers_;
    std::vector<PacketHandler> anyPacketHandlers_;
    std::unordered_map<std::string, std::vector<PacketHandler>> packetHandlers_;
    mutable std::mutex serverStateMutex_;
    std::unordered_map<std::string, BedrockServerConnection> connections_;
    std::atomic<int> clientCount_ {0};
    std::atomic<bool> closing_ {false};

    struct ScheduledPlayerClose {
        std::chrono::steady_clock::time_point deadline;
        BedrockServerConnection connection;
        uint64_t order = 0;
    };

    mutable std::mutex playerLifecycleMutex_;
    std::condition_variable playerLifecycleCv_;
    std::thread playerLifecycleThread_;
    std::vector<ScheduledPlayerClose> scheduledPlayerCloses_;
    std::unordered_set<std::string> closedPlayers_;
    std::unordered_map<std::string, BedrockServerClientStatus> detachedPlayerStatuses_;
    bool playerLifecycleStopping_ = false;
    uint64_t scheduledPlayerCloseOrder_ = 0;

    static BedrockServerOptions normalizeOptions(BedrockServerOptions options) {
        (void) validateVersion(options.version);
        const auto name = options.motd.find("name");
        if (name != options.motd.end() && name->second.isTruthy()) {
            // ServerAdvertisement's constructor mutates the object supplied by
            // Server: a truthy legacy name becomes motd before Object.assign.
            const auto alias = name->second;
            options.motd["motd"] = alias;
        }
        if (options.compressionAlgorithm == "none") {
            options.compressionLevel = 0;
        } else if (options.compressionAlgorithm != "deflate" &&
                   options.compressionAlgorithm != "snappy") {
            throw std::runtime_error(
                "Unknown compression algorithm: " + options.compressionAlgorithm
            );
        }
        return options;
    }

    static RakNetServerOptions makeRakNetOptions(
        const BedrockServerOptions& options,
        std::string advertisement
    ) {
        RakNetServerOptions raknet;
        raknet.host = options.host;
        raknet.port = options.port;
        raknet.maxPlayers = options.maxPlayers;
        // node_modules/bedrock-protocol/src/rak.js switches at 1.19.30
        // (Minecraft protocol 554), not at 1.20.0.
        raknet.protocolVersion = protocolVersionForMinecraft(options.version) >= 554 ? 11 : 10;
        raknet.advertisement = std::move(advertisement);
        return raknet;
    }

    static int protocolVersionForMinecraft(const std::string& version) {
        return static_cast<int>(ProtocolDefinition::forVersion(version).protocolVersion());
    }

    static bool isObjectPrototypeVersionName(std::string_view version) {
        // Options.Versions is a normal JavaScript object.  Missing names that
        // resolve through Object.prototype pass Connection's truthiness guard
        // and then coerce to NaN, making every relational comparison false.
        for (const std::string_view inherited : {
                 "constructor",
                 "__defineGetter__",
                 "__defineSetter__",
                 "hasOwnProperty",
                 "__lookupGetter__",
                 "__lookupSetter__",
                 "isPrototypeOf",
                 "propertyIsEnumerable",
                 "toString",
                 "valueOf",
                 "__proto__",
                 "toLocaleString"
             }) {
            if (version == inherited) {
                return true;
            }
        }
        return false;
    }

    static std::optional<uint32_t> playerComparisonProtocolVersion(
        const std::string& version
    ) {
        if (const auto* entry = findVersion(version)) {
            return entry->protocolVersion;
        }
        if (isObjectPrototypeVersionName(version)) {
            return std::nullopt;
        }
        throw std::runtime_error("Unknown version: " + version);
    }

    uint32_t configuredPlayerProtocolVersion(
        const BedrockServerConnection& connection
    ) const {
        (void) connection;
        return configuredServerProtocolVersion();
    }

    uint32_t configuredServerProtocolVersion() const {
        return protocolVersionFor(options_.version);
    }

    static ServerAdvertisement makeAdvertisement(const BedrockServerOptions& options) {
        ServerAdvertisement advertisement(options.motd, options.port, options.version);
        // server.js overwrites an object-provided playersMax after constructing
        // ServerAdvertisement.  Nullish defaults resolve to 3, while zero must
        // remain zero.
        advertisement.playersMax = options.maxPlayers;
        return advertisement;
    }

    static std::string connectionKey(const RakNetServerPeer& peer) {
        return peer.address + ":" + std::to_string(peer.port);
    }

    static std::string playerKey(const BedrockServerConnection& connection) {
        return connectionKey(connection.peer) + "#" +
            std::to_string(connection.clientGuid);
    }

    bool hasActivePlayer(const BedrockServerConnection& connection) const {
        const auto session = sessionSnapshot(connection);
        if (!session) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->status == BedrockServerClientStatus::Disconnected) {
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(playerLifecycleMutex_);
            return closedPlayers_.find(playerKey(connection)) == closedPlayers_.end();
        }
    }

    bool hasWritablePlayer(const BedrockServerConnection& connection) const {
        const auto session = sessionSnapshot(connection);
        if (!session) {
            return false;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        return session->status != BedrockServerClientStatus::Disconnected;
    }

    void schedulePlayerClose(
        const BedrockServerConnection& connection,
        std::chrono::milliseconds delay
    ) {
        // Live Player.close delivery belongs on the RakNet worker. This keeps
        // connection/session mutation serialized with receive/open/close and
        // uses the native delayed CloseConnection queue. The small fallback
        // scheduler exists only for a stopped backend (Server#close keeps the
        // JS Player timer alive) and deterministic no-network tests; in that
        // state closePlayer never mutates the cleared live maps.
        if (raknet_.listening() && !closing_.load()) {
            raknet_.closePeerAfter(connection.peer, delay);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(playerLifecycleMutex_);
            if (playerLifecycleStopping_) {
                return;
            }
            if (!playerLifecycleThread_.joinable()) {
                playerLifecycleThread_ = std::thread([this]() {
                    runPlayerCloseScheduler();
                });
            }
            scheduledPlayerCloses_.push_back({
                std::chrono::steady_clock::now() + delay,
                connection,
                scheduledPlayerCloseOrder_++
            });
        }
        playerLifecycleCv_.notify_one();
    }

    void runPlayerCloseScheduler() {
        std::unique_lock<std::mutex> lock(playerLifecycleMutex_);
        while (!playerLifecycleStopping_) {
            if (scheduledPlayerCloses_.empty()) {
                playerLifecycleCv_.wait(lock, [this]() {
                    return playerLifecycleStopping_ || !scheduledPlayerCloses_.empty();
                });
                continue;
            }

            const auto next = *std::min_element(
                scheduledPlayerCloses_.begin(),
                scheduledPlayerCloses_.end(),
                [](const ScheduledPlayerClose& lhs, const ScheduledPlayerClose& rhs) {
                    return lhs.deadline < rhs.deadline ||
                        (lhs.deadline == rhs.deadline && lhs.order < rhs.order);
                }
            );
            // wait_until releases the mutex. Keep only a value/unique order
            // across that wait: schedulePlayerClose may reallocate the vector.
            if (playerLifecycleCv_.wait_until(lock, next.deadline) !=
                std::cv_status::timeout) {
                continue;
            }

            const auto due = std::find_if(
                scheduledPlayerCloses_.begin(),
                scheduledPlayerCloses_.end(),
                [&](const ScheduledPlayerClose& scheduled) {
                    return scheduled.order == next.order;
                }
            );
            if (due == scheduledPlayerCloses_.end()) {
                continue;
            }
            const auto scheduled = *due;
            scheduledPlayerCloses_.erase(due);
            lock.unlock();
            closePlayer(scheduled.connection);
            lock.lock();
        }
    }

    void stopPlayerCloseScheduler() {
        {
            std::lock_guard<std::mutex> lock(playerLifecycleMutex_);
            playerLifecycleStopping_ = true;
            scheduledPlayerCloses_.clear();
        }
        playerLifecycleCv_.notify_all();
        if (playerLifecycleThread_.joinable() &&
            playerLifecycleThread_.get_id() != std::this_thread::get_id()) {
            playerLifecycleThread_.join();
        }
    }

    void closePlayer(const BedrockServerConnection& connection) {
        const auto key = playerKey(connection);
        {
            std::lock_guard<std::mutex> lock(playerLifecycleMutex_);
            if (!closedPlayers_.insert(key).second) {
                return;
            }
        }

        // Player.close emits its per-player close listeners synchronously at
        // the old status. The global callback is an existing C++ extension.
        emitPlayerClose(connection);
        raknet_.closePeer(connection.peer);
        clearPlayerListeners(connection);

        const auto session = sessionSnapshot(connection);
        if (session) {
            std::lock_guard<std::mutex> sessionLock(session->mutex);
            session->status = BedrockServerClientStatus::Disconnected;
        }

        std::lock_guard<std::mutex> lock(playerLifecycleMutex_);
        detachedPlayerStatuses_.erase(key);
    }

    void emitPlayerError(
        const BedrockServerConnection& connection,
        const std::string& message
    ) {
        const auto handlers = connection.playerEvents
            ? connection.playerEvents->errorSnapshot()
            : std::vector<BedrockServerConnection::ErrorHandler>{};
        if (handlers.empty()) {
            // EventEmitter throws the Error supplied to an unhandled `error`
            // event. Keep a distinct C++ type while preserving Error#message.
            throw BedrockUnhandledPlayerError(message);
        }
        for (const auto& handler : handlers) {
            handler(message);
        }
    }

    void emitPlayerClose(const BedrockServerConnection& connection) {
        const auto playerHandlers = connection.playerEvents
            ? connection.playerEvents->closeSnapshot()
            : std::vector<BedrockServerConnection::CloseHandler>{};
        std::vector<ConnectionHandler> serverHandlers;
        {
            std::lock_guard<std::mutex> lock(playerLifecycleMutex_);
            serverHandlers = disconnectHandlers_;
        }

        for (const auto& handler : playerHandlers) {
            handler();
        }
        for (const auto& handler : serverHandlers) {
            handler(connection);
        }
    }

    static void clearPlayerListeners(const BedrockServerConnection& connection) {
        if (connection.playerEvents) {
            connection.playerEvents->clear();
        }
    }

    struct SessionState {
        mutable std::mutex mutex;
        BedrockClientKeyPair serverKeys;
        std::vector<uint8_t> salt;
        std::string clientPublicKeyDerBase64;
        DerivedKeyResult encryptionKeys;
        std::unique_ptr<BedrockCipherStream> encryptStream;
        std::unique_ptr<BedrockCipherStream> decryptStream;
        bool hasEncryptionKeys = false;
        bool encryptionEnabled = false;
        bool compressionReady = false;
        bool networkSettingsSent = false;
        bool resourcePacksInfoSent = false;
        bool resourcePackStackSent = false;
        std::optional<BedrockLoginVerificationResult> loginVerification;
        BedrockServerClientStatus status = BedrockServerClientStatus::Authenticating;
        int compressionLevel = 7;
        uint64_t sendCounter = 0;
        uint64_t receiveCounter = 0;
    };

    std::unordered_map<std::string, std::shared_ptr<SessionState>> sessions_;

    std::shared_ptr<SessionState> sessionSnapshot(
        const BedrockServerConnection& connection
    ) const {
        const auto key = connectionKey(connection.peer);
        std::lock_guard<std::mutex> lock(serverStateMutex_);
        const auto connectionIt = connections_.find(key);
        if (connectionIt == connections_.end() ||
            connectionIt->second.clientGuid != connection.clientGuid) {
            return {};
        }
        const auto sessionIt = sessions_.find(key);
        return sessionIt == sessions_.end() ? nullptr : sessionIt->second;
    }

    std::optional<std::pair<BedrockServerConnection, std::shared_ptr<SessionState>>>
    playerSnapshot(const RakNetServerPeer& peer) const {
        const auto key = connectionKey(peer);
        std::lock_guard<std::mutex> lock(serverStateMutex_);
        const auto connectionIt = connections_.find(key);
        if (connectionIt == connections_.end() ||
            connectionIt->second.clientGuid != peer.clientGuid) {
            return std::nullopt;
        }
        const auto sessionIt = sessions_.find(key);
        if (sessionIt == sessions_.end()) {
            return std::nullopt;
        }
        return std::make_pair(connectionIt->second, sessionIt->second);
    }

    static std::vector<uint8_t> inflatePlayerRaw(
        const std::vector<uint8_t>& input
    ) {
        z_stream stream{};
        const int init = inflateInit2(&stream, -MAX_WBITS);
        if (init != Z_OK) {
            throw std::runtime_error("inflateInit2 failed");
        }

        uint8_t dummy = 0;
        stream.next_in = input.empty()
            ? reinterpret_cast<Bytef*>(&dummy)
            : const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
        stream.avail_in = static_cast<uInt>(input.size());

        std::vector<uint8_t> output;
        uint8_t buffer[32768];
        while (true) {
            stream.next_out = buffer;
            stream.avail_out = sizeof(buffer);

            const int result = inflate(&stream, Z_NO_FLUSH);
            const std::size_t produced = sizeof(buffer) - stream.avail_out;
            output.insert(output.end(), buffer, buffer + produced);

            if (result == Z_STREAM_END) {
                break;
            }
            if (result != Z_OK) {
                inflateEnd(&stream);
                throw std::runtime_error("inflate failed");
            }
            if (stream.avail_in == 0 && produced == 0) {
                inflateEnd(&stream);
                throw std::runtime_error("incomplete deflate stream");
            }
        }

        inflateEnd(&stream);
        return output;
    }

    std::vector<uint8_t> decodeEncryptedFramedBatch(
        const BedrockServerConnection& connection,
        const std::vector<uint8_t>& compressionPacket
    ) {
        if (!mcpeCodec_.compressorInPacketHeader()) {
            return inflatePlayerRaw(compressionPacket);
        }

        const bool hasCompressor = !compressionPacket.empty();
        const uint8_t compressor = hasCompressor ? compressionPacket[0] : 0;
        if (hasCompressor && compressor == 0) {
            return inflatePlayerRaw(std::vector<uint8_t>(
                compressionPacket.begin() + 1,
                compressionPacket.end()
            ));
        }
        if (hasCompressor && compressor == 0xff) {
            return std::vector<uint8_t>(
                compressionPacket.begin() + 1,
                compressionPacket.end()
            );
        }

        const auto value = hasCompressor
            ? std::to_string(compressor)
            : std::string("undefined");
        emitPlayerError(connection, "Unsupported compressor: " + value);

        // encryption.js leaves `buffer` undefined after a handled compressor
        // error and still calls onDecryptedPacket(buffer). Framer.getPackets
        // then throws this TypeError before any disconnect can run.
        throw BedrockUndefinedPlayerBufferError();
    }

    std::vector<uint8_t> decodeUnencryptedFramedBatch(
        bool compressionReady,
        const std::vector<uint8_t>& compressionPacket
    ) const {
        if (mcpeCodec_.compressorInPacketHeader() && compressionReady) {
            if (compressionPacket.empty()) {
                throw std::runtime_error("Unknown compression type undefined");
            }

            const uint8_t compressor = compressionPacket[0];
            std::vector<uint8_t> body(
                compressionPacket.begin() + 1,
                compressionPacket.end()
            );
            if (compressor == 0) {
                return inflatePlayerRaw(body);
            }
            if (compressor == 0xff) {
                return body;
            }
            if (compressor == 1) {
                throw std::runtime_error("Snappy compression not implemented");
            }
            throw std::runtime_error(
                "Unknown compression type " + std::to_string(compressor)
            );
        }

        // Framer.decode treats failed session-wide decompression as an
        // uncompressed batch on legacy versions and before network_settings.
        if (options_.compressionAlgorithm == "deflate") {
            try {
                return inflatePlayerRaw(compressionPacket);
            } catch (const std::exception&) {
                return compressionPacket;
            }
        }
        return compressionPacket;
    }

    std::optional<std::string> resolvePlayerProtocolType(
        const std::string& typeName
    ) const {
        auto versioned = playerProtocolTypes_.findTypeJson(
            options_.version,
            typeName
        );
        if (versioned.has_value()) {
            return versioned;
        }
        return generatedProtocolTypeJson(typeName);
    }

    void validatePlayerPacket(const VersionedGamePacket& packet) const {
        if (!mcpeCodec_.definition().hasPacket(packet.packetId)) {
            throw std::runtime_error("unknown packet id");
        }

        const auto typeName = packet.paramsType.empty()
            ? "packet_" + packet.name
            : packet.paramsType;
        const auto typeJson = resolvePlayerProtocolType(typeName);
        if (!typeJson.has_value()) {
            throw std::runtime_error("packet schema not found: " + packet.name);
        }

        PacketFieldCursor cursor(packet.payload);
        ProtoDefReader reader(cursor);
        ProtoDefContext context;
        std::vector<ProtoDefField> fields;
        ProtoDefDecoder decoder([this](const std::string& nestedType) {
            return resolvePlayerProtocolType(nestedType);
        });
        decoder.decode(*typeJson, reader, "", fields, context);
        if (std::any_of(
                fields.begin(),
                fields.end(),
                [](const ProtoDefField& field) {
                    return field.value.find("<missing:") != std::string::npos;
                }
            )) {
            throw std::runtime_error("packet field missing");
        }
    }

    std::vector<VersionedGamePacket> decodePlayerFramedBatch(
        const BedrockServerConnection& connection,
        const std::vector<uint8_t>& framedBatch
    ) {
        std::vector<VersionedGamePacket> packets;
        std::size_t offset = 0;
        while (offset < framedBatch.size()) {
            // Framer errors are outside Player.readPacket's try/catch and
            // therefore propagate without an error event or disconnect.
            const uint32_t packetSize = VersionedPacketCodec::readVarUInt(
                framedBatch,
                offset
            );
            if (offset + packetSize > framedBatch.size()) {
                throw std::runtime_error("framed batch packet exceeds buffer size");
            }

            std::vector<uint8_t> fullPacket(
                framedBatch.begin() + static_cast<std::ptrdiff_t>(offset),
                framedBatch.begin() + static_cast<std::ptrdiff_t>(offset + packetSize)
            );
            offset += packetSize;

            try {
                auto packet = mcpeCodec_.packetCodec().decodeFullPacket(fullPacket);
                validatePlayerPacket(packet);
                packets.push_back(std::move(packet));
            } catch (const std::exception&) {
                // serverPlayer.readPacket catches only deserializer failures,
                // sends Server error, and returns to the surrounding batch.
                disconnect(connection, "Server error");
            }
        }
        return packets;
    }

    std::vector<VersionedGamePacket> decodeEncryptedPlayerPackets(
        const BedrockServerConnection& connection,
        const std::vector<uint8_t>& compressionPacket
    ) {
        return decodePlayerFramedBatch(
            connection,
            decodeEncryptedFramedBatch(connection, compressionPacket)
        );
    }

    void handleEncapsulated(const RakNetServerPeer& peer, const std::vector<uint8_t>& payload) {
        if (closing_.load()) {
            return;
        }

        const auto player = playerSnapshot(peer);
        if (!player.has_value()) {
            // RakNet-native must never surface an unknown endpoint as a new
            // Player. This also makes late datagrams after Server#close inert.
            return;
        }
        const auto connection = player->first;
        const auto session = player->second;
        if (payload.empty() || payload[0] != 0xfe) {
            throw std::runtime_error(
                "Bad packet header " +
                (payload.empty()
                    ? std::string("undefined")
                    : std::to_string(payload[0]))
            );
        }

        std::vector<VersionedGamePacket> packets;
        bool encryptedSession = false;
        bool compressionReady = false;
        std::optional<BedrockChecksumVerification> verification;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            encryptedSession = session->encryptionEnabled && session->hasEncryptionKeys;
            compressionReady = session->compressionReady;
            if (encryptedSession) {
                if (!session->decryptStream) {
                    throw std::runtime_error("server decrypt stream is not initialized");
                }

                std::vector<uint8_t> encryptedOnly(payload.begin() + 1, payload.end());
                verification = BedrockEncryption::decryptAndVerify(
                    *session->decryptStream,
                    encryptedOnly,
                    session->receiveCounter,
                    session->encryptionKeys.secretKeyBytes
                );
            }
        }
        if (encryptedSession) {
            if (!verification) {
                return;
            }
            if (!verification->matches()) {
                const auto message = verification->mismatchMessage();
                emitPlayerError(connection, message);
                // A listener can synchronously close the player/server. Avoid
                // recreating sessions_[key] after that external state change.
                if (hasActivePlayer(connection)) {
                    disconnect(connection, "disconnectionScreen.badPacket");
                }
                return;
            }
            packets = decodeEncryptedPlayerPackets(
                connection,
                verification->packetPlaintext
            );
        } else {
            packets = decodePlayerFramedBatch(
                connection,
                decodeUnencryptedFramedBatch(
                    compressionReady,
                    std::vector<uint8_t>(payload.begin() + 1, payload.end())
                )
            );
        }

        for (const auto& packet : packets) {
            if (!hasActivePlayer(connection)) {
                return;
            }
            BedrockServerPacketEvent event;
            event.connection = connection;
            event.packet = packet;

            // serverPlayer.js performs its switch first. Network settings and
            // login return from readPacket and are intentionally invisible to
            // both named and generic packet listeners.
            if (handleBuiltInPacket(connection, packet)) {
                continue;
            }

            const auto sessionStatus = status(connection);
            if (sessionStatus == BedrockServerClientStatus::Disconnected ||
                sessionStatus == BedrockServerClientStatus::Authenticating) {
                continue;
            }

            auto nameIt = packetHandlers_.find(packet.name);
            if (nameIt != packetHandlers_.end()) {
                for (auto& handler : nameIt->second) {
                    handler(event);
                }
            }

            for (auto& handler : anyPacketHandlers_) {
                handler(event);
            }
        }
    }

    void handlePeerClosed(const RakNetServerPeer& peer) {
        const auto key = connectionKey(peer);
        const auto player = playerSnapshot(peer);
        if (!player.has_value()) {
            // A native close racing Server#close, or a stale close for a
            // reused endpoint, is not a Player event and must not decrement.
            return;
        }
        const auto connection = player->first;

        bool shouldEmitPlayerClose = false;
        {
            std::lock_guard<std::mutex> lock(playerLifecycleMutex_);
            shouldEmitPlayerClose = closedPlayers_.insert(playerKey(connection)).second;
        }
        if (shouldEmitPlayerClose) {
            // Incoming native close invokes Player.close before Server
            // deletes the client and decrements clientCount.
            emitPlayerClose(connection);
            clearPlayerListeners(connection);
        }
        {
            std::lock_guard<std::mutex> lock(playerLifecycleMutex_);
            detachedPlayerStatuses_.erase(playerKey(connection));
        }

        bool erased = false;
        {
            std::lock_guard<std::mutex> lock(serverStateMutex_);
            const auto current = connections_.find(key);
            if (current != connections_.end() &&
                current->second.clientGuid == peer.clientGuid) {
                connections_.erase(current);
                sessions_.erase(key);
                erased = true;
            }
        }
        if (erased) {
            --clientCount_;
        }
    }

    // Returns true when serverPlayer.js returns directly from readPacket.
    bool handleBuiltInPacket(
        const BedrockServerConnection& connection,
        const VersionedGamePacket& packet
    ) {
        if (packet.name == "request_network_settings") {
            if (mcpeCodec_.definition().hasPacket("network_settings")) {
                sendNetworkSettings(connection, true);
            }
            return true;
        }

        if (packet.name == "login") {
            if (options_.autoLogin &&
                mcpeCodec_.definition().hasPacket("server_to_client_handshake")) {
                handleLogin(connection, packet);
            }
            const auto session = sessionSnapshot(connection);
            bool shouldSendNetworkSettings = false;
            if (session) {
                std::lock_guard<std::mutex> lock(session->mutex);
                shouldSendNetworkSettings = !session->networkSettingsSent;
            }
            if (shouldSendNetworkSettings &&
                mcpeCodec_.definition().hasPacket("network_settings")) {
                sendNetworkSettings(connection, false);
            }
            return true;
        }

        if (packet.name == "client_to_server_handshake" && mcpeCodec_.definition().hasPacket("play_status")) {
            send(connection, "play_status", ProtoDefValue::object({
                {"status", ProtoDefValue::string("login_success")}
            }));

            setStatus(connection, BedrockServerClientStatus::Initializing);

            if (options_.autoResourcePacks &&
                mcpeCodec_.definition().hasPacket("resource_packs_info")) {
                sendEmptyResourcePacksInfo(connection);
            } else {
                emitJoin(connection);
            }
            return false;
        }

        if (packet.name == "set_local_player_as_initialized") {
            setStatus(connection, BedrockServerClientStatus::Initialized);
            emitSpawn(connection);
            return false;
        }

        if (options_.autoResourcePacks &&
            packet.name == "resource_pack_client_response") {
            handleResourcePackClientResponse(connection, packet);
        }

        return false;
    }

    void handleLogin(
        const BedrockServerConnection& connection,
        const VersionedGamePacket& packet
    ) {
        LoginPacketData login;
        BedrockLoginVerificationResult verifiedLogin;
        try {
            login = LoginPacketCodec::decode(packet.fullPacket);
            verifiedLogin = BedrockLoginVerifier::verify(login, options_.offline);
        } catch (const std::exception&) {
            disconnect(connection, "Server authentication error");
            return;
        }

        // loginVerify.js disconnects untrusted online clients but still
        // returns decoded data, so serverPlayer.js continues the handshake
        // and emits login. Preserve that observable control flow.
        if (verifiedLogin.disconnectNotAuthenticated) {
            disconnect(connection, "disconnectionScreen.notAuthenticated");
        }

        const auto session = sessionSnapshot(connection);
        if (!session) {
            return;
        }
        std::string token;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->serverKeys.privateKeyPem.empty()) {
                session->serverKeys = BedrockAuthJwt::generateP384KeyPair();
            }
            if (session->salt.empty()) {
                session->salt = {0xf0, 0x9f, 0xa7, 0x82};
            }
            session->clientPublicKeyDerBase64 = verifiedLogin.key;
            session->encryptionKeys = BedrockKeyExchange::deriveFromRemotePublicKeyDerBase64AndPrivateKeyPem(
                session->clientPublicKeyDerBase64,
                session->serverKeys.privateKeyPem,
                session->salt
            );
            const auto protocolVersion = configuredServerProtocolVersion();
            session->encryptStream = BedrockEncryption::createCipherStream(
                protocolVersion,
                session->encryptionKeys.secretKeyBytes,
                session->encryptionKeys.iv16,
                BedrockCipherMode::Encrypt
            );
            session->decryptStream = BedrockEncryption::createCipherStream(
                protocolVersion,
                session->encryptionKeys.secretKeyBytes,
                session->encryptionKeys.iv16,
                BedrockCipherMode::Decrypt
            );
            session->hasEncryptionKeys = true;

            const std::string payloadJson =
                "{\"salt\":\"" + BedrockAuthJwt::base64(session->salt) +
                "\",\"signedToken\":\"" + session->serverKeys.publicKeyDerBase64 + "\"}";

            token = BedrockAuthJwt::signEs384Jwt(
                session->serverKeys.privateKeyPem,
                session->serverKeys.publicKeyDerBase64,
                payloadJson
            );
        }

        send(connection, "server_to_client_handshake", ProtoDefValue::object({
            {"token", ProtoDefValue::string(token)}
        }));

        if (!hasWritablePlayer(connection)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->encryptionEnabled = true;
            session->sendCounter = 0;
            session->receiveCounter = 0;
            session->loginVerification = std::move(verifiedLogin);
        }

        BedrockServerPacketEvent event;
        event.connection = connection;
        event.packet = packet;
        for (auto& handler : loginHandlers_) {
            handler(event);
        }
    }

    void sendPreCompression(
        const BedrockServerConnection& connection,
        const std::string& packetName,
        const ProtoDefValue& value
    ) {
        if (!hasActivePlayer(connection)) {
            return;
        }
        ProtoDefPacketEncoder encoder(options_.version);
        auto payload = encoder.encodePacket(packetName, value);
        auto packet = mcpeCodec_.packetCodec().makePacketByName(packetName, payload);
        auto framed = mcpeCodec_.batchCodec().encodeFramedBatch({packet});

        std::vector<uint8_t> mcpe;
        mcpe.reserve(1 + framed.size());
        mcpe.push_back(0xfe);
        mcpe.insert(mcpe.end(), framed.begin(), framed.end());
        if (hasWritablePlayer(connection)) {
            raknet_.sendReliable(connection.peer, mcpe);
        }
    }

    void sendNetworkSettings(
        const BedrockServerConnection& connection,
        bool preLoginRequest
    ) {
        auto value = ProtoDefValue::object({
            {"compression_threshold", ProtoDefValue::uinteger(options_.compressionThreshold)},
            {"compression_algorithm", ProtoDefValue::string(options_.compressionAlgorithm)},
            {"client_throttle", ProtoDefValue::boolean(false)},
            {"client_throttle_threshold", ProtoDefValue::uinteger(0)},
            {"client_throttle_scalar", ProtoDefValue::floating(0.0)}
        });
        if (preLoginRequest) {
            sendPreCompression(connection, "network_settings", value);
        } else {
            send(connection, "network_settings", value);
        }
        const auto session = sessionSnapshot(connection);
        if (!session) {
            return;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        session->networkSettingsSent = true;
        session->compressionReady = true;
        // serverPlayer.js refreshes only this Player field, and only after a
        // modern request_network_settings path. The legacy login fallback
        // retains the constructor snapshot.
        if (preLoginRequest) {
            session->compressionLevel = options_.compressionLevel;
        }
    }

    void handleResourcePackClientResponse(
        const BedrockServerConnection& connection,
        const VersionedGamePacket& packet
    ) {
        const auto session = sessionSnapshot(connection);
        if (!session) {
            return;
        }
        const uint8_t status = packet.payload.empty() ? 0xff : packet.payload[0];
        bool resourcePackStackSent = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            resourcePackStackSent = session->resourcePackStackSent;
        }

        if (status == 0x03 && !resourcePackStackSent) {
            sendEmptyResourcePackStack(connection);
            if (hasActivePlayer(connection)) {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->resourcePackStackSent = true;
            }
            return;
        }

        if (status == 0x04 && !resourcePackStackSent) {
            sendEmptyResourcePackStack(connection);
            if (hasActivePlayer(connection)) {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->resourcePackStackSent = true;
            }
            return;
        }

        if (status == 0x04) {
            emitJoin(connection);
        }
    }

    void sendEmptyResourcePacksInfo(const BedrockServerConnection& connection) {
        if (!sessionSnapshot(connection)) {
            return;
        }
        send(connection, "resource_packs_info", ProtoDefValue::object({
            {"must_accept", ProtoDefValue::boolean(false)},
            {"has_addons", ProtoDefValue::boolean(false)},
            {"has_scripts", ProtoDefValue::boolean(false)},
            {"disable_vibrant_visuals", ProtoDefValue::boolean(false)},
            {"force_server_packs", ProtoDefValue::boolean(false)},
            {"world_template", ProtoDefValue::object({
                {"uuid", ProtoDefValue::string("00000000-0000-0000-0000-000000000000")},
                {"version", ProtoDefValue::string("")}
            })},
            {"behaviour_packs", ProtoDefValue::array({})},
            {"texture_packs", ProtoDefValue::array({})},
            {"resource_pack_links", ProtoDefValue::array({})}
        }));
        const auto afterSend = sessionSnapshot(connection);
        if (afterSend) {
            std::lock_guard<std::mutex> lock(afterSend->mutex);
            afterSend->resourcePacksInfoSent = true;
        }
    }

    void sendEmptyResourcePackStack(const BedrockServerConnection& connection) {
        send(connection, "resource_pack_stack", ProtoDefValue::object({
            {"must_accept", ProtoDefValue::boolean(false)},
            {"behavior_packs", ProtoDefValue::array({})},
            {"resource_packs", ProtoDefValue::array({})},
            {"game_version", ProtoDefValue::string(options_.version)},
            {"experiments", ProtoDefValue::array({})},
            {"experiments_previously_used", ProtoDefValue::boolean(false)},
            {"has_editor_packs", ProtoDefValue::boolean(false)}
        }));
    }

    void emitJoin(const BedrockServerConnection& connection) {
        for (auto& handler : joinHandlers_) {
            handler(connection);
        }
    }

    void setStatus(
        const BedrockServerConnection& connection,
        BedrockServerClientStatus nextStatus
    ) {
        // Connection.status in the JavaScript implementation emits before it
        // stores, so status(connection) intentionally exposes the old value to
        // status listeners.
        for (auto& handler : statusHandlers_) {
            handler(connection, nextStatus);
        }
        const auto session = sessionSnapshot(connection);
        if (session) {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->status = nextStatus;
        }
    }

    void emitSpawn(const BedrockServerConnection& connection) {
        for (auto& handler : spawnHandlers_) {
            handler(connection);
        }
    }
};

inline BedrockServer createServer(BedrockServerOptions options) {
    // createServer.js treats every falsy port as the default. A uint16_t has
    // only one falsy value, so zero must not request an ephemeral port here.
    if (options.port == 0) {
        options.port = 19132;
    }
    return BedrockServer(BedrockServer::CreateServerTag{}, std::move(options));
}

} // namespace bedrock
