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
#include <bedrock/protocol/SnappyCodec.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/protodef/ProtoDefContext.hpp>
#include <bedrock/protodef/ProtoDefDecoder.hpp>
#include <bedrock/protodef/ProtoDefField.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/protodef/ProtoDefPacketVariables.hpp>
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
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bedrock {

struct BedrockServerTestAccess;

// Compact MOTD value for the public ServerOptions facade.  It accepts the
// JavaScript-style title/level-name pair while remaining source-compatible
// with the historical ServerAdvertisementObject initializer:
//
//   .motd = {"My server", "My world"}
//   .motd = {{"motd", "My server"}}
class ServerMotd : public ServerAdvertisementObject {
public:
    using Base = ServerAdvertisementObject;
    using Value = Base::value_type;

    ServerMotd() = default;

    ServerMotd(std::initializer_list<Value> values)
        : Base(values) {}

    ServerMotd(Base values)
        : Base(std::move(values)) {}

    ServerMotd(std::string title, std::string levelName = {}) {
        (*this)["motd"] = std::move(title);
        if (!levelName.empty()) {
            (*this)["levelName"] = std::move(levelName);
        }
    }

    ServerMotd(const char* title)
        : ServerMotd(std::string(title ? title : "")) {}

    ServerMotd(const char* title, const char* levelName)
        : ServerMotd(
              std::string(title ? title : ""),
              std::string(levelName ? levelName : "")
          ) {}

    ServerMotd& operator=(std::string title) {
        clear();
        (*this)["motd"] = std::move(title);
        return *this;
    }

    ServerMotd& operator=(const char* title) {
        return operator=(std::string(title ? title : ""));
    }

    ServerMotd& operator=(Base values) {
        Base::operator=(std::move(values));
        return *this;
    }

    ServerMotd& operator=(std::initializer_list<Value> values) {
        Base::operator=(values);
        return *this;
    }
};

// JavaScript callbacks normally return a fresh ServerAdvertisement value.
// Historical C++ releases required ServerAdvertisement&, so retain that exact
// reference when such a callable is supplied while caching value returns long
// enough for the existing getAdvertisement() reference API.
class ServerAdvertisementCallback {
public:
    ServerAdvertisementCallback() = default;
    ServerAdvertisementCallback(std::nullptr_t) noexcept {}

    template <typename Callable>
        requires (
            !std::is_same_v<
                std::remove_cvref_t<Callable>,
                ServerAdvertisementCallback
            > &&
            std::is_invocable_r_v<
                ServerAdvertisement,
                std::remove_reference_t<Callable>&
            >
        )
    ServerAdvertisementCallback(Callable&& callback) {
        assign(std::forward<Callable>(callback));
    }

    template <typename Callable>
        requires (
            !std::is_same_v<
                std::remove_cvref_t<Callable>,
                ServerAdvertisementCallback
            > &&
            std::is_invocable_r_v<
                ServerAdvertisement,
                std::remove_reference_t<Callable>&
            >
        )
    ServerAdvertisementCallback& operator=(Callable&& callback) {
        assign(std::forward<Callable>(callback));
        return *this;
    }

    ServerAdvertisementCallback& operator=(std::nullptr_t) noexcept {
        referenceHandler_ = {};
        valueHandler_ = {};
        cachedValue_.reset();
        return *this;
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(referenceHandler_) ||
            static_cast<bool>(valueHandler_);
    }

    ServerAdvertisement& operator()() const {
        if (referenceHandler_) {
            return referenceHandler_();
        }
        if (!valueHandler_) {
            throw std::bad_function_call();
        }
        cachedValue_.emplace(valueHandler_());
        return *cachedValue_;
    }

private:
    template <typename Callable>
    void assign(Callable&& callback) {
        using Result = std::invoke_result_t<
            std::remove_reference_t<Callable>&
        >;
        referenceHandler_ = {};
        valueHandler_ = {};
        cachedValue_.reset();
        if constexpr (std::is_same_v<Result, ServerAdvertisement&>) {
            referenceHandler_ = std::forward<Callable>(callback);
        } else {
            valueHandler_ = std::forward<Callable>(callback);
        }
    }

    std::function<ServerAdvertisement&()> referenceHandler_;
    std::function<ServerAdvertisement()> valueHandler_;
    mutable std::optional<ServerAdvertisement> cachedValue_;
};

// Server#listen resolves to this configured address in server.js. When port
// is zero, boundPort() remains the way to read the OS-selected UDP port.
struct ServerListenResult {
    std::string host;
    uint16_t port = 0;
};

using BedrockServerListenResult = ServerListenResult;

struct BedrockServerOptions {
    std::string host = "0.0.0.0";
    uint16_t port = 19132;
    std::string version = "1.26.0";
    ServerMotd motd;
    int maxPlayers = 3;
    ServerAdvertisementCallback advertisementFn;
    bool offline = false;
    std::string raknetBackend = "raknet-native";
    bool autoLogin = true;
    // The JavaScript server completes login at client_to_server_handshake and
    // does not run a resource-pack exchange.  Keep the previous empty-pack
    // flow only as an explicit C++ extension.
    bool autoResourcePacks = false;
    uint16_t compressionThreshold = 512;
    std::string compressionAlgorithm = "deflate";
    int compressionLevel = 7;
    // connection.js starts one outbound queue timer per Player and uses
    // `options.batchingInterval || 20` as its period.
    int batchingInterval = 20;
};

// C++ lifecycle switches are intentionally separate from the ordinary
// JavaScript-shaped server settings.
struct ServerAdvancedOptions {
    bool autoLogin = true;
    // The JavaScript server does not run an empty resource-pack exchange.
    bool autoResourcePacks = false;
};

// Public createServer({...}) surface. BedrockServerOptions remains the native
// runtime type for direct construction and low-level relay composition.
struct ServerOptions {
    std::string host = "0.0.0.0";
    uint16_t port = 19132;
    std::string version = std::string(CURRENT_VERSION);
    bool offline = false;

    ServerMotd motd;
    int maxPlayers = 3;
    ServerAdvertisementCallback advertisementFn;

    std::string raknetBackend = "raknet-native";
    std::string compressionAlgorithm = "deflate";
    int compressionLevel = 7;
    uint16_t compressionThreshold = 512;
    int batchingInterval = 20;

    ServerAdvancedOptions advanced;
};

class BedrockServer;
BedrockServer createNativeServer(BedrockServerOptions options);
BedrockServer createServer(ServerOptions options);

enum class BedrockServerClientStatus;
struct BedrockServerPacketEvent;
struct BedrockServerLoggingInEvent;
struct BedrockServerClientHandshakeEvent;

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
    using VoidHandler = std::function<void()>;
    using PacketHandler = std::function<void(const BedrockServerPacketEvent&)>;
    using LoggingInHandler =
        std::function<void(const BedrockServerLoggingInEvent&)>;
    using ClientHandshakeHandler =
        std::function<void(const BedrockServerClientHandshakeEvent&)>;
    using StatusHandler = std::function<void(BedrockServerClientStatus)>;

    void onError(ErrorHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        errorHandlers_.push_back(std::move(handler));
    }

    void onClose(CloseHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        closeHandlers_.push_back(std::move(handler));
    }

    void onLoggingIn(LoggingInHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        loggingInHandlers_.push_back(std::move(handler));
    }

    void onClientHandshake(ClientHandshakeHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        clientHandshakeHandlers_.push_back(std::move(handler));
    }

    void onLogin(PacketHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        loginHandlers_.push_back(std::move(handler));
    }

    void onJoin(VoidHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        joinHandlers_.push_back(std::move(handler));
    }

    void onSpawn(VoidHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        spawnHandlers_.push_back(std::move(handler));
    }

    void onStatus(StatusHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        statusHandlers_.push_back(std::move(handler));
    }

    void onAny(PacketHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        anyPacketHandlers_.push_back(std::move(handler));
    }

    void onPacket(const std::string& packetName, PacketHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        packetHandlers_[packetName].push_back(std::move(handler));
    }

private:
    friend class BedrockServer;
    friend struct BedrockServerTestAccess;

    mutable std::mutex mutex_;
    std::vector<ErrorHandler> errorHandlers_;
    std::vector<CloseHandler> closeHandlers_;
    std::vector<LoggingInHandler> loggingInHandlers_;
    std::vector<ClientHandshakeHandler> clientHandshakeHandlers_;
    std::vector<PacketHandler> loginHandlers_;
    std::vector<VoidHandler> joinHandlers_;
    std::vector<VoidHandler> spawnHandlers_;
    std::vector<StatusHandler> statusHandlers_;
    std::vector<PacketHandler> anyPacketHandlers_;
    std::unordered_map<std::string, std::vector<PacketHandler>> packetHandlers_;

    std::vector<ErrorHandler> errorSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return errorHandlers_;
    }

    std::vector<CloseHandler> closeSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closeHandlers_;
    }

    std::vector<LoggingInHandler> loggingInSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return loggingInHandlers_;
    }

    std::vector<ClientHandshakeHandler> clientHandshakeSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return clientHandshakeHandlers_;
    }

    std::vector<PacketHandler> loginSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return loginHandlers_;
    }

    std::vector<VoidHandler> joinSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return joinHandlers_;
    }

    std::vector<VoidHandler> spawnSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return spawnHandlers_;
    }

    std::vector<StatusHandler> statusSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return statusHandlers_;
    }

    std::vector<PacketHandler> anyPacketSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return anyPacketHandlers_;
    }

    std::vector<PacketHandler> packetSnapshot(const std::string& packetName) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = packetHandlers_.find(packetName);
        return found == packetHandlers_.end()
            ? std::vector<PacketHandler>{}
            : found->second;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        errorHandlers_.clear();
        closeHandlers_.clear();
        loggingInHandlers_.clear();
        clientHandshakeHandlers_.clear();
        loginHandlers_.clear();
        joinHandlers_.clear();
        spawnHandlers_.clear();
        statusHandlers_.clear();
        anyPacketHandlers_.clear();
        packetHandlers_.clear();
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
    using VoidHandler = BedrockServerPlayerEventState::VoidHandler;
    using PacketHandler = BedrockServerPlayerEventState::PacketHandler;
    using LoggingInHandler = BedrockServerPlayerEventState::LoggingInHandler;
    using ClientHandshakeHandler =
        BedrockServerPlayerEventState::ClientHandshakeHandler;
    using StatusHandler = BedrockServerPlayerEventState::StatusHandler;

    std::string address;
    uint16_t port = 0;
    uint64_t clientGuid = 0;
    int mtu = 1400;
    RakNetServerPeer peer;
    std::shared_ptr<BedrockServerPlayerEventState> playerEvents;
    // The Node Player retains its owning Server. This non-owning pointer is
    // stable for every live connection because BedrockServer itself is not
    // movable while its RakNet callbacks are active.
    BedrockServer* server = nullptr;

    // Same endpoint key used by Server::clients(), equivalent to conn.address
    // in the JavaScript RakNet backend.
    std::string key() const {
        return address + ":" + std::to_string(port);
    }

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

    void onLoggingIn(LoggingInHandler handler) const {
        requirePlayerEvents().onLoggingIn(std::move(handler));
    }

    void onServerClientHandshake(ClientHandshakeHandler handler) const {
        requirePlayerEvents().onClientHandshake(std::move(handler));
    }

    void onLogin(PacketHandler handler) const {
        requirePlayerEvents().onLogin(std::move(handler));
    }

    void onLogin(VoidHandler handler) const {
        onLogin([handler = std::move(handler)](
            const BedrockServerPacketEvent&
        ) {
            handler();
        });
    }

    void onJoin(VoidHandler handler) const {
        requirePlayerEvents().onJoin(std::move(handler));
    }

    void onSpawn(VoidHandler handler) const {
        requirePlayerEvents().onSpawn(std::move(handler));
    }

    void onSpawn(ErrorHandler handler) const {
        onSpawn([handler = std::move(handler)]() {
            // The current d.ts declares a reason, while serverPlayer.js emits
            // spawn without arguments.
            handler("");
        });
    }

    void onClose(ErrorHandler handler) const {
        onClose([handler = std::move(handler)]() {
            handler("");
        });
    }

    void onStatus(StatusHandler handler) const {
        requirePlayerEvents().onStatus(std::move(handler));
    }

    void onAny(PacketHandler handler) const {
        requirePlayerEvents().onAny(std::move(handler));
    }

    void on(const std::string& eventName, VoidHandler handler) const {
        if (eventName == "login") {
            onLogin([handler = std::move(handler)](
                const BedrockServerPacketEvent&
            ) {
                handler();
            });
            return;
        }
        if (eventName == "join") {
            onJoin(std::move(handler));
            return;
        }
        if (eventName == "spawn") {
            onSpawn(std::move(handler));
            return;
        }
        if (eventName == "close") {
            onClose(std::move(handler));
            return;
        }
        throw std::runtime_error("unknown player lifecycle event: " + eventName);
    }

    void on(const std::string& eventName, PacketHandler handler) const {
        if (eventName == "packet") {
            onAny(std::move(handler));
            return;
        }
        if (eventName == "login") {
            onLogin(std::move(handler));
            return;
        }
        requirePlayerEvents().onPacket(eventName, std::move(handler));
    }

    void on(const std::string& eventName, LoggingInHandler handler) const {
        if (eventName != "loggingIn") {
            throw std::runtime_error("unknown player logging event: " + eventName);
        }
        onLoggingIn(std::move(handler));
    }

    void on(const std::string& eventName, ClientHandshakeHandler handler) const {
        if (eventName != "server.client_handshake") {
            throw std::runtime_error("unknown player handshake event: " + eventName);
        }
        onServerClientHandshake(std::move(handler));
    }

    void on(const std::string& eventName, StatusHandler handler) const {
        if (eventName != "status") {
            throw std::runtime_error("unknown player status event: " + eventName);
        }
        onStatus(std::move(handler));
    }

    void on(const std::string& eventName, ErrorHandler handler) const {
        if (eventName == "error") {
            onError(std::move(handler));
            return;
        }
        if (eventName == "close") {
            onClose([handler = std::move(handler)]() {
                // serverPlayer.js emits close without an argument even though
                // the current index.d.ts declares a reason string.
                handler("");
            });
            return;
        }
        throw std::runtime_error("unknown player error event: " + eventName);
    }

    BedrockServerClientStatus status() const;
    void setStatus(BedrockServerClientStatus status) const;
    void updateItemPalette(const ProtoDefValue& palette) const;
    std::optional<ProtoDefValue> getUserData() const;
    std::optional<ProtoDefValue> skinData() const;
    std::optional<BedrockLoginProfile> profile() const;
    std::optional<uint32_t> version() const;

    bool versionLessThan(const std::string& version) const;
    bool versionLessThan(uint32_t protocolVersion) const;
    bool versionGreaterThan(const std::string& version) const;
    bool versionGreaterThan(uint32_t protocolVersion) const;
    bool versionGreaterThanOrEqualTo(const std::string& version) const;
    bool versionGreaterThanOrEqualTo(uint32_t protocolVersion) const;
    bool versionLessThanOrEqualTo(const std::string& version) const;
    bool versionLessThanOrEqualTo(uint32_t protocolVersion) const;

    void write(
        const std::string& packetName,
        const ProtoDefValue& value,
        VersionedMcpeCompression compression = VersionedMcpeCompression::Automatic
    ) const;
    void queue(
        const std::string& packetName,
        const ProtoDefValue& value,
        VersionedMcpeCompression compression = VersionedMcpeCompression::Automatic
    ) const;
    void sendBuffer(
        const std::vector<uint8_t>& buffer,
        bool immediate = false,
        VersionedMcpeCompression compression = VersionedMcpeCompression::Automatic
    ) const;
    void sendQueued() const;
    void disconnect(
        const std::string& reason = "Server closed",
        bool hide = false
    ) const;
    void sendDisconnectStatus(const std::string& playStatus) const;
    void close() const;

    BedrockServer& owner() const;

