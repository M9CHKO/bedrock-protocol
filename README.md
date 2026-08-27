# bedrock-protocol

Minecraft Bedrock Protocol Client Library for C++20

## Supported Versions

The bundled protocol data currently includes these Bedrock versions:

`1.16.201 (422)` `1.16.210 (428)` `1.16.220 (431)` `1.17.0 (440)` `1.17.10 (448)` `1.17.30 (465)` `1.17.40 (471)` `1.18.0 (475)` `1.18.11 (486)` `1.18.30 (503)` `1.19.1 (527)` `1.19.10 (534)` `1.19.20 (544)` `1.19.21 (545)` `1.19.30 (554)` `1.19.40 (557)` `1.19.50 (560)` `1.19.60 (567)` `1.19.62 (567)` `1.19.63 (568)` `1.19.70 (575)` `1.19.80 (582)` `1.20.0 (589)` `1.20.10 (594)` `1.20.15 (594)` `1.20.30 (618)` `1.20.40 (622)` `1.20.50 (630)` `1.20.61 (649)` `1.20.71 (662)` `1.20.80 (671)` `1.21.0 (685)` `1.21.2 (686)` `1.21.20 (712)` `1.21.30 (729)` `1.21.42 (748)` `1.21.50 (766)` `1.21.60 (776)` `1.21.70 (786)` `1.21.80 (800)` `1.21.90 (818)` `1.21.93 (819)` `1.21.100 (827)` `1.21.111 (844)` `1.21.120 (859)` `1.21.124 (860)` `1.21.130 (898)` `1.26.0 (924)`

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
- ProtoDef `setVariable` switch branches and persistent item-palette state,
  including automatic `ShieldItemID` updates from `start_game` or
  `item_registry` and full decoding of encapsulated `Item.extra` payloads.
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
- Native `createServer` runtime: RakNet listener, authenticated/offline Player lifecycle, compression and encryption transitions, packet/status/login/join/spawn/close events, immediate `write`, timed `queue` batching, raw `sendBuffer`, and explicit `sendQueued` flushes. All of these paths are Bedrock-only.
- Live relay runtime (`Relay` / `createRelayServer`) with one isolated upstream
  C++ client, queues, packet-variable store, and lifecycle per accepted
  downstream Bedrock player. Multiple clients are supported concurrently;
  `forceSingle` restores the single-client rejection mode, and each destination
  can be a real Bedrock server or Bedrock Realm. Relay login preserves custom
  skin/client metadata per session, and high-level `logging`,
  `enableChunkCaching`, and `omitParseErrors` match the Bedrock JavaScript path.
- Bedrock chunk/world foundation ported from `prismarine-chunk`, including runtime and local/network-persistent paletted subchunks, full `little`/`littleVarint` NBT block-state palettes and block entities, the legacy and batched 1.18+ `subchunk` packet shapes, 2D/3D biome sections, no-cache and cached chunk-section decoding, cache blob status/miss handling, and a tracked `client.world()` that consumes standalone subchunk responses.
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
    .offline = false,
    .interactiveAuth = true,
    .clientCacheEnabled = false,
    .chunkRadius = 10,
});
```

| Option | Default | Meaning |
|---|---:|---|
| `host` | `localhost` | Bedrock server address. |
| `port` | `19132` | Bedrock server port. |
| `username` | `Bot` | Bot name. In online mode this is also the default auth cache profile. |
| `profile` | empty | Xbox auth cache profile. Empty means `username`. |
| `version` | `1.26.0` | Exact Bedrock release from the JavaScript `Versions` table. Unknown values, including `latest` and `auto`, are rejected. |
| `offline` | `false` | Use self-signed auth instead of Xbox Live. |
| `interactiveAuth` | `true` | If the Xbox cache is missing, show a device-code login prompt and save the profile cache. |
| `authTitle` | unset | OAuth title id. When unset, matches JavaScript by using `title.MinecraftNintendoSwitch`. |
| `deviceType` | conditional | Becomes `Nintendo` only while applying the unset-`authTitle` default; an explicit title leaves it unchanged. |
| `flow` | conditional | Becomes `live` only while applying the unset-`authTitle` default; an explicit title leaves it unchanged. |
| `xboxClientId` | empty | Deprecated C++ compatibility alias for `authTitle`; explicit `authTitle` takes precedence. |
| `authCacheRoot` | auto | Optional Xbox auth/key cache root. Empty uses the hidden default cache folder. |
| `realms` | disabled | Bedrock Realm selector. Set `realms.realmId`, `realms.realmInvite`, or `realms.pickRealm`; Realm auth resolves `host`/`port` before ping and reuses the same Authflow for game login. |
| `clientCacheEnabled` | `false` | Sends the client cache preference used by chunk cache flow. 
| `chunkRadius` | `10` | Requested chunk radius during automatic start-game initialization. |
| `debug` | `Off` | `Off`, `Events`, `Packets`, `Json`, or `Trace`. |
| `decodePackets` | `true` | Decode packet fields into JSON-style event fields. |
| `packetDump` | `false` | Print extra packet dump output. |

`bedrock::createClient()` is the normal in-process API. The old helper-process wrapper is still available as `bedrock::createExternalClient(...)` for compatibility with older local tests.

Connect to a Bedrock Realm by id:

```cpp
bedrock::Options options;
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
client.onSpawn([] {});
client.onHeartbeat([](int64_t responseTime) {});
```

The root `bedrock::title` object exposes the same seven title ids as
`prismarine-auth`'s `Titles`. Use `Options::onMsaCode` for the device-code
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
| `relay-test-server` | Runnable `createRelayServer` listener for joining from Minecraft and forwarding to an upstream server. |
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
