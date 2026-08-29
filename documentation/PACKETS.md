# Packet Documentation

This project uses PrismarineJS-style Minecraft Bedrock protocol data. Packet names, ids, and field layouts come from bundled files under:

```text
data/minecraft-data/bedrock/<version>/protocol.json
data/minecraft-data/bedrock/<version>/proto.yml
data/minecraft-data/bedrock/<version>/types.yml
```

## Useful References

- ['Minecraft Data documentation'](https://prismarinejs.github.io/minecraft-data/) - browsable data tables.
- [`Mojang/bedrock-protocol-docs`](https://github.com/Mojang/bedrock-protocol-docs) - Mojang's public notes for Bedrock protocol concepts.

## Inspect Packets At Runtime

Print packet names and ids:

```cpp
client.on("packet", [](const bedrock::Packet& packet) {
    std::cout << packet.name << " id=" << packet.id << "\n";
});
```

Enable more decoding:

```cpp
auto client = bedrock::createClient({
    .host = "localhost",
    .port = 19132,
    .username = "Notch",
    .version = "1.21.100",
    .offline = true,
});
```

`decodePackets = true` is useful for debugging, but it is slower on very large packets such as `crafting_data`.

For bot-side examples that read packet fields, change a local packet copy, and send schema-shaped outgoing packets, see [Bot Packet Examples](BOT_PACKETS.md).

## Common Login Flow Packets

Typical Bedrock login flow:

```text
RakNet unconnected ping/pong
RakNet open connection request 1/2
RakNet connected ping/pong
Minecraft NetworkSettingsRequest
Minecraft NetworkSettings
Minecraft Login
Minecraft PlayStatus
Minecraft ResourcePacksInfo
Minecraft ResourcePackClientResponse
Minecraft ResourcePackStack
Minecraft ResourcePackClientResponse
Minecraft StartGame
```

Exact packet order can vary by server and version.

## Version-Specific Packet Data

When investigating a packet for a concrete version, open that version's protocol files first. Example for `1.20.40`:

```text
data/minecraft-data/bedrock/1.20.40/protocol.json
data/minecraft-data/bedrock/1.20.40/proto.yml
data/minecraft-data/bedrock/1.20.40/types.yml
```

For generated lookup tables used by the C++ decoder:

```text
data/generated/protocol-types/bedrock/
data/generated/packet-schema/bedrock/
include/bedrock/generated/GeneratedProtocolTypes.hpp
src/generated/GeneratedProtocolTypes.cpp
```

`GeneratedProtocolTypes.cpp` is version-aware: the encoder and decoder select
the type definition for the negotiated Minecraft version instead of applying
the latest schema to every connection. Regenerate and verify all derived data
with:

```bash
python scripts/generate_protocol_artifacts.py
python scripts/generate_protocol_artifacts.py --check
```
