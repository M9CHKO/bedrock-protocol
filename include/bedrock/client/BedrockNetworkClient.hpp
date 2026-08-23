#pragma once

#include <bedrock/BedrockEncryption.hpp>
#include <bedrock/BedrockKeyExchange.hpp>
#include <bedrock/LoginPacket.hpp>
#include <bedrock/Options.hpp>
#include <bedrock/auth/BedrockAuthJwt.hpp>
#include <bedrock/auth/XboxLiveAuth.hpp>
#include <bedrock/client/RakNetClient.hpp>
#include <bedrock/client/VersionedClientSession.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>
#include <bedrock/world/BedrockChunk.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bedrock {

class Client;

class BedrockNetworkClientUnhandledError final : public std::runtime_error {
public:
    explicit BedrockNetworkClientUnhandledError(const std::string& message)
        : std::runtime_error(message) {}
};

struct BedrockNetworkClientTestAccess;

enum class BedrockNetworkClientStatus {
    Disconnected,
    Connecting,
    Authenticating,
    Initializing,
    Initialized
};

// JavaScript accepts either a cache-directory string or the explicit boolean
// false for `profilesFolder`.  Keep omission distinct from both values so the
// auth boundary can apply auth.js's `if (!options.profilesFolder)` default at
// the same time as the JavaScript implementation.
class ProfilesFolderOption {
public:
    enum class Kind {
        Omitted,
        Path,
        Boolean
    };

    ProfilesFolderOption() = default;
    ProfilesFolderOption(const ProfilesFolderOption&) = default;
    ProfilesFolderOption(ProfilesFolderOption&&) noexcept = default;
    ProfilesFolderOption& operator=(const ProfilesFolderOption&) = default;
    ProfilesFolderOption& operator=(ProfilesFolderOption&&) noexcept = default;

    ProfilesFolderOption(const char* value) {
        *this = value;
    }

    ProfilesFolderOption(std::string value) {
        *this = std::move(value);
    }

    ProfilesFolderOption(std::filesystem::path value) {
        *this = std::move(value);
    }

    ProfilesFolderOption(bool value) {
        *this = value;
    }

    ProfilesFolderOption& operator=(const char* value) {
        kind_ = Kind::Path;
        path_ = value ? std::filesystem::path(value) : std::filesystem::path{};
        boolean_ = false;
        return *this;
    }

    ProfilesFolderOption& operator=(std::string value) {
        kind_ = Kind::Path;
        path_ = std::filesystem::path(std::move(value));
        boolean_ = false;
        return *this;
    }

    ProfilesFolderOption& operator=(std::filesystem::path value) {
        kind_ = Kind::Path;
        path_ = std::move(value);
        boolean_ = false;
        return *this;
    }

    ProfilesFolderOption& operator=(bool value) {
        kind_ = Kind::Boolean;
        path_.clear();
        boolean_ = value;
        return *this;
    }

    bool provided() const noexcept {
        return kind_ != Kind::Omitted;
    }

    bool truthy() const noexcept {
        if (kind_ == Kind::Path) return !path_.empty();
        return kind_ == Kind::Boolean && boolean_;
    }

    bool isBoolean() const noexcept {
        return kind_ == Kind::Boolean;
    }

    bool booleanValue() const noexcept {
        return boolean_;
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

    void setResolved(std::filesystem::path value) {
        kind_ = Kind::Path;
        path_ = std::move(value);
        boolean_ = false;
    }

private:
    Kind kind_ = Kind::Omitted;
    std::filesystem::path path_;
    bool boolean_ = false;
};

struct BedrockNetworkClientOptions {
    std::string host;
    uint16_t port = 19132;
    std::string username;
    std::string profile;
    std::string version = std::string(CURRENT_VERSION);
    uint32_t protocolVersion = 0;
    bool offline = false;
    bool interactiveAuth = true;
    std::optional<std::string> authTitle;
    std::string deviceType;
    std::string flow;
    // Deprecated compatibility alias. New code should use authTitle.
    std::string xboxClientId;
    std::filesystem::path authCacheRoot;
    std::function<void(const XboxDeviceCodeInfo&)> onMsaCode;

    int mtu = 1400;
    int connectTimeoutMs = 9000;
    int batchingIntervalMs = 20;
    int compressionLevel = 7;
    bool autoInitPlayer = true;
    bool autoResourcePackResponses = true;
    bool clientCacheEnabled = false;
    bool trackWorld = true;
    int32_t chunkRadius = 10;

