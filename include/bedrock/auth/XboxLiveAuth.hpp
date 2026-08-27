#pragma once

#include <bedrock/JsValue.hpp>
#include <bedrock/Options.hpp>
#include <bedrock/auth/AuthCache.hpp>
#include <bedrock/auth/BedrockAuthJwt.hpp>
#include <bedrock/auth/JsRuntimeValue.hpp>
#include <bedrock/auth/LiveTokenManager.hpp>
#include <bedrock/auth/MsaTokenManager.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bedrock {

// @azure/msal-node accepts an open JavaScript configuration object and
// prismarine-auth preserves the caller's object identity while overwriting its
// `cache` member. This auth-only JS graph keeps property order, callable
// values, opaque callback identity, and nested object reference identity.
using MsalConfig = JsRuntimeValue;
using MsalConfigPtr = std::shared_ptr<MsalConfig>;

inline MsalConfigPtr makeMsalConfig(
    std::string clientId,
    std::string authority = "https://login.microsoftonline.com/consumers"
) {
    return std::make_shared<MsalConfig>(MsalConfig::object({
        {"auth", MsalConfig::object({
            {"clientId", MsalConfig::string(std::move(clientId))},
            {"authority", MsalConfig::string(std::move(authority))}
        })}
    }));
}

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
    JsProperty<std::string> authTitle;
    std::string deviceType;
    std::string flow;
    bool forceRefresh = false;
    MsalConfigPtr msalConfig;
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

struct PrismarineAuthFlowRuntime {
    MsalConfigPtr effectiveMsalConfig;
    std::shared_ptr<LiveTokenManager> live;
    std::shared_ptr<MsaTokenManager> msa;
    bool doTitleAuth = false;
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

    // Runs the exact flow-specific constructor segment after cache selection.
    // For a truthy supplied msalConfig the same object is returned and its
    // `cache` property is overwritten. A falsy/omitted config yields the
    // private structuredClone(defaultConfig) used by prismarine-auth.
    static MsalConfigPtr initializePrismarineAuthFlow(
        const XboxLiveAuthFlowOptions& options,
        const MsalConfigPtr& msalConfig
    );

    static MsalConfigPtr initializePrismarineAuthFlow(
        const XboxLiveAuthFlowOptions& options,
        const MsalConfigPtr& msalConfig,
        AuthCachePtr msaCache
    );

    static PrismarineAuthFlowRuntime initializePrismarineAuthFlowRuntime(
        const XboxLiveAuthFlowOptions& options,
        const MsalConfigPtr& msalConfig,
        AuthCachePtr msaCache,
        MsalPublicClientApplicationFactory applicationFactory = {},
        std::shared_ptr<JsMicrotaskQueue> microtaskQueue = {},
        MsaTokenManagerObservers observers = {},
        LiveTokenManagerDependencies liveDependencies = {}
    );

    static bool isTruthyMsalConfig(const MsalConfigPtr& msalConfig);

    static XboxLiveLoginPacket makeLoginPacket(XboxLiveAuthOptions options);

    // BedrockNetworkClient has already executed the synchronous Authflow
    // constructor before startQueue(). This entry point consumes that private
    // effective MSAL config without mutating a supplied config a second time.
    static XboxLiveLoginPacket makeLoginPacketFromPreparedFlow(
        XboxLiveAuthOptions options,
        MsalConfigPtr effectiveMsalConfig
    );

    // Completes auth.js's supplied options.authflow path. The caller already
    // owns the KeyExchange P-384 key and the Promise result, so this overload
    // must not construct/validate the built-in Authflow or touch its cache.
    static XboxLiveLoginPacket makeLoginPacketFromChains(
        XboxLiveAuthOptions options,
        BedrockClientKeyPair keyPair,
        std::vector<std::string> chains
    );

    static BedrockClientKeyPair loadOrCreateProfileKeyPair(
        const std::string& profileName,
        const std::filesystem::path& cacheRoot = {}
    );
};

} // namespace bedrock
