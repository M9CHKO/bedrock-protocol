#include <bedrock/RakNetConnect.hpp>
#include <bedrock/RakNetPing.hpp>
#include <bedrock/BedrockEncryption.hpp>
#include <bedrock/BedrockKeyExchange.hpp>
#include <bedrock/LoginPacket.hpp>
#include <bedrock/auth/BedrockAuthJwt.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/protocol/VersionedPayloadReader.hpp>
#include <bedrock/server/BedrockServer.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace bedrock {

struct BedrockServerTestAccess {
    static BedrockServerConnection addEncryptedPlayer(
        BedrockServer& server,
        const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& iv,
        uint16_t port = 19190,
        uint64_t clientGuid = 0x1122334455667788ull
    ) {
        BedrockServerConnection connection;
        connection.address = "127.0.0.1";
        connection.port = port;
        connection.clientGuid = clientGuid;
        connection.mtu = 1400;
        connection.peer.address = connection.address;
        connection.peer.port = connection.port;
        connection.peer.clientGuid = connection.clientGuid;
        connection.peer.mtu = connection.mtu;
        connection.playerEvents = std::make_shared<BedrockServerPlayerEventState>();

        const auto mapKey = BedrockServer::connectionKey(connection.peer);
        auto session = std::make_shared<BedrockServer::SessionState>();
        session->encryptionKeys.secretKeyBytes = key;
        session->encryptionKeys.iv16 = iv;
        session->hasEncryptionKeys = true;
        session->encryptionEnabled = true;
        session->compressionReady = true;
        session->encryptStream = BedrockEncryption::createCipherStream(
            server.configuredServerProtocolVersion(),
            key,
            iv,
            BedrockCipherMode::Encrypt
        );
        session->decryptStream = BedrockEncryption::createCipherStream(
            server.configuredServerProtocolVersion(),
            key,
            iv,
            BedrockCipherMode::Decrypt
        );
        {
            std::lock_guard<std::mutex> lock(server.serverStateMutex_);
            server.connections_[mapKey] = connection;
            server.sessions_[mapKey] = std::move(session);
            ++server.clientCount_;
        }
        return connection;
    }

    static BedrockServerConnection addPlainPlayer(
        BedrockServer& server,
        uint16_t port = 19201,
        uint64_t clientGuid = 12,
        bool compressionReady = true
    ) {
        BedrockServerConnection connection;
        connection.address = "127.0.0.1";
        connection.port = port;
        connection.clientGuid = clientGuid;
        connection.mtu = 1400;
        connection.peer.address = connection.address;
        connection.peer.port = connection.port;
        connection.peer.clientGuid = connection.clientGuid;
        connection.peer.mtu = connection.mtu;
        connection.playerEvents = std::make_shared<BedrockServerPlayerEventState>();

        auto session = std::make_shared<BedrockServer::SessionState>();
        session->compressionReady = compressionReady;
        const auto mapKey = BedrockServer::connectionKey(connection.peer);
        {
            std::lock_guard<std::mutex> lock(server.serverStateMutex_);
            server.connections_[mapKey] = connection;
            server.sessions_[mapKey] = std::move(session);
            ++server.clientCount_;
        }
        return connection;
    }

    static bool enableEncryption(
        BedrockServer& server,
        const BedrockServerConnection& connection,
        const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& iv
    ) {
        const auto session = server.sessionSnapshot(connection);
        if (!session) {
            return false;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        session->encryptionKeys.secretKeyBytes = key;
        session->encryptionKeys.iv16 = iv;
        session->hasEncryptionKeys = true;
        session->encryptionEnabled = true;
        session->compressionReady = true;
        session->encryptStream = BedrockEncryption::createCipherStream(
            server.configuredServerProtocolVersion(),
            key,
            iv,
            BedrockCipherMode::Encrypt
        );
        session->decryptStream = BedrockEncryption::createCipherStream(
            server.configuredServerProtocolVersion(),
            key,
            iv,
            BedrockCipherMode::Decrypt
        );
        session->sendCounter = 0;
        session->receiveCounter = 0;
        session->inboundEncryptionFailed = false;
        session->recentEncryptedBatchBytes = 0;
        session->recentEncryptedBatches.clear();
        BedrockServer::clearPendingEncryptedBatches(*session);
        return true;
    }

    static void handle(
        BedrockServer& server,
        const BedrockServerConnection& connection,
        const std::vector<uint8_t>& payload
    ) {
        server.handleEncapsulated(connection.peer, payload);
    }

    static void handleNativeClose(
        BedrockServer& server,
        const BedrockServerConnection& connection,
        uint8_t messageId
    ) {
        server.raknet_.handleNativePacket(connection.peer, {messageId});
    }

    static void emitError(
        BedrockServer& server,
        const BedrockServerConnection& connection,
        const std::string& message
    ) {
        server.emitPlayerError(connection, message);
    }

    static void closePlayer(
        BedrockServer& server,
        const BedrockServerConnection& connection
    ) {
        server.closePlayer(connection);
    }

    static std::pair<uint64_t, uint64_t> counters(
        const BedrockServer& server,
        const BedrockServerConnection& connection
    ) {
        const auto session = server.sessionSnapshot(connection);
        if (!session) {
            return {UINT64_MAX, UINT64_MAX};
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        return {session->sendCounter, session->receiveCounter};
    }

    static bool hasPlayerState(
        const BedrockServer& server,
        const BedrockServerConnection& connection
    ) {
        const auto key = BedrockServer::connectionKey(connection.peer);
        std::lock_guard<std::mutex> lock(server.serverStateMutex_);
        return server.connections_.find(key) != server.connections_.end() ||
            server.sessions_.find(key) != server.sessions_.end();
    }

    static std::size_t scheduledCloses(BedrockServer& server) {
        std::lock_guard<std::mutex> lock(server.playerLifecycleMutex_);
        return server.scheduledPlayerCloses_.size();
    }

    static void forceOutboundQueueFailure(
        BedrockServer& server,
        const BedrockServerConnection& connection,
        const VersionedGamePacket& packet
    ) {
        const auto session = server.sessionSnapshot(connection);
        if (!session) {
            throw std::runtime_error("missing test session");
        }
        {
            std::lock_guard<std::recursive_mutex> outboundLock(
                session->outboundMutex
            );
            std::lock_guard<std::mutex> sessionLock(session->mutex);
            session->hasEncryptionKeys = true;
            session->encryptionEnabled = true;
            session->encryptStream.reset();
            session->queuedPackets.push_back({
                packet,
                VersionedMcpeCompression::Automatic
            });
        }
        server.flushOutboundQueues();
    }

    static void rejectBatchAsDisconnected(
        BedrockServer& server,
        const BedrockServerConnection& connection
    ) {
        RakNetServerSendResult result;
        result.status = RakNetServerSendStatus::NotConnected;
        result.connectionState = 3;
        server.handleRejectedBatchSend(
            connection,
            {0xfe, 0x00},
            0xfe,
            0,
            {},
            "RakNet batch send rejected",
            result
        );
    }

    static std::pair<std::size_t, std::size_t> listenerCounts(
        const BedrockServerConnection& connection
    ) {
        if (!connection.playerEvents) {
            return {0, 0};
        }
        std::lock_guard<std::mutex> lock(connection.playerEvents->mutex_);
        return {
            connection.playerEvents->errorHandlers_.size(),
            connection.playerEvents->closeHandlers_.size()
        };
    }

    static std::exception_ptr rakNetLiveCallbackException() {
        RakNetServer server;
        const RakNetServerPeer peer {"127.0.0.1", 19132, 1, 1400};
        {
            std::lock_guard<std::mutex> lock(server.peersMutex_);
            server.peers_.emplace("127.0.0.1:19132", peer);
        }
        server.onRawPacket([](
            const RakNetServerPeer&,
            const std::vector<uint8_t>&
        ) {
            throw std::runtime_error("live callback boom");
        });

        try {
            server.handleNativePacket(
                peer,
                {0xfe, 0x00, 0x00}
            );
        } catch (...) {
            return std::current_exception();
        }
        return {};
    }

    static bool rakNetInternalMessageIsFiltered() {
        RakNetServer server;
        const RakNetServerPeer peer {"127.0.0.1", 19132, 1, 1400};
        {
            std::lock_guard<std::mutex> lock(server.peersMutex_);
            server.peers_.emplace("127.0.0.1:19132", peer);
        }
        int rawCalls = 0;
        int encapsulatedCalls = 0;
        server.onRawPacket([&](
            const RakNetServerPeer&,
            const std::vector<uint8_t>&
        ) {
            ++rawCalls;
        });
        server.onEncapsulated([&](
            const RakNetServerPeer&,
            const std::vector<uint8_t>&
        ) {
            ++encapsulatedCalls;
        });
        try {
            // RakPeer has already consumed transport frames before this
            // boundary. Internal RakNet notifications remain observable as
            // raw messages but must never surface as application payloads.
            server.handleNativePacket(
                peer,
                {0x00}
            );
        } catch (...) {
            return false;
        }
        return rawCalls == 1 && encapsulatedCalls == 0;
    }
};

} // namespace bedrock

namespace {

constexpr uint8_t RAKNET_MAGIC[16] = {
    0x00, 0xff, 0xff, 0x00,
    0xfe, 0xfe, 0xfe, 0xfe,
    0xfd, 0xfd, 0xfd, 0xfd,
    0x12, 0x34, 0x56, 0x78
};

constexpr uint8_t RAKNET_PROTOCOL = 11;
constexpr uint16_t RAKNET_MTU = 1400;
constexpr uint64_t RAKNET_CLIENT_GUID = 0x0102030405060708ull;

uint64_t nextRakNetClientGuid() {
    static std::atomic<uint64_t> next {RAKNET_CLIENT_GUID};
    return next.fetch_add(1);
}

bedrock::VersionedGamePacket makeSizedTickPacket(
    const bedrock::VersionedMcpeCodec& codec,
    std::size_t fullPacketSize
) {
    if (fullPacketSize == 0) {
        throw std::runtime_error("full packet size must be positive");
    }

    auto packet = codec.packetCodec().makePacketByName(
        "tick_sync",
        std::vector<uint8_t>(fullPacketSize - 1, static_cast<uint8_t>('x'))
    );
    if (packet.fullPacket.size() != fullPacketSize) {
        throw std::runtime_error("tick_sync packet id is no longer one byte");
    }
    return packet;
}

bool checkCompressionGoldens() {
    const auto modern = bedrock::VersionedMcpeCodec::forVersion("1.20.61");
    const auto legacy = bedrock::VersionedMcpeCodec::forVersion("1.20.40");

    // A 510-byte full packet has a two-byte varint prefix, making the Framer
    // input exactly 512 bytes. A 511-byte packet makes it exactly 513 bytes.
    const auto modernAtBoundary = makeSizedTickPacket(modern, 510);
    const auto modernOverBoundary = makeSizedTickPacket(modern, 511);
    const auto legacyAtBoundary = makeSizedTickPacket(legacy, 510);
    const auto legacyOverBoundary = makeSizedTickPacket(legacy, 511);

    const auto modernAtRaw = modern.encodeCompressionPacket(
        {modernAtBoundary},
        bedrock::VersionedMcpeCompression::Uncompressed
    );
    const auto modernAtAuto = modern.encodeCompressionPacket(
        {modernAtBoundary},
        "deflate",
        7,
        512
    );
    if (modernAtAuto != modernAtRaw || modernAtAuto.empty() || modernAtAuto[0] != 0xff) {
        std::cerr << "[SMOKE] 512-byte modern batch was compressed\n";
        return false;
    }

    const std::vector<uint8_t> modernLevel7Golden {
        0x00, 0xfb, 0xcf, 0x2c, 0x5e, 0x31, 0x0a, 0x46, 0x30, 0x00, 0x00
    };
    const auto modernOverAuto = modern.encodeCompressionPacket(
        {modernOverBoundary},
        "deflate",
        7,
        512
    );
    if (modernOverAuto != modernLevel7Golden ||
        modern.decodeCompressionPacket(modernOverAuto).framedBatch.size() != 513) {
        std::cerr << "[SMOKE] 513-byte modern batch did not match Node level-7 deflate\n";
        return false;
    }

    const auto legacyAtFramed = legacy.batchCodec().encodeFramedBatch({legacyAtBoundary});
    const auto legacyAtAuto = legacy.encodeCompressionPacket(
        {legacyAtBoundary},
        "deflate",
        7,
        512
    );
    if (legacyAtAuto != legacyAtFramed) {
        std::cerr << "[SMOKE] 512-byte legacy batch was compressed\n";
        return false;
    }

    const std::vector<uint8_t> legacyLevel7Golden {
        0xfb, 0xcf, 0x2c, 0x5e, 0x31, 0x0a, 0x46, 0x30, 0x00, 0x00
    };
    const auto legacyOverAuto = legacy.encodeCompressionPacket(
        {legacyOverBoundary},
        "deflate",
        7,
        512
    );
    if (legacyOverAuto != legacyLevel7Golden ||
        legacy.decodeCompressionPacket(legacyOverAuto).framedBatch.size() != 513) {
        std::cerr << "[SMOKE] 513-byte legacy batch did not match Node level-7 deflate\n";
        return false;
    }

    const std::vector<uint8_t> modernLevel1Golden {
        0x00, 0xfb, 0xcf, 0x2c, 0x5e, 0x31, 0x0a, 0x46, 0x70, 0x08, 0x00, 0x00
    };
    if (modern.encodeCompressionPacket(
            {modernOverBoundary},
            "deflate",
            1,
            512
        ) != modernLevel1Golden) {
        std::cerr << "[SMOKE] configured deflate level was ignored\n";
        return false;
    }

    if (modern.encodeCompressionPacket(
            {modernOverBoundary},
            "none",
            7,
            512
        ) != modern.encodeCompressionPacket(
            {modernOverBoundary},
            bedrock::VersionedMcpeCompression::Uncompressed
        )) {
        std::cerr << "[SMOKE] none compressor changed the framed batch\n";
        return false;
    }

    if (modern.encodeCompressionPacket(
            {modernAtBoundary},
            "snappy",
            7,
            512
        ) != modernAtRaw) {
        std::cerr << "[SMOKE] snappy ran at or below the threshold\n";
        return false;
    }

    const auto modernSnappy = modern.encodeCompressionPacket(
        {modernOverBoundary},
        "snappy",
        7,
        512
    );
    if (modernSnappy.empty() || modernSnappy[0] != 0x01 ||
        modernSnappy.size() >= modernAtRaw.size() ||
        modern.decodeCompressionPacket(modernSnappy).framedBatch.size() != 513) {
        std::cerr << "[SMOKE] oversized modern snappy batch mismatch\n";
        return false;
    }

    const auto legacySnappy = legacy.encodeCompressionPacket(
        {legacyOverBoundary},
        "snappy",
        7,
        512
    );
    const auto legacySnappyDecoded = legacy.decodeCompressionPacket(
        legacySnappy,
        "snappy"
    );
    if (legacySnappy.empty() || legacySnappy[0] == 0x01 ||
        legacySnappy.size() >= legacyAtFramed.size() ||
        legacySnappyDecoded.compressionHeader != 0x01 ||
        legacySnappyDecoded.framedBatch.size() != 513) {
        std::cerr << "[SMOKE] oversized legacy snappy batch mismatch\n";
        return false;
    }

    // Fixed Node encryption.js fixture: one framed three-byte packet becomes
    // 03 11 22 33, which is raw-deflated even though it is far below the
    // ordinary Framer threshold. Modern versions prepend compressor mode 0
    // before checksumming and encrypting; legacy versions do not.
    bedrock::VersionedGamePacket fixedPacket;
    fixedPacket.fullPacket = {0x11, 0x22, 0x33};
    const std::vector<uint8_t> modernEncryptedCompressionGolden {
        0x00, 0x63, 0x16, 0x54, 0x32, 0x06, 0x00
    };
    const std::vector<uint8_t> legacyEncryptedCompressionGolden {
        0x63, 0x16, 0x54, 0x32, 0x06, 0x00
    };
    const auto modernEncryptedCompression = modern.encodeEncryptedCompressionPacket(
        {fixedPacket},
        7
    );
    const auto legacyEncryptedCompression = legacy.encodeEncryptedCompressionPacket(
        {fixedPacket},
        7
    );
    if (modernEncryptedCompression != modernEncryptedCompressionGolden ||
        legacyEncryptedCompression != legacyEncryptedCompressionGolden) {
        std::cerr << "[SMOKE] encrypted batch did not match Node always-deflate framing\n";
        return false;
    }

    const std::vector<uint8_t> fixedSecret(32, 0x01);
    const std::vector<uint8_t> fixedIv(16, 0x02);
    const std::vector<uint8_t> modernPlaintextGolden {
        0x00, 0x63, 0x16, 0x54, 0x32, 0x06, 0x00,
        0x99, 0x7a, 0xab, 0x4d, 0x81, 0x16, 0x55, 0x27
    };
    const std::vector<uint8_t> legacyPlaintextGolden {
        0x63, 0x16, 0x54, 0x32, 0x06, 0x00,
        0x4f, 0xd7, 0xb7, 0xba, 0xfa, 0xb0, 0x8f, 0x8f
    };
    const auto modernPlaintext = bedrock::BedrockEncryption::makeAesPlaintext(
        modernEncryptedCompression,
        0,
        fixedSecret
    );
    const auto legacyPlaintext = bedrock::BedrockEncryption::makeAesPlaintext(
        legacyEncryptedCompression,
        0,
        fixedSecret
    );
    if (modernPlaintext != modernPlaintextGolden ||
        legacyPlaintext != legacyPlaintextGolden) {
        std::cerr << "[SMOKE] encrypted batch checksum/order did not match Node\n";
        return false;
    }

    bedrock::BedrockAesGcmStream modernCipher(
        fixedSecret,
        fixedIv,
        bedrock::BedrockAesGcmStream::Mode::Encrypt
    );
    bedrock::BedrockAesGcmStream legacyCipher(
        fixedSecret,
        fixedIv,
        bedrock::BedrockAesGcmStream::Mode::Encrypt
    );
    auto modernWire = modernCipher.process(modernPlaintext);
    auto legacyWire = legacyCipher.process(legacyPlaintext);
    modernWire.insert(modernWire.begin(), 0xfe);
    legacyWire.insert(legacyWire.begin(), 0xfe);
    const std::vector<uint8_t> modernWireGolden {
        0xfe, 0x07, 0xb5, 0xdf, 0x1d, 0x78, 0x51, 0xc1,
        0x64, 0xa9, 0x67, 0xf1, 0x49, 0x4a, 0xf2, 0xe5
    };
    const std::vector<uint8_t> legacyWireGolden {
        0xfe, 0x64, 0xc0, 0x9d, 0x7b, 0x4c, 0x57, 0x8e,
        0x2a, 0x64, 0x76, 0x46, 0x78, 0xd3, 0x28
    };
    if (modernWire != modernWireGolden || legacyWire != legacyWireGolden) {
        std::cerr << "[SMOKE] encrypted batch AES/outer-fe wire golden mismatch\n";
        return false;
    }

    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.20.61"
    });
    if (server.options().compressionAlgorithm != "deflate" ||
        server.options().compressionLevel != 7 ||
        server.options().compressionThreshold != 512 ||
        server.options().offline) {
        std::cerr << "[SMOKE] server compression defaults mismatch\n";
        return false;
    }
    server.setCompressor("deflate");
    if (server.options().compressionLevel != 1 ||
        server.options().compressionThreshold != 256) {
        std::cerr << "[SMOKE] setCompressor defaults mismatch\n";
        return false;
    }
    server.setCompressor("none", 9, 123);
    if (server.options().compressionAlgorithm != "none" ||
        server.options().compressionLevel != 0 ||
        server.options().compressionThreshold != 256) {
        std::cerr << "[SMOKE] setCompressor none semantics mismatch\n";
        return false;
    }

