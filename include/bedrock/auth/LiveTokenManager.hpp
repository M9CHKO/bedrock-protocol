#pragma once

#include <bedrock/auth/AuthCache.hpp>
#include <bedrock/auth/JsPromise.hpp>
#include <bedrock/auth/JsRuntimeValue.hpp>
#include <bedrock/auth/XboxTokenManager.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bedrock {

struct LiveTokenManagerObservers {
    using DebugMethod = std::function<void(
        const std::string& label,
        const std::vector<JsRuntimeValue>& arguments
    )>;

    DebugMethod debug;
    std::function<void(const std::string&, std::exception_ptr)> warn;
    std::function<void(const std::string&)> consoleWarnText;
    std::function<void(std::exception_ptr)> consoleDebug;
    std::function<void(std::exception_ptr)> unhandledRejection;
};

struct LiveTokenManagerDependencies {
    XboxTokenHttpClientPtr httpClient;
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue;
    std::function<double()> dateNowMilliseconds;

    // Native setTimeout seam. The argument is the exact JavaScript delay
    // expression before Node applies its timer clamping rules.
    std::function<JsPromise<void>(double milliseconds)> delay;
    LiveTokenManagerObservers observers;
};

struct LiveTokenManagerState;

// Direct native port of prismarine-auth 2.7.0
// src/TokenManagers/LiveTokenManager.js. This is the Microsoft Live OAuth
// branch used by Bedrock's `live` and `sisu` authentication flows; no Java
// token-manager behavior is included here.
class LiveTokenManager {
private:
    std::shared_ptr<LiveTokenManagerState> state_;

public:
    inline static constexpr std::string_view LiveDeviceCodeRequest =
        "https://login.live.com/oauth20_connect.srf";
    inline static constexpr std::string_view LiveTokenRequest =
        "https://login.live.com/oauth20_token.srf";

    LiveTokenManager(
        JsRuntimeValue clientIdValue,
        JsRuntimeValue scopesValue,
        AuthCachePtr cacheValue
    );

    // C++ testing/integration extension. The production constructor above
    // installs the same fetch transport, clock, queue, and timer behavior.
    LiveTokenManager(
        JsRuntimeValue clientIdValue,
        JsRuntimeValue scopesValue,
        AuthCachePtr cacheValue,
        LiveTokenManagerDependencies dependencies
    );

    LiveTokenManager(const LiveTokenManager&) = delete;
    LiveTokenManager& operator=(const LiveTokenManager&) = delete;
    LiveTokenManager(LiveTokenManager&&) = delete;
    LiveTokenManager& operator=(LiveTokenManager&&) = delete;

    // JavaScript-visible instance fields. They remain live slots so mutations
    // made between awaits are observed by subsequent request construction.
    JsRuntimeValue& clientId;
    JsRuntimeValue& scopes;
    AuthCachePtr& cache;
    JsRuntimeValue& forceRefresh;
    JsRuntimeValue& polling;

    JsPromise<JsRuntimeValue> verifyTokens();
    JsPromise<JsRuntimeValue> refreshTokens();
    JsPromise<JsRuntimeValue> getAccessToken();
    JsPromise<JsRuntimeValue> getRefreshToken();
    JsPromise<void> updateCache(JsRuntimeValue data);
    JsPromise<JsRuntimeValue> authDeviceCode(
        std::function<void(JsRuntimeValue response)> deviceCodeCallback
    );

    std::shared_ptr<JsMicrotaskQueue> microtaskQueue() const noexcept;
    XboxTokenHttpClientPtr httpClient() const noexcept;
};

} // namespace bedrock
