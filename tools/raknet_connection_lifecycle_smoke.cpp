#include <bedrock/server/RakNetServer.hpp>

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

namespace {

constexpr uint8_t kMagic[16] = {
    0x00, 0xff, 0xff, 0x00,
    0xfe, 0xfe, 0xfe, 0xfe,
    0xfd, 0xfd, 0xfd, 0xfd,
    0x12, 0x34, 0x56, 0x78
};

void writeU16BE(std::vector<uint8_t>& out, uint16_t value) {
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

void appendMagic(std::vector<uint8_t>& out) {
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
}

std::vector<uint8_t> unconnectedPing(uint64_t timestamp) {
    std::vector<uint8_t> out {0x01};
    writeU64BE(out, timestamp);
    appendMagic(out);
    return out;
}

void writeAddress(std::vector<uint8_t>& out, uint16_t port) {
    out.push_back(4);
    out.push_back(static_cast<uint8_t>(~uint8_t {127}));
    out.push_back(static_cast<uint8_t>(~uint8_t {0}));
    out.push_back(static_cast<uint8_t>(~uint8_t {0}));
    out.push_back(static_cast<uint8_t>(~uint8_t {1}));
    writeU16BE(out, port);
}

std::vector<uint8_t> openConnectionRequest1(uint8_t protocol, uint16_t mtu) {
    std::vector<uint8_t> out {0x05};
    appendMagic(out);
    out.push_back(protocol);
    const std::size_t udpPayloadSize = static_cast<std::size_t>(mtu - 28);
    if (out.size() < udpPayloadSize) {
        out.resize(udpPayloadSize, 0x00);
    }
    return out;
}

std::vector<uint8_t> openConnectionRequest2(
    uint16_t serverPort,
    uint16_t mtu,
    uint64_t clientGuid
) {
    std::vector<uint8_t> out {0x07};
    appendMagic(out);
    writeAddress(out, serverPort);
    writeU16BE(out, mtu);
    writeU64BE(out, clientGuid);
    return out;
}

std::vector<uint8_t> connectedDatagram(
    uint32_t sequence,
    const std::vector<uint8_t>& payload
) {
    std::vector<uint8_t> out {0x80};
    writeTriadLE(out, sequence);
    out.push_back(0x00); // unreliable, matching jsp-raknet handshake packets
    writeU16BE(out, static_cast<uint16_t>(payload.size() * 8u));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> connectionRequest(uint64_t clientGuid, uint64_t timestamp) {
    std::vector<uint8_t> out {0x09};
    writeU64BE(out, clientGuid);
    writeU64BE(out, timestamp);
    out.push_back(0x00);
    return out;
}

std::vector<uint8_t> newIncomingConnection(
    uint16_t serverPort,
    uint64_t requestTimestamp,
    uint64_t acceptedTimestamp
) {
    std::vector<uint8_t> out {0x13};
    writeAddress(out, serverPort);
    for (int i = 0; i < 20; ++i) {
        writeAddress(out, serverPort);
    }
    writeU64BE(out, requestTimestamp);
    writeU64BE(out, acceptedTimestamp);
    return out;
}

std::vector<uint8_t> ackDatagram(uint32_t sequence) {
    std::vector<uint8_t> out {0xc0};
    writeU16BE(out, 1);
    out.push_back(1);
    writeTriadLE(out, sequence);
    return out;
}

std::optional<std::vector<uint8_t>> receivePacket(int socket, int timeoutMs) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);

    timeval timeout {};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    const int ready = select(socket + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        return std::nullopt;
    }

    std::vector<uint8_t> packet(4096);
    const auto received = recvfrom(
        socket,
        packet.data(),
        packet.size(),
        0,
        nullptr,
        nullptr
    );
    if (received <= 0) {
        return std::nullopt;
    }
    packet.resize(static_cast<std::size_t>(received));
    return packet;
}

struct FirstFrame {
    uint32_t datagramSequence = 0;
    uint8_t reliability = 0;
    std::vector<uint8_t> payload;
};

std::optional<FirstFrame> firstFrame(
    const std::vector<uint8_t>& datagram
) {
    if (datagram.size() < 7 || datagram[0] < 0x80 || datagram[0] > 0x8f) {
        return std::nullopt;
    }

    FirstFrame frame;
    frame.datagramSequence =
        static_cast<uint32_t>(datagram[1]) |
        (static_cast<uint32_t>(datagram[2]) << 8u) |
        (static_cast<uint32_t>(datagram[3]) << 16u);
    std::size_t offset = 4;
    const uint8_t flags = datagram[offset++];
    frame.reliability = static_cast<uint8_t>((flags >> 5u) & 0x07u);
    const bool split = (flags & 0x10u) != 0;
    if (offset + 2 > datagram.size()) {
        return std::nullopt;
    }
    const uint16_t bitLength = static_cast<uint16_t>(
        (static_cast<uint16_t>(datagram[offset]) << 8u) |
        static_cast<uint16_t>(datagram[offset + 1])
    );
    offset += 2;

    const bool reliable = frame.reliability == 2 || frame.reliability == 3 ||
        frame.reliability == 4 || frame.reliability == 6 || frame.reliability == 7;
    const bool sequenced = frame.reliability == 1 || frame.reliability == 4;
    const bool ordered = frame.reliability == 3 || frame.reliability == 4 ||
        frame.reliability == 7;
    if (reliable) offset += 3;
    if (sequenced) offset += 3;
    if (ordered) offset += 4;
    if (split) offset += 10;

    const std::size_t byteLength = (static_cast<std::size_t>(bitLength) + 7u) / 8u;
    if (offset + byteLength > datagram.size()) {
        return std::nullopt;
    }
    frame.payload = std::vector<uint8_t>(
        datagram.begin() + static_cast<std::ptrdiff_t>(offset),
        datagram.begin() + static_cast<std::ptrdiff_t>(offset + byteLength)
    );
    return frame;
}

std::optional<std::vector<uint8_t>> firstFramePayload(
    const std::vector<uint8_t>& datagram
) {
    const auto frame = firstFrame(datagram);
    if (!frame.has_value()) {
        return std::nullopt;
    }
    return frame->payload;
}

std::optional<std::vector<uint8_t>> waitForPacket(
    int socket,
    int timeoutMs,
    const std::function<bool(const std::vector<uint8_t>&)>& predicate
) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()
        ).count();
        auto packet = receivePacket(socket, static_cast<int>(std::max<int64_t>(1, remaining)));
        if (!packet.has_value()) {
            continue;
        }
        if (predicate(*packet)) {
            return packet;
        }
    }
    return std::nullopt;
}

