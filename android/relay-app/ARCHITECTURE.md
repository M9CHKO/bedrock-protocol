# CPE Relay Android structure

The Android client is split by responsibility so packet decoding, analysis,
and on-screen presentation can evolve independently.

## Repository map

```text
android/relay-app/
├── README.md                         user guide, APK, permissions, limitations
├── ARCHITECTURE.md                   this source map
├── apk/                              installable arm64 builds and SHA-256 files
└── app/src/
    ├── main/
    │   ├── cpp/
    │   │   ├── native_bridge.cpp     packet tracking and JNI snapshots
    │   │   └── CMakeLists.txt        native Android build
    │   ├── java/com/m9chko/bedrockrelay/
    │   │   ├── MainActivity.java     launcher, SAF pickers, status and auth UI
    │   │   ├── RelayService.java     foreground relay and module coordinator
    │   │   ├── RelayOverlayController.java
    │   │   │                         fixed header + one selected in-game module
    │   │   ├── OverlayModuleCatalog.java
    │   │   │                         compact two-column in-game navigation
    │   │   ├── ModuleCatalogView.java
    │   │   │                         separate launcher module screens
    │   │   ├── InterfaceSettings.java
    │   │   │                         visibility migration, independent of features
    │   │   ├── RelayUi.java          common launcher/overlay visual language
    │   │   ├── *OverlayController.java
    │   │   │                         independent draggable HUD windows
    │   │   ├── ThreatAnalyzer.java   packet-only combat risk model
    │   │   ├── OfficialTexturePack.java
    │   │   │                         bounded Mojang FULL ZIP importer
    │   │   ├── SchematicTextureAtlas.java
    │   │   │                         block face lookup and bitmap cache
    │   │   └── schematic/            format-neutral schematic module
    │   └── res/                      Android resources and fallback artwork
    └── test/java/com/m9chko/bedrockrelay/
        ├── schematic/                format/import/folder tests
        └── *Test.java                camera, durability, threats and paths
```

Generated `build/` directories are not source. Imported textures, auth data,
diagnostic logs, and normalized schematic libraries live in Android app-private
storage and are not committed to the repository.

## App and lifecycle

- `MainActivity.java` — launcher activity, Android permissions, and document
  or directory pickers used by optional modules. Persistable SAF permissions
  let the in-game menu reuse a chosen schematic folder without asking again.
- `RelayService.java` — foreground-service lifecycle, polling cadence, and
  coordination between native state and the HUD controllers.
- `RelayOverlayController.java` — categorized in-game settings menu.
- `DiagnosticsLog.java` — opt-in diagnostics storage.

The launcher uses Connection, Modules, Journal and Settings tabs with a persistent
launch/stop action row on Connection. `RelayUi.java` supplies shared surfaces, contrast and
touch-target styling. Launcher JNI snapshots and log reads are coalesced on
a background worker; schematic service actions also check native state off
the input thread. Native start/stop use one serialized command worker with
service ownership so delayed teardown cannot stop a newer instance.

`ModuleCatalogView` only attaches the selected screen. `OverlayModuleCatalog`
only navigates: `RelayOverlayController` lazily builds one module at a time.
Platform/ZIP/craft controls are shared with the launcher, not duplicated native
implementations. The drawer header remains outside the scrolling content;
height is bounded for landscape. ZIP timing fields collapse without discarding
values. `InterfaceSettings` performs a one-time opt-in migration for floating
controls while preserving feature and server preferences. Legacy map-hide
preferences are cleared, and JNI also ignores that legacy argument.

`ModuleScreensTest` and `OverlayModuleScreensTest` render actual Android views
with Robolectric native graphics into `app/build/module-previews/` and check
module isolation, navigation, state persistence, touch targets and landscape
bounds. They are not an Android-device/Minecraft integration test.

Ordinary diagnostic writes use a bounded 64-entry queue; fatal records take
the synchronous durable path. Journal segments are bounded to 256 KiB each,
and clearing invalidates already-queued writes. The UI reads only a bounded
tail instead of repeatedly rebuilding the entire log.

## HUD controllers

- `EntityOutlineOverlayController.java` — world-to-screen entity projection,
  smoothing, labels, and danger highlighting.
- `ThreatAnalysisOverlayController.java` — draggable incoming-attack warning.
- `MiniMapOverlayController.java` — draggable packet-derived mini-map.
- `EquipmentOverlayController.java` — hands, armor, and durability HUD.
- `ChunkStatusOverlayController.java` — retained-chunk status widget.
- `SchematicOverlayController.java` — click-through world projection for the
  active construction schematic.
