# Vendored RakNet transport

This directory contains the RakNet source distributed with
`raknet-native@1.2.3`, the native transport used by the JavaScript
`bedrock-protocol` package. The original BSD license is retained in
`LICENSE`.

The C++ library builds these sources directly so both implementations use the
same `ReliabilityLayer` for ACK/NACK processing, retransmission, congestion
control, split-packet reassembly, duplicate suppression and ordered delivery.

Local compatibility changes are intentionally small:

- the upstream `raknet-native` protocol-version/older-client patch is retained;
- `SetMyGUID` is exposed before `Startup()` so the existing C++ public
  `clientGuid` and `serverGuid` options remain deterministic;
- `LinuxStrings.cpp` includes the POSIX declaration for `strcasecmp`;
- two legacy congestion-control parameter names that collide with Cygwin
  ctype macros are renamed without changing their behavior.

The Android 1.3.0 audit restores `CCRakNetSlidingWindow` receive-sequence
behavior from the first Android commit (`f5867c5`): no special early-session
resynchronization after an arbitrary large datagram-number jump. The normal
ACK/NACK, ordering, reassembly and duplicate rules remain in force.
`raknet-server-smoke` verifies both early and established-session jump
rejection and subsequent valid fragmented delivery. The lifecycle smoke test
also exercises ten 600 KiB fragmented reconnects with missing/out-of-order
fragments and duplicate retransmissions.

Duplicate-window and older-ordered rejection notifications are available to
attached reliability plugins without enabling all verbose notifications. This
does not change delivery decisions. `RakNetServer` stores only bounded metadata
in a fixed 64-entry diagnostic ring, never payloads or an unbounded event queue.
