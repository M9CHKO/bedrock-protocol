#pragma once

#include <bedrock/auth/AuthCache.hpp>
#include <bedrock/auth/BedrockTokenManagerCommon.hpp>

#include <memory>
#include <string_view>

namespace bedrock {

struct MinecraftBedrockTokenManagerState;

// Native port of prismarine-auth's MinecraftBedrockTokenManager.js. It owns
// only the multiplayer.minecraft.net Bedrock chain exchange and cache logic.
class MinecraftBedrockTokenManager {
private:
    std::shared_ptr<MinecraftBedrockTokenManagerState> state_;

public:
    inline static constexpr std::string_view AuthenticationEndpoint =
        "https://multiplayer.minecraft.net/authentication";

    explicit MinecraftBedrockTokenManager(AuthCachePtr cacheValue);
    MinecraftBedrockTokenManager(
        AuthCachePtr cacheValue,
        BedrockTokenManagerDependencies dependencies
    );

    MinecraftBedrockTokenManager(
        const MinecraftBedrockTokenManager&
    ) = delete;
    MinecraftBedrockTokenManager& operator=(
        const MinecraftBedrockTokenManager&
    ) = delete;

    AuthCachePtr& cache;
    JsRuntimeValue& forceRefresh;

    JsPromise<JsRuntimeValue> getCachedAccessToken();
    JsPromise<void> setCachedAccessToken(JsRuntimeValue data);
    JsPromise<JsRuntimeValue> verifyTokens();
    JsPromise<JsRuntimeValue> getAccessToken(
        JsRuntimeValue clientPublicKey,
        JsRuntimeValue xsts
    );

    std::shared_ptr<JsMicrotaskQueue> microtaskQueue() const noexcept;
    XboxTokenHttpClientPtr httpClient() const noexcept;
};

using BedrockTokenManager = MinecraftBedrockTokenManager;

} // namespace bedrock
