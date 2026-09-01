#pragma once

#include <bedrock/client/BedrockNetworkClient.hpp>
#include <bedrock/realms/BedrockRealms.hpp>
#include <bedrock/relay/BedrockRelay.hpp>
#include <bedrock/relay/LevelChunkRetentionCache.hpp>
#include <bedrock/server/BedrockServer.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bedrock {

struct BedrockRelayDownstreamProfile {
    std::string displayName;
    std::string xuid;
    std::string identity;
};

struct BedrockLiveRelayOptions {
    BedrockServerOptions server;
    BedrockNetworkClientOptions upstream;
    BedrockRealmSelection realms;

    bool forwardServerbound = true;
    bool forwardClientbound = true;
    bool skipClientboundLoginSuccess = true;
    bool skipClientboundResourcePacks = false;
    // Deprecated compatibility switches. Negotiation/encryption packets are
    // session-local and are always consumed by their owning endpoint.
    bool skipClientboundHandshake = true;
    bool forwardDownstreamClientData = true;
    bool queueClientboundLevelChunksUntilStartGame = true;
    bool enableChunkCaching = false;
    bool filterDownstreamHandshakePackets = true;
    bool logging = false;
    // Opt-in, secret-safe diagnostics for resource-pack negotiation and item
    // palette/order issues. This never changes packet forwarding or parsing
    // policy and does not print content keys or CDN URLs.
    bool itemResourceDiagnostics = false;
    // Relay#forceSingle rejects a second transport while any accepted
    // downstream Player session is still present.
    bool forceSingle = false;
    // Opt-in latest-connection-wins policy for single-player frontends. A
    // newly accepted transport synchronously tears down the previous relay
    // session and its upstream before its connect callback runs. This is
    // useful on mobile where an old UDP transport can outlive the game UI.
    bool replaceExisting = false;
    // relay.js chooses username from the relay-level offline flag, separately
    // from destination.offline ?? relay.offline used for upstream auth mode.
    bool useDownstreamDisplayNameForUpstreamUsername = false;
    VersionedMcpeCompression clientboundCompression = VersionedMcpeCompression::Automatic;
};

namespace detail {

// Shared with the pure option regression: relay.js replaces both the
// username consumed by Authflow and its cache profile identity.
void applyRelayDownstreamIdentity(
    BedrockLiveRelayOptions& options,
    const BedrockRelayDownstreamProfile& profile
);

} // namespace detail

struct BedrockLiveRelayStatus {
    bool listening = false;
    bool downstreamJoined = false;
    bool upstreamStarted = false;
    bool upstreamReady = false;
    uint16_t boundPort = 0;
    std::size_t downstreamConnections = 0;
    std::size_t downstreamJoinedCount = 0;
    std::size_t upstreamStartedCount = 0;
    std::size_t upstreamReadyCount = 0;
};

class BedrockLiveRelay {
public:
    using PacketHandler = std::function<void(BedrockRelayPacketEvent&)>;
    using ForwardedHandler =
        std::function<void(const BedrockRelayPacketEvent&)>;
    using ConnectionHandler = std::function<void(const BedrockServerConnection&)>;
    using ErrorHandler = std::function<void(const std::string&)>;
    using StatusHandler = std::function<void(const BedrockLiveRelayStatus&)>;
    using DiagnosticHandler = std::function<void(const std::string&)>;
    using MsaCodeHandler = std::function<void(
        const XboxDeviceCodeInfo&,
        const BedrockServerConnection&
    )>;
    using UpstreamJoinHandler = std::function<void(
        const BedrockServerConnection&,
        const std::shared_ptr<BedrockNetworkClient>&
    )>;

    explicit BedrockLiveRelay(BedrockLiveRelayOptions options = {});
    ~BedrockLiveRelay();

    BedrockLiveRelay(const BedrockLiveRelay&) = delete;
    BedrockLiveRelay& operator=(const BedrockLiveRelay&) = delete;

    ServerListenResult listen();
    int run();
    void close(const std::string& reason = "closed");

    void onConnect(ConnectionHandler handler);
    void onJoin(ConnectionHandler handler);
    // Distinct from onJoin(), which observes the downstream joining the local
    // server. This mirrors Relay.emit('join', downstream, upstream) after the
    // backend client is ready and the pending upstream queue has been flushed.
    void onUpstreamJoin(UpstreamJoinHandler handler);
    void onDisconnect(ConnectionHandler handler);
    void onClientbound(PacketHandler handler);
    void onServerbound(PacketHandler handler);
    // Observes the final immutable packet after relay handlers accepted it
    // and the destination send/queue operation completed.
    void onForwarded(ForwardedHandler handler);
    void on(const std::string& direction, PacketHandler handler);
    void onError(ErrorHandler handler);
    void onStatus(StatusHandler handler);
    void onDiagnostic(DiagnosticHandler handler);
    // Relay#openUpstreamConnection passes the exact downstream Player as the
    // second onMsaCode argument. The low-level runtime exposes its stable
    // connection identity; the high-level Relay maps it back to RelayPlayer.
    void onMsaCode(MsaCodeHandler handler);

    bool listening() const;
    bool downstreamJoined() const;
    bool upstreamStarted() const;
    bool upstreamReady() const;
    uint16_t boundPort() const;
    const BedrockLiveRelayOptions& options() const;

    // Keeps exact level_chunk packet bytes for the active downstream session.
    // NetworkChunkPublisherUpdate still controls the client's visible window;
    // the cache is cleared on dimension/session changes and bounded by memory.
    void configureLevelChunkRetention(bool enabled, uint32_t radiusChunks);
    LevelChunkRetentionStats levelChunkRetentionStats() const noexcept;

