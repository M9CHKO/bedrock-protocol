#pragma once

#include <bedrock/auth/AuthCache.hpp>
#include <bedrock/auth/BedrockTokenManagerCommon.hpp>

#include <memory>
#include <string_view>

namespace bedrock {

struct PlayfabTokenManagerState;

// Native port of prismarine-auth's PlayfabTokenManager.js for Minecraft
// Bedrock title 20CA2.
class PlayfabTokenManager {
private:
    std::shared_ptr<PlayfabTokenManagerState> state_;

public:
    inline static constexpr std::string_view LoginWithXboxEndpoint =
        "https://20ca2.playfabapi.com/Client/LoginWithXbox";

    explicit PlayfabTokenManager(AuthCachePtr cacheValue);
    PlayfabTokenManager(
        AuthCachePtr cacheValue,
        BedrockTokenManagerDependencies dependencies
    );

    PlayfabTokenManager(const PlayfabTokenManager&) = delete;
    PlayfabTokenManager& operator=(const PlayfabTokenManager&) = delete;

    AuthCachePtr& cache;

    JsPromise<void> setCachedAccessToken(JsRuntimeValue data);
    JsPromise<JsRuntimeValue> getCachedAccessToken();
    JsPromise<JsRuntimeValue> getAccessToken(JsRuntimeValue xsts);

    std::shared_ptr<JsMicrotaskQueue> microtaskQueue() const noexcept;
    XboxTokenHttpClientPtr httpClient() const noexcept;
};

} // namespace bedrock
