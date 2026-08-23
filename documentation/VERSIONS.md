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

The default is the JavaScript package's exact `CURRENT_VERSION`, `"1.26.0"`. Pass another exact key from the table below when needed. Values such as `"latest"`, `"auto"`, and an empty string are rejected, matching `Options.validateOptions()`.

## Version Notes

| Version range | Important protocol behavior |
|---|---|
| `1.20.40` to `1.20.50` | Uses the older compression batch shape. The client must not write the newer compressor id byte in compressed batches. |
| `1.20.61` and newer | Uses the newer compression handling negotiated through `NetworkSettings`. |
| Older than `1.21.90` | Uses the legacy identity chain shape. |
| `1.21.90` and newer | Uses the newer auth/client chain shape used by modern Bedrock clients. |
| `1.26.x` | Bundled data is present and roundtrip-tested for packet encode/decode tables. |

Server MOTD protocol numbers are not always reliable. Some servers advertise an old protocol while accepting newer clients. Pick the version you want the bot to speak and let the client use the matching bundled protocol data.

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
[ROUNDTRIP] checkedVersions=27 failures=0
```
