#include "bedrock/RakNetPing.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>

namespace bedrock {

static const uint8_t RAKNET_MAGIC[16] = {
    0x00, 0xff, 0xff, 0x00,
    0xfe, 0xfe, 0xfe, 0xfe,
    0xfd, 0xfd, 0xfd, 0xfd,
    0x12, 0x34, 0x56, 0x78
};

static constexpr size_t UNCONNECTED_PONG_HEADER_SIZE = 1 + 8 + 8 + 16;
static constexpr size_t MAX_OFFLINE_DATA_LENGTH = 400;

static void writeU64BE(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
    }
}

static uint64_t readU64BE(const std::vector<uint8_t>& data, size_t& off) {
    if (off + 8 > data.size()) {
        throw std::runtime_error("readU64BE out of range");
    }

    uint64_t v = 0;

    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<uint64_t>(data[off + i]);
    }

    off += 8;
    return v;
}

static bool checkMagic(const std::vector<uint8_t>& data, size_t off) {
    if (off + 16 > data.size()) {
        return false;
    }

    for (size_t i = 0; i < 16; ++i) {
        if (data[off + i] != RAKNET_MAGIC[i]) {
            return false;
        }
    }

    return true;
}

static void appendUtf8Replacement(std::string& out) {
    out.append("\xef\xbf\xbd", 3);
}

// Node's Buffer#toString() defaults to UTF-8. V8 replaces each malformed
// subsequence with U+FFFD, while preserving valid prefix bytes of a truncated
// sequence as a single malformed subsequence. Keep that observable conversion
// before ServerAdvertisement::fromString sees the native pong payload.
static std::string nodeBufferToUtf8(const uint8_t* bytes, size_t length) {
    std::string out;
    out.reserve(length);

    size_t offset = 0;
    while (offset < length) {
        const uint8_t lead = bytes[offset];
        if (lead <= 0x7f) {
            out.push_back(static_cast<char>(lead));
            ++offset;
            continue;
        }

        size_t sequenceLength = 0;
        uint8_t secondMinimum = 0x80;
        uint8_t secondMaximum = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf) {
            sequenceLength = 2;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            sequenceLength = 3;
            if (lead == 0xe0) secondMinimum = 0xa0;
            if (lead == 0xed) secondMaximum = 0x9f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            sequenceLength = 4;
            if (lead == 0xf0) secondMinimum = 0x90;
            if (lead == 0xf4) secondMaximum = 0x8f;
        } else {
            appendUtf8Replacement(out);
            ++offset;
            continue;
        }

        if (offset + 1 >= length) {
            appendUtf8Replacement(out);
            break;
        }

        const uint8_t second = bytes[offset + 1];
        if (second < secondMinimum || second > secondMaximum) {
            appendUtf8Replacement(out);
            ++offset;
            continue;
        }

        size_t continuation = 2;
        while (continuation < sequenceLength &&
               offset + continuation < length &&
               bytes[offset + continuation] >= 0x80 &&
               bytes[offset + continuation] <= 0xbf) {
            ++continuation;
        }

        if (continuation != sequenceLength) {
            appendUtf8Replacement(out);
            if (offset + continuation >= length) {
                break;
            }
            offset += continuation;
            continue;
        }

        out.append(
            reinterpret_cast<const char*>(bytes + offset),
            sequenceLength
        );
        offset += sequenceLength;
    }

    return out;
}

static std::vector<std::string> splitSemi(const std::string& s) {
    std::vector<std::string> parts;
    std::string current;

    for (char c : s) {
        if (c == ';') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }

    parts.push_back(current);
    return parts;
}

