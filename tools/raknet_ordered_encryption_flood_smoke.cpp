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
constexpr std::size_t clientPacketPayloadSize = 95;
constexpr std::size_t clientSplitPayloadSize = 4097;
constexpr std::size_t clientPacketCount = 4096;

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

    auto serverEncryptStream = bedrock::BedrockEncryption::createCipherStream(
        protocolVersion,
        key,
        iv,
        bedrock::BedrockCipherMode::Encrypt
    );
    auto clientDecryptStream = bedrock::BedrockEncryption::createCipherStream(
        protocolVersion,
        key,
        iv,
        bedrock::BedrockCipherMode::Decrypt
    );
    auto clientEncryptStream = bedrock::BedrockEncryption::createCipherStream(
        protocolVersion,
        key,
        iv,
        bedrock::BedrockCipherMode::Encrypt
    );
    auto serverDecryptStream = bedrock::BedrockEncryption::createCipherStream(
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
    std::mutex serverReceiveMutex;
    std::condition_variable serverReceiveCv;
    std::size_t serverReceived = 0;
    uint64_t serverReceiveCounter = 0;
    std::string serverFailure;
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
    server.onEncapsulated([
        &
    ](
        const bedrock::RakNetServerPeer&,
        const std::vector<uint8_t>& payload
    ) {
        std::lock_guard<std::mutex> lock(serverReceiveMutex);
        if (!serverFailure.empty()) return;
        try {
            if (payload.empty() || payload.front() != 0xfe) {
                serverFailure = "server received payload without MCPE 0xfe marker";
            } else if (serverReceived >= clientPacketCount) {
                serverFailure = "server received duplicate payload after ordered stream";
            } else {
                std::vector<uint8_t> encryptedOnly(
                    payload.begin() + 1,
                    payload.end()
                );
                auto verification =
                    bedrock::BedrockEncryption::decryptAndVerify(
                        *serverDecryptStream,
                        encryptedOnly,
                        serverReceiveCounter,
                        key
                    );
                if (!verification) {
                    serverFailure = "client payload produced no plaintext";
                } else if (!verification->matches()) {
                    serverFailure = verification->mismatchMessage();
                } else {
                    const auto expectedSize = serverReceived % 127 == 0
                        ? clientSplitPayloadSize
                        : clientPacketPayloadSize;
                    const auto expected = makePayload(serverReceived, expectedSize);
                    if (verification->packetPlaintext != expected) {
                        serverFailure =
                            "client payload order or bytes changed at sequence " +
                            std::to_string(serverReceived);
                    } else {
                        ++serverReceived;
                    }
                }
            }
        } catch (const std::exception& error) {
            serverFailure = error.what();
        }
        serverReceiveCv.notify_all();
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
    std::size_t clientReceived = 0;
    uint64_t clientReceiveCounter = 0;
    std::string clientFailure;
    const std::size_t expectedCount = mapPacketCount + 1;

    client.onEncapsulated([&](const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lock(receiveMutex);
        if (!clientFailure.empty()) return;
        try {
            if (payload.empty() || payload.front() != 0xfe) {
                clientFailure = "client received payload without MCPE 0xfe marker";
            } else if (clientReceived >= expectedCount) {
                clientFailure = "client received duplicate payload after ordered stream";
            } else {
                std::vector<uint8_t> encryptedOnly(
                    payload.begin() + 1,
                    payload.end()
                );
                auto verification =
                    bedrock::BedrockEncryption::decryptAndVerify(
                        *clientDecryptStream,
                        encryptedOnly,
                        clientReceiveCounter,
                        key
                    );
                if (!verification) {
                    clientFailure = "server payload produced no plaintext";
                } else if (!verification->matches()) {
                    clientFailure = verification->mismatchMessage();
                } else {
                    const auto expectedSize = clientReceived < mapPacketCount
                        ? mapPayloadSize
                        : std::size_t {37};
                    const auto expected = makePayload(clientReceived, expectedSize);
                    if (verification->packetPlaintext != expected) {
                        clientFailure =
                            "server payload order or bytes changed at sequence " +
                            std::to_string(clientReceived);
                    } else {
                        ++clientReceived;
                    }
                }
            }
        } catch (const std::exception& error) {
            clientFailure = error.what();
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

    // Exercise both directions concurrently. The server-to-client stream
    // mirrors live 128x128 map traffic; the client-to-server stream is mostly
    // 104-byte encrypted wire payloads, matching the Android checksum failure,
    // with periodic split packets mixed into the same ordered channel.
    std::thread clientSender([&]() {
        for (std::size_t sequence = 0;
             sequence < clientPacketCount;
             ++sequence) {
            const auto size = sequence % 127 == 0
                ? clientSplitPayloadSize
                : clientPacketPayloadSize;
            client.sendReliable(encryptPayload(
                *clientEncryptStream,
                makePayload(sequence, size),
                sequence,
                key
            ));
        }
    });
    for (std::size_t sequence = 0; sequence < mapPacketCount; ++sequence) {
        server.sendReliable(
            peer,
            encryptPayload(
                *serverEncryptStream,
                makePayload(sequence, mapPayloadSize),
                sequence,
                key
            )
        );
    }
    server.sendReliable(
        peer,
        encryptPayload(
            *serverEncryptStream,
            makePayload(mapPacketCount, 37),
            mapPacketCount,
            key
        )
    );
    clientSender.join();

    {
        std::unique_lock<std::mutex> lock(receiveMutex);
        (void) receiveCv.wait_for(
            lock,
            std::chrono::seconds(60),
            [&]() {
                return !clientFailure.empty() ||
                    clientReceived == expectedCount;
            }
        );
        if (clientFailure.empty() && clientReceived != expectedCount) {
            clientFailure = "timed out after " +
                std::to_string(clientReceived) +
                "/" + std::to_string(expectedCount) + " ordered payloads";
        }
    }
    {
        std::unique_lock<std::mutex> lock(serverReceiveMutex);
        (void) serverReceiveCv.wait_for(
            lock,
            std::chrono::seconds(60),
            [&]() {
                return !serverFailure.empty() ||
                    serverReceived == clientPacketCount;
            }
        );
        if (serverFailure.empty() && serverReceived != clientPacketCount) {
            serverFailure = "timed out after " +
                std::to_string(serverReceived) + "/" +
                std::to_string(clientPacketCount) +
                " client ordered payloads";
        }
    }

    const bool sessionAlive = client.connected() &&
        prematureServerCloses.load() == 0;
    client.close();
    server.close();

    if (!clientFailure.empty() || !serverFailure.empty()) {
        std::cerr << "[RAKNET-ORDERED-ENCRYPTION-FLOOD] "
                  << (!clientFailure.empty() ? clientFailure : serverFailure)
                  << "\n";
        return 1;
    }
    if (!sessionAlive) {
        std::cerr << "[RAKNET-ORDERED-ENCRYPTION-FLOOD] session closed "
                     "during map-sized traffic\n";
        return 1;
    }
    if (clientReceiveCounter != expectedCount ||
        serverReceiveCounter != clientPacketCount) {
        std::cerr << "[RAKNET-ORDERED-ENCRYPTION-FLOOD] checksum counters "
                  << clientReceiveCounter << "/" << serverReceiveCounter
                  << " != " << expectedCount << "/" << clientPacketCount
                  << "\n";
        return 1;
    }

    std::cout << "[RAKNET-ORDERED-ENCRYPTION-FLOOD] ok: "
              << mapPacketCount << " x " << mapPayloadSize
              << " byte server payloads and " << clientPacketCount
              << " ordered client payloads\n";
    return 0;
}
