#include <bedrock/server/RakNetServer.hpp>

#include <MessageIdentifiers.h>
#include <RakNetStatistics.h>
#include <RakNetTypes.h>
#include <RakPeerInterface.h>
#include <RakSleep.h>

#include <algorithm>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace bedrock {

struct RakNetServer::NativeState {
    RakNet::RakPeerInterface* peer = nullptr;
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

bool isDisconnectMessage(uint8_t id) {
    return id == ID_DISCONNECTION_NOTIFICATION ||
        id == ID_CONNECTION_LOST ||
        id == ID_INCOMPATIBLE_PROTOCOL_VERSION;
}

} // namespace

const char* rakNetServerSendStatusName(
    RakNetServerSendStatus status
) noexcept {
    switch (status) {
        case RakNetServerSendStatus::Accepted: return "accepted";
        case RakNetServerSendStatus::EmptyPayload: return "empty_payload";
        case RakNetServerSendStatus::ServerStopped: return "server_stopped";
        case RakNetServerSendStatus::UnknownPeer: return "unknown_peer";
        case RakNetServerSendStatus::NativeUnavailable:
            return "native_unavailable";
        case RakNetServerSendStatus::NotConnected: return "not_connected";
        case RakNetServerSendStatus::Rejected: return "rejected";
    }
    return "unknown";
}

RakNetServer::RakNetServer(RakNetServerOptions options)
    : options_(std::move(options)),
      native_(std::make_unique<NativeState>()) {}

RakNetServer::~RakNetServer() {
    close();
    destroyPeer();
}

void RakNetServer::listen() {
    if (running_.load()) return;
    joinWorkerIfExternal();
    destroyPeer();

    auto* peer = RakNet::RakPeerInterface::GetInstance();
    if (!peer) throw std::runtime_error("Unable to allocate RakPeer");

    const int maximumConnections = options_.maxPlayers > 0
        ? options_.maxPlayers
        : 3;
    peer->SetTimeoutTime(
        static_cast<RakNet::TimeMS>(std::max(options_.timeoutMs, 1'000)),
        RakNet::UNASSIGNED_SYSTEM_ADDRESS
    );
    peer->SetMaximumIncomingConnections(
        static_cast<unsigned short>(maximumConnections)
    );
    peer->allowClientsWithOlderVersion = true;
    if (options_.protocolVersion >= 0) {
        peer->SetProtocolVersion(options_.protocolVersion);
    }
    if (options_.serverGuid != 0) {
        peer->SetMyGUID(RakNet::RakNetGUID(options_.serverGuid));
    }

    const char* host = options_.host.empty() || options_.host == "0.0.0.0"
        ? nullptr
        : options_.host.c_str();
    RakNet::SocketDescriptor descriptor(options_.port, host);
    descriptor.socketFamily = AF_INET;
    const auto startup = peer->Startup(
        static_cast<unsigned int>(maximumConnections),
        &descriptor,
        1
    );
    if (startup != RakNet::RAKNET_STARTED) {
        RakNet::RakPeerInterface::DestroyInstance(peer);
        throw std::runtime_error(
            std::string("RakNet server startup failed: ") +
            startupError(startup)
        );
    }

    if (!options_.advertisement.empty()) {
        peer->SetOfflinePingResponse(
            options_.advertisement.data(),
            static_cast<unsigned int>(options_.advertisement.size())
        );
    }
    boundPort_.store(peer->GetMyBoundAddress().GetPort());
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        native_->peer = peer;
    }
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        peers_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        scheduledPeerCloses_.clear();
        shutdownScheduled_ = false;
        advertisementUpdatesEnabled_ = true;
        nextAdvertisementUpdate_ = std::chrono::steady_clock::now() +
            std::chrono::seconds(1);
        scheduledCloseOrder_ = 0;
    }

    running_.store(true);
    try {
        thread_ = std::thread([this]() {
            try {
                runLoop();
            } catch (const std::exception& error) {
                shutdownAfterWorkerError(error.what());
            } catch (...) {
                shutdownAfterWorkerError("unknown native exception");
            }
        });
    } catch (...) {
        running_.store(false);
        peer->Shutdown(0);
        destroyPeer();
        throw;
    }
}

void RakNetServer::close() {
    closeAfter(std::chrono::milliseconds(0));
}

