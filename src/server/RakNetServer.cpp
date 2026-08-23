#include <bedrock/server/RakNetServer.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace bedrock {

namespace {

constexpr uint8_t ID_UNCONNECTED_PING = 0x01;
constexpr uint8_t ID_OPEN_CONNECTION_REQUEST_1 = 0x05;
constexpr uint8_t ID_OPEN_CONNECTION_REPLY_1 = 0x06;
constexpr uint8_t ID_OPEN_CONNECTION_REQUEST_2 = 0x07;
constexpr uint8_t ID_OPEN_CONNECTION_REPLY_2 = 0x08;
constexpr uint8_t ID_CONNECTED_PING = 0x00;
constexpr uint8_t ID_CONNECTED_PONG = 0x03;
constexpr uint8_t ID_CONNECTION_REQUEST = 0x09;
constexpr uint8_t ID_CONNECTION_REQUEST_ACCEPTED = 0x10;
constexpr uint8_t ID_NEW_INCOMING_CONNECTION = 0x13;
constexpr uint8_t ID_NO_FREE_INCOMING_CONNECTIONS = 0x14;
constexpr uint8_t ID_DISCONNECTION_NOTIFICATION = 0x15;
constexpr uint8_t ID_CONNECTION_LOST = 0x16;
constexpr uint8_t ID_INCOMPATIBLE_PROTOCOL_VERSION = 0x19;
constexpr uint8_t ID_UNCONNECTED_PONG = 0x1c;
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
    return (static_cast<uint64_t>(now) << 16u) ^ static_cast<uint64_t>(rd()) ^ 0xBEDC0FFEEULL;
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

bool isRecognizedOfflinePacket(const std::vector<uint8_t>& packet) {
    if (packet.empty()) {
        return false;
    }
    switch (packet[0]) {
    case ID_UNCONNECTED_PING:
    case 0x02: // ID_UNCONNECTED_PING_OPEN_CONNECTIONS
    case 0x0d: // ID_OUT_OF_BAND_INTERNAL
        return packet.size() >= 25 && hasMagic(packet, 9);
    case ID_UNCONNECTED_PONG:
        // Native gates this case at 29 bytes and then compares 16 bytes at
        // offset 17. Require the full 33 bytes rather than reproduce its
        // out-of-bounds read for lengths 29..32.
        return packet.size() >= 33 && hasMagic(packet, 17);
    case ID_OPEN_CONNECTION_REQUEST_1:
    case ID_OPEN_CONNECTION_REPLY_1:
    case ID_OPEN_CONNECTION_REQUEST_2:
    case ID_OPEN_CONNECTION_REPLY_2:
    case 0x11: // ID_CONNECTION_ATTEMPT_FAILED
    case 0x12: // ID_ALREADY_CONNECTED
    case ID_NO_FREE_INCOMING_CONNECTIONS:
    case 0x17: // ID_CONNECTION_BANNED
    case 0x1a: // ID_IP_RECENTLY_CONNECTED
        return packet.size() >= 25 && hasMagic(packet, 1);
    case ID_INCOMPATIBLE_PROTOCOL_VERSION:
        return packet.size() == 26 && hasMagic(packet, 2);
    default:
        return false;
    }
}

void appendMagic(std::vector<uint8_t>& out) {
    out.insert(out.end(), std::begin(RAKNET_MAGIC), std::end(RAKNET_MAGIC));
}

std::string sockaddrToIp(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] {};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip);
}

uint16_t sockaddrToPort(const sockaddr_in& addr) {
    return ntohs(addr.sin_port);
}

void writeRakNetAddressIPv4(std::vector<uint8_t>& out, const sockaddr_in& addr) {
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(&addr.sin_addr.s_addr);
    out.push_back(4);
    out.push_back(static_cast<uint8_t>(~ip[0]));
    out.push_back(static_cast<uint8_t>(~ip[1]));
    out.push_back(static_cast<uint8_t>(~ip[2]));
    out.push_back(static_cast<uint8_t>(~ip[3]));
    writeU16BE(out, ntohs(addr.sin_port));
}

