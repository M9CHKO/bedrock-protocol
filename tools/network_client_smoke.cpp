#include <bedrock/bedrock.hpp>
#include <bedrock/auth/UuidV3.hpp>
#include <bedrock/client/BedrockNetworkClient.hpp>
#include <bedrock/server/BedrockServer.hpp>
#include <bedrock/world/BedrockSubChunkPacket.hpp>

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

    static bool queueRunning(BedrockNetworkClient& client) {
        std::lock_guard<std::mutex> lock(client.queueMutex_);
        return !client.stopQueue_;
    }

    static bool queueThreadJoinable(BedrockNetworkClient& client) {
        std::lock_guard<std::mutex> lock(client.queueLifecycleMutex_);
        return client.queueThread_.joinable();
    }

    static bool startQueue(BedrockNetworkClient& client) {
        return client.startQueue();
    }

    static void armQueueFailure(
        BedrockNetworkClient& client,
        std::function<void(const std::vector<uint8_t>&)> sender
    ) {
        client.closed_.store(false);
        client.closing_.store(false);
        client.rakNetStopRequested_.store(false);
        {
            std::lock_guard<std::mutex> lock(client.mutex_);
            client.status_ = BedrockNetworkClientStatus::Initialized;
        }
        std::lock_guard<std::mutex> lock(client.sendMutex_);
        client.reliableSendOverride_ = std::move(sender);
        client.raknet_ = std::make_shared<RakNetClient>();
    }

    static bool hasTransport(BedrockNetworkClient& client) {
        std::lock_guard<std::mutex> lock(client.sendMutex_);
        return static_cast<bool>(client.raknet_);
    }

    static bool stopRequested(const BedrockNetworkClient& client) {
        return client.rakNetStopRequested_.load();
    }

    static void applySubChunk(
        BedrockNetworkClient& client,
        const std::vector<uint8_t>& payload
    ) {
        VersionedGamePacket packet;
        packet.name = "subchunk";
        packet.payload = payload;
        client.handleSubChunk(packet);
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

bool checkSubChunkWorldTracking() {
    bedrock::BedrockNetworkClientOptions options;
    options.version = "1.18.11";
    options.offline = true;
    options.trackWorld = true;
    bedrock::BedrockNetworkClient client(options);

    auto terrain = bedrock::BedrockSubChunk::createAir(-2, 0);
    terrain.setBlockStateId(1, 2, 3, 321);

    bedrock::BedrockSubChunkPacket packet;
    packet.cacheEnabled = false;
    packet.dimension = 0;
    packet.originX = 10;
    packet.originY = -4;
    packet.originZ = -8;
    bedrock::BedrockSubChunkPacketEntry terrainEntry;
    terrainEntry.dx = 1;
    terrainEntry.dy = 2;
    terrainEntry.dz = -3;
    terrainEntry.result = bedrock::BedrockSubChunkResult::Success;
    terrainEntry.payload = terrain.encode(bedrock::ChunkStorageType::Runtime);
    packet.entries.push_back(std::move(terrainEntry));
    bedrock::BedrockSubChunkPacketEntry airEntry;
    airEntry.dx = -4;
    airEntry.dy = 5;
    airEntry.dz = 6;
    airEntry.result = bedrock::BedrockSubChunkResult::SuccessAllAir;
    packet.entries.push_back(std::move(airEntry));

    bedrock::BedrockNetworkClientTestAccess::applySubChunk(
        client,
        bedrock::BedrockSubChunkPacketCodec::encodePacketPayload(packet, "1.18.11")
    );

    const auto* terrainColumn = client.world().getLoadedColumn(11, -11);
    const auto* airColumn = client.world().getLoadedColumn(6, -2);
    if (terrainColumn == nullptr || airColumn == nullptr ||
        terrainColumn->getBlockStateId({.x = 1, .y = -30, .z = 3}) != 321 ||
        airColumn->getSection(16) == nullptr ||
        airColumn->getBlockStateId({.x = 0, .y = 16, .z = 0}) != 0) {
        std::cerr << "[NETWORK-CLIENT-SMOKE] subchunk world tracking mismatch\n";
        return false;
    }

    std::cout << "[NETWORK-CLIENT-SMOKE] subchunk world tracking ok\n";
    return true;
}

bool checkVersion(
    const std::string& version,
    LifecycleOrder lifecycleOrder,
    CloseMode closeMode,
    const std::string& compressionAlgorithm = "deflate",
    uint16_t compressionThreshold = 512
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
    std::atomic<int> sessionEvents {0};
    std::atomic<bool> sessionMismatch {false};
    std::atomic<int> loggingInEvents {0};
    std::atomic<bool> loggingInSawAuthenticatingStatus {false};

    std::atomic<int> callbackSequence {0};
    std::atomic<int> sessionSequence {0};
    std::atomic<int> loggingInSequence {0};
    std::atomic<int> joinSequence {0};
    std::atomic<int> clientHandshakeSequence {0};
    std::atomic<int> rawHandshakeSequence {0};
    std::atomic<bool> clientHandshakeMismatch {false};
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

    std::atomic<int> serverCallbackSequence {0};
    std::atomic<int> serverLoggingInSequence {0};
    std::atomic<int> serverHandshakeSequence {0};
    std::atomic<int> serverLoginSequence {0};
    std::atomic<int> serverLoggingInEvents {0};
    std::atomic<int> serverHandshakeEvents {0};
    std::atomic<int> serverLoginEvents {0};
    std::atomic<bool> serverLoggingInMismatch {false};
    std::atomic<bool> serverHandshakeMismatch {false};
    std::atomic<bool> serverLoginMismatch {false};

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
        .autoResourcePacks = true,
        .compressionThreshold = compressionThreshold,
        .compressionAlgorithm = compressionAlgorithm
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
    server.onLoggingIn([&](const bedrock::BedrockServerLoggingInEvent& event) {
        ++serverLoggingInEvents;
        serverLoggingInSequence = ++serverCallbackSequence;
        if (event.packet.name != "login" ||
            event.login.protocolVersion != bedrock::protocolVersionFor(version) ||
            event.login.identity.empty() ||
            event.login.client.empty() ||
            server.status(event.connection) !=
                bedrock::BedrockServerClientStatus::Authenticating ||
            server.loginVerification(event.connection).has_value()) {
            serverLoggingInMismatch = true;
        }
    });
    server.onServerClientHandshake([&](
        const bedrock::BedrockServerClientHandshakeEvent& event
    ) {
        ++serverHandshakeEvents;
        serverHandshakeSequence = ++serverCallbackSequence;
        if (event.key.empty() ||
            server.status(event.connection) !=
                bedrock::BedrockServerClientStatus::Authenticating ||
            server.loginVerification(event.connection).has_value()) {
            serverHandshakeMismatch = true;
        }
    });
    server.onLogin([&](const bedrock::BedrockServerPacketEvent& event) {
        ++serverLoginEvents;
        serverLoginSequence = ++serverCallbackSequence;
        if (event.packet.name != "login" ||
            !server.loginVerification(event.connection).has_value()) {
            serverLoginMismatch = true;
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

    client.onSession([&](const bedrock::BedrockClientProfile& profile) {
        ++sessionEvents;
        sessionSequence = ++callbackSequence;
        const auto storedProfile = client.profile();
        const auto username = client.username();
        if (profile.name != "CppSmoke" ||
            profile.uuid != bedrock::uuidFrom("CppSmoke") ||
            profile.xuid != "0" || !storedProfile.has_value() ||
            storedProfile->name != profile.name ||
            storedProfile->uuid != profile.uuid ||
            storedProfile->xuid != profile.xuid || !username.has_value() ||
            *username != profile.name || !client.accessToken().empty() ||
            client.status() !=
                bedrock::BedrockNetworkClientStatus::Disconnected ||
            bedrock::BedrockNetworkClientTestAccess::queueRunning(client)) {
            sessionMismatch = true;
        }
    });
    client.onLoggingIn([&]() {
        ++loggingInEvents;
        loggingInSequence = ++callbackSequence;
        loggingInSawAuthenticatingStatus =
            client.status() ==
                bedrock::BedrockNetworkClientStatus::Authenticating;
    });
    client.onJoin([&]() {
        joinSequence = ++callbackSequence;
        joined = true;
    });
    client.on("client.server_handshake", [&](const auto& event) {
        clientHandshakeSequence = ++callbackSequence;
        if (event.packet.name != "server_to_client_handshake" ||
            event.packet.payload.empty() || joinSequence.load() == 0 ||
            client.status() !=
                bedrock::BedrockNetworkClientStatus::Initializing) {
            clientHandshakeMismatch = true;
        }
    });
    client.on("server_to_client_handshake", [&](const auto&) {
        rawHandshakeSequence = ++callbackSequence;
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
            sessionEvents.load() == 1 &&
            loggingInEvents.load() == 1 &&
            joined.load() &&
            clientHandshakeSequence.load() > 0 &&
            rawHandshakeSequence.load() > 0 &&
            serverHandshakeEvents.load() == 1 &&
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
    if (sessionEvents.load() != 1 || sessionMismatch.load()) {
        fail("offline session profile/accessToken/order mismatch");
    }
    if (loggingInEvents.load() != 1 ||
        !loggingInSawAuthenticatingStatus.load()) {
        fail("loggingIn count/status mismatch");
    }
    if (sessionSequence.load() == 0 ||
        loggingInSequence.load() <= sessionSequence.load() ||
        joinSequence.load() <= loggingInSequence.load()) {
        fail("session/loggingIn/join ordering mismatch");
    }
    if (clientHandshakeMismatch.load() ||
        clientHandshakeSequence.load() <= joinSequence.load() ||
        rawHandshakeSequence.load() <= clientHandshakeSequence.load()) {
        fail("client.server_handshake alias ordering mismatch");
    }
    if (serverLoggingInEvents.load() != 1 ||
        serverHandshakeEvents.load() != 1 ||
        serverLoginEvents.load() != 1 ||
        serverLoggingInMismatch.load() ||
        serverHandshakeMismatch.load() ||
        serverLoginMismatch.load() ||
        serverLoggingInSequence.load() == 0 ||
        serverHandshakeSequence.load() <= serverLoggingInSequence.load() ||
        serverLoginSequence.load() <= serverHandshakeSequence.load()) {
        fail("server loggingIn/handshake/login payload or ordering mismatch");
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

bool checkServerPlayerFacade() {
    const std::string version = "1.20.40";
    const auto packetCodec = bedrock::VersionedPacketCodec::forVersion(version);
    const auto startGame = packetCodec.makePacketByName("start_game", {
        0x02,                         // zigzag64 entity_id = 1
        0x95, 0x9a, 0xef, 0x3a,       // varint64 runtime_entity_id
        0x00,                         // zigzag32 player_gamemode
        0x00, 0x00, 0x00, 0x00,       // x
        0x00, 0x00, 0x00, 0x00,       // y
        0x00, 0x00, 0x00, 0x00        // z
    });
    const auto playerSpawn = packetCodec.makePacketByName(
        "play_status",
        {0x00, 0x00, 0x00, 0x03}
    );

    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {"Player facade smoke", "Player API"},
        .maxPlayers = 1,
        .offline = true
    });

    std::mutex playerMutex;
    std::optional<bedrock::Player> player;
    std::mutex eventsMutex;
    std::vector<std::string> events;
    std::atomic<bool> mismatch {false};
    std::atomic<bool> spawned {false};
    std::atomic<bool> playerClosed {false};
    std::atomic<bool> clientClosed {false};
    std::atomic<bool> clientError {false};

    const auto record = [&](std::string event) {
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.push_back(std::move(event));
    };

    server.on("connect", [&](const bedrock::Player& connected) {
        record("connect");
        const auto clients = server.clients();
        const auto exactClient = server.client(connected.key());
        if (connected.server != &server ||
            &connected.owner() != &server ||
            connected.key() != connected.address + ":" +
                std::to_string(connected.port) ||
            clients.size() != 1 ||
            clients.find(connected.key()) == clients.end() ||
            !exactClient || exactClient->clientGuid != connected.clientGuid ||
            connected.status() != bedrock::BedrockServerClientStatus::Authenticating ||
            connected.profile().has_value() ||
            connected.version().has_value()) {
            mismatch = true;
        }
        {
            std::lock_guard<std::mutex> lock(playerMutex);
            player = connected;
        }

        connected.onLoggingIn([&, connected](
            const bedrock::BedrockServerLoggingInEvent& event
        ) {
            record("loggingIn");
            if (event.packet.name != "login" ||
                event.login.protocolVersion != bedrock::protocolVersionFor(version) ||
                connected.status() !=
                    bedrock::BedrockServerClientStatus::Authenticating ||
                connected.profile().has_value() ||
                connected.getUserData().has_value()) {
                mismatch = true;
            }
        });
        connected.onServerClientHandshake([&, connected](
            const bedrock::BedrockServerClientHandshakeEvent& event
        ) {
            record("server.client_handshake");
            if (event.key.empty() || connected.profile().has_value() ||
                connected.status() !=
                    bedrock::BedrockServerClientStatus::Authenticating) {
                mismatch = true;
            }
        });
        connected.on("login", bedrock::Player::PacketHandler(
            [&, connected](const bedrock::BedrockServerPacketEvent& event) {
                record("login");
                const auto profile = connected.profile();
                const auto userData = connected.getUserData();
                const auto skinData = connected.skinData();
                const auto clientVersion = connected.version();
                const auto* displayName = userData
                    ? userData->get("displayName")
                    : nullptr;
                if (event.packet.name != "login" || !profile ||
                    profile->name != "PlayerFacadeSmoke" ||
                    profile->uuid != bedrock::uuidFrom("PlayerFacadeSmoke") ||
                    profile->xuid != "0" || !displayName ||
                    displayName->kind != bedrock::ProtoDefValue::Kind::String ||
                    displayName->stringValue != "PlayerFacadeSmoke" ||
                    !skinData || !clientVersion ||
                    *clientVersion != bedrock::protocolVersionFor(version) ||
                    connected.versionLessThan(version) ||
                    connected.versionGreaterThan(version) ||
                    !connected.versionGreaterThanOrEqualTo(version) ||
                    !connected.versionLessThanOrEqualTo(version) ||
                    !connected.versionGreaterThan("1.20.30") ||
                    !connected.versionLessThan("1.20.50")) {
                    mismatch = true;
                }
            }
        ));
        connected.onStatus([&, connected](bedrock::BedrockServerClientStatus next) {
            if (next == bedrock::BedrockServerClientStatus::Initializing) {
                record("status:initializing");
                if (connected.status() !=
                    bedrock::BedrockServerClientStatus::Authenticating) {
                    mismatch = true;
                }
            } else if (next == bedrock::BedrockServerClientStatus::Connecting) {
                record("status:manual-connecting");
                if (connected.status() !=
                    bedrock::BedrockServerClientStatus::Initialized) {
                    mismatch = true;
                }
            } else if (next == bedrock::BedrockServerClientStatus::Initialized) {
                const auto oldStatus = connected.status();
                if (oldStatus == bedrock::BedrockServerClientStatus::Initializing) {
                    record("status:initialized");
                } else if (oldStatus ==
                    bedrock::BedrockServerClientStatus::Connecting) {
                    record("status:manual-initialized");
                } else {
                    mismatch = true;
                }
            }
        });
        connected.on("join", bedrock::Player::VoidHandler([&, connected]() {
            record("join");
            if (connected.status() !=
                bedrock::BedrockServerClientStatus::Initializing) {
                mismatch = true;
            }
        }));
        connected.on(
            "client_to_server_handshake",
            bedrock::Player::PacketHandler(
                [&](const bedrock::BedrockServerPacketEvent&) {
                    record("named:client_to_server_handshake");
                }
            )
        );
        connected.on(
            "set_local_player_as_initialized",
            bedrock::Player::PacketHandler(
                [&](const bedrock::BedrockServerPacketEvent&) {
                    record("named:set_local_player_as_initialized");
                }
            )
        );
        connected.on("packet", bedrock::Player::PacketHandler(
            [&](const bedrock::BedrockServerPacketEvent& event) {
                if (event.packet.name == "client_to_server_handshake" ||
                    event.packet.name == "set_local_player_as_initialized") {
                    record("packet:" + event.packet.name);
                }
            }
        ));
        connected.on("spawn", bedrock::Player::VoidHandler([&, connected]() {
            record("spawn");
            spawned = true;
            if (connected.status() !=
                bedrock::BedrockServerClientStatus::Initialized) {
                mismatch = true;
            }
            connected.setStatus(bedrock::BedrockServerClientStatus::Connecting);
            connected.setStatus(bedrock::BedrockServerClientStatus::Initialized);
        }));
        connected.on("close", bedrock::Player::VoidHandler([&, connected]() {
            record("close");
            playerClosed = true;
            const auto clients = server.clients();
            const auto exactClient = server.client(connected.key());
            if (clients.size() != 1 || !exactClient ||
                exactClient->clientGuid != connected.clientGuid ||
                connected.status() !=
                bedrock::BedrockServerClientStatus::Initialized) {
                mismatch = true;
            }
        }));
    });

    server.onJoin([&](const bedrock::Player& connected) {
        // Exercise Connection#sendBuffer(false) and the deterministic C++
        // _tick facade through the Player itself.
        connected.sendBuffer(startGame.fullPacket);
        connected.sendBuffer(playerSpawn.fullPacket);
        connected.sendQueued();
    });
    server.listen();

    bedrock::BedrockNetworkClient client({
        .host = "127.0.0.1",
        .port = server.boundPort(),
        .username = "PlayerFacadeSmoke",
        .version = version,
        .offline = true,
        .connectTimeoutMs = 1000
    });
    client.onClose([&](const std::string&) { clientClosed = true; });
    client.onError([&](const std::string& error) {
        clientError = true;
        std::cerr << "[NETWORK-CLIENT-SMOKE] player facade client error: "
                  << error << "\n";
    });

    if (!client.connect()) {
        server.close();
        std::cerr << "[NETWORK-CLIENT-SMOKE] player facade connect failed\n";
        return false;
    }

    for (int i = 0; i < 250 && !spawned.load() && !clientError.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::optional<bedrock::Player> connected;
    {
        std::lock_guard<std::mutex> lock(playerMutex);
        connected = player;
    }
    if (connected && spawned.load()) {
        connected->disconnect("Player facade close");
    } else {
        mismatch = true;
        client.close("player facade setup failed");
    }

    for (int i = 0; i < 150 &&
         (!playerClosed.load() || !clientClosed.load());
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    client.close("player facade cleanup");
    server.close();
    if (!server.clients().empty() ||
        (connected && server.client(connected->key()).has_value())) {
        mismatch = true;
    }

    std::vector<std::string> observed;
    {
        std::lock_guard<std::mutex> lock(eventsMutex);
        observed = events;
    }
    const std::vector<std::string> expected {
        "connect",
        "loggingIn",
        "server.client_handshake",
        "login",
        "status:initializing",
        "join",
        "named:client_to_server_handshake",
        "packet:client_to_server_handshake",
        "status:initialized",
        "spawn",
        "status:manual-connecting",
        "status:manual-initialized",
        "named:set_local_player_as_initialized",
        "packet:set_local_player_as_initialized",
        "close"
    };
    if (mismatch.load() || clientError.load() ||
        !spawned.load() || !playerClosed.load() || !clientClosed.load() ||
        observed != expected) {
        std::cerr << "[NETWORK-CLIENT-SMOKE] Player facade mismatch; events:";
        for (const auto& event : observed) std::cerr << " " << event;
        std::cerr << "\n";
        return false;
    }

    std::cout << "[NETWORK-CLIENT-SMOKE] Player facade ok\n";
    return true;
}

bool checkManualClientInitialization() {
    const std::string version = "1.20.40";
    const auto packetCodec = bedrock::VersionedPacketCodec::forVersion(version);
    const auto startGame = packetCodec.makePacketByName("start_game", {
        0x02,                         // zigzag64 entity_id = 1
        0x95, 0x9a, 0xef, 0x3a,       // varint64 runtime_entity_id = 123456789
        0x00,                         // zigzag32 player_gamemode = 0
        0x00, 0x00, 0x00, 0x00,       // x = 0
        0x00, 0x00, 0x00, 0x00,       // y = 0
        0x00, 0x00, 0x00, 0x00        // z = 0
    });
    const auto playerSpawn = packetCodec.makePacketByName(
        "play_status",
        {0x00, 0x00, 0x00, 0x03}
    );
    const std::vector<uint8_t> expectedInitialized {
        0x71, 0x95, 0x9a, 0xef, 0x3a
    };

    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = version,
        .motd = {"Manual bot init smoke", "Connection API"},
        .maxPlayers = 1,
        .offline = true
    });

    std::atomic<int> initializedPackets {0};
    std::atomic<int> serverSpawnEvents {0};
    std::atomic<int> serverJoinEvents {0};
    std::atomic<bool> clientHandlersReady {false};
    std::atomic<bool> initializedPacketMismatch {false};
    server.on("set_local_player_as_initialized", [&]
        (const bedrock::BedrockServerPacketEvent& event) {
            ++initializedPackets;
            if (event.packet.fullPacket != expectedInitialized) {
                initializedPacketMismatch = true;
            }
        }
    );
    server.onSpawn([&](const bedrock::BedrockServerConnection&) {
        ++serverSpawnEvents;
    });
    server.onJoin([&](const bedrock::BedrockServerConnection& connection) {
        ++serverJoinEvents;
        // createBot starts connecting before it returns. Hold this synthetic
        // server's first game packets until the test has attached the public
        // listeners, without changing the production factory behavior.
        for (int i = 0; i < 200 && !clientHandlersReady.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        server.sendPackets(connection, {startGame, playerSpawn});
    });
    server.listen();

    auto client = bedrock::createBot(bedrock::BotOptions {
        .host = "127.0.0.1",
        .port = server.boundPort(),
        .username = "ManualInitSmoke",
        .version = version,
        .offline = true,
        .autoInitPlayer = false,
        .connectTimeout = 1000,
        .skipPing = true,
        .followPort = false
    });

    std::atomic<int> playerSpawnPackets {0};
    std::atomic<int> playStatusEvents {0};
    std::atomic<int> clientSpawnEvents {0};
    std::atomic<int> initializedStatusEvents {0};
    std::atomic<int> closeEvents {0};
    std::atomic<bool> manualInitFinished {false};
    std::atomic<bool> mismatch {false};
    std::atomic<bool> gotError {false};

    client.onStatus([&](bedrock::ClientStatus next) {
        if (next != bedrock::ClientStatus::Initialized) return;
        ++initializedStatusEvents;
        if (client.status() != bedrock::ClientStatus::Initializing) {
            mismatch = true;
        }
    });
    client.onSpawn([&]() {
        ++clientSpawnEvents;
    });
    // Deliberately omit a start_game listener: startGameData() must retain and
    // decode the packet independently of event subscriptions.
    client.on("play_status", [&](const bedrock::Packet& packet) {
        ++playStatusEvents;
        const auto playStatus = packet.get("status");
        if (playStatus != "player_spawn") return;
        ++playerSpawnPackets;

        const auto entityId = client.entityId();
        const auto startGameData = client.startGameData();
        if (client.status() != bedrock::ClientStatus::Initializing ||
            !entityId || *entityId != kRuntimeEntityId ||
            !startGameData || startGameData->name != "start_game" ||
            !startGameData->ok ||
            startGameData->get("runtime_entity_id") !=
                std::to_string(kRuntimeEntityId) ||
            clientSpawnEvents.load() != 0) {
            mismatch = true;
        }

        client.updateItemPalette(bedrock::array({
            bedrock::object({
                {"name", bedrock::str("minecraft:shield")},
                {"runtime_id", bedrock::u64(355)}
            })
        }));
        if (client.network().packetVariableStore()->variable("ShieldItemID") !=
            std::optional<std::string>("355")) {
            mismatch = true;
        }

        client.write("set_local_player_as_initialized", bedrock::object({
            {"runtime_entity_id", bedrock::u64(kRuntimeEntityId)}
        }));
        client.setStatus(bedrock::ClientStatus::Initialized);
        if (client.status() != bedrock::ClientStatus::Initialized) {
            mismatch = true;
        }
        manualInitFinished = true;
    });
    client.onClose([&](const std::string&) {
        ++closeEvents;
    });
    client.onError([&](const std::string& error) {
        gotError = true;
        std::cerr << "[NETWORK-CLIENT-SMOKE] manual init error: "
                  << error << "\n";
    });
    clientHandlersReady = true;

    for (int i = 0; i < 300 &&
         (!manualInitFinished.load() || initializedPackets.load() != 1 ||
          serverSpawnEvents.load() != 1) &&
         !gotError.load();
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    client.close("manual init cleanup");
    server.close();

    if (!manualInitFinished.load() || mismatch.load() || gotError.load() ||
        initializedPacketMismatch.load() || playerSpawnPackets.load() != 1 ||
        clientSpawnEvents.load() != 0 || initializedStatusEvents.load() != 1 ||
        initializedPackets.load() != 1 || serverJoinEvents.load() != 1 ||
        serverSpawnEvents.load() != 1 ||
        closeEvents.load() != 1 ||
        client.status() != bedrock::ClientStatus::Disconnected) {
        std::cerr << "[NETWORK-CLIENT-SMOKE] manual bot initialization mismatch"
                  << " packets=" << playerSpawnPackets.load()
                  << " play-status-events=" << playStatusEvents.load()
                  << " client-spawn=" << clientSpawnEvents.load()
                  << " status=" << initializedStatusEvents.load()
                  << " initialized=" << initializedPackets.load()
                  << " server-join=" << serverJoinEvents.load()
                  << " server-spawn=" << serverSpawnEvents.load()
                  << " close=" << closeEvents.load()
                  << " factory-initialized=" << client.initialized()
                  << " auto-connect=" << client.autoConnectStarted()
                  << " connect-worker=" << client.connectWorkerStarted()
                  << " clients=" << server.clients().size() << "\n";
        return false;
    }

    std::cout << "[NETWORK-CLIENT-SMOKE] manual bot initialization ok\n";
    return true;
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
    std::atomic<int> sessionEvents {0};
    std::atomic<int> loggingInEvents {0};
    std::atomic<int> callbackSequence {0};
    std::atomic<int> sessionSequence {0};
    std::atomic<int> loggingInSequence {0};
    std::atomic<bool> kickMismatch {false};
    std::atomic<bool> kickSawOldStatus {false};
    std::atomic<bool> sessionMismatch {false};
    std::atomic<bool> loggingInSawAuthenticatingStatus {false};
    std::atomic<bool> kickSawLoggingIn {false};
    std::atomic<bool> gotError {false};

    // Deliberately register no `packet` or `disconnect` handler.  A missing
    // high-level kick->disconnect decoder mapping would leave these fields
    // empty and make this check fail.
    client.onSession([&](const bedrock::BedrockClientProfile& profile) {
        ++sessionEvents;
        sessionSequence = ++callbackSequence;
        const auto stored = client.profile();
        const auto username = client.username();
        if (profile.name != "CppKickSmoke" ||
            profile.uuid != bedrock::uuidFrom("CppKickSmoke") ||
            profile.xuid != "0" || !stored.has_value() ||
            stored->uuid != profile.uuid || !username.has_value() ||
            *username != profile.name || !client.accessToken().empty() ||
            client.status() != bedrock::ClientStatus::Disconnected) {
            sessionMismatch = true;
        }
    });
    client.onLoggingIn([&]() {
        ++loggingInEvents;
        loggingInSequence = ++callbackSequence;
        loggingInSawAuthenticatingStatus =
            client.status() == bedrock::ClientStatus::Authenticating;
    });
    client.on("kick", [&](const bedrock::Packet& packet) {
        ++kickEvents;
        kickSawLoggingIn = loggingInEvents.load() == 1;
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

    if (sessionEvents.load() != 1 || sessionMismatch.load() ||
        sessionSequence.load() == 0 ||
        loggingInSequence.load() <= sessionSequence.load() ||
        loggingInEvents.load() != 1 ||
        !loggingInSawAuthenticatingStatus.load() ||
        !kickSawLoggingIn.load() ||
        kickEvents.load() != 1 ||
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

bool checkQueueWorkerExceptionBoundary() {
    const auto runCase = [](bool throwingCloseListener) {
        bedrock::BedrockNetworkClient client({
            .host = "127.0.0.1",
            .port = 9,
            .username = "QueueFailure",
            .version = "1.20.40",
            .offline = true,
            .batchingIntervalMs = 5,
            .trackWorld = false
        });
        bedrock::BedrockNetworkClientTestAccess::armQueueFailure(
            client,
            [](const std::vector<uint8_t>&) {
                throw std::runtime_error("queue send boom");
            }
        );

        std::atomic<int> errors {0};
        std::atomic<int> closes {0};
        std::mutex reasonMutex;
        std::string errorReason;
        std::string closeReason;
        client.onError([&](const std::string& reason) {
            {
                std::lock_guard<std::mutex> lock(reasonMutex);
                errorReason = reason;
            }
            ++errors;
        });
        client.onClose([&](const std::string& reason) {
            {
                std::lock_guard<std::mutex> lock(reasonMutex);
                closeReason = reason;
            }
            ++closes;
            if (throwingCloseListener) {
                throw std::runtime_error("queue close listener boom");
            }
        });

        if (!bedrock::BedrockNetworkClientTestAccess::startQueue(client)) {
            return false;
        }
        client.queue(
            "client_cache_status",
            bedrock::ProtoDefValue::object({
                {"enabled", bedrock::ProtoDefValue::boolean(false)}
            })
        );
        for (int attempt = 0;
             attempt < 200 &&
                 client.status() !=
                     bedrock::BedrockNetworkClientStatus::Disconnected;
             ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        std::string capturedError;
        std::string capturedClose;
        {
            std::lock_guard<std::mutex> lock(reasonMutex);
            capturedError = errorReason;
            capturedClose = closeReason;
        }
        const std::string expected =
            "Network queue worker failure: queue send boom";
        const bool contained = errors.load() == 1 && closes.load() == 1 &&
            capturedError == expected && capturedClose == expected &&
            client.status() ==
                bedrock::BedrockNetworkClientStatus::Disconnected &&
            !bedrock::BedrockNetworkClientTestAccess::queueRunning(client) &&
            !bedrock::BedrockNetworkClientTestAccess::hasTransport(client) &&
            bedrock::BedrockNetworkClientTestAccess::stopRequested(client);

        // Reap a completed-but-joinable queue thread from an external thread;
        // the failure path itself must never attempt a self-join.
        client.close("queue failure cleanup");
        const bool reaped =
            !bedrock::BedrockNetworkClientTestAccess::queueThreadJoinable(client);
        if (!contained || !reaped) {
            std::cerr << "[NETWORK-CLIENT-SMOKE] queue boundary mismatch: throwClose="
                      << throwingCloseListener << " errors=" << errors.load()
                      << " closes=" << closes.load() << " error="
                      << capturedError << " close=" << capturedClose
                      << " running="
                      << bedrock::BedrockNetworkClientTestAccess::queueRunning(
                             client
                         )
                      << " transport="
                      << bedrock::BedrockNetworkClientTestAccess::hasTransport(
                             client
                         )
                      << " joinable="
                      << bedrock::BedrockNetworkClientTestAccess::
                             queueThreadJoinable(client)
                      << "\n";
        }
        return contained && reaped;
    };

    const bool ok = runCase(false) && runCase(true);
    if (ok) {
        std::cout << "[NETWORK-CLIENT-SMOKE] queue worker boundary ok\n";
    }
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--queue-worker-only") {
        return checkQueueWorkerExceptionBoundary() ? 0 : 1;
    }
    bool ok = true;
    ok = checkSubChunkWorldTracking() && ok;
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
        CloseMode::LocalClose,
        "snappy",
        0
    ) && ok;
    ok = checkVersion(
        "1.21.100",
        LifecycleOrder::Normal,
        CloseMode::RemoteDisconnect,
        "snappy",
        0
    ) && ok;
    ok = checkServerPlayerFacade() && ok;
    ok = checkManualClientInitialization() && ok;
    ok = checkHighLevelKickDecoding() && ok;
    ok = checkEncryptedErrorSurface() && ok;
    ok = checkQueueWorkerExceptionBoundary() && ok;
    return ok ? 0 : 1;
}
