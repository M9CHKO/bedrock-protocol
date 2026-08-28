# Client, Server, And Relay Creation

The normal public API follows the three JavaScript entry points:

| Task | Options type | Factory |
|---|---|---|
| Bot/client | `ClientOptions` (`BotOptions` is an alias) | `createClient` or `createBot` |
| Bedrock server | `ServerOptions` | `createServer` |
| Live proxy | `RelayOptions` | construct `Relay`, then `listen` |

All three APIs are Bedrock-only. None of these types starts a Java Edition
protocol path.

## Create A Bot

```cpp
auto client = bedrock::createClient({
    .host = "localhost",
    .port = 19132,
    .username = "ExampleBot",
    .version = "1.21.100",
    .offline = true
});
```

`createBot` is a readable alias with the same behavior:

```cpp
bedrock::BotOptions options {
    .host = "localhost",
    .username = "ExampleBot",
    .offline = true
};
auto bot = bedrock::createBot(std::move(options));
```

The common options are:

| Field | Default | Meaning |
|---|---:|---|
| `host` | `localhost` | Destination Bedrock address. |
| `port` | `19132` | Destination Bedrock port. |
| `username` | `Bot` | Offline name or online authentication profile. |
| `version` | omitted | Ping first and use the advertised supported version, otherwise the library's current version. An explicit value must be an exact supported release. |
| `offline` | `false` | `true` uses a self-signed offline login; `false` authenticates with Microsoft/Xbox Live. |
| `autoInitPlayer` | `true` | Send the local-player init packet, enter `Initialized`, and emit `spawn` automatically after `play_status.player_spawn`. |
| `viewDistance` | omitted | JavaScript-compatible requested view-distance option. |
| `authTitle` | omitted | Optional Xbox authentication title id. |
| `profilesFolder` | default cache | Authentication cache location or `false`. |
| `connectTimeout` | `9000` | Connection timeout in milliseconds. |
| `skipPing` | `false` | Connect without the discovery ping. |
| `followPort` | automatic | Follow the port advertised by ping; defaults off for Realms. |
| `conLog` | console | Connection progress callback. |
| `realms` | disabled | Join a Bedrock Realm by id, invite, or picker. |
| `batchingInterval` | `20` | Outgoing queue interval in milliseconds. |

The C++-only `debug`, `decodePackets`, `packetDump`, and `quiet` switches live
under `diagnostics`; for example, set
`options.diagnostics.debug = DebugMode::Events`. The larger
`LegacyClientOptions` (the old `Options` name is retained) is available
for native extension seams such as a prebuilt login packet or injected
Authflow. A typed legacy value still works with `createClient`; direct
`createClient({...})` selects the compact `ClientOptions` facade.

Set `autoInitPlayer = false` when a bot must finish world initialization
itself. As in `bedrock-protocol`, `player_spawn` then leaves the connection in
`Initializing` and does not emit the client's `spawn` event:

```cpp
auto bot = bedrock::createBot({
    .host = "127.0.0.1",
    .username = "ManualBot",
    .version = "1.21.100",
    .offline = true,
    .autoInitPlayer = false
});

bot.on("play_status", [&bot](const bedrock::Packet& packet) {
    if (packet.get("status") != "player_spawn") return;

    const auto entityId = bot.entityId();
    const auto startGame = bot.startGameData();
    if (!entityId || !startGame) return;

    bot.write("set_local_player_as_initialized", bedrock::object({
        {"runtime_entity_id", bedrock::u64(*entityId)}
    }));
    bot.setStatus(bedrock::ClientStatus::Initialized);
});
```

`startGameData()` retains and decodes the latest `start_game` packet even when
there is no `start_game` listener. Public mapper fields use the JavaScript enum
name (`player_spawn`) rather than the low-level diagnostic form
(`3/player_spawn`). `entityId()` is the cached runtime entity id.

`setStatus(next)` is also available for other advanced lifecycle control.
Status handlers run before the new value is stored, so `status()` returns the
previous state inside the callback. `updateItemPalette(palette)` exposes
JavaScript's connection helper for manually supplied item-state arrays:

```cpp
bot.updateItemPalette(bedrock::array({
    bedrock::object({
        {"name", bedrock::str("minecraft:shield")},
        {"runtime_id", bedrock::u64(355)}
    })
}));
```

