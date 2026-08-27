#pragma once

#include <bedrock/auth/AuthCache.hpp>
#include <bedrock/auth/BedrockTokenManagerCommon.hpp>

#include <memory>
#include <string_view>

namespace bedrock {

struct MinecraftBedrockServicesTokenManagerState;

// Native port of MinecraftBedrockServicesManager.js. This is the Bedrock
// franchise-session MCToken flow used by NetherNet and related services.
class MinecraftBedrockServicesTokenManager {
private:
    std::shared_ptr<MinecraftBedrockServicesTokenManagerState> state_;

public:
    inline static constexpr std::string_view SessionStartEndpoint =
        "https://authorization.franchise.minecraft-services.net/"
        "api/v1.0/session/start";

    explicit MinecraftBedrockServicesTokenManager(AuthCachePtr cacheValue);
    MinecraftBedrockServicesTokenManager(
        AuthCachePtr cacheValue,
        BedrockTokenManagerDependencies dependencies
    );

    MinecraftBedrockServicesTokenManager(
        const MinecraftBedrockServicesTokenManager&
    ) = delete;
    MinecraftBedrockServicesTokenManager& operator=(
        const MinecraftBedrockServicesTokenManager&
    ) = delete;

    AuthCachePtr& cache;

    JsPromise<JsRuntimeValue> getCachedAccessToken();
    JsPromise<void> setCachedToken(JsRuntimeValue data);
    JsPromise<JsRuntimeValue> getAccessToken(
        JsRuntimeValue sessionTicket,
        JsRuntimeValue options = JsRuntimeValue::object()
    );

    std::shared_ptr<JsMicrotaskQueue> microtaskQueue() const noexcept;
    XboxTokenHttpClientPtr httpClient() const noexcept;
};

} // namespace bedrock