bool sendPacket(
    int socket,
    const sockaddr_in& target,
    const std::vector<uint8_t>& packet
) {
    return sendto(
        socket,
        packet.data(),
        packet.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    ) == static_cast<ssize_t>(packet.size());
}

bool waitForValue(const std::atomic<int>& value, int expected, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load() == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return value.load() == expected;
}

bool checkProtocolCompatibility(
    uint8_t serverProtocol,
    uint64_t serverGuid,
    const std::vector<uint8_t>& clientProtocols
) {
    bedrock::RakNetServer server({
        .host = "127.0.0.1",
        .port = 0,
        .maxPlayers = 3,
        .protocolVersion = serverProtocol,
        .serverGuid = serverGuid,
        .advertisement = "protocol-compatibility-smoke"
    });
    server.listen();

    const int socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket < 0) {
        std::cerr << "[RAKNET-LIFECYCLE-SMOKE] protocol matrix socket failed\n";
        server.close();
        return false;
    }

    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(server.boundPort());
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);

    constexpr uint16_t mtu = 1200;
    bool ok = true;
    for (const uint8_t clientProtocol : clientProtocols) {
        if (!sendPacket(socket, target, openConnectionRequest1(clientProtocol, mtu))) {
            std::cerr << "[RAKNET-LIFECYCLE-SMOKE] failed protocol matrix request: server="
                      << static_cast<int>(serverProtocol) << " client="
                      << static_cast<int>(clientProtocol) << "\n";
            ok = false;
            break;
        }

        const auto response = receivePacket(socket, 1000);
        if (clientProtocol <= serverProtocol) {
            if (!response.has_value() || response->empty() || (*response)[0] != 0x06) {
                std::cerr << "[RAKNET-LIFECYCLE-SMOKE] older/equal protocol was rejected: server="
                          << static_cast<int>(serverProtocol) << " client="
                          << static_cast<int>(clientProtocol) << "\n";
                ok = false;
                break;
            }
            continue;
        }

        std::vector<uint8_t> expected {0x19, serverProtocol};
        appendMagic(expected);
        writeU64BE(expected, serverGuid);
        if (!response.has_value() || *response != expected) {
            std::cerr << "[RAKNET-LIFECYCLE-SMOKE] newer protocol response mismatch: server="
                      << static_cast<int>(serverProtocol) << " client="
                      << static_cast<int>(clientProtocol) << "\n";
            ok = false;
            break;
        }
    }

    ::close(socket);
    server.close();
    return ok;
}

bool completePeerConnection(
    bedrock::RakNetServer& server,
    int socket,
    const sockaddr_in& target,
    uint8_t protocol,
    uint64_t clientGuid,
    uint16_t mtu,
    const std::atomic<int>& openCount,
    int expectedOpenCount
) {
    if (!sendPacket(socket, target, openConnectionRequest1(protocol, mtu))) {
        return false;
    }
    const auto reply1 = receivePacket(socket, 1000);
    if (!reply1.has_value() || reply1->empty() || (*reply1)[0] != 0x06) {
        return false;
    }

    if (!sendPacket(
            socket,
            target,
            openConnectionRequest2(server.boundPort(), mtu, clientGuid))) {
        return false;
    }
    const auto reply2 = receivePacket(socket, 1000);
    if (!reply2.has_value() || reply2->empty() || (*reply2)[0] != 0x08) {
        return false;
    }

    constexpr uint64_t requestTimestamp = 0x0102030405060708ull;
    if (!sendPacket(
            socket,
            target,
            connectedDatagram(0, connectionRequest(clientGuid, requestTimestamp)))) {
        return false;
    }
    const auto accepted = waitForPacket(socket, 1000, [](const std::vector<uint8_t>& packet) {
        const auto payload = firstFramePayload(packet);
        return payload.has_value() && !payload->empty() && (*payload)[0] == 0x10;
    });
    if (!accepted.has_value()) {
        return false;
    }

    if (!sendPacket(
            socket,
            target,
            connectedDatagram(
                1,
                newIncomingConnection(
                    server.boundPort(),
                    requestTimestamp,
                    0x0807060504030201ull)))) {
        return false;
    }
    if (!waitForValue(openCount, expectedOpenCount, 1000)) {
        return false;
    }

    // RakPeer sends an immediate, one-shot UNRELIABLE connected ping before
    // exposing ID_NEW_INCOMING_CONNECTION to the native wrapper.
    const auto initialPing = waitForPacket(socket, 1000, [](const std::vector<uint8_t>& packet) {
        const auto frame = firstFrame(packet);
        return frame.has_value() && frame->payload.size() == 9 &&
            frame->payload[0] == 0x00;
    });
    if (!initialPing.has_value()) {
        return false;
    }
    const auto initialPingFrame = firstFrame(*initialPing);
    return initialPingFrame.has_value() && initialPingFrame->reliability == 0;
}