void writeRakNetAddressIPv4(
    std::vector<uint8_t>& out,
    const std::string& ipText,
    uint16_t port
) {
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

std::string defaultAdvertisement(const RakNetServerOptions& options) {
    if (!options.advertisement.empty()) {
        return options.advertisement;
    }

    std::ostringstream ss;
    ss << "MCPE;Bedrock Protocol C++;"
       << options.protocolVersion
       << ";unknown;0;"
       << options.maxPlayers
       << ";"
       << options.serverGuid
       << ";Bedrock Protocol C++;Survival;1;"
       << options.port
       << ";"
       << options.port
       << ";";
    return ss.str();
}

std::vector<uint8_t> buildUnconnectedPong(
    uint64_t pingTime,
    uint64_t serverGuid,
    const std::string& advertisement
) {
    std::vector<uint8_t> out;
    out.push_back(ID_UNCONNECTED_PONG);
    writeU64BE(out, pingTime);
    writeU64BE(out, serverGuid);
    appendMagic(out);
    writeU16BE(out, static_cast<uint16_t>(advertisement.size()));
    out.insert(out.end(), advertisement.begin(), advertisement.end());
    return out;
}

std::vector<uint8_t> buildOpenConnectionReply1(uint64_t serverGuid, uint16_t mtu) {
    std::vector<uint8_t> out;
    out.push_back(ID_OPEN_CONNECTION_REPLY_1);
    appendMagic(out);
    writeU64BE(out, serverGuid);
    out.push_back(0x00);
    writeU16BE(out, mtu);
    return out;
}

std::vector<uint8_t> buildIncompatibleProtocolVersion(
    uint8_t protocol,
    uint64_t serverGuid
) {
    std::vector<uint8_t> out;
    out.push_back(ID_INCOMPATIBLE_PROTOCOL_VERSION);
    out.push_back(protocol);
    appendMagic(out);
    writeU64BE(out, serverGuid);
    return out;
}

std::vector<uint8_t> buildNoFreeIncomingConnections(uint64_t serverGuid) {
    std::vector<uint8_t> out;
    out.push_back(ID_NO_FREE_INCOMING_CONNECTIONS);
    appendMagic(out);
    writeU64BE(out, serverGuid);
    return out;
}

std::vector<uint8_t> buildOpenConnectionReply2(
    uint64_t serverGuid,
    const sockaddr_in& clientAddress,
    uint16_t mtu
) {
    std::vector<uint8_t> out;
    out.push_back(ID_OPEN_CONNECTION_REPLY_2);
    appendMagic(out);
    writeU64BE(out, serverGuid);
    writeRakNetAddressIPv4(out, clientAddress);
    writeU16BE(out, mtu);
    out.push_back(0x00);
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

std::vector<uint8_t> buildConnectedPong(int64_t pingTime, int64_t pongTime) {
    std::vector<uint8_t> out;
    out.push_back(ID_CONNECTED_PONG);
    writeU64BE(out, static_cast<uint64_t>(pingTime));
    writeU64BE(out, static_cast<uint64_t>(pongTime));
    return out;
}

std::vector<uint8_t> buildConnectionRequestAccepted(
    const sockaddr_in& clientAddress,
    int64_t requestTimestamp,
    int64_t acceptedTimestamp
) {
    std::vector<uint8_t> out;
    out.push_back(ID_CONNECTION_REQUEST_ACCEPTED);
    writeRakNetAddressIPv4(out, clientAddress);
    writeU16BE(out, 0);

    for (int i = 0; i < 20; ++i) {
        writeRakNetAddressIPv4(out, "0.0.0.0", 0);
    }

    writeU64BE(out, static_cast<uint64_t>(requestTimestamp));
    writeU64BE(out, static_cast<uint64_t>(acceptedTimestamp));
    return out;
}

uint16_t readRakNetAddressPort(
    const std::vector<uint8_t>& data,
    std::size_t& offset
) {
    if (offset >= data.size()) {
        throw std::runtime_error("RakNet address version out of range");
    }

    const uint8_t version = data[offset++];
    if (version == 4) {
        if (offset + 4 > data.size()) {
            throw std::runtime_error("RakNet IPv4 address out of range");
        }
        offset += 4;
        return readU16BE(data, offset);
    }

    if (version == 6) {
        if (offset + 28 > data.size()) {
            throw std::runtime_error("RakNet IPv6 address out of range");
        }
        offset += 2; // address family
        const uint16_t port = readU16BE(data, offset);
        offset += 4;  // flow info
        offset += 16; // address
        offset += 4;  // scope id
        return port;
    }

    throw std::runtime_error("unsupported RakNet address version");
}

int64_t nowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
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
    bool split = false;
    bool ordered = false;
    uint32_t orderedIndex = 0;
    uint8_t orderedChannel = 0;
    uint32_t splitCount = 0;
    uint16_t splitId = 0;
    uint32_t splitIndex = 0;
    std::vector<uint8_t> payload;
};

std::vector<ParsedFrame> parseConnectedDatagram(
    const std::vector<uint8_t>& data,
    uint32_t& sequence
) {
    std::vector<ParsedFrame> frames;
    if (data.empty() || data[0] < 0x80 || data[0] > 0x8f) {
        return frames;
    }

    std::size_t offset = 1;
    sequence = readTriadLE(data, offset);

    while (offset < data.size()) {
        ParsedFrame frame;
        const uint8_t flags = data[offset++];
        frame.reliability = static_cast<uint8_t>((flags & 0xe0u) >> 5u);
        frame.split = (flags & 0x10u) != 0;

        const uint16_t bitLength = readU16BE(data, offset);
        std::size_t byteLength = (static_cast<std::size_t>(bitLength) + 7u) / 8u;

        if (isReliable(frame.reliability)) {
            (void) readTriadLE(data, offset);
        }
        if (isSequenced(frame.reliability)) {
            (void) readTriadLE(data, offset);
        }
        if (isOrdered(frame.reliability)) {
            frame.ordered = true;
            frame.orderedIndex = readTriadLE(data, offset);
            if (offset >= data.size()) {
                throw std::runtime_error("ordered channel out of range");
            }
            frame.orderedChannel = data[offset++];
        }
        if (frame.split) {
            if (offset + 10 > data.size()) {
                throw std::runtime_error("split header out of range");
            }
            frame.splitCount =
                (static_cast<uint32_t>(data[offset]) << 24u) |
                (static_cast<uint32_t>(data[offset + 1]) << 16u) |
                (static_cast<uint32_t>(data[offset + 2]) << 8u) |
                static_cast<uint32_t>(data[offset + 3]);
            offset += 4;
            frame.splitId = readU16BE(data, offset);
            frame.splitIndex =
                (static_cast<uint32_t>(data[offset]) << 24u) |
                (static_cast<uint32_t>(data[offset + 1]) << 16u) |
                (static_cast<uint32_t>(data[offset + 2]) << 8u) |
                static_cast<uint32_t>(data[offset + 3]);
            offset += 4;
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

std::string peerKey(const sockaddr_in& address) {
    return sockaddrToIp(address) + ":" + std::to_string(sockaddrToPort(address));
}

std::vector<uint32_t> readAckSequences(const std::vector<uint8_t>& data) {
    std::vector<uint32_t> sequences;
    if (data.size() < 3) {
        return sequences;
    }

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

bool shouldLogDroppedPacket(
    const sockaddr_in& sender,
    uint8_t packetId,
    const std::string& error
) {
    static std::mutex mutex;
    static std::unordered_map<std::string, uint64_t> counters;

    std::ostringstream key;
    key << sockaddrToIp(sender) << ":"
        << sockaddrToPort(sender) << ":"
        << static_cast<int>(packetId) << ":"
        << error;

    std::lock_guard<std::mutex> lock(mutex);
    auto& count = counters[key.str()];
    ++count;
    return count == 1 || (count % 40) == 0;
}

} // namespace

RakNetServer::RakNetServer(RakNetServerOptions options)
    : options_(std::move(options)) {
    if (options_.serverGuid == 0) {
        options_.serverGuid = makeGuid();
    }
}

RakNetServer::~RakNetServer() {
    close();
}

void RakNetServer::listen() {
    if (running_) {
        return;
    }

    // A close requested from the former worker cannot join itself.  Reap that
    // completed worker before assigning a new std::thread on relisten.
    joinWorkerIfExternal();

    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* res = nullptr;
    const std::string portString = std::to_string(options_.port);
    const char* host = options_.host.empty() || options_.host == "0.0.0.0"
        ? nullptr
        : options_.host.c_str();

    int gai = getaddrinfo(host, portString.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(gai));
    }

    socket_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (socket_ < 0) {
        freeaddrinfo(res);
        throw std::runtime_error("socket failed");
    }

    int yes = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (::bind(socket_, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        ::close(socket_);
        socket_ = -1;
        throw std::runtime_error("bind failed");
    }

    sockaddr_in bound {};
    socklen_t boundLen = sizeof(bound);
    if (getsockname(socket_, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
        boundPort_ = ntohs(bound.sin_port);
    } else {
        boundPort_ = options_.port;
    }

    freeaddrinfo(res);

    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        scheduledPeerCloses_.clear();
        shutdownScheduled_ = false;
        advertisementUpdatesEnabled_ = true;
        nextAdvertisementUpdate_ = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(1000);
        scheduledCloseOrder_ = 0;
    }
    running_ = true;
    thread_ = std::thread([this]() {
        runLoop();
    });
}

void RakNetServer::close() {
    closeAfter(std::chrono::milliseconds(0));
}

void RakNetServer::closeAfter(std::chrono::milliseconds delay) {
    if (delay.count() < 0) {
        delay = std::chrono::milliseconds(0);
    }

    if (running_) {
        const auto requestedDeadline = std::chrono::steady_clock::now() + delay;
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!shutdownScheduled_ || requestedDeadline < shutdownDeadline_) {
            shutdownScheduled_ = true;
            shutdownDeadline_ = requestedDeadline;
        }
        // server.js clears serverTimer before its 60 ms backend grace.
        advertisementUpdatesEnabled_ = false;
    }

    joinWorkerIfExternal();
}

void RakNetServer::closePeer(const RakNetServerPeer& peer, bool silent) {
    if (!running_) {
        return;
    }
    closePeerNow(
        peer.address + ":" + std::to_string(peer.port),
        peer.clientGuid,
        silent
    );
}

void RakNetServer::closePeerAfter(
    const RakNetServerPeer& peer,
    std::chrono::milliseconds delay,
    bool silent
) {
    if (!running_) {
        return;
    }
    if (delay.count() < 0) {
        delay = std::chrono::milliseconds(0);
    }

    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    scheduledPeerCloses_.push_back({
        std::chrono::steady_clock::now() + delay,
        peer.address + ":" + std::to_string(peer.port),
        peer.clientGuid,
        silent,
        scheduledCloseOrder_++
    });
}

void RakNetServer::joinWorkerIfExternal() {
    if (onWorkerThread()) {
        return;
    }

    std::lock_guard<std::mutex> lock(joinMutex_);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void RakNetServer::runLoop() {
    while (running_) {
        if (!processLifecycleDeadlines()) {
            break;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(socket_, &readfds);

        timeval timeout {};
        timeout.tv_sec = 0;
        // Timers in serverPlayer.js are millisecond timers.  A short poll keeps
        // the 60/100 ms lifecycle deadlines accurate without blocking sends.
        timeout.tv_usec = 5000;

        int ready = select(socket_ + 1, &readfds, nullptr, nullptr, &timeout);
        if (!running_) {
            break;
        }
        if (ready <= 0) {
            continue;
        }

        std::vector<uint8_t> packet(4096);
        sockaddr_in sender {};
        socklen_t senderLen = sizeof(sender);
        ssize_t received = recvfrom(
            socket_,
            packet.data(),
            packet.size(),
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &senderLen
        );

        if (received <= 0) {
            continue;
        }

        packet.resize(static_cast<std::size_t>(received));
        handlePacket(packet, &sender, static_cast<int>(senderLen));
    }
}

bool RakNetServer::processLifecycleDeadlines() {
    const auto now = std::chrono::steady_clock::now();
    struct PeerWireTarget {
        RakNetServerPeer peer;
        std::array<uint8_t, 128> endpoint {};
        int endpointLen = 0;
    };

    std::vector<ScheduledPeerClose> duePeerCloses;
    std::vector<PeerWireTarget> heartbeatTargets;
    std::vector<RakNetServerPeer> timedOutPeers;
    bool shutdownDue = false;
    bool advertisementUpdateDue = false;
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
                    duePeerCloses.push_back(*it);
                    it = scheduledPeerCloses_.erase(it);
                } else {
                    ++it;
                }
            }

            if (advertisementUpdatesEnabled_ &&
                nextAdvertisementUpdate_ <= now) {
                advertisementUpdateDue = true;
                do {
                    nextAdvertisementUpdate_ += std::chrono::milliseconds(1000);
                } while (nextAdvertisementUpdate_ <= now);
            }
        }
    }

    if (shutdownDue) {
        finishShutdown();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        auto it = peers_.begin();
        while (it != peers_.end()) {
            const auto& state = it->second;
            // RakPeer expires both UNVERIFIED_SENDER and
            // HANDLING_CONNECTION_REQUEST from the original Request2
            // allocation time.  The comparison is strictly greater than
            // 10,000 ms and the removal is silent.
            if (!state.connected &&
                state.request2AssignedAt != std::chrono::steady_clock::time_point {} &&
                now > state.request2AssignedAt &&
                now - state.request2AssignedAt > std::chrono::milliseconds(10000)) {
                it = peers_.erase(it);
            } else {
                // RakPeer injects a RELIABLE connected ping after half the
                // configured 30 second delivery timeout, but only when no
                // reliable datagram is already awaiting an ACK.
                if (state.connected && state.endpointLen > 0 &&
                    state.sentReliableDatagrams.empty() &&
                    state.lastReliableSend != std::chrono::steady_clock::time_point {} &&
                    now > state.lastReliableSend &&
                    now - state.lastReliableSend > std::chrono::milliseconds(15000)) {
                    heartbeatTargets.push_back({
                        state.peer,
                        state.endpoint,
                        state.endpointLen
                    });
                }
                ++it;
            }
        }
    }

    for (const auto& target : heartbeatTargets) {
        std::vector<uint8_t> ping {ID_CONNECTED_PING};
        writeU64BE(ping, static_cast<uint64_t>(nowMillis()));
        sendConnectedFrame(
            target.peer,
            target.endpoint.data(),
            target.endpointLen,
            ping,
            2
        );
    }

    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        auto it = peers_.begin();
        while (it != peers_.end()) {
            const auto& state = it->second;
            // ReliabilityLayer declares a peer dead only while a reliable
            // datagram is pending and no datagram has arrived for strictly
            // more than the configured 30,000 ms timeout.
            if (state.connected && !state.sentReliableDatagrams.empty() &&
                state.timeLastDatagramArrived != std::chrono::steady_clock::time_point {} &&
                now > state.timeLastDatagramArrived &&
                now - state.timeLastDatagramArrived > std::chrono::milliseconds(30000)) {
                timedOutPeers.push_back(state.peer);
                // Native CloseConnectionInternal removes the transport before
                // its locally generated ID_CONNECTION_LOST reaches JS.
                it = peers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // The native producer packet is local only: no 0x15/0x16 is put on the
    // wire. Invoke callbacks after transport erasure and outside peersMutex_,
    // so an attempted send from the callback is a no-op.
    for (const auto& peer : timedOutPeers) {
        if (closeConnectionHandler_) {
            closeConnectionHandler_(peer);
        }
    }

    if (advertisementUpdateDue && advertisementProvider_) {
        options_.advertisement = advertisementProvider_();
    }

    std::sort(
        duePeerCloses.begin(),
        duePeerCloses.end(),
        [](const ScheduledPeerClose& lhs, const ScheduledPeerClose& rhs) {
            if (lhs.deadline != rhs.deadline) {
                return lhs.deadline < rhs.deadline;
            }
            return lhs.order < rhs.order;
        }
    );
    for (const auto& scheduled : duePeerCloses) {
        closePeerNow(scheduled.key, scheduled.clientGuid, scheduled.silent);
    }
    return running_;
}

void RakNetServer::closePeerNow(
    const std::string& key,
    uint64_t clientGuid,
    bool silent
) {
    RakNetServerPeer peer;
    std::array<uint8_t, 128> endpoint {};
    int endpointLen = 0;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        auto it = peers_.find(key);
        if (it == peers_.end() || !it->second.connected ||
            it->second.peer.clientGuid != clientGuid) {
            return;
        }
        peer = it->second.peer;
        endpoint = it->second.endpoint;
        endpointLen = it->second.endpointLen;
    }

    // Player.close emits close before connection.close.  Deliver the local
    // close callback while the RakNet peer is still usable, then perform the
    // native-style transport close.
    if (closeConnectionHandler_) {
        closeConnectionHandler_(peer);
    }
    if (!silent && endpointLen > 0) {
        sendReliableOrdered(
            peer,
            endpoint.data(),
            endpointLen,
            std::vector<uint8_t>{ID_DISCONNECTION_NOTIFICATION}
        );
    }

    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = peers_.find(key);
    if (it != peers_.end() && it->second.peer.clientGuid == clientGuid) {
        peers_.erase(it);
    }
}

void RakNetServer::finishShutdown() {
    std::vector<std::pair<RakNetServerPeer, std::pair<std::array<uint8_t, 128>, int>>> peers;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        peers.reserve(peers_.size());
        for (const auto& entry : peers_) {
            if (!entry.second.connected || entry.second.endpointLen <= 0) {
                continue;
            }
            peers.push_back({
                entry.second.peer,
                {entry.second.endpoint, entry.second.endpointLen}
            });
        }
    }

    // RakPeer::Shutdown(blockDuration) notifies every active peer but the
    // native addon stops its JS delivery loop before Shutdown, so no local
    // closeConnection callbacks are emitted for this path.
    for (const auto& entry : peers) {
        sendReliableOrdered(
            entry.first,
            entry.second.first.data(),
            entry.second.second,
            std::vector<uint8_t>{ID_DISCONNECTION_NOTIFICATION}
        );
    }

    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        peers_.clear();
    }
    if (socket_ >= 0) {
        ::shutdown(socket_, SHUT_RDWR);
        ::close(socket_);
        socket_ = -1;
    }
    running_ = false;
}