    try {
        server.setCompressor("unknown");
        std::cerr << "[SMOKE] unknown compressor did not throw\n";
        return false;
    } catch (const std::exception& error) {
        if (std::string(error.what()) != "Unknown compression algorithm: unknown") {
            std::cerr << "[SMOKE] unknown compressor error mismatch: " << error.what() << "\n";
            return false;
        }
    }

    return true;
}

std::vector<uint8_t> makeEncryptedPlayerWire(
    const std::string& version,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& iv,
    const std::vector<uint8_t>& aesPlaintext
) {
    auto cipher = bedrock::BedrockEncryption::createCipherStream(
        bedrock::protocolVersionFor(version),
        key,
        iv,
        bedrock::BedrockCipherMode::Encrypt
    );
    auto encrypted = cipher->process(aesPlaintext);
    encrypted.insert(encrypted.begin(), 0xfe);
    return encrypted;
}

std::vector<uint8_t> makeValidEncryptedPlayerWire(
    const std::string& version,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& iv,
    const std::vector<uint8_t>& compressionPacket
) {
    return makeEncryptedPlayerWire(
        version,
        key,
        iv,
        bedrock::BedrockEncryption::makeAesPlaintext(compressionPacket, 0, key)
    );
}

std::vector<uint8_t> makeSequentialEncryptedPlayerWire(
    bedrock::BedrockCipherStream& encryptStream,
    const std::vector<uint8_t>& key,
    uint64_t counter,
    const std::vector<uint8_t>& compressionPacket
) {
    auto encrypted = encryptStream.process(
        bedrock::BedrockEncryption::makeAesPlaintext(
            compressionPacket,
            counter,
            key
        )
    );
    encrypted.insert(encrypted.begin(), 0xfe);
    return encrypted;
}

bool checkPlayerEventSnapshots() {
    const std::vector<uint8_t> key(32, 0);
    const std::vector<uint8_t> iv(16, 0);

    {
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv
        );
        std::vector<int> order;
        connection.onError([&](const std::string&) {
            order.push_back(1);
            connection.onError([&](const std::string&) {
                order.push_back(3);
            });
        });
        connection.onError([&](const std::string&) {
            order.push_back(2);
        });

        bedrock::BedrockServerTestAccess::emitError(server, connection, "first");
        if (order != std::vector<int>({1, 2})) {
            std::cerr << "[SMOKE] Player error listener mutation changed current snapshot\n";
            return false;
        }
        bedrock::BedrockServerTestAccess::emitError(server, connection, "second");
        if (order != std::vector<int>({1, 2, 1, 2, 3})) {
            std::cerr << "[SMOKE] deferred Player error listener order mismatch\n";
            return false;
        }
    }

    {
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv, 19191, 2
        );
        std::vector<int> order;
        bool closeSawOldStatus = false;
        connection.onError([](const std::string&) {});
        connection.onClose([&]() {
            order.push_back(1);
            closeSawOldStatus = server.status(connection) ==
                bedrock::BedrockServerClientStatus::Authenticating;
            connection.onClose([&]() {
                order.push_back(4);
            });
            connection.onError([](const std::string&) {});
        });
        connection.onClose([&]() {
            order.push_back(2);
        });
        server.onDisconnect([&](const bedrock::BedrockServerConnection&) {
            order.push_back(3);
            server.onDisconnect([&](const bedrock::BedrockServerConnection&) {
                order.push_back(5);
            });
        });

        bedrock::BedrockServerTestAccess::closePlayer(server, connection);
        if (order != std::vector<int>({1, 2, 3}) || !closeSawOldStatus ||
            bedrock::BedrockServerTestAccess::listenerCounts(connection) !=
                std::pair<std::size_t, std::size_t>{0, 0}) {
            std::cerr << "[SMOKE] Player close snapshot/removeAll ordering mismatch\n";
            return false;
        }
        bedrock::BedrockServerTestAccess::closePlayer(server, connection);
        if (order != std::vector<int>({1, 2, 3})) {
            std::cerr << "[SMOKE] Player close emitted more than once\n";
            return false;
        }
    }

    return true;
}

bool checkGcmChecksumRecovery() {
    const std::vector<uint8_t> key(32, 0);
    const std::vector<uint8_t> iv(16, 0);

    {
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv, 19207, 18
        );
        const auto codec = bedrock::VersionedMcpeCodec::forVersion("1.20.61");
        const auto tick = codec.packetCodec().makePacketByName(
            "tick_sync",
            std::vector<uint8_t>(16, 0)
        );
        const auto compressionPacket = codec.encodeEncryptedCompressionPacket(
            {tick},
            7
        );
        auto sender = bedrock::BedrockEncryption::createCipherStream(
            bedrock::protocolVersionFor("1.20.61"),
            key,
            iv,
            bedrock::BedrockCipherMode::Encrypt
        );
        const auto prefixWire = makeSequentialEncryptedPlayerWire(
            *sender, key, 0, compressionPacket
        );
        const auto missingWire = makeSequentialEncryptedPlayerWire(
            *sender, key, 1, compressionPacket
        );
        const auto recoveredWire = makeSequentialEncryptedPlayerWire(
            *sender, key, 2, compressionPacket
        );
        const auto followingWire = makeSequentialEncryptedPlayerWire(
            *sender, key, 3, compressionPacket
        );

        int tickCalls = 0;
        int recoveryCalls = 0;
        std::string recoveryDetail;
        server.onInbound([&](const bedrock::BedrockServerPacketEvent& event) {
            if (event.packet.name == "tick_sync") {
                ++tickCalls;
            }
        });
        server.onTransport([&](
            const bedrock::BedrockServerTransportEvent& event
        ) {
            if (event.message.starts_with(
                    "downstream encryption recovery succeeded "
                    "mode=forward_resync"
                )) {
                ++recoveryCalls;
                recoveryDetail = event.message;
            }
        });

        // Simulate one reliable application batch disappearing after the
        // sender has already advanced its continuous GCM stream.
        bedrock::BedrockServerTestAccess::handle(
            server,
            connection,
            prefixWire
        );
        bedrock::BedrockServerTestAccess::handle(
            server,
            connection,
            recoveredWire
        );
        bedrock::BedrockServerTestAccess::handle(
            server,
            connection,
            followingWire
        );

        const auto expectedRecovery =
            "downstream encryption recovery succeeded mode=forward_resync "
            "receive_counter=1 skipped_batches=1 skipped_cipher_bytes=" +
            std::to_string(missingWire.size() - 1) +
            " ignored_batches=0 ignored_bytes=0";
        if (recoveryCalls != 1 || recoveryDetail != expectedRecovery ||
            tickCalls != 3 ||
            bedrock::BedrockServerTestAccess::counters(server, connection) !=
                std::pair<uint64_t, uint64_t>{0, 4} ||
            bedrock::BedrockServerTestAccess::scheduledCloses(server) != 0) {
            std::cerr << "[SMOKE] GCM forward checksum recovery failed\n";
            return false;
        }
    }

    {
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv, 19208, 19
        );
        const auto codec = bedrock::VersionedMcpeCodec::forVersion("1.20.61");
        const auto tick = codec.packetCodec().makePacketByName(
            "tick_sync",
            std::vector<uint8_t>(16, 0)
        );
        const auto compressionPacket = codec.encodeEncryptedCompressionPacket(
            {tick},
            7
        );
        auto sender = bedrock::BedrockEncryption::createCipherStream(
            bedrock::protocolVersionFor("1.20.61"),
            key,
            iv,
            bedrock::BedrockCipherMode::Encrypt
        );
        const auto firstWire = makeSequentialEncryptedPlayerWire(
            *sender, key, 0, compressionPacket
        );
        const auto secondWire = makeSequentialEncryptedPlayerWire(
            *sender, key, 1, compressionPacket
        );
        auto staleWire = firstWire;
        staleWire[staleWire.size() / 2] ^= 0x5a;

        int tickCalls = 0;
        int pendingCalls = 0;
        int pendingDuplicateCalls = 0;
        int ignoredStaleCalls = 0;
        server.onInbound([&](const bedrock::BedrockServerPacketEvent& event) {
            if (event.packet.name == "tick_sync") {
                ++tickCalls;
            }
        });
        server.onTransport([&](
            const bedrock::BedrockServerTransportEvent& event
        ) {
            if (event.message.starts_with(
                    "downstream encryption recovery pending"
                )) {
                ++pendingCalls;
            } else if (event.message.starts_with(
                    "downstream encryption recovery duplicate ignored"
                )) {
                ++pendingDuplicateCalls;
            } else if (event.message.find("mode=ignored_stale") !=
                    std::string::npos) {
                ++ignoredStaleCalls;
            }
        });

        bedrock::BedrockServerTestAccess::handle(server, connection, staleWire);
        bedrock::BedrockServerTestAccess::handle(server, connection, staleWire);
        if (pendingCalls != 1 || pendingDuplicateCalls != 1 || tickCalls != 0 ||
            bedrock::BedrockServerTestAccess::counters(server, connection) !=
                std::pair<uint64_t, uint64_t>{0, 0}) {
            std::cerr << "[SMOKE] failed GCM probe mutated live cipher state\n";
            return false;
        }

        bedrock::BedrockServerTestAccess::handle(server, connection, firstWire);
        bedrock::BedrockServerTestAccess::handle(server, connection, secondWire);
        if (ignoredStaleCalls != 1 || tickCalls != 2 ||
            bedrock::BedrockServerTestAccess::counters(server, connection) !=
                std::pair<uint64_t, uint64_t>{0, 2} ||
            bedrock::BedrockServerTestAccess::scheduledCloses(server) != 0) {
            std::cerr << "[SMOKE] GCM stale-batch recovery did not resume stream\n";
            return false;
        }
    }

    {
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv, 19209, 20
        );
        const auto codec = bedrock::VersionedMcpeCodec::forVersion("1.20.61");
        const auto tick = codec.packetCodec().makePacketByName(
            "tick_sync",
            std::vector<uint8_t>(16, 0)
        );
        const auto compressionPacket = codec.encodeEncryptedCompressionPacket(
            {tick},
            7
        );
        auto sender = bedrock::BedrockEncryption::createCipherStream(
            bedrock::protocolVersionFor("1.20.61"),
            key,
            iv,
            bedrock::BedrockCipherMode::Encrypt
        );
        const auto validWire = makeSequentialEncryptedPlayerWire(
            *sender, key, 0, compressionPacket
        );

        int errorCalls = 0;
        std::string transportFailure;
        connection.onError([&](const std::string&) {
            ++errorCalls;
        });
        server.onTransport([&](
            const bedrock::BedrockServerTransportEvent& event
        ) {
            if (event.kind == bedrock::BedrockServerTransportEventKind::Error &&
                event.message.find("recovery_exhausted") != std::string::npos) {
                transportFailure = event.message;
            }
        });

        for (std::size_t i = 0; i < 9; ++i) {
            auto invalidWire = validWire;
            const auto offset = 1 + (i % (invalidWire.size() - 1));
            invalidWire[offset] ^= static_cast<uint8_t>(0x31 + i);
            bedrock::BedrockServerTestAccess::handle(
                server,
                connection,
                invalidWire
            );
        }

        if (errorCalls != 1 ||
            transportFailure.find("recovery_exhausted pending_batches=8") ==
                std::string::npos ||
            bedrock::BedrockServerTestAccess::counters(server, connection) !=
                std::pair<uint64_t, uint64_t>{1, 0} ||
            bedrock::BedrockServerTestAccess::scheduledCloses(server) != 1) {
            std::cerr << "[SMOKE] bounded GCM recovery did not fail closed\n";
            return false;
        }
    }

    return true;
}

