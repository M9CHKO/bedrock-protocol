#include <bedrock/BedrockEncryption.hpp>
#include <bedrock/client/RakNetClient.hpp>
#include <bedrock/server/RakNetServer.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr uint32_t protocolVersion = 827;
constexpr std::size_t mapPayloadSize = 81937;
constexpr std::size_t mapPacketCount = 64;

std::vector<uint8_t> makePayload(uint64_t sequence, std::size_t size) {
    std::vector<uint8_t> payload(size);
    payload[0] = 0x4d;
    for (std::size_t i = 0; i < sizeof(sequence); ++i) {
        payload[1 + i] = static_cast<uint8_t>(sequence >> (i * 8u));
    }
    for (std::size_t i = 1 + sizeof(sequence); i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(
            (i * 131u + static_cast<std::size_t>(sequence) * 17u) & 0xffu
        );
    }
    return payload;
}

std::vector<uint8_t> encryptPayload(
    bedrock::BedrockCipherStream& stream,
    const std::vector<uint8_t>& plaintext,
    uint64_t sequence,
    const std::vector<uint8_t>& key
) {
    auto aesPlaintext = bedrock::BedrockEncryption::makeAesPlaintext(
        plaintext,
        sequence,
        key
    );
    auto encryptedOnly = stream.process(aesPlaintext);
    std::vector<uint8_t> payload;
    payload.reserve(1 + encryptedOnly.size());
    payload.push_back(0xfe);
    payload.insert(
        payload.end(),
        encryptedOnly.begin(),
        encryptedOnly.end()
    );
    return payload;
}

} // namespace

int main() {
    std::vector<uint8_t> key(32);
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(0x31u + i * 7u);
    }
    const std::vector<uint8_t> iv(key.begin(), key.begin() + 16);

    auto encryptStream = bedrock::BedrockEncryption::createCipherStream(
        protocolVersion,
        key,
        iv,
        bedrock::BedrockCipherMode::Encrypt
    );
    auto decryptStream = bedrock::BedrockEncryption::createCipherStream(
        protocolVersion,
        key,
        iv,
        bedrock::BedrockCipherMode::Decrypt
    );

    bedrock::RakNetServerOptions serverOptions;
    serverOptions.host = "127.0.0.1";
    serverOptions.port = 0;
    serverOptions.maxPlayers = 1;
    serverOptions.protocolVersion = 11;
    bedrock::RakNetServer server(serverOptions);

    std::mutex peerMutex;
    std::condition_variable peerCv;
    std::optional<bedrock::RakNetServerPeer> connectedPeer;
    std::atomic<int> prematureServerCloses {0};
    server.onOpenConnection([&](const bedrock::RakNetServerPeer& peer) {
        {
            std::lock_guard<std::mutex> lock(peerMutex);
            connectedPeer = peer;
        }
        peerCv.notify_all();
    });
    server.onCloseConnection([&](const bedrock::RakNetServerPeer&) {
        ++prematureServerCloses;
    });
    server.listen();

    bedrock::RakNetClientOptions clientOptions;
    clientOptions.host = "127.0.0.1";
    clientOptions.port = server.boundPort();
    clientOptions.mtu = 1400;
    clientOptions.protocolVersion = 11;
    clientOptions.timeoutMs = 3000;
    bedrock::RakNetClient client(clientOptions);

    std::mutex receiveMutex;
    std::condition_variable receiveCv;
    std::size_t received = 0;
    uint64_t receiveCounter = 0;
    std::string failure;
    const std::size_t expectedCount = mapPacketCount + 1;

    client.onEncapsulated([&](const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lock(receiveMutex);
        if (!failure.empty()) return;
        try {
            if (payload.empty() || payload.front() != 0xfe) {
                failure = "received payload without MCPE 0xfe marker";
            } else if (received >= expectedCount) {
                failure = "received duplicate payload after ordered stream";
            } else {
                std::vector<uint8_t> encryptedOnly(
                    payload.begin() + 1,
                    payload.end()
                );
                auto verification =
                    bedrock::BedrockEncryption::decryptAndVerify(
                        *decryptStream,
                        encryptedOnly,
                        receiveCounter,
                        key
                    );
                if (!verification) {
                    failure = "encrypted payload produced no plaintext";
                } else if (!verification->matches()) {
                    failure = verification->mismatchMessage();
                } else {
                    const auto expectedSize = received < mapPacketCount
                        ? mapPayloadSize
                        : std::size_t {37};
                    const auto expected = makePayload(received, expectedSize);
                    if (verification->packetPlaintext != expected) {
                        failure = "payload order or bytes changed at sequence " +
                            std::to_string(received);
                    } else {
                        ++received;
                    }
                }
            }
        } catch (const std::exception& error) {
            failure = error.what();
        }
        receiveCv.notify_all();
    });

    if (!client.connect()) {
        std::cerr << "[RAKNET-ORDERED-ENCRYPTION-FLOOD] connect failed: "
                  << client.error() << "\n";
        server.close();
        return 1;
    }

    bedrock::RakNetServerPeer peer;
    {
        std::unique_lock<std::mutex> lock(peerMutex);
        if (!peerCv.wait_for(
                lock,
                std::chrono::seconds(3),
                [&]() { return connectedPeer.has_value(); }
            )) {
            std::cerr << "[RAKNET-ORDERED-ENCRYPTION-FLOOD] server did not "
                         "observe connected peer\n";
            client.close();
            server.close();
            return 1;
        }
        peer = *connectedPeer;
    }

    // This is the failure mode from live 128x128 map traffic: every payload
    // is large enough to require RakNet splitting, while Bedrock's cipher and
    // checksum counters require delivery exactly once and in order.
    for (std::size_t sequence = 0; sequence < mapPacketCount; ++sequence) {
        server.sendReliable(
            peer,
            encryptPayload(
                *encryptStream,
                makePayload(sequence, mapPayloadSize),
                sequence,
                key
            )
        );
    }
    server.sendReliable(
        peer,
        encryptPayload(
            *encryptStream,
            makePayload(mapPacketCount, 37),
            mapPacketCount,
            key
        )
    );

    {
        std::unique_lock<std::mutex> lock(receiveMutex);
        (void) receiveCv.wait_for(
            lock,
            std::chrono::seconds(60),
            [&]() { return !failure.empty() || received == expectedCount; }
        );
        if (failure.empty() && received != expectedCount) {
            failure = "timed out after " + std::to_string(received) +
                "/" + std::to_string(expectedCount) + " ordered payloads";
        }
    }

    const bool sessionAlive = client.connected() &&
        prematureServerCloses.load() == 0;
    client.close();
    server.close();

    if (!failure.empty()) {
        std::cerr << "[RAKNET-ORDERED-ENCRYPTION-FLOOD] " << failure << "\n";
        return 1;
    }
    if (!sessionAlive) {
        std::cerr << "[RAKNET-ORDERED-ENCRYPTION-FLOOD] session closed "
                     "during map-sized traffic\n";
        return 1;
    }
    if (receiveCounter != expectedCount) {
        std::cerr << "[RAKNET-ORDERED-ENCRYPTION-FLOOD] checksum counter "
                  << receiveCounter << " != " << expectedCount << "\n";
        return 1;
    }

    std::cout << "[RAKNET-ORDERED-ENCRYPTION-FLOOD] ok: "
              << mapPacketCount << " x " << mapPayloadSize
              << " byte encrypted payloads plus ordered tail\n";
    return 0;
}
