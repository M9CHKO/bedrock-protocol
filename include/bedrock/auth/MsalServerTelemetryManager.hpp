#pragma once

#include <bedrock/auth/JsRuntimeValue.hpp>

#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bedrock {

enum class MsalServerTelemetryApiId : int {
    DeviceCode = 671,
    RefreshToken = 872
};

enum class MsalServerTelemetryCacheOutcome {
    NotApplicable,
    ForceRefreshOrClaims,
    NoCachedAccessToken,
    CachedAccessTokenExpired,
    ProactivelyRefreshed
};

struct MsalServerTelemetryRequest {
    std::string clientId;
    int apiId = static_cast<int>(MsalServerTelemetryApiId::DeviceCode);
    std::string correlationId;
    std::string wrapperSKU;
    std::string wrapperVer;
};

// Dynamic constructor input for tests and callers which need JavaScript's
// runtime coercion rather than the declared TypeScript request types.
struct MsalServerTelemetryRuntimeRequest {
    JsRuntimeValue clientId = JsRuntimeValue::string("");
    JsRuntimeValue apiId = JsRuntimeValue::number(
        static_cast<int>(MsalServerTelemetryApiId::DeviceCode)
    );
    JsRuntimeValue correlationId = JsRuntimeValue::string("");
    JsRuntimeValue wrapperSKU = JsRuntimeValue::undefined();
    JsRuntimeValue wrapperVer = JsRuntimeValue::undefined();
};

struct MsalRegionDiscoveryMetadata {
    JsRuntimeValue regionUsed = JsRuntimeValue::undefined();
    JsRuntimeValue regionSource = JsRuntimeValue::undefined();
    JsRuntimeValue regionOutcome = JsRuntimeValue::undefined();
};

// CacheManager subset used by ServerTelemetryManager. Values are live
// JsRuntimeValue graphs so the manager observes the same entity identity on a
// get/mutate/set sequence as NodeStorage.
class IMsalServerTelemetryCache {
public:
    virtual ~IMsalServerTelemetryCache() = default;

    virtual JsRuntimeValue getServerTelemetry(std::string_view key) = 0;
    virtual void setServerTelemetry(
        std::string key,
        JsRuntimeValue entity,
        std::string correlationId
    ) = 0;
    virtual bool removeItem(
        std::string_view key,
        std::string_view correlationId
    ) = 0;
};

using MsalServerTelemetryCachePtr =
    std::shared_ptr<IMsalServerTelemetryCache>;

// NodeStorage-compatible in-memory implementation. Replacing an existing key
// retains its insertion position. Raw access is intentionally available for
// dependency-oracle tests and embedding in a wider native flat cache.
class MsalServerTelemetryMemoryCache final
    : public IMsalServerTelemetryCache {
public:
    struct Entry {
        std::string key;
        JsRuntimeValue value;
    };

    JsRuntimeValue getServerTelemetry(std::string_view key) override;
    void setServerTelemetry(
        std::string key,
        JsRuntimeValue entity,
        std::string correlationId
    ) override;
    bool removeItem(
        std::string_view key,
        std::string_view correlationId
    ) override;

    void setRawItem(std::string key, JsRuntimeValue value);
    JsRuntimeValue rawItem(std::string_view key) const;
    const std::vector<Entry>& entries() const noexcept { return entries_; }

private:
    std::vector<Entry> entries_;
};

// Explicit model of JavaScript unknown/Error/AuthError inputs accepted by
// cacheFailedRequest. The optional override models an Error with a customized
// toString method, including an empty/falsy result.
struct MsalServerTelemetryError {
    enum class Kind {
        Unknown,
        Error,
        AuthError
    };

    Kind kind = Kind::Unknown;
    std::string name = "Error";
    std::string message;
    std::string errorCode;
    std::string subError;
    std::optional<std::string> toStringOverride;

