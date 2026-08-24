#include <bedrock/auth/MsalCachePlugin.hpp>

#include <exception>
#include <stdexcept>
#include <utility>

namespace bedrock {
namespace {

std::shared_ptr<ISerializableTokenCache> requireTokenCache(
    const TokenCacheContextPtr& context,
    const char* member
) {
    if (!context) {
        throw std::runtime_error(
            "Cannot read properties of null (reading 'tokenCache')"
        );
    }
    if (!context->tokenCache.has_value() || !*context->tokenCache) {
        const auto nullish = context->tokenCache.isNull() ||
            (context->tokenCache.has_value() && !*context->tokenCache)
            ? "null"
            : "undefined";
        throw std::runtime_error(
            std::string("Cannot read properties of ") + nullish +
            " (reading '" +
            member + "')"
        );
    }
    return *context->tokenCache;
}

} // namespace

MsalCachePluginRuntime makeMsaTokenManagerCachePlugin(
    AuthCachePtr cache,
    JsRuntimeValue msaClientId,
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue
) {
    return makeMsaTokenManagerCachePlugin(
        std::make_shared<MsaTokenManagerState>(
            MsaTokenManagerState {
                .msaClientId = std::move(msaClientId),
                .cache = std::move(cache)
            }
        ),
        std::move(microtaskQueue)
    );
}

MsalCachePluginRuntime makeMsaTokenManagerCachePlugin(
    std::shared_ptr<MsaTokenManagerState> managerState,
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue
) {
    if (!microtaskQueue) {
        microtaskQueue = JsMicrotaskQueue::create();
    }
    if (!managerState) {
        managerState = std::make_shared<MsaTokenManagerState>();
    }

    MsalCachePluginRuntime runtime;
    runtime.managerState = std::move(managerState);
    runtime.microtaskQueue = std::move(microtaskQueue);

    const auto state = runtime.managerState;
    const auto queue = runtime.microtaskQueue;

    auto beforeCacheAccess = JsRuntimeValue::namedFunction<
        MsalCacheHookSignature
    >("beforeCacheAccess", [state, queue](
        TokenCacheContextPtr context
    ) -> JsPromise<void> {
        try {
            // CallExpression evaluates `cacheContext.tokenCache.deserialize`
            // before it evaluates the argument containing the first await.
            // Retain that receiver across the suspension point.
            auto tokenCache = requireTokenCache(context, "deserialize");
            auto deserialize = tokenCache->resolveDeserializeMethod();
            if (!deserialize) {
                throw std::runtime_error(
                    "cacheContext.tokenCache.deserialize is not a function"
                );
            }

            const auto currentCache = state->cache;
            if (!currentCache) {
                throw std::runtime_error(
                    "Cannot read properties of null (reading 'getCached')"
                );
            }
            if (!currentCache->hasGetCachedMethod()) {
                throw std::runtime_error(
                    "this.cache.getCached is not a function"
                );
            }
            auto getCached = currentCache->requireGetCachedMethod();

            // getCached() itself is invoked before the async function returns.
            auto cached = JsPromise<AuthCacheValue>::fromFuture(
                queue,
                getCached()
            );
            return cached.then(
                [tokenCache = std::move(tokenCache),
                 deserialize = std::move(deserialize)](
                    const AuthCacheValue& value
                ) {
                    // Keep the receiver alive while invoking the exact method
                    // reference captured before the await suspension.
                    (void) tokenCache;
                    deserialize(JsRuntimeJson::stringify(value));
                }
            );
        } catch (...) {
            // An async JavaScript function converts every pre-await throw into
            // a rejected Promise instead of throwing out of the hook call.
            return JsPromise<void>::rejected(
                queue,
                std::current_exception()
            );
        }
    });

    auto afterCacheAccess = JsRuntimeValue::namedFunction<
        MsalCacheHookSignature
    >("afterCacheAccess", [state, queue](
        TokenCacheContextPtr context
    ) -> JsPromise<void> {
        try {
            if (!context) {
                throw std::runtime_error(
                    "Cannot read properties of null (reading "
                    "'cacheHasChanged')"
                );
            }
            if (!context->cacheHasChanged) {
                return JsPromise<void>::resolved(queue);
            }

            // JavaScript resolves this.cache.setCachedPartial before it
            // evaluates JSON.parse(cacheContext.tokenCache.serialize()).
            // Preserve the same cache receiver if serialize() has side effects
            // that replace the manager's cache slot.
            const auto currentCache = state->cache;
            if (!currentCache) {
                throw std::runtime_error(
                    "Cannot read properties of null (reading "
                    "'setCachedPartial')"
                );
            }
            if (!currentCache->hasSetCachedPartialMethod()) {
                throw std::runtime_error(
                    "this.cache.setCachedPartial is not a function"
                );
            }
            auto setCachedPartial =
                currentCache->requireSetCachedPartialMethod();

            auto tokenCache = requireTokenCache(context, "serialize");
            auto serialize = tokenCache->resolveSerializeMethod();
            if (!serialize) {
                throw std::runtime_error(
                    "cacheContext.tokenCache.serialize is not a function"
                );
            }
            auto serialized = serialize();
            auto parsed = JsRuntimeJson::parse(serialized);

            return JsPromise<void>::fromFuture(
                queue,
                setCachedPartial(std::move(parsed))
            ).then([] {});
        } catch (...) {
            return JsPromise<void>::rejected(
                queue,
                std::current_exception()
            );
        }
    });

    runtime.cachePlugin = JsRuntimeValue::object({
        {"beforeCacheAccess", std::move(beforeCacheAccess)},
        {"afterCacheAccess", std::move(afterCacheAccess)}
    });
    return runtime;
}

} // namespace bedrock
