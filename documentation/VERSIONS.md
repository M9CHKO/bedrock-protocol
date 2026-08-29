# Supported Versions


The library loads bundled protocol data from `data/minecraft-data/bedrock`. Use a concrete version string in `bedrock::createClient` for reproducible bots:

```cpp
auto client = bedrock::createClient({
    .host = "localhost",
    .port = 19132,
    .username = "Notch",
    .version = "1.20.40",
    .offline = true
});
```

When `version` is omitted, `createClient` first uses the server advertisement
and falls back to the bundled release registry's exact `CURRENT_VERSION`, `"1.26.20"`,
if that advertised release is unavailable. Pass another exact key from the
table below to force a version. Explicit values such as `"latest"`, `"auto"`,
and an empty string are rejected, matching `Options.validateOptions()`.

## Version Notes

| Version range | Important protocol behavior |
|---|---|
| `1.20.40` to `1.20.50` | Uses the older compression batch shape. The client must not write the newer compressor id byte in compressed batches. |
| `1.20.61` and newer | Uses the newer compression handling negotiated through `NetworkSettings`. |
| Older than `1.21.90` | Uses the legacy identity chain shape. |
| `1.21.90` and newer | Uses the newer auth/client chain shape used by modern Bedrock clients. |
| `1.26.x` | Bundled data is present and roundtrip-tested for packet encode/decode tables. |

Server MOTD protocol numbers are not always reliable. Some servers advertise an old protocol while accepting newer clients. Pick the version you want the bot to speak and let the client use the matching bundled protocol data.

Static Bedrock data also follows minecraft-data's category-specific remaps.
Blocks, items, biomes, entities, recipes, windows, instruments, attributes,
block/entity loot, the default skin, and language data therefore do not
necessarily use the selected version's directory. Optional categories remain
empty when no Bedrock mapping exists; they never fall back to Java Edition data
or to a guessed local file.

The bundled Bedrock loot tables are explicitly mapped for `1.18.0` (3298 block
entries and 72 entity entries). Other releases do not silently reuse them.
Default-skin mappings begin at `1.16.201`; current versions use the `1.21.70`
persona skin, while historical versions without a `steve` mapping return no
skin. Cross-edition `blockMappings`/`blocksB2J`/`blocksJ2B` datasets are
excluded because they contain Java↔Bedrock conversion data.

`MinecraftDataAssets::loadBedrockFeatureRegistryByVersion` exposes the Bedrock
`features.json` boundaries and minecraft-data version comparisons. Its
`supportsFeature` result should be preferred over protocol-number checks for
format transitions such as block hashes, Item `auxiliary_value`, compressor
headers, item-registry packets, and login identity fields. When multiple
releases share one protocol, `resolveByProtocol` selects the newest release,
matching minecraft-data (for example `594` -> `1.20.15`).

The dynamic registry follows the same boundaries. `handleStartGame` accepts
the legacy embedded item palette and initializes sequential or hashed block
runtime IDs. From `1.21.60`, pass the dedicated `item_registry` packet to
`handleItemRegistry`; its version and NBT component fields are retained when
the palette is written again.

The pre-paletted Bedrock chunk implementations are available separately:
`BedrockChunk014` uses the fixed 128-high 83,200-byte layout and
`BedrockChunk10` uses sixteen fixed subchunks with its 164,627-byte dump.
Legacy block registries synthesize the 16 metadata states per numeric block ID
that minecraft-data creates when `blockStates.json` is unavailable. No
cross-edition biome fallback is loaded for these releases.

The native server follows `serverPlayer.js` when a player reports its protocol.
For `1.19.30` and newer this check runs on `request_network_settings`; older
clients carry the value in `login`. A protocol newer than the configured server
version receives `play_status: failed_spawn` and is closed before network
settings or authentication continue. Equal and older values are accepted, as
in JavaScript. The same decision is available directly through
`BedrockServer::handleClientProtocolVersion(connection, protocol)`.

