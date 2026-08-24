#pragma once

#include <bedrock/auth/JsRuntimeValue.hpp>

#include <exception>
#include <future>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace bedrock {

// The object passed by prismarine-auth to a supplied CacheFactory.
struct AuthCacheFactoryOptions {
    std::string cacheName;
    std::string username;
};

using AuthCacheValue = JsRuntimeValue;
using AuthCacheValueFuture = std::future<AuthCacheValue>;
using AuthCacheVoidFuture = std::future<void>;

// Promise helpers for supplied cache implementations. std::promise keeps a
// ready or rejected result without introducing an unrelated worker thread.
template <typename T>
std::future<T> makeReadyAuthCacheFuture(T value) {
    std::promise<T> promise;
    auto future = promise.get_future();
    promise.set_value(std::move(value));
    return future;
}

inline AuthCacheVoidFuture makeReadyAuthCacheFuture() {
    std::promise<void> promise;
    auto future = promise.get_future();
    promise.set_value();
    return future;
}

template <typename T>
std::future<T> makeRejectedAuthCacheFuture(std::exception_ptr error) {
    std::promise<T> promise;
    auto future = promise.get_future();
    promise.set_exception(std::move(error));
    return future;
}

template <typename T>
std::future<T> makeRejectedAuthCacheFuture(std::string message) {
    return makeRejectedAuthCacheFuture<T>(
        std::make_exception_ptr(std::runtime_error(std::move(message)))
    );
}

// Runtime Cache object accepted from a caller-provided CacheFactory. Calling a
// method is synchronous, just like invoking a JavaScript function; the exact
// future returned by the callback is the native counterpart of its Promise.
class AuthCache {
public:
    using GetCachedMethod = std::function<AuthCacheValueFuture()>;
    using SetCachedPartialMethod = std::function<
        AuthCacheVoidFuture(AuthCacheValue value)
    >;
    using SetCachedMethod = std::function<
        AuthCacheVoidFuture(AuthCacheValue value)
    >;
    // FileCache.reset() is declared Promise<void> in prismarine-auth's d.ts,
    // but its async runtime returns the newly written `{}` object.
    using ResetMethod = std::function<AuthCacheValueFuture()>;

    AuthCache() = default;
    virtual ~AuthCache() = default;

    explicit AuthCache(
        GetCachedMethod getCached,
        SetCachedPartialMethod setCachedPartial = {},
        ResetMethod reset = {},
        SetCachedMethod setCached = {}
    ) : getCached_(std::move(getCached)),
        setCachedPartial_(std::move(setCachedPartial)),
        reset_(std::move(reset)),
        setCached_(std::move(setCached)) {}

    AuthCacheValueFuture getCached() {
        return requireGetCachedMethod()();
    }

    AuthCacheVoidFuture setCachedPartial(AuthCacheValue value) {
        return requireSetCachedPartialMethod()(std::move(value));
    }

    AuthCacheVoidFuture setCached(AuthCacheValue value) {
        if (!setCached_) {
            throw std::runtime_error(
                "cache.setCached is not a function"
            );
        }
        return setCached_(std::move(value));
    }

    AuthCacheValueFuture reset() {
        if (!reset_) {
            throw std::runtime_error(
                "cache.reset is not a function"
            );
        }
        return reset_();
    }

    void setGetCachedMethod(GetCachedMethod method) {
        getCached_ = std::move(method);
    }

    void setSetCachedPartialMethod(SetCachedPartialMethod method) {
        setCachedPartial_ = std::move(method);
    }

    void setSetCachedMethod(SetCachedMethod method) {
        setCached_ = std::move(method);
    }

    void setResetMethod(ResetMethod method) {
        reset_ = std::move(method);
    }

    bool hasGetCachedMethod() const noexcept {
        return static_cast<bool>(getCached_);
    }

    bool hasSetCachedPartialMethod() const noexcept {
        return static_cast<bool>(setCachedPartial_);
    }

    bool hasSetCachedMethod() const noexcept {
        return static_cast<bool>(setCached_);
    }

    bool hasResetMethod() const noexcept {
        return static_cast<bool>(reset_);
    }

    // JavaScript resolves a method reference before evaluating its argument
    // list. Returning a copy lets callers retain that exact callable while an
    // argument expression performs arbitrary side effects on the cache object.
    GetCachedMethod requireGetCachedMethod() const {
        if (!getCached_) {
            throw std::runtime_error(
                "cache.getCached is not a function"
            );
        }
        return getCached_;
    }

    SetCachedPartialMethod requireSetCachedPartialMethod() const {
        if (!setCachedPartial_) {
            throw std::runtime_error(
                "cache.setCachedPartial is not a function"
            );
        }
        return setCachedPartial_;
    }

private:
    GetCachedMethod getCached_;
    SetCachedPartialMethod setCachedPartial_;
    ResetMethod reset_;
    SetCachedMethod setCached_;
};

using AuthCachePtr = std::shared_ptr<AuthCache>;
using AuthCacheWeakPtr = std::weak_ptr<AuthCache>;
using AuthCacheFactory = std::function<
    AuthCachePtr(AuthCacheFactoryOptions options)
>;
using AuthCacheFactoryPtr = std::shared_ptr<AuthCacheFactory>;

} // namespace bedrock
