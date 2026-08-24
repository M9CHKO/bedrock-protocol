#pragma once

#include <bedrock/auth/MsaTokenManager.hpp>
#include <bedrock/auth/MsalHttpClient.hpp>
#include <bedrock/auth/MsalSerializableTokenCache.hpp>
#include <bedrock/auth/MsalServerTelemetryManager.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bedrock {

struct NativeMsalDependencies {
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue;
    MsalHttpClientPtr httpClient;
    std::function<double()> dateNowMilliseconds;
    std::function<JsPromise<void>(double)> delayMilliseconds;
    std::function<std::string()> correlationIdFactory;
};

// Native port of the PublicClientApplication subset reached by
// prismarine-auth's MsaTokenManager. The public methods intentionally retain
// the same request/Promise/AuthenticationResult graph surface.
class NativeMsalPublicClientApplication final
    : public IMsalPublicClientApplication {
public:
    NativeMsalPublicClientApplication(
        std::shared_ptr<JsRuntimeValue> configuration,
        NativeMsalDependencies dependencies = {}
    );

    JsPromise<JsRuntimeValue> acquireTokenByRefreshToken(
        JsRuntimeValue request
    ) override;

    JsPromise<JsRuntimeValue> acquireTokenByDeviceCode(
        JsRuntimeValue request
    ) override;

    const std::string& clientId() const noexcept { return clientId_; }
    const std::string& authority() const noexcept { return authority_; }
    const JsRuntimeValue& cachePlugin() const noexcept { return cachePlugin_; }
    std::shared_ptr<MsalSerializableTokenCache> tokenCache() const noexcept {
        return tokenCache_;
    }
    MsalServerTelemetryCachePtr serverTelemetryCache() const noexcept {
        return serverTelemetryCache_;
    }
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue() const noexcept {
        return dependencies_.microtaskQueue;
    }

private:
    std::shared_ptr<JsRuntimeValue> sourceConfiguration_;
    NativeMsalDependencies dependencies_;
    std::string clientId_;
    std::string authority_;
    JsRuntimeValue cachePlugin_ = JsRuntimeValue::undefined();
    std::shared_ptr<MsalSerializableTokenCache> tokenCache_;
    MsalServerTelemetryCachePtr serverTelemetryCache_;

    double dateNowMilliseconds() const;
    std::int64_t nowSeconds() const;
    std::string createCorrelationId() const;
    JsPromise<void> delay(double milliseconds) const;
};

} // namespace bedrock