Incoming or outgoing `start_game` and `item_registry` packets still update the
shared `ShieldItemID` packet variable automatically.

## Create A Server

```cpp
auto server = bedrock::createServer({
    .host = "0.0.0.0",
    .port = 19132,
    .version = "1.21.100",
    .offline = true,
    .motd = {"Example server", "Example world"},
    .maxPlayers = 10
});
```

`createServer` starts listening before it returns. Constructing
`BedrockServer` directly leaves it stopped until `listen()` is called. The
direct call returns the configured JavaScript-shaped address:

```cpp
bedrock::BedrockServer server(options);
const auto address = server.listen();
std::cout << address.host << ":" << address.port << "\n";
```

If the configured port is `0`, `address.port` is still `0`, matching
`server.js`; use `server.boundPort()` for the UDP port selected by the OS.
`ServerOptions` is the compact JavaScript-shaped facade. Native lifecycle
switches are grouped under `ServerOptions::advanced`; the full
`BedrockServerOptions` type remains available for direct runtime construction.
Existing MOTD maps such as `.motd = {{"motd", "Example"}}` continue to work.

`advertisementFn` follows the JavaScript signature and may return an
advertisement by value:

```cpp
options.advertisementFn = [] {
    return bedrock::ServerAdvertisement({
        {"motd", "Dynamic MOTD"},
        {"playersOnline", 4},
        {"playersMax", 20}
    }, 19132, "1.21.100");
};
```

The former C++ callback returning `ServerAdvertisement&` is still accepted and
retains its reference semantics.

For a server, `offline = true` accepts self-signed clients. `offline = false`
verifies the Bedrock login chain. It does not refer to Java Edition online
mode.

`bedrock::Player` is the public name for the shared server-side player view.
Use `server.on("connect", ...)` as in JavaScript, then attach events to that
specific player:

```cpp
server.on("connect", [](const bedrock::Player& player) {
    player.onLogin([player](const bedrock::BedrockServerPacketEvent&) {
        if (const auto profile = player.profile()) {
            std::cout << profile->name << " xuid=" << profile->xuid << "\n";
        }
    });
    player.on("join", bedrock::Player::VoidHandler([] {
        // Login and encryption are ready; world initialization follows.
    }));
    player.on("packet", bedrock::Player::PacketHandler(
        [](const bedrock::BedrockServerPacketEvent& event) {
            std::cout << event.packet.name << "\n";
        }
    ));
});
```

Player copies share listener state and refer to the same live session. The
facade exposes `status()`, `setStatus()`, `profile()`, `version()`, `getUserData()`,
`skinData()`, version comparison helpers, `write`, `queue`, `sendBuffer`,
`sendQueued`, `updateItemPalette`, `disconnect`, `sendDisconnectStatus`, and
`close`. Lifecycle
listeners are available as `onLoggingIn`, `onServerClientHandshake`,
`onLogin`, `onJoin`, `onSpawn`, `onStatus`, `onError`, and `onClose`, plus the
JavaScript-shaped `on("...")` overloads. As in Node, status callbacks run
before the new status is stored, named packet listeners run before `packet`,
and `close` runs before the Player is removed.
`version()` returns the client-reported Bedrock protocol number; compare it to
a release string with `versionLessThan`/`versionGreaterThan` when that is more
convenient. Retained Player copies must not be used after their owning Server
has been destroyed.

As with JavaScript `Server.clients`, accepted players can be found by their
endpoint key. C++ returns synchronized snapshots instead of exposing a map
that the RakNet thread mutates concurrently:

```cpp
for (const auto& [key, player] : server.clients()) {
    std::cout << key << " " << player.clientGuid << "\n";
}
if (const auto player = server.client(endpointKey)) {
    player->disconnect("maintenance");
}
```

`Player::key()` produces the same key. A Player `close` callback still sees the
entry; it is removed immediately after that callback, matching `server.js`.

## Create A Relay

```cpp
bedrock::RelayOptions options {
    .version = "1.21.100",
    .host = "0.0.0.0",
    .port = 19132,
    .motd = "Example relay",
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
    // The destination client is ready and queued serverbound packets flushed.
});
relay.onClientbound([](bedrock::RelayPacketEvent& packet) {
    // destination server -> Minecraft client
});
relay.onServerbound([](bedrock::RelayPacketEvent& packet) {
    // Minecraft client -> destination server
});
const auto listener = relay.listen();
```

