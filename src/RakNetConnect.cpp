#include "bedrock/RakNetConnect.hpp"

#include "bedrock/detail/PlatformSocket.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace bedrock {

static const uint8_t RAKNET_MAGIC[16] = {
    0x00, 0xff, 0xff, 0x00,
    0xfe, 0xfe, 0xfe, 0xfe,
    0xfd, 0xfd, 0xfd, 0xfd,
    0x12, 0x34, 0x56, 0x78
};

static constexpr uint8_t ID_OPEN_CONNECTION_REQUEST_1 = 0x05;
static constexpr uint8_t ID_OPEN_CONNECTION_REPLY_1   = 0x06;
static constexpr uint8_t ID_OPEN_CONNECTION_REQUEST_2 = 0x07;
static constexpr uint8_t ID_OPEN_CONNECTION_REPLY_2   = 0x08;

static constexpr uint8_t RAKNET_PROTOCOL_VERSION = 11;

// IPv4 header 20 bytes + UDP header 8 bytes.
// Для Request1 UDP payload обычно mtu - 28.
static constexpr int UDP_IPV4_HEADER_SIZE = 28;

static void writeU16BE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(v & 0xff));
}

static void writeU32BE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(v & 0xff));
}

static void writeU64BE(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
    }
}

static uint16_t readU16BE(const std::vector<uint8_t>& data, size_t& off) {
    if (off + 2 > data.size()) {
        throw std::runtime_error("readU16BE out of range");
    }

    uint16_t v =
        static_cast<uint16_t>(static_cast<uint16_t>(data[off]) << 8) |
        static_cast<uint16_t>(data[off + 1]);

    off += 2;
    return v;
}

static uint32_t readU32BE(const std::vector<uint8_t>& data, size_t& off) {
    if (off + 4 > data.size()) {
        throw std::runtime_error("readU32BE out of range");
    }

    uint32_t v =
        (static_cast<uint32_t>(data[off]) << 24) |
        (static_cast<uint32_t>(data[off + 1]) << 16) |
        (static_cast<uint32_t>(data[off + 2]) << 8) |
        static_cast<uint32_t>(data[off + 3]);

    off += 4;
    return v;
}

static uint64_t readU64BE(const std::vector<uint8_t>& data, size_t& off) {
    if (off + 8 > data.size()) {
        throw std::runtime_error("readU64BE out of range");
    }

    uint64_t v = 0;

    for (int i = 0; i < 8; i++) {
        v = (v << 8) | static_cast<uint64_t>(data[off + i]);
    }

    off += 8;
    return v;
}

static bool checkMagic(const std::vector<uint8_t>& data, size_t off) {
    if (off + 16 > data.size()) {
        return false;
    }

    for (size_t i = 0; i < 16; i++) {
        if (data[off + i] != RAKNET_MAGIC[i]) {
            return false;
        }
    }

    return true;
}

static uint64_t makeGuid() {
    auto now = std::chrono::high_resolution_clock::now()
        .time_since_epoch()
        .count();

    std::random_device rd;

    uint64_t a = static_cast<uint64_t>(now);
    uint64_t b = static_cast<uint64_t>(rd());

    return (a << 16) ^ b ^ 0xC0FFEEBADC0DEULL;
}

static std::string sockaddrToIp(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] {};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip);
}

static void writeRakNetAddressIPv4(
    std::vector<uint8_t>& out,
    const std::string& ip,
    uint16_t port
) {
    in_addr addr {};

    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        throw std::runtime_error("inet_pton failed for IPv4 address: " + ip);
    }

    const uint8_t* b = reinterpret_cast<const uint8_t*>(&addr.s_addr);

    out.push_back(4);

    // RakNet IPv4 bytes are bitwise NOT.
    out.push_back(static_cast<uint8_t>(~b[0]));
    out.push_back(static_cast<uint8_t>(~b[1]));
    out.push_back(static_cast<uint8_t>(~b[2]));
    out.push_back(static_cast<uint8_t>(~b[3]));

    writeU16BE(out, port);
}

static std::pair<std::string, uint16_t> readRakNetAddressIPv4(
    const std::vector<uint8_t>& data,
    size_t& off
) {
    if (off + 7 > data.size()) {
        throw std::runtime_error("readRakNetAddressIPv4 out of range");
    }

    uint8_t version = data[off++];

    if (version != 4) {
        std::ostringstream ss;
        ss << "unsupported RakNet address version: " << static_cast<int>(version);
        throw std::runtime_error(ss.str());
    }

    uint8_t ip0 = static_cast<uint8_t>(~data[off++]);
    uint8_t ip1 = static_cast<uint8_t>(~data[off++]);
    uint8_t ip2 = static_cast<uint8_t>(~data[off++]);
    uint8_t ip3 = static_cast<uint8_t>(~data[off++]);

    uint16_t port = readU16BE(data, off);

    std::ostringstream ip;
    ip << static_cast<int>(ip0)
       << "."
       << static_cast<int>(ip1)
       << "."
       << static_cast<int>(ip2)
       << "."
       << static_cast<int>(ip3);

    return { ip.str(), port };
}

