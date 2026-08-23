# Getting Started

This guide is written for someone who wants to download the library, build it once, then write bots next to it.

## 1. Choose A Folder Layout

Recommended layout:

```text
workspace/
  bedrock-protocol-cpp/
  my_bot/
```

`bedrock-protocol-cpp` is the library. `my_bot` is your own project. You normally rebuild `my_bot` while writing bot logic. You rebuild `bedrock-protocol-cpp` only when the library itself changes.

## 2. Build The Library

### Windows

Use MSYS2/MinGW. Native MSVC is not supported yet.

PowerShell:

```powershell
cd C:\path\to\workspace\bedrock-protocol-cpp
.\scripts\build.ps1
```

Debug build:

```powershell
.\scripts\build.ps1 -Config Debug
```

Configure/build without installing:

```powershell
.\scripts\build.ps1 -NoInstall
```

### Linux

Install dependencies and build:

```bash
cd /path/to/workspace/bedrock-protocol-cpp
chmod +x scripts/build.sh
./scripts/build.sh --deps
```

Build again after dependencies are installed:

```bash
./scripts/build.sh
```

Debug build:

```bash
./scripts/build.sh --debug
```

### Termux

Install dependencies and build:

```bash
cd /path/to/workspace/bedrock-protocol-cpp
chmod +x scripts/build.sh
./scripts/build.sh --deps
```

Build again:

```bash
./scripts/build.sh
```

Do not run bots through `stdbuf` on Termux. It can set `LD_PRELOAD` and break Xbox authentication.

## 3. Write A Simple Bot

Create `workspace/my_bot/main.cpp`:

```cpp
#include <bedrock/bedrock.hpp>

#include <iostream>

int main() {
    auto client = bedrock::createClient({
        .host = "localhost",
        .port = 19132,
        .username = "Notch",
        .version = "1.20.40",
        .offline = true,
    });

    client.on("start_game", [](const bedrock::Packet&) {
        std::cout << "Joined world\n";
    });

    client.on("disconnect", [](const bedrock::Packet& packet) {
        std::cout << "Disconnected";
        if (packet.has("reason")) std::cout << " reason=" << packet.get("reason");
        if (packet.has("message")) std::cout << " message=" << packet.get("message");
        std::cout << "\n";
    });

    client.on("packet", [](const bedrock::Packet& packet) {
        std::cout << packet.name << " id=" << packet.id << "\n";
    });

    return client.run();
}
```

For another version, change only this line:

```cpp
.version = "1.21.100",
```

Omit `version` to use the JavaScript package default, `"1.26.0"`, or pass another exact supported release. `"latest"` and `"auto"` are not valid JavaScript `Versions` keys and are rejected.

## 4. Add CMake

Create `workspace/my_bot/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_bedrock_bot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(BedrockProtocol REQUIRED)

add_executable(my_bedrock_bot main.cpp)
target_link_libraries(my_bedrock_bot PRIVATE BedrockProtocol::bedrock_protocol)
```

## 5. Build Your Bot

### Windows

```powershell
cd C:\path\to\workspace\my_bot
$prefix = "C:\path\to\workspace\bedrock-protocol-cpp\install"
cmake -S . -B build -DCMAKE_PREFIX_PATH="$prefix"
cmake --build build
```

### Linux / Termux

```bash
cd /path/to/workspace/my_bot
prefix=/path/to/workspace/bedrock-protocol-cpp/install
cmake -S . -B build -DCMAKE_PREFIX_PATH="$prefix"
cmake --build build -j"$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN)"
```

## 6. Run Your Bot

### Windows

```powershell
.\build\my_bedrock_bot.exe
```

### Linux / Termux

```bash
./build/my_bedrock_bot
```

No host, port, username, or version arguments are passed in the terminal. They live in your C++ bot code.

## 7. Online And Offline Auth

Local offline server:

```cpp
.offline = true,
```

Public online server:

```cpp
.offline = false,
.interactiveAuth = true,
```

Online mode uses Xbox Live authentication. If the profile cache is missing, the bot prints a Microsoft device-code login URL/code, waits for you to sign in, saves the hidden auth cache, and then generates a fresh login packet for the selected version/server. Public servers usually reject offline/self-signed clients.

## 8. Common Beginner Fixes

If CMake cannot find the library, check `CMAKE_PREFIX_PATH`. It must point to `bedrock-protocol-cpp/install`.

If VS Code cannot find `<bedrock/bedrock.hpp>`, open the `bedrock-protocol-cpp` folder itself, not its parent folder, and run `CMake: Configure`.
