#include <bedrock/client/RakNetClient.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>
#include <thread>

namespace bedrock {

namespace {

constexpr uint8_t ID_OPEN_CONNECTION_REQUEST_1 = 0x05;
constexpr uint8_t ID_OPEN_CONNECTION_REPLY_1 = 0x06;
constexpr uint8_t ID_OPEN_CONNECTION_REQUEST_2 = 0x07;
constexpr uint8_t ID_OPEN_CONNECTION_REPLY_2 = 0x08;
constexpr uint8_t ID_CONNECTED_PING = 0x00;
constexpr uint8_t ID_CONNECTED_PONG = 0x03;
constexpr uint8_t ID_CONNECTION_REQUEST = 0x09;
constexpr uint8_t ID_CONNECTION_REQUEST_ACCEPTED = 0x10;
constexpr uint8_t ID_NEW_INCOMING_CONNECTION = 0x13;
constexpr uint8_t ID_DISCONNECTION_NOTIFICATION = 0x15;
constexpr uint8_t ID_CONNECTION_LOST = 0x16;
constexpr uint8_t ID_CONNECTION_BANNED = 0x17;
constexpr uint8_t ID_INCOMPATIBLE_PROTOCOL_VERSION = 0x19;
constexpr uint8_t ID_NACK = 0xa0;
constexpr uint8_t ID_ACK = 0xc0;

constexpr int UDP_IPV4_HEADER_SIZE = 28;

constexpr uint8_t RAKNET_MAGIC[16] = {
    0x00, 0xff, 0xff, 0x00,
    0xfe, 0xfe, 0xfe, 0xfe,
    0xfd, 0xfd, 0xfd, 0xfd,
    0x12, 0x34, 0x56, 0x78
};

uint64_t makeGuid() {
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::random_device rd;
    return (static_cast<uint64_t>(now) << 16u) ^ static_cast<uint64_t>(rd()) ^ 0xC11E47BADC0DEull;
}

int64_t nowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void writeU16BE(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    out.push_back(static_cast<uint8_t>(value & 0xffu));
}

void writeU32BE(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    out.push_back(static_cast<uint8_t>(value & 0xffu));
}

void writeU64BE(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xffu));
    }
}

void writeTriadLE(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
}

uint16_t readU16BE(const std::vector<uint8_t>& data, std::size_t& offset) {
    if (offset + 2 > data.size()) {
        throw std::runtime_error("readU16BE out of range");
    }
    uint16_t value =
        static_cast<uint16_t>(static_cast<uint16_t>(data[offset]) << 8u) |
        static_cast<uint16_t>(data[offset + 1]);
    offset += 2;
    return value;
}

uint32_t readU32BE(const std::vector<uint8_t>& data, std::size_t& offset) {
    if (offset + 4 > data.size()) {
        throw std::runtime_error("readU32BE out of range");
    }
    uint32_t value =
        (static_cast<uint32_t>(data[offset]) << 24u) |
        (static_cast<uint32_t>(data[offset + 1]) << 16u) |
        (static_cast<uint32_t>(data[offset + 2]) << 8u) |
        static_cast<uint32_t>(data[offset + 3]);
    offset += 4;
    return value;
}

uint64_t readU64BE(const std::vector<uint8_t>& data, std::size_t& offset) {
    if (offset + 8 > data.size()) {
        throw std::runtime_error("readU64BE out of range");
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8u) | static_cast<uint64_t>(data[offset + static_cast<std::size_t>(i)]);
    }
    offset += 8;
    return value;
}

uint32_t readTriadLE(const std::vector<uint8_t>& data, std::size_t& offset) {
    if (offset + 3 > data.size()) {
        throw std::runtime_error("readTriadLE out of range");
    }
    uint32_t value =
        static_cast<uint32_t>(data[offset]) |
        (static_cast<uint32_t>(data[offset + 1]) << 8u) |
        (static_cast<uint32_t>(data[offset + 2]) << 16u);
    offset += 3;
    return value;
}

bool hasMagic(const std::vector<uint8_t>& data, std::size_t offset) {
    if (offset + 16 > data.size()) {
        return false;
    }
    return std::equal(std::begin(RAKNET_MAGIC), std::end(RAKNET_MAGIC), data.begin() + static_cast<std::ptrdiff_t>(offset));
}