static bool recvPacket(
    detail::Socket sock,
    std::vector<uint8_t>& out,
    int timeoutMs
) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);

    timeval tv {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int ready = select(detail::selectWidth(sock), &readfds, nullptr, nullptr, &tv);

    if (ready <= 0) {
        return false;
    }

    out.assign(4096, 0);

    auto received = detail::receiveDatagram(sock, out.data(), out.size());

    if (received <= 0) {
        return false;
    }

    out.resize(static_cast<size_t>(received));
    return true;
}

static void sendPacket(
    detail::Socket sock,
    const sockaddr_in& target,
    const std::vector<uint8_t>& packet
) {
    auto sent = detail::sendDatagram(
        sock,
        packet.data(),
        packet.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    );

    if (sent < 0 || static_cast<size_t>(sent) != packet.size()) {
        throw std::runtime_error("sendto failed");
    }
}

static std::vector<int> buildMtuTryList(int requestedMtu) {
    std::vector<int> list;

    auto add = [&](int v) {
        if (v >= 576 && v <= 1492) {
            if (std::find(list.begin(), list.end(), v) == list.end()) {
                list.push_back(v);
            }
        }
    };

    add(requestedMtu);

    add(1492);
    add(1400);
    add(1300);
    add(1200);
    add(1100);
    add(1000);
    add(900);
    add(800);
    add(700);
    add(576);

    return list;
}

std::vector<uint8_t> RakNetConnector::buildOpenConnectionRequest1(int mtu) {
    std::vector<uint8_t> out;

    out.push_back(ID_OPEN_CONNECTION_REQUEST_1);
    out.insert(out.end(), std::begin(RAKNET_MAGIC), std::end(RAKNET_MAGIC));
    out.push_back(RAKNET_PROTOCOL_VERSION);

    int payloadSize = mtu - UDP_IPV4_HEADER_SIZE;

    if (payloadSize < static_cast<int>(out.size())) {
        throw std::runtime_error("MTU too small for OpenConnectionRequest1");
    }

    out.resize(static_cast<size_t>(payloadSize), 0);
    return out;
}

std::vector<uint8_t> RakNetConnector::buildOpenConnectionRequest2(
    const std::string& serverIp,
    uint16_t serverPort,
    int mtu,
    uint64_t clientGuid,
    bool serverSecurity,
    uint32_t securityCookie
) {
    std::vector<uint8_t> out;

    out.push_back(ID_OPEN_CONNECTION_REQUEST_2);
    out.insert(out.end(), std::begin(RAKNET_MAGIC), std::end(RAKNET_MAGIC));

    if (serverSecurity) {
        // Newer/secured RakNet handshake:
        // Request2 must echo server cookie,
        // then send "client supports security" as false.
        writeU32BE(out, securityCookie);
        out.push_back(0x00);
    }

    writeRakNetAddressIPv4(out, serverIp, serverPort);

    writeU16BE(out, static_cast<uint16_t>(mtu));
    writeU64BE(out, clientGuid);

    return out;
}

