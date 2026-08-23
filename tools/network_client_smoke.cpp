#include <bedrock/bedrock.hpp>
#include <bedrock/client/BedrockNetworkClient.hpp>
#include <bedrock/server/BedrockServer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bedrock {

struct BedrockNetworkClientTestAccess {
    static void armEncrypted(
        BedrockNetworkClient& client,
        const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& iv,
        std::function<void(const std::vector<uint8_t>&)> reliableSender = {}
    ) {
        client.closed_.store(false);
        client.closing_.store(false);
        client.status_ = BedrockNetworkClientStatus::Initialized;
        client.encryptionKeys_.secretKeyBytes = key;
        client.encryptionKeys_.iv16 = iv;
        client.encryptStream_ = BedrockEncryption::createCipherStream(
            client.options_.protocolVersion,
            key,
            iv,
            BedrockCipherMode::Encrypt
        );
        client.decryptStream_ = BedrockEncryption::createCipherStream(
            client.options_.protocolVersion,
            key,
            iv,
            BedrockCipherMode::Decrypt
        );
        client.sendCounter_ = 0;
        client.receiveCounter_ = 0;
        client.encryptionEnabled_ = true;
        client.compressionReady_ = true;
        client.reliableSendOverride_ = std::move(reliableSender);
    }

    static void dispatch(
        BedrockNetworkClient& client,
        const std::vector<uint8_t>& payload
    ) {
        client.dispatchRakNetPayload(payload);
    }

    static uint64_t sendCounter(const BedrockNetworkClient& client) {
        return client.sendCounter_;
    }

    static uint64_t receiveCounter(const BedrockNetworkClient& client) {
        return client.receiveCounter_;
    }
};

} // namespace bedrock

