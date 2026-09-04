#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
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
    int timeoutMs = 30'000;
};

struct RakNetServerPeer {
    std::string address;
    uint16_t port = 0;
    uint64_t clientGuid = 0;
    int mtu = 1400;
};

enum class RakNetServerSendStatus {
    Accepted,
    EmptyPayload,
    ServerStopped,
    UnknownPeer,
    NativeUnavailable,
    NotConnected,
    Rejected
};

const char* rakNetServerSendStatusName(RakNetServerSendStatus status) noexcept;

struct RakNetServerSendResult {
    RakNetServerSendStatus status = RakNetServerSendStatus::Rejected;
    uint32_t receipt = 0;
    int connectionState = -1;

    bool accepted() const noexcept {
        return status == RakNetServerSendStatus::Accepted;
    }
};

struct RakNetServerPeerStatistics {
    bool peerKnown = false;
    bool nativeActive = false;
    bool statisticsAvailable = false;
    int connectionState = -1;
    uint64_t userMessageBytesPushed = 0;
    uint64_t userMessageBytesSent = 0;
    uint64_t userMessageBytesResent = 0;
    uint64_t userMessageBytesReceivedProcessed = 0;
    uint64_t userMessageBytesReceivedIgnored = 0;
    uint64_t actualBytesSent = 0;
    uint64_t actualBytesReceived = 0;
    uint64_t sendBufferMessages = 0;
    uint64_t sendBufferBytes = 0;
    uint64_t resendBufferMessages = 0;
    uint64_t resendBufferBytes = 0;
    float packetLossLastSecond = 0.0f;
    float packetLossTotal = 0.0f;
};

class RakNetServer {
public:
    using OpenConnectionHandler =
        std::function<void(const RakNetServerPeer&)>;
    using CloseConnectionHandler =
        std::function<void(const RakNetServerPeer&)>;
    using RawPacketHandler = std::function<void(
        const RakNetServerPeer&,
        const std::vector<uint8_t>&
    )>;
    using EncapsulatedHandler = std::function<void(
        const RakNetServerPeer&,
        const std::vector<uint8_t>&
    )>;
    using WorkerErrorHandler = std::function<void(
        const RakNetServerPeer&,
        const std::string&
    )>;
    using AdvertisementProvider = std::function<std::string()>;

    explicit RakNetServer(RakNetServerOptions options = {});
    ~RakNetServer();

    RakNetServer(const RakNetServer&) = delete;
    RakNetServer& operator=(const RakNetServer&) = delete;
    RakNetServer(RakNetServer&& other) = delete;
    RakNetServer& operator=(RakNetServer&& other) = delete;

    void listen();
    void close();
    void closeAfter(std::chrono::milliseconds delay);

    // Mirrors raknet-native ServerClient.close(silent).
    void closePeer(const RakNetServerPeer& peer, bool silent = false);
    void closePeerAfter(
        const RakNetServerPeer& peer,
        std::chrono::milliseconds delay,
        bool silent = false
    );

    bool onWorkerThread() const {
        return thread_.joinable() &&
            thread_.get_id() == std::this_thread::get_id();
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

    void setAdvertisement(std::string advertisement);

    void setAdvertisementProvider(AdvertisementProvider provider) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        advertisementProvider_ = std::move(provider);
    }

    void onOpenConnection(OpenConnectionHandler handler) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        openConnectionHandler_ = std::move(handler);
    }

    void onCloseConnection(CloseConnectionHandler handler) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        closeConnectionHandler_ = std::move(handler);
    }

    void onRawPacket(RawPacketHandler handler) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        rawPacketHandler_ = std::move(handler);
    }

    void onEncapsulated(EncapsulatedHandler handler) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        encapsulatedHandler_ = std::move(handler);
    }

    // Reports an exception contained on the RakNet worker. A populated peer
    // identifies the affected connection; an empty peer is server-scoped.
    void onWorkerError(WorkerErrorHandler handler) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        workerErrorHandler_ = std::move(handler);
    }

    RakNetServerSendResult sendReliable(
        const RakNetServerPeer& peer,
        const std::vector<uint8_t>& payload,
        bool immediate = false
    );

    RakNetServerPeerStatistics peerStatistics(
        const RakNetServerPeer& peer
    ) const;

private:
    friend struct BedrockServerTestAccess;

    struct NativeState;

    struct ScheduledPeerClose {
        std::chrono::steady_clock::time_point deadline;
        RakNetServerPeer peer;
        bool silent = false;
        uint64_t order = 0;
    };

    RakNetServerOptions options_;
    std::unique_ptr<NativeState> native_;
    std::atomic<uint16_t> boundPort_ {0};
    std::atomic<bool> running_ {false};
    std::thread thread_;

    mutable std::mutex nativeMutex_;
    mutable std::mutex peersMutex_;
    std::unordered_map<std::string, RakNetServerPeer> peers_;
    mutable std::mutex callbackMutex_;
    OpenConnectionHandler openConnectionHandler_;
    CloseConnectionHandler closeConnectionHandler_;
    RawPacketHandler rawPacketHandler_;
    EncapsulatedHandler encapsulatedHandler_;
    WorkerErrorHandler workerErrorHandler_;
    AdvertisementProvider advertisementProvider_;

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
    bool closePeerNow(const RakNetServerPeer& peer, bool silent);
    void finishShutdown();
    void joinWorkerIfExternal();
    void destroyPeer() noexcept;
    void handleNativePacket(
        const RakNetServerPeer& peer,
        const std::vector<uint8_t>& packet
    );
    void reportWorkerError(
        const RakNetServerPeer& peer,
        const char* detail
    ) noexcept;
    void shutdownAfterWorkerError(const char* detail) noexcept;
    void failPeerAfterWorkerError(
        const RakNetServerPeer& peer,
        const char* detail
    ) noexcept;
    static std::string peerKey(const RakNetServerPeer& peer);
    OpenConnectionHandler openConnectionHandlerSnapshot() const;
    CloseConnectionHandler closeConnectionHandlerSnapshot() const;
    RawPacketHandler rawPacketHandlerSnapshot() const;
    EncapsulatedHandler encapsulatedHandlerSnapshot() const;
    WorkerErrorHandler workerErrorHandlerSnapshot() const;
    AdvertisementProvider advertisementProviderSnapshot() const;
};

} // namespace bedrock