void appendMagic(std::vector<uint8_t>& out) {
    out.insert(out.end(), std::begin(RAKNET_MAGIC), std::end(RAKNET_MAGIC));
}

void writeRakNetAddressIPv4(std::vector<uint8_t>& out, const std::string& ipText, uint16_t port) {
    in_addr addr {};
    if (inet_pton(AF_INET, ipText.c_str(), &addr) != 1) {
        throw std::runtime_error("invalid IPv4 address: " + ipText);
    }
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(&addr.s_addr);
    out.push_back(4);
    out.push_back(static_cast<uint8_t>(~ip[0]));
    out.push_back(static_cast<uint8_t>(~ip[1]));
    out.push_back(static_cast<uint8_t>(~ip[2]));
    out.push_back(static_cast<uint8_t>(~ip[3]));
    writeU16BE(out, port);
}

std::vector<uint8_t> buildOpenConnectionRequest1(int mtu, int protocolVersion) {
    std::vector<uint8_t> out;
    out.push_back(ID_OPEN_CONNECTION_REQUEST_1);
    appendMagic(out);
    out.push_back(static_cast<uint8_t>(protocolVersion));

    const int payloadSize = mtu - UDP_IPV4_HEADER_SIZE;
    if (payloadSize < static_cast<int>(out.size())) {
        throw std::runtime_error("MTU too small");
    }
    out.resize(static_cast<std::size_t>(payloadSize), 0);
    return out;
}

std::vector<uint8_t> buildOpenConnectionRequest2(
    const std::string& serverIp,
    uint16_t serverPort,
    int mtu,
    uint64_t clientGuid,
    bool serverSecurity,
    uint32_t securityCookie
) {
    std::vector<uint8_t> out;
    out.push_back(ID_OPEN_CONNECTION_REQUEST_2);
    appendMagic(out);

    if (serverSecurity) {
        writeU32BE(out, securityCookie);
        out.push_back(0x00); // client supports security = false
    }

    writeRakNetAddressIPv4(out, serverIp, serverPort);
    writeU16BE(out, static_cast<uint16_t>(mtu));
    writeU64BE(out, clientGuid);
    return out;
}

std::vector<uint8_t> buildAck(uint32_t sequence) {
    std::vector<uint8_t> out;
    out.push_back(ID_ACK);
    writeU16BE(out, 1);
    out.push_back(1);
    writeTriadLE(out, sequence);
    return out;
}

std::vector<uint32_t> readAckSequences(const std::vector<uint8_t>& data) {
    std::vector<uint32_t> sequences;
    if (data.size() < 3) return sequences;
    std::size_t offset = 1;
    uint16_t count = readU16BE(data, offset);
    for (uint16_t i = 0; i < count && offset < data.size(); ++i) {
        uint8_t single = data[offset++];
        if (single) {
            sequences.push_back(readTriadLE(data, offset));
        } else {
            uint32_t start = readTriadLE(data, offset);
            uint32_t end = readTriadLE(data, offset);
            if (end >= start && end - start < 4096) {
                for (uint32_t seq = start; seq <= end; ++seq) {
                    sequences.push_back(seq);
                }
            }
        }
    }
    return sequences;
}

bool isReliable(uint8_t reliability) {
    return reliability == 2 || reliability == 3 || reliability == 4 ||
        reliability == 6 || reliability == 7;
}

bool isSequenced(uint8_t reliability) {
    return reliability == 1 || reliability == 4;
}

bool isOrdered(uint8_t reliability) {
    return reliability == 3 || reliability == 4 || reliability == 7;
}

struct ParsedFrame {
    uint8_t reliability = 0;
    uint32_t orderedIndex = 0;
    bool ordered = false;
    bool split = false;
    uint32_t splitCount = 0;
    uint16_t splitId = 0;
    uint32_t splitIndex = 0;
    std::vector<uint8_t> payload;
};