void RakNetServer::handlePacket(
    const std::vector<uint8_t>& packet,
    const void* sender,
    int senderLen
) {
    if (packet.empty() || !sender || senderLen < static_cast<int>(sizeof(sockaddr_in))) {
        return;
    }

    const auto& senderAddr = *reinterpret_cast<const sockaddr_in*>(sender);
    const uint8_t packetId = packet[0];
    bool liveCallbackThrew = false;
    auto invokeLiveCallback = [&](auto&& callback) {
        try {
            callback();
        } catch (...) {
            // RakNet-native drops malformed transport input, but exceptions
            // raised after it has surfaced a live event belong to the JS
            // EventEmitter boundary and must escape the networking callback.
            liveCallbackThrew = true;
            throw;
        }
    };

    // ProcessOfflineNetworkPacket consumes valid magic-bearing offline
    // packets before they reach ReliabilityLayer. Every other UDP datagram
    // longer than two bytes refreshes the assigned endpoint before parsing or
    // duplicate filtering, including unknown and malformed packet IDs.
    if (packet.size() > 2 && !isRecognizedOfflinePacket(packet)) {
        std::lock_guard<std::mutex> lock(peersMutex_);
        auto it = peers_.find(peerKey(senderAddr));
        if (it != peers_.end()) {
            it->second.timeLastDatagramArrived = std::chrono::steady_clock::now();
        }
    }

    try {
        if (packetId == ID_UNCONNECTED_PING) {
            std::size_t offset = 1;
            uint64_t pingTime = readU64BE(packet, offset);
            if (!hasMagic(packet, offset)) {
                return;
            }

            auto pong = buildUnconnectedPong(
                pingTime,
                options_.serverGuid,
                defaultAdvertisement(options_)
            );
            sendTo(sender, senderLen, pong);
            return;
        }

        if (packetId == ID_OPEN_CONNECTION_REQUEST_1) {
            std::size_t offset = 1;
            if (!hasMagic(packet, offset)) {
                return;
            }
            offset += 16;
            if (offset >= packet.size()) {
                return;
            }

            const uint8_t clientProtocol = packet[offset];
            // raknet-native 1.2.3 sets
            // RakPeer::allowClientsWithOlderVersion = true when listening.
            // Older RakNet protocols therefore continue through the handshake;
            // only a protocol newer than the server is rejected.
            if (clientProtocol > static_cast<uint8_t>(options_.protocolVersion)) {
                sendTo(
                    sender,
                    senderLen,
                    buildIncompatibleProtocolVersion(
                        static_cast<uint8_t>(options_.protocolVersion),
                        options_.serverGuid
                    )
                );
                return;
            }

            const int mtu = static_cast<int>(packet.size()) + UDP_IPV4_HEADER_SIZE;
            auto reply = buildOpenConnectionReply1(
                options_.serverGuid,
                static_cast<uint16_t>(std::min(1400, mtu))
            );
            sendTo(sender, senderLen, reply);
            return;
        }

        if (packetId == ID_OPEN_CONNECTION_REQUEST_2) {
            std::size_t offset = 1;
            if (!hasMagic(packet, offset)) {
                return;
            }
            offset += 16;

            if (offset + 7 > packet.size()) {
                return;
            }
            offset += 7;

            const uint16_t mtu = readU16BE(packet, offset);
            const uint64_t clientGuid = readU64BE(packet, offset);

            RakNetServerPeer peer;
            peer.address = sockaddrToIp(senderAddr);
            peer.port = sockaddrToPort(senderAddr);
            peer.clientGuid = clientGuid;
            peer.mtu = mtu;

            // RakNativeServer passes `options.maxPlayers || 3` as both
            // Startup(maxConnections) and maximum incoming connections.
            // RakPeer's full check counts only fully connected remote peers,
            // while its fixed remote-system table is occupied from Request2.
            // When that table is exhausted by half-open peers RakPeer still
            // sends Reply2, but does not allocate state for the extra peer.
            // Request1 continues to be answered in both cases.
            const auto maximumIncomingConnections = static_cast<std::size_t>(
                options_.maxPlayers != 0 ? options_.maxPlayers : 3
            );
            bool noFreeIncomingConnections = false;
            {
                std::lock_guard<std::mutex> lock(peersMutex_);
                const auto connectedPeers = static_cast<std::size_t>(std::count_if(
                    peers_.begin(),
                    peers_.end(),
                    [](const auto& entry) {
                        return entry.second.connected;
                    }
                ));
                noFreeIncomingConnections =
                    connectedPeers >= maximumIncomingConnections;

                const auto key = peerKey(senderAddr);
                if (!noFreeIncomingConnections &&
                    peers_.find(key) == peers_.end() &&
                    peers_.size() < maximumIncomingConnections) {
                    PeerState state;
                    state.peer = peer;
                    // RakPeer::AssignSystemAddressToRemoteSystemList records
                    // connectionTime once. Duplicate Request2 and the later
                    // ConnectionRequest transition do not refresh it.
                    const auto assignedAt = std::chrono::steady_clock::now();
                    state.request2AssignedAt = assignedAt;
                    // ReliabilityLayer::Reset initializes both liveness clocks
                    // for the newly assigned RemoteSystem.
                    state.timeLastDatagramArrived = assignedAt;
                    state.lastReliableSend = assignedAt;
                    state.endpointLen = senderLen;
                    std::memcpy(
                        state.endpoint.data(),
                        sender,
                        static_cast<std::size_t>(senderLen)
                    );
                    peers_.emplace(key, std::move(state));
                }
            }
            if (noFreeIncomingConnections) {
                sendTo(
                    sender,
                    senderLen,
                    buildNoFreeIncomingConnections(options_.serverGuid)
                );
                return;
            }

            auto reply = buildOpenConnectionReply2(
                options_.serverGuid,
                senderAddr,
                std::min<uint16_t>(mtu, 1400)
            );
            sendTo(sender, senderLen, reply);
            return;
        }

        if (packetId == ID_ACK || packetId == ID_NACK) {
            auto sequences = readAckSequences(packet);
            std::vector<std::vector<uint8_t>> resend;
            {
                std::lock_guard<std::mutex> lock(peersMutex_);
                auto it = peers_.find(peerKey(senderAddr));
                if (it == peers_.end()) {
                    return;
                }
                auto& state = it->second;
                if (packetId == ID_ACK) {
                    for (uint32_t sequence : sequences) {
                        state.sentReliableDatagrams.erase(sequence);
                    }
                } else {
                    for (uint32_t sequence : sequences) {
                        auto it = state.sentReliableDatagrams.find(sequence);
                        if (it != state.sentReliableDatagrams.end()) {
                            resend.push_back(it->second);
                        }
                    }
                }
            }

            for (const auto& datagram : resend) {
                sendTo(sender, senderLen, datagram);
            }
            return;
        }

        if (packetId >= 0x80 && packetId <= 0x8f) {
            uint32_t sequence = 0;
            auto frames = parseConnectedDatagram(packet, sequence);

            RakNetServerPeer peer;
            bool duplicateDatagram = false;
            {
                std::lock_guard<std::mutex> lock(peersMutex_);
                auto it = peers_.find(peerKey(senderAddr));
                if (it == peers_.end()) {
                    return;
                }
                auto& state = it->second;
                state.endpointLen = senderLen;
                std::memcpy(state.endpoint.data(), sender, static_cast<std::size_t>(senderLen));
                peer = state.peer;

                duplicateDatagram = state.receivedDatagramSequences.find(sequence) !=
                    state.receivedDatagramSequences.end();
                if (!duplicateDatagram) {
                    state.receivedDatagramSequences[sequence] = true;
                    state.receivedDatagramOrder.push_back(sequence);
                    while (state.receivedDatagramOrder.size() > 4096) {
                        state.receivedDatagramSequences.erase(state.receivedDatagramOrder.front());
                        state.receivedDatagramOrder.pop_front();
                    }
                }
            }

            sendTo(sender, senderLen, buildAck(sequence));

            if (duplicateDatagram) {
                return;
            }

            auto processPayload = [&](const std::vector<uint8_t>& payload) {
                if (payload.empty()) {
                    return;
                }

                bool connected = false;
                bool connectionRequestAccepted = false;
                {
                    std::lock_guard<std::mutex> lock(peersMutex_);
                    auto it = peers_.find(peerKey(senderAddr));
                    if (it == peers_.end()) {
                        return;
                    }
                    connected = it->second.connected;
                    connectionRequestAccepted = it->second.connectionRequestAccepted;
                }

                if (!connected) {
                    if (payload[0] == ID_CONNECTION_REQUEST) {
                        std::size_t offset = 1;
                        (void) readU64BE(payload, offset); // client GUID
                        const int64_t requestTimestamp = static_cast<int64_t>(readU64BE(payload, offset));
                        if (offset >= payload.size()) {
                            throw std::runtime_error("connection request secure flag out of range");
                        }
                        ++offset; // secure flag

                        {
                            std::lock_guard<std::mutex> lock(peersMutex_);
                            auto it = peers_.find(peerKey(senderAddr));
                            if (it == peers_.end() || it->second.connected) {
                                return;
                            }
                            it->second.connectionRequestAccepted = true;
                        }

                        sendReliableOrdered(
                            peer,
                            sender,
                            senderLen,
                            buildConnectionRequestAccepted(senderAddr, requestTimestamp, nowMillis())
                        );
                        return;
                    }

                    if (payload[0] == ID_NEW_INCOMING_CONNECTION) {
                        if (!connectionRequestAccepted) {
                            return;
                        }

                        std::size_t offset = 1;
                        const uint16_t serverPort = readRakNetAddressPort(payload, offset);
                        if (serverPort != boundPort_) {
                            return;
                        }
                        for (int i = 0; i < 20; ++i) {
                            (void) readRakNetAddressPort(payload, offset);
                            if (payload.size() - offset == 16) {
                                break;
                            }
                        }
                        (void) readU64BE(payload, offset); // request timestamp
                        (void) readU64BE(payload, offset); // accepted timestamp

                        RakNetServerPeer openedPeer;
                        bool emitOpen = false;
                        {
                            std::lock_guard<std::mutex> lock(peersMutex_);
                            auto it = peers_.find(peerKey(senderAddr));
                            if (it != peers_.end() &&
                                it->second.connectionRequestAccepted &&
                                !it->second.connected) {
                                it->second.connected = true;
                                openedPeer = it->second.peer;
                                emitOpen = true;
                            }
                        }

                        if (emitOpen) {
                            // RakPeer sends one immediate UNRELIABLE ping when
                            // HANDLING_CONNECTION_REQUEST becomes CONNECTED,
                            // before surfacing ID_NEW_INCOMING_CONNECTION.
                            std::vector<uint8_t> ping {ID_CONNECTED_PING};
                            writeU64BE(ping, static_cast<uint64_t>(nowMillis()));
                            sendConnectedFrame(
                                openedPeer,
                                sender,
                                senderLen,
                                ping,
                                0
                            );
                            if (openConnectionHandler_) {
                                invokeLiveCallback([&]() {
                                    openConnectionHandler_(openedPeer);
                                });
                            }
                        }
                    }
                    return;
                }

                if (payload[0] < 0x80) {
                    if (payload[0] == ID_CONNECTED_PING) {
                        std::size_t offset = 1;
                        const int64_t pingTime = static_cast<int64_t>(readU64BE(payload, offset));
                        // RakPeer answers ID_CONNECTED_PING with an
                        // UNRELIABLE ID_CONNECTED_PONG.
                        sendConnectedFrame(
                            peer,
                            sender,
                            senderLen,
                            buildConnectedPong(pingTime, nowMillis()),
                            0
                        );
                        return;
                    }

                    if (payload[0] == ID_DISCONNECTION_NOTIFICATION ||
                        payload[0] == ID_CONNECTION_LOST) {
                        if (closeConnectionHandler_) {
                            invokeLiveCallback([&]() {
                                closeConnectionHandler_(peer);
                            });
                        }
                        {
                            std::lock_guard<std::mutex> lock(peersMutex_);
                            peers_.erase(peerKey(senderAddr));
                        }
                    }
                    return;
                }

                if (encapsulatedHandler_) {
                    invokeLiveCallback([&]() {
                        encapsulatedHandler_(peer, payload);
                    });
                }
            };

            for (const auto& frame : frames) {
                std::vector<uint8_t> payload = frame.payload;
                if (frame.split) {
                    if (frame.splitCount == 0 || frame.splitCount > 4096 || frame.splitIndex >= frame.splitCount) {
                        continue;
                    }

                    bool complete = false;
                    {
                        std::lock_guard<std::mutex> lock(peersMutex_);
                        auto peerIt = peers_.find(peerKey(senderAddr));
                        if (peerIt == peers_.end() ||
                            peerIt->second.peer.clientGuid != peer.clientGuid) {
                            continue;
                        }
                        auto& state = peerIt->second;
                        auto& split = state.splits[frame.splitId];
                        if (split.count == 0) {
                            split.count = frame.splitCount;
                            split.parts.resize(frame.splitCount);
                            split.received.resize(frame.splitCount, false);
                        }

                        if (split.count != frame.splitCount) {
                            state.splits.erase(frame.splitId);
                            continue;
                        }

                        auto index = static_cast<std::size_t>(frame.splitIndex);
                        split.parts[index] = frame.payload;
                        split.received[index] = true;
                        complete = std::all_of(split.received.begin(), split.received.end(), [](bool value) {
                            return value;
                        });

                        if (complete) {
                            payload.clear();
                            for (const auto& part : split.parts) {
                                payload.insert(payload.end(), part.begin(), part.end());
                            }
                            state.splits.erase(frame.splitId);
                        }
                    }

                    if (!complete) {
                        continue;
                    }
                }

                std::vector<std::vector<uint8_t>> readyPayloads;
                if (frame.ordered) {
                    const uint8_t channel = frame.orderedChannel < 32 ? frame.orderedChannel : 0;
                    std::lock_guard<std::mutex> lock(peersMutex_);
                    auto peerIt = peers_.find(peerKey(senderAddr));
                    if (peerIt == peers_.end() ||
                        peerIt->second.peer.clientGuid != peer.clientGuid) {
                        continue;
                    }
                    auto& state = peerIt->second;
                    uint32_t& expected = state.expectedOrderedIndex[channel];
                    bool& initialized = state.expectedOrderedIndexInitialized[channel];

                    if (!initialized) {
                        expected = frame.orderedIndex;
                        initialized = true;
                    }

                    if (frame.orderedIndex < expected) {
                        continue;
                    }

                    if (frame.orderedIndex > expected) {
                        state.pendingOrderedPayloads[channel].emplace(frame.orderedIndex, std::move(payload));
                        continue;
                    }

                    readyPayloads.push_back(std::move(payload));
                    ++expected;

                    auto& pending = state.pendingOrderedPayloads[channel];
                    while (true) {
                        auto it = pending.find(expected);
                        if (it == pending.end()) {
                            break;
                        }
                        readyPayloads.push_back(std::move(it->second));
                        pending.erase(it);
                        ++expected;
                    }
                } else {
                    readyPayloads.push_back(std::move(payload));
                }

                for (const auto& readyPayload : readyPayloads) {
                    processPayload(readyPayload);
                }
            }
            return;
        }

        if (rawPacketHandler_) {
            RakNetServerPeer peer;
            peer.address = sockaddrToIp(senderAddr);
            peer.port = sockaddrToPort(senderAddr);
            invokeLiveCallback([&]() {
                rawPacketHandler_(peer, packet);
            });
        }
    } catch (const std::exception& e) {
        if (liveCallbackThrew) {
            throw;
        }
        if (shouldLogDroppedPacket(senderAddr, packetId, e.what())) {
            std::cerr << "[raknet-server] dropped packet from "
                      << sockaddrToIp(senderAddr) << ":"
                      << sockaddrToPort(senderAddr)
                      << " id=0x" << std::hex << static_cast<int>(packetId)
                      << std::dec << " error=" << e.what() << "\n";
        }
        return;
    } catch (...) {
        if (liveCallbackThrew) {
            throw;
        }
        if (shouldLogDroppedPacket(senderAddr, packetId, "unknown")) {
            std::cerr << "[raknet-server] dropped packet from "
                      << sockaddrToIp(senderAddr) << ":"
                      << sockaddrToPort(senderAddr)
                      << " id=0x" << std::hex << static_cast<int>(packetId)
                      << std::dec << " error=unknown\n";
        }
        return;
    }
}

