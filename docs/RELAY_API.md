# Relay API

`bedrock::Relay` is the C++ proxy API modeled after `bedrock-protocol`'s JavaScript `Relay`.
It creates a local Bedrock server, accepts a Minecraft client, connects to an upstream server, and lets you inspect, edit, cancel, or inject packets in both directions.

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

`queue`, `send`, and `write` are aliases for the same packet injection behavior in the high-level relay API.

## Relay-Level Events

```cpp
relay.on("error", [](const std::string& message) {
    std::cerr << message << "\n";
});

relay.on("status", [](const bedrock::BedrockLiveRelayStatus& status) {
    std::cout << "ready=" << status.upstreamReady << "\n";
});

relay.on("disconnect", [](bedrock::RelayPlayer& player) {
    std::cout << "client disconnected\n";
});
```

## Notes

- The relay is transparent unless a handler edits, replaces, or cancels a packet.
- `packet.replace(name, params)` can replace the current packet with a newly encoded packet.
- Unknown or not-yet-supported packet mutations keep the original packet flowing instead of dropping traffic.