std::vector<ParsedFrame> parseConnectedDatagram(const std::vector<uint8_t>& data, uint32_t& sequence) {
    std::vector<ParsedFrame> frames;
    if (data.empty() || data[0] < 0x80 || data[0] > 0x8f) return frames;

    std::size_t offset = 1;
    sequence = readTriadLE(data, offset);

    while (offset < data.size()) {
        ParsedFrame frame;
        uint8_t flags = data[offset++];
        uint8_t reliability = static_cast<uint8_t>((flags & 0xe0u) >> 5u);
        frame.reliability = reliability;
        frame.split = (flags & 0x10u) != 0;

        uint16_t bitLength = readU16BE(data, offset);
        std::size_t byteLength = (static_cast<std::size_t>(bitLength) + 7u) / 8u;

        if (isReliable(reliability)) (void) readTriadLE(data, offset);
        if (isSequenced(reliability)) (void) readTriadLE(data, offset);
        if (isOrdered(reliability)) {
            frame.ordered = true;
            frame.orderedIndex = readTriadLE(data, offset);
            if (offset >= data.size()) throw std::runtime_error("ordered channel out of range");
            ++offset;
        }
        if (frame.split) {
            frame.splitCount = readU32BE(data, offset);
            frame.splitId = readU16BE(data, offset);
            frame.splitIndex = readU32BE(data, offset);
        }
        if (frame.split && byteLength == 0) {
            byteLength = data.size() - offset;
        }
        if (offset + byteLength > data.size()) {
            throw std::runtime_error("frame payload out of range");
        }
        frame.payload.assign(
            data.begin() + static_cast<std::ptrdiff_t>(offset),
            data.begin() + static_cast<std::ptrdiff_t>(offset + byteLength)
        );
        offset += byteLength;
        frames.push_back(std::move(frame));
    }
    return frames;
}

} // namespace

RakNetClient::RakNetClient(RakNetClientOptions options)
    : options_(std::move(options)),
      mtu_(options_.mtu) {
    if (options_.clientGuid == 0) {
        options_.clientGuid = makeGuid();
    }
}

RakNetClient::~RakNetClient() {
    close();
}

RakNetClient::RakNetClient(RakNetClient&& other) {
    *this = std::move(other);
}

RakNetClient& RakNetClient::operator=(RakNetClient&& other) {
    if (this == &other) return *this;

    // A worker or in-flight connect captures the address of its original
    // RakNetClient. Relocating that state would leave the old stack accessing
    // moved members (and moving from its own callback cannot synchronously
    // join). Under the usual no-concurrent-access rule for move operations,
    // locking both activity records makes this rejection atomic with respect
    // to worker/connect admission.
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
    const bool sourceWasClosed = other.closeRequested_.load();
    try { close(); } catch (...) {}
    try { other.close(); } catch (...) {}
    options_ = std::move(other.options_);
    {
        std::scoped_lock lock(socketMutex_, other.socketMutex_);
        socket_ = other.socket_;
        other.socket_ = -1;
    }
    localPort_ = other.localPort_;
    mtu_ = other.mtu_;
    target_ = other.target_;
    targetLen_ = other.targetLen_;
    running_.store(other.running_.load());
    connected_.store(other.connected_.load());
    // A fresh, never-closed client remains connectable after a move. An
    // explicitly closed source remains single-use after relocation.
    closeRequested_.store(sourceWasClosed);
    error_ = std::move(other.error_);
    {
        std::scoped_lock lock(callbackMutex_, other.callbackMutex_);
        connectedHandler_ = std::move(other.connectedHandler_);
        closeHandler_ = std::move(other.closeHandler_);
        encapsulatedHandler_ = std::move(other.encapsulatedHandler_);
        callbackLifetimeProvider_ = std::move(other.callbackLifetimeProvider_);
        beforeRunningCommitTestHook_ =
            std::move(other.beforeRunningCommitTestHook_);
    }
    outgoingSequence_ = other.outgoingSequence_;
    reliableIndex_ = other.reliableIndex_;
    orderedIndex_ = other.orderedIndex_;
    outgoingSplitId_ = other.outgoingSplitId_;
    splits_ = std::move(other.splits_);
    pendingInboundPayloads_ = std::move(other.pendingInboundPayloads_);
    nextInboundOrderedIndex_ = other.nextInboundOrderedIndex_;
    pendingOrderedPayloads_ = std::move(other.pendingOrderedPayloads_);
    receivedDatagramSequences_ = std::move(other.receivedDatagramSequences_);
    sentReliableDatagrams_ = std::move(other.sentReliableDatagrams_);
    return *this;
}

