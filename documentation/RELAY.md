# Relay API

The C++ relay API follows the packet event model from:

- `serverbound`: downstream client to upstream server.
- `clientbound`: upstream server to downstream client.
- `event.cancel()`: drop this packet.
- `event.replace(packet)`: forward a changed packet instead.
- `event.replace({packet1, packet2})`: forward several packets.

This includes the packet-level relay core plus `createRelayServer`, a live relay
runtime built from `createServer` and upstream `BedrockNetworkClient`. The
runtime accepts multiple RakNet clients and owns an isolated Player, upstream
client, queues, packet variables, and lifecycle for each downstream session.
It answers the Bedrock handshake, completes login encryption, and emits packet
and join events. The `relay-test-server` example exposes this runtime for live
testing.

## Basic Example

```cpp
#include <bedrock/bedrock.hpp>

#include <iostream>

int main() {
    bedrock::BedrockRelayOptions options;
    options.clientOptions.minecraftVersion = "1.20.40";
    options.clientOptions.outgoingCompression = bedrock::VersionedMcpeCompression::Uncompressed;
    options.enableChunkCaching = false;

    auto relay = bedrock::createRelay(options);
    relay.markDownstreamJoined();
    relay.markUpstreamJoined();

    relay.on("serverbound", [](bedrock::BedrockRelayPacketEvent& event) {
        std::cout << "serverbound " << event.packet.name << "\n";
    });

    relay.on("clientbound", [](bedrock::BedrockRelayPacketEvent& event) {
        std::cout << "clientbound " << event.packet.name << "\n";

        if (event.packet.name == "play_status") {
            event.cancel();
        }
    });
}
```

## Live Relay Server

Use `createRelayServer` when you want a Bedrock client to join the C++ listener and have the C++ library connect to an upstream server.

```cpp
bedrock::BedrockLiveRelayOptions options;
options.server.host = "0.0.0.0";
options.server.port = 19132;
options.server.version = "1.20.40";
options.server.motd = {{"motd", "Bedrock Protocol C++ Relay"}};

options.upstream.host = "localhost";
options.upstream.port = 19132;
options.upstream.username = "Notch";
options.upstream.version = "1.20.40";
options.upstream.offline = false;
options.upstream.interactiveAuth = true;

auto relay = bedrock::createRelayServer(options);

relay.on("serverbound", [](bedrock::BedrockRelayPacketEvent& event) {
    std::cout << event.sessionId << " client -> upstream "
              << event.packet.name << "\n";
});

relay.on("clientbound", [](bedrock::BedrockRelayPacketEvent& event) {
    std::cout << "upstream -> client " << event.packet.name << "\n";
});

relay.listen();
```

Each accepted downstream opens its own upstream connection. Use
`event.sessionId`, `relay.sessionCount()`, and `relay.upstreamCount()` for
aggregate diagnostics, or `relay.upstreamShared(connection)` for an owning
snapshot of one exact upstream client. Set `options.forceSingle = true` when a
second transport must be rejected before it creates a Player or upstream.

`BedrockLiveRelayStatus` retains the single-session boolean fields and also
reports `downstreamConnections`, `downstreamJoinedCount`,
`upstreamStartedCount`, and `upstreamReadyCount`. A boolean is true when at
least one active session is in that state.

For a Bedrock Realm upstream, set `options.realms.realmId`,
`options.realms.realmInvite`, or a picker instead of relying on the configured
upstream host/port. Realm authentication runs after the downstream identity is
captured, resolves the join address, and stores the same Authflow in
`options.upstream.authflow` for the game login. No Java Realms path is used.

`createRelayServer` currently requires the downstream listener version and upstream client version to match. This keeps packet ids, compression shape, encryption, and schema encoding consistent while the full JS relay runtime is being ported.

## Packet Directions

Use the same direction names as JavaScript relay:

```cpp
relay.on("serverbound", [](bedrock::BedrockRelayPacketEvent& event) {
    // client -> proxy -> server
});

relay.on("clientbound", [](bedrock::BedrockRelayPacketEvent& event) {
    // server -> proxy -> client
});
```

For old local code, these aliases are still available:

```cpp
relay.onClientPacket([](const bedrock::VersionedGamePacket& packet) {});
relay.onServerPacket([](const bedrock::VersionedGamePacket& packet) {});
```

## Relay Lifecycle

JavaScript relay does not start forwarding backend packets until the downstream client has joined the proxy. The C++ packet core exposes that state explicitly:

```cpp
auto frame = relay.handleClientboundMcpe(mcpeFromBackend);
// frame.queued == true; packet is in downQ

auto flushedToClient = relay.markDownstreamJoined();
```

After downstream join, packets from the downstream client are queued until the upstream client has joined the destination server:

```cpp
relay.markDownstreamJoined();

auto frame = relay.handleServerboundMcpe(mcpeFromClient);
// frame.queued == true; packet is in upQ

auto flushedToServer = relay.markUpstreamJoined();
```

Queue sizes are available for diagnostics:

```cpp
std::cout << relay.downQueueSize() << "\n";
std::cout << relay.upQueueSize() << "\n";
```

## Cancel A Packet