- `SchematicTextureAtlas.java` — lazy top/side/bottom texture lookup for
  schematic blocks with diagnostic fallback colors.

Each HUD owns only its small interactive window. Transparent screen areas do
not consume Minecraft touches, and gameplay HUDs hide while a container,
inventory, or chat UI is open.

## Analysis

- `ThreatAnalyzer.java` — pure Java hostile-mob classification, approach
  tracking, danger scoring, and incoming-damage range estimation.
- `ThreatAnalyzerTest.java` — unit coverage for activation range, passive mobs,
  and defensive reductions.

## Native relay

Android CMake links the repository's `BedrockProtocol::bedrock_protocol` target.
There is no Android-only copy of the scheduler: `MapDeliveryQueue`,
`MapImageCodec`, `MapSpoolPages`, `MapRequestQueue` and `NoRender` belong
to the [shared C++ modules](../../include/bedrock/relay/README.md).
Maps are mandatory intercepted plaintext, rebuilt by a disk worker, then
released by the downstream timer through its existing compression/encryption
lock. Configured delivery is 8/s and **512 KiB/s**, max 4000 IDs, 8 MiB queue
budget, 512 MiB disk. Java never stores or forwards image payloads.
See [full invariants and recovery limits](../../docs/map-delivery.md).

- `NativeBridge.java` — typed JNI surface used by Java.
- `src/main/cpp/native_bridge.cpp` — version-aware Bedrock packet tracking,
  retained chunks, equipment/attributes/effects, automation requests, and
  mini-map raster generation.

`include/bedrock/relay/AreaFillGeometry.hpp` holds platform-independent player
bounds, yaw-relative input and sampled route checks. Autofill marker changes
set a dirty flag in packet callbacks; a separate fixed-delay 100 ms worker
publishes them without accumulating a render queue. Placement completion
requires a server world update, not inventory consumption alone.

The threat and mini-map features remain packet-only: they do not capture the
screen, inject into Minecraft, or read Minecraft process memory.

Required server resource packs are transport traffic, not Android assets. When
no structured packet handler is registered, the six resource-pack negotiation
packet types keep their original bytes in both directions so Minecraft itself
can download, validate, and activate the pack. Optional local
`bedrock-samples` textures are imported separately and are used only by CPE
Relay's own HUD and schematic renderer.

## Schematics module

The independent `com.m9chko.bedrockrelay.schematic` package contains all file
and storage logic for the **Schematics** menu page:

- `NbtReader.java` — bounded big- and little-endian NBT decoder.
- `SchematicImporter.java` — `.mcstructure`, vanilla `.nbt`, `.litematic`,
  Sponge `.schem`, and legacy `.schematic` import.
- `SchematicModel.java` — immutable format-neutral palette and block volume.
- `SchematicRepository.java` — app-private library and active-entry selection.
- `SchematicSourceFolder.java` — persisted SAF directory, recursive compatible
  file discovery, and URI metadata used by the in-game importer.

Imported files are converted once and stored in the app's private directory.
The renderer receives only the normalized model plus the existing
packet-derived camera. It is disabled independently from entity outlines,
mini-map, chunks, equipment, automation, and threat analysis.

## Frame and UI flow

```text
Bedrock packets
    ↓
native_bridge.cpp session trackers
    ↓ compact JSON/JNI snapshots
RelayService polling and settings
    ├── entity projection + threat analysis
    ├── mini-map raster request
    ├── equipment/chunk widgets
    └── schematic camera projection
        ↓
independent Android overlay windows
```

Camera position, orientation, packet tick, entity movement, equipment, effects,
attributes, container state, and chunk data share the same relay session
generation. Controllers discard stale generations after reconnect. Container,
inventory, and chat packets set a common UI-blocked state so gameplay widgets
hide while Minecraft menus need the screen.

Only the visible widget bounds and the CPE drawer accept touches. Full-screen
entity and schematic canvases are non-touchable, so transparent overlay areas
do not block Minecraft controls.

## Texture flow

`OfficialTexturePack` validates a user-selected official FULL ZIP, rejects path
traversal and excessive extraction, then stages an atomic app-private texture
set. `TexturePackPaths` recognizes item PNGs, block PNG/TGA files,
`blocks.json`, `terrain_texture.json`, and the enchanted-item glint. TGA block
textures are converted to PNG during import. Existing overlay controllers
invalidate their bitmap caches when the imported generation changes.

Mojang assets are deliberately not bundled in the APK or Git repository. This
keeps distribution separate from the user's optional import and leaves a
functional fallback renderer when no official archive has been selected.