bool RakNetClient::connect() {
    if (running_) return true;
    if (!beginConnectActivity()) return running_.load();
    auto connectActivity = std::unique_ptr<void, std::function<void(void*)>>(
        this,
        [this](void*) { endConnectActivity(); }
    );
    if (closeRequested_.load()) return false;

    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* res = nullptr;
    const auto portString = std::to_string(options_.port);
    // raknet-native constructs its SystemAddress with IPv4 explicitly. RakNet
    // treats the IPv6 loopback as a special case and maps it to IPv4 loopback
    // before starting the AF_INET connection.
    const std::string resolvedHost =
        options_.host == "::1" ? "127.0.0.1" : options_.host;
    int gai = getaddrinfo(resolvedHost.c_str(), portString.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        error_ = std::string("getaddrinfo failed: ") + gai_strerror(gai);
        return false;
    }

    const int createdSocket = ::socket(
        res->ai_family,
        res->ai_socktype,
        res->ai_protocol
    );
    if (createdSocket < 0) {
        freeaddrinfo(res);
        error_ = "socket failed";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        socket_ = createdSocket;
    }
    if (closeRequested_.load()) {
        close();
        return false;
    }

    std::memcpy(target_.data(), res->ai_addr, static_cast<std::size_t>(res->ai_addrlen));
    targetLen_ = static_cast<int>(res->ai_addrlen);
    auto targetSockaddr = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    const std::string targetIp = inet_ntoa(targetSockaddr.sin_addr);
    freeaddrinfo(res);

    sockaddr_in local {};
    socklen_t localLen = sizeof(local);
    if (getsockname(createdSocket, reinterpret_cast<sockaddr*>(&local), &localLen) == 0) {
        localPort_ = ntohs(local.sin_port);
    }

    auto req1 = buildOpenConnectionRequest1(options_.mtu, options_.protocolVersion);
    sendToTarget(req1);

    std::vector<uint8_t> reply(4096);
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(createdSocket, &readfds);
    timeval timeout {};
    timeout.tv_sec = options_.timeoutMs / 1000;
    timeout.tv_usec = (options_.timeoutMs % 1000) * 1000;
    int ready = select(createdSocket + 1, &readfds, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        error_ = "timeout waiting for OpenConnectionReply1";
        close();
        return false;
    }
    ssize_t received = recvfrom(createdSocket, reply.data(), reply.size(), 0, nullptr, nullptr);
    if (received <= 0) {
        error_ = "failed reading OpenConnectionReply1";
        close();
        return false;
    }
    reply.resize(static_cast<std::size_t>(received));

    bool serverSecurity = false;
    uint32_t securityCookie = 0;

    try {
        std::size_t offset = 0;
        if (reply[offset++] != ID_OPEN_CONNECTION_REPLY_1 || !hasMagic(reply, offset)) {
            error_ = "invalid OpenConnectionReply1";
            close();
            return false;
        }
        offset += 16;
        (void) readU64BE(reply, offset);

        if (offset >= reply.size()) {
            error_ = "invalid OpenConnectionReply1 security flag";
            close();
            return false;
        }

        serverSecurity = reply[offset++] != 0;
        if (serverSecurity) {
            securityCookie = readU32BE(reply, offset);
        }

        mtu_ = static_cast<int>(readU16BE(reply, offset));
    } catch (const std::exception& e) {
        error_ = e.what();
        close();
        return false;
    }

    auto req2 = buildOpenConnectionRequest2(
        targetIp,
        options_.port,
        mtu_,
        options_.clientGuid,
        serverSecurity,
        securityCookie
    );
    sendToTarget(req2);

    reply.assign(4096, 0);
    FD_ZERO(&readfds);
    FD_SET(createdSocket, &readfds);
    timeout.tv_sec = options_.timeoutMs / 1000;
    timeout.tv_usec = (options_.timeoutMs % 1000) * 1000;
    ready = select(createdSocket + 1, &readfds, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        error_ = "timeout waiting for OpenConnectionReply2";
        close();
        return false;
    }
    received = recvfrom(createdSocket, reply.data(), reply.size(), 0, nullptr, nullptr);
    if (received <= 0) {
        error_ = "failed reading OpenConnectionReply2";
        close();
        return false;
    }

    if (closeRequested_.load()) {
        close();
        return false;
    }

    std::function<void()> beforeRunningCommitHook;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        beforeRunningCommitHook = beforeRunningCommitTestHook_;
    }
    if (beforeRunningCommitHook) beforeRunningCommitHook();

    // Commit the worker-visible running state, then close the only remaining
    // cancellation window. A stop before this store cannot be overwritten;
    // a stop after the recheck observes running=true and clears it itself.
    running_.store(true);
    if (closeRequested_.load()) {
        running_.store(false);
        close();
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(threadMutex_);
        workerActive_ = true;
        try {
            thread_ = std::thread([this]() {
                {
                    std::lock_guard<std::mutex> lock(threadMutex_);
                    workerThreadId_ = std::this_thread::get_id();
                }
                threadCv_.notify_all();

                // Keep the facade/network/RakNet ownership chain alive for
                // the entire worker callback stack. In particular, a user
                // callback may destroy the last Client while handlePacket()
                // is active.
                std::shared_ptr<void> threadLease;
                auto workerActivity =
                    std::unique_ptr<void, std::function<void(void*)>>(
                        this,
                        [this](void*) { endWorkerActivity(); }
                    );
                const auto provider = callbackLifetimeProviderSnapshot();
                if (provider) threadLease = provider();
                if (provider && !threadLease) return;
                // Do not catch live callback exceptions here. Like
                // EventEmitter, an unhandled listener exception crosses the
                // asynchronous event boundary; the activity guard still
                // releases the fd safely.
                runLoop();
            });
        } catch (...) {
            workerActive_ = false;
            throw;
        }
    } catch (const std::exception& e) {
        running_.store(false);
        error_ = e.what();
        finalizeSocketClose();
        threadCv_.notify_all();
        return false;
    } catch (...) {
        running_.store(false);
        error_ = "failed to start RakNet worker";
        finalizeSocketClose();
        threadCv_.notify_all();
        return false;
    }
    if (closeRequested_.load()) {
        running_.store(false);
        close();
        return false;
    }
    sendConnectionRequest();
    return true;
}