    // Optional prebuilt Login packet, including the packet id. Normal bots do
    // not need this; online/offline login packets are generated in-process.
    std::vector<uint8_t> loginPacket;
    std::string clientDataJson;

    // Canonical bedrock-protocol option names.  Presence-aware timer fields
    // override their older C++ aliases above; an omitted canonical field keeps
    // the old alias semantics for source compatibility.  viewDistance is
    // retained but intentionally does not replace chunkRadius: upstream
    // createClient.js reads client.viewDistance, not options.viewDistance.
    std::optional<int> connectTimeout;
    std::optional<int> batchingInterval;
    std::optional<int32_t> viewDistance;
    ProfilesFolderOption profilesFolder;
    std::string raknetBackend = "raknet-native";
    bool useRaknetWorkers = true;
    std::optional<bool> useNativeRaknet;
    std::string compressionAlgorithm = "deflate";
    uint16_t compressionThreshold = 512;
};

struct BedrockNetworkClientPacketEvent {
    VersionedGamePacket packet;
};

class BedrockNetworkClient {
public:
    using PacketHandler = std::function<void(const BedrockNetworkClientPacketEvent&)>;
    using StatusHandler = std::function<void(BedrockNetworkClientStatus)>;
    using ErrorHandler = std::function<void(const std::string&)>;
    using HeartbeatHandler = std::function<void(int64_t)>;
    using CallbackLifetimeProvider = std::function<std::shared_ptr<void>()>;
    using AuthenticationOptionsResolvedHandler =
        std::function<void(BedrockNetworkClientOptions)>;
    using AuthenticationUnhandledRejectionHandler =
        std::function<void(std::string)>;

    // Low-level/manual ownership contract: this object must outlive every
    // active callback. Destroying a directly owned BedrockNetworkClient from
    // one of its own callbacks is unsupported C++ lifetime behavior; use the
    // factory `bedrock::Client`, whose shared State/transport leases make
    // callback-triggered facade destruction safe.
    // Concurrent close calls are serialized. A close callback must not spawn
    // and synchronously join another thread that calls close() on this same
    // low-level object (Node's EventEmitter has no equivalent cross-thread
    // execution); same-thread recursive close remains supported.
    explicit BedrockNetworkClient(BedrockNetworkClientOptions options = {});
    ~BedrockNetworkClient() noexcept;

    BedrockNetworkClient(const BedrockNetworkClient&) = delete;
    BedrockNetworkClient& operator=(const BedrockNetworkClient&) = delete;

    // Performs the synchronous part of Client.connect(): login/lifecycle
    // preparation and startQueue(). createClient uses this before completing
    // the connect_allowed listener snapshot, while the blocking RakNet
    // handshake remains on its tracked worker.
    bool prepareConnectLifecycle(bool deferQueuePump = false);
    bool connect();
    int run();
    void close(const std::string& reason = "closed");
    void disconnect(const std::string& reason = "Client leaving", bool hide = false);

    void on(const std::string& packetName, PacketHandler handler);
    void onAny(PacketHandler handler);
    void onJoin(std::function<void()> handler);
    void onSpawn(std::function<void()> handler);
    void onHeartbeat(HeartbeatHandler handler);
    // Canonical JavaScript `close` event has no arguments.
    void onClose(std::function<void()> handler);
    // C++ extension retaining the native transport reason.
    void onClose(ErrorHandler handler);
    void onError(ErrorHandler handler);
    void onStatus(StatusHandler handler);
    void setCallbackLifetimeProvider(CallbackLifetimeProvider provider);
    // The JS auth helper mutates client.options before it constructs
    // Authflow and before it emits a constructor error.  Factory facades use
    // this owned snapshot to publish those same mutations without racing a
    // concurrent retry after the preparation phase becomes observable.
    void setAuthenticationOptionsResolvedHandler(
        AuthenticationOptionsResolvedHandler handler
    );
    // A missing `error` listener or a throwing listener rejects the ignored
    // async authenticate() Promise in JS; it does not abort startQueue().
    // This boundary is delivered only after native queue admission.
    void setAuthenticationUnhandledRejectionHandler(
        AuthenticationUnhandledRejectionHandler handler
    );
    bool onRakNetCallbackThread() const;
    bool onCloseEmissionThread() const;
    void requestRakNetStop() noexcept;

    void sendPacket(const VersionedGamePacket& packet);
    void sendBuffer(const std::vector<uint8_t>& buffer, bool immediate = false);
    void send(const std::string& packetName, const ProtoDefValue& value);
    void write(const std::string& packetName, const ProtoDefValue& value);
    void queue(const std::string& packetName, const ProtoDefValue& value);
    void sendQueued();

