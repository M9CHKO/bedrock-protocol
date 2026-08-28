#include <bedrock/Client.hpp>
#include <bedrock/BedrockProtocol.hpp>
#include <bedrock/BedrockProtocolCpp.hpp>
#include <bedrock/Packet.hpp>
#include <bedrock/PacketFromEvent.hpp>
#include <bedrock/bedrock.hpp>
#include <bedrock/bot/BedrockBot.hpp>
#include <bedrock/client/BedrockClient.hpp>
#include <bedrock/legacy/Client.hpp>

#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

static_assert(std::is_same_v<
    decltype(bedrock::createClient(std::declval<bedrock::Options>())),
    bedrock::Client
>);

static_assert(std::is_aggregate_v<bedrock::ClientOptions>);
static_assert(std::is_same_v<bedrock::BotOptions, bedrock::ClientOptions>);
static_assert(std::is_aggregate_v<bedrock::ServerOptions>);
static_assert(!std::is_same_v<bedrock::ServerOptions, bedrock::BedrockServerOptions>);
static_assert(std::is_aggregate_v<bedrock::RelayOptions>);
static_assert(std::is_same_v<bedrock::Player, bedrock::BedrockServerConnection>);
static_assert(std::is_same_v<
    decltype(bedrock::createServer(std::declval<bedrock::ServerOptions>())),
    bedrock::BedrockServer
>);
static_assert(std::is_same_v<
    decltype(bedrock::createServer({})),
    bedrock::BedrockServer
>);
static_assert(std::is_same_v<
    decltype(bedrock::createServer(std::declval<bedrock::BedrockServerOptions>())),
    bedrock::BedrockServer
>);
static_assert(std::is_same_v<
    decltype(bedrock::createRelay(std::declval<bedrock::RelayOptions>())),
    bedrock::Relay
>);
static_assert(std::is_same_v<
    decltype(bedrock::createRelay({})),
    bedrock::Relay
>);
static_assert(std::is_same_v<
    decltype(bedrock::createPacketRelay(
        std::declval<bedrock::BedrockRelayOptions>()
    )),
    bedrock::BedrockRelay
>);
static_assert(std::is_same_v<
    decltype(bedrock::createRelay(
        std::declval<bedrock::BedrockRelayOptions>()
    )),
    bedrock::BedrockRelay
>);
static_assert(std::is_same_v<
    decltype(bedrock::createLiveRelay(
        std::declval<bedrock::BedrockLiveRelayOptions>()
    )),
    bedrock::BedrockLiveRelay
>);
static_assert(std::is_same_v<
    decltype(std::declval<bedrock::BedrockServer&>().listen()),
    bedrock::ServerListenResult
>);
static_assert(std::is_same_v<
    decltype(std::declval<bedrock::Relay&>().listen()),
    bedrock::ServerListenResult
>);
static_assert(std::is_same_v<
    decltype(std::declval<const bedrock::BedrockServer&>().clients()),
    bedrock::BedrockServer::ClientMap
>);
static_assert(std::is_same_v<
    decltype(std::declval<const bedrock::Relay&>().clients()),
    bedrock::Relay::ClientMap
>);
static_assert(std::is_same_v<
    decltype(bedrock::createClient(std::declval<bedrock::ClientOptions>())),
    bedrock::Client
>);
static_assert(std::is_same_v<
    decltype(bedrock::createClient({})),
    bedrock::Client
>);
static_assert(std::is_same_v<
    decltype(bedrock::createBot(std::declval<bedrock::BotOptions>())),
    bedrock::Client
>);
static_assert(std::is_same_v<
    decltype(bedrock::createBot({})),
    bedrock::Client
>);
static_assert(std::is_same_v<
    decltype(bedrock::createBedrockBot(
        std::declval<bedrock::BedrockBotOptions>()
    )),
    bedrock::BedrockBot
>);
static_assert(std::is_same_v<
    decltype(bedrock::createBot(
        std::declval<bedrock::BedrockBotOptions>()
    )),
    bedrock::BedrockBot
>);
static_assert(std::is_same_v<
    decltype(bedrock::createNetworkClient({})),
    bedrock::BedrockNetworkClient
>);
static_assert(std::is_same_v<
    decltype(bedrock::createManualClient(std::declval<bedrock::Options>())),
    bedrock::Client
>);
static_assert(std::is_same_v<
    decltype(std::declval<const bedrock::Client&>().startGameData()),
    std::optional<bedrock::Packet>
>);
static_assert(std::is_same_v<
    decltype(std::declval<const bedrock::BedrockNetworkClient&>().startGameData()),
    std::optional<bedrock::VersionedGamePacket>
>);
static_assert(std::is_same_v<
    decltype(std::declval<bedrock::Client&>().setStatus(
        std::declval<bedrock::ClientStatus>()
    )),
    void
>);
static_assert(std::is_same_v<
    decltype(std::declval<const bedrock::Player&>().setStatus(
        std::declval<bedrock::BedrockServerClientStatus>()
    )),
    void
>);
static_assert(std::is_same_v<
    decltype(std::declval<bedrock::RelayPlayer&>().updateItemPalette(
        std::declval<const bedrock::ProtoDefValue&>()
    )),
    void
>);