bool checkEncryptedPlayerErrorBranches() {
    const std::vector<uint8_t> key(32, 0);
    const std::vector<uint8_t> iv(16, 0);
    const std::string checksumMessage =
        "Checksum mismatch 0000000000000000 != 9e1736c43d19118e";
    const std::vector<uint8_t> badChecksumPlaintext {
        0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    {
        bedrock::BedrockServer server({.version = "1.16.201", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv, 19192, 3
        );
        try {
            bedrock::BedrockServerTestAccess::handle(
                server,
                connection,
                makeEncryptedPlayerWire("1.16.201", key, iv, badChecksumPlaintext)
            );
            std::cerr << "[SMOKE] unhandled checksum error did not throw\n";
            return false;
        } catch (const bedrock::BedrockUnhandledPlayerError& error) {
            if (std::string(error.what()) != checksumMessage) {
                std::cerr << "[SMOKE] unhandled checksum message mismatch: "
                          << error.what() << "\n";
                return false;
            }
        }
        if (bedrock::BedrockServerTestAccess::counters(server, connection) !=
                std::pair<uint64_t, uint64_t>{0, 1} ||
            bedrock::BedrockServerTestAccess::scheduledCloses(server) != 0) {
            std::cerr << "[SMOKE] unhandled checksum reached disconnect or lost counter\n";
            return false;
        }
    }

    {
        bedrock::BedrockServer server({.version = "1.16.201", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv, 19200, 11
        );
        int errorCalls = 0;
        connection.onError([&](const std::string&) {
            ++errorCalls;
            server.close();
        });
        const auto badWire = makeEncryptedPlayerWire(
            "1.16.201",
            key,
            iv,
            badChecksumPlaintext
        );
        bedrock::BedrockServerTestAccess::handle(server, connection, badWire);
        bedrock::BedrockServerTestAccess::handle(server, connection, badWire);
        if (errorCalls != 1 ||
            bedrock::BedrockServerTestAccess::hasPlayerState(server, connection)) {
            std::cerr << "[SMOKE] error listener close resurrected Player session\n";
            return false;
        }
    }

    {
        bedrock::BedrockServer server({.version = "1.16.201", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv, 19193, 4
        );
        std::atomic<int> errorCalls {0};
        std::atomic<int> closeCalls {0};
        std::atomic<int> callbackSequence {0};
        std::atomic<int> errorSequence {0};
        std::atomic<int> closeSequence {0};
        bool errorBeforeDisconnect = false;
        std::string observed;
        connection.onError([&](const std::string& message) {
            ++errorCalls;
            errorSequence = ++callbackSequence;
            observed = message;
            errorBeforeDisconnect =
                bedrock::BedrockServerTestAccess::counters(server, connection) ==
                    std::pair<uint64_t, uint64_t>{0, 1} &&
                bedrock::BedrockServerTestAccess::scheduledCloses(server) == 0;
        });
        connection.onClose([&]() {
            ++closeCalls;
            closeSequence = ++callbackSequence;
        });
        const auto badWire = makeEncryptedPlayerWire(
            "1.16.201",
            key,
            iv,
            badChecksumPlaintext
        );
        bedrock::BedrockServerTestAccess::handle(server, connection, badWire);
        // A delayed close leaves a short window where already queued RakNet
        // payloads can arrive. Only the first checksum failure may advance the
        // stream, emit Player error, or send a disconnect packet.
        bedrock::BedrockServerTestAccess::handle(server, connection, badWire);
        if (errorCalls != 1 || observed != checksumMessage || !errorBeforeDisconnect ||
            bedrock::BedrockServerTestAccess::counters(server, connection) !=
                std::pair<uint64_t, uint64_t>{1, 1} ||
            bedrock::BedrockServerTestAccess::scheduledCloses(server) != 1) {
            std::cerr << "[SMOKE] handled checksum error/disconnect ordering mismatch\n";
            return false;
        }

        for (int i = 0; i < 50; ++i) {
            if (closeCalls.load() == 1 &&
                bedrock::BedrockServerTestAccess::listenerCounts(connection) ==
                    std::pair<std::size_t, std::size_t>{0, 0}) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (closeCalls.load() != 1 || errorSequence.load() == 0 ||
            !(errorSequence.load() < closeSequence.load()) ||
            bedrock::BedrockServerTestAccess::listenerCounts(connection) !=
                std::pair<std::size_t, std::size_t>{0, 0}) {
            std::cerr << "[SMOKE] checksum disconnect delayed Player close mismatch\n";
            return false;
        }
    }

    {
        bedrock::BedrockServer server({.version = "1.16.201", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv, 19194, 5
        );
        int laterCalls = 0;
        connection.onError([](const std::string&) {
            throw std::runtime_error("listener boom");
        });
        connection.onError([&](const std::string&) {
            ++laterCalls;
        });
        try {
            bedrock::BedrockServerTestAccess::handle(
                server,
                connection,
                makeEncryptedPlayerWire("1.16.201", key, iv, badChecksumPlaintext)
            );
            std::cerr << "[SMOKE] throwing error listener did not propagate\n";
            return false;
        } catch (const std::runtime_error& error) {
            if (std::string(error.what()) != "listener boom") {
                throw;
            }
        }
        if (laterCalls != 0 ||
            bedrock::BedrockServerTestAccess::counters(server, connection) !=
                std::pair<uint64_t, uint64_t>{0, 1} ||
            bedrock::BedrockServerTestAccess::scheduledCloses(server) != 0) {
            std::cerr << "[SMOKE] throwing error listener did not abort disconnect\n";
            return false;
        }
    }

    {
        bedrock::BedrockServer server({.version = "1.16.201", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv, 19195, 6
        );
        int errorCalls = 0;
        connection.onError([&](const std::string&) {
            ++errorCalls;
        });
        try {
            bedrock::BedrockServerTestAccess::handle(
                server,
                connection,
                makeValidEncryptedPlayerWire("1.16.201", key, iv, {0x00})
            );
            std::cerr << "[SMOKE] malformed raw-deflate did not throw\n";
            return false;
        } catch (const std::exception&) {
        }
        if (errorCalls != 0 ||
            bedrock::BedrockServerTestAccess::counters(server, connection) !=
                std::pair<uint64_t, uint64_t>{0, 1} ||
            bedrock::BedrockServerTestAccess::scheduledCloses(server) != 0) {
            std::cerr << "[SMOKE] raw-deflate error emitted/disconnected or lost counter\n";
            return false;
        }
    }

    for (const bool withListener : {false, true}) {
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server,
            key,
            iv,
            static_cast<uint16_t>(withListener ? 19197 : 19196),
            withListener ? 8 : 7
        );
        int errorCalls = 0;
        std::string observed;
        if (withListener) {
            connection.onError([&](const std::string& message) {
                ++errorCalls;
                observed = message;
            });
        }
        try {
            bedrock::BedrockServerTestAccess::handle(
                server,
                connection,
                makeValidEncryptedPlayerWire("1.20.61", key, iv, {0x02})
            );
            std::cerr << "[SMOKE] unsupported encrypted compressor did not throw\n";
            return false;
        } catch (const bedrock::BedrockUnhandledPlayerError& error) {
            if (withListener || std::string(error.what()) != "Unsupported compressor: 2") {
                std::cerr << "[SMOKE] unsupported compressor unhandled branch mismatch\n";
                return false;
            }
        } catch (const bedrock::BedrockUndefinedPlayerBufferError& error) {
            if (!withListener || std::string(error.what()) !=
                    "Cannot read properties of undefined (reading 'byteLength')") {
                std::cerr << "[SMOKE] unsupported compressor downstream branch mismatch\n";
                return false;
            }
        }
        if (errorCalls != (withListener ? 1 : 0) ||
            (withListener && observed != "Unsupported compressor: 2") ||
            bedrock::BedrockServerTestAccess::counters(server, connection) !=
                std::pair<uint64_t, uint64_t>{0, 1} ||
            bedrock::BedrockServerTestAccess::scheduledCloses(server) != 0) {
            std::cerr << "[SMOKE] unsupported compressor event/counter/disconnect mismatch\n";
            return false;
        }
    }

    {
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv, 19198, 9
        );
        int errorCalls = 0;
        connection.onError([&](const std::string&) {
            ++errorCalls;
        });
        try {
            bedrock::BedrockServerTestAccess::handle(
                server,
                connection,
                makeValidEncryptedPlayerWire("1.20.61", key, iv, {0xff, 0x80})
            );
            std::cerr << "[SMOKE] encrypted framing error did not throw\n";
            return false;
        } catch (const std::exception&) {
        }
        if (errorCalls != 0 ||
            bedrock::BedrockServerTestAccess::counters(server, connection) !=
                std::pair<uint64_t, uint64_t>{0, 1} ||
            bedrock::BedrockServerTestAccess::scheduledCloses(server) != 0) {
            std::cerr << "[SMOKE] framing error emitted/disconnected or lost counter\n";
            return false;
        }
    }

    {
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv, 19199, 10
        );
        int errorCalls = 0;
        connection.onError([&](const std::string&) {
            ++errorCalls;
        });
        bedrock::BedrockServerTestAccess::handle(
            server,
            connection,
            makeValidEncryptedPlayerWire("1.20.61", key, iv, {0xff, 0x00})
        );
        if (errorCalls != 0 ||
            bedrock::BedrockServerTestAccess::counters(server, connection) !=
                std::pair<uint64_t, uint64_t>{1, 1} ||
            bedrock::BedrockServerTestAccess::scheduledCloses(server) != 1) {
            std::cerr << "[SMOKE] readPacket deserialize error did not disconnect Server error\n";
            return false;
        }
    }

    {
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv, 19205, 16
        );
        int duplicateEvents = 0;
        std::string duplicateDetail;
        server.onTransport([&](
            const bedrock::BedrockServerTransportEvent& event
        ) {
            if (event.kind ==
                    bedrock::BedrockServerTransportEventKind::Receive &&
                event.message.starts_with("duplicate encrypted batch ignored")) {
                ++duplicateEvents;
                duplicateDetail = event.message;
            }
        });

        const auto codec = bedrock::VersionedMcpeCodec::forVersion("1.20.61");
        const auto handshake = codec.packetCodec().makePacketByName(
            "client_to_server_handshake",
            {}
        );
        const auto compressionPacket = codec.encodeCompressionPacket(
            {handshake},
            bedrock::VersionedMcpeCompression::Uncompressed
        );
        const auto wire = makeValidEncryptedPlayerWire(
            "1.20.61",
            key,
            iv,
            compressionPacket
        );
        bedrock::BedrockServerTestAccess::handle(server, connection, wire);
        bedrock::BedrockServerTestAccess::handle(server, connection, wire);

        if (duplicateEvents != 1 ||
            duplicateDetail !=
                "duplicate encrypted batch ignored original_receive_counter=0 "
                "receive_counter=1 replay_distance=1" ||
            bedrock::BedrockServerTestAccess::counters(server, connection) !=
                std::pair<uint64_t, uint64_t>{1, 1} ||
            bedrock::BedrockServerTestAccess::scheduledCloses(server) != 0) {
            std::cerr << "[SMOKE] encrypted duplicate was not ignored before "
                         "advancing cipher state\n";
            return false;
        }
    }

    {
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv, 19206, 17
        );
        int duplicateEvents = 0;
        std::string duplicateDetail;
        server.onTransport([&](
            const bedrock::BedrockServerTransportEvent& event
        ) {
            if (event.kind ==
                    bedrock::BedrockServerTransportEventKind::Receive &&
                event.message.starts_with("duplicate encrypted batch ignored")) {
                ++duplicateEvents;
                duplicateDetail = event.message;
            }
        });

        const auto codec = bedrock::VersionedMcpeCodec::forVersion("1.20.61");
        const auto tick = codec.packetCodec().makePacketByName(
            "tick_sync",
            std::vector<uint8_t>(16, 0)
        );
        const auto compressionPacket = codec.encodeEncryptedCompressionPacket(
            {tick},
            7
        );
        auto cipher = bedrock::BedrockEncryption::createCipherStream(
            bedrock::protocolVersionFor("1.20.61"),
            key,
            iv,
            bedrock::BedrockCipherMode::Encrypt
        );

        constexpr uint64_t acceptedBatchCount = 128;
        std::vector<uint8_t> firstWire;
        for (uint64_t counter = 0;
             counter < acceptedBatchCount;
             ++counter) {
            const auto plaintext = bedrock::BedrockEncryption::makeAesPlaintext(
                compressionPacket,
                counter,
                key
            );
            auto encrypted = cipher->process(plaintext);
            encrypted.insert(encrypted.begin(), 0xfe);
            if (counter == 0) {
                firstWire = encrypted;
            }
            bedrock::BedrockServerTestAccess::handle(
                server,
                connection,
                encrypted
            );
        }

        // A one-entry/32-entry replay cache passes the adjacent duplicate
        // test above but fails this live failure mode after normal movement
        // traffic advances the continuous cipher far enough.
        bedrock::BedrockServerTestAccess::handle(
            server,
            connection,
            firstWire
        );

        if (duplicateEvents != 1 ||
            duplicateDetail !=
                "duplicate encrypted batch ignored original_receive_counter=0 "
                "receive_counter=128 replay_distance=128" ||
            bedrock::BedrockServerTestAccess::counters(server, connection) !=
                std::pair<uint64_t, uint64_t>{0, acceptedBatchCount} ||
            bedrock::BedrockServerTestAccess::scheduledCloses(server) != 0) {
            std::cerr << "[SMOKE] delayed encrypted replay escaped the "
                         "transport retry history\n";
            return false;
        }
    }

    return true;
}

bool checkPlayerReadPacketBoundaries() {
    {
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addPlainPlayer(server);
        for (const auto& malformed : {
                 std::vector<uint8_t>{0xfd},
                 std::vector<uint8_t>{}
             }) {
            try {
                bedrock::BedrockServerTestAccess::handle(server, connection, malformed);
                std::cerr << "[SMOKE] known Player bad header did not throw\n";
                return false;
            } catch (const std::runtime_error& error) {
                const auto expected = malformed.empty()
                    ? std::string("Bad packet header undefined")
                    : std::string("Bad packet header 253");
                if (error.what() != expected) {
                    std::cerr << "[SMOKE] known Player bad header mismatch: "
                              << error.what() << "\n";
                    return false;
                }
            }
        }
        if (bedrock::BedrockServerTestAccess::scheduledCloses(server) != 0) {
            std::cerr << "[SMOKE] bad outer header reached generic disconnect\n";
            return false;
        }
    }

    {
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        bedrock::BedrockServerConnection unknown;
        unknown.address = "127.0.0.1";
        unknown.port = 29999;
        unknown.clientGuid = 99;
        unknown.peer.address = unknown.address;
        unknown.peer.port = unknown.port;
        unknown.peer.clientGuid = unknown.clientGuid;
        unknown.playerEvents = std::make_shared<bedrock::BedrockServerPlayerEventState>();
        bedrock::BedrockServerTestAccess::handle(server, unknown, {0xfd});
        if (bedrock::BedrockServerTestAccess::hasPlayerState(server, unknown)) {
            std::cerr << "[SMOKE] unknown RakNet peer resurrected Player state\n";
            return false;
        }
    }

    const auto malformedKnownPacket = []() {
        auto codec = bedrock::VersionedMcpeCodec::forVersion("1.20.61");
        return codec.packetCodec().makePacketByName(
            "request_network_settings",
            {}
        );
    }();

    {
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addPlainPlayer(
            server, 19202, 13, true
        );
        const auto codec = bedrock::VersionedMcpeCodec::forVersion("1.20.61");
        const auto wire = codec.encodeMcpePayload(
            {malformedKnownPacket},
            bedrock::VersionedMcpeCompression::Uncompressed
        );
        bedrock::BedrockServerTestAccess::handle(server, connection, wire);
        if (bedrock::BedrockServerTestAccess::scheduledCloses(server) != 1) {
            std::cerr << "[SMOKE] plaintext malformed known fields skipped readPacket catch\n";
            return false;
        }
    }

    {
        const std::vector<uint8_t> key(32, 0);
        const std::vector<uint8_t> iv(16, 0);
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addEncryptedPlayer(
            server, key, iv, 19203, 14
        );
        const auto codec = bedrock::VersionedMcpeCodec::forVersion("1.20.61");
        const auto framed = codec.batchCodec().encodeFramedBatch({malformedKnownPacket});
        std::vector<uint8_t> compressionPacket {0xff};
        compressionPacket.insert(
            compressionPacket.end(),
            framed.begin(),
            framed.end()
        );
        bedrock::BedrockServerTestAccess::handle(
            server,
            connection,
            makeValidEncryptedPlayerWire("1.20.61", key, iv, compressionPacket)
        );
        if (bedrock::BedrockServerTestAccess::counters(server, connection) !=
                std::pair<uint64_t, uint64_t>{1, 1} ||
            bedrock::BedrockServerTestAccess::scheduledCloses(server) != 1) {
            std::cerr << "[SMOKE] encrypted malformed known fields skipped readPacket catch\n";
            return false;
        }
    }

    {
        bedrock::BedrockServer server({.version = "1.20.61", .offline = true});
        const auto connection = bedrock::BedrockServerTestAccess::addPlainPlayer(
            server, 19204, 15, true
        );
        const auto harmless = bedrock::VersionedMcpeCodec::forVersion("1.20.61")
            .packetCodec().makePacketByName("client_to_server_handshake", {});
        std::atomic<int> closeCalls {0};
        connection.onClose([&]() {
            ++closeCalls;
            // A retained close-listener snapshot is allowed to call write,
            // but Connection.sendMCPE drops it and must not recreate Session.
            server.sendPacket(connection, harmless);
        });

        server.close();
        for (int attempt = 0; attempt < 30 && closeCalls.load() == 0; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Late network callbacks and public writes after Server#close are
        // inert even when they retain the old peer/GUID.
        bedrock::BedrockServerTestAccess::handle(server, connection, {0xfd});
        server.sendPacket(connection, harmless);
        server.disconnect(connection, "late");
        if (closeCalls.load() != 1 ||
            bedrock::BedrockServerTestAccess::hasPlayerState(server, connection) ||
            bedrock::BedrockServerTestAccess::listenerCounts(connection) !=
                std::pair<std::size_t, std::size_t>{0, 0}) {
            const auto listeners =
                bedrock::BedrockServerTestAccess::listenerCounts(connection);
            std::cerr << "[SMOKE] Server.close snapshot resurrected Player session: close="
                      << closeCalls.load() << " state="
                      << bedrock::BedrockServerTestAccess::hasPlayerState(
                             server,
                             connection
                         )
                      << " listeners=" << listeners.first << '/'
                      << listeners.second << "\n";
            return false;
        }
    }

    return true;
}

bool checkRakNetCallbackBoundary() {
    const auto callbackException =
        bedrock::BedrockServerTestAccess::rakNetLiveCallbackException();
    if (!callbackException) {
        std::cerr << "[SMOKE] RakNet swallowed live callback exception\n";
        return false;
    }
    try {
        std::rethrow_exception(callbackException);
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()) != "live callback boom") {
            std::cerr << "[SMOKE] RakNet changed live callback exception: "
                      << error.what() << "\n";
            return false;
        }
    } catch (...) {
        std::cerr << "[SMOKE] RakNet changed live callback exception type\n";
        return false;
    }

    if (!bedrock::BedrockServerTestAccess::rakNetInternalMessageIsFiltered()) {
        std::cerr << "[SMOKE] RakNet internal/application boundary regressed\n";
        return false;
    }
    return true;
}

bool checkTransportFailureDiagnostics() {
    {
        bedrock::BedrockServer server({
            .version = "1.21.100",
            .offline = true
        });
        const auto connection =
            bedrock::BedrockServerTestAccess::addPlainPlayer(
                server,
                19205,
                16,
                false
            );
        std::vector<std::pair<
            bedrock::BedrockServerTransportEventKind,
            std::string
        >> phases;
        std::size_t networkSettingsBytes = 0;
        uint64_t networkSettingsHash = 0;
        std::string sendRejection;
        server.onTransport([&](
            const bedrock::BedrockServerTransportEvent& event
        ) {
            phases.emplace_back(event.kind, event.packetName);
            if (event.kind ==
                    bedrock::BedrockServerTransportEventKind::SendPacket &&
                event.packetName == "network_settings") {
                networkSettingsBytes = event.byteLength;
                networkSettingsHash = event.byteHash;
            }
            if (event.kind ==
                bedrock::BedrockServerTransportEventKind::Error) {
                sendRejection = event.message;
            }
        });

        const auto codec =
            bedrock::VersionedMcpeCodec::forVersion("1.21.100");
        const uint32_t protocol = codec.definition().protocolVersion();
        const auto request = codec.packetCodec().makePacketByName(
            "request_network_settings",
            {
                static_cast<uint8_t>((protocol >> 24u) & 0xffu),
                static_cast<uint8_t>((protocol >> 16u) & 0xffu),
                static_cast<uint8_t>((protocol >> 8u) & 0xffu),
                static_cast<uint8_t>(protocol & 0xffu)
            }
        );
        const auto framed = codec.batchCodec().encodeFramedBatch({request});
        std::vector<uint8_t> wire {0xfe};
        wire.insert(wire.end(), framed.begin(), framed.end());
        bedrock::BedrockServerTestAccess::handle(
            server,
            connection,
            wire
        );

        const std::vector<std::pair<
            bedrock::BedrockServerTransportEventKind,
            std::string
        >> expected {
            {
                bedrock::BedrockServerTransportEventKind::DecodedPacket,
                "request_network_settings"
            },
            {
                bedrock::BedrockServerTransportEventKind::SendPacket,
                "network_settings"
            },
            {
                bedrock::BedrockServerTransportEventKind::Error,
                "network_settings"
            }
        };
        const bool sendRejected = sendRejection.find(
            "RakNet pre-compression send rejected "
            "raknet_status=server_stopped raknet_receipt=0 "
            "connection_state=-1"
        ) != std::string::npos;
        if (phases != expected || !sendRejected ||
            networkSettingsBytes != 12 ||
            networkSettingsHash != 0x1853c5e83e41e97dull) {
            std::cerr << "[SMOKE] rejected transport reported a false send or "
                         "changed protocol-827 network_settings identity\n";
            return false;
        }
    }

    {
        bedrock::BedrockServer server({
            .version = "1.21.100",
            .offline = true
        });
        const auto connection =
            bedrock::BedrockServerTestAccess::addPlainPlayer(
                server,
                19207,
                18,
                true
            );
        std::atomic<int> closeCalls {0};
        std::atomic<int> errorCalls {0};
        connection.onClose([&]() {
            ++closeCalls;
        });
        server.onTransport([&](
            const bedrock::BedrockServerTransportEvent& event
        ) {
            if (event.kind ==
                bedrock::BedrockServerTransportEventKind::Error) {
                ++errorCalls;
            }
        });

        bedrock::BedrockServerTestAccess::rejectBatchAsDisconnected(
            server,
            connection
        );
        // A stale producer retaining Player must be inert after the first
        // not-connected result; it must not recreate the previous log flood.
        bedrock::BedrockServerTestAccess::rejectBatchAsDisconnected(
            server,
            connection
        );

        if (closeCalls.load() != 1 || errorCalls.load() != 0 ||
            bedrock::BedrockServerTestAccess::hasPlayerState(
                server,
                connection
            ) || server.clientCount() != 0) {
            std::cerr << "[SMOKE] not-connected send did not finalize the "
                         "session exactly once: close="
                      << closeCalls.load() << " errors=" << errorCalls.load()
                      << " state="
                      << bedrock::BedrockServerTestAccess::hasPlayerState(
                             server,
                             connection
                         )
                      << " clients=" << server.clientCount() << "\n";
            return false;
        }
    }

    bedrock::BedrockServer server({.version = "1.21.100", .offline = true});
    const auto connection = bedrock::BedrockServerTestAccess::addPlainPlayer(
        server,
        19206,
        17,
        true
    );

    std::atomic<int> closeCalls {0};
    std::atomic<int> diagnosticCalls {0};
    std::mutex diagnosticMutex;
    std::vector<std::string> messages;
    connection.onClose([&]() {
        ++closeCalls;
    });
    // One broken observer must not suppress later observers or alter traffic.
    server.onTransport([](const bedrock::BedrockServerTransportEvent&) {
        throw std::runtime_error("diagnostic observer boom");
    });
    server.onTransport([&](const bedrock::BedrockServerTransportEvent& event) {
        if (event.kind != bedrock::BedrockServerTransportEventKind::Error) {
            return;
        }
        ++diagnosticCalls;
        std::lock_guard<std::mutex> lock(diagnosticMutex);
        messages.push_back(event.message);
    });

    const auto packet = bedrock::VersionedMcpeCodec::forVersion("1.21.100")
        .packetCodec().makePacketByName("client_to_server_handshake", {});
    bedrock::BedrockServerTestAccess::forceOutboundQueueFailure(
        server,
        connection,
        packet
    );

    bool sawQueueFailure = false;
    bool sawDisconnectFallback = false;
    {
        std::lock_guard<std::mutex> lock(diagnosticMutex);
        for (const auto& message : messages) {
            sawQueueFailure = sawQueueFailure ||
                message.find(
                    "outbound queue failure: server encrypt stream is not initialized"
                ) != std::string::npos;
            sawDisconnectFallback = sawDisconnectFallback ||
                message.find(
                    "outbound queue failure disconnect failure: "
                    "server encrypt stream is not initialized"
                ) != std::string::npos;
        }
    }

    if (!sawQueueFailure || !sawDisconnectFallback ||
        diagnosticCalls.load() < 2 || closeCalls.load() != 1 ||
        server.status(connection) !=
            bedrock::BedrockServerClientStatus::Disconnected) {
        std::cerr << "[SMOKE] outbound worker failure was hidden or left active: "
                  << "diagnostics=" << diagnosticCalls.load()
                  << " close=" << closeCalls.load()
                  << " status=" << static_cast<int>(server.status(connection))
                  << " queue_error=" << sawQueueFailure
                  << " fallback=" << sawDisconnectFallback << "\n";
        return false;
    }
    return true;
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

void appendMagic(std::vector<uint8_t>& out) {
    out.insert(out.end(), std::begin(RAKNET_MAGIC), std::end(RAKNET_MAGIC));
}

void writeRakNetAddress(std::vector<uint8_t>& out, uint16_t port) {
    out.push_back(4);
    out.push_back(static_cast<uint8_t>(~uint8_t {127}));
    out.push_back(static_cast<uint8_t>(~uint8_t {0}));
    out.push_back(static_cast<uint8_t>(~uint8_t {0}));
    out.push_back(static_cast<uint8_t>(~uint8_t {1}));
    writeU16BE(out, port);
}

uint16_t readU16BE(const std::vector<uint8_t>& data, std::size_t& offset) {
    if (offset + 2 > data.size()) throw std::runtime_error("readU16BE out of range");
    uint16_t value =
        static_cast<uint16_t>(static_cast<uint16_t>(data[offset]) << 8u) |
        static_cast<uint16_t>(data[offset + 1]);
    offset += 2;
    return value;
}

uint32_t readU32BE(const std::vector<uint8_t>& data, std::size_t& offset) {
    if (offset + 4 > data.size()) throw std::runtime_error("readU32BE out of range");
    const uint32_t value =
        (static_cast<uint32_t>(data[offset]) << 24u) |
        (static_cast<uint32_t>(data[offset + 1]) << 16u) |
        (static_cast<uint32_t>(data[offset + 2]) << 8u) |
        static_cast<uint32_t>(data[offset + 3]);
    offset += 4;
    return value;
}

uint32_t readVarUInt(const std::vector<uint8_t>& data, std::size_t& offset) {
    uint32_t result = 0;
    uint32_t shift = 0;

    while (true) {
        if (offset >= data.size()) throw std::runtime_error("readVarUInt out of range");
        uint8_t byte = data[offset++];
        result |= static_cast<uint32_t>(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) return result;
        shift += 7;
        if (shift >= 35) throw std::runtime_error("varuint too large");
    }
}

uint32_t readTriadLE(const std::vector<uint8_t>& data, std::size_t& offset) {
    if (offset + 3 > data.size()) throw std::runtime_error("readTriadLE out of range");
    uint32_t value =
        static_cast<uint32_t>(data[offset]) |
        (static_cast<uint32_t>(data[offset + 1]) << 8u) |
        (static_cast<uint32_t>(data[offset + 2]) << 16u);
    offset += 3;
    return value;
}

std::vector<uint8_t> buildConnectionRequestDatagram(
    uint64_t clientGuid = RAKNET_CLIENT_GUID
) {
    std::vector<uint8_t> payload;
    payload.push_back(0x09);
    writeU64BE(payload, clientGuid);
    writeU64BE(payload, 1234567);
    payload.push_back(0x00);

    std::vector<uint8_t> out;
    out.push_back(0x80);
    writeTriadLE(out, 0);
    out.push_back(3u << 5u);
    writeU16BE(out, static_cast<uint16_t>(payload.size() * 8u));
    writeTriadLE(out, 0);
    writeTriadLE(out, 0);
    out.push_back(0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> buildOpenConnectionRequest1() {
    std::vector<uint8_t> out {0x05};
    appendMagic(out);
    out.push_back(RAKNET_PROTOCOL);
    out.resize(RAKNET_MTU - 28, 0x00);
    return out;
}

std::vector<uint8_t> buildOpenConnectionRequest2(
    uint16_t port,
    uint64_t clientGuid = RAKNET_CLIENT_GUID
) {
    std::vector<uint8_t> out {0x07};
    appendMagic(out);
    writeRakNetAddress(out, port);
    writeU16BE(out, RAKNET_MTU);
    writeU64BE(out, clientGuid);
    return out;
}

std::vector<uint8_t> buildNewIncomingConnection(uint16_t port) {
    std::vector<uint8_t> out {0x13};
    writeRakNetAddress(out, port);
    for (int i = 0; i < 20; ++i) {
        writeRakNetAddress(out, port);
    }
    writeU64BE(out, 1234567);
    writeU64BE(out, 7654321);
    return out;
}

std::vector<uint8_t> buildAck(uint32_t sequence) {
    std::vector<uint8_t> out {0xc0};
    writeU16BE(out, 1);
    out.push_back(1);
    writeTriadLE(out, sequence);
    return out;
}

std::vector<uint8_t> buildReliableDatagram(
    const std::vector<uint8_t>& payload,
    uint32_t sequence,
    uint32_t reliableIndex,
    uint32_t orderedIndex
) {
    std::vector<uint8_t> out;
    out.push_back(0x80);
    writeTriadLE(out, sequence);
    out.push_back(3u << 5u);
    writeU16BE(out, static_cast<uint16_t>(payload.size() * 8u));
    writeTriadLE(out, reliableIndex);
    writeTriadLE(out, orderedIndex);
    out.push_back(0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> buildSplitReliableDatagram(
    const std::vector<uint8_t>& payloadPart,
    uint32_t sequence,
    uint32_t reliableIndex,
    uint32_t orderedIndex,
    uint32_t splitCount,
    uint16_t splitId,
    uint32_t splitIndex
) {
    std::vector<uint8_t> out;
    out.push_back(0x80);
    writeTriadLE(out, sequence);
    out.push_back(static_cast<uint8_t>((3u << 5u) | 0x10u));
    writeU16BE(out, static_cast<uint16_t>(payloadPart.size() * 8u));
    writeTriadLE(out, reliableIndex);
    writeTriadLE(out, orderedIndex);
    out.push_back(0);
    writeU32BE(out, splitCount);
    writeU16BE(out, splitId);
    writeU32BE(out, splitIndex);
    out.insert(out.end(), payloadPart.begin(), payloadPart.end());
    return out;
}

struct ParsedDatagramFrame {
    uint32_t sequence = 0;
    uint8_t reliability = 0;
    bool split = false;
    uint32_t orderedIndex = 0;
    uint32_t splitCount = 0;
    uint16_t splitId = 0;
    uint32_t splitIndex = 0;
    std::vector<uint8_t> payload;
};

std::vector<ParsedDatagramFrame> parseDatagramFrames(const std::vector<uint8_t>& data) {
    std::vector<ParsedDatagramFrame> frames;
    if (data.empty() || data[0] < 0x80 || data[0] > 0x8f) {
        return frames;
    }

    std::size_t offset = 1;
    const uint32_t sequence = readTriadLE(data, offset);

    while (offset < data.size()) {
        const uint8_t flags = data[offset++];
        const uint8_t reliability = static_cast<uint8_t>((flags & 0xe0u) >> 5u);
        const uint16_t bitLength = readU16BE(data, offset);
        const std::size_t byteLength = (static_cast<std::size_t>(bitLength) + 7u) / 8u;

        if (reliability == 2 || reliability == 3 || reliability == 4 || reliability == 6 || reliability == 7) {
            (void) readTriadLE(data, offset);
        }
        if (reliability == 1 || reliability == 4) {
            (void) readTriadLE(data, offset);
        }
        uint32_t orderedIndex = 0;
        if (reliability == 3 || reliability == 4 || reliability == 7) {
            orderedIndex = readTriadLE(data, offset);
            ++offset;
        }
        const bool split = (flags & 0x10u) != 0;
        uint32_t splitCount = 0;
        uint16_t splitId = 0;
        uint32_t splitIndex = 0;
        if (split) {
            splitCount = readU32BE(data, offset);
            splitId = readU16BE(data, offset);
            splitIndex = readU32BE(data, offset);
        }
        if (offset + byteLength > data.size()) {
            break;
        }
        ParsedDatagramFrame frame;
        frame.sequence = sequence;
        frame.reliability = reliability;
        frame.split = split;
        frame.orderedIndex = orderedIndex;
        frame.splitCount = splitCount;
        frame.splitId = splitId;
        frame.splitIndex = splitIndex;
        frame.payload.assign(
            data.begin() + static_cast<std::ptrdiff_t>(offset),
            data.begin() + static_cast<std::ptrdiff_t>(offset + byteLength)
        );
        frames.push_back(std::move(frame));
        offset += byteLength;
    }

    return frames;
}

std::vector<std::vector<uint8_t>> parseDatagramPayloads(const std::vector<uint8_t>& data) {
    std::vector<std::vector<uint8_t>> payloads;
    for (auto& frame : parseDatagramFrames(data)) {
        payloads.push_back(std::move(frame.payload));
    }
    return payloads;
}

bool isConnectionRequestAcceptedDatagram(const std::vector<uint8_t>& data) {
    for (const auto& payload : parseDatagramPayloads(data)) {
        if (!payload.empty() && payload[0] == 0x10) {
            return true;
        }
    }

    return false;
}

bool sendPacket(
    int sock,
    const sockaddr_in& target,
    const std::vector<uint8_t>& packet
) {
    return sendto(
        sock,
        packet.data(),
        packet.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    ) == static_cast<ssize_t>(packet.size());
}

void acknowledgeDatagram(
    int sock,
    const sockaddr_in& target,
    const std::vector<uint8_t>& data
) {
    const auto frames = parseDatagramFrames(data);
    if (!frames.empty()) {
        (void) sendPacket(sock, target, buildAck(frames.front().sequence));
    }
}

bool receivePacket(int sock, std::vector<uint8_t>& packet, int timeoutMs = 200) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);

    timeval timeout {};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    const int ready = select(sock + 1, &readfds, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        return false;
    }

    packet.assign(4096, 0);
    const ssize_t received = recvfrom(
        sock,
        packet.data(),
        packet.size(),
        0,
        nullptr,
        nullptr
    );
    if (received <= 0) {
        return false;
    }
    packet.resize(static_cast<std::size_t>(received));
    return true;
}

bool waitForPacketId(int sock, uint8_t packetId) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::vector<uint8_t> packet;
        if (receivePacket(sock, packet) &&
            !packet.empty() &&
            packet[0] == packetId) {
            return true;
        }
    }
    return false;
}

bool waitForAcceptedAndAck(int sock, const sockaddr_in& target) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::vector<uint8_t> packet;
        if (!receivePacket(sock, packet) ||
            !isConnectionRequestAcceptedDatagram(packet)) {
            continue;
        }

        std::size_t offset = 1;
        const uint32_t sequence = readTriadLE(packet, offset);
        return sendPacket(sock, target, buildAck(sequence));
    }
    return false;
}