void RakNetClient::close(const std::string& reason) {
    closeRequested_.store(true);
    bool wasRunning = running_.exchange(false);
    bool wasConnected = connected_.exchange(false);
    shutdownSocket();

    std::thread worker;
    bool selfWorker = false;
    bool selfConnect = false;
    {
        std::unique_lock<std::mutex> lock(threadMutex_);
        const auto current = std::this_thread::get_id();
        selfWorker = workerActive_ && workerThreadId_ == current;
        selfConnect = connectActive_ && connectThreadId_ == current;

        if (!selfConnect) {
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

    // A self-worker close leaves the descriptor allocated until runLoop's
    // tail. Every other caller has now proved that no select/recv can use it.
    if (!selfWorker) {
        finalizeSocketClose();
    }

    const auto provider = callbackLifetimeProviderSnapshot();
    auto closeLease = provider ? provider() : std::shared_ptr<void>();
    auto handler = closeHandlerSnapshot();
    if ((wasRunning || wasConnected) && handler && (!provider || closeLease)) {
        handler(reason);
    }
}

void RakNetClient::requestStop() noexcept {
    closeRequested_.store(true);
    running_.store(false);
    connected_.store(false);
    shutdownSocket();
}

void RakNetClient::sendReliable(const std::vector<uint8_t>& payload) {
    // RakNativeClient.sendReliable is a no-op until its low-level `connect`
    // event has set connected=true, and again immediately after close.
    if (!connected_.load() || closeRequested_.load()) return;
    sendReliableInternal(payload);
}

void RakNetClient::sendReliableInternal(const std::vector<uint8_t>& payload) {
    if (payload.empty() || closeRequested_.load()) return;

    const std::size_t maxPayloadPerDatagram = static_cast<std::size_t>(
        std::max(128, mtu_ > 0 ? mtu_ - 100 : 1200)
    );

    if (payload.size() > maxPayloadPerDatagram) {
        uint32_t splitCount = static_cast<uint32_t>((payload.size() + maxPayloadPerDatagram - 1u) / maxPayloadPerDatagram);
        uint32_t sharedOrderedIndex = 0;
        uint16_t splitId = 0;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            sharedOrderedIndex = orderedIndex_++;
            splitId = outgoingSplitId_++;
        }

        for (uint32_t splitIndex = 0; splitIndex < splitCount; ++splitIndex) {
            std::size_t begin = static_cast<std::size_t>(splitIndex) * maxPayloadPerDatagram;
            std::size_t end = std::min(begin + maxPayloadPerDatagram, payload.size());
            std::vector<uint8_t> chunk(payload.begin() + static_cast<std::ptrdiff_t>(begin), payload.begin() + static_cast<std::ptrdiff_t>(end));

            uint32_t sequence = 0;
            uint32_t reliableIndex = 0;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                sequence = outgoingSequence_++;
                reliableIndex = reliableIndex_++;
            }

            std::vector<uint8_t> out;
            out.push_back(0x80);
            writeTriadLE(out, sequence);
            out.push_back(static_cast<uint8_t>((3u << 5u) | 0x10u));
            writeU16BE(out, static_cast<uint16_t>(chunk.size() * 8u));
            writeTriadLE(out, reliableIndex);
            writeTriadLE(out, sharedOrderedIndex);
            out.push_back(0);
            writeU32BE(out, splitCount);
            writeU16BE(out, splitId);
            writeU32BE(out, splitIndex);
            out.insert(out.end(), chunk.begin(), chunk.end());

            sendToTarget(out);
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                sentReliableDatagrams_[sequence] = out;
            }
        }
        return;
    }

    uint32_t sequence = 0;
    uint32_t reliableIndex = 0;
    uint32_t orderedIndex = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        sequence = outgoingSequence_++;
        reliableIndex = reliableIndex_++;
        orderedIndex = orderedIndex_++;
    }

    std::vector<uint8_t> out;
    out.push_back(0x80);
    writeTriadLE(out, sequence);
    out.push_back(3u << 5u);
    writeU16BE(out, static_cast<uint16_t>(payload.size() * 8u));
    writeTriadLE(out, reliableIndex);
    writeTriadLE(out, orderedIndex);
    out.push_back(0);
    out.insert(out.end(), payload.begin(), payload.end());

    sendToTarget(out);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        sentReliableDatagrams_[sequence] = out;
    }
}