static_assert(std::is_same_v<
    decltype(bedrock::createBedrockClient(
        std::declval<bedrock::BedrockClientOptions>()
    )),
    bedrock::BedrockClient
>);

int main() {
    bedrock::ClientOptions compact {
        .host = "example.invalid",
        .port = 19133,
        .username = "FacadeBot",
        .version = "1.20.40",
        .offline = true,
        .autoInitPlayer = false,
        .viewDistance = 12,
        .authTitle = "00000000-0000-0000-0000-000000000000",
        .connectTimeout = 3210,
        .skipPing = true,
        .followPort = false,
        .batchingInterval = 17,
        .diagnostics = {
            .debug = bedrock::DebugMode::Events
        }
    };
    const auto expanded = bedrock::detail::expandClientOptions(
        std::move(compact)
    );
    if (expanded.host != "example.invalid" ||
        static_cast<uint16_t>(expanded.port) != 19133 ||
        expanded.username != "FacadeBot" || expanded.version != "1.20.40" ||
        !expanded.offline || expanded.autoInitPlayer ||
        expanded.viewDistance.optionalValue() != 12 ||
        expanded.connectTimeout.optionalValue() != 3210 ||
        !expanded.skipPing || expanded.followPort.optionalValue() != false ||
        expanded.batchingInterval.optionalValue() != 17 ||
        expanded.debug != bedrock::DebugMode::Events) {
        return 2;
    }

    bedrock::ServerOptions serverOptions;
    serverOptions.motd = "Facade server";
    if (serverOptions.motd.at("motd") != "Facade server") return 3;

    serverOptions.advertisementFn = [] {
        return bedrock::ServerAdvertisement(
            {{"motd", "value callback"}},
            19132,
            "1.20.40"
        );
    };
    if (serverOptions.advertisementFn().motd != "value callback") return 8;

    bedrock::ServerAdvertisement legacyAdvertisement(
        {{"motd", "legacy reference callback"}},
        19132,
        "1.20.40"
    );
    serverOptions.advertisementFn = [&]() -> bedrock::ServerAdvertisement& {
        return legacyAdvertisement;
    };
    if (&serverOptions.advertisementFn() != &legacyAdvertisement) return 9;
    serverOptions.advertisementFn = nullptr;
    if (serverOptions.advertisementFn) return 10;
    serverOptions.host = "127.0.0.1";
    serverOptions.port = 19133;
    serverOptions.version = "1.20.40";
    serverOptions.offline = true;
    serverOptions.compressionAlgorithm = "snappy";
    serverOptions.compressionLevel = 5;
    serverOptions.compressionThreshold = 128;
    serverOptions.batchingInterval = 9;
    serverOptions.advanced.autoLogin = false;
    serverOptions.advanced.autoResourcePacks = true;
    const auto expandedServer = bedrock::detail::expandServerOptions(
        std::move(serverOptions)
    );
    if (expandedServer.host != "127.0.0.1" ||
        expandedServer.port != 19133 ||
        expandedServer.version != "1.20.40" ||
        !expandedServer.offline || expandedServer.autoLogin ||
        !expandedServer.autoResourcePacks ||
        expandedServer.compressionAlgorithm != "snappy" ||
        expandedServer.compressionLevel != 5 ||
        expandedServer.compressionThreshold != 128 ||
        expandedServer.batchingInterval != 9) {
        return 13;
    }

    bedrock::Player playerView;
    playerView.playerEvents =
        std::make_shared<bedrock::BedrockServerPlayerEventState>();
    playerView.on("login", bedrock::Player::VoidHandler([] {}));
    playerView.on("packet", bedrock::Player::PacketHandler(
        [](const bedrock::BedrockServerPacketEvent&) {}
    ));
    playerView.on("status", bedrock::Player::StatusHandler(
        [](bedrock::BedrockServerClientStatus) {}
    ));

    bedrock::RelayOptions relayOptions;
    relayOptions.offline = true;
    if (!relayOptions.listenerOffline() ||
        !relayOptions.destinationOffline()) return 4;
    relayOptions.destination.offline = false;
    if (relayOptions.destinationOffline()) return 5;

    bedrock::RelayPlayer callbackPlayer;
    const bedrock::XboxDeviceCodeInfo callbackCode {
        .message = "device code"
    };
    int legacyMsaCallbacks = 0;
    int contextualMsaCallbacks = 0;
    relayOptions.onMsaCode = [&](const bedrock::XboxDeviceCodeInfo& code) {
        if (code.message == "device code") ++legacyMsaCallbacks;
    };
    relayOptions.onMsaCode(callbackCode, callbackPlayer);
    relayOptions.onMsaCode = [&](
        const bedrock::XboxDeviceCodeInfo& code,
        bedrock::RelayPlayer& player
    ) {
        if (code.message == "device code" && &player == &callbackPlayer) {
            ++contextualMsaCallbacks;
        }
    };
    relayOptions.onMsaCode(callbackCode, callbackPlayer);
    if (legacyMsaCallbacks != 1 || contextualMsaCallbacks != 1) return 6;
    relayOptions.onMsaCode = nullptr;
    if (relayOptions.onMsaCode) return 7;

    bedrock::BedrockNetworkClient paletteClient;
    paletteClient.updateItemPalette(bedrock::array({
        bedrock::object({
            {"name", bedrock::str("minecraft:shield")},
            {"runtime_id", bedrock::u64(0)}
        }),
        bedrock::object({
            {"name", bedrock::str("minecraft:shield")},
            {"runtime_id", bedrock::u64(999)}
        })
    }));
    if (paletteClient.packetVariableStore()->variable("ShieldItemID")) {
        return 11;
    }
    paletteClient.updateItemPalette(bedrock::array({
        bedrock::object({
            {"name", bedrock::str("minecraft:apple")},
            {"runtime_id", bedrock::u64(260)}
        }),
        bedrock::object({
            {"name", bedrock::str("minecraft:shield")},
            {"runtime_id", bedrock::u64(355)}
        }),
        bedrock::object({
            {"name", bedrock::str("minecraft:shield")},
            {"runtime_id", bedrock::u64(999)}
        })
    }));
    if (paletteClient.packetVariableStore()->variable("ShieldItemID") !=
        std::optional<std::string>("355")) {
        return 12;
    }

    bedrock::Packet packet;
    bedrock::legacy::Packet legacyPacket;
    bedrock::legacy::Client legacyClient;
    legacyClient.on("packet", [](bedrock::legacy::Packet&) {});
    legacyClient.emit(std::move(legacyPacket));
    return packet.fields.empty() ? 0 : 1;
}
