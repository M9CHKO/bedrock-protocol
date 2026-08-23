#include <bedrock/bedrock.hpp>

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr uint8_t kMagic[16] = {
    0x00, 0xff, 0xff, 0x00,
    0xfe, 0xfe, 0xfe, 0xfe,
    0xfd, 0xfd, 0xfd, 0xfd,
    0x12, 0x34, 0x56, 0x78
};

void writeU64BE(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
    }
}

std::vector<uint8_t> makePong(
    const std::vector<uint8_t>& request,
    const std::vector<uint8_t>& offlinePayload
) {
    std::vector<uint8_t> out {0x1c};
    if (request.size() >= 9) {
        out.insert(out.end(), request.begin() + 1, request.begin() + 9);
    } else {
        out.resize(9, 0x00);
    }
    writeU64BE(out, 0x0102030405060708ULL);
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    out.insert(out.end(), offlinePayload.begin(), offlinePayload.end());
    return out;
}

using ReplyBuilder = std::function<std::vector<std::vector<uint8_t>>(
    const std::vector<uint8_t>&
)>;

class UdpResponder {
public:
    explicit UdpResponder(ReplyBuilder replies) {
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ < 0) {
            throw std::runtime_error("responder socket() failed");
        }

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(
                socket_,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)
            ) != 0) {
            close(socket_);
            socket_ = -1;
            throw std::runtime_error("responder bind() failed");
        }

        socklen_t addressLength = sizeof(address);
        if (getsockname(
                socket_,
                reinterpret_cast<sockaddr*>(&address),
                &addressLength
            ) != 0) {
            close(socket_);
            socket_ = -1;
            throw std::runtime_error("responder getsockname() failed");
        }
        port_ = ntohs(address.sin_port);

        worker_ = std::thread([
            this,
            replies = std::move(replies)
        ]() mutable {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(socket_, &readSet);
            timeval timeout {};
            timeout.tv_sec = 3;
            const int ready = select(
                socket_ + 1,
                &readSet,
                nullptr,
                nullptr,
                &timeout
            );
            if (ready <= 0) {
                error_ = "responder did not receive ping";
                return;
            }

            std::vector<uint8_t> request(2048);
            sockaddr_storage source {};
            socklen_t sourceLength = sizeof(source);
            const auto received = recvfrom(
                socket_,
                request.data(),
                request.size(),
                0,
                reinterpret_cast<sockaddr*>(&source),
                &sourceLength
            );
            if (received <= 0) {
                error_ = "responder recvfrom() failed";
                return;
            }
            request.resize(static_cast<std::size_t>(received));
            receivedPing_ = true;

            for (const auto& reply : replies(request)) {
                const auto sent = sendto(
                    socket_,
                    reply.data(),
                    reply.size(),
                    0,
                    reinterpret_cast<const sockaddr*>(&source),
                    sourceLength
                );
                if (sent < 0 || static_cast<std::size_t>(sent) != reply.size()) {
                    error_ = "responder sendto() failed";
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }

    UdpResponder(const UdpResponder&) = delete;
    UdpResponder& operator=(const UdpResponder&) = delete;

    ~UdpResponder() {
        finish();
        if (socket_ >= 0) close(socket_);
    }

    uint16_t port() const noexcept { return port_; }

    void finish() {
        if (worker_.joinable()) worker_.join();
    }

    bool receivedPing() const noexcept { return receivedPing_.load(); }
    const std::string& error() const noexcept { return error_; }

private:
    int socket_ = -1;
    uint16_t port_ = 0;
    std::thread worker_;
    std::atomic<bool> receivedPing_ {false};
    std::string error_;
};

bool check(bool condition, const std::string& message) {
    if (condition) return true;
    std::cerr << "[PING-API-SMOKE] " << message << "\n";
    return false;
}

bool checkResponder(const UdpResponder& responder) {
    return check(responder.receivedPing(), "responder did not observe ping") &&
        check(responder.error().empty(), responder.error());
}

std::vector<uint8_t> prefixedAdvertisement(const std::string& advertisement) {
    const auto length = static_cast<uint16_t>(advertisement.size());
    std::vector<uint8_t> out {
        static_cast<uint8_t>((length >> 8u) & 0xffu),
        static_cast<uint8_t>(length & 0xffu)
    };
    out.insert(out.end(), advertisement.begin(), advertisement.end());
    return out;
}

} // namespace

int main() {
    bool ok = true;

    try {
        const std::string advertisement =
            "MCPE;M;924;1.26.0;2;5;42;L;Creative;1;19132;19133;0;";
        const auto payload = prefixedAdvertisement(advertisement);
        UdpResponder responder([payload](const std::vector<uint8_t>& request) {
            auto badMagic = makePong(request, {'b'});
            badMagic[17] ^= 0xffu;
            return std::vector<std::vector<uint8_t>> {
                {0x00},
                std::move(badMagic),
                makePong(request, {}),
                makePong(request, std::vector<uint8_t>(400, 'z')),
                makePong(request, payload)
            };
        });

        auto parsed = bedrock::ping({
            .host = "127.0.0.1",
            .port = responder.port()
        });
        responder.finish();
        ok &= checkResponder(responder);

        std::string expectedHeader;
        expectedHeader.push_back(static_cast<char>(payload[0]));
        expectedHeader.push_back(static_cast<char>(payload[1]));
        expectedHeader += "MCPE";
        const auto* header = parsed.header.stringValue();
        ok &= check(
            header && *header == expectedHeader,
            "Bedrock UInt16 prefix was not retained in ServerAdvertisement.header"
        );
        ok &= check(parsed.motd == "M", "prefixed advertisement motd mismatch");
        ok &= check(parsed.protocol == "924", "protocol must remain a string");
        ok &= check(parsed.playersOnline == 2, "playersOnline parse mismatch");
        ok &= check(parsed.portV4 == 19132, "portV4 parse mismatch");
    } catch (const std::exception& error) {
        std::cerr << "[PING-API-SMOKE] standard framing threw: "
                  << error.what() << "\n";
        ok = false;
    }

    try {
        UdpResponder responder([](const std::vector<uint8_t>& request) {
            return std::vector<std::vector<uint8_t>> {
                makePong(request, {'x'})
            };
        });
        const auto pong = bedrock::RakNetPinger::ping(
            "::1",
            responder.port(),
            400
        );
        responder.finish();
        ok &= checkResponder(responder);
        ok &= check(pong.ok, "one-byte payload was not accepted");
        ok &= check(pong.rawMotd == "x", "one-byte payload changed");
        ok &= check(
            pong.advertisement && pong.advertisement->header == "x",
            "one-byte payload header mismatch"
        );
    } catch (const std::exception& error) {
        std::cerr << "[PING-API-SMOKE] one-byte/IPv4 case threw: "
                  << error.what() << "\n";
        ok = false;
    }

    try {
        const std::vector<uint8_t> payload(399, 'x');
        UdpResponder responder([payload](const std::vector<uint8_t>& request) {
            return std::vector<std::vector<uint8_t>> {
                makePong(request, payload)
            };
        });
        const auto pong = bedrock::RakNetPinger::ping(
            "127.0.0.1",
            responder.port(),
            400
        );
        responder.finish();
        ok &= checkResponder(responder);
        ok &= check(pong.ok, "399-byte payload was not accepted");
        ok &= check(pong.rawMotd.size() == 399, "399-byte payload size changed");
    } catch (const std::exception& error) {
        std::cerr << "[PING-API-SMOKE] 399-byte case threw: "
                  << error.what() << "\n";
        ok = false;
    }

    try {
        const std::vector<uint8_t> payload {
            0xff, 0xc0, 0xaf, 0xe2, 0x82, ';', 'y'
        };
        UdpResponder responder([payload](const std::vector<uint8_t>& request) {
            return std::vector<std::vector<uint8_t>> {
                makePong(request, payload)
            };
        });
        const auto pong = bedrock::RakNetPinger::ping(
            "127.0.0.1",
            responder.port(),
            400
        );
        responder.finish();
        ok &= checkResponder(responder);
        const std::string replacement = "\xef\xbf\xbd";
        const std::string expectedHeader = replacement + replacement +
            replacement + replacement;
        const std::string expected = expectedHeader + ";y";
        ok &= check(pong.ok, "invalid UTF-8 payload was not accepted");
        ok &= check(
            pong.rawMotd == expected,
            "Buffer.toString UTF-8 replacement mismatch"
        );
        ok &= check(
            pong.advertisement && pong.advertisement->header ==
                std::string_view(expectedHeader),
            "replacement-decoded header mismatch"
        );
    } catch (const std::exception& error) {
        std::cerr << "[PING-API-SMOKE] UTF-8 case threw: "
                  << error.what() << "\n";
        ok = false;
    }

    try {
        UdpResponder responder([](const std::vector<uint8_t>& request) {
            return std::vector<std::vector<uint8_t>> {
                makePong(request, std::vector<uint8_t>(400, 'z'))
            };
        });
        const auto started = std::chrono::steady_clock::now();
        bool exactTimeout = false;
        try {
            (void) bedrock::ping({
                .host = "127.0.0.1",
                .port = responder.port()
            });
        } catch (const bedrock::RakTimeout& error) {
            exactTimeout = std::string(error.what()) == "Ping timed out";
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started
        ).count();
        responder.finish();
        ok &= checkResponder(responder);
        ok &= check(exactTimeout, "400-byte payload did not yield exact RakTimeout");
        ok &= check(
            elapsed >= 800 && elapsed < 3000,
            "top-level timeout was not fixed near 1000ms"
        );
    } catch (const std::exception& error) {
        std::cerr << "[PING-API-SMOKE] timeout case threw: "
                  << error.what() << "\n";
        ok = false;
    }

    try {
        (void) bedrock::ping({.host = "127.0.0.1"});
        std::cerr << "[PING-API-SMOKE] omitted port was accepted\n";
        ok = false;
    } catch (const std::invalid_argument& error) {
        ok &= check(
            std::string(error.what()) == "Wrong arguments",
            "omitted-port error mismatch"
        );
    } catch (const std::exception& error) {
        std::cerr << "[PING-API-SMOKE] omitted port threw wrong type: "
                  << error.what() << "\n";
        ok = false;
    }

    if (!ok) return 1;
    std::cout << "[PING-API-SMOKE] ok\n";
    return 0;
}
