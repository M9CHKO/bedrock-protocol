#include <bedrock/LoginPacket.hpp>
#include <bedrock/Options.hpp>
#include <bedrock/client/BedrockNetworkClient.hpp>
#include <bedrock/protocol/VersionedPacketCodec.hpp>
#include <bedrock/protocol/VersionedPayloadReader.hpp>
#include <bedrock/server/BedrockServer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace bedrock {

struct BedrockServerTestAccess {
    static BedrockServerConnection addPlainPlayer(
        BedrockServer& server,
        uint16_t port = 19320,
        uint64_t clientGuid = 0x1020304050607080ull
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
        connection.playerEvents =
            std::make_shared<BedrockServerPlayerEventState>();

        const auto key = BedrockServer::connectionKey(connection.peer);
        auto session = std::make_shared<BedrockServer::SessionState>();
        {
            std::lock_guard<std::mutex> lock(server.serverStateMutex_);
            server.connections_[key] = connection;
            server.sessions_[key] = std::move(session);
        }
        return connection;
    }

    static bool handleBuiltIn(
        BedrockServer& server,
        const BedrockServerConnection& connection,
        const VersionedGamePacket& packet
    ) {
        return server.handleBuiltInPacket(connection, packet);
    }
};

} // namespace bedrock

namespace {

using namespace std::chrono_literals;

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[SERVER-PROTOCOL-VERSION-SMOKE] " << message << "\n";
    }
    return condition;
}

