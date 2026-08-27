#pragma once

#include <bedrock/auth/Authflow.hpp>
#include <bedrock/auth/MicrosoftAuthFlow.hpp>
#include <bedrock/auth/XboxLiveAuth.hpp>
#include <bedrock/client/BedrockNetworkClient.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace bedrock {

// Inputs used by bedrock-protocol's `new prismarine-auth.Authflow(...)`
// path. This is deliberately Bedrock-only: no Java token manager or `mca`
// cache is constructed by this native factory.
struct NativeBedrockAuthflowOptions {
    std::string username;
    ProfilesFolderOption profilesFolder;
    JsProperty<std::string> authTitle;
    std::string deviceType;
    std::string flow;
    bool forceRefresh = false;
    MsalConfigPtr msalConfig;
    std::string password;
    std::function<void(const XboxDeviceCodeInfo&)> onMsaCode;
};

// Keep the complete manager graph available to BedrockNetworkClient's
// introspection/lifetime fields while also exposing the language-adapted
// Authflow used by the Realms preflight and supplied-authflow login path.
struct NativeBedrockAuthflowRuntime {
    AuthflowPtr authflow;
    MsalConfigPtr effectiveMsalConfig;
    std::shared_ptr<LiveTokenManager> live;
    std::shared_ptr<MsaTokenManager> msa;
    std::shared_ptr<XboxTokenManager> xbox;
    std::shared_ptr<MinecraftBedrockTokenManager> bedrock;
    std::shared_ptr<MinecraftBedrockServicesTokenManager> bedrockServices;
    std::shared_ptr<PlayfabTokenManager> playfab;
    std::shared_ptr<MicrosoftAuthFlow> microsoft;
    std::optional<XboxProofKey> xboxProofKey;
    std::unordered_map<std::string, AuthCachePtr> caches;
};

// auth.js's synchronous option normalizer. Callers publish these mutations
// before constructing the flow so constructor failures remain observable in
// the same order as the Node implementation.
void validateNativeBedrockAuthflowOptions(
    NativeBedrockAuthflowOptions& options
);

void validateNativeBedrockAuthflowPresence(
    const NativeBedrockAuthflowOptions& options
);

std::filesystem::path defaultBedrockProfilesFolder();

// MicrosoftAuthFlow's private cache-path selection. A caller can publish the
// returned path before the remaining constructor work, preserving the native
// network client's existing failure-order semantics.
std::filesystem::path initializeNativeBedrockAuthCacheRoot(
    const ProfilesFolderOption& profilesFolder
);

NativeBedrockAuthflowRuntime createNativeBedrockAuthflow(
    const NativeBedrockAuthflowOptions& options,
    const std::filesystem::path& effectiveCacheRoot
);

} // namespace bedrock