bool establishConnectedSession(
    int sock,
    const sockaddr_in& target,
    uint16_t port,
    uint64_t clientGuid = 0
) {
    if (clientGuid == 0) clientGuid = nextRakNetClientGuid();
    if (!sendPacket(sock, target, buildOpenConnectionRequest1()) ||
        !waitForPacketId(sock, 0x06)) {
        return false;
    }
    if (!sendPacket(
            sock,
            target,
            buildOpenConnectionRequest2(port, clientGuid)
        ) ||
        !waitForPacketId(sock, 0x08)) {
        return false;
    }
    if (!sendPacket(
            sock,
            target,
            buildConnectionRequestDatagram(clientGuid)
        ) ||
        !waitForAcceptedAndAck(sock, target)) {
        return false;
    }
    if (!sendPacket(
            sock,
            target,
            buildReliableDatagram(buildNewIncomingConnection(port), 1, 1, 1)) ||
        !waitForPacketId(sock, 0xc0)) {
        return false;
    }
    return true;
}

bool checkReliableSendBackpressure() {
    bedrock::RakNetServer server({
        .host = "127.0.0.1",
        .port = 0,
        .maxPlayers = 1,
        .protocolVersion = RAKNET_PROTOCOL
    });
    std::mutex peerMutex;
    bedrock::RakNetServerPeer connectedPeer;
    std::atomic<bool> opened {false};
    server.onOpenConnection([&](const bedrock::RakNetServerPeer& peer) {
        {
            std::lock_guard<std::mutex> lock(peerMutex);
            connectedPeer = peer;
        }
        opened = true;
    });
    server.listen();

    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(server.boundPort());
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0 || !establishConnectedSession(
            sock,
            target,
            server.boundPort()
        )) {
        if (sock >= 0) close(sock);
        server.close();
        std::cerr << "[SMOKE] reliable-window connection failed\n";
        return false;
    }
    for (int attempt = 0; attempt < 100 && !opened.load(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!opened.load()) {
        close(sock);
        server.close();
        std::cerr << "[SMOKE] reliable-window peer was not opened\n";
        return false;
    }

    // Drain the native-style unreliable connected ping emitted when the peer
    // transitions to CONNECTED.
    std::vector<uint8_t> ignored;
    while (receivePacket(sock, ignored, 5)) {
    }

    bedrock::RakNetServerPeer peer;
    {
        std::lock_guard<std::mutex> lock(peerMutex);
        peer = connectedPeer;
    }
    std::vector<uint8_t> payload(320'000);
    uint32_t random = 0x9e3779b9u;
    for (auto& byte : payload) {
        random ^= random << 13u;
        random ^= random >> 17u;
        random ^= random << 5u;
        byte = static_cast<uint8_t>(random & 0xffu);
    }
    server.sendReliable(peer, payload);

    bool ok = true;

    std::map<uint32_t, std::vector<uint8_t>> parts;
    std::optional<uint32_t> withheldSequence;
    std::optional<uint32_t> withheldSplitIndex;
    uint32_t expectedParts = 0;
    uint16_t expectedSplitId = 0;
    bool sawRetransmit = false;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline &&
           (!sawRetransmit || expectedParts == 0 ||
            parts.size() < expectedParts)) {
        std::vector<uint8_t> datagram;
        if (!receivePacket(sock, datagram, 100)) continue;
        for (const auto& frame : parseDatagramFrames(datagram)) {
            if (!frame.split || frame.splitCount == 0) continue;
            if (expectedParts == 0) {
                expectedParts = frame.splitCount;
                expectedSplitId = frame.splitId;
            }
            if (frame.splitId != expectedSplitId ||
                frame.splitCount != expectedParts ||
                frame.splitIndex >= expectedParts) {
                ok = false;
                continue;
            }

            const bool duplicate = parts.find(frame.splitIndex) != parts.end();
            parts.emplace(frame.splitIndex, frame.payload);
            if (!withheldSequence.has_value()) {
                withheldSequence = frame.sequence;
                withheldSplitIndex = frame.splitIndex;
                continue;
            }
            if (duplicate &&
                frame.splitIndex == *withheldSplitIndex) {
                sawRetransmit = true;
                (void) sendPacket(sock, target, buildAck(frame.sequence));
                continue;
            }
            if (frame.sequence == *withheldSequence) continue;
            (void) sendPacket(sock, target, buildAck(frame.sequence));
        }
    }

    std::vector<uint8_t> reconstructed;
    if (expectedParts != 0 && parts.size() == expectedParts) {
        for (uint32_t index = 0; index < expectedParts; ++index) {
            const auto found = parts.find(index);
            if (found == parts.end()) {
                ok = false;
                break;
            }
            reconstructed.insert(
                reconstructed.end(),
                found->second.begin(),
                found->second.end()
            );
        }
    } else {
        ok = false;
    }
    ok = sawRetransmit && reconstructed == payload && ok;
    if (!sawRetransmit) {
        std::cerr << "[SMOKE] reliable datagram was not retransmitted on timeout\n";
    }
    if (reconstructed != payload) {
        std::cerr << "[SMOKE] reliable-window payload reconstruction mismatch\n";
    }

    close(sock);
    server.close();
    return ok;
}

