# Android 1.5.2-NoRender

Historical notes for the versionCode 75 APK. Its map path is superseded in
current source by [bounded map delivery](map-delivery.md). In particular the
old map pass-through, fail-open and 512-map RAM cache described below no longer
apply to current source. The versionCode 75 artifact itself was not overwritten.

Requested rollback, not another network-stall fix. The local original 1.5.2 APK
is dated 2026-09-09 (versionCode 68). Network/decoder code was restored to
66eb396, before the September 11–12 memory, map-throttle, visual-pacing and
chunk-cache changes. Uncommitted 1.5.7/1.5.8 transport diagnostics were removed.
The pre-existing map-queue/placement and Windows edits were preserved.
This is therefore a rollback of the later network work plus a new module,
not a byte-identical reconstruction of the historical APK/source snapshot.

A recoverable copy of removed/replaced files is under
`artifacts/rollback-before-no-render/`. Existing released APKs were not deleted.

## Controls

- Before joining: **Модули → No Render**.
- In game: **CPE → Визуализация → No Render**.
- Master switch, independently selectable maps/entities; settings persist.
- Disable the master switch to restore presentation without reconnecting.
- Version name: `1.5.2-NoRender`; versionCode 75 permits installation over 1.5.7
  and 1.5.8-diagnostics without deleting app data. This is not a new 1.5.9 fix.

## What the module changes

Only clientbound presentation. Map textures become transparent and decorations
are hidden. Item-frame blocks remain. Each map's 128×128 pixels are retained in
a compact raster; partial rectangles merge into it while hidden. Unhiding
reconciles one map per existing 100 ms Android worker tick (large map walls take
time). Hiding already known maps is processed in groups of up to 32 per tick.
New incoming map textures are blanked immediately while enabled.

Actor spawns and metadata retain the original packet bytes except for local
visibility fields: invisible flag, zero scale (including attached equipment),
nametag visibility and text. Turning the module off restores the latest
server-provided values, including actors originally invisible or scaled.
Own-player metadata, actor movement, removals, inventory and all serverbound
traffic are untouched. Actors are not despawned/recreated on toggle. The separate
entity-outline HUD remains controlled by its own switch.

Caches are per session/dimension, capped at 512 maps and 4096 actors. Unsupported
or over-limit packets pass through unchanged and emit a sampled diagnostic;
they are not hidden without restorable data. Map frames, world blocks, particles
and server-side entity simulation are not disabled. This is a relay presentation
module, not a modification of Minecraft's renderer or a guarantee of FPS gains.

## Verification

`no-render-smoke` covers byte-exact off mode, spawn/existing actor hide, restoring
latest metadata, unique-ID removal, map transparency, merged partial map updates,
rapid toggles, dimension/session cleanup, malformed-data pass-through and limits.
`no-render-smoke --live` exercises a real local client → relay → test server,
including actions reaching the server while hidden and toggling back in-session.
Protocol fixture: 1.21.100. The existing local relay connection smoke also covers
1.20.40, 1.20.50 and 1.21.100.

These checks do not verify graphics on the user's Android device and do not
establish that the reported cpe.ign.gg disconnect/native crash is resolved.

Build verification: release APK assembled successfully, signature verified
against the existing Android Debug certificate; `ctest -R '^no-render'` passed
2/2 tests. Original relay connection smoke passed all three listed versions.
No Android device was attached to ADB. APK size: 4,859,167 bytes.
SHA-256: `759883B18BF13A1A0913C86344C84077DA62600E4184AFB8A26511E0B4E093F4`.
