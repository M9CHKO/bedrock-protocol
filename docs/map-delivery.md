# Bounded map delivery (Windows 1.3.8 test build)

The September 13 request adds a mandatory map scheduler to the rolled-back
relay baseline. This does not establish a cause for previous crashes and is
not a claim that Minecraft ran out of memory.

## Invariants

- Every decoded `clientbound_map_item_data` is consumed before normal relay
  handlers. No incoming batch is forwarded whole. Ordinary packets keep their
  usual path. Handler-generated maps, public queue/send/sendBuffer calls and
  the direct clientbound sink have the same mandatory gate (including wire ID).
- Malformed maps, storage failures and overflow are consumed, never passed
  through. Unknown/missing sessions also cannot send the original map.
- The worker merges bounded 128×128 rectangles into state keyed by signed
  map ID. A full image overwrites previous pixels. Full image+metadata snapshots
  also replace older pending input for that ID. Partial updates preserve other
  rectangles and metadata sections not present in the new update.
- The disk worker builds canonical plaintext map records, not ciphertext.
  It validates counts, string lengths, rectangles, varint widths and the entire
  packet before copying/allocating. Maximum accepted payload: 192 KiB;
  tracked metadata: 64 KiB; object/decoration/inclusion counts: 4096 each.
- At most one prepared map is admitted at a time. Maps never enter the normal
  application queue. The ordinary queue is flushed first; maps then acquire
  the SAME per-player output lock, compress, encrypt and send synchronously.
  Encrypted batches are never stored, rearranged or discarded by this scheduler.

## Resource and delivery limits

- At most eight rebuilt packets and 640 KiB plaintext per rolling second.
  Byte charging includes the full packet header and a conservative 5-byte batch
  length prefix. There is at least a 125 ms gap after each actual submission;
  no catch-up credit after a long pause. Large maps may mean fewer than 8/s.
- 8 MiB map queue budget: 3 MiB bounded intake, at most 128 waiting payloads,
  plus a conservative 5 MiB allowance for the bounded index, active worker
  image/record, prepared/in-flight output, serialization temporaries and request
  queue. Of that allowance, 2 MiB cover the enlarged bounded node sets,
  request payloads and a single active request's serialization/encryption temporaries.
  This is a map storage/buffering budget, NOT a total process-memory limit.
- Disk allocation is capped at 512 MiB per session using reusable 4 KiB pages
  sized for each canonical record plus its 4-byte length. A fixed 16 KiB bitmap
  tracks pages. Growth reserves new pages before releasing old ones; failure
  marks reload and never bypasses interception. Settled maps stay on disk for
  later partial updates and No Render restoration. The index, including reload
  markers, is capped at 4000 maps; beyond that a single `reloadAll` flag is used.
  4000 full 128x128 maps with 5-byte pixel varints and ordinary metadata occupy
  about 328 MiB rather than the old fixed-slot requirement of 750 MiB.
  Pathologically large metadata may still exhaust the byte budget before 4000.
- All file creation, reading, writing and cleanup happen on the map worker.
  Frontends supply their own cache directory. Files are private/exclusive and
  delete-on-close; POSIX unlinks immediately so process termination cleans them.
- No sleep or disk I/O on network callbacks. A worker wakes on a condition
  variable; delivery uses the existing downstream batching timer.
- Stop admitting maps when ordinary application packets are waiting, transport
  statistics are unavailable, send buffer exceeds 32 KiB or resend buffer
  exceeds 96 KiB. One admitted map cannot overtake already queued transport data.

## Frontends, No Render and failure behavior

Windows 1.3.8 and Android 1.5.2-Modules always configure maps as visible.
The old map visibility control is removed from both frontends; their native
configuration entry points also ignore the legacy map-hide parameter. Entity
No Render is separate and cannot pause map demand or delivery. The library
API still supports hidden maps for other integrations and regression tests.

No Render does not disable interception. Hidden maps produce new transparent
images; showing them restores merged originals through the same rate limits.
Actor visibility is independent. Maps are never handled by the actor filter.

Overflow drops new map updates and marks them for reload. The scheduler does
not manufacture map requests or claim to recover missing patches: a new full
image+metadata refresh clears that map's marker; otherwise reconnect/reload is
needed. Missing/corrupt disk records cannot trigger direct forwarding. Session
or dimension resets cancel old-generation work; temporary storage is recycled.

`map_delivery` diagnostics every 5 seconds include intercepted/submitted counts,
pending maps, reserved memory/disk, malformed/overflow/I/O counts, reload markers,
and received packet counts in both directions. `sent` means submitted through
the downstream sender, not proof that Minecraft rendered it. No payloads or
authentication secrets are logged.