bool checkReconnectSplitSequenceResynchronization() {
    bedrock::RakNetServer server({
        .host = "127.0.0.1",
        .port = 0,
        .maxPlayers = 1,
        .protocolVersion = RAKNET_PROTOCOL
    });
    std::atomic<bool> opened {false};
    std::atomic<bool> sawPrelude {false};
    std::atomic<bool> sawLargePayload {false};
    std::atomic<bool> acceptedSecondDiscontinuity {false};
    const std::vector<uint8_t> prelude {0xfe, 0x01, 0x02, 0x03};
    const std::vector<uint8_t> rejectedPayload(600, 0xfe);
    std::vector<uint8_t> largePayload(32'000);
    largePayload.front() = 0xfe;
    uint32_t random = 0x6d2b79f5u;
    for (std::size_t index = 1; index < largePayload.size(); ++index) {
        random ^= random << 13u;
        random ^= random >> 17u;
        random ^= random << 5u;
        largePayload[index] = static_cast<uint8_t>(random & 0xffu);
    }

    server.onOpenConnection([&](const bedrock::RakNetServerPeer&) {
        opened = true;
    });
    server.onEncapsulated([&](
        const bedrock::RakNetServerPeer&,
        const std::vector<uint8_t>& payload
    ) {
        if (payload == prelude) sawPrelude = true;
        if (payload == largePayload) sawLargePayload = true;
        if (payload == rejectedPayload) acceptedSecondDiscontinuity = true;
    });
    server.listen();

    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(server.boundPort());
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0 || !establishConnectedSession(
            sock,
            target,
            server.boundPort()
        )) {
        if (sock >= 0) close(sock);
        server.close();
        std::cerr << "[SMOKE] reconnect-sequence peer setup failed\n";
        return false;
    }
    for (int attempt = 0; attempt < 100 && !opened.load(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!sendPacket(
            sock,
            target,
            buildReliableDatagram(prelude, 2, 2, 2)
        )) {
        close(sock);
        server.close();
        return false;
    }
    for (int attempt = 0; attempt < 100 && !sawPrelude.load(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    constexpr std::size_t partBytes = 1'200;
    const uint32_t splitCount = static_cast<uint32_t>(
        (largePayload.size() + partBytes - 1) / partBytes
    );
    constexpr uint32_t firstDatagramSequence = 60'003;
    for (uint32_t splitIndex = 0; splitIndex < splitCount; ++splitIndex) {
        const auto beginOffset = static_cast<std::size_t>(splitIndex) *
            partBytes;
        const auto endOffset = std::min(
            largePayload.size(),
            beginOffset + partBytes
        );
        const std::vector<uint8_t> part(
            largePayload.begin() + static_cast<std::ptrdiff_t>(beginOffset),
            largePayload.begin() + static_cast<std::ptrdiff_t>(endOffset)
        );
        auto datagram = buildSplitReliableDatagram(
            part,
            firstDatagramSequence + splitIndex,
            3 + splitIndex,
            3,
            splitCount,
            29,
            splitIndex
        );
        // The real Login burst marks that more datagrams are pending.
        datagram.front() |= 0x08u;
        if (!sendPacket(sock, target, datagram)) {
            close(sock);
            server.close();
            return false;
        }
    }

    for (int attempt = 0; attempt < 300 && !sawLargePayload.load(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (sawLargePayload.load()) {
        auto secondJump = buildReliableDatagram(
            rejectedPayload,
            firstDatagramSequence + splitCount + 60'000,
            3 + splitCount,
            4
        );
        secondJump.front() |= 0x08u;
        if (!sendPacket(sock, target, secondJump)) {
            close(sock);
            server.close();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const bool ok = opened.load() && sawPrelude.load() &&
        sawLargePayload.load() && !acceptedSecondDiscontinuity.load();
    close(sock);
    server.close();
    if (!ok) {
        std::cerr << "[SMOKE] large split payload stayed blocked after an "
                     "early reconnect sequence discontinuity, or a second "
                     "discontinuity was incorrectly accepted\n";
    }
    return ok;
}

struct LifecycleWireObservation {
    int disconnectPackets = 0;
    int playStatusPackets = 0;
    int closePackets = 0;
    int closeReliability = -1;
    bool closeWasSplit = false;
    bool sawFirstReason = false;
    bool sawSecondReason = false;
    std::chrono::milliseconds firstGamePacketAt {-1};
    std::chrono::milliseconds closePacketAt {-1};
    std::vector<std::string> order;
};

bool containsAscii(const std::vector<uint8_t>& bytes, const std::string& value) {
    return std::search(bytes.begin(), bytes.end(), value.begin(), value.end()) != bytes.end();
}

LifecycleWireObservation collectLifecycleWire(
    int sock,
    const sockaddr_in& target,
    const bedrock::VersionedMcpeCodec& codec,
    std::chrono::steady_clock::time_point startedAt,
    int timeoutMs
) {
    LifecycleWireObservation observation;
    const auto deadline = startedAt + std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()
        );
        std::vector<uint8_t> datagram;
        if (!receivePacket(
                sock,
                datagram,
                static_cast<int>(std::min<int64_t>(25, std::max<int64_t>(1, remaining.count())))
            )) {
            continue;
        }

        const auto frames = parseDatagramFrames(datagram);
        if (!frames.empty()) {
            (void) sendPacket(sock, target, buildAck(frames.front().sequence));
        }
        for (const auto& frame : frames) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt
            );
            if (frame.payload == std::vector<uint8_t>{0x15}) {
                ++observation.closePackets;
                observation.closeReliability = frame.reliability;
                observation.closeWasSplit = frame.split;
                if (observation.closePacketAt.count() < 0) {
                    observation.closePacketAt = elapsed;
                }
                observation.order.push_back("raknet_close");
                continue;
            }
            if (frame.payload.empty() || frame.payload[0] != 0xfe) {
                continue;
            }

            try {
                const auto decoded = codec.decodeMcpePayload(frame.payload);
                for (const auto& packet : decoded.batch.packets) {
                    if (observation.firstGamePacketAt.count() < 0) {
                        observation.firstGamePacketAt = elapsed;
                    }
                    observation.order.push_back(packet.name);
                    if (packet.name == "disconnect") {
                        ++observation.disconnectPackets;
                        observation.sawFirstReason = observation.sawFirstReason ||
                            containsAscii(packet.payload, "first shutdown reason");
                        observation.sawSecondReason = observation.sawSecondReason ||
                            containsAscii(packet.payload, "second shutdown reason");
                    } else if (packet.name == "play_status") {
                        ++observation.playStatusPackets;
                    }
                }
            } catch (const std::exception&) {
            }
        }

        if (observation.closePackets > 0) {
            // All MCPE writes precede close in these lifecycle paths.  Keep a
            // short drain window for separately queued datagrams.
            std::vector<uint8_t> trailing;
            if (!receivePacket(sock, trailing, 15)) {
                break;
            }
            const auto trailingFrames = parseDatagramFrames(trailing);
            if (!trailingFrames.empty()) {
                (void) sendPacket(sock, target, buildAck(trailingFrames.front().sequence));
            }
            for (const auto& frame : trailingFrames) {
                if (frame.payload.empty() || frame.payload[0] != 0xfe) {
                    continue;
                }
                try {
                    const auto decoded = codec.decodeMcpePayload(frame.payload);
                    for (const auto& packet : decoded.batch.packets) {
                        observation.order.push_back(packet.name);
                        if (packet.name == "disconnect") {
                            ++observation.disconnectPackets;
                            observation.sawFirstReason = observation.sawFirstReason ||
                                containsAscii(packet.payload, "first shutdown reason");
                            observation.sawSecondReason = observation.sawSecondReason ||
                                containsAscii(packet.payload, "second shutdown reason");
                        } else if (packet.name == "play_status") {
                            ++observation.playStatusPackets;
                        }
                    }
                } catch (const std::exception&) {
                }
            }
            break;
        }
    }

    return observation;
}

struct LifecycleConnectionCapture {
    std::mutex mutex;
    std::vector<bedrock::BedrockServerConnection> connections;
    std::atomic<int> count {0};

    void store(const bedrock::BedrockServerConnection& value) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            connections.push_back(value);
        }
        ++count;
    }

    std::optional<bedrock::BedrockServerConnection> load(std::size_t index = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        if (index >= connections.size()) {
            return std::nullopt;
        }
        return connections[index];
    }
};

bool waitForConnection(LifecycleConnectionCapture& capture, int expected = 1) {
    for (int attempt = 0; attempt < 100 && capture.count.load() < expected; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return capture.count.load() >= expected &&
        capture.load(static_cast<std::size_t>(expected - 1)).has_value();
}

int openLifecycleSocket(
    sockaddr_in& target,
    uint16_t port,
    uint64_t clientGuid = 0
) {
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return -1;
    }
    target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
    if (!establishConnectedSession(sock, target, port, clientGuid)) {
        close(sock);
        return -1;
    }
    return sock;
}

bool checkEncryptedReceiveVsDelayedClose() {
    const std::vector<uint8_t> key(32, 0x31);
    const std::vector<uint8_t> iv(16, 0x42);
    LifecycleConnectionCapture capture;
    std::atomic<bool> encryptionReady {false};
    std::atomic<int> closeCalls {0};

    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.20.61",
        .motd = {{"motd", "Encrypted close race"}},
        .offline = true
    });
    server.onConnect([&](const bedrock::BedrockServerConnection& connection) {
        encryptionReady = bedrock::BedrockServerTestAccess::enableEncryption(
            server, connection, key, iv
        );
        connection.onClose([&]() {
            ++closeCalls;
        });
        capture.store(connection);
    });
    server.listen();

    sockaddr_in target {};
    const int sock = openLifecycleSocket(target, server.boundPort());
    if (sock < 0 || !waitForConnection(capture) || !encryptionReady.load()) {
        const auto capturedCount = capture.count.load();
        const auto ready = encryptionReady.load();
        if (sock >= 0) close(sock);
        server.close();
        std::cerr << "[SMOKE] encrypted delayed-close peer setup failed: sock="
                  << sock << " capture=" << capturedCount
                  << " encryption=" << ready << "\n";
        return false;
    }
    const auto connection = *capture.load();

    const auto codec = bedrock::VersionedMcpeCodec::forVersion("1.20.61");
    const auto tick = codec.packetCodec().makePacketByName(
        "tick_sync",
        std::vector<uint8_t>(16, 0)
    );
    const auto compressionPacket = codec.encodeEncryptedCompressionPacket({tick}, 7);
    auto cipher = bedrock::BedrockEncryption::createCipherStream(
        bedrock::protocolVersionFor("1.20.61"),
        key,
        iv,
        bedrock::BedrockCipherMode::Encrypt
    );

    std::thread sender([&]() {
        for (uint32_t index = 0; index < 180; ++index) {
            const auto plaintext = bedrock::BedrockEncryption::makeAesPlaintext(
                compressionPacket,
                index,
                key
            );
            auto encrypted = cipher->process(plaintext);
            encrypted.insert(encrypted.begin(), 0xfe);
            (void) sendPacket(
                sock,
                target,
                buildReliableDatagram(
                    encrypted,
                    index + 2,
                    index + 2,
                    index + 2
                )
            );
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    server.disconnect(connection, "delayed close stress");
    sender.join();
    for (int attempt = 0; attempt < 50 && closeCalls.load() == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto actualCloseCalls = closeCalls.load();
    const auto actualClientCount = server.clientCount();
    const auto actualPlayerState =
        bedrock::BedrockServerTestAccess::hasPlayerState(server, connection);
    const auto actualListeners =
        bedrock::BedrockServerTestAccess::listenerCounts(connection);
    const bool ok = actualCloseCalls == 1 && actualClientCount == 0 &&
        !actualPlayerState && actualListeners ==
            std::pair<std::size_t, std::size_t>{0, 0};
    close(sock);
    server.close();
    if (!ok) {
        std::cerr << "[SMOKE] encrypted receive/delayed-close serialization mismatch: close="
                  << actualCloseCalls << " clients=" << actualClientCount
                  << " state=" << actualPlayerState
                  << " listeners=" << actualListeners.first << '/'
                  << actualListeners.second
                  << "\n";
        return false;
    }
    return true;
}

bool checkDestroyWhileReceiving() {
    std::atomic<int> connects {0};
    auto server = std::make_unique<bedrock::BedrockServer>(
        bedrock::BedrockServerOptions{
            .host = "127.0.0.1",
            .port = 0,
            .version = "1.20.61",
            .motd = {{"motd", "Destructor traffic"}},
            .offline = true
        }
    );
    server->onConnect([&](const bedrock::BedrockServerConnection&) {
        ++connects;
    });
    server->listen();

    sockaddr_in target {};
    const int sock = openLifecycleSocket(target, server->boundPort());
    for (int attempt = 0; attempt < 100 && connects.load() == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (sock < 0 || connects.load() != 1) {
        if (sock >= 0) close(sock);
        server.reset();
        std::cerr << "[SMOKE] destructor traffic peer setup failed\n";
        return false;
    }

    std::atomic<bool> stop {false};
    std::thread sender([&]() {
        uint32_t index = 2;
        while (!stop.load()) {
            (void) sendPacket(
                sock,
                target,
                buildReliableDatagram({0xfe}, index, index, index)
            );
            ++index;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    server.reset();
    stop = true;
    sender.join();
    close(sock);
    return true;
}

bool checkPlayerDisconnectLifecycle() {
    LifecycleConnectionCapture capture;
    std::atomic<int> closeEvents {0};
    std::atomic<bool> callbackOrderOk {false};
    std::atomic<int> callbackClientCount {-1};
    std::atomic<int> callbackStatus {-1};
    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.20.40",
        .motd = {{"motd", "Disconnect lifecycle"}},
        .offline = true
    });
    server.onConnect([&](const bedrock::BedrockServerConnection& connection) {
        capture.store(connection);
    });
    server.onDisconnect([&](const bedrock::BedrockServerConnection& connection) {
        callbackClientCount = server.clientCount();
        callbackStatus = static_cast<int>(server.status(connection));
        callbackOrderOk = callbackClientCount.load() == 1 &&
            callbackStatus.load() == static_cast<int>(
                bedrock::BedrockServerClientStatus::Authenticating
            );
        ++closeEvents;
    });
    server.listen();

    sockaddr_in target {};
    const int sock = openLifecycleSocket(target, server.boundPort());
    if (sock < 0 || !waitForConnection(capture) || server.clientCount() != 1) {
        if (sock >= 0) close(sock);
        server.close();
        std::cerr << "[SMOKE] failed to establish disconnect lifecycle peer\n";
        return false;
    }
    const auto connection = *capture.load();

    // Cached native advertisement stays unchanged until serverTimer's 1s tick.
    const auto earlyPong = bedrock::RakNetPinger::ping(
        "127.0.0.1", server.boundPort(), 500
    );
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    const auto updatedPong = bedrock::RakNetPinger::ping(
        "127.0.0.1", server.boundPort(), 500
    );
    if (!earlyPong.ok || earlyPong.onlinePlayers != 0 ||
        !updatedPong.ok || updatedPong.onlinePlayers != 1 ||
        earlyPong.serverId != updatedPong.serverId) {
        close(sock);
        server.close();
        std::cerr << "[SMOKE] 1000ms cached playersOnline update mismatch\n";
        return false;
    }

    const auto startedAt = std::chrono::steady_clock::now();
    server.disconnect(connection, "first shutdown reason");
    server.disconnect(connection, "second shutdown reason");
    const auto observation = collectLifecycleWire(
        sock,
        target,
        bedrock::VersionedMcpeCodec::forVersion("1.20.40"),
        startedAt,
        500
    );

    const bool orderOk = observation.order.size() >= 3 &&
        observation.order[0] == "disconnect" &&
        observation.order[1] == "disconnect" &&
        observation.order[2] == "raknet_close";
    const bool ok = observation.disconnectPackets == 2 &&
        observation.sawFirstReason && observation.sawSecondReason &&
        observation.closePackets == 1 &&
        observation.closeReliability == 3 && !observation.closeWasSplit &&
        observation.closePacketAt.count() >= 80 &&
        observation.firstGamePacketAt < observation.closePacketAt &&
        orderOk && closeEvents.load() == 1 && callbackOrderOk.load() &&
        server.clientCount() == 0 &&
        server.status(connection) == bedrock::BedrockServerClientStatus::Disconnected;

    // Once the first timer has completed Player.disconnect is guarded by the
    // Disconnected status and cannot write another packet.
    server.disconnect(connection, "after close");
    std::vector<uint8_t> unexpected;
    const bool sentAfterClose = receivePacket(sock, unexpected, 35);

    // onCloseConnection only changes clientCount.  The already-cached PONG
    // still says one player until the next interval, with the same serverId.
    const auto cachedAfterClose = bedrock::RakNetPinger::ping(
        "127.0.0.1", server.boundPort(), 500
    );
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    const auto updatedAfterClose = bedrock::RakNetPinger::ping(
        "127.0.0.1", server.boundPort(), 500
    );
    const bool advertisementCloseTickOk = cachedAfterClose.ok &&
        cachedAfterClose.onlinePlayers == 1 && updatedAfterClose.ok &&
        updatedAfterClose.onlinePlayers == 0 &&
        cachedAfterClose.serverId == earlyPong.serverId &&
        updatedAfterClose.serverId == earlyPong.serverId;

    close(sock);
    server.close();
    if (!ok || sentAfterClose || !advertisementCloseTickOk) {
        std::cerr << "[SMOKE] Player.disconnect 100ms/repeat lifecycle mismatch\n";
        return false;
    }
    return true;
}

bool checkDisconnectStatusLifecycle() {
    LifecycleConnectionCapture capture;
    std::atomic<int> closeEvents {0};
    std::atomic<bool> callbackOrderOk {false};
    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.20.40",
        .motd = {{"motd", "Disconnect status lifecycle"}},
        .offline = true
    });
    server.onConnect([&](const bedrock::BedrockServerConnection& connection) {
        capture.store(connection);
    });
    server.onDisconnect([&](const bedrock::BedrockServerConnection& connection) {
        callbackOrderOk = server.clientCount() == 1 &&
            server.status(connection) == bedrock::BedrockServerClientStatus::Authenticating;
        ++closeEvents;
    });
    server.listen();

    sockaddr_in target {};
    const int sock = openLifecycleSocket(target, server.boundPort());
    if (sock < 0 || !waitForConnection(capture)) {
        if (sock >= 0) close(sock);
        server.close();
        return false;
    }
    const auto connection = *capture.load();
    const auto startedAt = std::chrono::steady_clock::now();
    server.sendDisconnectStatus(connection, "failed_client");

    // Player.close is synchronous: state and close event are complete before
    // sendDisconnectStatus returns, and a repeat cannot write another status.
    const bool synchronousClose = closeEvents.load() == 1 &&
        callbackOrderOk.load() && server.clientCount() == 0 &&
        server.status(connection) == bedrock::BedrockServerClientStatus::Disconnected;
    server.sendDisconnectStatus(connection, "failed_client");

    const auto observation = collectLifecycleWire(
        sock,
        target,
        bedrock::VersionedMcpeCodec::forVersion("1.20.40"),
        startedAt,
        250
    );
    const bool orderOk = observation.order.size() >= 2 &&
        observation.order[0] == "play_status" &&
        observation.order[1] == "raknet_close";
    const bool ok = synchronousClose && observation.playStatusPackets == 1 &&
        observation.closePackets == 1 && observation.closeReliability == 3 &&
        !observation.closeWasSplit && observation.closePacketAt.count() < 80 &&
        orderOk;

    close(sock);
    server.close();
    if (!ok) {
        std::cerr << "[SMOKE] sendDisconnectStatus synchronous/repeat mismatch: sync="
                  << synchronousClose << " play="
                  << observation.playStatusPackets << " close="
                  << observation.closePackets << " reliability="
                  << static_cast<int>(observation.closeReliability)
                  << " split=" << observation.closeWasSplit << " delay="
                  << observation.closePacketAt.count() << " order=";
        for (const auto& item : observation.order) std::cerr << item << ',';
        std::cerr << "\n";
        return false;
    }
    return true;
}

bool checkIncomingCloseLifecycle(uint8_t closeId) {
    LifecycleConnectionCapture capture;
    std::atomic<int> closeEvents {0};
    std::atomic<bool> callbackOrderOk {false};
    std::atomic<int> callbackClientCount {-1};
    std::atomic<int> callbackStatus {-1};
    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.20.40",
        .motd = {{"motd", "Incoming close lifecycle"}},
        .offline = true
    });
    server.onConnect([&](const bedrock::BedrockServerConnection& connection) {
        capture.store(connection);
    });
    server.onDisconnect([&](const bedrock::BedrockServerConnection& connection) {
        callbackClientCount = server.clientCount();
        callbackStatus = static_cast<int>(server.status(connection));
        callbackOrderOk = callbackClientCount.load() == 1 &&
            callbackStatus.load() == static_cast<int>(
                bedrock::BedrockServerClientStatus::Authenticating
            );
        ++closeEvents;
    });
    server.listen();

    sockaddr_in target {};
    const int sock = openLifecycleSocket(target, server.boundPort());
    if (sock < 0 || !waitForConnection(capture)) {
        if (sock >= 0) close(sock);
        server.close();
        return false;
    }
    const auto connection = *capture.load();
    if (closeId == 0x16) {
        // ID_CONNECTION_LOST is generated locally by RakPeer after its
        // timeout/retry state machine; a remote peer cannot forge it as an
        // application frame. Exercise the same native callback boundary
        // directly without waiting 30 seconds for the production timeout.
        bedrock::BedrockServerTestAccess::handleNativeClose(
            server,
            connection,
            closeId
        );
    } else {
        if (!sendPacket(
                sock,
                target,
                buildReliableDatagram({closeId}, 2, 2, 2)
            ) ||
            !waitForPacketId(sock, 0xc0)) {
            close(sock);
            server.close();
            return false;
        }
    }
    // The close callback intentionally runs before the peer table/count is
    // erased. Wait for both sides of that observable ordering instead of
    // racing the worker immediately after closeEvents becomes visible.
    for (int attempt = 0;
         attempt < 100 &&
         (closeEvents.load() == 0 || server.clientCount() != 0);
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const bool ok = closeEvents.load() == 1 && callbackOrderOk.load() &&
        server.clientCount() == 0 &&
        server.status(connection) == bedrock::BedrockServerClientStatus::Disconnected;
    close(sock);
    server.close();
    if (!ok) {
        std::cerr << "[SMOKE] incoming RakNet close id=0x" << std::hex
                  << static_cast<int>(closeId) << std::dec << " mismatch: events="
                  << closeEvents.load() << " order=" << callbackOrderOk.load()
                  << " callback_clients=" << callbackClientCount.load()
                  << " callback_status=" << callbackStatus.load()
                  << " clients=" << server.clientCount() << " status="
                  << static_cast<int>(server.status(connection)) << "\n";
        return false;
    }
    return true;
}

bool checkServerCloseLifecycle() {
    LifecycleConnectionCapture capture;
    std::atomic<int> closeEvents {0};
    std::atomic<bool> closeEntered {false};
    std::atomic<bool> closeReturned {false};
    std::atomic<bool> playerCloseOrderOk {true};
    std::atomic<int64_t> earliestPlayerCloseMs {10000};
    std::chrono::steady_clock::time_point startedAt;
    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.20.40",
        .motd = {{"motd", "Server close lifecycle"}},
        .offline = true
    });
    server.onConnect([&](const bedrock::BedrockServerConnection& connection) {
        capture.store(connection);
    });
    server.onDisconnect([&](const bedrock::BedrockServerConnection& connection) {
        // Native RakPeer::Shutdown(300) is blocking. The logical client table
        // is already empty, while the 100 ms Player close callback may run on
        // the lifecycle scheduler before the external close() call returns.
        playerCloseOrderOk = playerCloseOrderOk.load() &&
            server.clientCount() == 0 &&
            server.status(connection) == bedrock::BedrockServerClientStatus::Authenticating;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt
        ).count();
        auto earliest = earliestPlayerCloseMs.load();
        while (elapsed < earliest &&
               !earliestPlayerCloseMs.compare_exchange_weak(earliest, elapsed)) {
        }
        ++closeEvents;
    });
    server.listen();

    sockaddr_in firstTarget {};
    sockaddr_in secondTarget {};
    const int firstSock = openLifecycleSocket(firstTarget, server.boundPort());
    const int secondSock = openLifecycleSocket(secondTarget, server.boundPort());
    if (firstSock < 0 || secondSock < 0 || !waitForConnection(capture, 2) ||
        server.clientCount() != 2) {
        if (firstSock >= 0) close(firstSock);
        if (secondSock >= 0) close(secondSock);
        server.close();
        return false;
    }

    std::chrono::milliseconds closeDuration {-1};
    std::thread closer([&]() {
        startedAt = std::chrono::steady_clock::now();
        closeEntered = true;
        const auto before = startedAt;
        server.close("first shutdown reason");
        closeDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - before
        );
        closeReturned = true;
    });

    while (!closeEntered.load()) {
        std::this_thread::yield();
    }

    std::chrono::milliseconds logicalResetAt {-1};
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (server.clientCount() == 0) {
            logicalResetAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt
            );
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool transportStillPumping = server.listening();
    const auto firstObservation = collectLifecycleWire(
        firstSock,
        firstTarget,
        bedrock::VersionedMcpeCodec::forVersion("1.20.40"),
        startedAt,
        350
    );
    const auto secondObservation = collectLifecycleWire(
        secondSock,
        secondTarget,
        bedrock::VersionedMcpeCodec::forVersion("1.20.40"),
        startedAt,
        350
    );
    closer.join();

    const auto wireOk = [](const LifecycleWireObservation& observation) {
        const auto close = std::find(
            observation.order.begin(),
            observation.order.end(),
            "raknet_close"
        );
        return observation.disconnectPackets >= 1 &&
            observation.sawFirstReason && observation.closePackets == 1 &&
            observation.closeReliability == 3 && !observation.closeWasSplit &&
            observation.closePacketAt.count() >= 45 &&
            observation.firstGamePacketAt <= observation.closePacketAt &&
            close != observation.order.end() &&
            !observation.order.empty() &&
            observation.order[0] == "disconnect";
    };
    for (int attempt = 0; attempt < 100 && closeEvents.load() < 2; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    bool ok = logicalResetAt.count() >= 0 && logicalResetAt.count() < 45 &&
        transportStillPumping && wireOk(firstObservation) &&
        wireOk(secondObservation) &&
        closeDuration.count() >= 50 && closeDuration.count() < 1000 &&
        !server.listening() && server.clientCount() == 0 &&
        closeEvents.load() == 2 && playerCloseOrderOk.load() &&
        earliestPlayerCloseMs.load() >= 80;

    const auto repeatedAt = std::chrono::steady_clock::now();
    server.close("second shutdown reason");
    const auto repeatedDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - repeatedAt
    );
    ok = ok && repeatedDuration.count() >= 50 && closeEvents.load() == 2;

    close(firstSock);
    close(secondSock);
    if (!ok) {
        const auto printWire = [](const char* label, const LifecycleWireObservation& observation) {
            std::cerr << " " << label
                      << " disconnect=" << observation.disconnectPackets
                      << " reason=" << observation.sawFirstReason
                      << " close=" << observation.closePackets
                      << " reliability=" << observation.closeReliability
                      << " split=" << observation.closeWasSplit
                      << " gameAt=" << observation.firstGamePacketAt.count()
                      << " closeAt=" << observation.closePacketAt.count();
        };
        std::cerr << "[SMOKE] Server.close logical reset/60ms/repeat mismatch:"
                  << " resetAt=" << logicalResetAt.count()
                  << " pumping=" << transportStillPumping
                  << " duration=" << closeDuration.count()
                  << " repeated=" << repeatedDuration.count()
                  << " listening=" << server.listening()
                  << " count=" << server.clientCount()
                  << " events=" << closeEvents.load()
                  << " playerAt=" << earliestPlayerCloseMs.load()
                  << " playerOrder=" << playerCloseOrderOk.load();
        printWire("first", firstObservation);
        printWire("second", secondObservation);
        std::cerr << '\n';
        return false;
    }
    return true;
}