    BedrockNetworkClientStatus status() const;
    std::optional<uint64_t> entityId() const;
    uint32_t protocolVersion() const;
    bool versionLessThan(const std::string& version) const;
    bool versionLessThan(uint32_t protocolVersion) const noexcept;
    bool versionGreaterThan(const std::string& version) const;
    bool versionGreaterThan(uint32_t protocolVersion) const noexcept;
    bool versionGreaterThanOrEqualTo(const std::string& version) const;
    bool versionGreaterThanOrEqualTo(uint32_t protocolVersion) const noexcept;
    bool versionLessThanOrEqualTo(const std::string& version) const;
    bool versionLessThanOrEqualTo(uint32_t protocolVersion) const noexcept;
    BedrockNetworkClientOptions options() const;
    const VersionedClientSession& session() const;
    VersionedClientSession& session();
    const BedrockWorld& world() const;
    BedrockWorld& world();
    const BedrockBlobStore& blobStore() const;
    BedrockBlobStore& blobStore();

private:
    friend class Client;
    friend struct BedrockNetworkClientTestAccess;

    class RakNetCallbackScope {
    public:
        explicit RakNetCallbackScope(BedrockNetworkClient& owner);
        ~RakNetCallbackScope();

        RakNetCallbackScope(const RakNetCallbackScope&) = delete;
        RakNetCallbackScope& operator=(const RakNetCallbackScope&) = delete;

    private:
        BedrockNetworkClient& owner_;
    };

    BedrockNetworkClientOptions options_;
    VersionedClientSession session_;
    ProtoDefPacketEncoder packetEncoder_;
    std::shared_ptr<RakNetClient> raknet_;
    BedrockWorld world_;
    BedrockBlobStore blobStore_;
    std::unordered_map<uint64_t, BlobType> pendingBlobTypes_;
    std::vector<BedrockLevelChunkPacket> pendingCachedLevelChunks_;

    mutable std::mutex mutex_;
    std::condition_variable closedCv_;
    std::atomic<bool> closed_ {true};
    std::atomic<bool> closing_ {false};
    std::atomic<bool> rakNetStopRequested_ {false};
    BedrockNetworkClientStatus status_ = BedrockNetworkClientStatus::Disconnected;

    enum class ConnectLifecyclePhase {
        Idle,
        Preparing,
        Prepared,
        Connecting,
        Active
    };
    std::mutex connectLifecycleMutex_;
    std::condition_variable connectLifecycleCv_;
    ConnectLifecyclePhase connectLifecyclePhase_ = ConnectLifecyclePhase::Idle;
    std::thread::id connectLifecycleOwnerThreadId_;

    mutable std::mutex closingMutex_;
    std::condition_variable closingCv_;
    std::thread::id closingThreadId_;
    std::size_t closingDepth_ = 0;
    bool closingCommitted_ = false;

    std::vector<PacketHandler> anyHandlers_;
    std::unordered_map<std::string, std::vector<PacketHandler>> namedHandlers_;
    std::vector<std::function<void()>> joinHandlers_;
    std::vector<std::function<void()>> spawnHandlers_;
    std::vector<HeartbeatHandler> heartbeatHandlers_;
    std::vector<ErrorHandler> closeHandlers_;
    std::vector<ErrorHandler> errorHandlers_;
    std::vector<StatusHandler> statusHandlers_;
    mutable std::mutex eventHandlersMutex_;
    CallbackLifetimeProvider callbackLifetimeProvider_;
    std::function<void()> afterTransportInstallTestHook_;
    std::function<void()> beforeTransportInstallTestHook_;
    std::function<void()> beforeConnectPhaseCommitTestHook_;
    std::function<void()> beforeQueueStartTestHook_;
    AuthenticationOptionsResolvedHandler
        authenticationOptionsResolvedHandler_;
    AuthenticationUnhandledRejectionHandler
        authenticationUnhandledRejectionHandler_;
    std::function<void(const std::vector<uint8_t>&)> reliableSendOverride_;
    std::mutex sendMutex_;
    mutable std::mutex rakNetCallbackMutex_;
    std::thread::id rakNetCallbackThreadId_;
    std::size_t rakNetCallbackDepth_ = 0;