private:
    BedrockServerPlayerEventState& requirePlayerEvents() const {
        if (!playerEvents) {
            throw std::logic_error("player event state is not initialized");
        }
        return *playerEvents;
    }
};

// JavaScript-compatible public spelling. Copies are shared views of the same
// live server-side Player and retain the same listener state.
using Player = BedrockServerConnection;

struct BedrockServerPacketEvent {
    BedrockServerConnection connection;
    VersionedGamePacket packet;
};

struct BedrockServerLoggingInEvent {
    BedrockServerConnection connection;
    VersionedGamePacket packet;
    LoginPacketData login;
};

struct BedrockServerClientHandshakeEvent {
    BedrockServerConnection connection;
    std::string key;
};

class BedrockServer {
public:
    using ClientMap = std::unordered_map<std::string, Player>;
    using ConnectionHandler = std::function<void(const BedrockServerConnection&)>;
    using PacketHandler = std::function<void(const BedrockServerPacketEvent&)>;
    using LoggingInHandler =
        std::function<void(const BedrockServerLoggingInEvent&)>;
    using ClientHandshakeHandler =
        std::function<void(const BedrockServerClientHandshakeEvent&)>;
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
        stopOutboundQueueScheduler();
        raknet_.close();
        stopPlayerCloseScheduler();
    }

    void onConnect(ConnectionHandler handler) {
        connectHandlers_.push_back(std::move(handler));
    }

    // Server.on('connect', player => ...) is the only Server event declared
    // by bedrock-protocol's public TypeScript surface. Typed aliases remain
    // available for the additional C++ lifecycle hooks below.
    void on(const std::string& eventName, ConnectionHandler handler) {
        if (eventName != "connect") {
            throw std::runtime_error("unknown server connection event: " + eventName);
        }
        onConnect(std::move(handler));
    }

    void onJoin(ConnectionHandler handler) {
        joinHandlers_.push_back(std::move(handler));
    }

    // serverPlayer.js emits this after the login packet has decoded but before
    // protocol-version rejection or JWT verification.
    void onLoggingIn(LoggingInHandler handler) {
        loggingInHandlers_.push_back(std::move(handler));
    }

    // Explicit form of serverPlayer.js's internal server.client_handshake
    // event. Encryption is active, while profile/login publication is still
    // pending when these handlers run.
    void onServerClientHandshake(ClientHandshakeHandler handler) {
        clientHandshakeHandlers_.push_back(std::move(handler));
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

    ServerListenResult listen() {
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
            connection.server = this;

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

            ensureOutboundQueueScheduler();

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
        return {
            .host = options_.host,
            .port = options_.port
        };
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

    // Node exposes Server.clients directly. Return a synchronized snapshot so
    // callers can enumerate it safely while RakNet accepts or removes peers.
    ClientMap clients() const {
        std::lock_guard<std::mutex> lock(serverStateMutex_);
        return connections_;
    }

    std::optional<Player> client(const std::string& key) const {
        std::lock_guard<std::mutex> lock(serverStateMutex_);
        const auto found = connections_.find(key);
        return found == connections_.end()
            ? std::nullopt
            : std::optional<Player>(found->second);
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

    void setStatus(
        const BedrockServerConnection& connection,
        BedrockServerClientStatus status
    ) {
        transitionStatus(connection, status);
    }

    void updateItemPalette(
        const BedrockServerConnection& connection,
        const ProtoDefValue& palette
    ) {
        const auto session = sessionSnapshot(connection);
        if (!session) {
            throw std::runtime_error("player session not found");
        }
        detail::updateItemPaletteVariables(
            palette,
            session->protoDefVariables
        );
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

    // Mirrors Player#handleClientProtocolVersion. Server.validateOptions()
    // always resolves options.protocolVersion in JavaScript, so the active
    // branch accepts equal/older clients and rejects only a newer protocol
    // with failed_spawn.
    bool handleClientProtocolVersion(
        const BedrockServerConnection& connection,
        int32_t clientVersion
    ) {
        if (static_cast<int64_t>(configuredServerProtocolVersion()) <
            static_cast<int64_t>(clientVersion)) {
            sendDisconnectStatus(connection, "failed_spawn");
            return false;
        }
        return true;
    }

    void send(
        const BedrockServerConnection& connection,
        const std::string& packetName,
        const ProtoDefValue& value,
        VersionedMcpeCompression compression = VersionedMcpeCompression::Automatic
    ) {
        const auto session = sessionSnapshot(connection);
        if (!session) return;
        std::lock_guard<std::recursive_mutex> outboundLock(session->outboundMutex);
        ProtoDefPacketEncoder encoder(
            options_.version,
            playerProtocolTypes_,
            session->protoDefVariables
        );
        auto payload = encoder.encodePacket(packetName, value);
        sendPacket(
            connection,
            mcpeCodec_.packetCodec().makePacketByName(packetName, payload),
            compression
        );
    }

    // Connection#write serializes and sends one packet immediately.
    void write(
        const BedrockServerConnection& connection,
        const std::string& packetName,
        const ProtoDefValue& value,
        VersionedMcpeCompression compression = VersionedMcpeCompression::Automatic
    ) {
        send(connection, packetName, value, compression);
    }

    // Connection#queue serializes immediately, then lets the Player queue
    // timer combine all pending full packets into one encrypted/compressed
    // batch. level_chunk follows connection.js's sendBuffer(false) path and
    // therefore remains queued as well.
    void queue(
        const BedrockServerConnection& connection,
        const std::string& packetName,
        const ProtoDefValue& value,
        VersionedMcpeCompression compression = VersionedMcpeCompression::Automatic
    ) {
        const auto session = sessionSnapshot(connection);
        if (!session) return;
        std::lock_guard<std::recursive_mutex> outboundLock(session->outboundMutex);
        ProtoDefPacketEncoder encoder(
            options_.version,
            playerProtocolTypes_,
            session->protoDefVariables
        );
        auto payload = encoder.encodePacket(packetName, value);
        queuePacket(
            connection,
            mcpeCodec_.packetCodec().makePacketByName(packetName, payload),
            compression
        );
    }

    void queuePacket(
        const BedrockServerConnection& connection,
        const VersionedGamePacket& packet,
        VersionedMcpeCompression compression = VersionedMcpeCompression::Automatic
    ) {
        queuePackets(connection, {packet}, compression);
    }

    void queuePackets(
        const BedrockServerConnection& connection,
        const std::vector<VersionedGamePacket>& packets,
        VersionedMcpeCompression compression = VersionedMcpeCompression::Automatic
    ) {
        if (packets.empty()) return;
        const auto session = sessionSnapshot(connection);
        if (!session) return;
        std::lock_guard<std::recursive_mutex> outboundLock(session->outboundMutex);

        for (const auto& packet : packets) {
            processOutboundPacket(session, packet);
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->status == BedrockServerClientStatus::Disconnected) {
                return;
            }
            session->queuedPackets.reserve(
                session->queuedPackets.size() + packets.size()
            );
            for (const auto& packet : packets) {
                session->queuedPackets.push_back({packet, compression});
            }
        }
        ensureOutboundQueueScheduler();
    }

    // `buffer` is the serializer's complete packet buffer: packet id varuint
    // followed by its payload, without the outer batch length prefix.
    void sendBuffer(
        const BedrockServerConnection& connection,
        const std::vector<uint8_t>& buffer,
        bool immediate = false,
        VersionedMcpeCompression compression = VersionedMcpeCompression::Automatic
    ) {
        auto packet = mcpeCodec_.packetCodec().decodeFullPacket(buffer);
        if (immediate) {
            sendPacket(connection, packet, compression);
        } else {
            queuePacket(connection, packet, compression);
        }
    }

    // Exposes Connection#_tick as a deterministic C++ flush boundary while
    // the normal live path invokes it from the batching timer.
    void sendQueued(const BedrockServerConnection& connection) {
        const auto session = sessionSnapshot(connection);
        if (!session) return;
        std::lock_guard<std::recursive_mutex> outboundLock(session->outboundMutex);

        std::vector<QueuedPacket> queued;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->status == BedrockServerClientStatus::Disconnected ||
                session->queuedPackets.empty()) {
                return;
            }
            queued = std::move(session->queuedPackets);
            session->queuedPackets.clear();
        }

        std::size_t begin = 0;
        while (begin < queued.size()) {
            const auto compression = queued[begin].compression;
            std::size_t end = begin + 1;
            while (end < queued.size() &&
                   queued[end].compression == compression) {
                ++end;
            }

            std::vector<VersionedGamePacket> packets;
            packets.reserve(end - begin);
            for (std::size_t index = begin; index < end; ++index) {
                packets.push_back(std::move(queued[index].packet));
            }
            sendPackets(connection, packets, compression);
            begin = end;
        }
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
        std::lock_guard<std::recursive_mutex> outboundLock(session->outboundMutex);

        for (const auto& packet : packets) {
            processOutboundPacket(session, packet);
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

    // Native transport drop used by Relay#forceSingle. Unlike disconnect(),
    // this does not manufacture an MCPE disconnect packet for a connection
    // that the relay rejected before constructing its public Player session.
    void closeConnection(const BedrockServerConnection& connection) {
        if (!hasActivePlayer(connection)) return;
        closePlayer(connection);
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

    friend BedrockServer createNativeServer(BedrockServerOptions options);
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
    std::vector<LoggingInHandler> loggingInHandlers_;
    std::vector<ClientHandshakeHandler> clientHandshakeHandlers_;
    std::vector<PacketHandler> loginHandlers_;
    std::vector<StatusHandler> statusHandlers_;
    std::vector<PacketHandler> anyPacketHandlers_;
    std::unordered_map<std::string, std::vector<PacketHandler>> packetHandlers_;
    mutable std::mutex serverStateMutex_;
    std::unordered_map<std::string, BedrockServerConnection> connections_;
    std::atomic<int> clientCount_ {0};
    std::atomic<bool> closing_ {false};

    mutable std::mutex outboundQueueMutex_;
    std::condition_variable outboundQueueCv_;
    std::thread outboundQueueThread_;
    bool outboundQueueStopping_ = false;

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
        if (options.raknetBackend != "raknet-native") {
            throw std::runtime_error(
                "unsupported RakNet backend in this C++ build: " +
                options.raknetBackend
            );
        }
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

    struct QueuedPacket {
        VersionedGamePacket packet;
        VersionedMcpeCompression compression = VersionedMcpeCompression::Automatic;
    };

    struct SessionState {
        mutable std::mutex mutex;
        // Node runs queue admission, _tick, and write on one event loop.
        // Serialize those boundaries per Player while allowing sendQueued()
        // to call the ordinary sendPackets() path recursively.
        mutable std::recursive_mutex outboundMutex;
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
        ProtoDefVariableStorePtr protoDefVariables = makeProtoDefVariableStore();
        std::vector<QueuedPacket> queuedPackets;
    };

    std::chrono::milliseconds outboundQueueInterval() const {
        // JavaScript's `value || 20` keeps zero on the default and Node's
        // timer clamps a negative delay to its one-millisecond minimum.
        if (options_.batchingInterval == 0) {
            return std::chrono::milliseconds(20);
        }
        return std::chrono::milliseconds(std::max(options_.batchingInterval, 1));
    }

    void processOutboundPacket(
        const std::shared_ptr<SessionState>& session,
        const VersionedGamePacket& packet
    ) {
        if (packet.name != "start_game" && packet.name != "item_registry") {
            return;
        }
        ProtoDefPacketDecoder decoder(
            options_.version,
            playerProtocolTypes_,
            session->protoDefVariables
        );
        (void) decoder.decodePacket(packet.name, packet.payload);
    }

    void ensureOutboundQueueScheduler() {
        std::lock_guard<std::mutex> lock(outboundQueueMutex_);
        if (outboundQueueStopping_ || closing_.load() ||
            outboundQueueThread_.joinable()) {
            return;
        }
        outboundQueueThread_ = std::thread([this]() {
            runOutboundQueueScheduler();
        });
    }

    void runOutboundQueueScheduler() {
        const auto interval = outboundQueueInterval();
        std::unique_lock<std::mutex> lock(outboundQueueMutex_);
        while (!outboundQueueStopping_) {
            if (outboundQueueCv_.wait_for(lock, interval, [this]() {
                    return outboundQueueStopping_;
                })) {
                break;
            }

            lock.unlock();
            flushOutboundQueues();
            lock.lock();
        }
    }

    void flushOutboundQueues() {
        std::vector<BedrockServerConnection> players;
        {
            std::lock_guard<std::mutex> lock(serverStateMutex_);
            players.reserve(connections_.size());
            for (const auto& [key, connection] : connections_) {
                (void) key;
                players.push_back(connection);
            }
        }

        for (const auto& player : players) {
            try {
                sendQueued(player);
            } catch (...) {
                // Never let one malformed outbound batch terminate the
                // process from a std::thread boundary. Serialization errors
                // still surface synchronously from queue()/sendBuffer().
            }
        }
    }

    void stopOutboundQueueScheduler() {
        {
            std::lock_guard<std::mutex> lock(outboundQueueMutex_);
            outboundQueueStopping_ = true;
        }
        outboundQueueCv_.notify_all();
        if (outboundQueueThread_.joinable() &&
            outboundQueueThread_.get_id() != std::this_thread::get_id()) {
            outboundQueueThread_.join();
        }
    }

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
                return SnappyCodec::decompress(body);
            }
            throw std::runtime_error(
                "Unknown compression type " + std::to_string(compressor)
            );
        }

        // Framer.decode treats failed session-wide decompression as an
        // uncompressed batch on legacy versions and before network_settings.
        // A raw pre-negotiation batch can itself be a structurally valid
        // Snappy block, so there is no reliable probe like zlib's checksumless
        // stream parser. request_network_settings is always sent raw and makes
        // the session compressor authoritative only after the response.
        if (!compressionReady && options_.compressionAlgorithm == "snappy") {
            return compressionPacket;
        }
        if (options_.compressionAlgorithm == "deflate") {
            try {
                return inflatePlayerRaw(compressionPacket);
            } catch (const std::exception&) {
                return compressionPacket;
            }
        }
        if (options_.compressionAlgorithm == "snappy") {
            try {
                return SnappyCodec::decompress(compressionPacket);
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

    void validatePlayerPacket(
        const BedrockServerConnection& connection,
        const VersionedGamePacket& packet
    ) const {
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
        const auto session = sessionSnapshot(connection);
        if (!session) {
            throw std::runtime_error("player session not found");
        }
        decoder.setVariables(session->protoDefVariables->snapshot());
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
                validatePlayerPacket(connection, packet);
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

            const auto playerNamedHandlers = connection.playerEvents
                ? connection.playerEvents->packetSnapshot(packet.name)
                : std::vector<BedrockServerConnection::PacketHandler>{};
            for (const auto& handler : playerNamedHandlers) {
                handler(event);
            }

            const auto playerAnyHandlers = connection.playerEvents
                ? connection.playerEvents->anyPacketSnapshot()
                : std::vector<BedrockServerConnection::PacketHandler>{};
            for (const auto& handler : playerAnyHandlers) {
                handler(event);
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
            const auto clientVersion = requestNetworkSettingsProtocol(packet);
            if (handleClientProtocolVersion(connection, clientVersion) &&
                mcpeCodec_.definition().hasPacket("network_settings")) {
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

            transitionStatus(connection, BedrockServerClientStatus::Initializing);

            if (options_.autoResourcePacks &&
                mcpeCodec_.definition().hasPacket("resource_packs_info")) {
                sendEmptyResourcePacksInfo(connection);
            } else {
                emitJoin(connection);
            }
            return false;
        }

        if (packet.name == "set_local_player_as_initialized") {
            transitionStatus(connection, BedrockServerClientStatus::Initialized);
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
        try {
            login = LoginPacketCodec::decode(packet.fullPacket);
        } catch (const std::exception&) {
            disconnect(connection, "Server authentication error");
            return;
        }

        BedrockServerLoggingInEvent loggingInEvent;
        loggingInEvent.connection = connection;
        loggingInEvent.packet = packet;
        loggingInEvent.login = login;
        const auto playerLoggingInHandlers = connection.playerEvents
            ? connection.playerEvents->loggingInSnapshot()
            : std::vector<BedrockServerConnection::LoggingInHandler>{};
        for (const auto& handler : playerLoggingInHandlers) {
            handler(loggingInEvent);
        }
        const auto loggingInHandlers = loggingInHandlers_;
        for (auto& handler : loggingInHandlers) {
            handler(loggingInEvent);
        }

        if (!handleClientProtocolVersion(
                connection,
                static_cast<int32_t>(login.protocolVersion)
            )) {
            return;
        }

        BedrockLoginVerificationResult verifiedLogin;
        try {
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
        }

        BedrockServerClientHandshakeEvent clientHandshakeEvent;
        clientHandshakeEvent.connection = connection;
        clientHandshakeEvent.key = verifiedLogin.key;
        const auto playerHandshakeHandlers = connection.playerEvents
            ? connection.playerEvents->clientHandshakeSnapshot()
            : std::vector<BedrockServerConnection::ClientHandshakeHandler>{};
        for (const auto& handler : playerHandshakeHandlers) {
            handler(clientHandshakeEvent);
        }
        const auto clientHandshakeHandlers = clientHandshakeHandlers_;
        for (auto& handler : clientHandshakeHandlers) {
            handler(clientHandshakeEvent);
        }

        if (!hasWritablePlayer(connection)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->loginVerification = std::move(verifiedLogin);
        }

        BedrockServerPacketEvent event;
        event.connection = connection;
        event.packet = packet;
        const auto playerLoginHandlers = connection.playerEvents
            ? connection.playerEvents->loginSnapshot()
            : std::vector<BedrockServerConnection::PacketHandler>{};
        for (const auto& handler : playerLoginHandlers) {
            handler(event);
        }
        for (auto& handler : loginHandlers_) {
            handler(event);
        }
    }

    static int32_t requestNetworkSettingsProtocol(
        const VersionedGamePacket& packet
    ) {
        if (packet.payload.size() < 4) {
            throw std::runtime_error(
                "request_network_settings client_protocol is truncated"
            );
        }
        const uint32_t raw =
            (static_cast<uint32_t>(packet.payload[0]) << 24u) |
            (static_cast<uint32_t>(packet.payload[1]) << 16u) |
            (static_cast<uint32_t>(packet.payload[2]) << 8u) |
            static_cast<uint32_t>(packet.payload[3]);
        return static_cast<int32_t>(raw);
    }

    void sendPreCompression(
        const BedrockServerConnection& connection,
        const std::string& packetName,
        const ProtoDefValue& value
    ) {
        if (!hasActivePlayer(connection)) {
            return;
        }
        const auto session = sessionSnapshot(connection);
        if (!session) return;
        ProtoDefPacketEncoder encoder(
            options_.version,
            playerProtocolTypes_,
            session->protoDefVariables
        );
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
        const auto playerHandlers = connection.playerEvents
            ? connection.playerEvents->joinSnapshot()
            : std::vector<BedrockServerConnection::VoidHandler>{};
        for (const auto& handler : playerHandlers) {
            handler();
        }
        for (auto& handler : joinHandlers_) {
            handler(connection);
        }
    }

    void transitionStatus(
        const BedrockServerConnection& connection,
        BedrockServerClientStatus nextStatus
    ) {
        // Connection.status in the JavaScript implementation emits before it
        // stores, so status(connection) intentionally exposes the old value to
        // status listeners.
        const auto playerHandlers = connection.playerEvents
            ? connection.playerEvents->statusSnapshot()
            : std::vector<BedrockServerConnection::StatusHandler>{};
        for (const auto& handler : playerHandlers) {
            handler(nextStatus);
        }
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
        const auto playerHandlers = connection.playerEvents
            ? connection.playerEvents->spawnSnapshot()
            : std::vector<BedrockServerConnection::VoidHandler>{};
        for (const auto& handler : playerHandlers) {
            handler();
        }
        for (auto& handler : spawnHandlers_) {
            handler(connection);
        }
    }
};

inline BedrockServer& BedrockServerConnection::owner() const {
    if (!server) {
        throw std::logic_error("player is not attached to a BedrockServer");
    }
    return *server;
}

inline BedrockServerClientStatus BedrockServerConnection::status() const {
    return owner().status(*this);
}

inline void BedrockServerConnection::setStatus(
    BedrockServerClientStatus status
) const {
    owner().setStatus(*this, status);
}

inline void BedrockServerConnection::updateItemPalette(
    const ProtoDefValue& palette
) const {
    owner().updateItemPalette(*this, palette);
}

inline std::optional<ProtoDefValue> BedrockServerConnection::getUserData() const {
    return owner().userData(*this);
}

inline std::optional<ProtoDefValue> BedrockServerConnection::skinData() const {
    return owner().skinData(*this);
}

inline std::optional<BedrockLoginProfile> BedrockServerConnection::profile() const {
    return owner().profile(*this);
}

inline std::optional<uint32_t> BedrockServerConnection::version() const {
    return owner().clientVersion(*this);
}

inline bool BedrockServerConnection::versionLessThan(
    const std::string& version
) const {
    return owner().versionLessThan(*this, version);
}

inline bool BedrockServerConnection::versionLessThan(
    uint32_t protocolVersion
) const {
    return owner().versionLessThan(*this, protocolVersion);
}

inline bool BedrockServerConnection::versionGreaterThan(
    const std::string& version
) const {
    return owner().versionGreaterThan(*this, version);
}

inline bool BedrockServerConnection::versionGreaterThan(
    uint32_t protocolVersion
) const {
    return owner().versionGreaterThan(*this, protocolVersion);
}

inline bool BedrockServerConnection::versionGreaterThanOrEqualTo(
    const std::string& version
) const {
    return owner().versionGreaterThanOrEqualTo(*this, version);
}

inline bool BedrockServerConnection::versionGreaterThanOrEqualTo(
    uint32_t protocolVersion
) const {
    return owner().versionGreaterThanOrEqualTo(*this, protocolVersion);
}

inline bool BedrockServerConnection::versionLessThanOrEqualTo(
    const std::string& version
) const {
    return owner().versionLessThanOrEqualTo(*this, version);
}

inline bool BedrockServerConnection::versionLessThanOrEqualTo(
    uint32_t protocolVersion
) const {
    return owner().versionLessThanOrEqualTo(*this, protocolVersion);
}

inline void BedrockServerConnection::write(
    const std::string& packetName,
    const ProtoDefValue& value,
    VersionedMcpeCompression compression
) const {
    owner().write(*this, packetName, value, compression);
}

inline void BedrockServerConnection::queue(
    const std::string& packetName,
    const ProtoDefValue& value,
    VersionedMcpeCompression compression
) const {
    owner().queue(*this, packetName, value, compression);
}

inline void BedrockServerConnection::sendBuffer(
    const std::vector<uint8_t>& buffer,
    bool immediate,
    VersionedMcpeCompression compression
) const {
    owner().sendBuffer(*this, buffer, immediate, compression);
}

inline void BedrockServerConnection::sendQueued() const {
    owner().sendQueued(*this);
}

inline void BedrockServerConnection::disconnect(
    const std::string& reason,
    bool hide
) const {
    owner().disconnect(*this, reason, hide);
}

inline void BedrockServerConnection::sendDisconnectStatus(
    const std::string& playStatus
) const {
    owner().sendDisconnectStatus(*this, playStatus);
}

inline void BedrockServerConnection::close() const {
    owner().closeConnection(*this);
}

namespace detail {

inline BedrockServerOptions expandServerOptions(ServerOptions options) {
    BedrockServerOptions out;
    out.host = std::move(options.host);
    out.port = options.port;
    out.version = std::move(options.version);
    out.motd = std::move(options.motd);
    out.maxPlayers = options.maxPlayers;
    out.advertisementFn = std::move(options.advertisementFn);
    out.offline = options.offline;
    out.raknetBackend = std::move(options.raknetBackend);
    out.autoLogin = options.advanced.autoLogin;
    out.autoResourcePacks = options.advanced.autoResourcePacks;
    out.compressionThreshold = options.compressionThreshold;
    out.compressionAlgorithm = std::move(options.compressionAlgorithm);
    out.compressionLevel = options.compressionLevel;
    out.batchingInterval = options.batchingInterval;
    return out;
}

} // namespace detail

inline BedrockServer createNativeServer(BedrockServerOptions options) {
    // createServer.js treats every falsy port as the default. A uint16_t has
    // only one falsy value, so zero must not request an ephemeral port here.
    if (options.port == 0) {
        options.port = 19132;
    }
    return BedrockServer(BedrockServer::CreateServerTag{}, std::move(options));
}

inline BedrockServer createServer(ServerOptions options) {
    return createNativeServer(detail::expandServerOptions(std::move(options)));
}

// A typed native options value remains source-compatible. Making this a
// constrained template prevents createServer({...}) from becoming ambiguous:
// direct brace calls always select the compact ServerOptions facade above.
template <typename NativeOptions>
requires std::is_same_v<
    std::remove_cvref_t<NativeOptions>,
    BedrockServerOptions
>
inline BedrockServer createServer(NativeOptions&& options) {
    return createNativeServer(
        BedrockServerOptions(std::forward<NativeOptions>(options))
    );
}

} // namespace bedrock