This is the C++ equivalent of JavaScript's `new Relay(options)`. The
`createRelay(options)` convenience function remains available for existing C++
code, but the packet-only runtime now has the unambiguous name
`createPacketRelay`.

You normally set `offline` once. It applies to both the listener and the
destination connection:

| `options.offline` | `destination.offline` | Listener | Destination |
|---:|---:|---|---|
| `false` | omitted | verify online login | Xbox Live login |
| `true` | omitted | accept offline login | offline login |
| `false` | `true` | verify online login | offline login |
| `true` | `false` | accept offline login | Xbox Live login |

The nested value is therefore an override, not a second required setting.
`listenerOffline()` and `destinationOffline()` expose the two resolved values.
If both endpoints use the same mode, leave `destination.offline` omitted.

The high-level relay accepts multiple downstream players by default. Set
`forceSingle = true` for one player. `logging`, `enableChunkCaching`, and
`omitParseErrors` map to the JavaScript Relay behavior. A Realm destination is
selected through `destination.realms`.

`onConnect(player)` runs when Minecraft opens the downstream connection.
`onJoin(player, upstream)` is the later JavaScript Relay `join` event: the
destination client has joined, its forced `client_cache_status` was written,
`player.upstream` is bound, and pending serverbound packets were flushed. The
same event is available as `relay.on("join", handler)`. Every concurrent player
receives its own matching `BedrockNetworkClient` reference.

`Relay::listen()` returns the same configured `{host, port}` result as Server.
Because JavaScript Relay extends Server, C++ also exposes `relay.clients()` and
`relay.client(player.connection.key())`. These are owning snapshots of active
`RelayPlayer` objects keyed by downstream endpoint; `players()` and
`playerCount()` remain available for session-oriented code.

`RelayPlayer` also exposes the same downstream Player facade: profile/status
accessors, writable status, manual item-palette updates, lifecycle listeners,
version helpers, and disconnect/close methods.
Its internal downstream `join` listener runs before listeners registered from
`relay.onConnect`, matching the queue-release order in `relay.js`.

Online authentication can request a Microsoft device code. As in JavaScript,
the callback receives the exact downstream player whose upstream login needs
it:

```cpp
options.onMsaCode = [](
    const bedrock::XboxDeviceCodeInfo& code,
    bedrock::RelayPlayer& player
) {
    std::cout << player.sessionId() << ": " << code.message << "\n";
};
```

The former one-argument C++ callback remains valid. Without any callback, the
Relay disconnects only that player with the JavaScript sign-in-and-reconnect
message; other relay sessions keep running.

## Bedrock Block Registry

`MinecraftDataAssets` loads the complete Bedrock block registry directly from
`blocks.json`, `blockStates.json`, and `blockCollisionShapes.json`:

```cpp
bedrock::MinecraftDataAssets assets;
auto blocks = assets.loadBedrockBlockRegistryByVersion("1.21.100");

auto slab = blocks.fromProperties(
    "oak_slab",
    {{"minecraft:vertical_half", bedrock::BedrockBlockProperty::string("top")}}
);
if (slab) {
    std::cout << slab->stateId << " " << slab->displayName << "\n";
}
```

The registry provides `blockByType`, `blockByName`, `blockByStateId`,
`stateById`, `fromStateId`, `fromProperties`, and `fromString`. A returned
`BedrockBlock` includes typed Bedrock properties, hardness, light data,
harvest tools, drops, hash, and state-specific collision shapes. Block hashes
match the historical `prismarine-block` little-endian NBT/FNV-1a result and
are enabled from Bedrock protocol 582 (Minecraft 1.19.80).

For `0.14` and `1.0`, where minecraft-data has no `blockStates.json`, the
loader applies the original numeric mapping `stateId = (type << 4) | metadata`.
It preserves metadata display-name variations and last-write-wins duplicate
name indexes while state lookups retain the exact numeric block type.

Raw `BedrockBlock::shapes` retain minecraft-data's Bedrock representation:
`{centerX, centerY, centerZ, sizeX, sizeY, sizeZ}`. Use
`block.raycastShapes()` or the direct registry lookup
`registry.raycastShapesForState(stateId)` to obtain
`{minX, minY, minZ, maxX, maxY, maxZ}` boxes for `BedrockWorld::raycast`:

