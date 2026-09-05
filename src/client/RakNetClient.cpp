#include <bedrock/client/RakNetClient.hpp>

#include <MessageIdentifiers.h>
#include <RakNetTypes.h>
#include <RakPeerInterface.h>
#include <RakSleep.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <optional>
#include <utility>

namespace bedrock {

struct RakNetClient::NativeState {
    RakNet::RakPeerInterface* peer = nullptr;
    RakNet::SystemAddress target = RakNet::UNASSIGNED_SYSTEM_ADDRESS;
};

namespace {

const char* startupError(RakNet::StartupResult result) {
    switch (result) {
        case RakNet::RAKNET_STARTED: return "started";
        case RakNet::RAKNET_ALREADY_STARTED: return "already started";
        case RakNet::INVALID_SOCKET_DESCRIPTORS:
            return "invalid socket descriptors";
        case RakNet::INVALID_MAX_CONNECTIONS:
            return "invalid maximum connections";
        case RakNet::SOCKET_FAMILY_NOT_SUPPORTED:
            return "socket family not supported";
        case RakNet::SOCKET_PORT_ALREADY_IN_USE:
            return "socket port already in use";
        case RakNet::SOCKET_FAILED_TO_BIND: return "socket bind failed";
        case RakNet::SOCKET_FAILED_TEST_SEND:
            return "socket test send failed";
        case RakNet::PORT_CANNOT_BE_ZERO: return "port cannot be zero";
        case RakNet::FAILED_TO_CREATE_NETWORK_THREAD:
            return "failed to create network thread";
        case RakNet::COULD_NOT_GENERATE_GUID:
            return "could not generate GUID";
        case RakNet::STARTUP_OTHER_FAILURE: return "other startup failure";
    }
    return "unknown startup failure";
}

const char* connectError(RakNet::ConnectionAttemptResult result) {
    switch (result) {
        case RakNet::CONNECTION_ATTEMPT_STARTED: return "started";
        case RakNet::INVALID_PARAMETER: return "invalid parameter";
        case RakNet::CANNOT_RESOLVE_DOMAIN_NAME:
            return "cannot resolve domain name";
        case RakNet::ALREADY_CONNECTED_TO_ENDPOINT:
            return "already connected to endpoint";
        case RakNet::CONNECTION_ATTEMPT_ALREADY_IN_PROGRESS:
            return "connection attempt already in progress";
        case RakNet::SECURITY_INITIALIZATION_FAILED:
            return "security initialization failed";
    }
    return "unknown connection failure";
}

bool isDisconnectMessage(uint8_t id) {
    return id == ID_DISCONNECTION_NOTIFICATION ||
        id == ID_CONNECTION_LOST ||
        id == ID_CONNECTION_BANNED ||
        id == ID_INCOMPATIBLE_PROTOCOL_VERSION;
}

std::optional<std::string> connectionFailureMessage(uint8_t id) {
    switch (id) {
        case ID_CONNECTION_ATTEMPT_FAILED:
            return "Connection attempt failed";
        case ID_NO_FREE_INCOMING_CONNECTIONS:
            return "Server has no free incoming connections";
        case ID_CONNECTION_BANNED:
            return "Connection banned";
        case ID_INVALID_PASSWORD:
            return "Invalid RakNet password";
        case ID_INCOMPATIBLE_PROTOCOL_VERSION:
            return "Incompatible RakNet protocol version";
        case ID_IP_RECENTLY_CONNECTED:
            return "IP recently connected";
        default:
            return std::nullopt;
    }
}

} // namespace

RakNetClient::RakNetClient(RakNetClientOptions options)
    : options_(std::move(options)),
      native_(std::make_unique<NativeState>()),
      mtu_(options_.mtu) {}

RakNetClient::~RakNetClient() {
    close();
    destroyPeer();
}

RakNetClient::RakNetClient(RakNetClient&& other)
    : native_(std::make_unique<NativeState>()) {
    *this = std::move(other);
}

RakNetClient& RakNetClient::operator=(RakNetClient&& other) {
    if (this == &other) return *this;

    {
        std::scoped_lock lock(threadMutex_, other.threadMutex_);
        const auto active = [](const RakNetClient& client) {
            return client.running_.load() || client.connected_.load() ||
                client.connectActive_ || client.workerActive_ ||
                client.joinInProgress_ || client.thread_.joinable();
        };
        if (active(*this) || active(other)) {
            throw std::logic_error("Cannot move active RakNetClient");
        }
    }

    destroyPeer();
    options_ = std::move(other.options_);
    {
        std::scoped_lock lock(nativeMutex_, other.nativeMutex_);
        native_ = std::move(other.native_);
        if (!native_) native_ = std::make_unique<NativeState>();
        other.native_ = std::make_unique<NativeState>();
    }
    localPort_.store(other.localPort_.load());
    mtu_.store(other.mtu_.load());
    running_.store(false);
    connected_.store(false);
    connectAccepted_.store(other.connectAccepted_.load());
    connectedCallbackPending_.store(
        other.connectedCallbackPending_.load()
    );
    other.connectAccepted_.store(false);
    other.connectedCallbackPending_.store(false);
    closeRequested_.store(other.closeRequested_.load());
    {
        std::scoped_lock lock(errorMutex_, other.errorMutex_);
        error_ = std::move(other.error_);
    }
    {
        std::scoped_lock lock(callbackMutex_, other.callbackMutex_);
        connectedHandler_ = std::move(other.connectedHandler_);
        closeHandler_ = std::move(other.closeHandler_);
        encapsulatedHandler_ = std::move(other.encapsulatedHandler_);
        callbackLifetimeProvider_ =
            std::move(other.callbackLifetimeProvider_);
        beforeRunningCommitTestHook_ =
            std::move(other.beforeRunningCommitTestHook_);
    }
    return *this;
}

bool RakNetClient::connect() {
    if (running_.load()) {
        return connectAccepted_.load() &&
            !connectedCallbackPending_.load() && connected_.load() &&
            !closeRequested_.load();
    }
    if (!beginConnectActivity()) return running_.load();
    auto connectActivity = std::unique_ptr<void, std::function<void(void*)>>(
        this,
        [this](void*) { endConnectActivity(); }
    );
    if (closeRequested_.load()) return false;
    connectAccepted_.store(false);
    connectedCallbackPending_.store(false);

    RakNet::SystemAddress target;
    const std::string host = options_.host == "::1"
        ? "127.0.0.1"
        : options_.host;
    if (!target.FromStringExplicitPort(host.c_str(), options_.port, 4) &&
        !target.FromStringExplicitPort(host.c_str(), options_.port, 6)) {
        setError("Invalid connection address " + host + "/" +
            std::to_string(options_.port));
        return false;
    }

    auto* peer = RakNet::RakPeerInterface::GetInstance();
    if (!peer) {
        setError("Unable to allocate RakPeer");
        return false;
    }
    peer->SetOccasionalPing(true);
    peer->SetUnreliableTimeout(1000);
    if (options_.protocolVersion >= 0) {
        peer->SetProtocolVersion(options_.protocolVersion);
    }
    if (options_.clientGuid != 0) {
        peer->SetMyGUID(RakNet::RakNetGUID(options_.clientGuid));
    }

    RakNet::SocketDescriptor descriptor(0, nullptr);
    descriptor.socketFamily = AF_INET;
    const auto startup = peer->Startup(8, &descriptor, 1);
    if (startup != RakNet::RAKNET_STARTED) {
        setError(std::string("RakNet startup failed: ") +
            startupError(startup));
        try {
            RakNet::RakPeerInterface::DestroyInstance(peer);
        } catch (...) {
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        native_->peer = peer;
        native_->target = target;
    }
    localPort_.store(peer->GetMyBoundAddress().GetPort());

    std::function<void()> beforeRunningCommitHook;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        beforeRunningCommitHook = beforeRunningCommitTestHook_;
    }
    if (beforeRunningCommitHook) beforeRunningCommitHook();
    if (closeRequested_.load()) {
        shutdownPeer(0);
        destroyPeer();
        return false;
    }

    const unsigned interval = 500;
    const unsigned attempts = static_cast<unsigned>(std::max(
        3,
        (std::max(options_.timeoutMs, 1) +
            static_cast<int>(interval) - 1) /
            static_cast<int>(interval)
    ));
    RakNet::ConnectionAttemptResult result;
    {
        // Keep Connect and Shutdown/Destroy mutually exclusive. Starting the
        // native attempt before the receive worker is safe because RakPeer
        // queues notifications until Receive() drains them.
        std::lock_guard<std::mutex> lock(nativeMutex_);
        if (closeRequested_.load() || !native_ || native_->peer != peer) {
            return false;
        }
        result = peer->Connect(
            host.c_str(),
            options_.port,
            "",
            0,
            nullptr,
            0,
            attempts,
            interval,
            static_cast<RakNet::TimeMS>(
                std::max(options_.reliabilityTimeoutMs, 1)
            )
        );
    }
    if (result != RakNet::CONNECTION_ATTEMPT_STARTED) {
        setError(std::string("Unable to connect: ") + connectError(result));
        close("connect failed");
        return false;
    }

    bool cancelledBeforeWorker = false;
    try {
        std::lock_guard<std::mutex> lock(threadMutex_);
        if (closeRequested_.load()) {
            cancelledBeforeWorker = true;
        } else {
            running_.store(true);
        }
        if (cancelledBeforeWorker) {
            workerActive_ = false;
        } else {
            workerActive_ = true;
            try {
                thread_ = std::thread([this]() noexcept {
                    // Keep the owner lease alive through final bookkeeping.
                    // Releasing the last owner before endWorkerActivity()
                    // would make that final access to this a use-after-free.
                    std::shared_ptr<void> threadLease;
                    try {
                        {
                            std::lock_guard<std::mutex> lock(threadMutex_);
                            workerThreadId_ = std::this_thread::get_id();
                        }
                        threadCv_.notify_all();

                        const auto provider = callbackLifetimeProviderSnapshot();
                        if (provider) threadLease = provider();
                        if (!provider || threadLease) runLoop();
                    } catch (const std::exception& error) {
                        failWorkerCallback(error.what());
                    } catch (...) {
                        failWorkerCallback("unknown native exception");
                    }
                    // Keep worker bookkeeping and native teardown outside the
                    // guarded callback body, but still inside the noexcept
                    // thread entry so every return path reconciles state.
                    endWorkerActivity();
                });
            } catch (...) {
                workerActive_ = false;
                throw;
            }
        }
    } catch (const std::exception& e) {
        running_.store(false);
        setError(e.what());
        shutdownPeer(0);
        destroyPeer();
        threadCv_.notify_all();
        return false;
    } catch (...) {
        running_.store(false);
        setError("failed to start RakNet worker");
        shutdownPeer(0);
        destroyPeer();
        threadCv_.notify_all();
        return false;
    }
    // Once the worker is running, its onConnected callback may close the
    // connection before this thread reaches the wait. Let the completion
    // predicate distinguish that successful handshake from cancellation.
    if (cancelledBeforeWorker) {
        close("connect cancelled");
        return false;
    }

    bool completed = false;
    {
        std::unique_lock<std::mutex> lock(threadMutex_);
        completed = threadCv_.wait_for(
            lock,
            std::chrono::milliseconds(std::max(options_.timeoutMs, 1) + 250),
            [this]() {
                if (connectedCallbackPending_.load()) return false;
                return connectAccepted_.load() || closeRequested_.load() ||
                    !running_.load();
            }
        );
    }
    if (connectAccepted_.load()) return true;

    if (!completed && !closeRequested_.load()) {
        setError("Connection timed out");
        requestStop();
        close("connect timed out");
        return false;
    }

    if (closeRequested_.load() && !error().empty()) {
        // A native failure stopped the worker. Reap it synchronously without
        // emitting a transport-close callback for a session that never
        // reached ID_CONNECTION_REQUEST_ACCEPTED.
        close("connect failed");
    }
    return false;
}

void RakNetClient::close(const std::string& reason) {
    bool firstClose = false;
    bool wasRunning = false;
    bool wasConnected = false;
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        firstClose = !closeRequested_.exchange(true);
        if (firstClose) {
            wasRunning = running_.exchange(false);
            wasConnected = connected_.exchange(false);
        }
    }

