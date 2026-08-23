#pragma once

#include <bedrock/Options.hpp>
#include <bedrock/auth/BedrockAuthJwt.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bedrock {

// Exact prismarine-auth/src/common/Titles.js surface.  Static members support
// Titles::MinecraftNintendoSwitch while the root `title` object below mirrors
// bedrock-protocol's exported `title.MinecraftNintendoSwitch` API.
struct Titles {
    inline static constexpr std::string_view MinecraftNintendoSwitch = "00000000441cc96b";
    inline static constexpr std::string_view MinecraftPlaystation = "000000004827c78e";
    inline static constexpr std::string_view MinecraftAndroid = "0000000048183522";
    inline static constexpr std::string_view MinecraftJava = "00000000402b5328";
    inline static constexpr std::string_view MinecraftIOS = "000000004c17c01a";
    inline static constexpr std::string_view XboxAppIOS = "000000004c12ae6f";
    inline static constexpr std::string_view XboxGamepassIOS = "000000004c20a908";
};

inline constexpr Titles title {};

struct XboxDeviceCodeInfo {
    std::string verificationUri;
    std::string userCode;
    std::string message;
};

struct XboxLiveAuthOptions {
    std::string profileName = "Bot";
    std::string version = std::string(CURRENT_VERSION);
    uint32_t protocolVersion = 0;
    std::string serverAddress;
    bool offline = false;
    bool interactiveAuth = true;
    std::optional<std::string> authTitle;
    std::string deviceType;
    std::string flow;
    // Deprecated compatibility alias. New code should use authTitle.
    std::string xboxClientId;
    std::filesystem::path cacheRoot;
    std::vector<std::filesystem::path> minecraftDataRoots;
    std::string clientDataJson;
    std::function<void(const XboxDeviceCodeInfo&)> onMsaCode;
    std::function<void(const XboxDeviceCodeInfo&)> onDeviceCode;
    std::function<void(const std::string&)> onLog;
};

struct XboxLiveAuthFlowOptions {
    std::string authTitle;
    std::string deviceType;
    std::string flow;
};

struct XboxLiveLoginPacket {
    std::vector<uint8_t> loginPacket;
    BedrockClientKeyPair keyPair;
    std::string identity;
    std::string displayName;
    std::string xuid;
    bool online = false;
};

class XboxLiveAuth {
public:
    static XboxLiveAuthFlowOptions resolveFlowOptions(
        const XboxLiveAuthOptions& options
    );

    // MicrosoftAuthFlow's constructor first rejects a missing/falsy `flow`,
    // before it initializes the cache.  The remaining flow checks happen
    // after cache initialization.  Keep the phases separate so callers can
    // preserve that observable ordering (notably profilesFolder=true's
    // caught cache-path failure).
    static void validatePrismarineAuthFlowPresence(
        const XboxLiveAuthFlowOptions& options
    );
    static void validatePrismarineAuthFlow(
        const XboxLiveAuthFlowOptions& options
    );

    static XboxLiveLoginPacket makeLoginPacket(XboxLiveAuthOptions options);

    static BedrockClientKeyPair loadOrCreateProfileKeyPair(
        const std::string& profileName,
        const std::filesystem::path& cacheRoot = {}
    );
};

} // namespace bedrock
