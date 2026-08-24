#pragma once

#include <bedrock/auth/AuthCache.hpp>
#include <bedrock/auth/JsPromise.hpp>
#include <bedrock/auth/JsRuntimeValue.hpp>
#include <bedrock/auth/MsalCachePlugin.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bedrock {

// Narrow @azure/msal-node PublicClientApplication surface used by
// prismarine-auth's MsaTokenManager. The request remains a live JavaScript
// value graph so object identity, key order, callbacks, and scopes identity
// survive the language boundary.
class IMsalPublicClientApplication {
public:
    virtual ~IMsalPublicClientApplication() = default;

    virtual JsPromise<JsRuntimeValue> acquireTokenByRefreshToken(
        JsRuntimeValue request
    ) = 0;

    virtual JsPromise<JsRuntimeValue> acquireTokenByDeviceCode(
        JsRuntimeValue request
    ) = 0;
};

using MsalPublicClientApplicationPtr =
    std::shared_ptr<IMsalPublicClientApplication>;
using MsalPublicClientApplicationFactory = std::function<
    MsalPublicClientApplicationPtr(
        const std::shared_ptr<JsRuntimeValue>& config
    )
>;

struct MsaTokenManagerObservers {
    using DebugMethod = std::function<void(
        const std::string& label,
        const std::vector<JsRuntimeValue>& arguments
    )>;
    using WarnMethod = std::function<void(
        const std::string& message,
        std::exception_ptr error
    )>;
    using ConsoleDebugMethod = std::function<void(
        const std::optional<std::string>& serialized
    )>;

    DebugMethod debug;
    WarnMethod warn;
    ConsoleDebugMethod consoleDebug;
    std::function<double()> dateNowMilliseconds;
    std::function<void(std::exception_ptr)> unhandledRejection;
};

struct MsaTokenManagerRuntimeState {
    JsRuntimeValue scopes = JsRuntimeValue::array();
    JsRuntimeValue forceRefresh = JsRuntimeValue::undefined();
    JsRuntimeValue msaCache = JsRuntimeValue::undefined();
    std::shared_ptr<JsRuntimeValue> msalConfig;
    MsalPublicClientApplicationPtr msalApp;
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue;
    MsaTokenManagerObservers observers;
    JsRuntimeValue cachePlugin = JsRuntimeValue::object();
};

// Direct native port of prismarine-auth/src/TokenManagers/MsaTokenManager.js.
// The first constructor intentionally has exactly its three source arguments.
class MsaTokenManager {
private:
    std::shared_ptr<MsaTokenManagerState> managerState_;
    std::shared_ptr<MsaTokenManagerRuntimeState> runtimeState_;

public:
    MsaTokenManager(
        std::shared_ptr<JsRuntimeValue> msalConfigValue,
        JsRuntimeValue scopesValue,
        AuthCachePtr cacheValue
    );

    MsaTokenManager(
        std::shared_ptr<JsRuntimeValue> msalConfigValue,
        JsRuntimeValue scopesValue,
        AuthCachePtr cacheValue,
        MsalPublicClientApplicationFactory applicationFactory,
        std::shared_ptr<JsMicrotaskQueue> microtaskQueue,
        MsaTokenManagerObservers observers = {}
    );

    MsaTokenManager(const MsaTokenManager&) = delete;
    MsaTokenManager& operator=(const MsaTokenManager&) = delete;
    MsaTokenManager(MsaTokenManager&&) = delete;
    MsaTokenManager& operator=(MsaTokenManager&&) = delete;

    // JavaScript-visible instance properties. References are bound to shared
    // runtime slots so arrow cache hooks observe later assignments to cache.
    JsRuntimeValue& msaClientId;
    JsRuntimeValue& scopes;
    AuthCachePtr& cache;
    JsRuntimeValue& forceRefresh;
    JsRuntimeValue& msaCache;
    MsalPublicClientApplicationPtr& msalApp;
    std::shared_ptr<JsRuntimeValue>& msalConfig;

    JsRuntimeValue getUsers();
    JsPromise<JsRuntimeValue> getAccessToken();
    JsPromise<JsRuntimeValue> getRefreshToken();
    JsPromise<JsRuntimeValue> refreshTokens();
    JsPromise<JsRuntimeValue> verifyTokens();
    JsPromise<JsRuntimeValue> authDeviceCode(
        std::function<void(const JsRuntimeValue& response)> dataCallback
    );

    const JsRuntimeValue& installedCachePlugin() const noexcept;
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue() const noexcept;
    std::shared_ptr<MsaTokenManagerState> sharedManagerState() const noexcept;
};

} // namespace bedrock