    shutdownPeer(firstClose ? 300u : 0u);

    std::thread worker;
    bool selfWorker = false;
    bool selfConnect = false;
    {
        std::unique_lock<std::mutex> lock(threadMutex_);
        const auto current = std::this_thread::get_id();
        selfWorker = workerActive_ && workerThreadId_ == current;
        selfConnect = connectActive_ && connectThreadId_ == current;

        // connect() waits for the connected callback to finish. That callback
        // must not wait back on connect() when it closes its own session.
        if (!selfConnect && !selfWorker) {
            threadCv_.wait(lock, [this]() { return !connectActive_; });
        }
        if (thread_.joinable() && thread_.get_id() == current) {
            selfWorker = true;
            thread_.detach();
        } else if (thread_.joinable() && !joinInProgress_) {
            joinInProgress_ = true;
            worker = std::move(thread_);
        } else if (!selfWorker) {
            threadCv_.wait(lock, [this]() {
                return !workerActive_ && !joinInProgress_;
            });
        }
    }

    if (worker.joinable()) {
        worker.join();
        {
            std::lock_guard<std::mutex> lock(threadMutex_);
            joinInProgress_ = false;
        }
        threadCv_.notify_all();
    }
    if (!selfWorker) destroyPeer();

    const auto provider = callbackLifetimeProviderSnapshot();
    auto closeLease = provider ? provider() : std::shared_ptr<void>();
    auto handler = closeHandlerSnapshot();
    if (firstClose && (wasRunning || wasConnected) && handler &&
        (!provider || closeLease)) {
        handler(reason);
    }
}