    BedrockServer& server();
    // Compatibility view of the first active relay session.
    BedrockNetworkClient* upstream();
    BedrockNetworkClient* upstream(const BedrockServerConnection& connection);
    // Owning snapshots for code that may race a session disconnect.
    std::shared_ptr<BedrockNetworkClient> upstreamShared();
    std::shared_ptr<BedrockNetworkClient> upstreamShared(
        const BedrockServerConnection& connection
    );
    std::size_t sessionCount() const;
    std::size_t upstreamCount() const;
    void closeUpstreamConnection(
        const BedrockServerConnection& connection,
        const std::string& reason = "closed"
    );
    // Starts relay teardown immediately, before the server's delayed
    // protocol-level disconnect finishes. This prevents backend packets from
    // reaching callbacks or queues after the downstream has been rejected.
    void disconnectDownstream(
        const BedrockServerConnection& connection,
        const std::string& reason = "closed"
    );
    uint64_t finalSessionResetCount() const noexcept;

    static std::string sessionId(const BedrockServerConnection& connection);

private:
    struct Session;

    BedrockLiveRelayOptions options_;
    BedrockNetworkClientOptions baseUpstreamOptions_;
    std::unique_ptr<BedrockServer> server_;

    mutable std::mutex mutex_;
    std::condition_variable closedCv_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    std::unordered_set<std::string> rejectedConnections_;
    std::string primarySessionId_;
    mutable std::recursive_mutex handlerDispatchMutex_;
    mutable std::mutex levelChunkRetentionConfigMutex_;
    bool levelChunkRetentionEnabled_ = false;
    uint32_t retainedLevelChunkRadius_ = 24;

    std::atomic<bool> closed_ {true};
    std::atomic<bool> listening_ {false};
    std::atomic<uint64_t> finalSessionResetCount_ {0};

    std::vector<ConnectionHandler> connectHandlers_;
    std::vector<ConnectionHandler> joinHandlers_;
    std::vector<UpstreamJoinHandler> upstreamJoinHandlers_;
    std::vector<ConnectionHandler> disconnectHandlers_;
    std::vector<PacketHandler> clientboundHandlers_;
    std::vector<PacketHandler> serverboundHandlers_;
    std::vector<ForwardedHandler> forwardedHandlers_;
    std::vector<ErrorHandler> errorHandlers_;
    std::vector<StatusHandler> statusHandlers_;
    std::vector<DiagnosticHandler> diagnosticHandlers_;
    mutable std::mutex msaCodeHandlersMutex_;
    std::vector<MsaCodeHandler> msaCodeHandlers_;

    static BedrockLiveRelayOptions normalizeOptions(BedrockLiveRelayOptions options);
    static bool isDownstreamHandshakePacket(const std::string& name);
    static bool isClientboundHandshakePacket(const std::string& name);
    static bool isClientboundResourcePackPacket(const std::string& name);
    static bool isPlayStatusLoginSuccess(const VersionedGamePacket& packet);
    static bool isPlayStatusPlayerSpawn(const std::string& version, const VersionedGamePacket& packet);

    void emitError(const std::string& message);
    void emitStatus();
    void emitDiagnostic(const std::string& message);
    void emitForwarded(
        const std::shared_ptr<Session>& session,
        BedrockRelayDirection direction,
        const VersionedGamePacket& packet
    ) noexcept;
    bool emitMsaCode(
        const XboxDeviceCodeInfo& code,
        const BedrockServerConnection& connection
    );
    std::shared_ptr<Session> findSession(
        const BedrockServerConnection& connection
    ) const;
    std::shared_ptr<Session> primarySession() const;
    void captureDownstreamClientData(
        const std::shared_ptr<Session>& session,
        const VersionedGamePacket& packet
    );
    void handleDownstreamJoin(const std::shared_ptr<Session>& session);
    void resolveUpstreamRealm(BedrockNetworkClientOptions& upstreamOptions);
    bool resetRelaySession(
        const std::shared_ptr<Session>& session,
        const std::string& reason,
        bool retainDownstream
    );
    bool removeRelaySession(
        const std::shared_ptr<Session>& session,
        const std::string& reason
    );
    void startUpstream(const std::shared_ptr<Session>& session);
    void handleUpstreamPacket(
        const std::shared_ptr<Session>& session,
        const VersionedGamePacket& packet
    );
    void handleDownstreamPacket(const BedrockServerPacketEvent& event);
    void diagnosePacket(
        const std::shared_ptr<Session>& session,
        BedrockRelayDirection direction,
        const VersionedGamePacket& packet
    );
    void forwardClientbound(
        const std::shared_ptr<Session>& session,
        const VersionedGamePacket& packet
    );
    void retainClientboundLevelChunk(
        const std::shared_ptr<Session>& session,
        const VersionedGamePacket& packet
    ) noexcept;
    void forwardServerbound(
        const std::shared_ptr<Session>& session,
        const VersionedGamePacket& packet,
        bool immediate = false
    );
    std::vector<VersionedGamePacket> applyHandlers(
        const std::shared_ptr<Session>& session,
        BedrockRelayDirection direction,
        const VersionedGamePacket& packet
    );
};

inline BedrockLiveRelay createLiveRelay(BedrockLiveRelayOptions options = {}) {
    return BedrockLiveRelay(std::move(options));
}

// Historical spelling retained for source compatibility.
inline BedrockLiveRelay createRelayServer(BedrockLiveRelayOptions options = {}) {
    return createLiveRelay(std::move(options));
}

} // namespace bedrock