```cpp
const auto hit = world.raycast(
    origin,
    direction,
    6.0,
    {},
    [&blocks](int32_t stateId, const bedrock::BlockPosition&) {
        return blocks.raycastShapesForState(stateId);
    }
);
```

`BedrockBlock::canHarvest` mirrors the Node helper. `digTime` uses the same
breaking formula and accepts a caller-supplied Bedrock tool multiplier; it
does not load cross-edition material tables. The older compact
`BlockRuntimeRegistry` API remains available. If its generated TSV is absent,
`MinecraftDataAssets` derives it from the Bedrock JSON registry and maps each
name to that block's default state.

Bedrock sign blocks expose the `prismarine-block` helpers directly. Text is
stored in the native Bedrock `Text` string and an absent entity is initialized
with `id: "Sign"`; unrelated NBT fields are preserved:

```cpp
auto sign = blocks.fromStateId(
    blocks.blockByName("standing_sign")->defaultState
);
sign->setSignText(std::vector<std::string> {"Line one", "Line two"});
std::cout << sign->signText() << "\n";
```

`getSignText()` returns the one-element Bedrock side array, matching Node.
`blockEntity()` exposes the typed `NbtDocument`, while
`propertiesWithComputedStates()` merges superimposed-state values and
`isWaterlogged()` reports the effective waterlogged value. Sign helpers reject
non-sign blocks instead of exposing a helper that does not exist for that
block type in JavaScript.

## Bedrock Items

`BedrockItemRegistry` is the Bedrock branch of `prismarine-item`, backed by
the bundled `items.json` and Bedrock `enchantments.json`:

```cpp
auto items = assets.loadBedrockItemRegistryByVersion("1.21.100");
auto sword = items.create(
    items.itemByName("diamond_sword")->id,
    1
);

sword.setCustomName("Relay Blade");
sword.setCustomLore({"Bedrock only", "Created in C++"});
sword.setDurabilityUsed(7);
sword.setEnchantments({
    {.name = "sharpness", .level = 5},
    {.name = "unbreaking", .level = 2}
});
```

Locally created stacks receive monotonically increasing `stackId` values.
Durable items receive explicit `Damage: 0` NBT, matching current Bedrock
`prismarine-item`. The object exposes custom name/lore, repair cost,
durability, normalized enchantments, `CanPlaceOn`, `CanDestroy`, spawn-egg
mob names, and Node-compatible equality controls.

Use `toNetwork`/`fromNetwork` for the typed `BedrockNetworkItem` form and
`toProtoDefValue`/`fromProtoDefValue` directly with packet fields:

```cpp
bot.write("inventory_slot", bedrock::object({
    {"item", items.toProtoDefValue(sword)}
}));
```

The converter selects the packed `auxiliary_value` layout before protocol 440
and the modern count/metadata/optional-stack-id layout afterward. NBT is
encoded in the exact Item schema format; current Item byte output matches the
Node serializer. `prismarine-item` itself does not provide an anvil function
for Bedrock, so no cross-edition anvil implementation is introduced here.

## Bedrock Static Registry And Gameplay Data

`BedrockRegistry` is a version-aware facade over blocks, items, biomes, and
entities, as well as recipes, inventory windows, note-block instruments, and
attributes. It follows minecraft-data's per-version Bedrock remaps, so each
component may come from the nearest compatible Bedrock dataset:

```cpp
bedrock::MinecraftDataAssets assets;
auto registry = assets.loadBedrockRegistryByVersion("1.21.100");

const auto* plains = registry.biomeByName("minecraft:plains");
const auto* chicken = registry.entityByName("chicken");
const auto* stone = registry.blockByName("stone");
const auto* sword = registry.itemByName("diamond_sword");
const auto* recipe = registry.recipeById(0);
const auto* inventory = registry.windowById("inventory");
const auto* harp = registry.instrumentByName("harp");
```

The component registries are available through `blocks()`, `items()`,
`biomes()`, `entities()`, `recipes()`, `windows()`, `instruments()`, and
`attributes()`. Every component can also be loaded independently through the
matching `MinecraftDataAssets::loadBedrock...RegistryByVersion` or
`ByProtocol` method.