void RakNetClient::runLoop() {
    while (running_) {
        int descriptor = -1;
        {
            std::lock_guard<std::mutex> lock(socketMutex_);
            descriptor = socket_;
        }
        if (descriptor < 0) break;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(descriptor, &readfds);

        timeval timeout {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;

        int ready = select(descriptor + 1, &readfds, nullptr, nullptr, &timeout);
        if (!running_) break;
        if (ready <= 0) continue;

        std::vector<uint8_t> packet(65536);
        ssize_t received = recvfrom(
            descriptor,
            packet.data(),
            packet.size(),
            0,
            nullptr,
            nullptr
        );
        if (received <= 0) continue;
        packet.resize(static_cast<std::size_t>(received));
        handlePacket(packet);
        const auto provider = callbackLifetimeProviderSnapshot();
        if (provider && !provider()) {
            // A callback destroyed/closed the facade. Stop the worker before
            // its thread-wide lease is released at the lambda boundary.
            running_.store(false);
            break;
        }
    }
}

void RakNetClient::handlePacket(const std::vector<uint8_t>& packet) {
    if (packet.empty()) return;
    const uint8_t packetId = packet[0];
    bool liveCallbackActive = false;

    try {
        if (packetId == ID_ACK || packetId == ID_NACK) {
            auto sequences = readAckSequences(packet);
            std::vector<std::vector<uint8_t>> resend;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (packetId == ID_ACK) {
                    for (uint32_t sequence : sequences) sentReliableDatagrams_.erase(sequence);
                } else {
                    for (uint32_t sequence : sequences) {
                        auto it = sentReliableDatagrams_.find(sequence);
                        if (it != sentReliableDatagrams_.end()) resend.push_back(it->second);
                    }
                }
            }
            for (const auto& datagram : resend) sendToTarget(datagram);
            return;
        }

        if (packetId < 0x80 || packetId > 0x8f) return;

        uint32_t sequence = 0;
        auto frames = parseConnectedDatagram(packet, sequence);
        sendToTarget(buildAck(sequence));

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (!receivedDatagramSequences_.insert(sequence).second) {
                return; // duplicate datagram; ACK it but do not decrypt/process twice
            }
            if (receivedDatagramSequences_.size() > 32768) {
                receivedDatagramSequences_.clear();
                receivedDatagramSequences_.insert(sequence);
            }
        }

        auto processPayload = [&](const std::vector<uint8_t>& payload) {
            if (payload.empty()) return;

            if (payload[0] == ID_CONNECTED_PING) {
                std::size_t offset = 1;
                sendConnectedPong(static_cast<int64_t>(readU64BE(payload, offset)));
                return;
            }

            if (payload[0] == ID_CONNECTION_REQUEST_ACCEPTED) {
                bool expected = false;
                if (connected_.compare_exchange_strong(expected, true)) {
                    sendNewIncomingConnection();
                    auto handler = connectedHandlerSnapshot();
                    if (handler) {
                        liveCallbackActive = true;
                        handler();
                        liveCallbackActive = false;
                    }
                }
                return;
            }

            if (payload[0] == ID_DISCONNECTION_NOTIFICATION ||
                payload[0] == ID_CONNECTION_LOST ||
                payload[0] == ID_CONNECTION_BANNED ||
                payload[0] == ID_INCOMPATIBLE_PROTOCOL_VERSION) {
                requestStop();
                auto handler = closeHandlerSnapshot();
                if (handler) {
                    liveCallbackActive = true;
                    // raknet-native exposes MessageID as a numeric reason.
                    // CloseHandler is string-based in this C++ API, so retain
                    // that exact observable value in decimal form.
                    handler(std::to_string(payload[0]));
                    liveCallbackActive = false;
                }
                return;
            }

            auto handler = encapsulatedHandlerSnapshot();
            if (handler) {
                liveCallbackActive = true;
                handler(payload);
                liveCallbackActive = false;
            }
        };

        auto flushPendingIfReady = [&]() {
            std::vector<std::vector<uint8_t>> ready;

            {
                std::lock_guard<std::mutex> lock(stateMutex_);

                if (!splits_.empty()) {
                    return;
                }

                for (auto& item : pendingInboundPayloads_) {
                    pendingOrderedPayloads_[item.orderedIndex] = std::move(item.payload);
                }
                pendingInboundPayloads_.clear();

                while (true) {
                    auto it = pendingOrderedPayloads_.find(nextInboundOrderedIndex_);
                    if (it == pendingOrderedPayloads_.end()) {
                        break;
                    }

                    ready.push_back(std::move(it->second));
                    pendingOrderedPayloads_.erase(it);
                    ++nextInboundOrderedIndex_;
                }
            }

            for (const auto& payload : ready) {
                processPayload(payload);
            }
        };

        for (const auto& frame : frames) {
            std::vector<uint8_t> payload = frame.payload;
            bool shouldQueueForOrdering = false;

            if (frame.split) {
                if (frame.splitCount == 0 || frame.splitCount > 4096 || frame.splitIndex >= frame.splitCount) {
                    continue;
                }

                bool complete = false;

                {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    auto& split = splits_[frame.splitId];

                    if (split.count == 0) {
                        split.count = frame.splitCount;
                        split.parts.resize(frame.splitCount);
                        split.received.resize(frame.splitCount, false);
                    }

                    if (split.count != frame.splitCount) {
                        splits_.erase(frame.splitId);
                        continue;
                    }

                    auto index = static_cast<std::size_t>(frame.splitIndex);
                    split.parts[index] = frame.payload;
                    split.received[index] = true;

                    complete = std::all_of(
                        split.received.begin(),
                        split.received.end(),
                        [](bool value) { return value; }
                    );

                    if (complete) {
                        payload.clear();
                        for (const auto& part : split.parts) {
                            payload.insert(payload.end(), part.begin(), part.end());
                        }
                        splits_.erase(frame.splitId);
                    }
                }

                if (!complete) {
                    continue;
                }

                shouldQueueForOrdering = true;
            }

            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (frame.ordered || !splits_.empty() || !pendingInboundPayloads_.empty()) {
                    shouldQueueForOrdering = true;
                }

                if (shouldQueueForOrdering) {
                    pendingInboundPayloads_.push_back({
                        frame.ordered ? frame.orderedIndex : nextInboundOrderedIndex_,
                        std::move(payload)
                    });
                }
            }

            if (shouldQueueForOrdering) {
                flushPendingIfReady();
                continue;
            }

            processPayload(payload);
        }

        flushPendingIfReady();
    } catch (const std::exception& e) {
        if (liveCallbackActive) throw;
        error_ = e.what();
    } catch (...) {
        if (liveCallbackActive) throw;
        error_ = "RakNet packet handling failed";
    }
}