    static MsalServerTelemetryError unknown();
    static MsalServerTelemetryError error(
        std::string message,
        std::string name = "Error"
    );
    static MsalServerTelemetryError authError(
        std::string errorCode = {},
        std::string errorMessage = {},
        std::string subError = {},
        std::string name = "AuthError"
    );

    std::string toString() const;
};

struct MsalExtraSkuParams {
    std::optional<std::string> libraryName;
    std::optional<std::string> libraryVersion;
    std::optional<std::string> extensionName;
    std::optional<std::string> extensionVersion;
    std::optional<std::string> skus;
};

// Direct native port of @azure/msal-common 14.16.1
// ServerTelemetryManager. MAX_CUR_HEADER_BYTES is deliberately not applied by
// the dependency; only last-request entries are limited, with a strict <330
// UTF-16-code-unit test.
class MsalServerTelemetryManager {
public:
    static constexpr int kDeviceCodeApiId = 671;
    static constexpr int kRefreshTokenApiId = 872;
    static constexpr std::size_t kMaxCurrentHeaderBytes = 80;
    static constexpr std::size_t kMaxLastHeaderBytes = 330;
    static constexpr std::size_t kMaxCachedErrors = 50;

    explicit MsalServerTelemetryManager(
        MsalServerTelemetryRequest request,
        MsalServerTelemetryCachePtr cache = {}
    );
    explicit MsalServerTelemetryManager(
        MsalServerTelemetryRuntimeRequest request,
        MsalServerTelemetryCachePtr cache = {}
    );

    std::string generateCurrentRequestHeaderValue();
    std::string generateLastRequestHeaderValue();

    // Convenience aliases for MsalRequestBuilderOptions.currentTelemetry and
    // .lastTelemetry.
    std::string currentTelemetry() {
        return generateCurrentRequestHeaderValue();
    }
    std::string lastTelemetry() {
        return generateLastRequestHeaderValue();
    }

    void cacheFailedRequest(const MsalServerTelemetryError& error);
    void cacheFailedRequest(std::exception_ptr error);
    void cacheUnknownFailure();

    JsRuntimeValue incrementCacheHits();
    JsRuntimeValue getLastRequests();
    void clearTelemetryCache();

    static std::size_t maxErrorsToSend(
        const JsRuntimeValue& serverTelemetryEntity
    );

    std::string getRegionDiscoveryFields() const;
    void updateRegionDiscoveryMetadata(
        MsalRegionDiscoveryMetadata metadata
    );

    void setCacheOutcome(MsalServerTelemetryCacheOutcome outcome);
    void setCacheOutcome(std::string value);
    void setCacheOutcome(JsRuntimeValue value);

    void setNativeBrokerErrorCode(std::string errorCode);
    JsRuntimeValue getNativeBrokerErrorCodeValue();
    std::optional<std::string> getNativeBrokerErrorCode();
    void clearNativeBrokerErrorCode();

    static std::string makeExtraSkuString(const MsalExtraSkuParams& params);

    const std::string& telemetryCacheKey() const noexcept {
        return telemetryCacheKey_;
    }
    MsalServerTelemetryCachePtr cache() const noexcept { return cache_; }

private:
    MsalServerTelemetryCachePtr cache_;
    JsRuntimeValue apiId_ = JsRuntimeValue::number(kDeviceCodeApiId);
    JsRuntimeValue correlationId_ = JsRuntimeValue::string("");
    std::string telemetryCacheKey_;
    JsRuntimeValue wrapperSKU_ = JsRuntimeValue::string("");
    JsRuntimeValue wrapperVer_ = JsRuntimeValue::string("");
    JsRuntimeValue regionUsed_ = JsRuntimeValue::undefined();
    JsRuntimeValue regionSource_ = JsRuntimeValue::undefined();
    JsRuntimeValue regionOutcome_ = JsRuntimeValue::undefined();
    JsRuntimeValue cacheOutcome_ = JsRuntimeValue::string("0");

    void writeLastRequests(JsRuntimeValue entity);
};

} // namespace bedrock