bool completeTimeoutPeerConnection(
    bedrock::RakNetServer& server,
    int socket,
    const sockaddr_in& target,
    uint8_t protocol,
    uint64_t clientGuid,
    uint16_t mtu,
    const std::atomic<int>& openCount,
    int expectedOpenCount
) {
    if (!sendPacket(socket, target, openConnectionRequest1(protocol, mtu))) {
        return false;
    }
    const auto reply1 = receivePacket(socket, 1000);
    if (!reply1.has_value() || reply1->empty() || (*reply1)[0] != 0x06) {
        return false;
    }

    if (!sendPacket(
            socket,
            target,
            openConnectionRequest2(server.boundPort(), mtu, clientGuid))) {
        return false;
    }
    const auto reply2 = receivePacket(socket, 1000);
    if (!reply2.has_value() || reply2->empty() || (*reply2)[0] != 0x08) {
        return false;
    }

    constexpr uint64_t requestTimestamp = 0x0102030405060708ull;
    if (!sendPacket(
            socket,
            target,
            connectedDatagram(0, connectionRequest(clientGuid, requestTimestamp)))) {
        return false;
    }
    const auto accepted = waitForPacket(socket, 1000, [](const std::vector<uint8_t>& packet) {
        const auto frame = firstFrame(packet);
        return frame.has_value() && !frame->payload.empty() &&
            frame->payload[0] == 0x10;
    });
    if (!accepted.has_value()) {
        return false;
    }
    const auto acceptedFrame = firstFrame(*accepted);
    if (!acceptedFrame.has_value() || acceptedFrame->reliability != 3 ||
        !sendPacket(socket, target, ackDatagram(acceptedFrame->datagramSequence))) {
        return false;
    }

    if (!sendPacket(
            socket,
            target,
            connectedDatagram(
                1,
                newIncomingConnection(
                    server.boundPort(),
                    requestTimestamp,
                    0x0807060504030201ull)))) {
        return false;
    }
    return waitForValue(openCount, expectedOpenCount, 1000);
}

bool expectFullRequest2(
    const bedrock::RakNetServer& server,
    int socket,
    const sockaddr_in& target,
    uint8_t protocol,
    uint64_t clientGuid,
    uint16_t mtu,
    uint64_t serverGuid
) {
    // RakPeer continues to answer Request1 when maximum incoming connections
    // has already been reached.
    if (!sendPacket(socket, target, openConnectionRequest1(protocol, mtu))) {
        return false;
    }
    const auto reply1 = receivePacket(socket, 1000);
    if (!reply1.has_value() || reply1->empty() || (*reply1)[0] != 0x06) {
        return false;
    }

    if (!sendPacket(
            socket,
            target,
            openConnectionRequest2(server.boundPort(), mtu, clientGuid))) {
        return false;
    }
    const auto reply2 = receivePacket(socket, 1000);
    std::vector<uint8_t> expected {0x14};
    appendMagic(expected);
    writeU64BE(expected, serverGuid);
    return reply2.has_value() && *reply2 == expected;
}

