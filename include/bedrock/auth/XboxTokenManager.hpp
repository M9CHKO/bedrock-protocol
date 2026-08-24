#pragma once

#include <bedrock/auth/AuthCache.hpp>
#include <bedrock/auth/JsPromise.hpp>
#include <bedrock/auth/JsRuntimeValue.hpp>
#include <bedrock/auth/XboxProofKey.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bedrock {

struct XboxTokenHttpRequest {
    std::string method = "post";
    std::string url;
    JsRuntimeValue headersObject = JsRuntimeValue::object();
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    const std::string* header(std::string_view name) const noexcept;
};

struct XboxTokenHttpResponse {
    int status = 0;
    std::string statusText;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string bodyText;

    bool ok() const noexcept { return status >= 200 && status < 300; }
    const std::string* header(std::string_view name) const noexcept;
};

class IXboxTokenHttpClient {
public:
    virtual ~IXboxTokenHttpClient() = default;

    // Native equivalent of global fetch(url, init). The response body remains
    // unconsumed text so XboxTokenManager can preserve the source's exact
    // json()/text() and status-check ordering.
    virtual JsPromise<XboxTokenHttpResponse> fetch(
        XboxTokenHttpRequest request
    ) = 0;
};

using XboxTokenHttpClientPtr = std::shared_ptr<IXboxTokenHttpClient>;

// Default byte transport. Protocol and JSON/error handling intentionally stay
// in XboxTokenManager; this adapter only performs the HTTP exchange.
class CurlXboxTokenHttpClient final : public IXboxTokenHttpClient {
public:
    explicit CurlXboxTokenHttpClient(
        std::shared_ptr<JsMicrotaskQueue> microtaskQueue
    );

    JsPromise<XboxTokenHttpResponse> fetch(
        XboxTokenHttpRequest request
    ) override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

struct XboxTokenManagerDependencies {
    XboxTokenHttpClientPtr httpClient;
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue;

    // Date.now() seam. It is evaluated independently at every source call:
    // UUID names, signature timestamps, and each cache-validity check.
    std::function<double()> dateNowMilliseconds;
};

struct XboxTokenManagerState;

// Direct native port of prismarine-auth 2.7.0
// src/TokenManagers/XboxTokenManager.js. The first constructor deliberately
// has exactly the source's two arguments.
class XboxTokenManager {
private:
    std::shared_ptr<XboxTokenManagerState> state_;

public:
    XboxTokenManager(XboxProofKey ecKey, AuthCachePtr cacheValue);

    // C++ testing/integration extension; the production constructor above
    // installs the same default queue, clock, and fetch transport.
    XboxTokenManager(
        XboxProofKey ecKey,
        AuthCachePtr cacheValue,
        XboxTokenManagerDependencies dependencies
    );

    XboxTokenManager(const XboxTokenManager&) = delete;
    XboxTokenManager& operator=(const XboxTokenManager&) = delete;
    XboxTokenManager(XboxTokenManager&&) = delete;
    XboxTokenManager& operator=(XboxTokenManager&&) = delete;

    // JavaScript-visible instance fields. jwk and headers are live ordered
    // value graphs, so caller mutation is observed by later requests.
    XboxProofKey& key;
    JsRuntimeValue& jwk;
    AuthCachePtr& cache;
    JsRuntimeValue& headers;

    JsPromise<void> setCachedToken(JsRuntimeValue data);
    JsPromise<JsRuntimeValue> getCachedTokens(
        JsRuntimeValue relyingParty = JsRuntimeValue::undefined()
    );

    void checkTokenError(
        const JsRuntimeValue& errorCode,
        const JsRuntimeValue& response
    ) const;

    JsPromise<JsRuntimeValue> getUserToken(
        JsRuntimeValue accessToken,
        JsRuntimeValue azure = JsRuntimeValue::undefined()
    );

    std::vector<std::uint8_t> sign(
        std::string_view url,
        std::string_view authorizationToken,
        std::string_view payload
    ) const;

    JsPromise<JsRuntimeValue> getDeviceToken(
        JsRuntimeValue asDevice = JsRuntimeValue::undefined()
    );

    JsPromise<JsRuntimeValue> getXSTSToken(
        JsRuntimeValue tokens,
        JsRuntimeValue options = JsRuntimeValue::object()
    );

    JsPromise<JsRuntimeValue> getTitleToken(
        JsRuntimeValue msaAccessToken,
        JsRuntimeValue deviceToken
    );

    std::shared_ptr<JsMicrotaskQueue> microtaskQueue() const noexcept;
    XboxTokenHttpClientPtr httpClient() const noexcept;

    // prismarine-auth Util.createHash authority: SHA-1 of Node's `binary`
    // string bytes, lowercase hex, first six characters. This is the dynamic
    // cache-property name used for relying-party-specific XSTS entries.
    static std::string relyingPartyCacheKey(
        const JsRuntimeValue& relyingParty
    );
};

} // namespace bedrock