void RakNetServer::sendTo(const void* target, int targetLen, const std::vector<uint8_t>& packet) {
    if (socket_ < 0 || !target || packet.empty()) {
        return;
    }

    (void) sendto(
        socket_,
        packet.data(),
        packet.size(),
        0,
        reinterpret_cast<const sockaddr*>(target),
        static_cast<socklen_t>(targetLen)
    );
}

void RakNetServer::sendReliable(const RakNetServerPeer& peer, const std::vector<uint8_t>& payload) {
    std::array<uint8_t, 128> endpoint {};
    int endpointLen = 0;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        auto it = peers_.find(peer.address + ":" + std::to_string(peer.port));
        if (it == peers_.end() || it->second.endpointLen <= 0) {
            return;
        }
        endpoint = it->second.endpoint;
        endpointLen = it->second.endpointLen;
    }

    sendReliableOrdered(peer, endpoint.data(), endpointLen, payload);
}

void RakNetServer::sendConnectedFrame(
    const RakNetServerPeer& peer,
    const void* target,
    int targetLen,
    const std::vector<uint8_t>& payload,
    uint8_t reliability
) {
    if (payload.empty() || (reliability != 0 && reliability != 2)) {
        return;
    }

    const auto key = peer.address + ":" + std::to_string(peer.port);
    uint32_t sequence = 0;
    uint32_t reliableIndex = 0;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        auto it = peers_.find(key);
        if (it == peers_.end() || !it->second.connected ||
            it->second.peer.clientGuid != peer.clientGuid) {
            return;
        }
        sequence = it->second.outgoingSequence++;
        if (reliability == 2) {
            reliableIndex = it->second.reliableIndex++;
        }
    }

    std::vector<uint8_t> out;
    out.push_back(0x80);
    writeTriadLE(out, sequence);
    out.push_back(static_cast<uint8_t>(reliability << 5u));
    writeU16BE(out, static_cast<uint16_t>(payload.size() * 8u));
    if (reliability == 2) {
        writeTriadLE(out, reliableIndex);
    }
    out.insert(out.end(), payload.begin(), payload.end());

    if (reliability == 2) {
        std::lock_guard<std::mutex> lock(peersMutex_);
        auto it = peers_.find(key);
        if (it == peers_.end() || !it->second.connected ||
            it->second.peer.clientGuid != peer.clientGuid) {
            return;
        }
        it->second.sentReliableDatagrams[sequence] = out;
        it->second.lastReliableSend = std::chrono::steady_clock::now();
    }

    sendTo(target, targetLen, out);
}