void RakNetClient::requestStop() noexcept {
    closeRequested_.store(true);
    running_.store(false);
    connected_.store(false);
    shutdownPeer(0);
}

void RakNetClient::sendReliable(const std::vector<uint8_t>& payload) {
    if (payload.empty() || !connected_.load() || closeRequested_.load()) {
        return;
    }
    std::lock_guard<std::mutex> lock(nativeMutex_);
    if (!native_ || !native_->peer ||
        native_->peer->GetConnectionState(native_->target) !=
            RakNet::IS_CONNECTED) {
        return;
    }
    (void) native_->peer->Send(
        reinterpret_cast<const char*>(payload.data()),
        static_cast<int>(payload.size()),
        MEDIUM_PRIORITY,
        RELIABLE_ORDERED,
        0,
        native_->target,
        false
    );
}

void RakNetClient::runLoop() {
    while (running_.load()) {
        RakNet::Packet* nativePacket = nullptr;
        {
            std::lock_guard<std::mutex> lock(nativeMutex_);
            if (!native_ || !native_->peer || !native_->peer->IsActive()) {
                break;
            }
            nativePacket = native_->peer->Receive();
        }
        if (!nativePacket) {
            RakSleep(3);
            continue;
        }

        std::vector<uint8_t> packet(
            nativePacket->data,
            nativePacket->data + nativePacket->length
        );
        {
            std::lock_guard<std::mutex> lock(nativeMutex_);
            if (native_ && native_->peer) {
                native_->peer->DeallocatePacket(nativePacket);
            }
        }
        handleNativePacket(packet);

        const auto provider = callbackLifetimeProviderSnapshot();
        if (provider && !provider()) {
            running_.store(false);
            break;
        }
    }
}

