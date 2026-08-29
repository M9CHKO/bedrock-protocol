# Updating The Library

Use this file when you change the library itself, update protocol data, or prepare the repository for publishing.

## Rebuild After Code Changes

Windows:

```powershell
cd C:\path\to\bedrock-protocol-cpp
.\scripts\build.ps1
```

Linux / Termux:

```bash
cd /path/to/bedrock-protocol-cpp
./scripts/build.sh
```

Run the roundtrip check:

```bash
./build/protocol-roundtrip
```

## Rebuild Only Your Bot

If you edited only your bot project, do not rebuild the library.

```bash
cmake --build build
```

Your bot links to the already installed library through `CMAKE_PREFIX_PATH`.

## Update Bundled Minecraft Data

The library keeps protocol data in:

```text
data/minecraft-data/
data/generated/
```

Update flow:

```bash
./scripts/vendor_minecraft_data.sh
python scripts/generate_protocol_artifacts.py
python scripts/generate_protocol_artifacts.py --check
./scripts/build.sh
ctest --test-dir build --output-on-failure
```

The unified generator reads each registered release through
`dataPaths.json`, including schema aliases, and deterministically rebuilds the
packet registry, version-aware C++ protocol types, packet-schema TSV files,
and protocol-types TSV files. The same operations are available as CMake
targets:

```bash
cmake --build build --target generate-protocol-artifacts
cmake --build build --target check-protocol-artifacts
```

`generated-protocol-consistency` also runs the temporary-directory comparison
under CTest. CI must fail when a checked-in generated artifact is stale.

If packet layouts changed, inspect the affected version files:

```text
data/minecraft-data/bedrock/<version>/protocol.json
data/minecraft-data/bedrock/<version>/proto.yml
data/minecraft-data/bedrock/<version>/types.yml
```

## Git Publishing Checklist

Before publishing:

- Keep `build/`, `install/`, auth caches, logs, packet dumps, and zips out of git.
- Keep source, headers, CMake files, scripts, docs, and required data in git.
- Run the build script once from a clean checkout.
- Run `python scripts/generate_protocol_artifacts.py --check`.
- Run `ctest --test-dir build --output-on-failure`.
- Test at least one offline local server and one online server if you changed auth/login code.

The repository `.gitignore` already excludes local build output and runtime artifacts.
