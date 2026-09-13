# Shared relay modules

These C++20 modules are used by the Android JNI library, the Windows native
library and ordinary library consumers. Frontend visibility preferences do not
replace the library's packet gates. Source lives here rather than in a fork per
platform; Android CMake links `BedrockProtocol::bedrock_protocol` from the root.

## Map pipeline

- `BedrockLiveRelay.hpp`: per-session routing, lifecycle, ordinary forwarding,
  and scheduled map admission. Maps are removed before normal handlers run.
- `MapImageState.hpp`: bounded map validation and merging by map ID. Partial
  rectangles preserve untouched pixels and metadata; output is newly encoded.
- `MapDeliveryQueue.hpp`: asynchronous plaintext storage/rebuild and rolling
  send budget. Defaults: 4000 IDs, 8 updates/s, 512 KiB/s, 125 ms minimum gap,
  one admission at a time; no catch-up burst. RAM budget 8 MiB; disk 512 MiB.
- `MapSpoolPages.hpp`: bounded reusable 4 KiB disk-page allocator; no fixed
  worst-case slot reservation for every map.
- `MapRequestQueue.hpp`: separate demand-side queue, 4000 entries / 256 KiB,
  up to 8 requests/s and two IDs awaiting a response. It does not synthesize
  requests for images the server never supplied.
- `NoRender.hpp`: actor visibility/cache restoration only. It does not own map
  delivery or suppress chest/shulker-box block-entity packets. Replayed actor
  metadata retains the latest server tick.

The default applications always enable map display; library integrations may
still select hidden maps. Even then, transparent/restored maps use the same
scheduler. Failed parsing, missing sessions, overflow and I/O failures never
permit a direct original-payload fallback. Encryption is performed only at
actual submission through the shared per-client output lock.

Read [map-delivery.md](../../../docs/map-delivery.md) for the exact resource
accounting, protocol validation, transport backpressure, reset semantics and
reload limitations. `sent` means transport submission, not rendered pixels.

## Independent gameplay modules

`PlatformBuilder`/`PlatformPlan` handle construction; `AutoCraftStore` handles
craft/store cycles; `ShulkerDeposit` handles unloading; `MapArchive`,
`MapShulkerQueue` and `MapPlacement` handle ZIP-based map crafting/placement.
That ZIP automation is independent of automatic map-image streaming.
`AreaFillGeometry`, `EntityPositionTracker`, `ItemDurability`,
`ChunkPublisherRetention` and `LevelChunkRetentionCache` provide reusable
geometry/tracking/cache logic without UI dependencies.

## Verification

Build with tools and testing enabled, then run CTest. Relevant cases include
`no-render-smoke`, `no-render-live-smoke`, `server-outbound-queue-smoke`,
`map-request-queue-smoke`, `map-request-queue-live-smoke` and
`map-capacity-smoke`. The capacity test covers 4000 full maps within the disk
budget. Live tests use controlled local peers and preserve unrelated gameplay;
they cannot certify real Minecraft rendering or a public backend's policy.

Android-specific UI and settings tests run with `:app:testDebugUnitTest` from
`android/relay-app`. APK build: `:app:assembleRelease`. See its
[README](../../../android/relay-app/README.md) and
[architecture](../../../android/relay-app/ARCHITECTURE.md).