RakNetOpenResult RakNetConnector::openConnection(
    const std::string& host,
    uint16_t port,
    int mtu,
    int timeoutMs
) {
    RakNetOpenResult result;
    result.host = host;
    result.port = port;
    result.requestedMtu = mtu;
    result.clientGuid = makeGuid();

    addrinfo hints {};
    addrinfo* res = nullptr;
    detail::Socket sock = detail::invalidSocket;

    try {
        detail::ensureSockets();
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        std::string portStr = std::to_string(port);

        int gai = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);

        if (gai != 0) {
            result.error = std::string("getaddrinfo failed: ") + gai_strerror(gai);
            return result;
        }

        if (!res) {
            result.error = "getaddrinfo returned empty result";
            return result;
        }

        sockaddr_in target {};
        std::memcpy(&target, res->ai_addr, sizeof(sockaddr_in));

        result.resolvedIp = sockaddrToIp(target);
        result.resolvedPort = ntohs(target.sin_port);

        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (sock == detail::invalidSocket) {
            result.error = "socket failed";
            freeaddrinfo(res);
            return result;
        }

        std::vector<uint8_t> reply1;
        bool gotReply1 = false;
        int selectedMtu = -1;
        int selectedPayloadSize = -1;

        auto mtuList = buildMtuTryList(mtu);

        for (int candidateMtu : mtuList) {
            result.triedMtus.push_back(candidateMtu);

            auto req1 = buildOpenConnectionRequest1(candidateMtu);

            std::cout << "[REQ1] try mtu="
                      << candidateMtu
                      << " udpPayload="
                      << req1.size()
                      << "\n";

            sendPacket(sock, target, req1);

            if (recvPacket(sock, reply1, timeoutMs)) {
                gotReply1 = true;
                selectedMtu = candidateMtu;
                selectedPayloadSize = static_cast<int>(req1.size());
                break;
            }
        }

        if (!gotReply1) {
            result.error = "timeout waiting for OpenConnectionReply1 on all MTU candidates";
            detail::closeSocket(sock);
            freeaddrinfo(res);
            return result;
        }

        result.usedMtu = selectedMtu;
        result.request1PayloadSize = selectedPayloadSize;

        size_t off1 = 0;

        if (reply1.size() < 28) {
            result.error = "OpenConnectionReply1 too small";
            detail::closeSocket(sock);
            freeaddrinfo(res);
            return result;
        }

        uint8_t id1 = reply1[off1++];

        if (id1 != ID_OPEN_CONNECTION_REPLY_1) {
            std::ostringstream ss;
            ss << "unexpected reply1 packet id: 0x"
               << std::hex
               << static_cast<int>(id1);

            result.error = ss.str();
            detail::closeSocket(sock);
            freeaddrinfo(res);
            return result;
        }

        if (!checkMagic(reply1, off1)) {
            result.error = "invalid magic in OpenConnectionReply1";
            detail::closeSocket(sock);
            freeaddrinfo(res);
            return result;
        }

        off1 += 16;

        result.serverGuid = readU64BE(reply1, off1);

        if (off1 >= reply1.size()) {
            result.error = "OpenConnectionReply1 missing security byte";
            detail::closeSocket(sock);
            freeaddrinfo(res);
            return result;
        }

        result.serverSecurity = reply1[off1++] != 0;

        if (result.serverSecurity) {
            result.securityCookie = readU32BE(reply1, off1);
            result.hasSecurityCookie = true;

            std::cout << "[SECURITY] server has security cookie: "
                      << result.securityCookie
                      << "\n";
        }

        result.mtuFromReply1 = static_cast<int>(readU16BE(reply1, off1));

        int finalMtu = result.usedMtu;

        if (result.mtuFromReply1 >= 576 && result.mtuFromReply1 <= 1492) {
            finalMtu = result.mtuFromReply1;
        } else {
            std::cout << "[WARN] invalid mtuFromReply1="
                      << result.mtuFromReply1
                      << ", fallback to usedMtu="
                      << result.usedMtu
                      << "\n";
        }

        auto req2 = buildOpenConnectionRequest2(
            result.resolvedIp,
            port,
            finalMtu,
            result.clientGuid,
            result.serverSecurity,
            result.securityCookie
        );

        std::cout << "[REQ2] send mtu="
                  << finalMtu
                  << " payload="
                  << req2.size()
                  << " security="
                  << (result.serverSecurity ? "true" : "false")
                  << "\n";

        sendPacket(sock, target, req2);

        std::vector<uint8_t> reply2;

        if (!recvPacket(sock, reply2, timeoutMs)) {
            result.error = "timeout waiting for OpenConnectionReply2";
            detail::closeSocket(sock);
            freeaddrinfo(res);
            return result;
        }

        size_t off2 = 0;

        if (reply2.size() < 35) {
            result.error = "OpenConnectionReply2 too small";
            detail::closeSocket(sock);
            freeaddrinfo(res);
            return result;
        }

        uint8_t id2 = reply2[off2++];

        if (id2 != ID_OPEN_CONNECTION_REPLY_2) {
            std::ostringstream ss;
            ss << "unexpected reply2 packet id: 0x"
               << std::hex
               << static_cast<int>(id2);

            result.error = ss.str();
            detail::closeSocket(sock);
            freeaddrinfo(res);
            return result;
        }

        if (!checkMagic(reply2, off2)) {
            result.error = "invalid magic in OpenConnectionReply2";
            detail::closeSocket(sock);
            freeaddrinfo(res);
            return result;
        }

        off2 += 16;

        result.serverGuid = readU64BE(reply2, off2);

        auto clientAddr = readRakNetAddressIPv4(reply2, off2);
        result.clientAddressFromServer = clientAddr.first;
        result.clientPortFromServer = clientAddr.second;

        result.mtuFromReply2 = static_cast<int>(readU16BE(reply2, off2));

        if (off2 < reply2.size()) {
            result.serverSecurity = reply2[off2++] != 0;
        }

        result.ok = true;

        detail::closeSocket(sock);
        freeaddrinfo(res);
        return result;
    } catch (const std::exception& e) {
        if (sock != detail::invalidSocket) {
            detail::closeSocket(sock);
        }

        if (res) {
            freeaddrinfo(res);
        }

        result.error = e.what();
        return result;
    }
}

} // namespace bedrock