bool checkHalfOpenMaximumConnections() {
    constexpr uint8_t protocol = 11;
    constexpr uint16_t mtu = 1200;
    constexpr uint64_t serverGuid = 0x4848484848484848ull;
    constexpr uint64_t firstGuid = 0x1111222233334444ull;
    constexpr uint64_t secondGuid = 0x5555666677778888ull;

    std::atomic<int> openCount {0};
    std::atomic<int> closeCount {0};
    bedrock::RakNetServer server({
        .host = "127.0.0.1",
        .port = 0,
        .maxPlayers = 1,
        .protocolVersion = protocol,
        .serverGuid = serverGuid,
        .advertisement = "half-open-capacity-smoke"
    });
    server.onOpenConnection([&](const bedrock::RakNetServerPeer&) {
        ++openCount;
    });
    server.onCloseConnection([&](const bedrock::RakNetServerPeer&) {
        ++closeCount;
    });
    server.listen();

    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(server.boundPort());
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);

    std::vector<int> sockets;
    for (int index = 0; index < 3; ++index) {
        const int socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket < 0) {
            for (const int openSocket : sockets) ::close(openSocket);
            server.close();
            return false;
        }
        sockets.push_back(socket);
    }
    const auto finish = [&]() {
        server.close();
        for (const int socket : sockets) ::close(socket);
    };
    const auto fail = [&](const char* message) {
        std::cerr << "[RAKNET-LIFECYCLE-SMOKE] " << message << "\n";
        finish();
        return false;
    };
    const auto requestHalfOpen = [&](int socket, uint64_t clientGuid) {
        if (!sendPacket(socket, target, openConnectionRequest1(protocol, mtu))) {
            return false;
        }
        const auto reply1 = receivePacket(socket, 1000);
        if (!reply1.has_value() || reply1->empty() || (*reply1)[0] != 0x06) {
            return false;
        }
        if (!sendPacket(
                socket,
                target,
                openConnectionRequest2(server.boundPort(), mtu, clientGuid))) {
            return false;
        }
        const auto reply2 = receivePacket(socket, 1000);
        return reply2.has_value() && !reply2->empty() && (*reply2)[0] == 0x08;
    };
    const auto repeatRequest2 = [&](int socket, uint64_t clientGuid) {
        if (!sendPacket(
                socket,
                target,
                openConnectionRequest2(server.boundPort(), mtu, clientGuid))) {
            return false;
        }
        const auto reply2 = receivePacket(socket, 1000);
        return reply2.has_value() && !reply2->empty() && (*reply2)[0] == 0x08;
    };
    const auto waitForConnectionAccepted = [&](int socket, int timeoutMs) {
        return waitForPacket(socket, timeoutMs, [](const std::vector<uint8_t>& packet) {
            const auto payload = firstFramePayload(packet);
            return payload.has_value() && !payload->empty() && (*payload)[0] == 0x10;
        }).has_value();
    };

    if (!requestHalfOpen(sockets[0], firstGuid)) {
        return fail("first half-open Request2 did not receive Reply2");
    }
    // The local timestamp is deliberately captured after Reply2. The native
    // Request2 allocation therefore happened no later than this point, which
    // leaves conservative margins on both sides of the 10 second boundary.
    const auto leaseStart = std::chrono::steady_clock::now();

    if (!requestHalfOpen(sockets[1], secondGuid)) {
        return fail("half-open Request2 did not receive native-style Reply2");
    }

    // Startup(maxConnections=1) allocated its sole RemoteSystem entry to the
    // first Request2. Native still returns Reply2 to the second Request2, but
    // its subsequent connected datagram is completely ignored (even no ACK).
    if (!sendPacket(
            sockets[1],
            target,
            connectedDatagram(0, connectionRequest(secondGuid, 2)))) {
        return fail("failed second half-open connection request");
    }
    if (receivePacket(sockets[1], 250).has_value()) {
        return fail("unallocated half-open peer received a connected response");
    }

    if (openCount.load() != 0 || closeCount.load() != 0) {
        return fail("half-open allocation emitted a connection callback");
    }

    // Duplicate Request2 at roughly nine seconds must not renew
    // RemoteSystemStruct::connectionTime. Transitioning the allocated peer to
    // HANDLING_CONNECTION_REQUEST must not renew it either.
    std::this_thread::sleep_until(leaseStart + std::chrono::milliseconds(8500));
    if (!repeatRequest2(sockets[0], firstGuid)) {
        return fail("duplicate half-open Request2 did not receive Reply2");
    }

    constexpr uint64_t firstRequestTimestamp = 1;
    if (!sendPacket(
            sockets[0],
            target,
            connectedDatagram(
                0,
                connectionRequest(firstGuid, firstRequestTimestamp)))) {
        return fail("failed half-open HANDLING transition");
    }
    if (!waitForConnectionAccepted(sockets[0], 1000)) {
        return fail("half-open peer did not enter HANDLING state");
    }

    // Re-attempt close to the boundary while the first state still occupies
    // the only physical slot. Reply2 is still returned, but the second peer
    // remains unallocated and receives neither ACK nor ConnectionAccepted.
    std::this_thread::sleep_until(leaseStart + std::chrono::milliseconds(9500));
    if (!repeatRequest2(sockets[1], secondGuid)) {
        return fail("pre-expiry Request2 did not receive Reply2");
    }
    if (!sendPacket(
            sockets[1],
            target,
            connectedDatagram(1, connectionRequest(secondGuid, 2)))) {
        return fail("failed pre-expiry connection request");
    }
    if (receivePacket(sockets[1], 250).has_value()) {
        return fail("half-open slot was released before the strict boundary");
    }
    if (openCount.load() != 0 || closeCount.load() != 0) {
        return fail("HANDLING half-open peer emitted a connection callback");
    }

    // Wait safely beyond the original Request2 time. If either duplicate
    // Request2 or ConnectionRequest refreshed the lease, the second peer will
    // still be unable to obtain the slot here.
    std::this_thread::sleep_until(leaseStart + std::chrono::milliseconds(10350));
    if (!repeatRequest2(sockets[1], secondGuid)) {
        return fail("post-expiry Request2 did not receive Reply2");
    }
    if (!sendPacket(
            sockets[1],
            target,
            connectedDatagram(2, connectionRequest(secondGuid, 3)))) {
        return fail("failed post-expiry connection request");
    }
    if (!waitForConnectionAccepted(sockets[1], 1000)) {
        return fail("expired half-open slot was not reusable");
    }
    if (openCount.load() != 0 || closeCount.load() != 0) {
        return fail("silent half-open expiry emitted a connection callback");
    }

    if (!sendPacket(
            sockets[1],
            target,
            connectedDatagram(
                3,
                newIncomingConnection(server.boundPort(), 3, 4)))) {
        return fail("failed replacement peer new incoming connection");
    }
    if (!waitForValue(openCount, 1, 1000)) {
        return fail("replacement peer did not open");
    }

    if (!expectFullRequest2(
            server,
            sockets[2],
            target,
            protocol,
            0x9999aaaabbbbccccull,
            mtu,
            serverGuid)) {
        return fail("connected peer did not make later Request2 report full");
    }
    if (closeCount.load() != 0) {
        return fail("half-open lifecycle emitted a close callback");
    }

    finish();
    return true;
}