bool waitFor(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = 6000ms
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

bool checkLegacyLoginGate() {
    const std::string serverVersion = "1.19.20";
    const auto serverProtocol = static_cast<int32_t>(
        bedrock::protocolVersionFor(serverVersion)
    );

    bedrock::BedrockServer server({
        .version = serverVersion,
        .offline = true
    });
    const auto connection =
        bedrock::BedrockServerTestAccess::addPlainPlayer(server);

    bool ok = true;
    ok &= check(
        server.handleClientProtocolVersion(connection, serverProtocol),
        "equal legacy protocol was rejected"
    );
    ok &= check(
        server.handleClientProtocolVersion(connection, serverProtocol - 1),
        "older legacy protocol was rejected"
    );
    ok &= check(
        server.handleClientProtocolVersion(connection, -1),
        "signed negative protocol did not follow JavaScript comparison"
    );

    std::atomic<int> loginEvents {0};
    std::atomic<int> loggingInEvents {0};
    std::atomic<int> handshakeEvents {0};
    std::atomic<int> closeEvents {0};
    std::atomic<int> callbackSequence {0};
    std::atomic<int> firstLoggingInSequence {0};
    std::atomic<int> firstCloseSequence {0};
    std::atomic<bool> loggingInMismatch {false};
    server.onLogin([&](const bedrock::BedrockServerPacketEvent&) {
        ++loginEvents;
    });
    server.onServerClientHandshake([&](const auto&) {
        ++handshakeEvents;
    });
    server.onLoggingIn([&](const bedrock::BedrockServerLoggingInEvent& event) {
        const int eventIndex = ++loggingInEvents;
        const int sequence = ++callbackSequence;
        if (eventIndex == 1) {
            firstLoggingInSequence = sequence;
        }
        const uint32_t expectedProtocol = event.connection.port == connection.port
            ? bedrock::protocolVersionFor("1.19.30")
            : bedrock::protocolVersionFor(serverVersion);
        const std::string expectedClient = event.connection.port == connection.port
            ? "invalid-client-jwt"
            : "invalid-equal-client-jwt";
        if (event.packet.name != "login" ||
            event.login.protocolVersion != expectedProtocol ||
            event.login.identity != "{}" ||
            event.login.client != expectedClient ||
            server.status(event.connection) !=
                bedrock::BedrockServerClientStatus::Authenticating ||
            server.loginVerification(event.connection).has_value()) {
            loggingInMismatch = true;
        }
    });
    connection.onClose([&]() {
        firstCloseSequence = ++callbackSequence;
        ++closeEvents;
    });

    const auto newerProtocol = bedrock::protocolVersionFor("1.19.30");
    const auto fullLogin = bedrock::LoginPacketCodec::encode(
        newerProtocol,
        "{}",
        "invalid-client-jwt"
    );
    const auto packet = bedrock::VersionedPacketCodec::forVersion(serverVersion)
        .decodeFullPacket(fullLogin);
    const bool handled = bedrock::BedrockServerTestAccess::handleBuiltIn(
        server,
        connection,
        packet
    );

    ok &= check(handled, "legacy login was not consumed by Player.readPacket");
    ok &= check(
        server.status(connection) ==
            bedrock::BedrockServerClientStatus::Disconnected,
        "newer legacy login did not close synchronously via failed_spawn"
    );
    ok &= check(closeEvents.load() == 1, "legacy failed_spawn close event mismatch");
    ok &= check(loggingInEvents.load() == 1, "rejected legacy login missed loggingIn");
    ok &= check(!loggingInMismatch.load(), "legacy loggingIn payload/status mismatch");
    ok &= check(
        firstLoggingInSequence.load() > 0 &&
            firstLoggingInSequence.load() < firstCloseSequence.load(),
        "legacy loggingIn was not emitted before failed_spawn close"
    );
    ok &= check(loginEvents.load() == 0, "rejected legacy login reached login handlers");
    ok &= check(handshakeEvents.load() == 0, "rejected legacy login reached handshake");

    const auto invalidConnection =
        bedrock::BedrockServerTestAccess::addPlainPlayer(
            server,
            19321,
            0x1020304050607081ull
        );
    std::atomic<int> invalidCloseEvents {0};
    invalidConnection.onClose([&]() {
        ++invalidCloseEvents;
    });
    const auto invalidLogin = bedrock::LoginPacketCodec::encode(
        static_cast<uint32_t>(serverProtocol),
        "{}",
        "invalid-equal-client-jwt"
    );
    const auto invalidPacket =
        bedrock::VersionedPacketCodec::forVersion(serverVersion)
            .decodeFullPacket(invalidLogin);
    const bool invalidHandled = bedrock::BedrockServerTestAccess::handleBuiltIn(
        server,
        invalidConnection,
        invalidPacket
    );

    ok &= check(invalidHandled, "invalid equal-version login was not consumed");
    ok &= check(
        loggingInEvents.load() == 2,
        "invalid JWT login missed pre-verification loggingIn"
    );
    ok &= check(!loggingInMismatch.load(), "invalid JWT loggingIn payload/status mismatch");
    ok &= check(loginEvents.load() == 0, "invalid JWT reached authenticated login");
    ok &= check(handshakeEvents.load() == 0, "invalid JWT reached handshake");
    ok &= check(
        server.status(invalidConnection) ==
            bedrock::BedrockServerClientStatus::Authenticating,
        "invalid JWT was closed before loggingIn returned"
    );
    ok &= check(
        waitFor([&]() { return invalidCloseEvents.load() == 1; }),
        "invalid JWT delayed authentication close timed out"
    );

    server.close();
    if (ok) {
        std::cout << "[SERVER-PROTOCOL-VERSION-SMOKE] legacy login gate ok\n";
    }
    return ok;
}

bool checkLoggingInExceptionBoundary() {
    const std::string version = "1.19.20";
    bedrock::BedrockServer server({
        .version = version,
        .offline = true
    });
    const auto connection = bedrock::BedrockServerTestAccess::addPlainPlayer(
        server,
        19322,
        0x1020304050607082ull
    );

    std::atomic<int> closeEvents {0};
    std::atomic<int> loginEvents {0};
    connection.onClose([&]() { ++closeEvents; });
    server.onLogin([&](const bedrock::BedrockServerPacketEvent&) {
        ++loginEvents;
    });
    server.onLoggingIn([](const bedrock::BedrockServerLoggingInEvent&) {
        throw std::runtime_error("loggingIn listener failure");
    });

    const auto fullLogin = bedrock::LoginPacketCodec::encode(
        bedrock::protocolVersionFor(version),
        "{}",
        "invalid-client-jwt"
    );
    const auto packet = bedrock::VersionedPacketCodec::forVersion(version)
        .decodeFullPacket(fullLogin);

    bool propagated = false;
    try {
        (void) bedrock::BedrockServerTestAccess::handleBuiltIn(
            server,
            connection,
            packet
        );
    } catch (const std::runtime_error& error) {
        propagated = std::string(error.what()) == "loggingIn listener failure";
    }

    bool ok = true;
    ok &= check(propagated, "loggingIn listener exception was swallowed as auth failure");
    ok &= check(closeEvents.load() == 0, "loggingIn exception scheduled an auth close");
    ok &= check(loginEvents.load() == 0, "loggingIn exception reached authenticated login");
    ok &= check(
        server.status(connection) ==
            bedrock::BedrockServerClientStatus::Authenticating,
        "loggingIn exception changed Player status"
    );

    server.close();
    if (ok) {
        std::cout << "[SERVER-PROTOCOL-VERSION-SMOKE] loggingIn exception boundary ok\n";
    }
    return ok;
}

bool checkModernNetworkSettingsGate() {
    const std::string serverVersion = "1.20.61";
    const std::string clientVersion = "1.21.100";

    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = serverVersion,
        .motd = {{"motd", "Protocol Version Gate"}},
        .maxPlayers = 1,
        .offline = true
    });

    std::atomic<int> serverLoginEvents {0};
    std::atomic<int> serverLoggingInEvents {0};
    std::atomic<int> serverHandshakeEvents {0};
    std::atomic<int> serverJoinEvents {0};
    std::atomic<int> serverCloseEvents {0};
    server.onLogin([&](const bedrock::BedrockServerPacketEvent&) {
        ++serverLoginEvents;
    });
    server.onLoggingIn([&](const bedrock::BedrockServerLoggingInEvent&) {
        ++serverLoggingInEvents;
    });
    server.onServerClientHandshake([&](const auto&) {
        ++serverHandshakeEvents;
    });
    server.onJoin([&](const bedrock::BedrockServerConnection&) {
        ++serverJoinEvents;
    });
    server.onDisconnect([&](const bedrock::BedrockServerConnection&) {
        ++serverCloseEvents;
    });
    server.listen();

    bedrock::BedrockNetworkClientOptions options;
    options.host = "127.0.0.1";
    options.port = server.boundPort();
    options.username = "ProtocolVersionSmoke";
    options.profile = options.username;
    options.version = clientVersion;
    options.offline = true;
    options.connectTimeoutMs = 1500;
    options.batchingIntervalMs = 5;

    bedrock::BedrockNetworkClient client(std::move(options));
    std::atomic<int> playStatusEvents {0};
    std::atomic<int32_t> playStatus {-1};
    std::atomic<int> networkSettingsEvents {0};
    std::atomic<int> joinEvents {0};
    std::atomic<int> loggingInEvents {0};
    std::atomic<int> closeEvents {0};
    std::atomic<int> errorEvents {0};

    client.on("play_status", [&](const bedrock::BedrockNetworkClientPacketEvent& event) {
        playStatus = bedrock::VersionedPayloadReader::readPlayStatus(
            event.packet
        ).status;
        ++playStatusEvents;
    });
    client.on("network_settings", [&](const bedrock::BedrockNetworkClientPacketEvent&) {
        ++networkSettingsEvents;
    });
    client.onJoin([&]() {
        ++joinEvents;
    });
    client.onLoggingIn([&]() {
        ++loggingInEvents;
    });
    client.onClose([&]() {
        ++closeEvents;
    });
    client.onError([&](const std::string& error) {
        ++errorEvents;
        std::cerr << "[SERVER-PROTOCOL-VERSION-SMOKE] client error: "
                  << error << "\n";
    });

    bool ok = check(client.connect(), "modern mismatch transport did not connect");
    const bool completed = waitFor([&]() {
        return playStatusEvents.load() > 0 &&
            serverCloseEvents.load() > 0 &&
            closeEvents.load() > 0;
    });

    ok &= check(completed, "modern failed_spawn lifecycle timed out");
    ok &= check(playStatusEvents.load() == 1, "modern play_status count mismatch");
    ok &= check(playStatus.load() == 2, "newer client did not receive failed_spawn");
    ok &= check(
        networkSettingsEvents.load() == 0,
        "network_settings was sent after rejecting a newer client"
    );
    ok &= check(serverLoginEvents.load() == 0, "rejected modern client reached login");
    ok &= check(
        serverLoggingInEvents.load() == 0,
        "modern request rejection unexpectedly reached server loggingIn"
    );
    ok &= check(
        serverHandshakeEvents.load() == 0,
        "modern request rejection unexpectedly reached server handshake"
    );
    ok &= check(serverJoinEvents.load() == 0, "rejected modern client reached join");
    ok &= check(joinEvents.load() == 0, "rejected modern client emitted client join");
    ok &= check(
        loggingInEvents.load() == 0,
        "modern request rejection unexpectedly sent client login"
    );
    ok &= check(errorEvents.load() == 0, "normal version rejection emitted an error");

    client.close("protocol version smoke cleanup");
    server.close("protocol version smoke cleanup");
    if (ok) {
        std::cout << "[SERVER-PROTOCOL-VERSION-SMOKE] modern request gate ok\n";
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = checkLegacyLoginGate() && ok;
    ok = checkLoggingInExceptionBoundary() && ok;
    ok = checkModernNetworkSettingsGate() && ok;
    return ok ? 0 : 1;
}
