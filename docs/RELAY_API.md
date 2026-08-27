# Relay API

`bedrock::Relay` is the C++ proxy API modeled after `bedrock-protocol`'s JavaScript `Relay`.
It creates a local Bedrock server, accepts Minecraft clients, opens one isolated
upstream connection for each accepted client, and lets you inspect, edit,
cancel, or inject packets in both directions.

## Basic Proxy

```cpp
#include <bedrock/bedrock.hpp>
#include <iostream>

int main() {
    bedrock::Relay relay({
        .version = "1.21.2",
        .host = "0.0.0.0",
        .port = 19132,
        .motd = "Bedrock Protocol C++ Relay",
        .username = "RelayBot",
        .offline = false,
        .destination = {
            .host = "cpe.ign.gg",
            .port = 19132
        }
    });

    relay.on("connect", [](bedrock::RelayPlayer& player) {
        std::cout << "client " << player.connection.address
                  << ":" << player.connection.port << "\n";
    });

    relay.listen();
    while (true) {}
}
```

## Bedrock Realm Destination

The relay can resolve a Bedrock Realm before opening its upstream connection.
The Realm Authflow is reused for the upstream game login, matching the
JavaScript relay path:

```cpp
bedrock::RelayOptions options;
options.version = "1.21.2";
options.host = "0.0.0.0";
options.port = 19132;
options.username = "RelayBot";
options.destination.realms.realmInvite =
    "https://realms.gg/AB1CD2EFA3B";

bedrock::Relay relay(std::move(options));
relay.listen();
```

`destination.realms.realmId`, `realmInvite`, `pickRealm`, and
`pickRealmAsync` use the same Bedrock-only selectors as `createClient`.
`RelayOptions::profilesFolder`, `onMsaCode`, `authTitle`, `deviceType`,
`flow`, `forceRefresh`, `msalConfig`, and `password` are forwarded to the
upstream Authflow. Host and port are replaced by the Realm join address.

## Multiple Clients And `forceSingle`

Multiple downstream clients are accepted by default. Every accepted transport
gets its own `RelayPlayer`, upstream `BedrockNetworkClient`, pre-join queues,
chunk-release state, and ProtoDef packet-variable store. Closing one player
closes only its matching upstream connection.

The connection callback is the safest place to retain the exact player:

```cpp
relay.onConnect([](bedrock::RelayPlayer& player) {
    std::cout << "session=" << player.sessionId() << "\n";
});
```

`RelayPacketEvent::sessionId` identifies the same session in global packet
handlers. `relay.playerCount()`, `relay.players()`, and
`relay.player(connection)` expose the active high-level players. The no-argument
`relay.player()` remains as a compatibility view for old single-client code;
multi-client code should use the callback player or an exact connection lookup.

Set `RelayOptions::forceSingle = true` to reject a second transport while one
accepted player is active. A rejected transport does not emit the relay
`connect` callback and never creates an upstream client.

The low-level runtime has matching `sessionCount()`, `upstreamCount()`,
`upstream(connection)`, and owning `upstreamShared(connection)` accessors.

## Relay Options And Login Metadata

The high-level options are forwarded to the live runtime: `logging = true`
enables session and packet tracing, while `enableChunkCaching` selects the
forced `client_cache_status` value sent to each upstream server.

Clientbound packets are strictly decoded before any high-level handler runs.
A malformed backend packet is always canceled and never reaches the downstream
client. With the default `omitParseErrors = false`, only that packet's
downstream session is disconnected; with `omitParseErrors = true`, the packet
is dropped and the session remains usable. Serverbound decode errors propagate
to the caller, matching the JavaScript `RelayPlayer.readPacket` path.

Each downstream login's signed `clientData` JSON is forwarded to its matching
upstream login. Custom skin, animation, device/platform, and unknown extension
fields are retained. The upstream login builder still replaces the destination
`ServerAddress` and applies the normal runtime and protocol-version
normalization.

## Packet Events

Use `player.on("clientbound", ...)` for packets from the upstream server to the Minecraft client.
Use `player.on("serverbound", ...)` for packets from the Minecraft client to the upstream server.