bool checkMaximumIncomingConnections(
    int configuredMaxPlayers,
    int expectedCapacity,
    bool checkReleasedSlot,
    uint64_t serverGuid
) {
    constexpr uint8_t protocol = 11;
    constexpr uint16_t mtu = 1200;

    std::atomic<int> openCount {0};
    std::atomic<int> closeCount {0};
    bedrock::RakNetServer server({
        .host = "127.0.0.1",
        .port = 0,
        .maxPlayers = configuredMaxPlayers,
        .protocolVersion = protocol,
        .serverGuid = serverGuid,
        .advertisement = "maximum-incoming-connections-smoke"
    });
    server.onOpenConnection([&](const bedrock::RakNetServerPeer&) {
        ++openCount;
    });
    server.onCloseConnection([&](const bedrock::RakNetServerPeer&) {
        ++closeCount;
    });
    server.listen();

    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(server.boundPort());
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);

    std::vector<int> sockets;
    const auto finish = [&]() {
        server.close();
        for (const int socket : sockets) {
            ::close(socket);
        }
    };
    const auto fail = [&](const char* message) {
        std::cerr << "[RAKNET-LIFECYCLE-SMOKE] " << message
                  << ": configuredMaxPlayers=" << configuredMaxPlayers << "\n";
        finish();
        return false;
    };
    const auto makeSocket = [&]() {
        const int socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket >= 0) {
            sockets.push_back(socket);
        }
        return socket;
    };

    if (server.options().maxPlayers != configuredMaxPlayers) {
        return fail("configured maxPlayers was mutated");
    }

    for (int index = 0; index < expectedCapacity; ++index) {
        const int socket = makeSocket();
        if (socket < 0 || !completePeerConnection(
                server,
                socket,
                target,
                protocol,
                0x1000000000000000ull + static_cast<uint64_t>(index),
                mtu,
                openCount,
                index + 1)) {
            return fail("failed to fill incoming connection slot");
        }
    }

    const int refusedSocket = makeSocket();
    if (refusedSocket < 0 || !expectFullRequest2(
            server,
            refusedSocket,
            target,
            protocol,
            0x2000000000000000ull,
            mtu,
            serverGuid)) {
        return fail("full server did not return exact no-free response");
    }

    if (checkReleasedSlot) {
        if (sockets.empty() || !sendPacket(
                sockets.front(),
                target,
                connectedDatagram(2, std::vector<uint8_t>{0x15}))) {
            return fail("failed to disconnect capacity peer");
        }
        if (!waitForValue(closeCount, 1, 1000)) {
            return fail("disconnect did not release capacity peer");
        }

        const int replacementSocket = makeSocket();
        if (replacementSocket < 0 || !completePeerConnection(
                server,
                replacementSocket,
                target,
                protocol,
                0x3000000000000000ull,
                mtu,
                openCount,
                expectedCapacity + 1)) {
            return fail("released incoming connection slot was not reusable");
        }
    }

    finish();
    return true;
}