`BedrockBiomeDefinition` preserves both generations of the Bedrock schema:
legacy `precipitation`, `rainfall`, `depth`, parent/child links and climate
points, plus the current `hasPrecipitation` flag. `precipitationEnabled()`
normalizes either representation. `BedrockBiomeRegistry::biome(id)` mirrors
`prismarine-biome`: an unknown ID produces an empty value retaining that ID,
with zero color/rainfall/temperature and null height. Pointer lookups return
`nullptr` instead.

`BedrockEntityDefinition` keeps the static network ID, internal ID, nullable
dimensions, type, and optional category. Bedrock minecraft-data contains a few
duplicate names and internal IDs. `entityByName` and `entityByInternalId`
match the Node indexer's last-write-wins behavior; `entitiesByName` and
`entitiesByInternalId` return every matching definition in source order.

`BedrockRecipeRegistry` keeps the quoted numeric recipe key as a sparse
`uint32_t` ID. `recipeByName` is last-write-wins because Bedrock contains
duplicate recipe names; `recipesByName` returns all matches in numeric ID
order, and `recipesByType` groups recipes by crafting station. Ingredients,
the optional input matrix, multiple outputs, priority, optional metadata, and
structured output NBT are retained without converting NBT to text:

```cpp
auto recipes = assets.loadBedrockRecipeRegistryByVersion("1.21.100");
if (const auto* recipe = recipes.recipeById(1524)) {
    const auto& output = recipe->output.front();
    if (output.nbt) {
        std::cout << bedrock::protoDefValueToJson(*output.nbt) << "\n";
    }
}
```

`BedrockWindowRegistry` exposes window ID/name indexes, declared slot ranges,
properties, and `openedWith` metadata. `BedrockInstrumentRegistry` indexes
note-block instruments by numeric ID and name. `BedrockAttributeRegistry`
indexes attributes by friendly name and Bedrock resource name; definitions
also provide `clamp(value)`.

These categories are optional exactly as they are in minecraft-data. For
example, current Bedrock versions remap recipes to `1.19.10`, windows to
`1.16.201`, and instruments to `1.17.0`, but no longer expose the static
attributes table. In that case `attributes()` is an empty registry and its
resolved path is empty; the loader never guesses a file from the selected
version directory.

Only `bedrock/*/biomes.json` and `bedrock/*/entities.json` are loaded. A static
effects registry is not exposed because minecraft-data currently remaps that
category to a Java Edition dataset.

### Dynamic Item And Block Runtime Palettes

`BedrockRegistry` ports the mutable Bedrock branch of `prismarine-registry`.
Before Minecraft 1.21.60 the item palette is part of `start_game`; newer
servers send it separately in `item_registry`:

```cpp
auto registry = assets.loadBedrockRegistryByVersion("1.21.100");

// Feed the structured packet parameters obtained from the packet decoder.
registry.handleStartGame(decodedStartGameParams);
registry.handleItemRegistry(decodedItemRegistryParams);

const auto* state = registry.blockStateByRuntimeId(runtimeId);
const auto itemstates = registry.writeItemStatesValue();
```

`handleStartGame` creates `blocksByRuntimeId` semantics. It uses sequential
state IDs for older versions and switches to hashes only when both the
`blockHashes` feature and `block_network_ids_are_hashes` packet flag are true.
The network-palette hash is computed from each typed state descriptor, matching
`prismarine-registry`; it is intentionally distinct from the historical
property-name-only `BedrockBlock::hash` behavior.

`loadItemStates` replaces item indexes in packet order. Known vanilla names
inherit their static display, stack, durability, variation, and enchantment
metadata while receiving the server's runtime ID. Unknown namespaces remain
valid custom items. As in Node, duplicate names and IDs remain in the array and
the corresponding independent index is last-write-wins. `writeItemStates`
restores `minecraft:` for vanilla names and derives `component_based` from the
namespace. Newer optional `version` and item-component NBT fields are retained
losslessly for round trips.

## Bedrock Loot And Default Skin

`MinecraftDataAssets` also exposes the Bedrock-only block/entity loot tables
and the versioned `defaultSkin` object used by `bedrock-protocol` login:

