#pragma once

#include <bedrock/auth/JsRuntimeValue.hpp>
#include <bedrock/auth/MsalError.hpp>
#include <bedrock/auth/MsalSerializableTokenCache.hpp>

#include <memory>
#include <functional>
#include <string>
#include <optional>
#include <vector>

namespace bedrock {

// Request/authority state consumed by msal-common's ResponseHandler after an
// AAD consumers token response has been received. Times are JavaScript epoch
// seconds; AuthenticationResult exposes the derived values as Date objects.
struct MsalTokenResponseContext {
    std::string clientId;
    std::string canonicalAuthority =
        "https://login.microsoftonline.com/consumers/";
    std::string preferredCacheEnvironment = "login.windows.net";
    std::string authorityTenant = "consumers";
    std::vector<std::string> normalizedRequestScopes;
    std::string correlationId;
    std::optional<JsRuntimeValue> authenticationResultCorrelationId;
    std::string requestId;
    double reqTimestampSeconds = 0.0;
    // CacheHelpers.createAccessTokenEntity reads TimeUtils.nowSeconds()
    // lazily, and only when an access token is actually present. Native PCA
    // supplies this callback so clock reads keep the same observable order.
    std::function<double()> nowSecondsCallback;
    // Retained as a deterministic fallback for direct bounded-handler users.
    double nowSeconds = 0.0;
    JsRuntimeValue sshKid = JsRuntimeValue::undefined();

    // RefreshTokenClient passes these two values to ResponseHandler. A direct
    // migration refresh does not repopulate an account removed between cache
    // lookup and response unless forceCache is true.
    bool handlingRefreshTokenResponse = false;
    bool forceCacheRefreshTokenResponse = false;
};

// Exact token-response InteractionRequiredAuthError counterpart. The common
// MsalError surface intentionally has only AuthError, ClientAuthError and
// ServerError; token validation needs the additional enumerable server fields.
class MsalInteractionRequiredAuthError final : public MsalAuthError {
public:
    MsalInteractionRequiredAuthError(
        std::string errorCode,
        std::string errorMessage,
        std::string subError = {},
        std::string timestamp = {},
        std::string traceId = {},
        std::string correlationId = {},
        std::string claims = {},
        JsRuntimeValue errorNo = JsRuntimeValue::undefined()
    );

    const std::string& timestamp() const noexcept { return timestamp_; }
    const std::string& traceId() const noexcept { return traceId_; }
    const std::string& claims() const noexcept { return claims_; }
    const JsRuntimeValue& errorNo() const noexcept { return errorNo_; }

private:
    std::string timestamp_;
    std::string traceId_;
    std::string claims_;
    JsRuntimeValue errorNo_ = JsRuntimeValue::undefined();
};

// Immutable output of ResponseHandler.generateCacheRecord. It deliberately
// exists before beforeCacheAccess runs; savePreparedTokenResponse must be
// called only after that hook has populated the serializable cache.
class MsalPreparedTokenResponse final {
public:
    MsalPreparedTokenResponse() noexcept = default;
    bool valid() const noexcept;

private:
    struct State;

    explicit MsalPreparedTokenResponse(std::shared_ptr<const State> state);

    std::shared_ptr<const State> state_;

    friend class MsalTokenResponseHandler;
};

// Synchronous bounded port of msal-common 14.16.1 ResponseHandler plus the
// msal-node 2.16.3 serializer boundary. Persistence hooks intentionally stay
// outside this class. The exact plugin sequence is prepare, beforeCacheAccess,
// save, afterCacheAccess, authenticationResult.
class MsalTokenResponseHandler {
public:
    explicit MsalTokenResponseHandler(MsalTokenResponseContext context);

    const MsalTokenResponseContext& context() const noexcept {
        return context_;
    }

    void validateTokenResponse(const JsRuntimeValue& serverResponse) const;

    MsalPreparedTokenResponse prepareServerTokenResponse(
        const JsRuntimeValue& serverResponse,
        const MsalSerializableTokenCache& tokenCacheBeforeHook
    ) const;

    // Returns false only for the acquireTokenSilent race where an account was
    // removed by beforeCacheAccess. In that case no entity is written and the
    // returned bool must be passed to generateAuthenticationResult.
    bool savePreparedTokenResponse(
        const MsalPreparedTokenResponse& prepared,
        MsalSerializableTokenCache& tokenCacheAfterHook
    ) const;

    JsRuntimeValue generateAuthenticationResult(
        const MsalPreparedTokenResponse& prepared,
        bool includeServerTokenResponse
    ) const;

    // Convenience path for callers without persistence hooks. Hook-enabled
    // callers must use the split API above to preserve msal-common ordering.
    JsRuntimeValue handleServerTokenResponse(
        const JsRuntimeValue& serverResponse,
        MsalSerializableTokenCache& tokenCache
    ) const;

    // Public key helpers operate on msal-node's serialized snake_case shapes.
    static std::string generateAccountCacheKey(
        const JsRuntimeValue& serializedAccount
    );
    static std::string generateCredentialKey(
        const JsRuntimeValue& serializedCredential
    );
    static std::string generateAppMetadataKey(
        const JsRuntimeValue& serializedAppMetadata
    );

private:
    MsalTokenResponseContext context_;
};

} // namespace bedrock
