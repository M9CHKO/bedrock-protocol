# bedrock-protocol

Minecraft Bedrock Protocol Client Library for C++20

Standalone Android 8+ relay app (no Termux), including a directly
installable arm64 APK: [Android relay app](android/relay-app/README.md).

Current Android build:
[CPE Relay 1.2.19](android/relay-app/apk/CPE-Relay-v1.2.19-arm64-v8a-release-debug-signed.apk)
([SHA-256](android/relay-app/apk/CPE-Relay-v1.2.19-arm64-v8a-release-debug-signed.apk.sha256)).
The Android frontend transparently carries server-required resource packs and
adds an optional packet-derived HUD: entity projection, retained-chunk status,
equipment and durability, mini-map, threat analysis, auto-equipment, polygonal
area auto-fill, and a client-only phantom schematic guide for `.mcstructure`, vanilla structure `.nbt`, `.litematic`,
`.schem`, and `.schematic` files. The HUD does not capture the screen, inject
code into Minecraft, or read Minecraft process memory. See the
[Android architecture map](android/relay-app/ARCHITECTURE.md) for module and
data-flow details.

## Supported Versions

The bundled protocol data currently includes these Bedrock versions:

`1.16.201 (422)` `1.16.210 (428)` `1.16.220 (431)` `1.17.0 (440)` `1.17.10 (448)` `1.17.30 (465)` `1.17.40 (471)` `1.18.0 (475)` `1.18.11 (486)` `1.18.30 (503)` `1.19.1 (527)` `1.19.10 (534)` `1.19.20 (544)` `1.19.21 (545)` `1.19.30 (554)` `1.19.40 (557)` `1.19.50 (560)` `1.19.60 (567)` `1.19.62 (567)` `1.19.63 (568)` `1.19.70 (575)` `1.19.80 (582)` `1.20.0 (589)` `1.20.10 (594)` `1.20.15 (594)` `1.20.30 (618)` `1.20.40 (622)` `1.20.50 (630)` `1.20.61 (649)` `1.20.71 (662)` `1.20.80 (671)` `1.21.0 (685)` `1.21.2 (686)` `1.21.20 (712)` `1.21.30 (729)` `1.21.42 (748)` `1.21.50 (766)` `1.21.60 (776)` `1.21.70 (786)` `1.21.80 (800)` `1.21.90 (818)` `1.21.93 (819)` `1.21.100 (827)` `1.21.111 (844)` `1.21.120 (859)` `1.21.124 (860)` `1.21.130 (898)` `1.26.0 (924)` `1.26.10 (944)` `1.26.20 (975)`

More details and version-specific notes are in [documentation/VERSIONS.md](documentation/VERSIONS.md).

It is built for bot projects: you write normal C++ code, put connection settings in `bedrock::createClient({...})`, build your bot, and run your bot executable without passing host/version/user arguments in the terminal.

```cpp
#include <bedrock/bedrock.hpp>

#include <iostream>

int main() {
    auto client = bedrock::createClient({
        .host = "localhost",
        .port = 19132,
        .username = "Notch",
        .version = "1.20.40",
        .offline = true,
    });

    client.on("start_game", [](const bedrock::Packet&) {
        std::cout << "Joined world\n";
    });

    client.on("packet", [](const bedrock::Packet& packet) {
        std::cout << packet.name << " id=" << packet.id << "\n";
    });

    return client.run();
}
```

## What Works

- RakNet ping and connect.
- Minecraft Bedrock network handshake.
- `NetworkSettingsRequest` / `NetworkSettings`.
- Login packet generation with versioned `clientData`.
- Xbox Live auth for online servers through profile cache and interactive device-code login.
- Native Bedrock auth primitives include `LiveTokenManager`,
  `MsaTokenManager`, `XboxTokenManager`, `MinecraftBedrockTokenManager`,
  `MinecraftBedrockServicesTokenManager`, and `PlayfabTokenManager` from
  `prismarine-auth`, with their Bedrock-only `MicrosoftAuthFlow`
  orchestration used by online client login. The Minecraft Java manager and
  its separate cache are intentionally outside this library.
- Minecraft Bedrock Realms discovery and connection by numeric Realm id,
  `realms.gg` invite, or picker callback. The native `BedrockRealmApi` also
  ports the Bedrock worlds, invites, backups/downloads, subscriptions, slots,
  player permissions, blocklist, texture-pack policy, and exponential 5xx
  retry paths from `prismarine-realms`; its Java Realms API is intentionally
  not included.