```cpp
bedrock::MinecraftDataAssets assets;
auto loot = assets.loadBedrockLootRegistryByVersion("1.18.0");

bedrock::BedrockLootBlockStates states {
    {"stone_type", bedrock::ProtoDefValue::string("andesite")}
};
const auto* drop = loot.blockLootForStates("minecraft:stone", states);
const auto variants = loot.blockLootVariants("stone");
const auto* piglin = loot.entityLootByName("zombified_piglin");

auto skin = assets.loadBedrockDefaultSkinByVersion("1.26.0");
if (skin) {
    sendLoginSkinObject(skin->raw());
    uploadRgba(
        skin->skinImage().width,
        skin->skinImage().height,
        skin->skinImage().bytes
    );
}
```

`blockLootByName` matches minecraft-data's `blockLoot` index: duplicate block
names use the final source entry. Bedrock has thousands of state-conditioned
entries, so `blockLootVariants` preserves all of them in source order and
`blockLootForStates` selects the most specific matching condition. Extra
states supplied by the caller are allowed. Drop definitions retain metadata,
chance, nullable stack-size bounds, block age, silk-touch/no-silk-touch, and
player-kill conditions. Entity lookup accepts names with or without the
`minecraft:` namespace.

`BedrockDefaultSkin` retains the original `ProtoDefValue` object and exact file
JSON for forward-compatible login use. Its typed view decodes skin, cape, and
animated RGBA images; geometry and resource-patch JSON; optional geometry
engine version; animation bytes; persona pieces; and tint colors. Image
`validRgba()` verifies `width * height * 4`. Versions without an explicit
`steve` remap return `std::nullopt` instead of guessing a file.

The unified registry exposes the same categories through `registry.loot()`,
`registry.blockLootByName(...)`, `registry.entityLootByName(...)`, and the
nullable `registry.defaultSkin()` pointer. Loot is optional independently of
the default skin. For example, the current release has a mapped default skin
but no mapped static loot table.

`blockMappings`, `blocksB2J`, and `blocksJ2B` are deliberately not exposed:
they map between Java Edition and Bedrock identifiers and therefore are not a
Bedrock-only runtime API.

## Bedrock Features And Version Comparisons

`BedrockFeatureRegistry` ports minecraft-data's Bedrock `supportFeature` and
`Version` behavior from `bedrock/common/features.json` and
`protocolVersions.json`:

```cpp
auto features = assets.loadBedrockFeatureRegistryByVersion("1.21.90");

if (features.supportsFeature("newLoginIdentityFields")) {
    // Use the wrapped Bedrock login identity shape.
}

const auto durabilityKey =
    features.featureString("whereDurabilityIsSerialized"); // "Damage"

if (features.isNewerOrEqualTo("1.20.61")) {
    // The batch header carries its compressor ID.
}
```

`supportFeature(name)` returns a typed `ProtoDefValue`, matching JavaScript:
boolean features return `Bool`, value features retain values such as
`"Damage"`, `"short"`, and `"ench"`, and unknown, disabled, or otherwise
falsey features return `Bool(false)`. `feature(name)` also exposes the source
description and resolved value, while `all()` retains source order.

Version comparisons use minecraft-data's ordering rather than lexical string
comparison. Exact `.0` releases provide major aliases (`1.20` equals
`1.20.0`), `_major` feature ranges expand to the oldest/newest release of that
major, and `latest` is open-ended. Duplicate protocol lookups select the newest
Bedrock release, so protocol `594` resolves to `1.20.15` and `567` resolves to
`1.19.62`.

The unified `BedrockRegistry` exposes the same data through `features()`,
`supportFeature(name)`, and `supportsFeature(name)`. Block hashes and the
legacy Item `auxiliary_value` shape are now selected from these feature flags,
not from duplicated protocol thresholds. Pre-1.16.201 Item datasets retain the
older protocol fallback because minecraft-data's feature range starts at
1.16.201 while those historical Bedrock packet schemas already use
`auxiliary_value`.

## Bedrock Chat And Language

`MinecraftDataAssets` follows the Bedrock `language` remap and creates the
Bedrock branch of `prismarine-chat`:

```cpp
bedrock::MinecraftDataAssets assets;
auto chat = assets.loadBedrockChatByVersion("1.21.100");

auto message = chat.message(bedrock::object({
    {"translate", bedrock::str("chat.type.text")},
    {"with", bedrock::array({
        bedrock::object({
            {"text", bedrock::str("Steve")},
            {"color", bedrock::str("aqua")}
        }),
        bedrock::str("Hello from C++")
    })}
}));

std::cout << message.toString() << "\n"; // <Steve> Hello from C++
std::cout << message.toMotd() << "\n";   // includes Bedrock section codes
std::cout << message.toAnsi() << "\n";   // terminal colors
std::cout << message.toHTML() << "\n";   // escaped styled HTML
```

