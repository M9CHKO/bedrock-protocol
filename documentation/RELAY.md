# Relay API

The C++ relay API follows the packet event model from:

- `serverbound`: downstream client to upstream server.
- `clientbound`: upstream server to downstream client.
- `event.cancel()`: drop this packet.
- `event.replace(packet)`: forward a changed packet instead.
- `event.replace({packet1, packet2})`: forward several packets.

The normal live proxy is `RelayOptions` plus the `Relay` constructor, matching
JavaScript's `new Relay(options)`. It accepts multiple
RakNet clients and owns an isolated Player, upstream client, queues, packet
variables, and lifecycle for each downstream session. The packet-only
`BedrockRelayOptions` and direct `BedrockLiveRelayOptions` composition remain
available as advanced layers.

## Live Relay (Recommended)

```cpp
bedrock::RelayOptions options {
    .version = "1.20.40",
    .host = "0.0.0.0",
    .port = 19132,
    .offline = true,
    .destination = {
        .host = "127.0.0.1",
        .port = 19133
    }
};

bedrock::Relay relay(std::move(options));
relay.onJoin([](
    bedrock::RelayPlayer& player,
    bedrock::BedrockNetworkClient& upstream
) {
    std::cout << "destination ready for " << player.sessionId() << "\n";
});
relay.onServerbound([](bedrock::RelayPacketEvent& event) {
    std::cout << "client -> destination " << event.name << "\n";
});
relay.onClientbound([](bedrock::RelayPacketEvent& event) {
    std::cout << "destination -> client " << event.name << "\n";
});
relay.listen();
```

The root `offline` value applies to the listener and destination. Do not set
`destination.offline` in the normal case. It is only an override when the two
sides intentionally use different authentication modes. With `offline =
false`, both sides use online authentication; with `offline = true`, both use
offline login.

For online upstream authentication, `RelayOptions::onMsaCode` accepts
`(const XboxDeviceCodeInfo&, RelayPlayer&)`. The player is the exact downstream
session requesting the code. The older one-argument callback remains valid;
without either callback, Relay disconnects only that session with the standard
sign-in prompt.

High-level `onJoin(player, upstream)` mirrors `Relay.emit('join', ds, client)`.
It runs after the destination login, forced cache-status write, upstream
publication, and pending upstream queue flush. Low-level `onJoin(connection)`
is the earlier downstream boundary; `onUpstreamJoin` exposes the later event.

The `RelayPlayer` itself mirrors the downstream server `Player`. From
`relay.onConnect`, register `onLogin`, `onJoin`, `onSpawn`, `onClose`, named
packet/`packet` listeners, or inspect `profile()`, `getUserData()`, `version()`
and `status()`. `disconnect`, `sendDisconnectStatus`, and `close` act only on
that downstream session. The Relay's internal queue-release join listener is
registered first, matching the constructor order in `relay.js`.

`relay.clients()` and `relay.client(player.connection.key())` mirror the
`Server.clients` surface inherited by JavaScript Relay, using synchronized
owning snapshots in C++. `relay.listen()` returns the configured host/port;
when port `0` requests an ephemeral socket, read the actual port from
`relay.live().boundPort()`.

The full high-level guide is [docs/RELAY_API.md](../docs/RELAY_API.md), and the
shared creation API is [documentation/API.md](API.md).

## Packet-Core Example (Advanced)

```cpp
#include <bedrock/bedrock.hpp>

#include <iostream>

int main() {
    bedrock::BedrockRelayOptions options;
    options.clientOptions.minecraftVersion = "1.20.40";
    options.clientOptions.outgoingCompression = bedrock::VersionedMcpeCompression::Uncompressed;
    options.enableChunkCaching = false;

    auto relay = bedrock::createPacketRelay(options);
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

## Direct Live Runtime (Advanced)

Use `BedrockLiveRelayOptions` and `createLiveRelay` only when you need direct
access to the separate low-level server and upstream client options.

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

auto relay = bedrock::createLiveRelay(options);

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

`createLiveRelay` currently requires the downstream listener version and upstream client version to match. This keeps packet ids, compression shape, encryption, and schema encoding consistent while the full JS relay runtime is being ported. `createRelayServer` remains as a compatibility alias.

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
| Disconnect on a parse error | `parseErrorPolicy = RelayParseErrorPolicy::Disconnect` |
| Drop only a packet that cannot be parsed | `parseErrorPolicy = RelayParseErrorPolicy::Drop` |
| Forward the original packet bytes after a parse error | `parseErrorPolicy = RelayParseErrorPolicy::ForwardRaw` |

High-level Relay strictly parses a packet before exposing structured fields to
handlers. Transport forwarding remains independent: `ForwardRaw` reports the
error through `onParseError`, skips structured handlers, and forwards the
original `VersionedGamePacket` without decoding or re-encoding it. `Drop`
discards only that packet, while `Disconnect` tears down the matching
downstream and upstream session immediately. For source compatibility,
omitting `parseErrorPolicy` maps `omitParseErrors = false` to `Disconnect` and
`omitParseErrors = true` to `Drop`.

Downstream `request_network_settings`, `login`, and
`client_to_server_handshake` packets are always consumed by the downstream
server session. Upstream `network_settings` and
`server_to_client_handshake` packets are likewise consumed by the independently
authenticated upstream client. Neither session's negotiation packets enter the
opposite game stream.

Forwarded login JSON retains custom skin, animation, device/platform, and
unknown fields while normalizing destination- and version-specific values.

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

Edit the `RelayOptions` object in `examples/relay_test_server.cpp` before
building to change:

| Setting | Meaning |
|---|---|
| `version` | Bedrock version used by the downstream listener and upstream client. |
| `host` / `port` | Address Minecraft connects to. |
| `destination.host` / `destination.port` | Real Bedrock server the relay joins. |
| `offline` | Authentication mode inherited by both sides. |
| `destination.offline` | Optional upstream-only override. |

Start `relay-test-server`, add the listener address in Minecraft, and join it.
The terminal prints `[client -> upstream]` and `[upstream -> client]` packet
names while forwarding supported traffic through the high-level `Relay`.

## Version Safety

Relay tests run through every bundled protocol version in `protocol-roundtrip`. The tests skip packets that do not exist in a version and verify the behavior on versions where those packets are present.

```bash
./build/protocol-roundtrip
```

Expected summary:

```text
[ROUNDTRIP] checkedVersions=50 failures=0
```

## What Is Not Done Yet

Remaining JavaScript-style relay work includes:

- More `createLiveRelay` edge-case parity around unusual login,
  resource-pack, respawn, and reconnect sequences.

The native server Player now includes immediate writes, raw packet admission,
20 ms queue batching, compression/encryption-aware batch flushes, and close
lifecycle handling. `live-relay-smoke` exercises version and Realm paths,
`live-relay-multi-client-smoke` verifies two isolated encrypted routes,
single-session teardown, and `forceSingle`; `live-relay-options-smoke` verifies
cache negotiation, custom login metadata, and both parse-error policies; and
`server-outbound-queue-smoke` verifies the exact encrypted batch boundaries.