void RakNetServer::closeAfter(std::chrono::milliseconds delay) {
    if (delay.count() < 0) delay = std::chrono::milliseconds(0);
    if (running_.load()) {
        const auto deadline = std::chrono::steady_clock::now() + delay;
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!shutdownScheduled_ || deadline < shutdownDeadline_) {
            shutdownScheduled_ = true;
            shutdownDeadline_ = deadline;
        }
        advertisementUpdatesEnabled_ = false;
    }
    joinWorkerIfExternal();
}

void RakNetServer::closePeer(
    const RakNetServerPeer& peer,
    bool silent
) {
    if (!running_.load()) return;
    closePeerNow(peer, silent);
}

void RakNetServer::closePeerAfter(
    const RakNetServerPeer& peer,
    std::chrono::milliseconds delay,
    bool silent
) {
    if (!running_.load()) return;
    if (delay.count() < 0) delay = std::chrono::milliseconds(0);
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    scheduledPeerCloses_.push_back({
        std::chrono::steady_clock::now() + delay,
        peer,
        silent,
        scheduledCloseOrder_++
    });
}

void RakNetServer::setAdvertisement(std::string advertisement) {
    options_.advertisement = std::move(advertisement);
    std::lock_guard<std::mutex> lock(nativeMutex_);
    if (!native_ || !native_->peer || !native_->peer->IsActive()) return;
    native_->peer->SetOfflinePingResponse(
        options_.advertisement.data(),
        static_cast<unsigned int>(options_.advertisement.size())
    );
}

RakNetServerSendResult RakNetServer::sendReliable(
    const RakNetServerPeer& peer,
    const std::vector<uint8_t>& payload,
    bool immediate
) {
    RakNetServerSendResult result;
    if (payload.empty()) {
        result.status = RakNetServerSendStatus::EmptyPayload;
        return result;
    }
    if (!running_.load()) {
        result.status = RakNetServerSendStatus::ServerStopped;
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        const auto found = peers_.find(peerKey(peer));
        if (found == peers_.end() ||
            found->second.clientGuid != peer.clientGuid) {
            result.status = RakNetServerSendStatus::UnknownPeer;
            return result;
        }
    }

    std::lock_guard<std::mutex> lock(nativeMutex_);
    if (!native_ || !native_->peer || !native_->peer->IsActive()) {
        result.status = RakNetServerSendStatus::NativeUnavailable;
        return result;
    }
    const RakNet::RakNetGUID guid(peer.clientGuid);
    result.connectionState = static_cast<int>(
        native_->peer->GetConnectionState(guid)
    );
    if (result.connectionState != static_cast<int>(RakNet::IS_CONNECTED)) {
        result.status = RakNetServerSendStatus::NotConnected;
        return result;
    }
    result.receipt = native_->peer->Send(
        reinterpret_cast<const char*>(payload.data()),
        static_cast<int>(payload.size()),
        immediate ? IMMEDIATE_PRIORITY : MEDIUM_PRIORITY,
        RELIABLE_ORDERED,
        0,
        guid,
        false
    );
    result.status = result.receipt == 0
        ? RakNetServerSendStatus::Rejected
        : RakNetServerSendStatus::Accepted;
    return result;
}

RakNetServerPeerStatistics RakNetServer::peerStatistics(
    const RakNetServerPeer& peer
) const {
    RakNetServerPeerStatistics result;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        const auto found = peers_.find(peerKey(peer));
        result.peerKnown = found != peers_.end() &&
            found->second.clientGuid == peer.clientGuid;
    }
    if (!result.peerKnown) return result;

    std::lock_guard<std::mutex> lock(nativeMutex_);
    if (!native_ || !native_->peer || !native_->peer->IsActive()) {
        return result;
    }
    result.nativeActive = true;
    const RakNet::RakNetGUID guid(peer.clientGuid);
    result.connectionState = static_cast<int>(
        native_->peer->GetConnectionState(guid)
    );
    const auto address = native_->peer->GetSystemAddressFromGuid(guid);
    if (address == RakNet::UNASSIGNED_SYSTEM_ADDRESS) return result;

    RakNet::RakNetStatistics statistics {};
    if (!native_->peer->GetStatistics(address, &statistics)) return result;
    result.statisticsAvailable = true;
    result.userMessageBytesPushed =
        statistics.runningTotal[RakNet::USER_MESSAGE_BYTES_PUSHED];
    result.userMessageBytesSent =
        statistics.runningTotal[RakNet::USER_MESSAGE_BYTES_SENT];
    result.userMessageBytesResent =
        statistics.runningTotal[RakNet::USER_MESSAGE_BYTES_RESENT];
    result.actualBytesSent =
        statistics.runningTotal[RakNet::ACTUAL_BYTES_SENT];
    result.actualBytesReceived =
        statistics.runningTotal[RakNet::ACTUAL_BYTES_RECEIVED];
    for (int priority = 0; priority < NUMBER_OF_PRIORITIES; ++priority) {
        result.sendBufferMessages += statistics.messageInSendBuffer[priority];
        result.sendBufferBytes += static_cast<uint64_t>(
            statistics.bytesInSendBuffer[priority]
        );
    }
    result.resendBufferMessages = statistics.messagesInResendBuffer;
    result.resendBufferBytes = statistics.bytesInResendBuffer;
    return result;
}