void RakNetServer::sendReliableOrdered(
    const RakNetServerPeer& peer,
    const void* target,
    int targetLen,
    const std::vector<uint8_t>& payload
) {
    if (payload.empty()) {
        return;
    }

    const std::size_t maxPayloadPerDatagram = static_cast<std::size_t>(
        std::max(128, peer.mtu > 0 ? peer.mtu - 100 : 1200)
    );

    if (payload.size() > maxPayloadPerDatagram) {
        uint32_t splitCount = static_cast<uint32_t>(
            (payload.size() + maxPayloadPerDatagram - 1u) / maxPayloadPerDatagram
        );

        uint32_t sharedOrderedIndex = 0;
        uint16_t splitId = 0;
        {
            std::lock_guard<std::mutex> lock(peersMutex_);
            auto it = peers_.find(peer.address + ":" + std::to_string(peer.port));
            if (it == peers_.end() ||
                it->second.peer.clientGuid != peer.clientGuid) {
                return;
            }
            sharedOrderedIndex = it->second.orderedIndex++;
            splitId = it->second.outgoingSplitId++;
        }

        for (uint32_t splitIndex = 0; splitIndex < splitCount; ++splitIndex) {
            std::size_t begin = static_cast<std::size_t>(splitIndex) * maxPayloadPerDatagram;
            std::size_t end = std::min(begin + maxPayloadPerDatagram, payload.size());

            std::vector<uint8_t> chunk(
                payload.begin() + static_cast<std::ptrdiff_t>(begin),
                payload.begin() + static_cast<std::ptrdiff_t>(end)
            );

            uint32_t sequence = 0;
            uint32_t reliableIndex = 0;
            {
                std::lock_guard<std::mutex> lock(peersMutex_);
                auto it = peers_.find(peer.address + ":" + std::to_string(peer.port));
                if (it == peers_.end() ||
                    it->second.peer.clientGuid != peer.clientGuid) {
                    return;
                }
                sequence = it->second.outgoingSequence++;
                reliableIndex = it->second.reliableIndex++;
            }

            std::vector<uint8_t> out;
            out.push_back(0x80);
            writeTriadLE(out, sequence);

            const uint8_t reliability = 3;
            out.push_back(static_cast<uint8_t>((reliability << 5u) | 0x10u));
            writeU16BE(out, static_cast<uint16_t>(chunk.size() * 8u));
            writeTriadLE(out, reliableIndex);
            writeTriadLE(out, sharedOrderedIndex);
            out.push_back(0);
            writeU32BE(out, splitCount);
            writeU16BE(out, splitId);
            writeU32BE(out, splitIndex);
            out.insert(out.end(), chunk.begin(), chunk.end());

            {
                std::lock_guard<std::mutex> lock(peersMutex_);
                auto it = peers_.find(peer.address + ":" + std::to_string(peer.port));
                if (it == peers_.end() ||
                    it->second.peer.clientGuid != peer.clientGuid) {
                    return;
                }
                it->second.sentReliableDatagrams[sequence] = out;
                it->second.lastReliableSend = std::chrono::steady_clock::now();
            }
            sendTo(target, targetLen, out);
        }
        return;
    }

    uint32_t sequence = 0;
    uint32_t reliableIndex = 0;
    uint32_t orderedIndex = 0;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        auto it = peers_.find(peer.address + ":" + std::to_string(peer.port));
        if (it == peers_.end() ||
            it->second.peer.clientGuid != peer.clientGuid) {
            return;
        }
        sequence = it->second.outgoingSequence++;
        reliableIndex = it->second.reliableIndex++;
        orderedIndex = it->second.orderedIndex++;
    }

    std::vector<uint8_t> out;
    out.push_back(0x80);
    writeTriadLE(out, sequence);

    const uint8_t reliability = 3;
    out.push_back(static_cast<uint8_t>(reliability << 5u));
    writeU16BE(out, static_cast<uint16_t>(payload.size() * 8u));
    writeTriadLE(out, reliableIndex);
    writeTriadLE(out, orderedIndex);
    out.push_back(0);
    out.insert(out.end(), payload.begin(), payload.end());

    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        auto it = peers_.find(peer.address + ":" + std::to_string(peer.port));
        if (it == peers_.end() ||
            it->second.peer.clientGuid != peer.clientGuid) {
            return;
        }
        it->second.sentReliableDatagrams[sequence] = out;
        it->second.lastReliableSend = std::chrono::steady_clock::now();
    }
    sendTo(target, targetLen, out);
}

} // namespace bedrock