- Offline/self-signed auth for local offline servers.
- Resource pack response flow.
- Deflate-raw and native raw Snappy block compression for both legacy
  session-wide batches and modern per-packet compressor headers, with no Node
  runtime dependency.
- Packet id/name decoding across bundled protocol versions.
- Schema-based packet encoding and `client.write(packetName, bedrock::object({...}))` for packets in the bundled version registry.
- Packet-level `nbt`, `lnbt`, and `nbtLoop` encoding/decoding through the
  native Bedrock NBT codec. Values use prismarine-nbt's
  `{ "type", "name", "value" }` shape, cover tags 0-12, and are retained as
  structured `ProtoDefValue` data by the packet/relay decoder.
- Bedrock file-level `prismarine-nbt` equivalents: `writeUncompressed`,
  `parseUncompressed`, `parseAs`, `hasBedrockLevelHeader`, automatic
  Little/LittleVarInt detection, `simplify`, semantic `equal`, and strict
  eight-byte `level.dat` header reading/writing. Java Edition's
  big-endian/GZIP branch is intentionally excluded.
- ProtoDef `setVariable` switch branches and persistent item-palette state,
  including automatic `ShieldItemID` updates from `start_game` or
  `item_registry`, public `Connection::updateItemPalette` equivalents, and full
  decoding of encapsulated `Item.extra` payloads.
- Lossless structured packet decoding for `buffer`/`ByteArray`/`restBuffer`,
  canonical UUIDs, signed and zigzag array counts, and relay-safe restoration
  of empty arrays, absent options, metadata loops, and `void` switch branches.
- ProtoDef-compatible 8/16/32/64/128-bit Bedrock flag sets, including
  `varint64` input flags, signed `zigzag64` entity metadata, composite flag
  masks, and lossless preservation of unknown raw bits during relay rewrites.
- Scoped ProtoDef constructor parsing for nested containers/switches, omission
  of inactive switch placeholders in relay values, and persistent packet
  variables in event dispatchers and packet inspectors.
- Optional deep packet JSON decoding for debugging.
- `bedrock-protocol`-style in-process client creation and event handlers.
- Packet-level relay core with `clientbound` / `serverbound` events, `cancel()`, `replace()`, MCPE repacking, forced `client_cache_status`, and level chunk queueing before `start_game`.
- Native `createServer` runtime: RakNet listener, authenticated/offline Player lifecycle, Node-compatible client protocol gating at `request_network_settings`/`login`, compression and encryption transitions, packet/status/`loggingIn`/login/join/spawn/close events, immediate `write`, timed `queue` batching, raw `sendBuffer`, and explicit `sendQueued` flushes. All of these paths are Bedrock-only.
- Live relay runtime (`Relay`, with `createRelay` retained as a convenience)
  with one isolated upstream
  C++ client, queues, packet-variable store, and lifecycle per accepted
  downstream Bedrock player. Multiple clients are supported concurrently;
  `forceSingle` restores the single-client rejection mode, optional
  `replaceExisting` gives mobile frontends latest-connection-wins teardown,
  and each destination
  can be a real Bedrock server or Bedrock Realm. Relay login preserves custom
  skin/client metadata per session, phase-isolated downstream/upstream
  negotiation, and high-level `logging`, `enableChunkCaching`, explicit
  `Disconnect`/`Drop`/`ForwardRaw` parse-error policies, and session-aware
  `onMsaCode(code, player)` match the Bedrock JavaScript path. Relay
  `onJoin(player, upstream)` exposes the exact destination-ready pair.