    std::mutex queueMutex_;
    std::mutex queueLifecycleMutex_;
    std::condition_variable queueCv_;
    std::vector<VersionedGamePacket> queuedPackets_;
    std::thread queueThread_;
    bool stopQueue_ = true;
    // Logical timer demand is distinct from physical execution. close() must
    // suppress native queue ticks for its whole synchronous EventEmitter
    // snapshot while retaining a connect/resume request for rollback.
    bool queuePumpRequested_ = false;
    bool queuePumpEnabled_ = false;
    bool queuePumpInFlight_ = false;
    std::optional<std::chrono::steady_clock::time_point> queuePumpResumeDue_;
    std::optional<std::chrono::steady_clock::time_point> chunkRadiusDue_;
    std::optional<std::chrono::steady_clock::time_point> keepAliveDue_;
    int64_t tick_ = 0;

    std::optional<uint64_t> runtimeEntityId_;
    bool initializeOnNextStartGame_ = false;
    bool resourcePacksInfoHandled_ = false;
    bool resourcePackStackHandlerArmed_ = false;
    bool compressionReady_ = false;
    std::string compressionAlgorithm_ = "none";
    uint16_t compressionThreshold_ = 512;

    bool encryptionEnabled_ = false;
    DerivedKeyResult encryptionKeys_;
    uint64_t sendCounter_ = 0;
    uint64_t receiveCounter_ = 0;
    std::unique_ptr<BedrockCipherStream> encryptStream_;
    std::unique_ptr<BedrockCipherStream> decryptStream_;
    BedrockClientKeyPair clientKeys_;
    mutable std::mutex optionsMutex_;
    // Protected by connectLifecycleMutex_. A rejected Authflow constructor
    // still permits startQueue(), but it must never reach login/transport.
    bool authenticationRejected_ = false;

    static BedrockNetworkClientOptions normalizeOptions(BedrockNetworkClientOptions options);
    static bool versionAtLeast(const std::string& version, int major, int minor, int patch);
    static bool versionAtMost(const std::string& version, const std::string& maximum);

    void setStatus(BedrockNetworkClientStatus status);
    void emitError(const std::string& message);
    enum class CloseOrigin { Public, Transport };
    void emitClose(
        const std::string& reason,
        bool closeTransport,
        CloseOrigin origin
    );
    void beginDeferredClose(const std::string& reason);
    bool sendDisconnectPacket(const std::string& reason, bool hide);
    void clearEventHandlers();
    void emitAnyPacket(const VersionedGamePacket& packet);
    void emitNamedPacket(const VersionedGamePacket& packet);
    void emitNamedEvent(
        const std::string& eventName,
        const VersionedGamePacket& packet
    );
    void emitJoin();
    void emitSpawn();
    void emitHeartbeat(int64_t tick);
    CallbackLifetimeProvider callbackLifetimeProviderSnapshot() const;

    void handleRakNetConnected();
    void enterRakNetCallback();
    void leaveRakNetCallback() noexcept;
    void dispatchRakNetPayload(const std::vector<uint8_t>& payload);
    void handleRakNetPayload(const std::vector<uint8_t>& payload);
    void handlePacket(const VersionedGamePacket& packet);
    void handleResourcePacksInfo();
    void handleResourcePackStack();
    void handleStartGame(const VersionedGamePacket& packet);
    void handlePlayStatus(const VersionedGamePacket& packet);
    void handleTickSync(const VersionedGamePacket& packet);
    void handleLevelChunk(const VersionedGamePacket& packet);
    void handleClientCacheMissResponse(const VersionedGamePacket& packet);
    bool tryStoreLevelChunk(const BedrockLevelChunkPacket& levelChunk);

    void prepareLoginPacket();
    void sendLogin();
    void startEncryptionFromServerHandshake(const std::string& token);
    void drainSessionOutgoing();
    bool startQueue(bool pumpEnabled = true);
    void resumeQueuePump();
    bool pauseQueuePump();
    void stopQueue();
    void stopQueueIfRunning();
    void stopQueueLocked(bool preserveRollbackDeadline = false);
    void queueLoop();
    void resetLifecycle();
    void sendLocalPlayerInitialized(uint64_t runtimeEntityId);
    void sendPackets(const std::vector<VersionedGamePacket>& packets);
    void sendPacketsLocked(const std::vector<VersionedGamePacket>& packets);
    void sendReliablePayload(const std::vector<uint8_t>& payload);
    VersionedMcpeCompression choosePlainCompression(const std::vector<VersionedGamePacket>& packets) const;
};

inline BedrockNetworkClient createNetworkClient(BedrockNetworkClientOptions options = {}) {
    return BedrockNetworkClient(std::move(options));
}

} // namespace bedrock