For a schema-valid `login`, `BedrockServer::onLoggingIn` runs before that
legacy version decision and before JWT verification. Its
`BedrockServerLoggingInEvent` contains the connection, raw packet, and decoded
`LoginPacketData`. `onLogin` remains the later authenticated lifecycle event.
On the client, `Client::onLoggingIn` and
`BedrockNetworkClient::onLoggingIn` run immediately after the login write while
the status is `Authenticating`. A modern client rejected during
`request_network_settings` never sends login and therefore emits neither side's
`loggingIn` event.

The earlier auth lifecycle is exposed through `onSession(profile)`. Offline
sessions emit synchronously before the queue starts; online Authflow sessions
emit after their token promise resolves, with the queue active but before
RakNet transport creation. The profile, authenticated username, and original
Mojang/Xbox token chains remain available from `profile()`, `username()`, and
`accessToken()`. During encrypted login, the client-side
`client.server_handshake` alias runs after `join`/`Initializing` and before the
ordinary `server_to_client_handshake` listener. On the server,
`onServerClientHandshake` runs after encryption starts but before `onLogin` and
its stored verification/profile become visible.

## Version Table

| Library version string | Minecraft version in data | Protocol |
|---:|---:|---:|
| `1.16.201` | 1.16.201 | 422 |
| `1.16.210` | 1.16.210 | 428 |
| `1.16.220` | 1.16.220 | 431 |
| `1.17.0` | 1.17.0 | 440 |
| `1.17.10` | 1.17.10 | 448 |
| `1.17.30` | 1.17.30 | 465 |
| `1.17.40` | 1.17.40 | 471 |
| `1.18.0` | 1.18.0 | 475 |
| `1.18.11` | 1.18.11 | 486 |
| `1.18.30` | 1.18.30 | 503 |
| `1.19.1` | 1.19.1 | 527 |
| `1.19.10` | 1.19.10 | 534 |
| `1.19.20` | 1.19.20 | 544 |
| `1.19.21` | 1.19.21 | 545 |
| `1.19.30` | 1.19.30 | 554 |
| `1.19.40` | 1.19.40 | 557 |
| `1.19.50` | 1.19.50 | 560 |
| `1.19.60` | 1.19.60 | 567 |
| `1.19.62` | 1.19.62 | 567 |
| `1.19.63` | 1.19.63 | 568 |
| `1.19.70` | 1.19.70 | 575 |
| `1.19.80` | 1.19.80 | 582 |
| `1.20.0` | 1.20.0 | 589 |
| `1.20.10` | 1.20.10 | 594 |
| `1.20.15` | 1.20.15 | 594 |
| `1.20.30` | 1.20.30 | 618 |
| `1.20.40` | 1.20.40 | 622 |
| `1.20.50` | 1.20.50 | 630 |
| `1.20.61` | 1.20.61 | 649 |
| `1.20.71` | 1.20.71 | 662 |
| `1.20.80` | 1.20.80 | 671 |
| `1.21.0` | 1.21.0 | 685 |
| `1.21.2` | 1.21.2 | 686 |
| `1.21.20` | 1.21.20 | 712 |
| `1.21.30` | 1.21.30 | 729 |
| `1.21.42` | 1.21.42 | 748 |
| `1.21.50` | 1.21.50 | 766 |
| `1.21.60` | 1.21.60 | 776 |
| `1.21.70` | 1.21.70 | 786 |
| `1.21.80` | 1.21.80 | 800 |
| `1.21.90` | 1.21.90 | 818 |
| `1.21.93` | 1.21.93 | 819 |
| `1.21.100` | 1.21.100 | 827 |
| `1.21.111` | 1.21.111 | 844 |
| `1.21.120` | 1.21.120 | 859 |
| `1.21.124` | 1.21.124 | 860 |
| `1.21.130` | 1.21.130 | 898 |
| `1.26.0` | 1.26.0 | 924 |
| `1.26.10` | 1.26.10 | 944 |
| `1.26.20` | 1.26.20 | 975 |

## How To Check Locally

List versions:

```bash
./build/bedrock --versions
```

Run packet roundtrip checks:

```bash
./build/protocol-roundtrip
```

Expected summary:

```text
[ROUNDTRIP] checkedVersions=50 failures=0
```