Translation formatting supports sequential `%s`, positional `%1$s`, and
literal `%%` substitutions. Missing keys use `fallback` and then the key
itself. Output follows the Node depth limit of 8 and length limit of 4096.
Legacy `§` colors, modifiers, reset behavior, RGB colors, HTML escaping, and
Unicode JSON escapes are supported. `BedrockMessageBuilder` provides the
fluent `setText`, `setTranslate`, `addWith`, `addExtra`, style, score,
click-event, and generic hover-event surface. `fromNotch` accepts a JSON
component or plain Bedrock text.

`TextPacket` now retains the complete Bedrock packet shape: `type`,
`needsTranslation`, `sourceName`, `message`, `parameters`, `xuid`,
`platformChatId`, and `filteredMessage`. A bot can render it directly:

```cpp
client.onText([&chat](const bedrock::TextPacket& packet) {
    std::cout << chat.fromTextPacket(packet).toAnsi() << "\n";
});
```

Translation, popup, and jukebox-popup packets use their parameter array; JSON
variants are parsed as components. The low-level `PacketPayloadReader` and
`VersionedPayloadReader` expose the same parameter and filtered-message fields.
The NBT chat-component and client-side `fromNetwork` paths in
`prismarine-chat` are Java Edition protocol features and are not included.

## Bedrock Chunk Columns And Worlds

### Early Numeric-ID Chunks

`BedrockChunk014` and `BedrockChunk10` port the two early Bedrock
`prismarine-chunk` implementations. They expose block type/data, block and sky
light nibbles, biome/height access, `initialize`, typed `getBlock`/`setBlock`,
`load`, `dump`, and the `0xffff` mask. The 1.0 fixed subchunk is also available
as `BedrockSubChunk10`:

```cpp
auto blocks014 = assets.loadBedrockBlockRegistryByVersion("0.14");
bedrock::BedrockChunk014 chunk;
chunk.setBlockType({.x = 1, .y = 40, .z = 2}, 1);
chunk.setBlockData({.x = 1, .y = 40, .z = 2}, 1); // granite metadata

auto granite = chunk.getBlock({.x = 1, .y = 40, .z = 2}, blocks014);
auto wire = chunk.dump(); // exactly 83,200 bytes
```

`BedrockChunk10::dump()` emits the exact 164,627-byte Node layout. Its
historical implementation aliases height and biome to the same 2D byte and
ignores the biome/height trailer in `load()`; C++ intentionally retains those
observable semantics for byte compatibility. The `ChunkVersion` enum includes
the complete Bedrock constants from `V0_9_00` through `V1_18_0`.

### Paletted Chunk Columns

`BedrockChunkColumn` mirrors the Bedrock `CommonChunkColumn` metadata API. A
loaded subchunk owns its block and sky light values; like JavaScript, direct
column light access requires that section to exist:

```cpp
bedrock::BedrockChunkColumn column(0, 0);
column.setBounds(-4, 20);
column.setBlockStateId({.x = 3, .y = -16, .z = 5}, 42);
column.setBlockLight({.x = 3, .y = -16, .z = 5}, 9);
column.setSkyLight({.x = 3, .y = -16, .z = 5}, 15);

auto* section = column.getSectionAtIndex(-1);
```

`BedrockWorld::getBlockLight` and `getSkyLight` return zero for an unloaded
column, while their setters are no-ops, matching `prismarine-world`.

The registry-backed overloads return complete `BedrockBlock` objects and write
all Bedrock block metadata:

```cpp
auto block = column.getBlock(position, blocks); // full=true by default
column.setBlock(position, *block, blocks);

auto worldBlock = world.getBlock(position, blocks);
world.setBlock(position, *worldBlock, blocks);
```

The explicit registry lets a newly allocated subchunk use that version's real
air default state (modern Bedrock does not assign air to state `0`). Passing
`full=false` skips light and block-entity NBT. When `BlockPosition::layer` is
unset, `superimposed` represents the second Bedrock storage layer and water in
that layer supplies the computed `waterlogged` state; setting an explicit
layer reads or writes only that layer. As in `CommonChunkColumn`, writing a
block without an entity does not delete an entity already stored at the same
position.