bool checkSelfThreadCloseLifecycle() {
    std::atomic<bool> callbackReturned {false};
    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.20.40",
        .motd = {{"motd", "Self close lifecycle"}},
        .offline = true
    });
    server.onConnect([&](const bedrock::BedrockServerConnection&) {
        server.close("callback close");
        callbackReturned = true;
    });
    server.listen();

    sockaddr_in target {};
    const int sock = openLifecycleSocket(target, server.boundPort());
    if (sock < 0) {
        server.close();
        return false;
    }
    for (int attempt = 0; attempt < 100 && !callbackReturned.load(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (int attempt = 0; attempt < 100 && server.listening(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const bool ok = callbackReturned.load() && !server.listening() &&
        server.clientCount() == 0;
    close(sock);
    // Reaps the worker that deliberately could not join itself, and also
    // verifies the repeated Server.close 60 ms path.
    server.close();
    if (!ok) {
        std::cerr << "[SMOKE] callback Server.close self-join lifecycle mismatch\n";
        return false;
    }
    return true;
}

bool checkShutdownLifecycles() {
    if (!checkPlayerDisconnectLifecycle()) {
        std::cerr << "[SMOKE] player disconnect lifecycle failed\n";
        return false;
    }
    if (!checkDisconnectStatusLifecycle()) {
        std::cerr << "[SMOKE] disconnect status lifecycle failed\n";
        return false;
    }
    if (!checkIncomingCloseLifecycle(0x15)) return false;
    if (!checkIncomingCloseLifecycle(0x16)) return false;
    if (!checkServerCloseLifecycle()) {
        std::cerr << "[SMOKE] server close lifecycle failed\n";
        return false;
    }
    if (!checkSelfThreadCloseLifecycle()) {
        std::cerr << "[SMOKE] self-thread close lifecycle failed\n";
        return false;
    }
    return true;
}

bool checkConnectedRequestAccepted(uint16_t port) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return false;
    }

    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);

    const bool connected = establishConnectedSession(sock, target, port);
    close(sock);
    return connected;
}

bool checkNetworkSettingsResponse(uint16_t port) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return false;
    }

    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);

    if (!establishConnectedSession(sock, target, port)) {
        close(sock);
        return false;
    }

    auto codec = bedrock::VersionedMcpeCodec::forVersion("1.20.61");
    auto request = codec.packetCodec().makePacketByName(
        "request_network_settings",
        {0x00, 0x00, 0x02, 0x89}
    );
    auto framed = codec.batchCodec().encodeFramedBatch({request});
    std::vector<uint8_t> mcpe {0xfe};
    mcpe.insert(mcpe.end(), framed.begin(), framed.end());
    auto splitAt = mcpe.size() / 2;
    std::vector<uint8_t> part0(mcpe.begin(), mcpe.begin() + static_cast<std::ptrdiff_t>(splitAt));
    std::vector<uint8_t> part1(mcpe.begin() + static_cast<std::ptrdiff_t>(splitAt), mcpe.end());
    auto datagram0 = buildSplitReliableDatagram(part0, 2, 2, 2, 2, 7, 0);
    // Every fragment of one ordered split packet shares orderedIndex.
    auto datagram1 = buildSplitReliableDatagram(part1, 3, 3, 2, 2, 7, 1);

    sendto(
        sock,
        datagram0.data(),
        datagram0.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    );
    sendto(
        sock,
        datagram1.data(),
        datagram1.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    );

    for (int attempt = 0; attempt < 10; ++attempt) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        timeval timeout {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        int ready = select(sock + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        std::vector<uint8_t> reply(8192);
        ssize_t received = recvfrom(sock, reply.data(), reply.size(), 0, nullptr, nullptr);
        if (received <= 0) continue;
        reply.resize(static_cast<std::size_t>(received));
        acknowledgeDatagram(sock, target, reply);

        for (const auto& payload : parseDatagramPayloads(reply)) {
            if (payload.empty() || payload[0] != 0xfe) continue;

            auto decoded = codec.decodeUncompressedMcpePayload(payload);
            for (const auto& packet : decoded.batch.packets) {
                if (packet.name == "network_settings") {
                    close(sock);
                    return true;
                }
            }
        }
    }

    close(sock);
    return false;
}

uint64_t transportDiagnosticHash(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 1469598103934665603ull;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool checkProtocol827NetworkSettingsDelivery() {
    std::atomic<bool> sendPacketIdentity {false};
    std::atomic<bool> acceptedBatchIdentity {false};
    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.21.100",
        .motd = {{"motd", "Protocol 827 network settings"}},
        .offline = true
    });
    server.onTransport([&](
        const bedrock::BedrockServerTransportEvent& event
    ) {
        if (event.kind ==
                bedrock::BedrockServerTransportEventKind::SendPacket &&
            event.packetName == "network_settings") {
            sendPacketIdentity = event.byteLength == 12 &&
                event.byteHash == 0x1853c5e83e41e97dull;
        }
        if (event.kind ==
                bedrock::BedrockServerTransportEventKind::SendBatch &&
            event.byteLength == 14 &&
            event.byteHash == 0x8742a95bb29ed2e3ull &&
            event.message.find("pre_compression=true") != std::string::npos &&
            event.message.find("priority=immediate") != std::string::npos &&
            event.message.find("raknet_status=accepted") != std::string::npos &&
            event.message.find("raknet_receipt=0") == std::string::npos &&
            event.message.find("connection_state=2") != std::string::npos) {
            acceptedBatchIdentity = true;
        }
    });
    server.listen();

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        server.close();
        return false;
    }
    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(server.boundPort());
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
    if (!establishConnectedSession(sock, target, server.boundPort())) {
        close(sock);
        server.close();
        return false;
    }

    const auto codec =
        bedrock::VersionedMcpeCodec::forVersion("1.21.100");
    const uint32_t protocol = codec.definition().protocolVersion();
    const auto request = codec.packetCodec().makePacketByName(
        "request_network_settings",
        {
            static_cast<uint8_t>((protocol >> 24u) & 0xffu),
            static_cast<uint8_t>((protocol >> 16u) & 0xffu),
            static_cast<uint8_t>((protocol >> 8u) & 0xffu),
            static_cast<uint8_t>(protocol & 0xffu)
        }
    );
    const auto framed = codec.batchCodec().encodeFramedBatch({request});
    std::vector<uint8_t> mcpe {0xfe};
    mcpe.insert(mcpe.end(), framed.begin(), framed.end());
    const auto splitAt = mcpe.size() / 2;
    const std::vector<uint8_t> part0(
        mcpe.begin(),
        mcpe.begin() + static_cast<std::ptrdiff_t>(splitAt)
    );
    const std::vector<uint8_t> part1(
        mcpe.begin() + static_cast<std::ptrdiff_t>(splitAt),
        mcpe.end()
    );
    const auto datagram0 = buildSplitReliableDatagram(
        part0,
        2,
        2,
        2,
        2,
        17,
        0
    );
    const auto datagram1 = buildSplitReliableDatagram(
        part1,
        3,
        3,
        2,
        2,
        17,
        1
    );
    (void) sendto(
        sock,
        datagram0.data(),
        datagram0.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    );
    (void) sendto(
        sock,
        datagram1.data(),
        datagram1.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    );

    bool wireBatchIdentity = false;
    bool wirePacketIdentity = false;
    for (int attempt = 0; attempt < 15 && !wirePacketIdentity; ++attempt) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        timeval timeout {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        const int ready = select(
            sock + 1,
            &readfds,
            nullptr,
            nullptr,
            &timeout
        );
        if (ready <= 0) continue;

        std::vector<uint8_t> reply(8192);
        const ssize_t received = recvfrom(
            sock,
            reply.data(),
            reply.size(),
            0,
            nullptr,
            nullptr
        );
        if (received <= 0) continue;
        reply.resize(static_cast<std::size_t>(received));
        acknowledgeDatagram(sock, target, reply);

        for (const auto& payload : parseDatagramPayloads(reply)) {
            if (payload.empty() || payload.front() != 0xfe) continue;
            wireBatchIdentity = payload.size() == 14 &&
                transportDiagnosticHash(payload) == 0x8742a95bb29ed2e3ull;
            const auto decoded = codec.decodeUncompressedMcpePayload(payload);
            for (const auto& packet : decoded.batch.packets) {
                if (packet.name != "network_settings") continue;
                wirePacketIdentity = packet.fullPacket.size() == 12 &&
                    transportDiagnosticHash(packet.fullPacket) ==
                        0x1853c5e83e41e97dull;
            }
        }
    }

    close(sock);
    server.close();
    const bool ok = protocol == 827 && wireBatchIdentity &&
        wirePacketIdentity && sendPacketIdentity.load() &&
        acceptedBatchIdentity.load();
    if (!ok) {
        std::cerr << "[SMOKE] protocol-827 network_settings was not accepted "
                     "and delivered with byte identity\n";
    }
    return ok;
}

