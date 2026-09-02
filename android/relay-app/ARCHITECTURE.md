# CPE Relay Android structure

The Android client is split by responsibility so packet decoding, analysis,
and on-screen presentation can evolve independently.

## App and lifecycle

- `MainActivity.java` — launcher activity, Android permissions, and document
  pickers used by optional modules.
- `RelayService.java` — foreground-service lifecycle, polling cadence, and
  coordination between native state and the HUD controllers.
- `RelayOverlayController.java` — categorized in-game settings menu.
- `DiagnosticsLog.java` — opt-in diagnostics storage.

## HUD controllers

- `EntityOutlineOverlayController.java` — world-to-screen entity projection,
  smoothing, labels, and danger highlighting.
- `ThreatAnalysisOverlayController.java` — draggable incoming-attack warning.
- `MiniMapOverlayController.java` — draggable packet-derived mini-map.
- `EquipmentOverlayController.java` — hands, armor, and durability HUD.
- `ChunkStatusOverlayController.java` — retained-chunk status widget.
- `SchematicOverlayController.java` — click-through world projection for the
  active construction schematic.

Each HUD owns only its small interactive window. Transparent screen areas do
not consume Minecraft touches, and gameplay HUDs hide while a container,
inventory, or chat UI is open.

## Analysis

- `ThreatAnalyzer.java` — pure Java hostile-mob classification, approach
  tracking, danger scoring, and incoming-damage range estimation.
- `ThreatAnalyzerTest.java` — unit coverage for activation range, passive mobs,
  and defensive reductions.

## Native relay

- `NativeBridge.java` — typed JNI surface used by Java.
- `src/main/cpp/native_bridge.cpp` — version-aware Bedrock packet tracking,
  retained chunks, equipment/attributes/effects, automation requests, and
  mini-map raster generation.

The threat and mini-map features remain packet-only: they do not capture the
screen, inject into Minecraft, or read Minecraft process memory.

## Schematics module

The independent `com.m9chko.bedrockrelay.schematic` package contains all file
and storage logic for the **Schematics** menu page:

- `NbtReader.java` — bounded big- and little-endian NBT decoder.
- `SchematicImporter.java` — `.mcstructure`, vanilla `.nbt`, `.litematic`,
  Sponge `.schem`, and legacy `.schematic` import.
- `SchematicModel.java` — immutable format-neutral palette and block volume.
- `SchematicRepository.java` — app-private library and active-entry selection.

Imported files are converted once and stored in the app's private directory.
The renderer receives only the normalized model plus the existing
packet-derived camera. It is disabled independently from entity outlines,
mini-map, chunks, equipment, automation, and threat analysis.