bool checkConnectedTimeoutLifecycle() {
    constexpr uint8_t protocol = 11;
    constexpr uint16_t mtu = 1200;
    constexpr uint64_t serverGuid = 0x5454545454545454ull;
    constexpr uint64_t responsiveGuid = 0x1111111122222222ull;
    constexpr uint64_t silentGuid = 0x3333333344444444ull;
    constexpr uint64_t replacementGuid = 0x5555555566666666ull;
    const std::vector<uint8_t> callbackPayload {0xfe, 0xde, 0xad};
    const std::vector<uint8_t> applicationPayload {0xfe, 0x42};

    std::atomic<int> openCount {0};
    std::atomic<int> closeCount {0};
    std::atomic<int> responsiveCloseCount {0};
    std::atomic<int> silentCloseCount {0};
    std::atomic<int> applicationCount {0};
    std::atomic<int64_t> malformedSentAtMs {-1};
    std::atomic<int64_t> offlinePingSentAtMs {-1};
    std::atomic<int64_t> silentClosedAtMs {-1};
    std::atomic<bool> callbackSendAttempted {false};
    std::atomic<bool> callbackPeerMismatch {false};
    std::atomic<bool> applicationMismatch {false};

    const auto steadyMillis = []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    };

    bedrock::RakNetServer server({
        .host = "127.0.0.1",
        .port = 0,
        .maxPlayers = 2,
        .protocolVersion = protocol,
        .serverGuid = serverGuid,
        .advertisement = "connected-timeout-smoke"
    });
    server.onOpenConnection([&](const bedrock::RakNetServerPeer&) {
        ++openCount;
    });
    server.onCloseConnection([&](const bedrock::RakNetServerPeer& peer) {
        if (peer.clientGuid == silentGuid) {
            ++silentCloseCount;
            silentClosedAtMs = steadyMillis();
            callbackSendAttempted = true;
            // Native removes the remote system before ID_CONNECTION_LOST is
            // surfaced. This must therefore be a no-op and must not deadlock.
            server.sendReliable(peer, callbackPayload);
        } else if (peer.clientGuid == responsiveGuid) {
            ++responsiveCloseCount;
        } else {
            callbackPeerMismatch = true;
        }
        ++closeCount;
    });
    server.onEncapsulated([&](
        const bedrock::RakNetServerPeer& peer,
        const std::vector<uint8_t>& payload
    ) {
        if (peer.clientGuid != responsiveGuid || payload != applicationPayload) {
            applicationMismatch = true;
        }
        ++applicationCount;
    });
    server.listen();

    std::array<int, 4> sockets {-1, -1, -1, -1};
    for (auto& socket : sockets) {
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket < 0) {
            server.close();
            for (const int openSocket : sockets) {
                if (openSocket >= 0) ::close(openSocket);
            }
            std::cerr << "[RAKNET-LIFECYCLE-SMOKE] connected-timeout socket failed\n";
            return false;
        }
    }

    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(server.boundPort());
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);

    const auto finish = [&]() {
        server.close();
        for (const int socket : sockets) {
            ::close(socket);
        }
    };
    const auto fail = [&](const char* message) {
        std::cerr << "[RAKNET-LIFECYCLE-SMOKE] " << message << "\n";
        finish();
        return false;
    };

    if (!completeTimeoutPeerConnection(
            server,
            sockets[0],
            target,
            protocol,
            responsiveGuid,
            mtu,
            openCount,
            1) ||
        !completeTimeoutPeerConnection(
            server,
            sockets[1],
            target,
            protocol,
            silentGuid,
            mtu,
            openCount,
            2)) {
        return fail("failed to establish connected-timeout peers");
    }

    // Native connected-pong is UNRELIABLE, independently of the reliable
    // heartbeat used for delivery-timeout detection.
    std::vector<uint8_t> clientPing {0x00};
    writeU64BE(clientPing, 0x0102030405060708ull);
    if (!sendPacket(sockets[0], target, connectedDatagram(2, clientPing))) {
        return fail("failed to send connected ping");
    }
    const auto pong = waitForPacket(sockets[0], 1000, [](const std::vector<uint8_t>& packet) {
        const auto frame = firstFrame(packet);
        return frame.has_value() && !frame->payload.empty() &&
            frame->payload[0] == 0x03;
    });
    if (!pong.has_value()) {
        return fail("missing connected pong");
    }
    const auto pongFrame = firstFrame(*pong);
    if (!pongFrame.has_value() || pongFrame->reliability != 0) {
        return fail("connected pong was not native UNRELIABLE");
    }

    // ReliabilityLayer refreshes timeLastDatagramArrived before parsing any
    // assigned endpoint datagram longer than two bytes. The unresponsive peer
    // sends one deliberately malformed packet, then never ACKs its heartbeat.
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    malformedSentAtMs = steadyMillis();
    if (!sendPacket(sockets[1], target, std::vector<uint8_t>{0x42, 0xaa, 0xbb})) {
        return fail("failed to send malformed liveness datagram");
    }

    // A valid magic-bearing offline ping is consumed by
    // ProcessOfflineNetworkPacket before ReliabilityLayer. It receives its
    // pong but must not refresh the connected peer's delivery-timeout clock.
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    offlinePingSentAtMs = steadyMillis();
    if (!sendPacket(
            sockets[1],
            target,
            unconnectedPing(0x1112131415161718ull))) {
        return fail("failed to send assigned-endpoint offline ping");
    }
    const auto offlinePong = waitForPacket(
        sockets[1],
        1000,
        [](const std::vector<uint8_t>& packet) {
            return !packet.empty() && packet[0] == 0x1c;
        }
    );
    if (!offlinePong.has_value()) {
        return fail("assigned-endpoint offline ping was not processed");
    }

    int responsiveHeartbeatCount = 0;
    int silentHeartbeatCount = 0;
    bool invalidHeartbeatReliability = false;
    bool closePacketOnWire = false;
    bool callbackPayloadOnWire = false;
    bool ioFailure = false;
    const auto observationDeadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(36000);
    while (std::chrono::steady_clock::now() < observationDeadline) {
        for (int index = 0; index < 2; ++index) {
            const auto packet = receivePacket(sockets[index], 10);
            if (!packet.has_value()) {
                continue;
            }
            const auto frame = firstFrame(*packet);
            if (!frame.has_value() || frame->payload.empty()) {
                continue;
            }
            if (frame->payload[0] == 0x00 && frame->payload.size() == 9) {
                if (frame->reliability != 2) {
                    invalidHeartbeatReliability = true;
                    continue;
                }
                if (index == 0) {
                    ++responsiveHeartbeatCount;
                    if (!sendPacket(
                            sockets[0],
                            target,
                            ackDatagram(frame->datagramSequence))) {
                        ioFailure = true;
                    }
                } else {
                    ++silentHeartbeatCount;
                }
                continue;
            }
            if (frame->payload[0] == 0x15 || frame->payload[0] == 0x16) {
                closePacketOnWire = true;
            }
            if (frame->payload == callbackPayload) {
                callbackPayloadOnWire = true;
            }
        }

        if (ioFailure || closeCount.load() > 1) {
            break;
        }
        if (closeCount.load() == 1 &&
            steadyMillis() - malformedSentAtMs.load() >= 30500) {
            break;
        }
    }

    const int64_t timeoutElapsed =
        silentClosedAtMs.load() - malformedSentAtMs.load();
    const int64_t timeoutAfterOfflinePing =
        silentClosedAtMs.load() - offlinePingSentAtMs.load();
    if (ioFailure) {
        return fail("failed to ACK responsive heartbeat");
    }
    if (responsiveHeartbeatCount < 2 || silentHeartbeatCount < 1) {
        return fail("native reliable heartbeat cadence mismatch");
    }
    if (invalidHeartbeatReliability) {
        return fail("delivery heartbeat was not RELIABLE without ordering");
    }
    if (closeCount.load() != 1 || silentCloseCount.load() != 1 ||
        responsiveCloseCount.load() != 0 || callbackPeerMismatch.load()) {
        return fail("connection-lost callback count or peer mismatch");
    }
    // The malformed packet arrived roughly one second after connection. If it
    // had not refreshed ReliabilityLayer state, closure would be about 29s
    // after this timestamp rather than strictly beyond 30s.
    if (timeoutElapsed < 29800 || timeoutElapsed > 36000) {
        return fail("connected delivery timeout boundary mismatch");
    }
    if (timeoutAfterOfflinePing < 25000 || timeoutAfterOfflinePing >= 29500) {
        return fail("recognized offline ping incorrectly refreshed liveness");
    }
    if (!callbackSendAttempted.load() || closePacketOnWire ||
        callbackPayloadOnWire) {
        return fail("local connection-lost emitted forbidden wire traffic");
    }

    // ACKing each reliable ping keeps this peer alive and allows the next
    // half-timeout heartbeat. It must still accept application traffic after
    // the other peer has crossed 30 seconds without an incoming datagram.
    if (!sendPacket(
            sockets[0],
            target,
            connectedDatagram(3, applicationPayload)) ||
        !waitForValue(applicationCount, 1, 1000)) {
        return fail("ACK-responsive peer did not survive beyond 30 seconds");
    }
    if (applicationMismatch.load()) {
        return fail("post-timeout application delivery mismatch");
    }

    // Transport erasure happens before the callback and releases the physical
    // RemoteSystem slot for a new connected peer.
    if (!completeTimeoutPeerConnection(
            server,
            sockets[2],
            target,
            protocol,
            replacementGuid,
            mtu,
            openCount,
            3)) {
        return fail("timed-out transport slot was not reusable");
    }
    if (!expectFullRequest2(
            server,
            sockets[3],
            target,
            protocol,
            0x7777777788888888ull,
            mtu,
            serverGuid)) {
        return fail("responsive plus replacement peers did not refill capacity");
    }
    if (closeCount.load() != 1) {
        return fail("connection-lost callback was emitted more than once");
    }

    std::cout << "[RAKNET-LIFECYCLE-SMOKE] connected timeout elapsed="
              << timeoutElapsed << "ms, responsive heartbeats="
              << responsiveHeartbeatCount << "\n";
    finish();
    return true;
}

