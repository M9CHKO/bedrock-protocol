#pragma once

#include <bedrock/auth/MsalHttpClient.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bedrock {

// Platform fields observed from @azure/msal-node on the Windows x64 runtime
// used by bedrock-protocol. Empty fields are omitted, matching addLibraryInfo.
struct MsalPlatformInfo {
    MsalPlatformInfo();

    std::string os;
    std::string cpu;
};

enum class MsalTelemetryOperation {
    DeviceCodePolling,
    RefreshToken
};

// The exact RequestParameterBuilder subset exercised by prismarine-auth's
// MsaTokenManager through @azure/msal-node 2.16.3 / msal-common 14.16.1.
// Values are retained in insertion order because JavaScript Map#set retains
// the original position when an existing key is replaced.
class MsalRequestParameterBuilder {
public:
    using Parameter = std::pair<std::string, std::string>;

    void addScopes(const std::vector<std::string>& scopes);
    void addClientId(std::string_view clientId);
    void addGrantType(std::string_view grantType);
    void addDeviceCode(std::string_view deviceCode);
    void addRefreshToken(std::string_view refreshToken);
    void addCorrelationId(std::string_view correlationId);
    void addClientInfo();
    void addLibraryInfo(const MsalPlatformInfo& platform);
    void addThrottling();
    void addServerTelemetry(
        MsalTelemetryOperation operation,
        std::string_view lastTelemetry = "5|0|||0,0",
        std::string_view currentTelemetry = {}
    );

    std::string createQueryString() const;
    const std::vector<Parameter>& parameters() const noexcept;

    // Equivalent to JavaScript encodeURIComponent, including its unescaped
    // punctuation set and URIError-equivalent rejection of lone surrogates.
    static std::string encodeURIComponent(std::string_view value);

    // Models initializeBaseRequest followed by RequestParameterBuilder's
    // ScopeSet: trim, remove empty strings, exact-value de-duplicate, retain
    // first occurrence, then append missing OIDC defaults in source order.
    static std::vector<std::string> normalizeScopes(
        const std::vector<std::string>& scopes
    );

private:
    std::vector<Parameter> parameters_;

    void set(std::string key, std::string value);
};

struct MsalRequestBuilderOptions {
    std::string clientId;
    std::vector<std::string> scopes {
        "XboxLive.signin",
        "offline_access"
    };
    std::string correlationId;
    MsalPlatformInfo platform;
    std::string authority =
        "https://login.microsoftonline.com/consumers";
    std::string currentTelemetry;
    std::string lastTelemetry = "5|0|||0,0";

    // DeviceCodeClient reads the live request.correlationId twice after its
    // callback: the token URL coerces the raw value, while the form body uses
    // `value || createNewGuid()`. Overrides preserve that rare divergence.
    std::optional<std::string> tokenQueryCorrelationId;
    std::optional<std::string> deviceCodeBodyCorrelationId;

    // Used only when correlationId is empty. Supplying this callback gives
    // tests/callers the same deterministic injection point as MSAL's crypto
    // provider; otherwise an RFC 4122 version-4 identifier is generated.
    std::function<std::string()> correlationIdFactory;
};

// Builds the initial device-code POST, the subsequent token-poll POST, and
// the refresh-token POST exactly as emitted by the dependency versions above.
class MsalRequestBuilder {
public:
    explicit MsalRequestBuilder(MsalRequestBuilderOptions options);

    MsalHttpRequest deviceCodeRequest() const;
    MsalHttpRequest deviceCodeTokenRequest(
        std::string_view deviceCode
    ) const;
    MsalHttpRequest refreshTokenRequest(
        std::string_view refreshToken
    ) const;

    // Verb-prefixed aliases make the builder convenient at call sites while
    // retaining the concise operation names above.
    MsalHttpRequest createDeviceCodeRequest() const {
        return deviceCodeRequest();
    }
    MsalHttpRequest createDeviceCodeTokenRequest(
        std::string_view deviceCode
    ) const {
        return deviceCodeTokenRequest(deviceCode);
    }
    MsalHttpRequest createRefreshTokenRequest(
        std::string_view refreshToken
    ) const {
        return refreshTokenRequest(refreshToken);
    }

    const std::string& clientId() const noexcept;
    const std::vector<std::string>& scopes() const noexcept;
    const std::string& correlationId() const noexcept;
    const MsalPlatformInfo& platform() const noexcept;
    const std::string& authority() const noexcept;

    std::string deviceCodeEndpoint() const;
    std::string tokenEndpoint() const;

    static std::string generateCorrelationId();

private:
    std::string clientId_;
    std::vector<std::string> scopes_;
    std::string correlationId_;
    MsalPlatformInfo platform_;
    std::string authority_;
    std::string currentTelemetry_;
    std::string lastTelemetry_;
    std::string tokenQueryCorrelationId_;
    std::string deviceCodeBodyCorrelationId_;

    static std::vector<std::pair<std::string, std::string>> formHeaders();
};

} // namespace bedrock