namespace {

constexpr uint64_t kRuntimeEntityId = 123456789;

enum class LifecycleOrder {
    Normal,
    SpawnBeforeStartGame
};

enum class CloseMode {
    LocalClose,
    LocalDisconnect,
    RemoteDisconnect
};

bool isPlayerSpawn(const bedrock::VersionedGamePacket& packet) {
    return packet.payload.size() == 4 &&
        packet.payload[0] == 0x00 &&
        packet.payload[1] == 0x00 &&
        packet.payload[2] == 0x00 &&
        packet.payload[3] == 0x03;
}

bool tickSyncExpected(const std::string& version) {
    return bedrock::ProtocolDefinition::forVersion(version).protocolVersion() <=
        bedrock::ProtocolDefinition::forVersion("1.20.80").protocolVersion();
}

bool checkDisconnectGolden(
    const std::string& version,
    const std::vector<uint8_t>& expected
) {
    const bedrock::ProtoDefPacketEncoder encoder(version);
    const auto payload = encoder.encodePacket(
        "disconnect",
        bedrock::ProtoDefValue::object({
            {"reason", bedrock::ProtoDefValue::string("unknown")},
            {"hide_disconnect_reason", bedrock::ProtoDefValue::boolean(false)},
            {"message", bedrock::ProtoDefValue::string("why")},
            {"filtered_message", bedrock::ProtoDefValue::string("")}
        })
    );
    const auto packet = bedrock::VersionedPacketCodec::forVersion(version)
        .makePacketByName("disconnect", payload);
    if (packet.fullPacket == expected) {
        return true;
    }

    std::cerr << "[NETWORK-CLIENT-SMOKE] " << version
              << " disconnect Node golden mismatch\n";
    return false;
}

bool checkVersion(
    const std::string& version,
    LifecycleOrder lifecycleOrder,
    CloseMode closeMode
) {
    const bool race = lifecycleOrder == LifecycleOrder::SpawnBeforeStartGame;
    const bool expectTickSync = tickSyncExpected(version);
    const int expectedInitializedPackets = race ? 2 : 1;
    const std::string expectedCloseReason =
        closeMode == CloseMode::LocalDisconnect
            ? std::string("Client leaving")
            : closeMode == CloseMode::RemoteDisconnect
                ? std::string("Server requested disconnect")
                : std::string("closed");

    const std::vector<uint8_t> startGamePayload = {
        0x02,                         // zigzag64 entity_id = 1
        0x95, 0x9a, 0xef, 0x3a,       // varint64 runtime_entity_id = 123456789
        0x00,                         // zigzag32 player_gamemode = 0
        0x00, 0x00, 0x00, 0x00,       // x = 0
        0x00, 0x00, 0x00, 0x00,       // y = 0
        0x00, 0x00, 0x00, 0x00        // z = 0
    };
    const auto packetCodec = bedrock::VersionedPacketCodec::forVersion(version);
    const auto startGame = packetCodec.makePacketByName("start_game", startGamePayload);
    const auto playerSpawn = packetCodec.makePacketByName(
        "play_status",
        {0x00, 0x00, 0x00, 0x03}
    );

    const std::vector<uint8_t> expectedClientCacheStatus = {0x81, 0x01, 0x00};
    const std::vector<uint8_t> expectedRequestChunkRadius = {0x45, 0x14, 0x00};
    const std::vector<uint8_t> expectedInitialized = {0x71, 0x95, 0x9a, 0xef, 0x3a};
    const std::vector<uint8_t> expectedResourcePackCompleted = {0x08, 0x04, 0x00, 0x00};
    std::vector<uint8_t> expectedDisconnect = {0x05, 0x00, 0x00, 0x0e};
    const std::string defaultDisconnectReason = "Client leaving";
    expectedDisconnect.insert(
        expectedDisconnect.end(),
        defaultDisconnectReason.begin(),
        defaultDisconnectReason.end()
    );
    const std::vector<uint8_t> expectedHiddenDisconnect = {
        0x05, 0x00, 0x00, 0x03, 0x77, 0x68, 0x79
    };

    std::atomic<bool> joined {false};
    std::atomic<bool> gotPlayStatus {false};
    std::atomic<bool> gotResourcePacksInfo {false};
    std::atomic<bool> gotResourcePackStack {false};
    std::atomic<bool> gotSpawn {false};
    std::atomic<bool> spawnSawInitializedStatus {false};
    std::atomic<bool> gotError {false};

    std::atomic<int> callbackSequence {0};
    std::atomic<int> spawnSequence {0};
    std::atomic<int> namedPlayerSpawnSequence {0};
    std::atomic<int> remoteDisconnectAnySequence {0};
    std::atomic<int> remoteDisconnectNamedSequence {0};
    std::atomic<int> remoteKickSequence {0};
    std::atomic<int> remoteKickEvents {0};
    std::atomic<int> closeSequence {0};
    std::atomic<int> closeEvents {0};
    std::atomic<int> disconnectedStatusEvents {0};
    std::atomic<int> lateCloseEvents {0};
    std::atomic<int> lateStatusEvents {0};
    std::atomic<bool> closeSawOldStatus {false};
    std::atomic<bool> closeReasonMismatch {false};
    std::atomic<bool> remoteKickMismatch {false};
    std::atomic<bool> remoteKickSawOldStatus {false};

    std::atomic<int> clientCacheStatusPackets {0};
    std::atomic<int> requestChunkRadiusPackets {0};
    std::atomic<int> initializedPackets {0};
    std::atomic<int> resourcePackCompletedPackets {0};
    std::atomic<int> tickSyncPackets {0};
    std::atomic<int> disconnectPackets {0};

    std::atomic<bool> clientCacheStatusMismatch {false};
    std::atomic<bool> requestChunkRadiusMismatch {false};
    std::atomic<bool> initializedMismatch {false};
    std::atomic<bool> resourcePackCompletedMismatch {false};
    std::atomic<bool> disconnectMismatch {false};

    std::mutex connectionMutex;
    std::optional<bedrock::BedrockServerConnection> serverConnection;

    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {{"motd", "Network Client Smoke"}},
        .maxPlayers = 3,
        .offline = true,
        // The JavaScript server does not negotiate resource packs. This smoke
        // explicitly exercises the retained C++ empty-pack extension because
        // the client-side lifecycle assertions below depend on that exchange.
        .autoResourcePacks = true
    });

    server.on("client_cache_status", [&](const bedrock::BedrockServerPacketEvent& event) {
        ++clientCacheStatusPackets;
        if (event.packet.fullPacket != expectedClientCacheStatus) {
            clientCacheStatusMismatch = true;
        }
    });
    server.on("request_chunk_radius", [&](const bedrock::BedrockServerPacketEvent& event) {
        ++requestChunkRadiusPackets;
        if (event.packet.fullPacket != expectedRequestChunkRadius) {
            requestChunkRadiusMismatch = true;
        }
    });
    server.on("set_local_player_as_initialized", [&](const bedrock::BedrockServerPacketEvent& event) {
        ++initializedPackets;
        if (event.packet.fullPacket != expectedInitialized) {
            initializedMismatch = true;
        }
    });
    server.on("resource_pack_client_response", [&](const bedrock::BedrockServerPacketEvent& event) {
        ++resourcePackCompletedPackets;
        if (event.packet.fullPacket != expectedResourcePackCompleted) {
            resourcePackCompletedMismatch = true;
        }
    });
    server.on("tick_sync", [&](const bedrock::BedrockServerPacketEvent&) {
        ++tickSyncPackets;
    });
    server.on("disconnect", [&](const bedrock::BedrockServerPacketEvent& event) {
        const int packetIndex = ++disconnectPackets;
        const auto& expected = packetIndex == 1
            ? expectedHiddenDisconnect
            : expectedDisconnect;
        if (packetIndex > 2 || event.packet.fullPacket != expected) {
            disconnectMismatch = true;
        }
    });
    server.onJoin([&](const bedrock::BedrockServerConnection& connection) {
        {
            std::lock_guard<std::mutex> lock(connectionMutex);
            serverConnection = connection;
        }
        if (race) {
            server.sendPackets(connection, {playerSpawn, startGame, startGame});
        } else {
            server.sendPackets(connection, {startGame, playerSpawn});
        }
    });
    server.listen();

    auto client = bedrock::createNetworkClient(bedrock::BedrockNetworkClientOptions{
        .host = "127.0.0.1",
        .port = server.boundPort(),
        .username = "CppSmoke",
        .version = version,
        .offline = true,
        .connectTimeoutMs = 1000
    });

    client.onJoin([&]() {
        joined = true;
    });
    client.onSpawn([&]() {
        spawnSequence = ++callbackSequence;
        spawnSawInitializedStatus =
            client.status() == bedrock::BedrockNetworkClientStatus::Initialized;
        gotSpawn = true;
    });
    client.onAny([&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        if (event.packet.name == "disconnect") {
            remoteDisconnectAnySequence = ++callbackSequence;
        }
    });
    client.on("disconnect", [&](const bedrock::BedrockNetworkClientPacketEvent&) {
        remoteDisconnectNamedSequence = ++callbackSequence;
    });
    client.on("kick", [&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        ++remoteKickEvents;
        remoteKickSequence = ++callbackSequence;
        remoteKickSawOldStatus =
            client.status() == bedrock::BedrockNetworkClientStatus::Initialized;
        if (event.packet.name != "disconnect") {
            remoteKickMismatch = true;
        }
    });
    client.onClose([&](const std::string& reason) {
        ++closeEvents;
        closeSequence = ++callbackSequence;
        closeSawOldStatus =
            client.status() == bedrock::BedrockNetworkClientStatus::Initialized;
        if (reason != expectedCloseReason) {
            closeReasonMismatch = true;
        }
    });
    client.onStatus([&](bedrock::BedrockNetworkClientStatus nextStatus) {
        if (nextStatus == bedrock::BedrockNetworkClientStatus::Disconnected) {
            ++disconnectedStatusEvents;
        }
    });

    client.on("play_status", [&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        gotPlayStatus = true;
        if (isPlayerSpawn(event.packet)) {
            namedPlayerSpawnSequence = ++callbackSequence;
        }
    });
    client.on("resource_packs_info", [&](const bedrock::BedrockNetworkClientPacketEvent&) {
        gotResourcePacksInfo = true;
    });
    client.on("resource_pack_stack", [&](const bedrock::BedrockNetworkClientPacketEvent&) {
        gotResourcePackStack = true;
    });

    client.onError([&](const std::string& error) {
        gotError = true;
        std::cerr << "[NETWORK-CLIENT-SMOKE] " << version << " error: " << error << "\n";
    });

    if (!client.connect()) {
        std::cerr << "[NETWORK-CLIENT-SMOKE] " << version << " connect failed\n";
        server.close();
        return false;
    }

    int stableIterations = 0;
    for (int i = 0; i < 250 && !gotError.load(); ++i) {
        const bool lifecycleComplete =
            joined.load() &&
            gotPlayStatus.load() &&
            gotResourcePacksInfo.load() &&
            gotResourcePackStack.load() &&
            gotSpawn.load() &&
            clientCacheStatusPackets.load() >= 1 &&
            requestChunkRadiusPackets.load() >= 1 &&
            initializedPackets.load() >= expectedInitializedPackets &&
            resourcePackCompletedPackets.load() >= 2 &&
            (!expectTickSync || tickSyncPackets.load() >= 1);

        if (lifecycleComplete) {
            // Keep the connection alive briefly after all required packets. This
            // lets a wrongly enabled post-spawn tick_sync become observable on
            // versions newer than 1.20.80 without racing the server callback.
            if (++stableIterations >= 10) {
                break;
            }
        } else {
            stableIterations = 0;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const auto entityIdBeforeClose = client.entityId();
    const bool initializedBeforeClose =
        client.status() == bedrock::BedrockNetworkClientStatus::Initialized;

    bool closeSetupOk = true;
    if (closeMode == CloseMode::LocalDisconnect) {
        // Preserve the JS field-name bug: hide_disconnect_screen is not the
        // schema's hide_disconnect_reason, so true still serializes as false.
        client.write("disconnect", bedrock::ProtoDefValue::object({
            {"reason", bedrock::ProtoDefValue::string("unknown")},
            {"hide_disconnect_reason", bedrock::ProtoDefValue::boolean(false)},
            {"message", bedrock::ProtoDefValue::string("why")},
            {"filtered_message", bedrock::ProtoDefValue::string("")}
        }));
        for (int i = 0; i < 50 && disconnectPackets.load() < 1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        client.disconnect();
    } else if (closeMode == CloseMode::RemoteDisconnect) {
        std::optional<bedrock::BedrockServerConnection> connection;
        {
            std::lock_guard<std::mutex> lock(connectionMutex);
            connection = serverConnection;
        }
        if (!connection.has_value()) {
            closeSetupOk = false;
            client.close();
        } else {
            server.disconnect(*connection, "Remote close", false);
        }
    } else {
        client.close();
    }

    for (int i = 0; i < 100 &&
         (closeEvents.load() == 0 ||
          client.status() != bedrock::BedrockNetworkClientStatus::Disconnected ||
          (closeMode == CloseMode::LocalDisconnect && disconnectPackets.load() < 2));
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const bool disconnectedAfterClose =
        client.status() == bedrock::BedrockNetworkClientStatus::Disconnected;

    // disconnect is a no-op once Disconnected. A subsequent close still runs
    // JS cleanup/removeAllListeners, but must not emit close or status.
    client.onClose([&](const std::string&) {
        ++lateCloseEvents;
    });
    client.onStatus([&](bedrock::BedrockNetworkClientStatus) {
        ++lateStatusEvents;
    });
    client.disconnect("ignored after close", true);
    client.close("duplicate close");
    client.close("third close");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.close();

    bool ok = true;
    const auto fail = [&](const std::string& message) {
        std::cerr << "[NETWORK-CLIENT-SMOKE] " << version << " " << message << "\n";
        ok = false;
    };

    if (!joined.load()) {
        fail("did not join");
    }
    if (!gotPlayStatus.load()) {
        fail("did not receive play_status");
    }
    if (!gotResourcePacksInfo.load()) {
        fail("did not receive resource_packs_info");
    }
    if (!gotResourcePackStack.load()) {
        fail("did not receive resource_pack_stack");
    }
    if (!gotSpawn.load()) {
        fail("did not emit spawn");
    }
    if (!spawnSawInitializedStatus.load()) {
        fail("spawn handler did not observe Initialized status");
    }

    if (!entityIdBeforeClose.has_value() || *entityIdBeforeClose != kRuntimeEntityId) {
        fail("runtime entity id mismatch");
    }
    if (!initializedBeforeClose) {
        fail("final status is not Initialized");
    }

    if (spawnSequence.load() == 0 ||
        namedPlayerSpawnSequence.load() <= spawnSequence.load()) {
        fail("spawn was not emitted before named player_spawn play_status");
    }

    if (clientCacheStatusPackets.load() != 1 || clientCacheStatusMismatch.load()) {
        fail("client_cache_status golden mismatch");
    }
    if (requestChunkRadiusPackets.load() != 1 || requestChunkRadiusMismatch.load()) {
        fail("request_chunk_radius golden mismatch");
    }
    if (initializedPackets.load() != expectedInitializedPackets || initializedMismatch.load()) {
        fail("set_local_player_as_initialized golden/count mismatch");
    }
    if (resourcePackCompletedPackets.load() != 2 || resourcePackCompletedMismatch.load()) {
        fail("resource_pack_client_response golden/count mismatch");
    }
    if (expectTickSync && tickSyncPackets.load() == 0) {
        fail("did not send tick_sync for <= 1.20.80");
    }
    if (!expectTickSync && tickSyncPackets.load() != 0) {
        fail("sent tick_sync for > 1.20.80");
    }
    if (!closeSetupOk || !disconnectedAfterClose) {
        fail("close path did not reach Disconnected");
    }
    if (closeEvents.load() != 1 || !closeSawOldStatus.load() ||
        closeReasonMismatch.load()) {
        fail("close event count/reason/old-status mismatch");
    }
    if (disconnectedStatusEvents.load() != 0) {
        fail("status listener observed Disconnected after removeAllListeners");
    }
    if (lateCloseEvents.load() != 0 || lateStatusEvents.load() != 0) {
        fail("repeated close/disconnect was not idempotent");
    }
    if (closeMode == CloseMode::LocalDisconnect) {
        if (disconnectPackets.load() != 2 || disconnectMismatch.load()) {
            fail("disconnect JS golden/default packet mismatch");
        }
        if (remoteDisconnectAnySequence.load() != 0 ||
            remoteDisconnectNamedSequence.load() != 0) {
            fail("client observed its own disconnect packet");
        }
    } else if (disconnectPackets.load() != 0) {
        fail("close path unexpectedly wrote disconnect");
    }
    if (closeMode == CloseMode::RemoteDisconnect) {
        if (remoteDisconnectAnySequence.load() == 0 ||
            remoteKickEvents.load() != 1 ||
            remoteKickMismatch.load() ||
            !remoteKickSawOldStatus.load() ||
            !(remoteDisconnectAnySequence.load() < remoteDisconnectNamedSequence.load() &&
              remoteDisconnectNamedSequence.load() < remoteKickSequence.load() &&
              remoteKickSequence.load() < closeSequence.load())) {
            fail("remote disconnect/kick/close ordering mismatch");
        }
    } else if (remoteDisconnectAnySequence.load() != 0 ||
               remoteDisconnectNamedSequence.load() != 0 ||
               remoteKickEvents.load() != 0) {
        fail("non-remote close observed a remote disconnect/kick");
    }
    if (gotError.load()) {
        ok = false;
    }

    if (ok) {
        std::cout << "[NETWORK-CLIENT-SMOKE] " << version
                  << (race ? " race" : " normal")
                  << (closeMode == CloseMode::LocalDisconnect
                          ? " disconnect"
                          : closeMode == CloseMode::RemoteDisconnect
                              ? " remote-close"
                              : " close")
                  << " ok\n";
    }
    return ok;
}

bool checkHighLevelKickDecoding() {
    const std::string version = "1.20.40";
    const auto packetCodec = bedrock::VersionedPacketCodec::forVersion(version);
    const std::vector<uint8_t> startGamePayload = {
        0x02,                         // zigzag64 entity_id = 1
        0x95, 0x9a, 0xef, 0x3a,       // varint64 runtime_entity_id = 123456789
        0x00,                         // zigzag32 player_gamemode = 0
        0x00, 0x00, 0x00, 0x00,       // x = 0
        0x00, 0x00, 0x00, 0x00,       // y = 0
        0x00, 0x00, 0x00, 0x00        // z = 0
    };
    const auto startGame = packetCodec.makePacketByName("start_game", startGamePayload);
    const auto playerSpawn = packetCodec.makePacketByName(
        "play_status",
        {0x00, 0x00, 0x00, 0x03}
    );

    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {{"motd", "High-level Kick Smoke"}},
        .maxPlayers = 1,
        .offline = true,
        .autoResourcePacks = true
    });
    server.onJoin([&](const bedrock::BedrockServerConnection& connection) {
        server.sendPackets(connection, {startGame, playerSpawn});
    });
    server.onSpawn([&](const bedrock::BedrockServerConnection& connection) {
        server.disconnect(connection, "High-level kick", false);
    });
    server.listen();

    bedrock::Options options;
    options.host = "127.0.0.1";
    options.port = server.boundPort();
    options.username = "CppKickSmoke";
    options.version = version;
    options.offline = true;
    options.connectTimeoutMs = 1000;
    options.decodePackets = true;
    bedrock::Client client(std::move(options));

    std::atomic<int> kickEvents {0};
    std::atomic<int> closeEvents {0};
    std::atomic<bool> kickMismatch {false};
    std::atomic<bool> kickSawOldStatus {false};
    std::atomic<bool> gotError {false};

    // Deliberately register no `packet` or `disconnect` handler.  A missing
    // high-level kick->disconnect decoder mapping would leave these fields
    // empty and make this check fail.
    client.on("kick", [&](const bedrock::Packet& packet) {
        ++kickEvents;
        kickSawOldStatus = client.status() == bedrock::ClientStatus::Initialized;
        if (packet.name != "disconnect" ||
            packet.get("message") != "High-level kick" ||
            packet.get("hide_disconnect_reason") != "false") {
            kickMismatch = true;
        }
    });
    client.onClose([&](const std::string&) {
        ++closeEvents;
    });
    client.onError([&](const std::string& error) {
        gotError = true;
        std::cerr << "[NETWORK-CLIENT-SMOKE] high-level kick error: "
                  << error << "\n";
    });

    if (!client.connect()) {
        std::cerr << "[NETWORK-CLIENT-SMOKE] high-level kick connect failed\n";
        server.close();
        return false;
    }

    for (int i = 0; i < 250 &&
         (closeEvents.load() == 0 ||
          client.status() != bedrock::ClientStatus::Disconnected) &&
         !gotError.load();
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    client.close("duplicate close");
    server.close();

    if (kickEvents.load() != 1 ||
        closeEvents.load() != 1 ||
        kickMismatch.load() ||
        !kickSawOldStatus.load() ||
        gotError.load()) {
        std::cerr << "[NETWORK-CLIENT-SMOKE] high-level kick decode/lifecycle mismatch\n";
        return false;
    }

    std::cout << "[NETWORK-CLIENT-SMOKE] high-level kick ok\n";
    return true;
}

struct CraftedEncryptedPayload {
    std::vector<uint8_t> wire;
    std::string mismatchMessage;
};

CraftedEncryptedPayload makeEncryptedInbound(
    uint32_t protocolVersion,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& iv,
    const std::vector<uint8_t>& compressionPacket,
    bool corruptChecksum
) {
    auto plaintext = bedrock::BedrockEncryption::makeAesPlaintext(
        compressionPacket,
        0,
        key
    );
    if (corruptChecksum) {
        plaintext.back() ^= 0x01;
    }

    uint64_t probeCounter = 0;
    const auto probe = bedrock::BedrockEncryption::verifyAesPlaintext(
        plaintext,
        probeCounter,
        key
    );

    auto encrypt = bedrock::BedrockEncryption::createCipherStream(
        protocolVersion,
        key,
        iv,
        bedrock::BedrockCipherMode::Encrypt
    );
    auto encrypted = encrypt->process(plaintext);

    CraftedEncryptedPayload result;
    result.wire.reserve(1 + encrypted.size());
    result.wire.push_back(0xfe);
    result.wire.insert(result.wire.end(), encrypted.begin(), encrypted.end());
    if (!probe.matches()) {
        result.mismatchMessage = probe.mismatchMessage();
    }
    return result;
}

bool isEncryptedBadPacketDisconnect(
    const std::vector<uint8_t>& wire,
    const std::string& version,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& iv
) {
    if (wire.size() < 2 || wire[0] != 0xfe) {
        return false;
    }

    const auto protocolVersion = bedrock::ProtocolDefinition::forVersion(version)
        .protocolVersion();
    auto decrypt = bedrock::BedrockEncryption::createCipherStream(
        protocolVersion,
        key,
        iv,
        bedrock::BedrockCipherMode::Decrypt
    );
    uint64_t receiveCounter = 0;
    auto verification = bedrock::BedrockEncryption::decryptAndVerify(
        *decrypt,
        std::vector<uint8_t>(wire.begin() + 1, wire.end()),
        receiveCounter,
        key
    );
    if (!verification || !verification->matches() || receiveCounter != 1) {
        return false;
    }

    const auto decoded = bedrock::VersionedMcpeCodec::forVersion(version)
        .decodeEncryptedCompressionPacket(verification->packetPlaintext);
    if (decoded.batch.packets.size() != 1 ||
        decoded.batch.packets[0].name != "disconnect") {
        return false;
    }

    const bedrock::ProtoDefPacketEncoder encoder(version);
    const auto expectedPayload = encoder.encodePacket(
        "disconnect",
        bedrock::ProtoDefValue::object({
            {"reason", bedrock::ProtoDefValue::string("unknown")},
            {"hide_disconnect_reason", bedrock::ProtoDefValue::boolean(false)},
            {"message", bedrock::ProtoDefValue::string("disconnectionScreen.badPacket")},
            {"filtered_message", bedrock::ProtoDefValue::string("")}
        })
    );
    const auto expected = bedrock::VersionedPacketCodec::forVersion(version)
        .makePacketByName("disconnect", expectedPayload);
    return decoded.batch.packets[0].fullPacket == expected.fullPacket;
}

bool checkEncryptedErrorSurface() {
    const std::string version = "1.20.61";
    const auto protocolVersion = bedrock::ProtocolDefinition::forVersion(version)
        .protocolVersion();
    std::vector<uint8_t> key(32);
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(i);
    }
    const std::vector<uint8_t> iv(key.begin(), key.begin() + 16);
    const auto mismatch = makeEncryptedInbound(
        protocolVersion,
        key,
        iv,
        {0xff, 0x01},
        true
    );

    const auto makeOptions = [&]() {
        bedrock::BedrockNetworkClientOptions options;
        options.version = version;
        options.offline = true;
        return options;
    };
    const auto fail = [](const std::string& message) {
        std::cerr << "[NETWORK-CLIENT-SMOKE] encrypted error surface "
                  << message << "\n";
        return false;
    };

    // EventEmitter's special no-listener `error` branch throws before the
    // bad-packet disconnect statement.
    {
        bedrock::BedrockNetworkClient client(makeOptions());
        std::vector<std::vector<uint8_t>> sent;
        bedrock::BedrockNetworkClientTestAccess::armEncrypted(
            client,
            key,
            iv,
            [&](const std::vector<uint8_t>& payload) { sent.push_back(payload); }
        );

        bool threwExpected = false;
        try {
            bedrock::BedrockNetworkClientTestAccess::dispatch(client, mismatch.wire);
        } catch (const bedrock::BedrockNetworkClientUnhandledError& error) {
            threwExpected = std::string(error.what()) == mismatch.mismatchMessage;
        }
        if (!threwExpected || !sent.empty() ||
            client.status() != bedrock::BedrockNetworkClientStatus::Initialized ||
            bedrock::BedrockNetworkClientTestAccess::receiveCounter(client) != 1 ||
            bedrock::BedrockNetworkClientTestAccess::sendCounter(client) != 0) {
            client.close("test cleanup");
            return fail("no-listener mismatch branch diverged");
        }
        client.close("test cleanup");
    }

    // With an error listener, encryption.js emits exactly once, writes one
    // encrypted disconnect, then closes in that order.
    {
        bedrock::BedrockNetworkClient client(makeOptions());
        std::vector<std::vector<uint8_t>> sent;
        std::vector<int> order;
        std::string errorText;
        std::string closeReason;
        bedrock::BedrockNetworkClientTestAccess::armEncrypted(
            client,
            key,
            iv,
            [&](const std::vector<uint8_t>& payload) {
                order.push_back(2);
                sent.push_back(payload);
            }
        );
        client.onError([&](const std::string& error) {
            order.push_back(1);
            errorText = error;
        });
        client.onClose([&](const std::string& reason) {
            order.push_back(3);
            closeReason = reason;
        });

        try {
            bedrock::BedrockNetworkClientTestAccess::dispatch(client, mismatch.wire);
        } catch (const std::exception& error) {
            return fail("listener mismatch unexpectedly threw: " + std::string(error.what()));
        }
        if (order != std::vector<int>({1, 2, 3}) ||
            errorText != mismatch.mismatchMessage ||
            closeReason != "disconnectionScreen.badPacket" ||
            sent.size() != 1 ||
            !isEncryptedBadPacketDisconnect(sent[0], version, key, iv) ||
            client.status() != bedrock::BedrockNetworkClientStatus::Disconnected ||
            bedrock::BedrockNetworkClientTestAccess::receiveCounter(client) != 1 ||
            bedrock::BedrockNetworkClientTestAccess::sendCounter(client) != 1) {
            return fail("listener error/disconnect/close ordering diverged");
        }
    }

    // Closing from the first listener clears member storage, but Node's
    // listener snapshot still invokes the second listener. The later explicit
    // disconnect observes Disconnected and is a no-op.
    {
        bedrock::BedrockNetworkClient client(makeOptions());
        std::vector<std::vector<uint8_t>> sent;
        std::vector<int> order;
        bedrock::BedrockNetworkClientTestAccess::armEncrypted(
            client,
            key,
            iv,
            [&](const std::vector<uint8_t>& payload) { sent.push_back(payload); }
        );
        client.onError([&](const std::string&) {
            order.push_back(1);
            client.close("listener close");
        });
        client.onError([&](const std::string&) {
            order.push_back(3);
        });
        client.onClose([&](const std::string&) {
            order.push_back(2);
        });

        bedrock::BedrockNetworkClientTestAccess::dispatch(client, mismatch.wire);
        if (order != std::vector<int>({1, 2, 3}) || !sent.empty() ||
            client.status() != bedrock::BedrockNetworkClientStatus::Disconnected ||
            bedrock::BedrockNetworkClientTestAccess::receiveCounter(client) != 1 ||
            bedrock::BedrockNetworkClientTestAccess::sendCounter(client) != 0) {
            return fail("listener-close snapshot branch diverged");
        }
    }

    // A throwing listener aborts EventEmitter iteration and prevents both the
    // remaining listener and encryption.js's following disconnect call.
    {
        bedrock::BedrockNetworkClient client(makeOptions());
        std::vector<std::vector<uint8_t>> sent;
        int firstEvents = 0;
        int secondEvents = 0;
        bedrock::BedrockNetworkClientTestAccess::armEncrypted(
            client,
            key,
            iv,
            [&](const std::vector<uint8_t>& payload) { sent.push_back(payload); }
        );
        client.onError([&](const std::string&) {
            ++firstEvents;
            throw std::runtime_error("listener boom");
        });
        client.onError([&](const std::string&) {
            ++secondEvents;
        });

        bool threwExpected = false;
        try {
            bedrock::BedrockNetworkClientTestAccess::dispatch(client, mismatch.wire);
        } catch (const std::runtime_error& error) {
            threwExpected = std::string(error.what()) == "listener boom";
        }
        if (!threwExpected || firstEvents != 1 || secondEvents != 0 ||
            !sent.empty() ||
            client.status() != bedrock::BedrockNetworkClientStatus::Initialized) {
            client.close("test cleanup");
            return fail("throwing-listener branch diverged");
        }
        client.close("test cleanup");
    }

    // A valid checksum consumes receiveCounter before inflateRawSync fails,
    // but that zlib exception is not a client `error` event and does not close.
    {
        bedrock::BedrockNetworkClient client(makeOptions());
        std::vector<std::vector<uint8_t>> sent;
        int errorEvents = 0;
        int closeEvents = 0;
        bedrock::BedrockNetworkClientTestAccess::armEncrypted(
            client,
            key,
            iv,
            [&](const std::vector<uint8_t>& payload) { sent.push_back(payload); }
        );
        client.onError([&](const std::string&) { ++errorEvents; });
        client.onClose([&](const std::string&) { ++closeEvents; });
        const auto badDeflate = makeEncryptedInbound(
            protocolVersion,
            key,
            iv,
            {0x00, 0x00},
            false
        );

        bool threw = false;
        try {
            bedrock::BedrockNetworkClientTestAccess::dispatch(client, badDeflate.wire);
        } catch (const std::exception&) {
            threw = true;
        }
        if (!threw || errorEvents != 0 || closeEvents != 0 || !sent.empty() ||
            client.status() != bedrock::BedrockNetworkClientStatus::Initialized ||
            bedrock::BedrockNetworkClientTestAccess::receiveCounter(client) != 1) {
            client.close("test cleanup");
            return fail("valid-checksum inflate failure diverged");
        }
        client.close("test cleanup");
    }

    // Modern encryption.js explicitly emits this one error, then its
    // undefined decompressed buffer fails at the transport boundary.
    {
        bedrock::BedrockNetworkClient client(makeOptions());
        std::vector<std::vector<uint8_t>> sent;
        std::vector<std::string> errors;
        int closeEvents = 0;
        bedrock::BedrockNetworkClientTestAccess::armEncrypted(
            client,
            key,
            iv,
            [&](const std::vector<uint8_t>& payload) { sent.push_back(payload); }
        );
        client.onError([&](const std::string& error) { errors.push_back(error); });
        client.onClose([&](const std::string&) { ++closeEvents; });
        const auto unknown = makeEncryptedInbound(
            protocolVersion,
            key,
            iv,
            {0x7e},
            false
        );

        bool threw = false;
        try {
            bedrock::BedrockNetworkClientTestAccess::dispatch(client, unknown.wire);
        } catch (const std::exception&) {
            threw = true;
        }
        if (!threw || errors != std::vector<std::string>({"Unsupported compressor: 126"}) ||
            closeEvents != 0 || !sent.empty() ||
            client.status() != bedrock::BedrockNetworkClientStatus::Initialized ||
            bedrock::BedrockNetworkClientTestAccess::receiveCounter(client) != 1) {
            client.close("test cleanup");
            return fail("handled unsupported-compressor branch diverged");
        }
        client.close("test cleanup");
    }

    // Without an error listener, the exact unsupported-compressor Error is
    // the first throw and the safe undefined-buffer adaptation is not reached.
    {
        bedrock::BedrockNetworkClient client(makeOptions());
        std::vector<std::vector<uint8_t>> sent;
        bedrock::BedrockNetworkClientTestAccess::armEncrypted(
            client,
            key,
            iv,
            [&](const std::vector<uint8_t>& payload) { sent.push_back(payload); }
        );
        const auto unknown = makeEncryptedInbound(
            protocolVersion,
            key,
            iv,
            {0x7e},
            false
        );

        bool threwExpected = false;
        try {
            bedrock::BedrockNetworkClientTestAccess::dispatch(client, unknown.wire);
        } catch (const bedrock::BedrockNetworkClientUnhandledError& error) {
            threwExpected = std::string(error.what()) == "Unsupported compressor: 126";
        }
        if (!threwExpected || !sent.empty() ||
            client.status() != bedrock::BedrockNetworkClientStatus::Initialized ||
            bedrock::BedrockNetworkClientTestAccess::receiveCounter(client) != 1) {
            client.close("test cleanup");
            return fail("no-listener unsupported-compressor branch diverged");
        }
        client.close("test cleanup");
    }

    std::cout << "[NETWORK-CLIENT-SMOKE] encrypted error surface ok\n";
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok = checkDisconnectGolden(
        "1.20.40",
        {0x05, 0x00, 0x00, 0x03, 0x77, 0x68, 0x79}
    ) && ok;
    ok = checkDisconnectGolden(
        "1.26.0",
        {0x05, 0x00, 0x00, 0x03, 0x77, 0x68, 0x79, 0x00}
    ) && ok;
    ok = checkVersion(
        "1.20.40",
        LifecycleOrder::Normal,
        CloseMode::LocalDisconnect
    ) && ok;
    ok = checkVersion(
        "1.20.50",
        LifecycleOrder::SpawnBeforeStartGame,
        CloseMode::LocalClose
    ) && ok;
    ok = checkVersion(
        "1.21.100",
        LifecycleOrder::Normal,
        CloseMode::RemoteDisconnect
    ) && ok;
    ok = checkHighLevelKickDecoding() && ok;
    ok = checkEncryptedErrorSurface() && ok;
    return ok ? 0 : 1;
}