static int toIntSafe(const std::string& s, int fallback = -1) {
    try {
        if (s.empty()) return fallback;
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
}

static uint64_t makeClientGuid() {
    auto now = std::chrono::high_resolution_clock::now()
        .time_since_epoch()
        .count();

    std::random_device rd;

    uint64_t a = static_cast<uint64_t>(now);
    uint64_t b = static_cast<uint64_t>(rd());

    return (a << 16) ^ b ^ 0xBADC0FFEEULL;
}

static int64_t nowMillis() {
    using namespace std::chrono;

    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

std::vector<uint8_t> RakNetPinger::buildUnconnectedPing() {
    std::vector<uint8_t> out;

    // RakNet ID_UNCONNECTED_PING
    out.push_back(0x01);

    // ping time, big endian
    writeU64BE(out, static_cast<uint64_t>(nowMillis()));

    // RakNet offline message magic
    out.insert(out.end(), std::begin(RAKNET_MAGIC), std::end(RAKNET_MAGIC));

    // client guid, big endian
    writeU64BE(out, makeClientGuid());

    return out;
}

RakNetPongInfo RakNetPinger::parseUnconnectedPong(
    const std::string& host,
    uint16_t port,
    const std::vector<uint8_t>& data
) {
    RakNetPongInfo info;
    info.host = host;
    info.port = port;

    try {
        // raknet-native only forwards a pong whose RakNet header is intact and
        // whose complete offline payload is shorter than 400 bytes. Its JS
        // wrapper then ignores a pong with no payload at all.
        if (data.size() < UNCONNECTED_PONG_HEADER_SIZE) {
            return info;
        }

        size_t off = 0;

        uint8_t packetId = data[off++];

        if (packetId != 0x1c) {
            return info;
        }

        info.pingTime = static_cast<int64_t>(readU64BE(data, off));
        info.serverGuid = readU64BE(data, off);

        if (!checkMagic(data, off)) {
            return info;
        }

        off += 16;
        const size_t payloadLength = data.size() - off;
        if (payloadLength == 0 || payloadLength >= MAX_OFFLINE_DATA_LENGTH) {
            return info;
        }

        // raknet-native/lib/RakNet.js slices only the synthetic id + TimeMS
        // bytes. The complete offline payload, including Bedrock's UInt16BE
        // advertisement length prefix, is therefore visible to Buffer#toString.
        info.rawMotd = nodeBufferToUtf8(data.data() + off, payloadLength);
        info.advertisement = fromServerName(info.rawMotd);

        auto parts = splitSemi(info.rawMotd);

        if (parts.size() > 0) info.edition = parts[0];
        if (parts.size() > 1) info.motd = parts[1];
        if (parts.size() > 2) info.protocolVersion = toIntSafe(parts[2]);
        if (parts.size() > 3) info.gameVersion = parts[3];
        if (parts.size() > 4) info.onlinePlayers = toIntSafe(parts[4]);
        if (parts.size() > 5) info.maxPlayers = toIntSafe(parts[5]);
        if (parts.size() > 6) info.serverId = parts[6];
        if (parts.size() > 7) info.subMotd = parts[7];
        if (parts.size() > 8) info.gameMode = parts[8];
        if (parts.size() > 9) info.gameModeNumeric = toIntSafe(parts[9]);
        if (parts.size() > 10) info.ipv4Port = toIntSafe(parts[10]);
        if (parts.size() > 11) info.ipv6Port = toIntSafe(parts[11]);

        info.ok = true;
        return info;
    } catch (const std::exception& e) {
        info.error = e.what();
        return info;
    }
}

RakNetPongInfo RakNetPinger::ping(
    const std::string& host,
    uint16_t port,
    int timeoutMs
) {
    RakNetPongInfo result;
    result.host = host;
    result.port = port;

    int sock = -1;
    struct addrinfo hints {};
    struct addrinfo* res = nullptr;

    try {
        // The selected raknet-native Client starts an AF_INET socket and first
        // resolves every host as IPv4. Even dual-stack hostnames therefore use
        // their first IPv4 result. Its SystemAddress helper maps ::1 to the IPv4
        // loopback as a special case.
        const std::string ipv4Host = host == "::1" ? "127.0.0.1" : host;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        std::string portStr = std::to_string(port);

        int gai = getaddrinfo(ipv4Host.c_str(), portStr.c_str(), &hints, &res);
        bool ipv6Only = false;

        if (gai != 0) {
            struct addrinfo ipv6Hints {};
            ipv6Hints.ai_family = AF_INET6;
            ipv6Hints.ai_socktype = SOCK_DGRAM;
            ipv6Hints.ai_protocol = IPPROTO_UDP;
            struct addrinfo* ipv6Result = nullptr;
            const int ipv6Gai = getaddrinfo(
                host.c_str(),
                portStr.c_str(),
                &ipv6Hints,
                &ipv6Result
            );
            if (ipv6Gai != 0) {
                result.error = "Invalid connection address " + host + "/" +
                    std::to_string(port);
                return result;
            }
            freeaddrinfo(ipv6Result);
            ipv6Only = true;
        }

        struct addrinfo* chosen = res;

        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (sock < 0) {
            result.error = "socket() failed";
            if (res) freeaddrinfo(res);
            return result;
        }

        const int broadcast = 1;
        (void) setsockopt(
            sock,
            SOL_SOCKET,
            SO_BROADCAST,
            &broadcast,
            sizeof(broadcast)
        );

        auto packet = buildUnconnectedPing();

        if (!ipv6Only && chosen) {
            // RakPeer::Ping returns false on a send/address failure, but the
            // native binding ignores that return value and still waits for its
            // ordinary timeout.
            (void) sendto(
                sock,
                packet.data(),
                packet.size(),
                0,
                chosen->ai_addr,
                chosen->ai_addrlen
            );
        }

        if (res) {
            freeaddrinfo(res);
            res = nullptr;
        }

        const auto timeout = std::chrono::milliseconds(std::max(timeoutMs, 0));
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                result.timedOut = true;
                result.error = "Ping timed out";
                close(sock);
                return result;
            }

            const auto remaining = std::chrono::duration_cast<
                std::chrono::microseconds
            >(deadline - now);
            struct timeval tv {};
            tv.tv_sec = static_cast<decltype(tv.tv_sec)>(
                remaining.count() / 1'000'000
            );
            tv.tv_usec = static_cast<decltype(tv.tv_usec)>(
                remaining.count() % 1'000'000
            );

            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);
            const int ready = select(sock + 1, &readfds, nullptr, nullptr, &tv);
            if (ready < 0) {
                if (errno == EINTR) continue;
                result.error = "select() failed";
                close(sock);
                return result;
            }
            if (ready == 0) {
                result.timedOut = true;
                result.error = "Ping timed out";
                close(sock);
                return result;
            }

            std::vector<uint8_t> buf(4096);
            const ssize_t received = recvfrom(
                sock,
                buf.data(),
                buf.size(),
                0,
                nullptr,
                nullptr
            );
            if (received <= 0) {
                if (received < 0 && errno == EINTR) continue;
                result.error = "recvfrom() failed";
                close(sock);
                return result;
            }

            buf.resize(static_cast<size_t>(received));
            auto parsed = parseUnconnectedPong(host, port, buf);
            if (!parsed.ok) {
                // RakPeer does not surface malformed, unrelated, empty, or
                // oversized pong packets to the JavaScript pong handler.
                continue;
            }

            close(sock);
            return parsed;
        }
    } catch (const std::exception& e) {
        if (sock >= 0) {
            close(sock);
        }

        if (res) {
            freeaddrinfo(res);
        }

        result.error = e.what();
        return result;
    }
}

} // namespace bedrock