- Bedrock chunk/world foundation ported from `prismarine-chunk` and
  `prismarine-world`, including runtime and persistent paletted subchunks,
  entities and block entities, 2D/3D biomes, cached/no-cache chunk decoding,
  traversal/raycast helpers, block-update events, and generator/storage save
  queues. `BedrockBlockRegistry` adds the Bedrock `prismarine-block` surface:
  typed states/properties, Node-compatible hashes, harvest/dig helpers, and
  state-specific collision shapes loaded directly from minecraft-data. Typed
  column/world block access carries light, biome, position, block-entity NBT,
  and Bedrock's superimposed layer; sign blocks expose native `Text` NBT
  helpers compatible with `prismarine-block`. `BedrockChunk014` and
  `BedrockChunk10` preserve the original fixed numeric-id chunk layouts,
  nibble arrays, biome/color data, and wire dumps for early Bedrock releases.
  `BedrockItemRegistry` ports the Bedrock `prismarine-item` object, including
  stack IDs, modern and auxiliary-value packet conversion, exact Item NBT,
  durability, names/lore, enchants, and adventure block lists.
  `BedrockRegistry` adds version-aware Bedrock biome/entity metadata plus
  recipes, inventory windows, note-block instruments, and attributes behind
  one facade. `BedrockFeatureRegistry` adds Node-compatible Bedrock feature
  values and version comparisons, replacing hard-coded block/item format
  boundaries. Recipe NBT, sparse IDs, duplicate names, optional tables, and
  old/current schemas are preserved through Bedrock-only minecraft-data remaps.
  The dynamic `prismarine-registry` surface handles `start_game`/
  `item_registry` item palettes and sequential or typed-state-hashed block
  runtime IDs for custom servers.
  Bedrock block/entity loot keeps both Node's last-write-wins indexes and every
  state variant, while the versioned default skin exposes raw login fields plus
  decoded RGBA, geometry, resource-patch, animation, and persona data.
  `BedrockChat` ports the Bedrock `prismarine-chat` path with its native
  language catalog, translation formatting, section colors, plain/MOTD/ANSI/
  HTML output, message builder, and complete Bedrock text-packet parameters.
- CMake package install for separate bot projects.
- Windows through MSYS2/MinGW, Linux, and Termux builds.

For outgoing packets, pass fields in the same shape as the packet schema for the selected version. This mirrors the  `bedrock-protocol-cpp` / `protodef` model: enums use their string names, arrays use arrays, optional values use `null`, buffers use bytes, and nested containers use nested objects.

## Quick Start

Build and install the library once:

```bash
cd bedrock-protocol-cpp
./scripts/build.sh
```

Windows PowerShell:

```powershell
cd C:\path\to\bedrock-protocol-cpp
.\scripts\build.ps1
```

Then create your bot project and link to:

```cmake
BedrockProtocol::bedrock_protocol
```

Detailed beginner instructions are here:

- [Getting Started](documentation/GETTING_STARTED.md)
- [Client, Server, And Relay Creation](documentation/API.md)
- [Bot Packet Examples](documentation/BOT_PACKETS.md)
- [Relay API](documentation/RELAY.md)
- [Supported Versions](documentation/VERSIONS.md)
- [Packet Documentation](documentation/PACKETS.md)
- [Updating The Library](documentation/MAINTENANCE.md)

## Install Layout

The build scripts install into `install/` by default:

```text
bedrock-protocol-cpp/
  install/
    bin/                       runtime helpers
    include/                   public C++ headers
    lib/                       static library and CMake package files
    share/bedrock-protocol-cpp/  protocol data and clientData template
```

You do not rebuild the library every time you edit your bot. Rebuild the library only when files inside this library change. For normal bot development, rebuild only your bot project.

## Client API

```cpp
auto client = bedrock::createClient({
    .host = "localhost",
    .port = 19132,
    .username = "Notch",
    .version = "1.26.0",
    .offline = false
});
```

Direct brace calls use the compact `bedrock::ClientOptions` facade. It contains
the normal JavaScript `ClientOptions` fields; C++ logging/decoder switches are
grouped under `.diagnostics`, without exposing login-packet and transport
internals. `bedrock::BotOptions` and `bedrock::createBot()` are readable aliases
for bot projects. `ServerOptions` follows the same facade rule, while Relay is
constructed as `bedrock::Relay(options)` and started with `listen()`, matching
JavaScript.

Bots can opt out of automatic spawn initialization with
`.autoInitPlayer = false`, inspect `entityId()` and the retained
`startGameData()`, send `set_local_player_as_initialized`, then call
`setStatus(ClientStatus::Initialized)`. Mapper fields in public packets use
JavaScript enum names such as `player_spawn`. See the manual lifecycle example
in [documentation/API.md](documentation/API.md).

The complete client/server/Relay option tables and examples are in
[documentation/API.md](documentation/API.md). Existing typed
`bedrock::Options` calls remain supported under the clearer alias
`bedrock::LegacyClientOptions`.

`bedrock::createClient()` is the normal in-process API. The old helper-process wrapper is still available as `bedrock::createExternalClient(...)` for compatibility with older local tests.

Connect to a Bedrock Realm by id:

```cpp
bedrock::ClientOptions options;
options.username = "Xbox account email";
options.realms.realmId = 1112223;

auto client = bedrock::createClient(std::move(options));
return client.run();
```