bool checkLoginHandshakeResponse(
    uint16_t port,
    bool& sawOutboundSplit,
    bool& sawCompressedText
) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return false;
    }

    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);

    if (!establishConnectedSession(sock, target, port)) {
        close(sock);
        return false;
    }

    auto codec = bedrock::VersionedMcpeCodec::forVersion("1.20.61");

    // Modern protocol negotiation is raw framed MCPE on both directions;
    // compressor mode bytes become mandatory only after network_settings.
    auto settingsRequest = codec.packetCodec().makePacketByName(
        "request_network_settings",
        {0x00, 0x00, 0x02, 0x89}
    );
    auto settingsFramed = codec.batchCodec().encodeFramedBatch({settingsRequest});
    std::vector<uint8_t> settingsMcpe {0xfe};
    settingsMcpe.insert(settingsMcpe.end(), settingsFramed.begin(), settingsFramed.end());
    auto settingsDatagram = buildReliableDatagram(settingsMcpe, 2, 2, 2);
    sendto(
        sock,
        settingsDatagram.data(),
        settingsDatagram.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    );

    bool receivedNetworkSettings = false;
    for (int attempt = 0; attempt < 10 && !receivedNetworkSettings; ++attempt) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        timeval timeout {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        int ready = select(sock + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        std::vector<uint8_t> reply(8192);
        ssize_t received = recvfrom(sock, reply.data(), reply.size(), 0, nullptr, nullptr);
        if (received <= 0) continue;
        reply.resize(static_cast<std::size_t>(received));
        acknowledgeDatagram(sock, target, reply);

        for (const auto& payload : parseDatagramPayloads(reply)) {
            if (payload.empty() || payload[0] != 0xfe) continue;
            auto decoded = codec.decodeUncompressedMcpePayload(payload);
            for (const auto& packet : decoded.batch.packets) {
                if (packet.name == "network_settings") {
                    receivedNetworkSettings = true;
                }
            }
        }
    }
    if (!receivedNetworkSettings) {
        close(sock);
        return false;
    }

    // A normal packet is ignored while the server-side Player is still in
    // Authenticating, exactly like serverPlayer.js's default switch branch.
    auto ignoredBeforeLogin = codec.packetCodec().makePacketByName(
        "tick_sync",
        std::vector<uint8_t>(16, 0)
    );
    auto ignoredMcpe = codec.encodeMcpePayload(
        {ignoredBeforeLogin},
        bedrock::VersionedMcpeCompression::Uncompressed
    );
    auto ignoredDatagram = buildReliableDatagram(ignoredMcpe, 3, 3, 3);
    sendto(
        sock,
        ignoredDatagram.data(),
        ignoredDatagram.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    );

    auto keys = bedrock::BedrockAuthJwt::generateP384KeyPair();
    const std::string identityPayload =
        "{\"extraData\":{\"displayName\":\"Smoke\",\"identity\":\"00000000-0000-0000-0000-000000000000\",\"XUID\":\"0\"},"
        "\"certificateAuthority\":true,"
        "\"identityPublicKey\":\"" + keys.publicKeyDerBase64 + "\"}";
    auto identityToken = bedrock::BedrockAuthJwt::signEs384Jwt(
        keys.privateKeyPem,
        keys.publicKeyDerBase64,
        identityPayload
    );
    const std::string identityJson = "{\"chain\":[\"" + identityToken + "\"]}";

    const std::string clientPayload = "{\"GameVersion\":\"1.20.61\",\"ThirdPartyName\":\"Smoke\"}";
    auto clientToken = bedrock::BedrockAuthJwt::signEs384Jwt(
        keys.privateKeyPem,
        keys.publicKeyDerBase64,
        clientPayload
    );

    auto loginFullPacket = bedrock::LoginPacketCodec::encode(649, identityJson, clientToken);
    auto loginPacket = codec.packetCodec().decodeFullPacket(loginFullPacket);
    auto mcpe = codec.encodeMcpePayload({loginPacket}, bedrock::VersionedMcpeCompression::Uncompressed);
    auto datagram = buildReliableDatagram(mcpe, 4, 4, 4);

    sendto(
        sock,
        datagram.data(),
        datagram.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    );

    std::string serverHandshakeToken;
    for (int attempt = 0; attempt < 10 && serverHandshakeToken.empty(); ++attempt) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        timeval timeout {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        int ready = select(sock + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        std::vector<uint8_t> reply(8192);
        ssize_t received = recvfrom(sock, reply.data(), reply.size(), 0, nullptr, nullptr);
        if (received <= 0) continue;
        reply.resize(static_cast<std::size_t>(received));
        acknowledgeDatagram(sock, target, reply);

        for (const auto& payload : parseDatagramPayloads(reply)) {
            if (payload.empty() || payload[0] != 0xfe) continue;

            auto decoded = codec.decodeMcpePayload(payload);
            for (const auto& packet : decoded.batch.packets) {
                if (packet.name == "server_to_client_handshake") {
                    std::size_t offset = 0;
                    uint32_t tokenLen = readVarUInt(packet.payload, offset);
                    if (offset + tokenLen <= packet.payload.size()) {
                        serverHandshakeToken.assign(
                            reinterpret_cast<const char*>(packet.payload.data() + offset),
                            tokenLen
                        );
                    }
                }
            }
        }
    }

    if (serverHandshakeToken.empty()) {
        close(sock);
        return false;
    }

    auto derived = bedrock::BedrockKeyExchange::deriveFromServerHandshakeJwtAndPrivateKeyPem(
        serverHandshakeToken,
        keys.privateKeyPem
    );

    bedrock::BedrockAesGcmStream clientEncrypt(
        derived.secretKeyBytes,
        derived.iv16,
        bedrock::BedrockAesGcmStream::Mode::Encrypt
    );
    bedrock::BedrockAesGcmStream clientDecrypt(
        derived.secretKeyBytes,
        derived.iv16,
        bedrock::BedrockAesGcmStream::Mode::Decrypt
    );
    uint64_t clientSendCounter = 0;
    uint64_t clientReceiveCounter = 0;

    const auto encryptPackets = [&](const std::vector<bedrock::VersionedGamePacket>& packets) {
        auto compression = codec.encodeEncryptedCompressionPacket(packets, 7);
        auto plaintext = bedrock::BedrockEncryption::makeAesPlaintext(
            compression,
            clientSendCounter++,
            derived.secretKeyBytes
        );
        auto encryptedOnly = clientEncrypt.process(plaintext);
        std::vector<uint8_t> result;
        result.reserve(1 + encryptedOnly.size());
        result.push_back(0xfe);
        result.insert(result.end(), encryptedOnly.begin(), encryptedOnly.end());
        return result;
    };

    auto handshake = codec.packetCodec().makePacketByName("client_to_server_handshake", {});
    // Repeated handshake packets repeat status/join in serverPlayer.js; they
    // are deliberately batched together here to lock that behavior down.
    auto encrypted = encryptPackets({handshake, handshake});
    auto encryptedDatagram = buildReliableDatagram(encrypted, 5, 5, 5);

    sendto(
        sock,
        encryptedDatagram.data(),
        encryptedDatagram.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    );

    int loginSuccessPackets = 0;
    bool sawResourcePacksInfo = false;
    int quietPollsAfterPlayStatus = 0;
    for (int attempt = 0; attempt < 20 && quietPollsAfterPlayStatus < 2; ++attempt) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        timeval timeout {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        int ready = select(sock + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            if (loginSuccessPackets >= 2) {
                ++quietPollsAfterPlayStatus;
            }
            continue;
        }

        std::vector<uint8_t> reply(8192);
        ssize_t received = recvfrom(sock, reply.data(), reply.size(), 0, nullptr, nullptr);
        if (received <= 0) continue;
        reply.resize(static_cast<std::size_t>(received));
        acknowledgeDatagram(sock, target, reply);

        for (const auto& payload : parseDatagramPayloads(reply)) {
            if (payload.empty() || payload[0] != 0xfe) continue;

            std::vector<uint8_t> encryptedOnly(payload.begin() + 1, payload.end());
            auto aesPlaintext = clientDecrypt.process(encryptedOnly);
            if (aesPlaintext.size() < 8) {
                close(sock);
                return false;
            }
            std::vector<uint8_t> plaintext(aesPlaintext.begin(), aesPlaintext.end() - 8);
            std::vector<uint8_t> checksum(aesPlaintext.end() - 8, aesPlaintext.end());
            auto expectedChecksum = bedrock::BedrockEncryption::computeChecksum(
                plaintext,
                clientReceiveCounter++,
                derived.secretKeyBytes
            );
            if (checksum != expectedChecksum) {
                close(sock);
                return false;
            }
            auto decoded = codec.decodeCompressionPacket(plaintext);
            for (const auto& packet : decoded.batch.packets) {
                if (packet.name == "play_status" &&
                    packet.fullPacket == std::vector<uint8_t>({0x02, 0x00, 0x00, 0x00, 0x00})) {
                    // This tiny packet is below the configured threshold, but
                    // encryption.js still raw-deflates every encrypted batch.
                    if (decoded.compressionHeader != static_cast<uint8_t>(
                            bedrock::VersionedMcpeCompression::DeflateRaw
                        )) {
                        close(sock);
                        return false;
                    }
                    ++loginSuccessPackets;
                }
                if (packet.name == "resource_packs_info") {
                    sawResourcePacksInfo = true;
                }
            }
        }
    }

    if (loginSuccessPackets != 2 || sawResourcePacksInfo) {
        close(sock);
        return false;
    }

    auto initialized = codec.packetCodec().makePacketByName(
        "set_local_player_as_initialized",
        {0x95, 0x9a, 0xef, 0x3a}
    );
    auto initializedDatagram = buildReliableDatagram(
        encryptPackets({initialized}),
        6,
        6,
        6
    );
    sendto(
        sock,
        initializedDatagram.data(),
        initializedDatagram.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    );

    // onSpawn sends 5000 repeated characters. The JavaScript Framer compresses
    // the framed batch before RakNet sees it, so it must fit one datagram.
    for (int attempt = 0; attempt < 20 && !sawCompressedText; ++attempt) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        timeval timeout {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        int ready = select(sock + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;

        std::vector<uint8_t> reply(8192);
        ssize_t received = recvfrom(sock, reply.data(), reply.size(), 0, nullptr, nullptr);
        if (received <= 0) continue;
        reply.resize(static_cast<std::size_t>(received));
        acknowledgeDatagram(sock, target, reply);
        if (reply.size() > 4 && reply[0] >= 0x80 && reply[0] <= 0x8f &&
            (reply[4] & 0x10u) != 0) {
            sawOutboundSplit = true;
            continue;
        }

        for (const auto& payload : parseDatagramPayloads(reply)) {
            if (payload.empty() || payload[0] != 0xfe) continue;

            std::vector<uint8_t> encryptedOnly(payload.begin() + 1, payload.end());
            auto aesPlaintext = clientDecrypt.process(encryptedOnly);
            if (aesPlaintext.size() < 8) {
                close(sock);
                return false;
            }

            std::vector<uint8_t> plaintext(aesPlaintext.begin(), aesPlaintext.end() - 8);
            std::vector<uint8_t> checksum(aesPlaintext.end() - 8, aesPlaintext.end());
            auto expectedChecksum = bedrock::BedrockEncryption::computeChecksum(
                plaintext,
                clientReceiveCounter++,
                derived.secretKeyBytes
            );
            if (checksum != expectedChecksum) {
                close(sock);
                return false;
            }

            auto decoded = codec.decodeCompressionPacket(plaintext);
            const bool actuallyDeflated =
                decoded.compressionHeader ==
                    static_cast<uint8_t>(bedrock::VersionedMcpeCompression::DeflateRaw) &&
                decoded.compressionPacket.size() < decoded.framedBatch.size();
            for (const auto& packet : decoded.batch.packets) {
                if (packet.name != "text") continue;
                auto text = bedrock::VersionedPayloadReader::readText(packet);
                if (actuallyDeflated && text.message == std::string(5000, 'x')) {
                    sawCompressedText = true;
                }
            }
        }
    }

    bedrock::ProtoDefPacketEncoder inboundEncoder("1.20.61");
    auto inboundDisconnect = codec.packetCodec().makePacketByName(
        "disconnect",
        inboundEncoder.encodePacket("disconnect", bedrock::ProtoDefValue::object({
            {"reason", bedrock::ProtoDefValue::string("unknown")},
            {"hide_disconnect_reason", bedrock::ProtoDefValue::boolean(false)},
            {"message", bedrock::ProtoDefValue::string("client disconnect")},
            {"filtered_message", bedrock::ProtoDefValue::string("")}
        }))
    );
    auto disconnectDatagram = buildReliableDatagram(
        encryptPackets({inboundDisconnect}),
        7,
        7,
        7
    );
    sendto(
        sock,
        disconnectDatagram.data(),
        disconnectDatagram.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    );

    auto afterDisconnect = codec.packetCodec().makePacketByName(
        "tick_sync",
        std::vector<uint8_t>(16, 0)
    );
    auto afterDisconnectDatagram = buildReliableDatagram(
        encryptPackets({afterDisconnect}),
        8,
        8,
        8
    );
    sendto(
        sock,
        afterDisconnectDatagram.data(),
        afterDisconnectDatagram.size(),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    );

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    close(sock);
    return sawCompressedText;
}

bool checkRakNetProtocol11Boundary() {
    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.19.30",
        .motd = {{"motd", "RakNet 11 boundary"}}
    });
    server.listen();
    auto open = bedrock::RakNetConnector::openConnection(
        "127.0.0.1",
        server.boundPort(),
        1400,
        300
    );
    server.close();
    return open.ok;
}

