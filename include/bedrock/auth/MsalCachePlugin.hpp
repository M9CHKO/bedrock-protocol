#pragma once

#include <bedrock/JsValue.hpp>
#include <bedrock/auth/AuthCache.hpp>
#include <bedrock/auth/JsPromise.hpp>
#include <bedrock/auth/JsRuntimeValue.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bedrock {

// Native surface of @azure/msal-common's ISerializableTokenCache used by
// prismarine-auth's MsaTokenManager cache plugin.  `std::nullopt` represents
// JavaScript undefined from a top-level JSON.stringify call.
class ISerializableTokenCache {
public:
    using DeserializeMethod = std::function<
        void(const std::optional<std::string>& serializedCache)
    >;
    using SerializeMethod = std::function<std::string()>;

    virtual ~ISerializableTokenCache() = default;

    virtual void deserialize(
        const std::optional<std::string>& serializedCache
    ) = 0;
    virtual std::string serialize() = 0;

    // Property lookup and method invocation are separated by `await` in the
    // before hook. A dynamic implementation may override these resolvers to
    // return the exact callable currently stored in its JS-facing property.
    virtual DeserializeMethod resolveDeserializeMethod() {
        return [this](const std::optional<std::string>& value) {
            deserialize(value);
        };
    }

    virtual SerializeMethod resolveSerializeMethod() {
        return [this] { return serialize(); };
    }
};

// Same two public properties exposed by @azure/msal-common's
// TokenCacheContext getters.
struct TokenCacheContext {
    JsProperty<std::shared_ptr<ISerializableTokenCache>> tokenCache;
    bool cacheHasChanged = false;
};

using TokenCacheContextPtr = std::shared_ptr<TokenCacheContext>;
using MsalCacheHookSignature = JsPromise<void>(TokenCacheContextPtr);

// The hooks close over the manager object in JavaScript and therefore read
// `this.cache` anew for every invocation.  Keeping that slot in shared state
// reproduces the same late lookup and lets a later cache assignment be seen.
struct MsaTokenManagerState {
    JsRuntimeValue msaClientId = JsRuntimeValue::undefined();
    AuthCachePtr cache;
};

struct MsalCachePluginRuntime {
    std::shared_ptr<MsaTokenManagerState> managerState;
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue;
    JsRuntimeValue cachePlugin = JsRuntimeValue::object();
};

// Builds the exact `{ beforeCacheAccess, afterCacheAccess }` object installed
// by prismarine-auth/src/TokenManagers/MsaTokenManager.js.
MsalCachePluginRuntime makeMsaTokenManagerCachePlugin(
    AuthCachePtr cache,
    JsRuntimeValue msaClientId = JsRuntimeValue::undefined(),
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue = {}
);

MsalCachePluginRuntime makeMsaTokenManagerCachePlugin(
    std::shared_ptr<MsaTokenManagerState> managerState,
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue = {}
);

} // namespace bedrock
