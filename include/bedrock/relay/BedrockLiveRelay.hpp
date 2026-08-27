#pragma once

#include <bedrock/client/BedrockNetworkClient.hpp>
#include <bedrock/realms/BedrockRealms.hpp>
#include <bedrock/relay/BedrockRelay.hpp>
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
    bool skipClientboundHandshake = true;
    bool forwardDownstreamClientData = true;
    bool queueClientboundLevelChunksUntilStartGame = true;
    bool enableChunkCaching = false;
    bool filterDownstreamHandshakePackets = true;
    bool logging = false;
    // Relay#forceSingle rejects a second transport while any accepted
    // downstream Player session is still present.
    bool forceSingle = false;
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
    using ConnectionHandler = std::function<void(const BedrockServerConnection&)>;
    using ErrorHandler = std::function<void(const std::string&)>;
    using StatusHandler = std::function<void(const BedrockLiveRelayStatus&)>;

    explicit BedrockLiveRelay(BedrockLiveRelayOptions options = {});
    ~BedrockLiveRelay();

    BedrockLiveRelay(const BedrockLiveRelay&) = delete;
    BedrockLiveRelay& operator=(const BedrockLiveRelay&) = delete;

    void listen();
    int run();
    void close(const std::string& reason = "closed");

    void onConnect(ConnectionHandler handler);
    void onJoin(ConnectionHandler handler);
    void onDisconnect(ConnectionHandler handler);
    void onClientbound(PacketHandler handler);
    void onServerbound(PacketHandler handler);
    void on(const std::string& direction, PacketHandler handler);
    void onError(ErrorHandler handler);
    void onStatus(StatusHandler handler);

    bool listening() const;
    bool downstreamJoined() const;
    bool upstreamStarted() const;
    bool upstreamReady() const;
    uint16_t boundPort() const;
    const BedrockLiveRelayOptions& options() const;

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
    mutable std::mutex handlerDispatchMutex_;

    std::atomic<bool> closed_ {true};
    std::atomic<bool> listening_ {false};

    std::vector<ConnectionHandler> connectHandlers_;
    std::vector<ConnectionHandler> joinHandlers_;
    std::vector<ConnectionHandler> disconnectHandlers_;
    std::vector<PacketHandler> clientboundHandlers_;
    std::vector<PacketHandler> serverboundHandlers_;
    std::vector<ErrorHandler> errorHandlers_;
    std::vector<StatusHandler> statusHandlers_;

    static BedrockLiveRelayOptions normalizeOptions(BedrockLiveRelayOptions options);
    static bool isDownstreamHandshakePacket(const std::string& name);
    static bool isClientboundHandshakePacket(const std::string& name);
    static bool isClientboundResourcePackPacket(const std::string& name);
    static bool isPlayStatusLoginSuccess(const VersionedGamePacket& packet);
    static bool isPlayStatusPlayerSpawn(const std::string& version, const VersionedGamePacket& packet);

    void emitError(const std::string& message);
    void emitStatus();
    std::shared_ptr<Session> findSession(
        const BedrockServerConnection& connection
    ) const;
    std::shared_ptr<Session> primarySession() const;
    void captureDownstreamClientData(
        const std::shared_ptr<Session>& session,
        const VersionedGamePacket& packet
    );
    void resolveUpstreamRealm(BedrockNetworkClientOptions& upstreamOptions);
    void resetRelaySession(
        const std::shared_ptr<Session>& session,
        const std::string& reason,
        bool retainDownstream
    );
    void removeRelaySession(
        const std::shared_ptr<Session>& session,
        const std::string& reason
    );
    void startUpstream(const std::shared_ptr<Session>& session);
    void handleUpstreamPacket(
        const std::shared_ptr<Session>& session,
        const VersionedGamePacket& packet
    );
    void handleDownstreamPacket(const BedrockServerPacketEvent& event);
    void forwardClientbound(
        const std::shared_ptr<Session>& session,
        const VersionedGamePacket& packet
    );
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

inline BedrockLiveRelay createRelayServer(BedrockLiveRelayOptions options = {}) {
    return BedrockLiveRelay(std::move(options));
}

} // namespace bedrock