bool checkServerAdvertisementSurface() {
    bedrock::BedrockServer cached({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.20.61",
        .motd = {
            {"motd", "Object MOTD"},
            {"name", "Aliased MOTD"},
            {"levelName", "Object Level"},
            {"playersOnline", 41},
            {"playersMax", 99}
        },
        .maxPlayers = 0,
        .offline = true
    });

    auto& first = cached.getAdvertisement();
    const auto cachedServerId = first.serverId;
    auto& second = cached.getAdvertisement();
    const auto storedMotd = cached.options().motd.find("motd");
    if (&first != &second || first.serverId != cachedServerId ||
        second.serverId != cachedServerId || first.motd != "Aliased MOTD" ||
        first.levelName != "Object Level" || first.playersOnline != 0 ||
        first.playersMax != 0 || storedMotd == cached.options().motd.end() ||
        storedMotd->second != "Aliased MOTD") {
        std::cerr << "[SMOKE] cached/object advertisement surface mismatch\n";
        return false;
    }

    cached.listen();
    const auto cachedPong = bedrock::RakNetPinger::ping(
        "127.0.0.1", cached.boundPort(), 500
    );
    const auto expectedCachedWire = cached.getAdvertisement().toBuffer();
    const std::string expectedCachedPayload(
        expectedCachedWire.begin(),
        expectedCachedWire.end()
    );
    const bool cachedWireOk = cachedPong.ok &&
        cachedPong.rawMotd == expectedCachedPayload &&
        cachedPong.motd == "Aliased MOTD" && cachedPong.onlinePlayers == 0 &&
        cachedPong.maxPlayers == 0 &&
        cachedPong.serverId == cachedServerId.toString();
    cached.close();
    if (!cachedWireOk) {
        std::cerr
            << "[SMOKE] object advertisement wire mismatch: expected "
            << expectedCachedWire.size() << " byte Node-compatible payload, got "
            << cachedPong.rawMotd.size() << " bytes\n";
        return false;
    }

    bedrock::ServerAdvertisement custom({
        {"motd", "Callback MOTD"},
        {"playersOnline", 17},
        {"playersMax", 23},
        {"serverId", "callback-server-id"}
    }, 0, "1.20.40");
    std::atomic<int> callbackCalls {0};
    bedrock::BedrockServer callbackServer({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.20.40",
        .motd = {{"motd", "Ignored MOTD"}},
        .maxPlayers = 0,
        .advertisementFn = [&]() -> bedrock::ServerAdvertisement& {
            ++callbackCalls;
            return custom;
        },
        .offline = true
    });

    if (callbackCalls.load() != 0) {
        std::cerr << "[SMOKE] advertisementFn ran before listen\n";
        return false;
    }

    callbackServer.listen();
    if (callbackCalls.load() != 1 || custom.playersOnline != 17 ||
        &callbackServer.getAdvertisement() != &custom || custom.playersOnline != 17) {
        callbackServer.close();
        std::cerr << "[SMOKE] initial advertisementFn/getAdvertisement mismatch\n";
        return false;
    }

    // The explicit getAdvertisement above is the second callback.  The next
    // invocation must be the serverTimer-equivalent provider at about 1000 ms.
    const int callsBeforeTimer = callbackCalls.load();
    const auto timerDeadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(1600);
    while (callbackCalls.load() == callsBeforeTimer &&
           std::chrono::steady_clock::now() < timerDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto pong = bedrock::RakNetPinger::ping(
        "127.0.0.1", callbackServer.boundPort(), 500
    );
    const auto expectedCallbackWire = custom.toBuffer();
    const std::string expectedCallbackPayload(
        expectedCallbackWire.begin(),
        expectedCallbackWire.end()
    );
    const bool ok = callbackCalls.load() > callsBeforeTimer && pong.ok &&
        pong.rawMotd == expectedCallbackPayload &&
        pong.motd == "Callback MOTD" && pong.onlinePlayers == 17 &&
        pong.maxPlayers == 23 && pong.serverId == "callback-server-id" &&
        custom.playersOnline == 17;
    callbackServer.close();
    if (!ok) {
        std::cerr << "[SMOKE] 1000ms advertisementFn provider mismatch\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--advertisement-only") {
        if (!checkServerAdvertisementSurface()) {
            return 1;
        }
        std::cout << "[SMOKE] advertisement surface ok\n";
        return 0;
    }

    if (argc == 2 && std::string(argv[1]) == "--shutdown-only") {
        if (!checkShutdownLifecycles()) {
            return 1;
        }
        std::cout << "[SMOKE] shutdown lifecycle ok\n";
        return 0;
    }

    if (argc == 2 && std::string(argv[1]) == "--player-errors-only") {
        if (!checkPlayerEventSnapshots() ||
            !checkGcmChecksumRecovery() ||
            !checkEncryptedPlayerErrorBranches() ||
            !checkPlayerReadPacketBoundaries() ||
            !checkRakNetCallbackBoundary() ||
            !checkTransportFailureDiagnostics() ||
            !checkEncryptedReceiveVsDelayedClose() ||
            !checkDestroyWhileReceiving()) {
            return 1;
        }
        std::cout << "[SMOKE] Player error semantics ok\n";
        return 0;
    }

    if (!checkCompressionGoldens()) {
        return 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--compression-only") {
        std::cout << "[SMOKE] compression ok\n";
        return 0;
    }
    if (!checkPlayerEventSnapshots() ||
        !checkGcmChecksumRecovery() ||
        !checkEncryptedPlayerErrorBranches() ||
        !checkPlayerReadPacketBoundaries() ||
        !checkRakNetCallbackBoundary() ||
        !checkTransportFailureDiagnostics() ||
        !checkEncryptedReceiveVsDelayedClose() ||
        !checkDestroyWhileReceiving()) {
        return 1;
    }
    if (!checkShutdownLifecycles()) {
        return 1;
    }
    if (!checkServerAdvertisementSurface()) {
        return 1;
    }
    if (!checkReliableSendBackpressure()) {
        return 1;
    }
    if (!checkReconnectSplitSequenceResynchronization()) {
        return 1;
    }
    if (!checkProtocol827NetworkSettingsDelivery()) {
        return 1;
    }

    std::atomic<int> connects {0};
    std::atomic<int> requestNetworkSettingsPackets {0};
    std::atomic<int> loginPackets {0};
    std::atomic<int> authenticatedLoginEvents {0};
    std::atomic<int> joins {0};
    std::atomic<int> spawns {0};
    std::atomic<int> closeEvents {0};
    std::atomic<int> forbiddenGenericPackets {0};
    std::atomic<int> tickSyncPackets {0};
    std::atomic<int> disconnectPackets {0};
    std::atomic<int> callbackSequence {0};
    std::atomic<int> initializingStatusEvents {0};
    std::atomic<int> initializedStatusEvents {0};
    std::atomic<int> firstInitializingStatusSequence {0};
    std::atomic<int> secondInitializingStatusSequence {0};
    std::atomic<int> initializedStatusSequence {0};
    std::atomic<int> firstJoinSequence {0};
    std::atomic<int> joinSequence {0};
    std::atomic<int> handshakeNamedPackets {0};
    std::atomic<int> firstHandshakeNamedSequence {0};
    std::atomic<int> handshakeNamedSequence {0};
    std::atomic<int> handshakeGenericPackets {0};
    std::atomic<int> firstHandshakeGenericSequence {0};
    std::atomic<int> handshakeGenericSequence {0};
    std::atomic<int> spawnSequence {0};
    std::atomic<int> initializedNamedSequence {0};
    std::atomic<int> initializedGenericSequence {0};
    std::atomic<int> disconnectNamedSequence {0};
    std::atomic<int> disconnectGenericSequence {0};
    std::atomic<int> tickNamedSequence {0};
    std::atomic<int> tickGenericSequence {0};
    std::atomic<bool> connectStatusMismatch {false};
    std::atomic<bool> loginLifecycleMismatch {false};
    std::atomic<bool> statusLifecycleMismatch {false};
    std::atomic<bool> joinSawInitializing {true};
    std::atomic<bool> spawnSawInitialized {false};
    std::atomic<bool> initializedNamedSawInitialized {false};
    bool sawOutboundSplit = false;
    bool sawCompressedText = false;

    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.20.61",
        .motd = {{"motd", "Bedrock Protocol C++ Smoke"}},
        .maxPlayers = 8,
        .offline = true
    });

    server.onConnect([&](const bedrock::BedrockServerConnection& connection) {
        ++connects;
        if (!connection.playerEvents ||
            server.status(connection) != bedrock::BedrockServerClientStatus::Authenticating) {
            connectStatusMismatch = true;
        }
        std::cout
            << "[SMOKE] openConnection "
            << connection.address
            << ":"
            << connection.port
            << " mtu="
            << connection.mtu
            << "\n";
    });
    server.on("request_network_settings", [&](const bedrock::BedrockServerPacketEvent& event) {
        ++requestNetworkSettingsPackets;
        std::cout
            << "[SMOKE] packet "
            << event.packet.name
            << " from "
            << event.connection.address
            << ":"
            << event.connection.port
            << "\n";
    });
    server.on("login", [&](const bedrock::BedrockServerPacketEvent&) {
        ++loginPackets;
    });
    server.onLogin([&](const bedrock::BedrockServerPacketEvent& event) {
        ++authenticatedLoginEvents;
        const auto verification = server.loginVerification(event.connection);
        const auto userData = server.userData(event.connection);
        const auto skinData = server.skinData(event.connection);
        const auto profile = server.profile(event.connection);
        const auto version = server.clientVersion(event.connection);
        const auto* displayName = userData ? userData->get("displayName") : nullptr;
        const auto* gameVersion = skinData ? skinData->get("GameVersion") : nullptr;
        if (event.packet.name != "login" ||
            server.status(event.connection) != bedrock::BedrockServerClientStatus::Authenticating ||
            !verification || verification->didVerify ||
            verification->disconnectNotAuthenticated ||
            !displayName || displayName->kind != bedrock::ProtoDefValue::Kind::String ||
            displayName->stringValue != "Smoke" ||
            !gameVersion || gameVersion->kind != bedrock::ProtoDefValue::Kind::String ||
            gameVersion->stringValue != "1.20.61" ||
            !profile || profile->name != "Smoke" ||
            profile->uuid != "00000000-0000-0000-0000-000000000000" ||
            profile->xuid != "0" ||
            !version || *version != 649) {
            loginLifecycleMismatch = true;
        }
    });
    server.on("client_to_server_handshake", [&](const bedrock::BedrockServerPacketEvent& event) {
        const int sequence = ++callbackSequence;
        const int packetIndex = ++handshakeNamedPackets;
        if (packetIndex == 1) {
            firstHandshakeNamedSequence = sequence;
        } else {
            handshakeNamedSequence = sequence;
        }
        if (server.status(event.connection) != bedrock::BedrockServerClientStatus::Initializing) {
            std::cerr << "[SMOKE] handshake named handler did not observe Initializing\n";
        }
    });
    server.on("set_local_player_as_initialized", [&](const bedrock::BedrockServerPacketEvent& event) {
        initializedNamedSequence = ++callbackSequence;
        initializedNamedSawInitialized =
            server.status(event.connection) == bedrock::BedrockServerClientStatus::Initialized;
    });
    server.on("disconnect", [&](const bedrock::BedrockServerPacketEvent&) {
        ++disconnectPackets;
        disconnectNamedSequence = ++callbackSequence;
    });
    server.on("tick_sync", [&](const bedrock::BedrockServerPacketEvent&) {
        ++tickSyncPackets;
        tickNamedSequence = ++callbackSequence;
    });
    server.onAny([&](const bedrock::BedrockServerPacketEvent& event) {
        const int sequence = ++callbackSequence;
        if (event.packet.name == "request_network_settings" || event.packet.name == "login") {
            ++forbiddenGenericPackets;
        } else if (event.packet.name == "client_to_server_handshake") {
            const int packetIndex = ++handshakeGenericPackets;
            if (packetIndex == 1) {
                firstHandshakeGenericSequence = sequence;
            } else {
                handshakeGenericSequence = sequence;
            }
        } else if (event.packet.name == "set_local_player_as_initialized") {
            initializedGenericSequence = sequence;
        } else if (event.packet.name == "disconnect") {
            disconnectGenericSequence = sequence;
        } else if (event.packet.name == "tick_sync") {
            tickGenericSequence = sequence;
        }
    });
    server.onJoin([&](const bedrock::BedrockServerConnection& connection) {
        const int joinIndex = ++joins;
        const int sequence = ++callbackSequence;
        if (joinIndex == 1) {
            firstJoinSequence = sequence;
            // Player captured level 7 at construction. A later public server
            // compressor change must not alter that session snapshot, and the
            // encrypted path must ignore both `none` and its huge threshold.
            server.setCompressor("deflate", 0, 65535);
            server.setCompressor("none");
        } else {
            joinSequence = sequence;
        }
        if (server.status(connection) != bedrock::BedrockServerClientStatus::Initializing) {
            joinSawInitializing = false;
        }
        std::cout
            << "[SMOKE] join "
            << connection.address
            << ":"
            << connection.port
            << "\n";
    });
    server.onSpawn([&](const bedrock::BedrockServerConnection& connection) {
        ++spawns;
        spawnSequence = ++callbackSequence;
        spawnSawInitialized =
            server.status(connection) == bedrock::BedrockServerClientStatus::Initialized;
        server.send(connection, "text", bedrock::ProtoDefValue::object({
            {"type", bedrock::ProtoDefValue::string("raw")},
            {"needs_translation", bedrock::ProtoDefValue::boolean(false)},
            {"message", bedrock::ProtoDefValue::string(std::string(5000, 'x'))},
            {"xuid", bedrock::ProtoDefValue::string("")},
            {"platform_chat_id", bedrock::ProtoDefValue::string("")},
            {"filtered_message", bedrock::ProtoDefValue::string("")}
        }));
    });
    server.onDisconnect([&](const bedrock::BedrockServerConnection&) {
        ++closeEvents;
    });
    server.onStatus([&](
        const bedrock::BedrockServerConnection& connection,
        bedrock::BedrockServerClientStatus nextStatus
    ) {
        const int sequence = ++callbackSequence;
        const auto oldStatus = server.status(connection);
        if (nextStatus == bedrock::BedrockServerClientStatus::Initializing) {
            const int statusIndex = ++initializingStatusEvents;
            if (statusIndex == 1) {
                firstInitializingStatusSequence = sequence;
                if (oldStatus != bedrock::BedrockServerClientStatus::Authenticating) {
                    statusLifecycleMismatch = true;
                }
            } else {
                secondInitializingStatusSequence = sequence;
                if (oldStatus != bedrock::BedrockServerClientStatus::Initializing) {
                    statusLifecycleMismatch = true;
                }
            }
        } else if (nextStatus == bedrock::BedrockServerClientStatus::Initialized) {
            ++initializedStatusEvents;
            initializedStatusSequence = sequence;
            if (oldStatus != bedrock::BedrockServerClientStatus::Initializing) {
                statusLifecycleMismatch = true;
            }
        }
    });

    server.listen();
    const uint16_t port = server.boundPort();
    std::cout << "[SMOKE] listening 127.0.0.1:" << port << "\n";

    auto pong = bedrock::RakNetPinger::ping("127.0.0.1", port, 1000);
    if (!pong.ok) {
        std::cerr << "[SMOKE] ping failed: " << pong.error << "\n";
        server.close();
        return 1;
    }

    if (pong.motd != "Bedrock Protocol C++ Smoke" ||
        pong.gameVersion != "1.20.61" || pong.maxPlayers != 8) {
        std::cerr << "[SMOKE] advertisement mismatch: " << pong.rawMotd << "\n";
        server.close();
        return 1;
    }

    auto open = bedrock::RakNetConnector::openConnection("127.0.0.1", port, 1400, 1000);
    if (!open.ok) {
        std::cerr << "[SMOKE] open connection failed: " << open.error << "\n";
        server.close();
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (connects.load() != 0) {
        std::cerr << "[SMOKE] OpenConnectionRequest2 emitted open connection\n";
        server.close();
        return 1;
    }

    if (!checkConnectedRequestAccepted(port)) {
        std::cerr << "[SMOKE] connected ConnectionRequestAccepted failed\n";
        server.close();
        return 1;
    }

    for (int i = 0; i < 20 && connects.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (connects.load() == 0 || connectStatusMismatch.load()) {
        std::cerr << "[SMOKE] server did not emit open connection after NewIncomingConnection\n";
        server.close();
        return 1;
    }

    if (!checkNetworkSettingsResponse(port)) {
        std::cerr << "[SMOKE] request_network_settings -> network_settings failed\n";
        server.close();
        return 1;
    }

    if (requestNetworkSettingsPackets.load() != 0) {
        std::cerr << "[SMOKE] server emitted request_network_settings despite JS return semantics\n";
        server.close();
        return 1;
    }

    if (!checkLoginHandshakeResponse(port, sawOutboundSplit, sawCompressedText)) {
        std::cerr << "[SMOKE] login -> server_to_client_handshake failed\n";
        server.close();
        return 1;
    }

    if (sawOutboundSplit || !sawCompressedText) {
        std::cerr << "[SMOKE] large text was not deflated before RakNet delivery\n";
        server.close();
        return 1;
    }

    if (joins.load() == 0) {
        std::cerr << "[SMOKE] server did not emit join\n";
        server.close();
        return 1;
    }

    for (int i = 0; i < 50 && tickGenericSequence.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (loginPackets.load() != 0 || forbiddenGenericPackets.load() != 0) {
        std::cerr << "[SMOKE] request_network_settings/login leaked into packet dispatch\n";
        server.close();
        return 1;
    }

    if (authenticatedLoginEvents.load() != 1 || loginLifecycleMismatch.load()) {
        std::cerr << "[SMOKE] authenticated login lifecycle event mismatch\n";
        server.close();
        return 1;
    }

    if (joins.load() != 2 || handshakeNamedPackets.load() != 2 ||
        handshakeGenericPackets.load() != 2 || initializingStatusEvents.load() != 2 ||
        !joinSawInitializing.load() || statusLifecycleMismatch.load() ||
        !(firstInitializingStatusSequence.load() < firstJoinSequence.load() &&
          firstJoinSequence.load() < firstHandshakeNamedSequence.load() &&
          firstHandshakeNamedSequence.load() < firstHandshakeGenericSequence.load() &&
          firstHandshakeGenericSequence.load() < secondInitializingStatusSequence.load() &&
          secondInitializingStatusSequence.load() < joinSequence.load() &&
          joinSequence.load() < handshakeNamedSequence.load() &&
          handshakeNamedSequence.load() < handshakeGenericSequence.load())) {
        std::cerr << "[SMOKE] handshake join/status/dispatch ordering mismatch\n";
        server.close();
        return 1;
    }

    if (initializedStatusEvents.load() != 1 || spawns.load() != 1 ||
        !spawnSawInitialized.load() ||
        !initializedNamedSawInitialized.load() ||
        !(initializedStatusSequence.load() < spawnSequence.load() &&
          spawnSequence.load() < initializedNamedSequence.load() &&
          initializedNamedSequence.load() < initializedGenericSequence.load())) {
        std::cerr << "[SMOKE] initialized spawn/status/dispatch ordering mismatch\n";
        server.close();
        return 1;
    }

    if (disconnectPackets.load() != 1 || closeEvents.load() != 0 ||
        !(disconnectNamedSequence.load() < disconnectGenericSequence.load()) ||
        tickSyncPackets.load() != 1 ||
        !(disconnectGenericSequence.load() < tickNamedSequence.load() &&
          tickNamedSequence.load() < tickGenericSequence.load())) {
        std::cerr << "[SMOKE] inbound disconnect closed/short-circuited packet dispatch\n";
        server.close();
        return 1;
    }

    server.close();

    if (!checkRakNetProtocol11Boundary()) {
        std::cerr << "[SMOKE] Minecraft 1.19.30 did not select RakNet protocol 11\n";
        return 1;
    }

    std::cout << "[SMOKE] ok\n";
    return 0;
}