void RakNetClient::handleNativePacket(const std::vector<uint8_t>& packet) {
    if (packet.empty()) return;
    const uint8_t id = packet.front();

    if (id == ID_CONNECTION_REQUEST_ACCEPTED) {
        bool expected = false;
        if (!connected_.compare_exchange_strong(expected, true)) return;
        connectedCallbackPending_.store(true);
        {
            std::lock_guard<std::mutex> lock(nativeMutex_);
            if (native_ && native_->peer) {
                mtu_.store(native_->peer->GetMTUSize(native_->target));
            }
        }
        const auto handler = connectedHandlerSnapshot();
        if (handler) handler();
        // Publish handshake success only after the callback completed. A
        // callback may intentionally close the accepted session and still
        // constitutes a successful handshake; a throwing callback never
        // reaches this store and is converted into worker failure instead.
        connectAccepted_.store(true);
        connectedCallbackPending_.store(false);
        threadCv_.notify_all();
        return;
    }

    if (id == ID_ALREADY_CONNECTED) {
        bool expected = false;
        if (connected_.compare_exchange_strong(expected, true)) {
            connectedCallbackPending_.store(true);
            const auto handler = connectedHandlerSnapshot();
            if (handler) handler();
            connectAccepted_.store(true);
            connectedCallbackPending_.store(false);
            threadCv_.notify_all();
        }
        return;
    }

    if (const auto failure = connectionFailureMessage(id)) {
        if (!connected_.load()) {
            setError(*failure);
            requestStop();
            threadCv_.notify_all();
            return;
        }
    }

    if (isDisconnectMessage(id)) {
        const bool hadSession = connected_.load();
        if (!hadSession && error().empty()) {
            setError("Connection closed during handshake: " +
                std::to_string(id));
        }
        requestStop();
        threadCv_.notify_all();
        const auto handler = closeHandlerSnapshot();
        if (hadSession && handler) handler(std::to_string(id));
        return;
    }

    if (id < ID_USER_PACKET_ENUM || !connected_.load()) return;
    const auto handler = encapsulatedHandlerSnapshot();
    if (handler) handler(packet);
}