Heightmaps use `BedrockHeightMap` (256 `uint16_t` values). `loadHeights`,
`getHeights`, and `writeHeightMap` follow the Node API; writing an unset map
creates and emits a zero-filled 512-byte little-endian map.

Block entities can be inserted from their coordinates with `addBlockEntity`
and moved inside a column with `moveBlockEntity`. Regular actor NBT is keyed by
its signed `UniqueID`:

```cpp
column.addEntity(entityNbt);
const auto& entities = column.getEntities();
const auto diskNbt = column.diskEncodeEntities();

bedrock::BedrockChunkColumn restored;
restored.diskDecodeEntities(diskNbt);
```

`diskEncodeEntities`/`diskDecodeEntities` and the corresponding block-entity
methods use the Bedrock little-endian NBT sequence used by LevelDB. Network
block entities continue to use little-varint NBT.

The `prismarine-world` traversal helpers are available as
`ManhattanIterator` (including the historical `ManathanIterator` alias),
`OctahedronIterator`, `SpiralIterator2d`, and `RaycastIterator`. Their ordering,
voxel tie-breaking, face values, and shape intersection match Node.

`BedrockWorld::raycast` uses those rules directly against loaded Bedrock
columns. By default state id `0` is air and every other state is treated as a
full cube:

```cpp
const auto hit = world.raycast(
    {.x = 0.5, .y = 65.6, .z = 0.5},
    {.x = 1.0, .y = 0.0, .z = 0.0},
    6.0
);
if (hit) {
    std::cout << hit->position.x << " face="
              << static_cast<int>(hit->face) << "\n";
}
```

The optional matcher filters state ids and positions. An optional shape
provider returns ray-intersection boxes in
`{minX, minY, minZ, maxX, maxY, maxZ}` form, allowing slabs, doors, and other
non-full blocks to return an exact intersection point. Use
`BedrockBlockRegistry::raycastShapesForState` to convert the raw Bedrock
center/size representation.

The one-argument `BedrockWorld::getBlock` returns a synchronized snapshot of
state id, block light, sky light, biome, and optional block-entity NBT. The
registry-backed overload returns `BedrockBlock` and attaches its global
position. `onBlockUpdate(handler)`
receives the old and new snapshots after a world setter mutates a loaded
column. The coordinate overload mirrors Node's `blockUpdate:x,y,z` event:

```cpp
world.onBlockUpdate({.x = 4, .y = 64, .z = 8}, [](
    const bedrock::BedrockWorldBlockSnapshot& oldBlock,
    const bedrock::BedrockWorldBlockSnapshot& newBlock
) {
    std::cout << oldBlock.stateId << " -> " << newBlock.stateId << "\n";
});
```

Global handlers run before coordinate handlers, matching `prismarine-world`.
Setters and `getBlock` remain no-ops/null for unloaded columns.

For generated or persistent worlds, configure synchronous C++ callbacks. The
control flow matches the promises in Node: storage is tried first, a generator
is the fallback, generated columns are queued for saving, and loaded columns
are not rewritten until changed.

```cpp
bedrock::BedrockWorld world({
    .chunkGenerator = [](int32_t x, int32_t z) {
        return bedrock::BedrockChunkColumn(x, z);
    },
    .loadColumn = loadBedrockColumn,
    .saveColumn = saveBedrockColumn
});

auto* column = world.getColumn(2, -3);
world.setBlockStateId({.x = 33, .y = 64, .z = -47}, stoneStateId);
world.waitSaving();
```

`setColumn(..., save)`, `queueSaving`, `saveAt`, `saveNow`, and `waitSaving`
mirror `prismarine-world`. Unloading a dirty column is deferred until its save
callback succeeds; the unload event runs before `onDoneSaving`. The callbacks
operate only on `BedrockChunkColumn`; Java Anvil storage is intentionally not
part of this library.

## Advanced Layers

The compatibility layers remain public for protocol work:

- `LegacyClientOptions` / `Options`: full native client configuration.
- `BedrockServerOptions`: direct native server configuration.
- `BedrockLiveRelayOptions` with `createLiveRelay`: direct server/upstream
  composition and low-level session access.
- `BedrockRelayOptions` with `createPacketRelay`: packet-only relay core
  without sockets.

Use these only when the compact creation API does not expose the required
extension. Existing typed calls remain source-compatible.