void RakNetServer::runLoop() {
    while (running_.load()) {
        if (!processLifecycleDeadlines()) break;

        bool receivedAny = false;
        std::size_t processed = 0;
        constexpr std::size_t maxReceiveBatch = 256;
        while (running_.load() && processed < maxReceiveBatch) {
            RakNet::Packet* nativePacket = nullptr;
            RakNetServerPeer packetPeer;
            {
                std::lock_guard<std::mutex> lock(nativeMutex_);
                if (!native_ || !native_->peer ||
                    !native_->peer->IsActive()) {
                    running_.store(false);
                    break;
                }
                nativePacket = native_->peer->Receive();
                if (nativePacket) {
                    char address[128] {};
                    nativePacket->systemAddress.ToString(
                        false,
                        address,
                        '/'
                    );
                    packetPeer.address = address;
                    packetPeer.port = nativePacket->systemAddress.GetPort();
                    packetPeer.clientGuid = nativePacket->guid.g;
                    packetPeer.mtu = native_->peer->GetMTUSize(
                        nativePacket->systemAddress
                    );
                }
            }
            if (!nativePacket) break;
            receivedAny = true;
            ++processed;

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
            try {
                handleNativePacket(packetPeer, packet);
            } catch (const std::exception& error) {
                failPeerAfterWorkerError(packetPeer, error.what());
            } catch (...) {
                failPeerAfterWorkerError(
                    packetPeer,
                    "unknown native exception"
                );
            }
        }
        if (!receivedAny) RakSleep(5);
    }
}

bool RakNetServer::processLifecycleDeadlines() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<ScheduledPeerClose> due;
    bool shutdownDue = false;
    bool advertisementDue = false;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        shutdownDue = shutdownScheduled_ && shutdownDeadline_ <= now;
        if (shutdownDue) {
            shutdownScheduled_ = false;
            scheduledPeerCloses_.clear();
        } else {
            auto it = scheduledPeerCloses_.begin();
            while (it != scheduledPeerCloses_.end()) {
                if (it->deadline <= now) {
                    due.push_back(*it);
                    it = scheduledPeerCloses_.erase(it);
                } else {
                    ++it;
                }
            }
            if (advertisementUpdatesEnabled_ &&
                nextAdvertisementUpdate_ <= now) {
                advertisementDue = true;
                do {
                    nextAdvertisementUpdate_ += std::chrono::seconds(1);
                } while (nextAdvertisementUpdate_ <= now);
            }
        }
    }

    if (shutdownDue) {
        finishShutdown();
        return false;
    }

    std::sort(due.begin(), due.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.deadline != rhs.deadline) return lhs.deadline < rhs.deadline;
        return lhs.order < rhs.order;
    });
    for (const auto& item : due) {
        try {
            closePeerNow(item.peer, item.silent);
        } catch (const std::exception& error) {
            // closePeerNow removes the peer before invoking its observer. A
            // failing close observer must not tear down unrelated sessions.
            reportWorkerError(item.peer, error.what());
        } catch (...) {
            reportWorkerError(
                item.peer,
                "scheduled peer close raised an unknown exception"
            );
        }
    }

    if (advertisementDue) {
        try {
            const auto provider = advertisementProviderSnapshot();
            if (provider) setAdvertisement(provider());
        } catch (const std::exception& error) {
            reportWorkerError({}, error.what());
        } catch (...) {
            reportWorkerError(
                {},
                "advertisement provider raised an unknown exception"
            );
        }
    }
    return running_.load();
}

