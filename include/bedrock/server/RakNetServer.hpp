#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bedrock {

struct RakNetServerOptions {
    std::string host = "0.0.0.0";
    uint16_t port = 19132;
    int maxPlayers = 3;
    int protocolVersion = 11;
    uint64_t serverGuid = 0;
    std::string advertisement;
};

struct RakNetServerPeer {
    std::string address;
    uint16_t port = 0;
    uint64_t clientGuid = 0;
    int mtu = 1400;
};

class RakNetServer {
public:
    using OpenConnectionHandler = std::function<void(const RakNetServerPeer&)>;
    using CloseConnectionHandler = std::function<void(const RakNetServerPeer&)>;
    using RawPacketHandler = std::function<void(const RakNetServerPeer&, const std::vector<uint8_t>&)>;
    using EncapsulatedHandler = std::function<void(const RakNetServerPeer&, const std::vector<uint8_t>&)>;
    using AdvertisementProvider = std::function<std::string()>;

    explicit RakNetServer(RakNetServerOptions options = {});
    ~RakNetServer();

    RakNetServer(const RakNetServer&) = delete;
    RakNetServer& operator=(const RakNetServer&) = delete;

    // The worker captures this.  Moving a listening server would leave that
    // capture pointing at the moved-from object, so RakNetServer is
    // deliberately immovable just like it is non-copyable.
    RakNetServer(RakNetServer&& other) = delete;
    RakNetServer& operator=(RakNetServer&& other) = delete;

    void listen();
    void close();
    void closeAfter(std::chrono::milliseconds delay);

    // raknet-native ServerClient.close(silent) delegates to
    // RakPeer::CloseConnection(guid, !silent).
    void closePeer(const RakNetServerPeer& peer, bool silent = false);
    void closePeerAfter(
        const RakNetServerPeer& peer,
        std::chrono::milliseconds delay,
        bool silent = false
    );

    bool onWorkerThread() const {
        return thread_.joinable() && thread_.get_id() == std::this_thread::get_id();
    }

    bool listening() const {
        return running_;
    }

    uint16_t boundPort() const {
        return boundPort_;
    }

    const RakNetServerOptions& options() const {
        return options_;
    }

    void setAdvertisement(std::string advertisement) {
        options_.advertisement = std::move(advertisement);
    }

    void setAdvertisementProvider(AdvertisementProvider provider) {
        advertisementProvider_ = std::move(provider);
    }

    void onOpenConnection(OpenConnectionHandler handler) {
        openConnectionHandler_ = std::move(handler);
    }

    void onCloseConnection(CloseConnectionHandler handler) {
        closeConnectionHandler_ = std::move(handler);
    }

    void onRawPacket(RawPacketHandler handler) {
        rawPacketHandler_ = std::move(handler);
    }

    void onEncapsulated(EncapsulatedHandler handler) {
        encapsulatedHandler_ = std::move(handler);
    }

    void sendReliable(const RakNetServerPeer& peer, const std::vector<uint8_t>& payload);

private:
    friend struct BedrockServerTestAccess;

    struct PeerState {
        struct SplitAccumulator {
            uint32_t count = 0;
            std::vector<std::vector<uint8_t>> parts;
            std::vector<bool> received;
        };

        RakNetServerPeer peer;
        std::array<uint8_t, 128> endpoint {};
        int endpointLen = 0;
        uint32_t outgoingSequence = 0;
        uint32_t reliableIndex = 0;
        uint32_t orderedIndex = 0;
        uint16_t outgoingSplitId = 1;
        std::deque<uint32_t> receivedDatagramOrder;
        std::unordered_map<uint32_t, bool> receivedDatagramSequences;
        std::array<uint32_t, 32> expectedOrderedIndex {};
        std::array<bool, 32> expectedOrderedIndexInitialized {};
        std::array<std::map<uint32_t, std::vector<uint8_t>>, 32> pendingOrderedPayloads;
        std::unordered_map<uint16_t, SplitAccumulator> splits;
        std::unordered_map<uint32_t, std::vector<uint8_t>> sentReliableDatagrams;
        std::deque<uint32_t> pendingReliableDatagrams;
        std::unordered_map<
            uint32_t,
            std::chrono::steady_clock::time_point
        > reliableDatagramSentAt;
        std::chrono::steady_clock::time_point request2AssignedAt {};
        std::chrono::steady_clock::time_point timeLastDatagramArrived {};
        std::chrono::steady_clock::time_point lastReliableSend {};
        bool connectionRequestAccepted = false;
        bool connected = false;
    };

    struct ScheduledPeerClose {
        std::chrono::steady_clock::time_point deadline;
        std::string key;
        uint64_t clientGuid = 0;
        bool silent = false;
        uint64_t order = 0;
    };

    RakNetServerOptions options_;
    int socket_ = -1;
    uint16_t boundPort_ = 0;
    std::atomic<bool> running_ { false };
    std::thread thread_;
    OpenConnectionHandler openConnectionHandler_;
    CloseConnectionHandler closeConnectionHandler_;
    RawPacketHandler rawPacketHandler_;
    EncapsulatedHandler encapsulatedHandler_;
    AdvertisementProvider advertisementProvider_;
    std::mutex peersMutex_;
    std::unordered_map<std::string, PeerState> peers_;
    std::mutex lifecycleMutex_;
    std::mutex joinMutex_;
    std::vector<ScheduledPeerClose> scheduledPeerCloses_;
    bool shutdownScheduled_ = false;
    std::chrono::steady_clock::time_point shutdownDeadline_ {};
    bool advertisementUpdatesEnabled_ = false;
    std::chrono::steady_clock::time_point nextAdvertisementUpdate_ {};
    uint64_t scheduledCloseOrder_ = 0;

    void runLoop();
    bool processLifecycleDeadlines();
    void closePeerNow(const std::string& key, uint64_t clientGuid, bool silent);
    void finishShutdown();
    void joinWorkerIfExternal();
    void handlePacket(const std::vector<uint8_t>& packet, const void* sender, int senderLen);
    void sendTo(const void* target, int targetLen, const std::vector<uint8_t>& packet);
    void sendConnectedFrame(const RakNetServerPeer& peer, const void* target, int targetLen, const std::vector<uint8_t>& payload, uint8_t reliability);
    void sendReliableOrdered(
        const RakNetServerPeer& peer,
        const void* target,
        int targetLen,
        const std::vector<uint8_t>& payload,
        bool bypassCongestionWindow = false
    );
    void flushReliableQueue(const std::string& key);
};

} // namespace bedrock