bool checkOfflinePacketLivenessRouting() {
    constexpr uint8_t protocol = 11;
    constexpr uint16_t mtu = 1200;
    constexpr uint64_t serverGuid = 0x6161616161616161ull;
    constexpr uint64_t clientGuid = 0x7171717171717171ull;
    const std::vector<uint8_t> callbackPayload {0xfe, 0x71};

    std::atomic<int> openCount {0};
    std::atomic<int> closeCount {0};
    std::atomic<int64_t> closedAtMs {-1};
    const auto steadyMillis = []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    };

    bedrock::RakNetServer server({
        .host = "127.0.0.1",
        .port = 0,
        .maxPlayers = 1,
        .protocolVersion = protocol,
        .serverGuid = serverGuid,
        .advertisement = "offline-liveness-routing-smoke"
    });
    server.onOpenConnection([&](const bedrock::RakNetServerPeer&) {
        ++openCount;
    });
    server.onCloseConnection([&](const bedrock::RakNetServerPeer& peer) {
        closedAtMs = steadyMillis();
        ++closeCount;
        server.sendReliable(peer, callbackPayload);
    });
    server.listen();

    const int socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket < 0) {
        server.close();
        return false;
    }
    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(server.boundPort());
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
    const auto finish = [&]() {
        server.close();
        ::close(socket);
    };
    const auto fail = [&](const char* message) {
        std::cerr << "[RAKNET-LIFECYCLE-SMOKE] " << message << "\n";
        finish();
        return false;
    };

    if (!completeTimeoutPeerConnection(
            server,
            socket,
            target,
            protocol,
            clientGuid,
            mtu,
            openCount,
            1)) {
        return fail("failed offline-routing peer connection");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    std::vector<uint8_t> truncatedRequest1 {0x05};
    appendMagic(truncatedRequest1);
    const int64_t truncatedSentAtMs = steadyMillis();
    if (!sendPacket(socket, target, truncatedRequest1)) {
        return fail("failed truncated Request1 liveness packet");
    }
    // Magic alone is insufficient: native requires at least 25 bytes before
    // treating this ID as offline, so the 17-byte packet reaches and refreshes
    // ReliabilityLayer but produces no Reply1.
    const auto truncatedResponse = receivePacket(socket, 100);
    if (truncatedResponse.has_value() && !truncatedResponse->empty() &&
        (*truncatedResponse)[0] == 0x06) {
        return fail("truncated Request1 was incorrectly processed offline");
    }

    std::this_thread::sleep_until(
        std::chrono::steady_clock::time_point(
            std::chrono::milliseconds(truncatedSentAtMs + 3000)
        )
    );
    const int64_t validRequestSentAtMs = steadyMillis();
    if (!sendPacket(socket, target, openConnectionRequest1(protocol, mtu))) {
        return fail("failed valid Request1 routing packet");
    }
    const auto reply1 = waitForPacket(
        socket,
        1000,
        [](const std::vector<uint8_t>& packet) {
            return !packet.empty() && packet[0] == 0x06;
        }
    );
    if (!reply1.has_value()) {
        return fail("valid Request1 was not processed offline");
    }

    int heartbeatCount = 0;
    bool invalidHeartbeatReliability = false;
    bool closePacketOnWire = false;
    bool callbackPayloadOnWire = false;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(36000);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto packet = receivePacket(socket, 20);
        if (packet.has_value()) {
            const auto frame = firstFrame(*packet);
            if (frame.has_value() && !frame->payload.empty()) {
                if (frame->payload[0] == 0x00 && frame->payload.size() == 9) {
                    ++heartbeatCount;
                    if (frame->reliability != 2) {
                        invalidHeartbeatReliability = true;
                    }
                }
                if (frame->payload[0] == 0x15 || frame->payload[0] == 0x16) {
                    closePacketOnWire = true;
                }
                if (frame->payload == callbackPayload) {
                    callbackPayloadOnWire = true;
                }
            }
        }
        if (closeCount.load() == 1 &&
            steadyMillis() - truncatedSentAtMs >= 30500) {
            break;
        }
        if (closeCount.load() > 1) {
            break;
        }
    }

    const int64_t elapsedAfterTruncated = closedAtMs.load() - truncatedSentAtMs;
    const int64_t elapsedAfterValid = closedAtMs.load() - validRequestSentAtMs;
    if (heartbeatCount < 1 || invalidHeartbeatReliability) {
        return fail("offline-routing heartbeat mismatch");
    }
    if (closeCount.load() != 1) {
        return fail("offline-routing close callback count mismatch");
    }
    if (elapsedAfterTruncated < 29800 || elapsedAfterTruncated > 36000) {
        return fail("truncated Request1 did not refresh ReliabilityLayer");
    }
    if (elapsedAfterValid < 25000 || elapsedAfterValid >= 29500) {
        return fail("valid Request1 incorrectly refreshed ReliabilityLayer");
    }
    if (closePacketOnWire || callbackPayloadOnWire) {
        return fail("offline-routing timeout emitted wire close traffic");
    }

    finish();
    return true;
}

} // namespace