bool RakNetServer::closePeerNow(
    const RakNetServerPeer& peer,
    bool silent
) {
    std::optional<RakNetServerPeer> closedPeer;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        const auto found = peers_.find(peerKey(peer));
        if (found == peers_.end() ||
            found->second.clientGuid != peer.clientGuid) {
            return false;
        }
        closedPeer = found->second;
        peers_.erase(found);
    }

    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        if (native_ && native_->peer) {
            native_->peer->CloseConnection(
                RakNet::RakNetGUID(peer.clientGuid),
                !silent,
                0,
                LOW_PRIORITY
            );
        }
    }

    // Close is a local state transition. Do not wait for an ACK from a peer
    // that may already be gone or congested before tearing down the session.
    const auto handler = closeConnectionHandlerSnapshot();
    if (closedPeer && handler) handler(*closedPeer);
    return true;
}

void RakNetServer::finishShutdown() {
    running_.store(false);
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        if (native_ && native_->peer && native_->peer->IsActive()) {
            native_->peer->Shutdown(300);
        }
    }
    std::lock_guard<std::mutex> lock(peersMutex_);
    peers_.clear();
}

void RakNetServer::joinWorkerIfExternal() {
    if (onWorkerThread()) return;
    std::lock_guard<std::mutex> lock(joinMutex_);
    if (thread_.joinable()) thread_.join();
    if (!running_.load()) destroyPeer();
}

void RakNetServer::destroyPeer() noexcept {
    RakNet::RakPeerInterface* peer = nullptr;
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        if (native_) {
            peer = native_->peer;
            native_->peer = nullptr;
        }
    }
    if (!peer) return;
    try {
        if (peer->IsActive()) peer->Shutdown(0);
    } catch (...) {
    }
    try {
        RakNet::RakPeerInterface::DestroyInstance(peer);
    } catch (...) {
    }
}

