#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bedrock {

struct RakNetClientTestAccess;

struct RakNetClientOptions {
    std::string host = "127.0.0.1";
    uint16_t port = 19132;
    int mtu = 1400;
    int protocolVersion = 11;
    uint64_t clientGuid = 0;
    // Wall-clock budget for the initial RakNet connection handshake.
    int timeoutMs = 3000;
    // Active-session reliable-delivery timeout. This must stay independent
    // from timeoutMs: mobile clients can require a long inactivity window
    // without making a failed initial connection equally slow.
    int reliabilityTimeoutMs = 30'000;
};

class RakNetClient {
public:
    using ConnectedHandler = std::function<void()>;
    using CloseHandler = std::function<void(const std::string&)>;
    using EncapsulatedHandler =
        std::function<void(const std::vector<uint8_t>&)>;
    using CallbackLifetimeProvider =
        std::function<std::shared_ptr<void>()>;

    explicit RakNetClient(RakNetClientOptions options = {});
    ~RakNetClient();

    RakNetClient(const RakNetClient&) = delete;
    RakNetClient& operator=(const RakNetClient&) = delete;

    // RakPeer and the callback worker both retain this object's address.
    // Active clients therefore remain immovable, matching the previous API.
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
        std::lock_guard<std::mutex> lock(errorMutex_);
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
    // their last public owner from the RakNet worker itself.
    void setCallbackLifetimeProvider(CallbackLifetimeProvider provider) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callbackLifetimeProvider_ = std::move(provider);
    }

private:
    friend struct RakNetClientTestAccess;

    struct NativeState;

    RakNetClientOptions options_;
    std::unique_ptr<NativeState> native_;
    std::atomic<uint16_t> localPort_ {0};
    std::atomic<int> mtu_ {1400};
    std::atomic<bool> running_ {false};
    std::atomic<bool> connected_ {false};
    std::atomic<bool> connectAccepted_ {false};
    std::atomic<bool> connectedCallbackPending_ {false};
    std::atomic<bool> closeRequested_ {false};
    std::thread thread_;

    mutable std::mutex nativeMutex_;
    mutable std::mutex errorMutex_;
    std::string error_;

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

    void runLoop();
    void handleNativePacket(const std::vector<uint8_t>& packet);
    bool beginConnectActivity();
    void endConnectActivity();
    void shutdownPeer(unsigned int blockDuration) noexcept;
    void destroyPeer() noexcept;
    void endWorkerActivity() noexcept;
    void failWorkerCallback(const char* detail) noexcept;
    void setError(std::string error);
    CallbackLifetimeProvider callbackLifetimeProviderSnapshot() const;
    ConnectedHandler connectedHandlerSnapshot() const;
    CloseHandler closeHandlerSnapshot() const;
    EncapsulatedHandler encapsulatedHandlerSnapshot() const;
};

} // namespace bedrock