For an invite use `options.realms.realmInvite =
"https://realms.gg/AB1CD2EFA3B"`. A picker can return one item from the
`std::vector<bedrock::BedrockRealm>` passed to `options.realms.pickRealm`.
An asynchronous native picker can return `std::future<BedrockRealm>` through
`options.realms.pickRealmAsync`. Realm ids may be assigned as integers or
decimal strings.
When Realms are enabled, `followPort` defaults to false, matching
`bedrock-protocol`; an explicitly supplied `followPort` value is retained.

Events:

```cpp
client.on("packet", [](const bedrock::Packet& packet) {});
client.on("start_game", [](const bedrock::Packet& packet) {});
client.on("disconnect", [](const bedrock::Packet& packet) {});
client.onText([](const bedrock::TextPacket& text) {});
client.onSession([](const bedrock::BedrockClientProfile& profile) {});
client.onLoggingIn([] {});
client.onJoin([] {});
client.onSpawn([] {});
client.onHeartbeat([](int64_t responseTime) {});
```

After `session`, `client.profile()`, `client.username()`, and
`client.accessToken()` expose the same authenticated profile and original
Mojang/Xbox chains that JavaScript stores on the client. The internal
`client.server_handshake` alias is also available through `client.on(...)`;
servers can observe the matching pre-login boundary with
`server.onServerClientHandshake(...)`.

Server-side players use the same event shape as `serverPlayer.js`:

```cpp
server.on("connect", [](const bedrock::Player& player) {
    player.onLogin([player](const bedrock::BedrockServerPacketEvent&) {
        if (const auto profile = player.profile()) {
            std::cout << "login " << profile->name << "\n";
        }
    });
    player.on("join", bedrock::Player::VoidHandler([] {}));
    player.on("packet", bedrock::Player::PacketHandler(
        [](const bedrock::BedrockServerPacketEvent&) {}
    ));
});
```

`Player` also owns the per-session `write`, `queue`, `sendBuffer`,
`disconnect`, status/version, profile, and close APIs. `RelayPlayer` delegates
the same downstream lifecycle surface while retaining its Relay packet hooks.
`server.clients()`/`server.client(key)` and the matching Relay methods expose
thread-safe active-player snapshots. Direct `Server::listen()` and
`Relay::listen()` return the configured host/port, and `advertisementFn` now
accepts the normal JavaScript value-returning callback while preserving the
older C++ reference-returning form.

The root `bedrock::title` object exposes the same seven title ids as
`prismarine-auth`'s `Titles`. Use `ClientOptions::onMsaCode` for the device-code
callback; the older `onDeviceCode` name remains available on the lower-level
`XboxLiveAuthOptions` API.

Packet examples for bots are in [documentation/BOT_PACKETS.md](documentation/BOT_PACKETS.md).

Relay/proxy API documentation is in [docs/RELAY_API.md](docs/RELAY_API.md).

Sending a schema-shaped packet:

```cpp
client.write("request_chunk_radius", bedrock::object({
    {"chunk_radius", bedrock::i32(20)},
    {"max_radius", bedrock::u32(0)}
}));
```

Examples included in this repository:

| Example | Purpose |
|---|---|
| `simple-create-client-bot` | Minimal connect/event bot. |
| `packet-event-bot` | Packet event logging and one outgoing schema packet. |
| `medium-bot` | Medium bot example with packet handlers, chunk radius request, and movement packet writing. |
| `relay-packet-bot` | Packet-level relay example with serverbound/clientbound hooks. |
| `relay-test-server` | Runnable high-level `Relay` listener for joining from Minecraft and forwarding to an upstream server. |
| `simple-server` | Minimal `createServer` listener with connect, packet, and join events. |


## Build Status Checks

Run the local protocol roundtrip check:

```bash
./build/protocol-roundtrip
```

Expected result:

```text
[ROUNDTRIP] checkedVersions=50 failures=0
```

## VS Code

Open the library folder itself:

```text
C:\path\to\bedrock-protocol-cpp
```

Do not open the parent folder that contains `bedrock-protocol-cpp/`, logs, and zip files. The checked-in `.vscode/settings.json` expects `${workspaceFolder}` to be the library root.

Then run:

```text
CMake: Configure
CMake: Build
```

If IntelliSense still shows a red include for `<bedrock/bedrock.hpp>`, run:

```text
C/C++: Reset IntelliSense Database
```

## References

- Packet tables: [Minecraft Data documentation](https://prismarinejs.github.io/minecraft-data/)
- Mojang protocol notes: [`Mojang/bedrock-protocol-docs`](https://github.com/Mojang/bedrock-protocol-docs)

## License

Add your project license before publishing this repository.