void RakNetClient::sendToTarget(const std::vector<uint8_t>& packet) {
    if (targetLen_ <= 0 || packet.empty()) return;
    std::lock_guard<std::mutex> lock(socketMutex_);
    if (socket_ < 0) return;
    (void) sendto(
        socket_,
        packet.data(),
        packet.size(),
        0,
        reinterpret_cast<const sockaddr*>(target_.data()),
        static_cast<socklen_t>(targetLen_)
    );
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

void RakNetClient::shutdownSocket() noexcept {
    std::lock_guard<std::mutex> lock(socketMutex_);
    if (socket_ >= 0) {
        // shutdown wakes select/recv but does not make the descriptor number
        // reusable. The worker tail or its joiner performs the final close.
        (void) ::shutdown(socket_, SHUT_RDWR);
    }
}

void RakNetClient::finalizeSocketClose() noexcept {
    int descriptor = -1;
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        descriptor = socket_;
        socket_ = -1;
    }
    if (descriptor >= 0) {
        (void) ::close(descriptor);
    }
}

void RakNetClient::endWorkerActivity() noexcept {
    running_.store(false);
    finalizeSocketClose();
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        workerActive_ = false;
        workerThreadId_ = std::thread::id{};
    }
    threadCv_.notify_all();
}

RakNetClient::CallbackLifetimeProvider
RakNetClient::callbackLifetimeProviderSnapshot() const {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return callbackLifetimeProvider_;
}

