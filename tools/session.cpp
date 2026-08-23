#include "bedrock/BedrockFramer.hpp"
#include "bedrock/BedrockKeyExchange.hpp"
#include "bedrock/BedrockEncryption.hpp"
#include <bedrock/events/BedrockPacketEventDispatcher.hpp>
#include <bedrock/legacy/Client.hpp>
#include <bedrock/PacketFromEvent.hpp>
#include <bedrock/protocol/PacketRegistry.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protocol/PacketFactory.hpp>
#include <bedrock/protocol/VersionedPacketCodec.hpp>
#include <bedrock/protodef/ProtoDefJson.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <bedrock/events/PacketEventApi.hpp>

static bool g_packetDumpMode = false;
static bool g_packetJsonMode = false;
static bool g_decodeEvents = false;
static std::string g_minecraftVersion;

static std::vector<int> parseVersionParts(const std::string& version) {
    std::vector<int> out;
    std::string cur;

    for (char c : version) {
        if (c == '.') {
            out.push_back(cur.empty() ? 0 : std::stoi(cur));
            cur.clear();
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            cur += c;
        }
    }

    out.push_back(cur.empty() ? 0 : std::stoi(cur));
    while (out.size() < 3) out.push_back(0);
    return out;
}

static bool versionAtLeast(const std::string& version, int a, int b, int c) {
    auto v = parseVersionParts(version);
    if (v[0] != a) return v[0] > a;
    if (v[1] != b) return v[1] > b;
    return v[2] >= c;
}

static bool usesCompressionHeader() {
    if (g_minecraftVersion.empty()) {
        return true;
    }

    return versionAtLeast(g_minecraftVersion, 1, 20, 61);
}


static std::string jsonEscape(const std::string& in) {
    std::ostringstream out;
    for (unsigned char c : in) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c << std::dec;
                else out << static_cast<char>(c);
        }
    }
    return out.str();
}

static std::string bytesHexJson(const std::vector<uint8_t>& data, std::size_t maxLen = 256) {
    std::ostringstream out;
    std::size_t n = std::min(maxLen, data.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (i) out << ' ';
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    if (data.size() > n) out << " ... +" << std::dec << (data.size() - n) << " bytes";
    return out.str();
}

static std::string apiFieldEscape(const std::string& in) {
    std::ostringstream out;
    for (unsigned char c : in) {
        if (c == ' ') out << "%20";
        else if (c == '\t') out << "%09";
        else if (c == '\r') out << "%0D";
        else if (c == '\n') out << "%0A";
        else if (c == '=') out << "%3D";
        else if (c == '%') out << "%25";
        else if (c < 0x20) out << '?';
        else out << static_cast<char>(c);
    }
    return out.str();
}

static void printApiJson(const bedrock::BedrockPacketEvent& event) {
    std::cout << "[API_JSON] {";
    std::cout << "\"version\":\"" << jsonEscape(event.version) << "\",";
    std::cout << "\"id\":" << event.packetId << ",";
    std::cout << "\"name\":\"" << jsonEscape(event.packetName) << "\",";
    std::cout << "\"ok\":" << (event.decodeError.empty() ? "true" : "false") << ",";
    std::cout << "\"decode_error\":\"" << jsonEscape(event.decodeError) << "\",";
    std::cout << "\"payload_size\":" << event.payload.size() << ",";
    std::cout << "\"raw_size\":" << event.rawPacket.size() << ",";
    std::cout << "\"payload_hex\":\"" << bytesHexJson(event.payload) << "\",";
    std::cout << "\"raw_hex\":\"" << bytesHexJson(event.rawPacket) << "\",";
    std::cout << "\"fields\":{";
    for (std::size_t i = 0; i < event.fields.size(); ++i) {
        const auto& f = event.fields[i];
        if (i) std::cout << ",";
        std::cout << "\"" << jsonEscape(f.name) << "\":\"" << jsonEscape(f.value) << "\"";
    }
    std::cout << "},\"field_info\":[";
    for (std::size_t i = 0; i < event.fields.size(); ++i) {
        const auto& f = event.fields[i];
        if (i) std::cout << ",";
        std::cout << "{\"name\":\"" << jsonEscape(f.name) << "\",\"type\":\"" << jsonEscape(f.type)
                  << "\",\"value\":\"" << jsonEscape(f.value) << "\",\"offset\":" << f.offset
                  << ",\"size\":" << f.size << "}";
    }
    std::cout << "]}\n" << std::flush;
}

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

static constexpr uint8_t ID_CONNECTED_PING              = 0x00;
static constexpr uint8_t ID_CONNECTED_PONG              = 0x03;
static constexpr uint8_t ID_CONNECTION_REQUEST          = 0x09;
static constexpr uint8_t ID_CONNECTION_REQUEST_ACCEPTED = 0x10;
static constexpr uint8_t ID_NEW_INCOMING_CONNECTION     = 0x13;

static constexpr uint8_t ID_NACK = 0xA0;
static constexpr uint8_t ID_ACK  = 0xC0;

static constexpr uint8_t ID_USER_PACKET_ENUM = 0xFE;

// Bedrock GamePacket IDs.
// RequestNetworkSettings / network_settings_request = 0xC1 / 193.
// NetworkSettings / network_settings = commonly 0x8F / 143 in modern protocol schemas.
static constexpr uint32_t ID_REQUEST_NETWORK_SETTINGS = 0xC1;
static constexpr uint32_t ID_NETWORK_SETTINGS = 0x8F;

static constexpr uint8_t RAKNET_PROTOCOL_VERSION = 11;

static std::string g_serverHandshakeJwt;
static bool g_gotServerHandshakeJwt = false;

static bool g_seenPlayStatus = false;
static bool g_seenResourcePacksInfo = false;
static bool g_seenResourcePackStack = false;
static bool g_seenStartGame = false;
static bool g_gotDisconnect = false;
static std::string g_disconnectText;

static uint64_t g_levelChunkCount = 0;
static uint64_t g_gamePacketCount = 0;

static int32_t zigZagDecode32(uint32_t v) {
    return static_cast<int32_t>((v >> 1) ^ (~(v & 1) + 1));
}


static constexpr int UDP_IPV4_HEADER_SIZE = 28;

static int64_t nowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

static uint64_t makeGuid() {
    auto now = std::chrono::high_resolution_clock::now()
        .time_since_epoch()
        .count();

    std::random_device rd;

    uint64_t a = static_cast<uint64_t>(now);
    uint64_t b = static_cast<uint64_t>(rd());

    return (a << 16) ^ b ^ 0xBEEFBADCAFED00DULL;
}

static void printHex(const std::vector<uint8_t>& data, size_t maxLen = 160) {
    size_t n = std::min(maxLen, data.size());

    for (size_t i = 0; i < n; i++) {
        std::cout
            << std::hex
            << std::setw(2)
            << std::setfill('0')
            << static_cast<int>(data[i])
            << " ";
    }

    if (data.size() > n) {
        std::cout << "...";
    }

    std::cout << std::dec << "\n";
}

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

static void writeTriadLE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
}

