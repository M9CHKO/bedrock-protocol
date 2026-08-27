#pragma once

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bedrock {

struct RakNetClientTestAccess;

struct RakNetClientOptions {
    std::string host = "127.0.0.1";
    uint16_t port = 19132;
    int mtu = 1400;
    int protocolVersion = 11;
    uint64_t clientGuid = 0;
    int timeoutMs = 3000;
};

class RakNetClient {
public:
    using ConnectedHandler = std::function<void()>;
    using CloseHandler = std::function<void(const std::string&)>;
    using EncapsulatedHandler = std::function<void(const std::vector<uint8_t>&)>;
    using CallbackLifetimeProvider = std::function<std::shared_ptr<void>()>;

    explicit RakNetClient(RakNetClientOptions options = {});
    ~RakNetClient();

    RakNetClient(const RakNetClient&) = delete;
    RakNetClient& operator=(const RakNetClient&) = delete;

    // The transport worker captures this object's address. A live transport
    // therefore cannot be relocated; active moves throw std::logic_error
    // before mutating either object. Fresh and fully quiescent clients remain
    // movable.
    RakNetClient(RakNetClient&& other);
    RakNetClient& operator=(RakNetClient&& other);

    bool connect();
    void close(const std::string& reason = "closed");
    void requestStop() noexcept;
    void sendReliable(const std::vector<uint8_t>& payload);

    bool connected() const {
        return connected_;
    }

    uint16_t localPort() const {
        return localPort_;
    }

    int mtu() const {
        return mtu_;
    }

    std::string error() const {
        return error_;
    }

    void onConnected(ConnectedHandler handler) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        connectedHandler_ = std::move(handler);
    }

    void onClose(CloseHandler handler) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        closeHandler_ = std::move(handler);
    }

    void onEncapsulated(EncapsulatedHandler handler) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        encapsulatedHandler_ = std::move(handler);
    }

    // Optional owner lease used by facade clients whose callbacks may destroy
    // their last public owner from the RakNet worker itself. The provider also
    // acts as callback admission: an empty lease stops further delivery.
    void setCallbackLifetimeProvider(CallbackLifetimeProvider provider) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callbackLifetimeProvider_ = std::move(provider);
    }

private:
    friend struct RakNetClientTestAccess;

    struct SplitAccumulator {
        uint32_t count = 0;
        std::vector<std::vector<uint8_t>> parts;
        std::vector<bool> received;
    };

    struct PendingInboundPayload {
        uint32_t orderedIndex = 0;
        std::vector<uint8_t> payload;
    };

    RakNetClientOptions options_;
    int socket_ = -1;
    uint16_t localPort_ = 0;
    int mtu_ = 1400;
    std::array<uint8_t, 128> target_ {};
    int targetLen_ = 0;
    std::atomic<bool> running_ { false };
    std::atomic<bool> connected_ { false };
    std::atomic<bool> closeRequested_ { false };
    std::thread thread_;
    std::string error_;

    // shutdown() wakes the UDP descriptor but deliberately does not release
    // it until every select/recv/send user has unwound. This prevents both a
    // C++ data race on socket_ and the OS reusing the fd under runLoop().
    mutable std::mutex socketMutex_;
    mutable std::mutex threadMutex_;
    std::condition_variable threadCv_;
    bool workerActive_ = false;
    bool joinInProgress_ = false;
    bool connectActive_ = false;
    std::thread::id workerThreadId_;
    std::thread::id connectThreadId_;

    ConnectedHandler connectedHandler_;
    CloseHandler closeHandler_;
    EncapsulatedHandler encapsulatedHandler_;
    CallbackLifetimeProvider callbackLifetimeProvider_;
    std::function<void()> beforeRunningCommitTestHook_;
    mutable std::mutex callbackMutex_;

    std::mutex stateMutex_;
    uint32_t outgoingSequence_ = 0;
    uint32_t reliableIndex_ = 0;
    uint32_t orderedIndex_ = 0;
    uint16_t outgoingSplitId_ = 1;
    std::unordered_map<uint16_t, SplitAccumulator> splits_;
    std::vector<PendingInboundPayload> pendingInboundPayloads_;
    uint32_t nextInboundOrderedIndex_ = 0;
    std::map<uint32_t, std::vector<uint8_t>> pendingOrderedPayloads_;
    std::unordered_set<uint32_t> receivedDatagramSequences_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> sentReliableDatagrams_;

    void runLoop();
    void handlePacket(const std::vector<uint8_t>& packet);
    void sendToTarget(const std::vector<uint8_t>& packet);
    void sendReliableInternal(
        const std::vector<uint8_t>& payload,
        bool allowDuringClose = false
    );
    bool beginConnectActivity();
    void endConnectActivity();
    void shutdownSocket() noexcept;
    void finalizeSocketClose() noexcept;
    void endWorkerActivity() noexcept;
    std::shared_ptr<void> acquireCallbackLifetime() const;
    CallbackLifetimeProvider callbackLifetimeProviderSnapshot() const;
    ConnectedHandler connectedHandlerSnapshot() const;
    CloseHandler closeHandlerSnapshot() const;
    EncapsulatedHandler encapsulatedHandlerSnapshot() const;
    void sendConnectionRequest();
    void sendNewIncomingConnection();
    void sendConnectedPong(int64_t pingTime);
};

} // namespace bedrock