std::shared_ptr<void> RakNetClient::acquireCallbackLifetime() const {
    const auto provider = callbackLifetimeProviderSnapshot();
    return provider ? provider() : std::shared_ptr<void>();
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

void RakNetClient::sendConnectionRequest() {
    std::vector<uint8_t> payload;
    payload.push_back(ID_CONNECTION_REQUEST);
    writeU64BE(payload, options_.clientGuid);
    writeU64BE(payload, static_cast<uint64_t>(nowMillis()));
    payload.push_back(0x00);
    sendReliableInternal(payload);
}

void RakNetClient::sendNewIncomingConnection() {
    std::vector<uint8_t> payload;
    payload.push_back(ID_NEW_INCOMING_CONNECTION);

    sockaddr_in server {};
    if (targetLen_ >= static_cast<int>(sizeof(sockaddr_in))) {
        server = *reinterpret_cast<const sockaddr_in*>(target_.data());
    }

    auto writeAddress = [&](const sockaddr_in& addr) {
        payload.push_back(4); // IPv4

        uint32_t ip = ntohl(addr.sin_addr.s_addr);

        payload.push_back(static_cast<uint8_t>((~((ip >> 24) & 0xffu)) & 0xffu));
        payload.push_back(static_cast<uint8_t>((~((ip >> 16) & 0xffu)) & 0xffu));
        payload.push_back(static_cast<uint8_t>((~((ip >> 8) & 0xffu)) & 0xffu));
        payload.push_back(static_cast<uint8_t>((~(ip & 0xffu)) & 0xffu));

        writeU16BE(payload, ntohs(addr.sin_port));
    };

    writeAddress(server);

    sockaddr_in zero {};
    zero.sin_family = AF_INET;
    zero.sin_addr.s_addr = 0;
    zero.sin_port = 0;

    for (int i = 0; i < 20; ++i) {
        writeAddress(zero);
    }

    writeU64BE(payload, static_cast<uint64_t>(nowMillis()));
    writeU64BE(payload, static_cast<uint64_t>(nowMillis()));

    sendReliable(payload);
}

void RakNetClient::sendConnectedPong(int64_t pingTime) {
    std::vector<uint8_t> payload;
    payload.push_back(ID_CONNECTED_PONG);
    writeU64BE(payload, static_cast<uint64_t>(pingTime));
    writeU64BE(payload, static_cast<uint64_t>(nowMillis()));
    sendReliable(payload);
}

} // namespace bedrock