static void writeI32LE(std::vector<uint8_t>& out, int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);

    out.push_back(static_cast<uint8_t>(u & 0xff));
    out.push_back(static_cast<uint8_t>((u >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((u >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((u >> 24) & 0xff));
}

static void writeI32BE(std::vector<uint8_t>& out, int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);

    out.push_back(static_cast<uint8_t>((u >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((u >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((u >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(u & 0xff));
}

static void writeVarUInt(std::vector<uint8_t>& out, uint32_t v) {
    while (true) {
        if ((v & ~0x7fu) == 0) {
            out.push_back(static_cast<uint8_t>(v));
            return;
        }

        out.push_back(static_cast<uint8_t>((v & 0x7f) | 0x80));
        v >>= 7;
    }
}

static uint32_t readVarUInt(const std::vector<uint8_t>& data, size_t& off) {
    uint32_t result = 0;

    for (int shift = 0; shift <= 28; shift += 7) {
        if (off >= data.size()) {
            throw std::runtime_error("readVarUInt out of range");
        }

        uint8_t byte = data[off++];
        result |= static_cast<uint32_t>(byte & 0x7f) << shift;

        if ((byte & 0x80) == 0) {
            return result;
        }
    }

    throw std::runtime_error("VarUInt too big");
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

static uint16_t readU16LE(const std::vector<uint8_t>& data, size_t& off) {
    if (off + 2 > data.size()) {
        throw std::runtime_error("readU16LE out of range");
    }

    uint16_t v =
        static_cast<uint16_t>(data[off]) |
        static_cast<uint16_t>(static_cast<uint16_t>(data[off + 1]) << 8);

    off += 2;
    return v;
}

static uint32_t readU32LE(const std::vector<uint8_t>& data, size_t& off) {
    if (off + 4 > data.size()) {
        throw std::runtime_error("readU32LE out of range");
    }

    uint32_t v =
        static_cast<uint32_t>(data[off]) |
        (static_cast<uint32_t>(data[off + 1]) << 8) |
        (static_cast<uint32_t>(data[off + 2]) << 16) |
        (static_cast<uint32_t>(data[off + 3]) << 24);

    off += 4;
    return v;
}

static float readFloatLE(const std::vector<uint8_t>& data, size_t& off) {
    uint32_t raw = readU32LE(data, off);

    float f;
    std::memcpy(&f, &raw, sizeof(float));
    return f;
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

static uint32_t readTriadLE(const std::vector<uint8_t>& data, size_t& off) {
    if (off + 3 > data.size()) {
        throw std::runtime_error("readTriadLE out of range");
    }

    uint32_t v =
        static_cast<uint32_t>(data[off]) |
        (static_cast<uint32_t>(data[off + 1]) << 8) |
        (static_cast<uint32_t>(data[off + 2]) << 16);

    off += 3;
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
    out.push_back(static_cast<uint8_t>(~b[0]));
    out.push_back(static_cast<uint8_t>(~b[1]));
    out.push_back(static_cast<uint8_t>(~b[2]));
    out.push_back(static_cast<uint8_t>(~b[3]));
    writeU16BE(out, port);
}

static std::pair<std::string, uint16_t> readRakNetAddress(
    const std::vector<uint8_t>& data,
    size_t& off
) {
    if (off >= data.size()) {
        throw std::runtime_error("readRakNetAddress out of range");
    }

    uint8_t version = data[off++];

    if (version == 4) {
        if (off + 6 > data.size()) {
            throw std::runtime_error("read IPv4 address out of range");
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

    if (version == 6) {
        // IPv6 RakNet address is longer. For now we skip enough for diagnostics.
        // family(2) + port(2) + flow info(4) + addr(16) + scope id(4) = 28
        if (off + 28 > data.size()) {
            throw std::runtime_error("read IPv6 address out of range");
        }

        off += 28;
        return { "IPv6-address-skipped", 0 };
    }

    std::ostringstream ss;
    ss << "unsupported RakNet address version: " << static_cast<int>(version);
    throw std::runtime_error(ss.str());
}

static bool recvPacket(int sock, std::vector<uint8_t>& out, int timeoutMs) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);

    timeval tv {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int ready = select(sock + 1, &readfds, nullptr, nullptr, &tv);

    if (ready <= 0) {
        return false;
    }

    out.assign(4096, 0);

    ssize_t received = recvfrom(
        sock,
        out.data(),
        out.size(),
        0,
        nullptr,
        nullptr
    );

    if (received <= 0) {
        return false;
    }

    out.resize(static_cast<size_t>(received));
    return true;
}

static bool recvPacketById(
    int sock,
    uint8_t expectedId,
    std::vector<uint8_t>& out,
    int totalTimeoutMs
) {
    auto start = std::chrono::steady_clock::now();

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        ).count();

        if (elapsed >= totalTimeoutMs) {
            return false;
        }

        int left = static_cast<int>(totalTimeoutMs - elapsed);
        int step = left > 250 ? 250 : left;

        std::vector<uint8_t> packet;

        if (!recvPacket(sock, packet, step)) {
            continue;
        }

        if (packet.empty()) {
            continue;
        }

        uint8_t id = packet[0];

        if (id == expectedId) {
            out = packet;
            return true;
        }

        std::cout << "[OPEN] ignoring packet while waiting for 0x"
                  << std::hex
                  << static_cast<int>(expectedId)
                  << ": got 0x"
                  << static_cast<int>(id)
                  << std::dec
                  << " size="
                  << packet.size()
                  << "\n";
    }
}


static void sendPacket(
    int sock,
    const sockaddr_in& target,
    const std::vector<uint8_t>& packet
) {
    ssize_t sent = sendto(
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

static std::vector<uint8_t> buildOpenConnectionRequest1(int mtu) {
    std::vector<uint8_t> out;

    out.push_back(ID_OPEN_CONNECTION_REQUEST_1);
    out.insert(out.end(), std::begin(RAKNET_MAGIC), std::end(RAKNET_MAGIC));
    out.push_back(RAKNET_PROTOCOL_VERSION);

    int payloadSize = mtu - UDP_IPV4_HEADER_SIZE;

    if (payloadSize < static_cast<int>(out.size())) {
        throw std::runtime_error("MTU too small for Request1");
    }

    out.resize(static_cast<size_t>(payloadSize), 0);
    return out;
}

static std::vector<uint8_t> buildOpenConnectionRequest2(
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
        writeU32BE(out, securityCookie);
        out.push_back(0x00); // client supports security = false
    }

    writeRakNetAddressIPv4(out, serverIp, serverPort);

    writeU16BE(out, static_cast<uint16_t>(mtu));
    writeU64BE(out, clientGuid);

    return out;
}

struct OpenState {
    int sock = -1;
    sockaddr_in target {};
    std::string resolvedIp;
    uint16_t port = 19132;

    uint64_t clientGuid = 0;
    uint64_t serverGuid = 0;

    int mtu = 1400;
    bool serverSecurity = false;
    uint32_t securityCookie = 0;
};

static OpenState openRakNetSocket(
    const std::string& host,
    uint16_t port,
    int requestedMtu
) {
    OpenState st;
    st.clientGuid = makeGuid();
    st.port = port;

    addrinfo hints {};
    addrinfo* res = nullptr;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    std::string portStr = std::to_string(port);

    int gai = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);

    if (gai != 0) {
        throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(gai));
    }

    if (!res) {
        throw std::runtime_error("getaddrinfo returned empty result");
    }

    std::memcpy(&st.target, res->ai_addr, sizeof(sockaddr_in));
    freeaddrinfo(res);

    st.resolvedIp = sockaddrToIp(st.target);

    st.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (st.sock < 0) {
        throw std::runtime_error("socket failed");
    }

    std::cout << "[OPEN] resolvedIp=" << st.resolvedIp << " port=" << port << "\n";

    auto req1 = buildOpenConnectionRequest1(requestedMtu);

    std::cout << "[REQ1] mtu=" << requestedMtu
              << " udpPayload=" << req1.size()
              << "\n";

    sendPacket(st.sock, st.target, req1);

    std::vector<uint8_t> reply1;

    if (!recvPacketById(st.sock, ID_OPEN_CONNECTION_REPLY_1, reply1, 4000)) {
        throw std::runtime_error("timeout waiting OpenConnectionReply1");
    }

    std::cout << "[REPLY1] size=" << reply1.size() << " hex=";
    printHex(reply1, 96);

    size_t off1 = 0;

    uint8_t id1 = reply1[off1++];

    if (id1 != ID_OPEN_CONNECTION_REPLY_1) {
        throw std::runtime_error("unexpected OpenConnectionReply1 packet id");
    }

    if (!checkMagic(reply1, off1)) {
        throw std::runtime_error("invalid magic in Reply1");
    }

    off1 += 16;

    st.serverGuid = readU64BE(reply1, off1);

    st.serverSecurity = reply1[off1++] != 0;

    if (st.serverSecurity) {
        st.securityCookie = readU32BE(reply1, off1);

        std::cout << "[SECURITY] cookie="
                  << st.securityCookie
                  << "\n";
    }

    int mtuFromReply1 = static_cast<int>(readU16BE(reply1, off1));

    st.mtu = requestedMtu;

    if (mtuFromReply1 >= 576 && mtuFromReply1 <= 1492) {
        st.mtu = mtuFromReply1;
    }

    auto req2 = buildOpenConnectionRequest2(
        st.resolvedIp,
        port,
        st.mtu,
        st.clientGuid,
        st.serverSecurity,
        st.securityCookie
    );

    std::cout << "[REQ2] mtu=" << st.mtu
              << " payload=" << req2.size()
              << " security=" << (st.serverSecurity ? "true" : "false")
              << "\n";

    sendPacket(st.sock, st.target, req2);

    std::vector<uint8_t> reply2;

    if (!recvPacketById(st.sock, ID_OPEN_CONNECTION_REPLY_2, reply2, 4000)) {
        throw std::runtime_error("timeout waiting OpenConnectionReply2");
    }

    std::cout << "[REPLY2] size=" << reply2.size() << " hex=";
    printHex(reply2, 96);

    size_t off2 = 0;

    uint8_t id2 = reply2[off2++];

    if (id2 != ID_OPEN_CONNECTION_REPLY_2) {
        throw std::runtime_error("unexpected OpenConnectionReply2 packet id");
    }

    if (!checkMagic(reply2, off2)) {
        throw std::runtime_error("invalid magic in Reply2");
    }

    off2 += 16;

    st.serverGuid = readU64BE(reply2, off2);

    auto clientSeen = readRakNetAddress(reply2, off2);
    int mtuFromReply2 = static_cast<int>(readU16BE(reply2, off2));

    if (mtuFromReply2 >= 576 && mtuFromReply2 <= 1492) {
        st.mtu = mtuFromReply2;
    }

    std::cout << "[OPEN OK] serverGuid=" << st.serverGuid
              << " clientSeen=" << clientSeen.first << ":" << clientSeen.second
              << " mtu=" << st.mtu
              << "\n";

    return st;
}

struct RakNetFrame {
    uint8_t reliability = 0;
    bool split = false;
    uint32_t reliableIndex = 0;
    uint32_t orderedIndex = 0;
    uint8_t orderedChannel = 0;

    uint32_t splitCount = 0;
    uint16_t splitId = 0;
    uint32_t splitIndex = 0;

    std::vector<uint8_t> payload;
};

struct IncomingSplitAssembly {
    uint32_t splitCount = 0;
    std::map<uint32_t, std::vector<uint8_t>> chunks;
};

struct PendingInboundPayload {
    uint32_t orderedIndex = 0;
    uint8_t reliability = 0;
    bool split = false;
    std::vector<uint8_t> payload;
};

static bool isReliable(uint8_t reliability) {
    return reliability == 2 || reliability == 3 || reliability == 4 || reliability == 6 || reliability == 7;
}

static bool isOrdered(uint8_t reliability) {
    return reliability == 3 || reliability == 4 || reliability == 7;
}

static bool isSequenced(uint8_t reliability) {
    return reliability == 1 || reliability == 4;
}

static std::vector<uint8_t> buildReliableOrderedDatagram(
    uint32_t datagramSequence,
    uint32_t reliableIndex,
    uint32_t orderedIndex,
    uint8_t orderedChannel,
    const std::vector<uint8_t>& payload
) {
    std::vector<uint8_t> out;

    // 0x80..0x8f = connected frame set/datagram.
    out.push_back(0x80);
    writeTriadLE(out, datagramSequence);

    uint8_t reliability = 3; // RELIABLE_ORDERED
    uint8_t flags = static_cast<uint8_t>(reliability << 5);

    out.push_back(flags);

    uint16_t bitLength = static_cast<uint16_t>(payload.size() * 8);
    writeU16BE(out, bitLength);

    writeTriadLE(out, reliableIndex);
    writeTriadLE(out, orderedIndex);
    out.push_back(orderedChannel);

    out.insert(out.end(), payload.begin(), payload.end());

    return out;
}

struct SplitDatagram {
    uint32_t sequence = 0;
    std::vector<uint8_t> data;
};

static std::vector<SplitDatagram> buildReliableOrderedSplitDatagrams(
    uint32_t& datagramSequence,
    uint32_t& reliableIndex,
    uint32_t& orderedIndex,
    uint8_t orderedChannel,
    const std::vector<uint8_t>& payload,
    size_t maxPayloadPerFragment
) {
    std::vector<SplitDatagram> result;

    if (maxPayloadPerFragment < 128) {
        throw std::runtime_error("maxPayloadPerFragment too small");
    }

    uint32_t splitCount = static_cast<uint32_t>(
        (payload.size() + maxPayloadPerFragment - 1) / maxPayloadPerFragment
    );

    uint16_t splitId = static_cast<uint16_t>(
        (nowMillis() ^ reliableIndex ^ orderedIndex) & 0xffff
    );

    uint32_t sharedOrderedIndex = orderedIndex++;

    for (uint32_t splitIndex = 0; splitIndex < splitCount; splitIndex++) {
        size_t begin = static_cast<size_t>(splitIndex) * maxPayloadPerFragment;
        size_t end = std::min(begin + maxPayloadPerFragment, payload.size());

        std::vector<uint8_t> chunk(
            payload.begin() + static_cast<std::ptrdiff_t>(begin),
            payload.begin() + static_cast<std::ptrdiff_t>(end)
        );

        std::vector<uint8_t> out;

        uint32_t seq = datagramSequence++;

        out.push_back(0x80);
        writeTriadLE(out, seq);

        uint8_t reliability = 3; // RELIABLE_ORDERED
        uint8_t flags = static_cast<uint8_t>((reliability << 5) | 0x10); // split flag
        out.push_back(flags);

        uint16_t bitLength = static_cast<uint16_t>(chunk.size() * 8);
        writeU16BE(out, bitLength);

        writeTriadLE(out, reliableIndex++);
        writeTriadLE(out, sharedOrderedIndex);
        out.push_back(orderedChannel);

        // split count: u32 BE
        // split id:    u16 BE
        // split index: u32 BE
        writeU32BE(out, splitCount);
        writeU16BE(out, splitId);
        writeU32BE(out, splitIndex);

        out.insert(out.end(), chunk.begin(), chunk.end());

        result.push_back({ seq, std::move(out) });
    }

    return result;
}



static uint32_t sendEncryptedGamePacketReliable(
    int sock,
    const sockaddr_in& target,
    std::map<uint32_t, std::vector<uint8_t>>& sentReliableDatagrams,
    uint32_t& datagramSeq,
    uint32_t& reliableIndex,
    uint32_t& orderedIndex,
    const std::vector<uint8_t>& gamePacket,
    uint64_t& encryptedSendCounter,
    const std::vector<uint8_t>& secretKeyBytes,
    bedrock::BedrockCipherStream& encryptStream,
    const std::string& label
) {
    auto framedPackets = bedrock::BedrockFramer::framePackets(
        { gamePacket }
    );

    auto compressed = bedrock::BedrockFramer::deflateRaw(
        framedPackets
    );

    std::vector<uint8_t> compressorPacket;
    if (usesCompressionHeader()) {
        compressorPacket.push_back(0x00); // deflate
    }
    compressorPacket.insert(
        compressorPacket.end(),
        compressed.begin(),
        compressed.end()
    );

    auto aesPlaintext = bedrock::BedrockEncryption::makeAesPlaintext(
        compressorPacket,
        encryptedSendCounter++,
        secretKeyBytes
    );

    auto encryptedOnly = encryptStream.process(aesPlaintext);

    std::vector<uint8_t> encryptedMcpe;
    encryptedMcpe.reserve(1 + encryptedOnly.size());
    encryptedMcpe.push_back(0xfe);
    encryptedMcpe.insert(
        encryptedMcpe.end(),
        encryptedOnly.begin(),
        encryptedOnly.end()
    );

    uint32_t seq = datagramSeq++;

    auto datagram = buildReliableOrderedDatagram(
        seq,
        reliableIndex++,
        orderedIndex++,
        0,
        encryptedMcpe
    );

    sendPacket(sock, target, datagram);
    sentReliableDatagrams[seq] = datagram;

    std::cout << "[SEND] " << label
              << " seq="
              << seq
              << " gamePacket="
              << gamePacket.size()
              << " framed="
              << framedPackets.size()
              << " compressorPacket="
              << compressorPacket.size()
              << " encryptedMcpe="
              << encryptedMcpe.size()
              << " datagram="
              << datagram.size()
              << "\n";

    std::cout << "[SEND] " << label << " game hex=";
    printHex(gamePacket, 64);

    std::cout << "[SEND] " << label << " encrypted hex=";
    printHex(encryptedMcpe, 96);

    return seq;
}


static void dumpApiOutboundGamePacket(
    const std::vector<uint8_t>& gamePacket,
    const std::string& label
) {
    if (std::getenv("BEDROCK_API_DUMP") == nullptr) {
        return;
    }

    try {
        size_t off = 0;
        uint32_t header = readVarUInt(gamePacket, off);
        uint32_t packetId = header & 0x3ff;

        static bedrock::ProtocolDefinition protocolDefinition =
            bedrock::ProtocolDefinition::forVersion(g_minecraftVersion);

        std::string name = protocolDefinition.packetName(packetId);
        if (name.empty()) {
            name = "unknown_" + std::to_string(packetId);
        }

        std::cout << "[API] packet"
                  << " id=" << packetId
                  << " name=" << apiFieldEscape(name)
                  << " ok=true"
                  << " fields=4"
                  << " direction=serverbound"
                  << " label=" << apiFieldEscape(label)
                  << " raw_size=" << gamePacket.size()
                  << " payload_size=" << (gamePacket.size() >= off ? gamePacket.size() - off : 0)
                  << "\n" << std::flush;
    } catch (const std::exception& e) {
        std::cout << "[API] packet"
                  << " id=0"
                  << " name=outbound_decode_error"
                  << " ok=false"
                  << " fields=3"
                  << " direction=serverbound"
                  << " label=" << apiFieldEscape(label)
                  << " decode_error=" << apiFieldEscape(e.what())
                  << "\n" << std::flush;
    }
}


static uint32_t sendPlainGamePacketReliable(
    int sock,
    const sockaddr_in& target,
    std::map<uint32_t, std::vector<uint8_t>>& sentReliableDatagrams,
    uint32_t& datagramSeq,
    uint32_t& reliableIndex,
    uint32_t& orderedIndex,
    const std::vector<uint8_t>& gamePacket,
    const std::string& label
) {
    bedrock::BedrockFramerSettings settings;
    settings.compressionReady = true;
    settings.compressorInHeader = usesCompressionHeader();
    settings.compressionThreshold = 256;
    settings.compressionAlgorithm = 0;

    auto batch = bedrock::BedrockFramer::encodeBatch({ gamePacket }, settings);

    uint32_t seq = datagramSeq++;

    auto datagram = buildReliableOrderedDatagram(
        seq,
        reliableIndex++,
        orderedIndex++,
        0,
        batch
    );

    sendPacket(sock, target, datagram);
    sentReliableDatagrams[seq] = datagram;

    std::cout << "[SEND] " << label
              << " seq="
              << seq
              << " gamePacket="
              << gamePacket.size()
              << " batch="
              << batch.size()
              << " datagram="
              << datagram.size()
              << " encrypted=false\n";

    std::cout << "[SEND] " << label << " game hex=";
    printHex(gamePacket, 64);

    std::cout << "[SEND] " << label << " batch hex=";
    printHex(batch, 96);

    return seq;
}

static uint32_t sendGamePacketReliable(
    int sock,
    const sockaddr_in& target,
    std::map<uint32_t, std::vector<uint8_t>>& sentReliableDatagrams,
    uint32_t& datagramSeq,
    uint32_t& reliableIndex,
    uint32_t& orderedIndex,
    const std::vector<uint8_t>& gamePacket,
    bool encryptionReady,
    uint64_t& encryptedSendCounter,
    const std::vector<uint8_t>& secretKeyBytes,
    bedrock::BedrockCipherStream* encryptStream,
    const std::string& label
) {
    dumpApiOutboundGamePacket(gamePacket, label);

    if (encryptionReady) {
        if (encryptStream == nullptr) {
            throw std::runtime_error("encryption is ready but encrypt stream is missing");
        }

        return sendEncryptedGamePacketReliable(
            sock,
            target,
            sentReliableDatagrams,
            datagramSeq,
            reliableIndex,
            orderedIndex,
            gamePacket,
            encryptedSendCounter,
            secretKeyBytes,
            *encryptStream,
            label
        );
    }

    return sendPlainGamePacketReliable(
        sock,
        target,
        sentReliableDatagrams,
        datagramSeq,
        reliableIndex,
        orderedIndex,
        gamePacket,
        label
    );
}


static std::vector<uint8_t> buildAck(uint32_t datagramSequence) {
    std::vector<uint8_t> out;

    out.push_back(ID_ACK);

    // record count = 1
    writeU16BE(out, 1);

    // record type:
    // 1 = single sequence number
    // 0 = range start/end
    out.push_back(0x01);
    writeTriadLE(out, datagramSequence);

    return out;
}

static std::vector<uint32_t> parseAckSequences(const std::vector<uint8_t>& data) {
    std::vector<uint32_t> sequences;

    if (data.size() < 3) {
        return sequences;
    }

    size_t off = 1;

    uint16_t recordCount = readU16BE(data, off);

    for (uint16_t i = 0; i < recordCount && off < data.size(); i++) {
        uint8_t isSingle = data[off++];

        if (isSingle) {
            uint32_t seq = readTriadLE(data, off);
            sequences.push_back(seq);
        } else {
            uint32_t start = readTriadLE(data, off);
            uint32_t end = readTriadLE(data, off);

            if (end >= start && end - start < 4096) {
                for (uint32_t seq = start; seq <= end; seq++) {
                    sequences.push_back(seq);
                }
            }
        }
    }

    return sequences;
}


static std::vector<RakNetFrame> parseConnectedDatagram(
    const std::vector<uint8_t>& data,
    uint32_t& datagramSequence
) {
    std::vector<RakNetFrame> frames;

    if (data.empty()) {
        return frames;
    }

    uint8_t packetId = data[0];

    if (packetId < 0x80 || packetId > 0x8f) {
        return frames;
    }

    size_t off = 1;

    datagramSequence = readTriadLE(data, off);

    while (off < data.size()) {
        RakNetFrame f;

        uint8_t flags = data[off++];

        f.reliability = static_cast<uint8_t>((flags & 0xE0) >> 5);
        f.split = (flags & 0x10) != 0;

        uint16_t bitLength = readU16BE(data, off);
        size_t byteLength = (bitLength + 7) / 8;

        if (isReliable(f.reliability)) {
            f.reliableIndex = readTriadLE(data, off);
        }

        if (isSequenced(f.reliability)) {
            // sequencing index
            (void)readTriadLE(data, off);
        }

        if (isOrdered(f.reliability)) {
            f.orderedIndex = readTriadLE(data, off);

            if (off >= data.size()) {
                throw std::runtime_error("ordered channel out of range");
            }

            f.orderedChannel = data[off++];
        }

        if (f.split) {
            // split count u32 BE, split id u16 BE, split index u32 BE
            if (off + 10 > data.size()) {
                throw std::runtime_error("split header out of range");
            }

            f.splitCount = readU32BE(data, off);
            f.splitId = readU16BE(data, off);
            f.splitIndex = readU32BE(data, off);
        }

        if (f.split && byteLength == 0) {
            // Some RakNet implementations/proxies send split frame length as 0.
            // In that case the fragment payload is the rest of the datagram.
            byteLength = data.size() - off;
        }

        if (off + byteLength > data.size()) {
            throw std::runtime_error("frame payload out of range");
        }

        f.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(off),
                         data.begin() + static_cast<std::ptrdiff_t>(off + byteLength));

        off += byteLength;

        frames.push_back(std::move(f));
    }

    return frames;
}

static std::vector<uint8_t> buildConnectionRequest(
    uint64_t clientGuid,
    int64_t timestamp
) {
    std::vector<uint8_t> p;

    p.push_back(ID_CONNECTION_REQUEST);
    writeU64BE(p, clientGuid);
    writeU64BE(p, static_cast<uint64_t>(timestamp));

    // use security = false
    p.push_back(0x00);

    return p;
}

static std::vector<uint8_t> buildConnectedPong(
    int64_t pingTime,
    int64_t pongTime
) {
    std::vector<uint8_t> p;

    p.push_back(ID_CONNECTED_PONG);
    writeU64BE(p, static_cast<uint64_t>(pingTime));
    writeU64BE(p, static_cast<uint64_t>(pongTime));

    return p;
}

static std::vector<uint8_t> buildNewIncomingConnection(
    const std::string& serverIp,
    uint16_t serverPort,
    int64_t requestTimestamp,
    int64_t acceptedTimestamp
) {
    std::vector<uint8_t> p;

    p.push_back(ID_NEW_INCOMING_CONNECTION);

    writeRakNetAddressIPv4(p, serverIp, serverPort);

    // 20 internal addresses.
    for (int i = 0; i < 20; i++) {
        writeRakNetAddressIPv4(p, "0.0.0.0", 0);
    }

    writeU64BE(p, static_cast<uint64_t>(requestTimestamp));
    writeU64BE(p, static_cast<uint64_t>(acceptedTimestamp));

    return p;
}


static std::vector<uint8_t> buildNetworkSettingsRequestGamePacket(
    int protocolVersion,
    uint32_t networkSettingsRequestPacketId
) {
    std::vector<uint8_t> packet;

    // GamePacket header:
    // low 10 bits = packet id
    // next 2 bits = sender subclient id
    // next 2 bits = target subclient id
    uint32_t header = networkSettingsRequestPacketId & 0x3ff;

    writeVarUInt(packet, header);
    writeI32BE(packet, protocolVersion);

    return packet;
}

static std::vector<uint8_t> buildNetworkSettingsRequestUserPacket(
    int protocolVersion,
    const std::string& mode,
    uint32_t networkSettingsRequestPacketId
) {
    auto gamePacket = buildNetworkSettingsRequestGamePacket(
        protocolVersion,
        networkSettingsRequestPacketId
    );

    std::vector<uint8_t> out;

    if (mode == "raw") {
        return gamePacket;
    }

    if (mode == "len") {
        writeVarUInt(out, static_cast<uint32_t>(gamePacket.size()));
        out.insert(out.end(), gamePacket.begin(), gamePacket.end());
        return out;
    }

    if (mode == "fe-raw") {
        out.push_back(ID_USER_PACKET_ENUM);
        out.insert(out.end(), gamePacket.begin(), gamePacket.end());
        return out;
    }

    if (mode == "fe-nocomp-len") {
        out.push_back(ID_USER_PACKET_ENUM);

        // Diagnostic variant:
        // 0xff = no compression marker used by modern Bedrock compression layer.
        out.push_back(0xff);

        writeVarUInt(out, static_cast<uint32_t>(gamePacket.size()));
        out.insert(out.end(), gamePacket.begin(), gamePacket.end());
        return out;
    }

    // Default mode: fe-len
    // RakNet user packet 0xfe + length-prefixed Bedrock game packet.
    out.push_back(ID_USER_PACKET_ENUM);
    writeVarUInt(out, static_cast<uint32_t>(gamePacket.size()));
    out.insert(out.end(), gamePacket.begin(), gamePacket.end());

    return out;
}


struct ParsedNetworkSettings {
    uint16_t compressionThreshold = 0;
    uint16_t compressionAlgorithm = 0;
    bool clientThrottleEnabled = false;
    uint8_t clientThrottleThreshold = 0;
    float clientThrottleScalar = 0.0f;
};

static ParsedNetworkSettings parseNetworkSettingsBody(
    const std::vector<uint8_t>& packet,
    size_t bodyOffset,
    size_t packetEnd
) {
    ParsedNetworkSettings ns;

    size_t off = bodyOffset;

    if (packetEnd - off < 4) {
        throw std::runtime_error("network_settings body too small");
    }

    ns.compressionThreshold = readU16LE(packet, off);
    ns.compressionAlgorithm = readU16LE(packet, off);

    if (off < packetEnd) {
        ns.clientThrottleEnabled = packet[off++] != 0;
    }

    if (off < packetEnd) {
        ns.clientThrottleThreshold = packet[off++];
    }

    if (off + 4 <= packetEnd) {
        ns.clientThrottleScalar = readFloatLE(packet, off);
    }

    return ns;
}

static const char* compressionAlgorithmName(uint16_t id) {
    switch (id) {
        case 0: return "deflate";
        case 1: return "snappy";
        default: return "unknown";
    }
}

static void inspectPossibleGamePackets(
    const std::vector<uint8_t>& payload,
    bool& gotNetworkSettings
) {
    if (payload.empty()) {
        return;
    }

    std::cout << "[GAME-INSPECT] rawPayload=";
    printHex(payload, 128);

    size_t off = 0;

    if (payload[0] == ID_USER_PACKET_ENUM) {
        std::cout << "[GAME-INSPECT] has RakNet user packet 0xfe\n";
        off = 1;
    } else {
        std::cout << "[GAME-INSPECT] no 0xfe prefix, trying raw gamepacket parse\n";
    }

    // Some modern streams may include a compression marker before the batch.
    if (off < payload.size() && (payload[off] == 0x00 || payload[off] == 0x01 || payload[off] == 0xff)) {
        std::cout << "[GAME-INSPECT] possible compression marker=0x"
                  << std::hex
                  << static_cast<int>(payload[off])
                  << std::dec
                  << "\n";

        // For diagnostics only. We do not decompress yet.
        // If it is zlib/snappy, later stage will implement decompression.
        if (payload[off] == 0x00 || payload[off] == 0x01 || payload[off] == 0xff) {
            size_t afterMarker = off + 1;

            if (afterMarker < payload.size()) {
                off = afterMarker;
            }
        }
    }

    int packetCount = 0;

    while (off < payload.size() && packetCount < 8) {
        size_t beforeLen = off;

        try {
            uint32_t packetLen = readVarUInt(payload, off);

            if (packetLen == 0 || off + packetLen > payload.size()) {
                off = beforeLen;
                break;
            }

            size_t packetEnd = off + packetLen;

            uint32_t header = readVarUInt(payload, off);
            uint32_t packetId = header & 0x3ff;
            uint32_t senderSubId = (header >> 10) & 0x3;
            uint32_t targetSubId = (header >> 12) & 0x3;
            std::cout << "[GAME] len="
                      << packetLen
                      << " packetId="
                      << packetId
                      << " senderSubId="
                      << senderSubId
                      << " targetSubId="
                      << targetSubId
                      << " bodyBytes="
                      << (packetEnd - off)
                      << "\n";

            if (packetId == ID_NETWORK_SETTINGS) {
                gotNetworkSettings = true;
                std::cout << "[OK] Received NetworkSettings packet candidate\n";

                try {
                    auto ns = parseNetworkSettingsBody(
                        payload,
                        off,
                        packetEnd
                    );

                    std::cout << "[NETWORK_SETTINGS] compressionThreshold="
                              << ns.compressionThreshold
                              << "\n";

                    std::cout << "[NETWORK_SETTINGS] compressionAlgorithm="
                              << ns.compressionAlgorithm
                              << " / "
                              << compressionAlgorithmName(ns.compressionAlgorithm)
                              << "\n";

                    std::cout << "[NETWORK_SETTINGS] clientThrottleEnabled="
                              << (ns.clientThrottleEnabled ? "true" : "false")
                              << "\n";

                    std::cout << "[NETWORK_SETTINGS] clientThrottleThreshold="
                              << static_cast<int>(ns.clientThrottleThreshold)
                              << "\n";

                    std::cout << "[NETWORK_SETTINGS] clientThrottleScalar="
                              << ns.clientThrottleScalar
                              << "\n";

                    std::cout << "[NEXT] After this packet outgoing batches must use:\n";
                    std::cout << "       0xfe + compressionHeader + framed packets\n";
                    std::cout << "       compressionHeader 0xff = not compressed\n";
                    std::cout << "       compressionHeader 0x00 = deflateRaw compressed\n";
                } catch (const std::exception& e) {
                    std::cout << "[NETWORK_SETTINGS] parse failed: "
                              << e.what()
                              << "\n";
                }
            }

            off = packetEnd;
            packetCount++;
        } catch (const std::exception& e) {
            std::cout << "[GAME-INSPECT] length-prefixed parse failed: "
                      << e.what()
                      << "\n";

            break;
        }
    }

    // Fallback: maybe payload is directly a single game packet without length prefix.
    if (!gotNetworkSettings) {
        try {
            size_t rawOff = payload[0] == ID_USER_PACKET_ENUM ? 1 : 0;
            uint32_t header = readVarUInt(payload, rawOff);
            uint32_t packetId = header & 0x3ff;

            std::cout << "[GAME-FALLBACK] direct packetId="
                      << packetId
                      << " bodyBytes="
                      << (payload.size() - rawOff)
                      << "\n";

            if (packetId == ID_NETWORK_SETTINGS) {
                gotNetworkSettings = true;
                std::cout << "[OK] Received NetworkSettings packet candidate by fallback\n";
            }
        } catch (...) {
            // ignored
        }
    }
}



static std::vector<uint8_t> readBinaryFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);

    if (!f) {
        throw std::runtime_error("failed to open binary file: " + path);
    }

    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(f),
        std::istreambuf_iterator<char>()
    );
}

static void writeTextFile(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);

    if (!f) {
        throw std::runtime_error("failed to write text file: " + path);
    }

    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

struct RuntimeCommand {
    std::string name;
    bedrock::ProtoDefValue value;
};

static std::string unescapeCommandValue(const std::string& input) {
    std::string out;
    for (size_t i = 0; i < input.size(); i++) {
        if (input[i] == '%' && i + 2 < input.size()) {
            int hi = -1;
            int lo = -1;
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return -1;
            };
            hi = hex(input[i + 1]);
            lo = hex(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(input[i]);
    }
    return out;
}

static std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= line.size()) {
        size_t pos = line.find('\t', start);
        if (pos == std::string::npos) {
            parts.push_back(line.substr(start));
            break;
        }
        parts.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

static std::vector<RuntimeCommand> pollRuntimeCommands(
    const std::string& path,
    std::streampos& offset
) {
    std::vector<RuntimeCommand> commands;
    if (path.empty()) {
        return commands;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return commands;
    }

    in.seekg(offset);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            offset = in.tellg();
            continue;
        }

        auto parts = splitTabs(line);
        if (parts.size() < 3 || parts[0] != "send_json") {
            offset = in.tellg();
            continue;
        }

        RuntimeCommand command;
        command.name = unescapeCommandValue(parts[1]);
        command.value = bedrock::protoDefValueFromJson(unescapeCommandValue(parts[2]));
        commands.push_back(std::move(command));
        offset = in.tellg();
    }

    if (in.eof()) {
        in.clear();
        offset = in.tellg();
        if (offset == std::streampos(-1)) {
            in.clear();
            in.seekg(0, std::ios::end);
            offset = in.tellg();
        }
    }
    return commands;
}

static std::vector<uint8_t> makeRuntimeCommandPacket(const RuntimeCommand& command) {
    bedrock::ProtoDefPacketEncoder encoder(g_minecraftVersion);
    auto payload = encoder.encodePacket(command.name, command.value);
    auto codec = bedrock::VersionedPacketCodec::forVersion(g_minecraftVersion);
    return codec.encodeFullPacketByName(command.name, payload);
}

static std::string readStringVarUIntFromPacket(
    const std::vector<uint8_t>& packet,
    size_t& off
) {
    uint32_t len = readVarUInt(packet, off);

    if (off + len > packet.size()) {
        throw std::runtime_error("string length exceeds packet size");
    }

    std::string s(
        reinterpret_cast<const char*>(packet.data() + off),
        len
    );

    off += len;
    return s;
}


static void inspectDecodedBedrockBatch(
    const std::vector<uint8_t>& payload,
    bool compressionReady,
    bool& gotAnyGamePacket
) {
    if (payload.empty() || payload[0] != ID_USER_PACKET_ENUM) {
        return;
    }

    bedrock::BedrockFramerSettings settings;
    settings.compressionReady = compressionReady;
    settings.compressorInHeader = usesCompressionHeader();
    settings.compressionThreshold = 256;
    settings.compressionAlgorithm = 0;

    try {
        auto packets = bedrock::BedrockFramer::decodeBatch(payload, settings);

        std::cout << "[BATCH-DECODE] packets="
                  << packets.size()
                  << " compressionReady="
                  << (compressionReady ? "true" : "false")
                  << "\n";

        bedrock::BedrockPacketEventDispatcher packetEvents(g_minecraftVersion);
        packetEvents.events().onAny([](const bedrock::BedrockPacketEvent& event) {
            std::cout << "[EVENT] packet "
                      << event.packetName
                      << " id="
                      << event.packetId
                      << " fields="
                      << event.fields.size()
                      << "\n";
        });

        for (size_t i = 0; i < packets.size(); i++) {
            const auto& packet = packets[i];

            if (packet.empty()) {
                continue;
            }

            size_t off = 0;
            uint32_t header = readVarUInt(packet, off);
            uint32_t packetId = header & 0x3ff;
            uint32_t senderSubId = (header >> 10) & 0x3;
            uint32_t targetSubId = (header >> 12) & 0x3;

            std::cout << "[GAME-DECODED] index="
                      << i
                      << " packetId="
                      << packetId
                      << " senderSubId="
                      << senderSubId
                      << " targetSubId="
                      << targetSubId
                      << " size="
                      << packet.size()
                      << " bodyBytes="
                      << (packet.size() - off)
                      << "\n";

            std::cout << "[GAME-DECODED] hex=";
            printHex(packet, 160);

            gotAnyGamePacket = true;

            if (packetId == 2) {
                std::cout << "[LOGIN-RESULT] play_status packet candidate\n";
            }

            if (packetId == 5) {
                std::cout << "[LOGIN-RESULT] disconnect packet candidate\n";
            }

            if (packetId == 3) {
                std::cout << "[LOGIN-RESULT] server_to_client_handshake packet candidate\n";

                try {
                    size_t tokenOff = off;
                    std::string serverToken = readStringVarUIntFromPacket(
                        packet,
                        tokenOff
                    );

                    std::cout << "[HANDSHAKE] server token length="
                              << serverToken.size()
                              << "\n";

                    std::cout << "[HANDSHAKE] server token start="
                              << serverToken.substr(0, 120)
                              << "...\n";

                    writeTextFile(
                        "/tmp/server_handshake_jwt.txt",
                        serverToken
                    );

                    std::cout << "[HANDSHAKE] saved /tmp/server_handshake_jwt.txt\n";

                    g_serverHandshakeJwt = serverToken;
                    g_gotServerHandshakeJwt = true;

                    std::cout << "[HANDSHAKE] stored server JWT in memory for encryption\n";
                } catch (const std::exception& e) {
                    std::cout << "[HANDSHAKE] parse/save failed: "
                              << e.what()
                              << "\n";
                }
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[BATCH-DECODE] failed: "
                  << e.what()
                  << "\n";

        std::cout << "[BATCH-DECODE] raw payload=";
        printHex(payload, 180);
    }
}


static void inspectEncryptedBedrockPayload(
    const std::vector<uint8_t>& payload,
    const std::vector<uint8_t>& secretKeyBytes,
    bedrock::BedrockCipherStream& decryptStream,
    uint64_t& receiveCounter,
    bool& gotEncryptedGamePacket
) {
    if (payload.empty() || payload[0] != ID_USER_PACKET_ENUM) {
        return;
    }

    try {
        if (payload[0] != 0xfe) {
            throw std::runtime_error("encrypted payload missing 0xfe");
        }

        std::vector<uint8_t> encryptedOnly(
            payload.begin() + 1,
            payload.end()
        );
        const auto consumedCounter = receiveCounter;
        auto verification = bedrock::BedrockEncryption::decryptAndVerify(
            decryptStream,
            encryptedOnly,
            receiveCounter,
            secretKeyBytes
        );
        if (!verification) {
            return;
        }
        if (!verification->matches()) {
            throw std::runtime_error(verification->mismatchMessage());
        }
        const auto& packetPlaintext = verification->packetPlaintext;

        std::cout << "[DECRYPT] receiveCounter="
                  << consumedCounter
                  << " packetPlaintext="
                  << packetPlaintext.size()
                  << "\n";

        // compact mode: skip decrypted plaintext hex dump
        if (packetPlaintext.empty()) {
            throw std::runtime_error("empty decrypted packet plaintext");
        }

        std::vector<uint8_t> framed;

        if (!usesCompressionHeader()) {
            framed = bedrock::BedrockFramer::inflateRaw(packetPlaintext);
            std::cout << "[DECRYPT] compression=deflate-old framed="
                      << framed.size()
                      << "\n";
        } else if (packetPlaintext[0] == 0x00) {
            std::vector<uint8_t> compressed(
                packetPlaintext.begin() + 1,
                packetPlaintext.end()
            );

            framed = bedrock::BedrockFramer::inflateRaw(compressed);

            std::cout << "[DECRYPT] compression=deflate framed="
                      << framed.size()
                      << "\n";
        } else if (packetPlaintext[0] == 0xff) {
            framed.assign(
                packetPlaintext.begin() + 1,
                packetPlaintext.end()
            );

            std::cout << "[DECRYPT] compression=none framed="
                      << framed.size()
                      << "\n";
        } else {
            std::cout << "[DECRYPT] unknown compression header=0x"
                      << std::hex
                      << static_cast<int>(packetPlaintext[0])
                      << std::dec
                      << "\n";
            return;
        }

        auto packets = bedrock::BedrockFramer::unframePackets(framed);

        std::cout << "[DECRYPT] game packets="
                  << packets.size()
                  << "\n";

        bedrock::BedrockPacketEventDispatcher packetEvents(g_minecraftVersion);
        packetEvents.events().onAny([](const bedrock::BedrockPacketEvent& event) {
            std::cout << "[EVENT] packet "
                      << event.packetName
                      << " id="
                      << event.packetId
                      << " fields="
                      << event.fields.size()
                      << "\n";
        });

        for (size_t i = 0; i < packets.size(); i++) {
            const auto& packet = packets[i];

            if (packet.empty()) {
                continue;
            }

            size_t off = 0;
            uint32_t header = readVarUInt(packet, off);
            uint32_t packetId = header & 0x3ff;
            uint32_t senderSubId = (header >> 10) & 0x3;
            uint32_t targetSubId = (header >> 12) & 0x3;

            ++g_gamePacketCount;

            if (packetId == 6) {
                g_seenResourcePacksInfo = true;
                std::cout << "[PACKS] resource_packs_info received\n";
            } else if (packetId == 7) {
                g_seenResourcePackStack = true;
                std::cout << "[PACKS] resource_pack_stack received\n";
            } else if (packetId == 11) {
                g_seenStartGame = true;
                std::cout << "[JOIN] start_game received\n";
            } else if (packetId == 58) {
                ++g_levelChunkCount;
            }

            std::vector<uint8_t> eventPayload(
                packet.begin() + static_cast<std::ptrdiff_t>(off),
                packet.end()
            );

            bedrock::GamePacket eventPacket;
            eventPacket.packetId = packetId;

            static bedrock::ProtocolDefinition protocolDefinition =
                bedrock::ProtocolDefinition::forVersion(g_minecraftVersion);

            eventPacket.name = protocolDefinition.packetName(packetId);
            eventPacket.fullPacket = packet;
            eventPacket.payload = eventPayload;

            if (eventPacket.name.empty()) {
                eventPacket.name = "unknown_" + std::to_string(packetId);
            }

            bool apiDump = std::getenv("BEDROCK_API_DUMP") != nullptr;
            bool apiFast = std::getenv("BEDROCK_API_FAST") != nullptr
                && std::string(std::getenv("BEDROCK_API_FAST")) == "1";

            static uint64_t apiPacketSeq = 0;

            if (apiDump && apiFast) {
                std::cout << "[API] packet"
                          << " seq=" << (++apiPacketSeq)
                          << " id=" << eventPacket.packetId
                          << " name=" << apiFieldEscape(eventPacket.name)
                          << " ok=true"
                          << " fields=5"
                          << " direction=clientbound"
                          << " raw_size=" << eventPacket.fullPacket.size()
                          << " payload_size=" << eventPacket.payload.size()
                          << "\n" << std::flush;
                gotEncryptedGamePacket = true;
                continue;
            }

            static bedrock::BedrockPacketEventDispatcher packetEvents(g_minecraftVersion);
            bedrock::BedrockPacketEvent emittedEvent;
            emittedEvent.version = g_minecraftVersion;
            emittedEvent.packetId = eventPacket.packetId;
            emittedEvent.packetName = eventPacket.name;
            emittedEvent.rawPacket = eventPacket.fullPacket;
            emittedEvent.payload = eventPacket.payload;

            if (g_decodeEvents) {
                try {
                    emittedEvent = packetEvents.dispatch(eventPacket);
                } catch (const std::exception& e) {
                    emittedEvent.version = g_minecraftVersion;
                    emittedEvent.packetId = eventPacket.packetId;
                    emittedEvent.packetName = eventPacket.name;
                    emittedEvent.rawPacket = eventPacket.fullPacket;
                    emittedEvent.payload = eventPacket.payload;
                    emittedEvent.decodeError = e.what();
                } catch (...) {
                    emittedEvent.version = g_minecraftVersion;
                    emittedEvent.packetId = eventPacket.packetId;
                    emittedEvent.packetName = eventPacket.name;
                    emittedEvent.rawPacket = eventPacket.fullPacket;
                    emittedEvent.payload = eventPacket.payload;
                    emittedEvent.decodeError = "unknown decode error";
                }
            }
            static bedrock::legacy::Client mc;
            static bool mcHandlersReady = false;

            if (!mcHandlersReady) {
                mc.on("packet", [](auto& packet) {
                    std::cout << packet << "\n";
                });

                mcHandlersReady = true;
            }

            auto apiEvent = bedrock::toPacketEventApi(emittedEvent);

            if (apiDump) {
                std::cout << "[API] packet"
                          << " id=" << apiEvent.id
                          << " name=" << apiEvent.name
                          << " ok=" << (apiEvent.ok() ? "true" : "false")
                          << " fields=" << (apiEvent.params.size() + 1)
                          << " direction=clientbound";

                for (const auto& kv : apiEvent.params) {
                    std::cout << " " << kv.first << "=" << apiFieldEscape(kv.second);
                }
                if (!emittedEvent.decodeError.empty()) std::cout << " decode_error=" << apiFieldEscape(emittedEvent.decodeError);

                std::cout << "\n" << std::flush;

                if (apiEvent.name == "disconnect") {
                    g_gotDisconnect = true;
                    std::ostringstream dmsg;
                    dmsg << "[DISCONNECT]";
                    if (apiEvent.has("reason")) dmsg << " reason=" << apiEvent.get("reason");
                    if (apiEvent.has("message")) dmsg << " message=" << apiEvent.get("message");
                    if (apiEvent.has("filtered_message")) dmsg << " filtered=" << apiEvent.get("filtered_message");
                    if (apiEvent.has("hide_disconnect_reason")) dmsg << " hidden=" << apiEvent.get("hide_disconnect_reason");
                    g_disconnectText = dmsg.str();
                    std::cout << g_disconnectText << "\n" << std::flush;
                }
            } else {
                if (g_packetDumpMode) {
                    auto packetObject = bedrock::packetFromEvent(emittedEvent);
                    mc.emit(packetObject);
                }

                std::cout << "[EVENT] packet "
                          << emittedEvent.packetName
                          << " id="
                          << emittedEvent.packetId
                          << " fields="
                          << emittedEvent.fields.size();

                if (!emittedEvent.decodeError.empty()) {
                    std::cout << " decode_error=" << emittedEvent.decodeError;
                }

                std::cout << "\n";
            }
            gotEncryptedGamePacket = true;
        }
    } catch (const std::exception& e) {
        std::cout << "[DECRYPT] failed: "
                  << e.what()
                  << "\n";

        std::cout << "[DECRYPT] encrypted payload hex=";
        printHex(payload, 160);
    }
}


static bool parseConnectionRequestAccepted(
    const std::vector<uint8_t>& payload,
    int64_t& requestTimestamp,
    int64_t& acceptedTimestamp
) {
    if (payload.empty() || payload[0] != ID_CONNECTION_REQUEST_ACCEPTED) {
        return false;
    }

    size_t off = 1;

    auto clientAddress = readRakNetAddress(payload, off);

    uint16_t systemIndex = readU16BE(payload, off);

    std::cout << "[ACCEPTED] clientAddress="
              << clientAddress.first
              << ":"
              << clientAddress.second
              << " systemIndex="
              << systemIndex
              << "\n";

    for (int i = 0; i < 20; i++) {
        auto addr = readRakNetAddress(payload, off);

        if (i < 3) {
            std::cout << "[ACCEPTED] systemAddress[" << i << "]="
                      << addr.first
                      << ":"
                      << addr.second
                      << "\n";
        }
    }

    requestTimestamp = static_cast<int64_t>(readU64BE(payload, off));
    acceptedTimestamp = static_cast<int64_t>(readU64BE(payload, off));

    std::cout << "[ACCEPTED] requestTimestamp="
              << requestTimestamp
              << " acceptedTimestamp="
              << acceptedTimestamp
              << "\n";

    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage:\n";
        std::cout << "  " << argv[0] << " <host> [port] [mtu] [mcProtocol] [mode] [packetId] [delay] [loginPacket] [--hold] [--packet-dump] [--packet-json] [--version <version>] [--private-key <pem>]\n\n";
        std::cout << "Modes:\n";
        std::cout << "  fe-len          default: 0xfe + varint length + gamepacket\n";
        std::cout << "  fe-raw          0xfe + raw gamepacket\n";
        std::cout << "  len             varint length + gamepacket\n";
        std::cout << "  raw             raw gamepacket\n";
        std::cout << "  fe-nocomp-len   diagnostic: 0xfe + 0xff + length + gamepacket\n\n";
        std::cout << "Examples:\n";
        std::cout << "  " << argv[0] << " modern.server.net 19132 1400 893 fe-len\n";
        std::cout << "  " << argv[0] << " cpe.ign.gg 19132 1400 419 fe-len\n";
        return 1;
    }

    std::string host = argv[1];
    uint16_t port = 19132;
    int mtu = 1400;
    int mcProtocol = 893;
    std::string mcMode = "fe-len";
    uint32_t networkSettingsRequestPacketId = ID_REQUEST_NETWORK_SETTINGS;
    int delayAfterNewIncomingMs = 250;
    std::string loginPacketPath = "/tmp/login_packet.bin";
    std::string privateKeyPath;
    std::string commandFilePath;
    std::streampos commandFileOffset = 0;
    int holdSeconds = 0;
    bool holdForever = false;

    if (argc >= 3) {
        int p = std::stoi(argv[2]);

        if (p <= 0 || p > 65535) {
            std::cerr << "Invalid port\n";
            return 1;
        }

        port = static_cast<uint16_t>(p);
    }

    if (argc >= 4) {
        mtu = std::stoi(argv[3]);

        if (mtu < 576 || mtu > 1492) {
            std::cerr << "Invalid MTU\n";
            return 1;
        }
    }

    if (argc >= 5) {
        mcProtocol = std::stoi(argv[4]);
    }

    if (argc >= 6) {
        mcMode = argv[5];
    }

    if (argc >= 7) {
        std::string idArg = argv[6];

        if (idArg.rfind("0x", 0) == 0 || idArg.rfind("0X", 0) == 0) {
            networkSettingsRequestPacketId = static_cast<uint32_t>(
                std::stoul(idArg, nullptr, 16)
            );
        } else {
            networkSettingsRequestPacketId = static_cast<uint32_t>(
                std::stoul(idArg, nullptr, 10)
            );
        }
    }

    if (argc >= 8) {
        delayAfterNewIncomingMs = std::stoi(argv[7]);
    }

    if (argc >= 9) {
        loginPacketPath = argv[8];
    }

    if (argc >= 10) {
        for (int argi = 9; argi < argc; ++argi) {
            std::string holdArg = argv[argi];

            if (holdArg == "--packet-dump") {
                g_packetDumpMode = true;
                g_decodeEvents = true;
                continue;
            }
            if (holdArg == "--packet-json" || holdArg == "--api-json") {
                g_packetJsonMode = true;
                g_decodeEvents = true;
                continue;
            }
            if (holdArg == "--decode-events") {
                g_decodeEvents = true;
                continue;
            }
            if (holdArg == "--version" && argi + 1 < argc) {
                g_minecraftVersion = argv[++argi];
                continue;
            }
            if (holdArg == "--private-key" && argi + 1 < argc) {
                privateKeyPath = argv[++argi];
                continue;
            }
            if (holdArg == "--command-file" && argi + 1 < argc) {
                commandFilePath = argv[++argi];
                continue;
            }
            if (holdArg.rfind("--version=", 0) == 0) {
                g_minecraftVersion = holdArg.substr(std::string("--version=").size());
                continue;
            }

            if (holdArg == "--hold" || holdArg == "hold" || holdArg == "forever") {
                holdForever = true;
            } else {
                holdSeconds = std::stoi(holdArg);
                if (holdSeconds < 0) {
                    holdSeconds = 0;
                }
            }
        }
    }

    try {
        std::cout << "[SESSION] host=" << host
                  << " port=" << port
                  << " mtu=" << mtu
                  << " mcProtocol=" << mcProtocol
                  << " mcMode=" << mcMode
                  << "\n";

        OpenState st = openRakNetSocket(host, port, mtu);

        uint32_t datagramSeq = 0;
        uint32_t reliableIndex = 0;
        uint32_t orderedIndex = 0;

        std::map<uint16_t, IncomingSplitAssembly> incomingSplits;
        std::vector<PendingInboundPayload> pendingInboundPayloads;

        bool networkSettingsSent = false;
        bool gotNetworkSettings = false;

        bool waitingNewIncomingAck = false;
        uint32_t newIncomingSeq = 0;
        std::vector<uint8_t> lastNewIncomingDatagram;
        auto lastNewIncomingSendTime = std::chrono::steady_clock::now();

        bool loginSent = false;
        bool gotPostLoginGamePacket = false;
        uint32_t loginSeq = 0;

        std::map<uint32_t, std::vector<uint8_t>> sentReliableDatagrams;

        bool encryptedHandshakeSent = false;
        bool encryptedHandshakeAcked = false;
        uint32_t encryptedHandshakeSeq = 0;
        uint64_t encryptedSendCounter = 0;
        uint64_t encryptedReceiveCounter = 0;
        bool encryptionReadyForInbound = false;
        bool gotEncryptedInboundGamePacket = false;
        std::vector<uint8_t> encryptionSecretKeyBytes;
        std::vector<uint8_t> encryptionIv16;
        std::unique_ptr<bedrock::BedrockCipherStream> encryptStream;
        std::unique_ptr<bedrock::BedrockCipherStream> decryptStream;

        bool sentHaveAllPacks = false;
        bool haveAllPacksAcked = false;
        uint32_t haveAllPacksSeq = 0;

        bool sentCompleted = false;
        bool completedAcked = false;
        uint32_t completedSeq = 0;

        bool sentClientCacheStatus = false;
        bool clientCacheStatusAcked = false;
        uint32_t clientCacheStatusSeq = 0;

        bool sentRequestChunkRadius = false;
        bool requestChunkRadiusAcked = false;
        uint32_t requestChunkRadiusSeq = 0;

        bool sentLocalPlayerInitialized = false;
        bool localPlayerInitializedAcked = false;
        uint32_t localPlayerInitializedSeq = 0;

        int64_t connectionRequestTime = nowMillis();

        auto connReqPayload = buildConnectionRequest(
            st.clientGuid,
            connectionRequestTime
        );

        auto datagram = buildReliableOrderedDatagram(
            datagramSeq++,
            reliableIndex++,
            orderedIndex++,
            0,
            connReqPayload
        );

        std::cout << "[SEND] ConnectionRequest payload="
                  << connReqPayload.size()
                  << " datagram="
                  << datagram.size()
                  << "\n";

        sendPacket(st.sock, st.target, datagram);

        bool accepted = false;
        int64_t acceptedRequestTs = 0;
        int64_t acceptedServerTs = 0;

        auto start = std::chrono::steady_clock::now();
        auto lastPacketTime = start;
        bool warnedSlowJoin = false;
        auto holdStart = std::chrono::steady_clock::now();
        bool holdStarted = false;
        bool initAckLogged = false;

        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start
            ).count();

            if (g_gotDisconnect && !g_seenStartGame) {
                if (!g_disconnectText.empty()) {
                    std::cerr << "[ERROR] server disconnected before start_game: " << g_disconnectText << "\n";
                } else {
                    std::cerr << "[ERROR] server disconnected before start_game.\n";
                }
                break;
            }

            if (!holdStarted && !g_seenStartGame && elapsed > 6500) {
                std::cerr << "[ERROR] join timeout: no start_game after 6.5s. Server rejected this protocol/version, silently closed, or old session is still disconnecting.\n";
                break;
            }

            if (!holdStarted && !warnedSlowJoin && !g_seenStartGame && elapsed > 2500) {
                std::cerr << "[WAIT] still waiting for start_game... if this was a quick reconnect, wait a few seconds or use another account/profile.\n";
                warnedSlowJoin = true;
            }

            if (holdStarted && !holdForever && holdSeconds > 0) {
                auto holdElapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - holdStart
                ).count();

                if (holdElapsed >= holdSeconds) {
                    std::cout << "[HOLD] completed holdSeconds=" << holdSeconds << "\n";
                    break;
                }
            }

            for (const auto& command : pollRuntimeCommands(commandFilePath, commandFileOffset)) {
                try {
                    auto gamePacket = makeRuntimeCommandPacket(command);
                    sendGamePacketReliable(
                        st.sock,
                        st.target,
                        sentReliableDatagrams,
                        datagramSeq,
                        reliableIndex,
                        orderedIndex,
                        gamePacket,
                        encryptionReadyForInbound,
                        encryptedSendCounter,
                        encryptionSecretKeyBytes,
                        encryptStream.get(),
                        "ApiSend:" + command.name
                    );
                } catch (const std::exception& e) {
                    std::cerr << "[API-SEND] " << e.what() << "\n";
                }
            }

            std::vector<uint8_t> packet;

            if (!recvPacket(st.sock, packet, 1000)) {
                if (!g_seenStartGame) {
                    std::cout << "[WAIT] no packet yet before join\n";
                }

                if (waitingNewIncomingAck && !lastNewIncomingDatagram.empty()) {
                    auto now = std::chrono::steady_clock::now();
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - lastNewIncomingSendTime
                    ).count();

                    if (ms >= 1000) {
                        sendPacket(st.sock, st.target, lastNewIncomingDatagram);
                        lastNewIncomingSendTime = now;

                        std::cout << "[RESEND] NewIncomingConnection seq="
                                  << newIncomingSeq
                                  << " waiting for ACK\n";
                    }
                }

                continue;
            }

            if (packet.empty()) {
                continue;
            }

            lastPacketTime = std::chrono::steady_clock::now();
            uint8_t pid = packet[0];

            std::cout << "[RECV] packetId=0x"
                      << std::hex
                      << static_cast<int>(pid)
                      << std::dec
                      << " size="
                      << packet.size()
                      << "\n";

            if (pid == ID_ACK) {
                auto ackSeqs = parseAckSequences(packet);

                std::cout << "[ACK] server acknowledged datagrams: ";

                for (uint32_t seq : ackSeqs) {
                    std::cout << seq << " ";
                }

                std::cout << "\n";

                if (waitingNewIncomingAck) {
                    for (uint32_t seq : ackSeqs) {
                        if (seq == newIncomingSeq) {
                            waitingNewIncomingAck = false;

                            std::cout << "[ACK] NewIncomingConnection acknowledged seq="
                                      << newIncomingSeq
                                      << "\n";

                            if (mcProtocol < 554) {
                                std::cout << "[WARN] mcProtocol="
                                          << mcProtocol
                                          << " is older than 554. NetworkSettingsRequest is not the next packet for this server.\n";
                                std::cout << "[WARN] For this old server we need the legacy LoginPacket path in the next stage.\n";
                            } else if (!networkSettingsSent) {
                                auto nsPayload = buildNetworkSettingsRequestUserPacket(
                                    mcProtocol,
                                    mcMode,
                                    networkSettingsRequestPacketId
                                );

                                uint32_t nsSeq = datagramSeq++;

                                auto nsDatagram = buildReliableOrderedDatagram(
                                    nsSeq,
                                    reliableIndex++,
                                    orderedIndex++,
                                    0,
                                    nsPayload
                                );

                                std::cout << "[SEND] NetworkSettingsRequest seq="
                                          << nsSeq
                                          << " packetId="
                                          << networkSettingsRequestPacketId
                                          << " protocol="
                                          << mcProtocol
                                          << " mode="
                                          << mcMode
                                          << " payload="
                                          << nsPayload.size()
                                          << " datagram="
                                          << nsDatagram.size()
                                          << "\n";

                                std::cout << "[SEND] NetworkSettingsRequest hex=";
                                printHex(nsPayload, 96);

                                sendPacket(st.sock, st.target, nsDatagram);
                                sentReliableDatagrams[nsSeq] = nsDatagram;
                                networkSettingsSent = true;
                            }
                        }
                    }
                }

                if (encryptedHandshakeSent && !encryptedHandshakeAcked) {
                    for (uint32_t seq : ackSeqs) {
                        if (seq == encryptedHandshakeSeq) {
                            encryptedHandshakeAcked = true;

                            std::cout << "[ACK] encrypted client_to_server_handshake acknowledged seq="
                                      << encryptedHandshakeSeq
                                      << "\n";
                        }
                    }
                }

                if (sentHaveAllPacks && !haveAllPacksAcked) {
                    for (uint32_t seq : ackSeqs) {
                        if (seq == haveAllPacksSeq) {
                            haveAllPacksAcked = true;

                            std::cout << "[ACK] ResourcePack have_all_packs acknowledged seq="
                                      << haveAllPacksSeq
                                      << "\n";
                        }
                    }
                }

                if (sentCompleted && !completedAcked) {
                    for (uint32_t seq : ackSeqs) {
                        if (seq == completedSeq) {
                            completedAcked = true;

                            std::cout << "[ACK] ResourcePack completed acknowledged seq="
                                      << completedSeq
                                      << "\n";
                        }
                    }
                }

                if (sentClientCacheStatus && !clientCacheStatusAcked) {
                    for (uint32_t seq : ackSeqs) {
                        if (seq == clientCacheStatusSeq) {
                            clientCacheStatusAcked = true;

                            std::cout << "[ACK] ClientCacheStatus acknowledged seq="
                                      << clientCacheStatusSeq
                                      << "\n";
                        }
                    }
                }

                if (sentRequestChunkRadius && !requestChunkRadiusAcked) {
                    for (uint32_t seq : ackSeqs) {
                        if (seq == requestChunkRadiusSeq) {
                            requestChunkRadiusAcked = true;

                            std::cout << "[ACK] RequestChunkRadius acknowledged seq="
                                      << requestChunkRadiusSeq
                                      << "\n";
                        }
                    }
                }

                if (sentLocalPlayerInitialized && !localPlayerInitializedAcked) {
                    for (uint32_t seq : ackSeqs) {
                        if (seq == localPlayerInitializedSeq) {
                            localPlayerInitializedAcked = true;

                            std::cout << "[ACK] SetLocalPlayerAsInitialized acknowledged seq="
                                      << localPlayerInitializedSeq
                                      << "\n";
                        }
                    }
                }

                continue;
            }

            if (pid == ID_NACK) {
                auto nackSeqs = parseAckSequences(packet);

                std::cout << "[NACK] server requested retransmit: ";

                for (uint32_t seq : nackSeqs) {
                    std::cout << seq << " ";
                }

                std::cout << "\n";

                for (uint32_t seq : nackSeqs) {
                    auto it = sentReliableDatagrams.find(seq);

                    if (it != sentReliableDatagrams.end()) {
                        sendPacket(st.sock, st.target, it->second);

                        std::cout << "[RESEND] datagram seq="
                                  << seq
                                  << " size="
                                  << it->second.size()
                                  << "\n";
                    } else {
                        std::cout << "[RESEND] missing datagram seq="
                                  << seq
                                  << "\n";
                    }
                }

                continue;
            }

            if (pid >= 0x80 && pid <= 0x8f) {
                uint32_t incomingSeq = 0;
                auto frames = parseConnectedDatagram(packet, incomingSeq);

                std::cout << "[FRAMESET] seq="
                          << incomingSeq
                          << " frames="
                          << frames.size()
                          << "\n";

                auto ack = buildAck(incomingSeq);
                sendPacket(st.sock, st.target, ack);

                std::cout << "[SEND] ACK seq="
                          << incomingSeq
                          << "\n";

                for (const auto& frame : frames) {
                    std::vector<uint8_t> framePayload;

                    if (frame.split) {
                        auto& assembly = incomingSplits[frame.splitId];

                        if (assembly.splitCount == 0) {
                            assembly.splitCount = frame.splitCount;
                        }

                        assembly.chunks[frame.splitIndex] = frame.payload;

                        std::cout << "[SPLIT-IN] id="
                                  << frame.splitId
                                  << " index="
                                  << frame.splitIndex
                                  << "/"
                                  << frame.splitCount
                                  << " chunk="
                                  << framePayload.size()
                                  << " have="
                                  << assembly.chunks.size()
                                  << "\n";

                        if (assembly.chunks.size() < assembly.splitCount) {
                            continue;
                        }

                        for (uint32_t i = 0; i < assembly.splitCount; i++) {
                            auto it = assembly.chunks.find(i);

                            if (it == assembly.chunks.end()) {
                                std::cout << "[SPLIT-IN] missing chunk index="
                                          << i
                                          << " for splitId="
                                          << frame.splitId
                                          << "\n";
                                continue;
                            }

                            framePayload.insert(
                                framePayload.end(),
                                it->second.begin(),
                                it->second.end()
                            );
                        }

                        std::cout << "[SPLIT-IN] reassembled id="
                                  << frame.splitId
                                  << " total="
                                  << framePayload.size()
                                  << "\n";

                        incomingSplits.erase(frame.splitId);
                    } else {
                        framePayload = frame.payload;
                    }

                    auto processInboundPayload = [&](const std::vector<uint8_t>& payloadToProcess,
                                                       uint8_t reliabilityToProcess,
                                                       bool splitToProcess) {
                        if (payloadToProcess.empty()) {
                            return;
                        }

                        uint8_t innerId = payloadToProcess[0];

                        std::cout << "[INNER] id=0x"
                                  << std::hex
                                  << static_cast<int>(innerId)
                                  << std::dec
                                  << " payload="
                                  << payloadToProcess.size()
                                  << " reliability="
                                  << static_cast<int>(reliabilityToProcess)
                                  << " split="
                                  << (splitToProcess ? "true" : "false")
                                  << "\n";

                        if (encryptedHandshakeSent && encryptionReadyForInbound) {
                            inspectEncryptedBedrockPayload(
                                payloadToProcess,
                                encryptionSecretKeyBytes,
                                *decryptStream,
                                encryptedReceiveCounter,
                                gotEncryptedInboundGamePacket
                            );
                        } else if (loginSent) {
                            inspectDecodedBedrockBatch(
                                payloadToProcess,
                                true,
                                gotPostLoginGamePacket
                            );
                        } else {
                            inspectPossibleGamePackets(payloadToProcess, gotNetworkSettings);
                        }

                        if (innerId == 0x15) {
                            std::cout << "[DISCONNECT] Server sent RakNet DisconnectNotification 0x15\n";
                            std::cout << "[DISCONNECT] This usually means Minecraft payload/protocol was rejected.\n";
                        }

                        if (innerId == ID_CONNECTED_PING) {
                            size_t off = 1;
                            int64_t pingTime = static_cast<int64_t>(readU64BE(payloadToProcess, off));

                            auto pongPayload = buildConnectedPong(
                                pingTime,
                                nowMillis()
                            );

                            auto pongDatagram = buildReliableOrderedDatagram(
                                datagramSeq++,
                                reliableIndex++,
                                orderedIndex++,
                                0,
                                pongPayload
                            );

                            sendPacket(st.sock, st.target, pongDatagram);

                            std::cout << "[SEND] ConnectedPong\n";
                        }

                        if (innerId == ID_CONNECTION_REQUEST_ACCEPTED) {
                            accepted = parseConnectionRequestAccepted(
                                payloadToProcess,
                                acceptedRequestTs,
                                acceptedServerTs
                            );

                            auto newIncomingPayload = buildNewIncomingConnection(
                                st.resolvedIp,
                                st.port,
                                acceptedRequestTs,
                                acceptedServerTs
                            );

                            newIncomingSeq = datagramSeq++;

                            auto newIncomingDatagram = buildReliableOrderedDatagram(
                                newIncomingSeq,
                                reliableIndex++,
                                orderedIndex++,
                                0,
                                newIncomingPayload
                            );

                            lastNewIncomingDatagram = newIncomingDatagram;
                            lastNewIncomingSendTime = std::chrono::steady_clock::now();
                            waitingNewIncomingAck = true;

                            sendPacket(st.sock, st.target, newIncomingDatagram);
                            sentReliableDatagrams[newIncomingSeq] = newIncomingDatagram;

                            std::cout << "[SEND] NewIncomingConnection seq="
                                      << newIncomingSeq
                                      << " payload="
                                      << newIncomingPayload.size()
                                      << " datagram="
                                      << newIncomingDatagram.size()
                                      << "\n";

                            std::cout << "[WAIT] waiting ACK for NewIncomingConnection before NetworkSettingsRequest\n";
                        }
                    };

                    if (framePayload.empty()) {
                        continue;
                    }

                    bool shouldDeferForOrdering =
                        frame.split ||
                        !incomingSplits.empty() ||
                        !pendingInboundPayloads.empty();

                    if (shouldDeferForOrdering) {
                        pendingInboundPayloads.push_back({
                            frame.orderedIndex,
                            frame.reliability,
                            frame.split,
                            framePayload
                        });

                        std::cout << "[ORDER-IN] queued orderedIndex="
                                  << frame.orderedIndex
                                  << " split="
                                  << (frame.split ? "true" : "false")
                                  << " payload="
                                  << framePayload.size()
                                  << " pending="
                                  << pendingInboundPayloads.size()
                                  << " activeSplits="
                                  << incomingSplits.size()
                                  << "\n";

                        if (!incomingSplits.empty()) {
                            continue;
                        }

                        std::stable_sort(
                            pendingInboundPayloads.begin(),
                            pendingInboundPayloads.end(),
                            [](const PendingInboundPayload& a, const PendingInboundPayload& b) {
                                return a.orderedIndex < b.orderedIndex;
                            }
                        );

                        std::cout << "[ORDER-IN] flushing pending payloads count="
                                  << pendingInboundPayloads.size()
                                  << "\n";

                        auto pendingCopy = pendingInboundPayloads;
                        pendingInboundPayloads.clear();

                        for (const auto& pending : pendingCopy) {
                            processInboundPayload(
                                pending.payload,
                                pending.reliability,
                                pending.split
                            );
                        }

                        continue;
                    }

                    processInboundPayload(
                        framePayload,
                        frame.reliability,
                        frame.split
                    );
                }

                if (accepted && mcProtocol < 554) {
                    break;
                }


                if (loginSent && g_gotServerHandshakeJwt && !encryptedHandshakeSent) {
                    try {
                        if (privateKeyPath.empty()) {
                            throw std::runtime_error("private key path was not provided");
                        }
                        auto privateKeyBytes = readBinaryFile(privateKeyPath);

                        std::string privateKeyPem(
                            reinterpret_cast<const char*>(privateKeyBytes.data()),
                            privateKeyBytes.size()
                        );

                        auto derived = bedrock::BedrockKeyExchange::deriveFromServerHandshakeJwtAndPrivateKeyPem(
                            g_serverHandshakeJwt,
                            privateKeyPem
                        );

                        std::cout << "[ENCRYPTION] derived secretKey size="
                                  << derived.secretKeyBytes.size()
                                  << " iv16="
                                  << derived.iv16.size()
                                  << "\n";

                        encryptionSecretKeyBytes = derived.secretKeyBytes;
                        encryptionIv16 = derived.iv16;
                        encryptionReadyForInbound = true;

                        encryptStream = bedrock::BedrockEncryption::createCipherStream(
                            static_cast<uint32_t>(mcProtocol),
                            encryptionSecretKeyBytes,
                            encryptionIv16,
                            bedrock::BedrockCipherMode::Encrypt
                        );

                        decryptStream = bedrock::BedrockEncryption::createCipherStream(
                            static_cast<uint32_t>(mcProtocol),
                            encryptionSecretKeyBytes,
                            encryptionIv16,
                            bedrock::BedrockCipherMode::Decrypt
                        );

                        // Raw client_to_server_handshake packet is packet id 4.
                        std::vector<uint8_t> clientHandshakePacket = { 0x04 };

                        auto framedPackets = bedrock::BedrockFramer::framePackets(
                            { clientHandshakePacket }
                        );

                        auto compressed = bedrock::BedrockFramer::deflateRaw(
                            framedPackets
                        );

                        std::vector<uint8_t> compressorPacket;
                        if (usesCompressionHeader()) {
                            compressorPacket.push_back(0x00); // deflate compression header
                        }
                        compressorPacket.insert(
                            compressorPacket.end(),
                            compressed.begin(),
                            compressed.end()
                        );

                        auto aesPlaintext = bedrock::BedrockEncryption::makeAesPlaintext(
                            compressorPacket,
                            encryptedSendCounter++,
                            derived.secretKeyBytes
                        );

                        auto encryptedOnly = encryptStream->process(aesPlaintext);

                        std::vector<uint8_t> encryptedMcpe;
                        encryptedMcpe.reserve(1 + encryptedOnly.size());
                        encryptedMcpe.push_back(0xfe);
                        encryptedMcpe.insert(
                            encryptedMcpe.end(),
                            encryptedOnly.begin(),
                            encryptedOnly.end()
                        );

                        encryptedHandshakeSeq = datagramSeq++;

                        auto encryptedDatagram = buildReliableOrderedDatagram(
                            encryptedHandshakeSeq,
                            reliableIndex++,
                            orderedIndex++,
                            0,
                            encryptedMcpe
                        );

                        sendPacket(st.sock, st.target, encryptedDatagram);
                        sentReliableDatagrams[encryptedHandshakeSeq] = encryptedDatagram;

                        encryptedHandshakeSent = true;

                        std::cout << "[SEND] EncryptedClientHandshake seq="
                                  << encryptedHandshakeSeq
                                  << " packet="
                                  << clientHandshakePacket.size()
                                  << " framed="
                                  << framedPackets.size()
                                  << " compressorPacket="
                                  << compressorPacket.size()
                                  << " encryptedMcpe="
                                  << encryptedMcpe.size()
                                  << " datagram="
                                  << encryptedDatagram.size()
                                  << "\n";

                        std::cout << "[SEND] EncryptedClientHandshake hex=";
                        printHex(encryptedMcpe, 96);
                    } catch (const std::exception& e) {
                        std::cerr << "[ENCRYPTION] failed to send encrypted handshake: "
                                  << e.what()
                                  << "\n";
                        return 8;
                    }
                }

                if (accepted && networkSettingsSent && gotNetworkSettings && !loginSent) {
                    try {
                        auto loginPacket = readBinaryFile(loginPacketPath);

                        std::cout << "[LOGIN] loaded one-run login packet size="
                                  << loginPacket.size()
                                  << "\n";

                        bedrock::BedrockFramerSettings framerSettings;
                        framerSettings.compressionReady = true;
                        framerSettings.compressorInHeader = usesCompressionHeader();
                        framerSettings.compressionThreshold = 256;
                        framerSettings.compressionAlgorithm = 0;

                        auto loginBatch = bedrock::BedrockFramer::encodeBatch(
                            { loginPacket },
                            framerSettings
                        );

                        std::cout << "[SEND] Login rawLogin="
                                  << loginPacket.size()
                                  << " batch="
                                  << loginBatch.size()
                                  << "\n";

                        std::cout << "[SEND] Login batch first bytes=";
                        printHex(loginBatch, 96);

                        const size_t safeFragmentPayload = 1200;

                        if (loginBatch.size() + 64 <= static_cast<size_t>(st.mtu)) {
                            loginSeq = datagramSeq++;

                            auto loginDatagram = buildReliableOrderedDatagram(
                                loginSeq,
                                reliableIndex++,
                                orderedIndex++,
                                0,
                                loginBatch
                            );

                            sendPacket(st.sock, st.target, loginDatagram);
                            sentReliableDatagrams[loginSeq] = loginDatagram;

                            std::cout << "[SEND] Login single seq="
                                      << loginSeq
                                      << " datagram="
                                      << loginDatagram.size()
                                      << "\n";
                        } else {
                            auto splitDatagrams = buildReliableOrderedSplitDatagrams(
                                datagramSeq,
                                reliableIndex,
                                orderedIndex,
                                0,
                                loginBatch,
                                safeFragmentPayload
                            );

                            std::cout << "[SEND] Login split count="
                                      << splitDatagrams.size()
                                      << " fragmentPayload="
                                      << safeFragmentPayload
                                      << "\n";

                            for (const auto& sd : splitDatagrams) {
                                sendPacket(st.sock, st.target, sd.data);
                                sentReliableDatagrams[sd.sequence] = sd.data;

                                std::cout << "[SEND] split seq="
                                          << sd.sequence
                                          << " datagram="
                                          << sd.data.size()
                                          << "\n";
                            }

                            if (!splitDatagrams.empty()) {
                                loginSeq = splitDatagrams.front().sequence;
                            }
                        }

                        loginSent = true;
                    } catch (const std::exception& e) {
                        std::cerr << "[LOGIN] failed to send login: "
                                  << e.what()
                                  << "\n";
                        return 5;
                    }
                }

                if (g_seenResourcePacksInfo && !sentHaveAllPacks) {
                    auto responseCompleted =
                        bedrock::PacketFactory::resourcePackClientResponseCompleted();

                    haveAllPacksSeq = sendGamePacketReliable(
                        st.sock,
                        st.target,
                        sentReliableDatagrams,
                        datagramSeq,
                        reliableIndex,
                        orderedIndex,
                        responseCompleted,
                        encryptionReadyForInbound,
                        encryptedSendCounter,
                        encryptionSecretKeyBytes,
                        encryptStream.get(),
                        "ResourcePackResponseCompletedInfo"
                    );

                    sentHaveAllPacks = true;
                }

                if (g_seenResourcePackStack && !sentCompleted) {
                    auto responseCompleted =
                        bedrock::PacketFactory::resourcePackClientResponseCompleted();

                    completedSeq = sendGamePacketReliable(
                        st.sock,
                        st.target,
                        sentReliableDatagrams,
                        datagramSeq,
                        reliableIndex,
                        orderedIndex,
                        responseCompleted,
                        encryptionReadyForInbound,
                        encryptedSendCounter,
                        encryptionSecretKeyBytes,
                        encryptStream.get(),
                        "ResourcePackResponseCompleted"
                    );

                    sentCompleted = true;
                }

                if (g_seenStartGame && !sentClientCacheStatus) {
                    std::cout << "[STARTGAME] seen. Sending post-startgame client init packets.\n";

                    auto clientCacheStatus =
                        bedrock::PacketFactory::clientCacheStatus(false);

                    clientCacheStatusSeq = sendGamePacketReliable(
                        st.sock,
                        st.target,
                        sentReliableDatagrams,
                        datagramSeq,
                        reliableIndex,
                        orderedIndex,
                        clientCacheStatus,
                        encryptionReadyForInbound,
                        encryptedSendCounter,
                        encryptionSecretKeyBytes,
                        encryptStream.get(),
                        "ClientCacheStatus"
                    );

                    sentClientCacheStatus = true;

                    auto requestChunkRadius =
                        bedrock::PacketFactory::requestChunkRadius(20);

                    requestChunkRadiusSeq = sendGamePacketReliable(
                        st.sock,
                        st.target,
                        sentReliableDatagrams,
                        datagramSeq,
                        reliableIndex,
                        orderedIndex,
                        requestChunkRadius,
                        encryptionReadyForInbound,
                        encryptedSendCounter,
                        encryptionSecretKeyBytes,
                        encryptStream.get(),
                        "RequestChunkRadius"
                    );

                    sentRequestChunkRadius = true;

                    auto setLocalPlayerInitialized =
                        bedrock::PacketFactory::setLocalPlayerInitializedMinusOne();

                    localPlayerInitializedSeq = sendGamePacketReliable(
                        st.sock,
                        st.target,
                        sentReliableDatagrams,
                        datagramSeq,
                        reliableIndex,
                        orderedIndex,
                        setLocalPlayerInitialized,
                        encryptionReadyForInbound,
                        encryptedSendCounter,
                        encryptionSecretKeyBytes,
                        encryptStream.get(),
                        "SetLocalPlayerAsInitialized"
                    );

                    sentLocalPlayerInitialized = true;
                }

                if (
                    sentClientCacheStatus &&
                    sentRequestChunkRadius &&
                    sentLocalPlayerInitialized &&
                    clientCacheStatusAcked &&
                    requestChunkRadiusAcked &&
                    localPlayerInitializedAcked
                ) {
                    if (!initAckLogged) {
                        std::cout << "[INIT] post-startgame client init packets ACKed\n";
                        initAckLogged = true;
                    }

                    if (holdForever || holdSeconds > 0) {
                        if (!holdStarted) {
                            holdStarted = true;
                            holdStart = std::chrono::steady_clock::now();

                            if (holdForever) {
                                std::cout << "[HOLD] bot joined; holding connection forever\n";
                            } else {
                                std::cout << "[HOLD] bot joined; holding connection for "
                                          << holdSeconds
                                          << " seconds\n";
                            }
                        }

                        continue;
                    }

                    break;
                }

                // Do not break immediately after completed ACK.
                // After ResourcePack completed, server may send a huge split StartGame/ItemRegistry batch.
            }
        }

        close(st.sock);

        if (!accepted) {
            std::cerr << "[FAIL] did not receive ConnectionRequestAccepted\n";
            return 2;
        }

        if (mcProtocol >= 554 && !gotNetworkSettings) {
            std::cerr << "[FAIL] did not receive NetworkSettings response\n";
            std::cerr << "[HINT] Try another mode: fe-raw, len, raw, fe-nocomp-len\n";
            std::cerr << "[HINT] Example: ./build/mc_network_settings_test <host> 19132 1400 "
                      << mcProtocol
                      << " fe-raw\n";
            return 4;
        }

        std::cout << "\n[SUMMARY] gamePackets="
                  << g_gamePacketCount
                  << " levelChunks="
                  << g_levelChunkCount
                  << " startGame="
                  << (g_seenStartGame ? "yes" : "no")
                  << "\n";

        if (!g_seenStartGame) {
            std::cerr << "[ERROR] session ended before start_game. Server rejected the login, the protocol/version is wrong, or the server closed silently.\n";
            return 8;
        }

        std::cout << "\n[OK] Minecraft session completed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 3;
    }
}