```cpp
relay.on("clientbound", [](bedrock::BedrockRelayPacketEvent& event) {
    if (event.packet.name == "play_status") {
        event.cancel();
    }
});
```

## Replace A Packet

Use the versioned packet codec to build the replacement packet. This keeps packet id and version shape correct.

```cpp
auto codec = bedrock::VersionedMcpeCodec::forVersion("1.20.40");

relay.on("serverbound", [&](bedrock::BedrockRelayPacketEvent& event) {
    if (event.packet.name == "client_cache_status") {
        event.replace(codec.packetCodec().makePacketByName(
            "client_cache_status",
            {0x00}
        ));
    }
});
```

## Handle MCPE Payloads

The relay accepts already-framed MCPE payloads. If packets are unchanged, it can forward the original payload. If a handler cancels or replaces a packet, the relay repacks a new MCPE payload.

```cpp
auto frame = relay.handleServerboundMcpe(mcpeFromClient);

for (const auto& packet : frame.forwardedPackets) {
    std::cout << packet.name << "\n";
}

auto bytesToServer = frame.forwardedMcpe;
```

Clientbound is the reverse direction:

```cpp
auto frame = relay.handleClientboundMcpe(mcpeFromServer);
auto bytesToClient = frame.forwardedMcpe;
```

## Built-In JS Relay Behaviors

The C++ relay core includes these behaviors from the JavaScript relay:

| Behavior | C++ option |
|---|---|
| Force `client_cache_status` | `forceClientCacheStatus = true` |
| Choose chunk cache value | `enableChunkCaching = false` / `true` |
| Queue `level_chunk` before `start_game` | `queueClientboundLevelChunksUntilStartGame = true` |
| Skip duplicate `play_status login_success` | `skipClientboundLoginSuccess = true` |
| Skip upstream resource-pack handshake packets in live relay | `skipClientboundResourcePacks = true` |
| Forward per-session skin and arbitrary login `clientData` | `forwardDownstreamClientData = true` |
| Queue backend packets before downstream join | `markDownstreamJoined()` / `flushDownQueue()` |
| Queue downstream packets before upstream join | `markUpstreamJoined()` / `flushUpQueue()` |
| Disable serverbound forwarding | `forwardServerbound = false` |
| Disable clientbound forwarding | `forwardClientbound = false` |
| Reject a second live downstream | `forceSingle = true` |
| Trace session and packet routing | high-level `RelayOptions::logging = true` |
| Drop malformed backend packets without closing their session | high-level `RelayOptions::omitParseErrors = true` |

High-level Relay always strictly parses clientbound packets before handlers.
Malformed backend packets are canceled in both modes; the default
`omitParseErrors = false` additionally disconnects only the matching downstream
session. Forwarded login JSON retains custom skin, animation, device/platform,
and unknown fields while normalizing destination- and version-specific values.

## Run The Relay Example

Build:

```bash
./scripts/build.sh --no-install
```

Windows PowerShell:

```powershell
.\scripts\build.ps1 -NoInstall
```

Run the packet-core example:

```bash
./build/relay-packet-bot
```

Windows:

```powershell
.\build\relay-packet-bot.exe
```

Expected output includes:

```text
serverbound client_cache_status
forwarded serverbound packets=1 client_cache_status=0
clientbound play_status
forwarded clientbound packets=0
```

Run the live relay listener:

```bash
./build/relay-test-server
```

Windows:

```powershell
.\build\relay-test-server.exe
```

Edit `examples/relay_test_server.cpp` before building to change:

| Setting | Meaning |
|---|---|
| `version` | Bedrock version used by the downstream listener and upstream client. |
| `listenHost` / `listenPort` | Address Minecraft connects to. |
| `upstreamHost` / `upstreamPort` | Real server the C++ upstream client joins. |
| `upstreamUsername` / `upstreamProfile` | Bot display/profile name. |
| `upstreamOffline` | Use offline auth for local offline upstream servers. |
| `interactiveAuth` | Show device-code login when the Xbox cache is missing. |

Start `relay-test-server`, add the listener address in Minecraft, and join it. The terminal prints `[client -> upstream]` and `[upstream -> client]` packet names while forwarding supported traffic through `createRelayServer`.

## Version Safety

Relay tests run through every bundled protocol version in `protocol-roundtrip`. The tests skip packets that do not exist in a version and verify the behavior on versions where those packets are present.

```bash
./build/protocol-roundtrip
```

Expected summary:

```text
[ROUNDTRIP] checkedVersions=48 failures=0
```

## What Is Not Done Yet

Remaining JavaScript-style relay work includes:

- More `createRelayServer` edge-case parity around unusual login,
  resource-pack, respawn, and reconnect sequences.

The native server Player now includes immediate writes, raw packet admission,
20 ms queue batching, compression/encryption-aware batch flushes, and close
lifecycle handling. `live-relay-smoke` exercises version and Realm paths,
`live-relay-multi-client-smoke` verifies two isolated encrypted routes,
single-session teardown, and `forceSingle`; `live-relay-options-smoke` verifies
cache negotiation, custom login metadata, and both parse-error policies; and
`server-outbound-queue-smoke` verifies the exact encrypted batch boundaries.