void RakNetServer::handleNativePacket(
    const RakNetServerPeer& packetPeer,
    const std::vector<uint8_t>& packet
) {
    if (packet.empty()) return;
    const uint8_t id = packet.front();
    if (id == ID_NEW_INCOMING_CONNECTION) {
        const auto rawHandler = rawPacketHandlerSnapshot();
        if (rawHandler) rawHandler(packetPeer, packet);
        bool inserted = false;
        {
            std::lock_guard<std::mutex> lock(peersMutex_);
            const auto key = peerKey(packetPeer);
            const auto found = peers_.find(key);
            if (found == peers_.end() ||
                found->second.clientGuid != packetPeer.clientGuid) {
                peers_[key] = packetPeer;
                inserted = true;
            }
        }
        if (inserted) {
            const auto handler = openConnectionHandlerSnapshot();
            if (handler) handler(packetPeer);
        }
        return;
    }

    if (isDisconnectMessage(id)) {
        std::optional<RakNetServerPeer> closedPeer;
        {
            std::lock_guard<std::mutex> lock(peersMutex_);
            const auto found = peers_.find(peerKey(packetPeer));
            if (found != peers_.end() &&
                found->second.clientGuid == packetPeer.clientGuid) {
                closedPeer = found->second;
                peers_.erase(found);
            }
        }
        if (closedPeer) {
            std::exception_ptr rawFailure;
            try {
                const auto rawHandler = rawPacketHandlerSnapshot();
                if (rawHandler) rawHandler(packetPeer, packet);
            } catch (...) {
                rawFailure = std::current_exception();
            }

            // The peer is deliberately removed before observers run. Always
            // reconcile the logical close even when the preceding raw observer
            // fails; failPeerAfterWorkerError cannot rediscover an erased peer.
            try {
                const auto handler = closeConnectionHandlerSnapshot();
                if (handler) handler(*closedPeer);
            } catch (const std::exception& closeError) {
                if (!rawFailure) throw;
                reportWorkerError(*closedPeer, closeError.what());
            } catch (...) {
                if (!rawFailure) throw;
                reportWorkerError(
                    *closedPeer,
                    "disconnect close observer raised an unknown exception"
                );
            }
            if (rawFailure) std::rethrow_exception(rawFailure);
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        const auto found = peers_.find(peerKey(packetPeer));
        if (found == peers_.end() ||
            found->second.clientGuid != packetPeer.clientGuid) {
            return;
        }
    }
    const auto rawHandler = rawPacketHandlerSnapshot();
    if (rawHandler) rawHandler(packetPeer, packet);

    if (id < ID_USER_PACKET_ENUM) return;
    const auto handler = encapsulatedHandlerSnapshot();
    if (handler) handler(packetPeer, packet);
}

void RakNetServer::reportWorkerError(
    const RakNetServerPeer& peer,
    const char* detail
) noexcept {
    try {
        const auto handler = workerErrorHandlerSnapshot();
        if (handler) {
            handler(
                peer,
                detail == nullptr || *detail == '\0'
                    ? "unknown native exception"
                    : detail
            );
        }
    } catch (...) {
        // Failure reporting is observational and must not cross the same
        // worker boundary that it is protecting.
    }
}

void RakNetServer::shutdownAfterWorkerError(const char* detail) noexcept {
    // This is the last-resort boundary around the polling thread. Do not rely
    // on closeAfter() to do the cleanup: running_ is already false here, so a
    // later closeAfter() only joins the worker and would otherwise leave the
    // native socket and logical peer table alive until destruction.
    running_.store(false);
    reportWorkerError({}, detail);

    try {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        scheduledPeerCloses_.clear();
        shutdownScheduled_ = false;
        advertisementUpdatesEnabled_ = false;
    } catch (...) {
        reportWorkerError({}, "worker shutdown could not clear lifecycle state");
    }

    std::unordered_map<std::string, RakNetServerPeer> closedPeers;
    try {
        std::lock_guard<std::mutex> lock(peersMutex_);
        closedPeers.swap(peers_);
    } catch (...) {
        reportWorkerError({}, "worker shutdown could not detach peer state");
    }

    // The polling thread is distinct from RakNet's internal threads, so it is
    // safe to stop and destroy the native peer here. Never try to join our own
    // std::thread from this path; an external close/destructor will join it.
    destroyPeer();

    CloseConnectionHandler closeHandler;
    try {
        closeHandler = closeConnectionHandlerSnapshot();
    } catch (...) {
        reportWorkerError({}, "worker shutdown could not snapshot close handler");
    }
    if (!closeHandler) return;
    for (const auto& entry : closedPeers) {
        try {
            closeHandler(entry.second);
        } catch (const std::exception& error) {
            reportWorkerError(entry.second, error.what());
        } catch (...) {
            reportWorkerError(
                entry.second,
                "peer reconciliation raised an unknown exception"
            );
        }
    }
}

void RakNetServer::failPeerAfterWorkerError(
    const RakNetServerPeer& peer,
    const char* detail
) noexcept {
    reportWorkerError(peer, detail);
    try {
        // A callback exception invalidates the ordering assumptions of this
        // RakNet session. Close only that peer and keep the listener alive.
        if (!closePeerNow(peer, true)) {
            // A callback on ID_NEW_INCOMING_CONNECTION can fail before the
            // peer is inserted into peers_. It is still a live native RakNet
            // connection and must not be left occupying a server slot.
            std::lock_guard<std::mutex> lock(nativeMutex_);
            if (native_ && native_->peer) {
                native_->peer->CloseConnection(
                    RakNet::RakNetGUID(peer.clientGuid),
                    false,
                    0,
                    LOW_PRIORITY
                );
            }
        }
    } catch (const std::exception& closeError) {
        reportWorkerError(peer, closeError.what());
    } catch (...) {
        reportWorkerError(peer, "peer cleanup raised an unknown exception");
    }
}

std::string RakNetServer::peerKey(const RakNetServerPeer& peer) {
    return peer.address + ":" + std::to_string(peer.port);
}

RakNetServer::OpenConnectionHandler
RakNetServer::openConnectionHandlerSnapshot() const {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return openConnectionHandler_;
}

RakNetServer::CloseConnectionHandler
RakNetServer::closeConnectionHandlerSnapshot() const {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return closeConnectionHandler_;
}

RakNetServer::RawPacketHandler
RakNetServer::rawPacketHandlerSnapshot() const {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return rawPacketHandler_;
}

RakNetServer::EncapsulatedHandler
RakNetServer::encapsulatedHandlerSnapshot() const {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return encapsulatedHandler_;
}

RakNetServer::WorkerErrorHandler
RakNetServer::workerErrorHandlerSnapshot() const {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return workerErrorHandler_;
}

RakNetServer::AdvertisementProvider
RakNetServer::advertisementProviderSnapshot() const {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return advertisementProvider_;
}

} // namespace bedrock