bool RakNetClient::beginConnectActivity() {
    std::lock_guard<std::mutex> lock(threadMutex_);
    if (connectActive_ || workerActive_ || joinInProgress_) return false;
    connectActive_ = true;
    connectThreadId_ = std::this_thread::get_id();
    return true;
}

void RakNetClient::endConnectActivity() {
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        connectActive_ = false;
        connectThreadId_ = std::thread::id{};
    }
    threadCv_.notify_all();
}

void RakNetClient::shutdownPeer(unsigned int blockDuration) noexcept {
    try {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        if (native_ && native_->peer && native_->peer->IsActive()) {
            native_->peer->Shutdown(blockDuration);
        }
    } catch (...) {
    }
}

void RakNetClient::destroyPeer() noexcept {
    RakNet::RakPeerInterface* peer = nullptr;
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        if (native_) {
            peer = native_->peer;
            native_->peer = nullptr;
            native_->target = RakNet::UNASSIGNED_SYSTEM_ADDRESS;
        }
    }
    if (peer) {
        try {
            if (peer->IsActive()) peer->Shutdown(0);
        } catch (...) {
        }
        try {
            RakNet::RakPeerInterface::DestroyInstance(peer);
        } catch (...) {
        }
    }
}

void RakNetClient::endWorkerActivity() noexcept {
    running_.store(false);
    connected_.store(false);
    destroyPeer();
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        workerActive_ = false;
        workerThreadId_ = std::thread::id{};
    }
    threadCv_.notify_all();
}

void RakNetClient::failWorkerCallback(const char* detail) noexcept {
    const bool hadSession = connected_.load();
    connectAccepted_.store(false);
    connectedCallbackPending_.store(false);
    std::string reason;
    try {
        reason = "RakNet worker callback failure";
        if (detail != nullptr && *detail != '\0') {
            reason += ": ";
            reason += detail;
        }
    } catch (...) {
        // Shutting down the native worker is still mandatory even when the
        // diagnostic string itself cannot be allocated.
        reason.clear();
    }
    try {
        setError(reason);
    } catch (...) {
    }
    requestStop();
    threadCv_.notify_all();

    if (!hadSession) return;
    try {
        const auto handler = closeHandlerSnapshot();
        if (handler) handler(reason);
    } catch (...) {
        // A failing observer cannot be allowed to escape std::thread's entry
        // function. The native connection is already stopped and error() keeps
        // the original worker failure for diagnostics.
    }
}

void RakNetClient::setError(std::string error) {
    std::lock_guard<std::mutex> lock(errorMutex_);
    error_ = std::move(error);
}

RakNetClient::CallbackLifetimeProvider
RakNetClient::callbackLifetimeProviderSnapshot() const {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return callbackLifetimeProvider_;
}

RakNetClient::ConnectedHandler RakNetClient::connectedHandlerSnapshot() const {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return connectedHandler_;
}

RakNetClient::CloseHandler RakNetClient::closeHandlerSnapshot() const {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return closeHandler_;
}

RakNetClient::EncapsulatedHandler
RakNetClient::encapsulatedHandlerSnapshot() const {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return encapsulatedHandler_;
}

} // namespace bedrock