```cpp
relay.on("connect", [](bedrock::RelayPlayer& player) {
    player.on("clientbound", [](bedrock::RelayPacketEvent& packet) {
        if (packet.name == "text") {
            std::cout << "server chat: " << packet.get("message") << "\n";
        }
    });

    player.on("serverbound", [](bedrock::RelayPacketEvent& packet) {
        if (packet.name == "text") {
            std::cout << "client chat: " << packet.get("message") << "\n";
        }
    });
});
```

`packet.params` is decoded before handlers run. Nested fields are stored as nested `PacketValue` objects:

```cpp
player.on("serverbound", [](bedrock::RelayPacketEvent& packet) {
    if (packet.name != "move_player") return;

    auto* pos = packet.value("position");
    if (!pos || pos->kind != bedrock::PacketValue::Kind::Object) return;

    const auto& obj = pos->objectValue;
    std::cout << "x=" << obj.at("x").doubleValue << "\n";
});
```

## Edit Packets

Changing params re-encodes the packet before it is forwarded.

```cpp
player.on("serverbound", [](bedrock::RelayPacketEvent& packet) {
    if (packet.name == "text") {
        packet.set("message", packet.get("message") + " [relay]");
    }
});
```

For nested fields, edit `packet.params` directly:

```cpp
player.on("serverbound", [](bedrock::RelayPacketEvent& packet) {
    if (packet.name == "move_player" && packet.has("position")) {
        packet.params["position"].objectValue["y"] = bedrock::f32(120.0f);
    }
});
```

## Cancel Packets

The destination object matches the JavaScript `des.canceled` pattern.

```cpp
player.on("serverbound", [](bedrock::RelayPacketEvent& packet, bedrock::RelayPacketDestination& des) {
    if (packet.name == "command_request" && packet.get("command") == "/test") {
        des.cancel();
    }
});
```

## Send Packets

Send to the Minecraft client:

```cpp
player.queue("text", {
    {"type", bedrock::str("raw")},
    {"needs_translation", bedrock::boolean(false)},
    {"source_name", bedrock::str("")},
    {"message", bedrock::str("Hello from relay")},
    {"xuid", bedrock::str("")},
    {"platform_chat_id", bedrock::str("")}
});
```

Send to the upstream server:

```cpp
player.upstream.queue("text", {
    {"type", bedrock::str("chat")},
    {"needs_translation", bedrock::boolean(false)},
    {"source_name", bedrock::str("RelayBot")},
    {"message", bedrock::str("Hello upstream")},
    {"xuid", bedrock::str("")},
    {"platform_chat_id", bedrock::str("")}
});
```

`queue` follows the Node Player/Client batching timer and can combine several
packets into one encrypted MCPE batch. `write` and `send` are immediate. The
relay-level `batchingInterval` option controls both the downstream Player queue
and the upstream Client queue; its default is 20 ms.

## Relay-Level Events

```cpp
relay.on("error", [](const std::string& message) {
    std::cerr << message << "\n";
});

relay.on("status", [](const bedrock::BedrockLiveRelayStatus& status) {
    std::cout << "ready=" << status.upstreamReadyCount
              << "/" << status.downstreamConnections << "\n";
});

relay.on("disconnect", [](bedrock::RelayPlayer& player) {
    std::cout << "client disconnected\n";
});
```

The legacy boolean fields (`downstreamJoined`, `upstreamStarted`, and
`upstreamReady`) mean that at least one active session is in that state. Use
`downstreamConnections`, `downstreamJoinedCount`, `upstreamStartedCount`, and
`upstreamReadyCount` for exact multi-client status.

## Notes

- The relay is transparent unless a handler edits, replaces, or cancels a packet.
- `packet.replace(name, params)` can replace the current packet with a newly encoded packet.
- Unknown or not-yet-supported packet mutations keep the original packet flowing instead of dropping traffic.
- A locally closed RakNet client notifies the peer before releasing its UDP
  socket, so the corresponding relay session is removed immediately instead of
  waiting for a transport timeout.