## Demand-side pacing (Windows 1.3.7)

`map_info_request` is consumed before normal serverbound forwarding and handled
by a separate bounded plaintext queue. At most 4000 entries / 256 KiB payloads
are retained, including the short in-flight duplicate cache. Byte-identical
requests coalesce for 5 seconds; different client pixel edits retain their
order. Invalid counts/indices, malformed wire headers and overflow are consumed,
never forwarded directly. Oversized pixel requests are rejected before copying.

The downstream timer submits at most 8 requests/s with a 125 ms minimum gap,
one at a time, through the upstream's normal encryption/output lock. Two map IDs
may be awaiting a response at once; these reservations expire after 5 seconds.
Expiry only allows subsequent queued/new demand; it never synthesizes retries.
No Render pauses demand, and showing maps resumes it. A pending image backlog
of 4 also pauses new requests. Ordinary actions bypass this queue. This limits
client-triggered demand, not unsolicited server pushes; response-side storage
and pacing are still mandatory. Backend receipt/rendering are not implied by
the submitted counters.

The first nonempty scheduler state and final session state are also reported,
so a short failure does not disappear between periodic reports. Diagnostics
include request intake, submission, waiting, duplicates, malformed and overflow.

`map-request-queue-smoke --live` uses a local backend deliberately configured to
disconnect above 8 requests/980 ms (4 in the original 1.3.6 regression). The pre-integration relay fails the original test;
the test is a demand-pacing regression, not proof that the real server uses this
limit or that its observed disconnect has this cause. Unit tests exercise
No Render, duplicate expiry, pixel payload fidelity, RAM/entry caps and no burst.

The extended live test also exposed a simultaneous-close deadlock. LLDB showed
the test client's public close joining its RakNet receive worker while that
worker handled a Bedrock disconnect and waited for the public close to return.
Inbound game-level disconnect now uses the receive/transport close path, which
waits only for a competing close's cleanup commit. Named disconnect/kick events
still precede cleanup. A barrier-driven regression in
`live-relay-remote-close-smoke` forces this overlap for both protocol versions.

## Verification and interactive test

The 1.3.6 real-server trial reached exactly 512 maps and stopped requesting more:
3471 requests, 22 duplicates, 2937 overflow, 512 submitted. User confirmed the
world remained responsive. The 1.3.7 request cap is 4000 as explicitly requested;
its increased rate still needs a fresh real-server test. `map-capacity-smoke`
checks 4000 distinct requests, first delivery, hiding and complete restoration
of every pixel of all 4000 maps, and rejection of map/request 4001. It runs both
protocol versions with virtual scheduler time, real worker/file I/O and normal
codec validation. This is capacity/data-integrity coverage, not render-speed proof.

`no-render-smoke` exercises merging, full replacement, hiding/restoring, rapid
toggles, malformed rejection, RAM/disk/index bounds, storage failure, reset,
wire-ID interception, rolling byte/count limits and no catch-up.
`no-render-smoke --live` uses an actual local client/relay/server connection,
mixed batches, all public output paths, malformed maps and a map flood while
ordinary clientbound/serverbound actions continue.

Windows controls: Modules → No Render. Both checkboxes default off. The
scheduler still runs with both off. Logs: `%LOCALAPPDATA%/CPE Relay Windows/relay.log`
and its `.previous` rotation. Real Minecraft/server testing is a separate step;
automated transport tests cannot verify rendering or resolve a remote timeout.

## Remote-close lifetime regression (Windows 1.3.5)

The 1.3.4 interactive test ended with a native access violation in its worker,
not the UI process. `live-relay-remote-close-smoke` reproduced an access violation
on the unpatched UCRT Release core after a backend RakNet close. Unlike the
older client-initiated shutdown tests, it drops the backend first and does not
retain an extra strong reference to the relay's upstream client.

Each relay upstream now installs the network client's existing weak lifetime
provider before starting its workers. Worker leases retain the client until
callback unwinding and final worker bookkeeping finish; session reset still
stops the workers, and the weak provider creates no permanent ownership cycle.
The regression covers transport-level and Bedrock-level disconnects, repeated
joins on the same relay, exactly-once reset and eventual owner expiration, for
1.21.2 and 1.21.100. This fixes a teardown lifetime defect, not a claim that any
preceding remote server disconnect has been resolved.

Scheduler health no longer depends on item/NBT diagnostics. The Windows log
retains critical events ahead of debug tails and uses original timestamps.
Background worker death is reported explicitly and retained in the UI status.