int main() {
    constexpr uint8_t serverProtocol = 11;
    constexpr uint64_t serverGuid = 0x0102030405060708ull;
    constexpr uint64_t clientGuid = 0x1122334455667788ull;
    constexpr uint16_t mtu = 1200;

    if (!checkProtocolCompatibility(11, 0x1111111111111111ull, {9, 10, 11, 12}) ||
        !checkProtocolCompatibility(10, 0x1010101010101010ull, {9, 10, 11, 12})) {
        return 1;
    }
    bool connectedTimeoutOk = false;
    bool offlineRoutingOk = false;
    std::thread connectedTimeoutThread([&]() {
        connectedTimeoutOk = checkConnectedTimeoutLifecycle();
    });
    std::thread offlineRoutingThread([&]() {
        offlineRoutingOk = checkOfflinePacketLivenessRouting();
    });
    const bool capacityAndLeaseOk =
        checkMaximumIncomingConnections(1, 1, true, 0x4141414141414141ull) &&
        checkMaximumIncomingConnections(0, 3, false, 0x4343434343434343ull) &&
        checkHalfOpenMaximumConnections();
    connectedTimeoutThread.join();
    offlineRoutingThread.join();
    if (!capacityAndLeaseOk || !connectedTimeoutOk || !offlineRoutingOk) {
        return 1;
    }

    std::atomic<int> openCount {0};
    std::atomic<int> appCount {0};
    std::atomic<bool> peerMismatch {false};
    std::atomic<bool> payloadMismatch {false};
    const std::vector<uint8_t> postOpenPayload {0xfe, 0xca, 0xfe};

    bedrock::RakNetServer server({
        .host = "127.0.0.1",
        .port = 0,
        .maxPlayers = 3,
        .protocolVersion = serverProtocol,
        .serverGuid = serverGuid,
        .advertisement = "lifecycle-smoke"
    });
    server.onOpenConnection([&](const bedrock::RakNetServerPeer& peer) {
        if (peer.clientGuid != clientGuid || peer.mtu != mtu) {
            peerMismatch = true;
        }
        ++openCount;
    });
    server.onEncapsulated([&](
        const bedrock::RakNetServerPeer&,
        const std::vector<uint8_t>& payload
    ) {
        if (payload != postOpenPayload) {
            payloadMismatch = true;
        }
        ++appCount;
    });
    server.listen();

    const int socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket < 0) {
        std::cerr << "[RAKNET-LIFECYCLE-SMOKE] socket failed\n";
        server.close();
        return 1;
    }

    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(server.boundPort());
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);

    const auto run = [&]() -> bool {
        const auto fail = [](const char* message) {
            std::cerr << "[RAKNET-LIFECYCLE-SMOKE] " << message << "\n";
            return false;
        };

        if (!sendPacket(socket, target, openConnectionRequest1(12, mtu))) {
            return fail("failed to send incompatible request1");
        }
        auto incompatible = receivePacket(socket, 1000);
        std::vector<uint8_t> expectedIncompatible {0x19, serverProtocol};
        appendMagic(expectedIncompatible);
        writeU64BE(expectedIncompatible, serverGuid);
        if (!incompatible.has_value() || *incompatible != expectedIncompatible) {
            return fail("incompatible protocol response mismatch");
        }

        if (!sendPacket(socket, target, openConnectionRequest1(serverProtocol, mtu))) {
            return fail("failed to send request1");
        }
        auto reply1 = receivePacket(socket, 1000);
        if (!reply1.has_value() || reply1->empty() || (*reply1)[0] != 0x06) {
            return fail("missing open connection reply1");
        }

        if (!sendPacket(socket, target, openConnectionRequest2(server.boundPort(), mtu, clientGuid))) {
            return fail("failed to send request2");
        }
        auto reply2 = receivePacket(socket, 1000);
        if (!reply2.has_value() || reply2->empty() || (*reply2)[0] != 0x08) {
            return fail("missing open connection reply2");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (openCount.load() != 0) {
            return fail("request2 emitted open callback");
        }

        constexpr uint64_t requestTimestamp = 0x0102030405060708ull;
        if (!sendPacket(
                socket,
                target,
                connectedDatagram(0, connectionRequest(clientGuid, requestTimestamp)))) {
            return fail("failed to send connection request");
        }
        auto accepted = waitForPacket(socket, 1000, [](const std::vector<uint8_t>& packet) {
            auto payload = firstFramePayload(packet);
            return payload.has_value() && !payload->empty() && (*payload)[0] == 0x10;
        });
        if (!accepted.has_value()) {
            return fail("missing connection request accepted");
        }
        if (openCount.load() != 0) {
            return fail("connection request emitted open callback");
        }

        const auto validIncoming = newIncomingConnection(
            server.boundPort(),
            requestTimestamp,
            2
        );
        if (!sendPacket(
                socket,
                target,
                connectedDatagram(1, validIncoming))) {
            return fail("failed to send valid new incoming connection");
        }
        if (!waitForValue(openCount, 1, 1000)) {
            return fail("valid new incoming connection did not emit open callback");
        }

        if (!sendPacket(socket, target, connectedDatagram(2, validIncoming))) {
            return fail("failed to send duplicate new incoming connection");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (openCount.load() != 1) {
            return fail("open callback was not emitted exactly once");
        }

        if (!sendPacket(socket, target, connectedDatagram(3, postOpenPayload))) {
            return fail("failed to send post-open application payload");
        }
        if (!waitForValue(appCount, 1, 1000)) {
            return fail("post-open application payload was not delivered");
        }
        if (peerMismatch.load()) {
            return fail("open callback peer mismatch");
        }
        if (payloadMismatch.load()) {
            return fail("encapsulated payload mismatch");
        }
        return true;
    };

    const bool ok = run();
    ::close(socket);
    server.close();
    if (ok) {
        std::cout << "[RAKNET-LIFECYCLE-SMOKE] ok\n";
    }
    return ok ? 0 : 1;
}
