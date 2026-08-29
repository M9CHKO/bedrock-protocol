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
