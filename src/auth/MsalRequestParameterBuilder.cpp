#include <bedrock/auth/MsalRequestParameterBuilder.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace bedrock {
namespace {

constexpr std::string_view kContentTypeName = "Content-Type";
constexpr std::string_view kContentTypeValue =
    "application/x-www-form-urlencoded;charset=utf-8";
constexpr std::string_view kMsalSku = "msal.js.node";
constexpr std::string_view kMsalVersion = "2.16.3";
constexpr std::string_view kThrottlingCapability = "retry-after, h429";
constexpr std::string_view kLastTelemetry = "5|0|||0,0";

struct DecodedCodePoint {
    std::uint32_t value = 0;
    std::size_t width = 0;
};

DecodedCodePoint decodeUtf8OrWtf8(
    std::string_view input,
    std::size_t offset
) {
    if (offset >= input.size()) {
        throw std::invalid_argument("URI malformed");
    }

    const auto byte = [&](std::size_t index) {
        return static_cast<std::uint8_t>(input[index]);
    };
    const auto continuation = [&](std::size_t index) {
        return index < input.size() && (byte(index) & 0xc0U) == 0x80U;
    };

    const auto first = byte(offset);
    if (first < 0x80U) return { first, 1 };

    if (first >= 0xc2U && first <= 0xdfU && continuation(offset + 1)) {
        return {
            ((first & 0x1fU) << 6) | (byte(offset + 1) & 0x3fU),
            2
        };
    }

    if (first >= 0xe0U && first <= 0xefU &&
        continuation(offset + 1) && continuation(offset + 2)) {
        const auto second = byte(offset + 1);
        if ((first == 0xe0U && second < 0xa0U) ||
            (first == 0xedU && second >= 0xc0U)) {
            throw std::invalid_argument("URI malformed");
        }
        return {
            ((first & 0x0fU) << 12) |
                ((second & 0x3fU) << 6) |
                (byte(offset + 2) & 0x3fU),
            3
        };
    }

    if (first >= 0xf0U && first <= 0xf4U &&
        continuation(offset + 1) && continuation(offset + 2) &&
        continuation(offset + 3)) {
        const auto second = byte(offset + 1);
        if ((first == 0xf0U && second < 0x90U) ||
            (first == 0xf4U && second >= 0x90U)) {
            throw std::invalid_argument("URI malformed");
        }
        return {
            ((first & 0x07U) << 18) |
                ((second & 0x3fU) << 12) |
                ((byte(offset + 2) & 0x3fU) << 6) |
                (byte(offset + 3) & 0x3fU),
            4
        };
    }

    throw std::invalid_argument("URI malformed");
}

void appendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7fU) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else if (codePoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codePoint >> 12)));
        output.push_back(static_cast<char>(
            0x80U | ((codePoint >> 6) & 0x3fU)
        ));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codePoint >> 18)));
        output.push_back(static_cast<char>(
            0x80U | ((codePoint >> 12) & 0x3fU)
        ));
        output.push_back(static_cast<char>(
            0x80U | ((codePoint >> 6) & 0x3fU)
        ));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    }
}

bool isEcmaTrimCodePoint(std::uint32_t value) noexcept {
    switch (value) {
        case 0x0009U:
        case 0x000aU:
        case 0x000bU:
        case 0x000cU:
        case 0x000dU:
        case 0x0020U:
        case 0x00a0U:
        case 0x1680U:
        case 0x2028U:
        case 0x2029U:
        case 0x202fU:
        case 0x205fU:
        case 0x3000U:
        case 0xfeffU:
            return true;
        default:
            return value >= 0x2000U && value <= 0x200aU;
    }
}

std::string trimEcmaWhitespace(std::string_view value) {
    struct Unit {
        std::size_t begin;
        std::size_t end;
        std::uint32_t codePoint;
    };

    std::vector<Unit> units;
    units.reserve(value.size());
    for (std::size_t offset = 0; offset < value.size();) {
        const auto decoded = decodeUtf8OrWtf8(value, offset);
        units.push_back({ offset, offset + decoded.width, decoded.value });
        offset += decoded.width;
    }

    std::size_t first = 0;
    while (first < units.size() &&
        isEcmaTrimCodePoint(units[first].codePoint)) {
        ++first;
    }
    std::size_t last = units.size();
    while (last > first && isEcmaTrimCodePoint(units[last - 1].codePoint)) {
        --last;
    }

    if (first == last) return {};
    return std::string(value.substr(
        units[first].begin,
        units[last - 1].end - units[first].begin
    ));
}

bool encodeURIComponentSafe(std::uint8_t value) noexcept {
    return (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9') || value == '-' || value == '_' ||
        value == '.' || value == '!' || value == '~' || value == '*' ||
        value == '\'' || value == '(' || value == ')';
}

void appendPercentEncoded(std::string& output, std::uint8_t value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    if (encodeURIComponentSafe(value)) {
        output.push_back(static_cast<char>(value));
        return;
    }
    output.push_back('%');
    output.push_back(kHex[(value >> 4) & 0x0fU]);
    output.push_back(kHex[value & 0x0fU]);
}

std::string normalizedAuthority(std::string authority) {
    if (authority.empty()) {
        authority = "https://login.microsoftonline.com/consumers";
    }
    while (!authority.empty() && authority.back() == '/') {
        authority.pop_back();
    }
    return authority;
}

std::string appendQuery(std::string endpoint, std::string_view query) {
    if (query.empty()) return endpoint;
    endpoint.push_back(endpoint.find('?') == std::string::npos ? '?' : '&');
    endpoint.append(query);
    return endpoint;
}

} // namespace

MsalPlatformInfo::MsalPlatformInfo() {
#if defined(_WIN32) || defined(__MSYS__) || defined(__CYGWIN__)
    os = "win32";
#elif defined(__APPLE__)
    os = "darwin";
#elif defined(__linux__)
    os = "linux";
#elif defined(__FreeBSD__)
    os = "freebsd";
#elif defined(__OpenBSD__)
    os = "openbsd";
#elif defined(__NetBSD__)
    os = "netbsd";
#elif defined(_AIX)
    os = "aix";
#elif defined(__sun)
    os = "sunos";
#endif

#if defined(_M_X64) || defined(__x86_64__)
    cpu = "x64";
#elif defined(_M_IX86) || defined(__i386__)
    cpu = "ia32";
#elif defined(_M_ARM64) || defined(__aarch64__)
    cpu = "arm64";
#elif defined(_M_ARM) || defined(__arm__)
    cpu = "arm";
#elif defined(__s390x__)
    cpu = "s390x";
#elif defined(__ppc64__)
    cpu = "ppc64";
#endif
}

void MsalRequestParameterBuilder::set(std::string key, std::string value) {
    const auto found = std::find_if(
        parameters_.begin(),
        parameters_.end(),
        [&](const Parameter& item) { return item.first == key; }
    );
    if (found == parameters_.end()) {
        parameters_.emplace_back(std::move(key), std::move(value));
    } else {
        found->second = std::move(value);
    }
}

void MsalRequestParameterBuilder::addScopes(
    const std::vector<std::string>& scopes
) {
    const auto normalized = normalizeScopes(scopes);
    std::string joined;
    for (const auto& scope : normalized) {
        if (!joined.empty()) joined.push_back(' ');
        joined += scope;
    }
    set("scope", encodeURIComponent(joined));
}

void MsalRequestParameterBuilder::addClientId(std::string_view clientId) {
    set("client_id", encodeURIComponent(clientId));
}

void MsalRequestParameterBuilder::addGrantType(std::string_view grantType) {
    set("grant_type", encodeURIComponent(grantType));
}

void MsalRequestParameterBuilder::addDeviceCode(std::string_view deviceCode) {
    set("device_code", encodeURIComponent(deviceCode));
}

void MsalRequestParameterBuilder::addRefreshToken(
    std::string_view refreshToken
) {
    set("refresh_token", encodeURIComponent(refreshToken));
}

void MsalRequestParameterBuilder::addCorrelationId(
    std::string_view correlationId
) {
    set("client-request-id", encodeURIComponent(correlationId));
}

void MsalRequestParameterBuilder::addClientInfo() {
    set("client_info", "1");
}

void MsalRequestParameterBuilder::addLibraryInfo(
    const MsalPlatformInfo& platform
) {
    set("x-client-SKU", std::string(kMsalSku));
    set("x-client-VER", std::string(kMsalVersion));
    if (!platform.os.empty()) set("x-client-OS", platform.os);
    if (!platform.cpu.empty()) set("x-client-CPU", platform.cpu);
}

void MsalRequestParameterBuilder::addThrottling() {
    set("x-ms-lib-capability", std::string(kThrottlingCapability));
}

void MsalRequestParameterBuilder::addServerTelemetry(
    MsalTelemetryOperation operation,
    std::string_view lastTelemetry,
    std::string_view currentTelemetry
) {
    const std::string apiId = operation ==
            MsalTelemetryOperation::DeviceCodePolling
        ? "671"
        : "872";
    set(
        "x-client-current-telemetry",
        currentTelemetry.empty()
            ? "5|" + apiId + ",0,,,|,"
            : std::string(currentTelemetry)
    );
    set(
        "x-client-last-telemetry",
        lastTelemetry.empty()
            ? std::string(kLastTelemetry)
            : std::string(lastTelemetry)
    );
}

std::string MsalRequestParameterBuilder::createQueryString() const {
    std::string result;
    for (const auto& item : parameters_) {
        if (!result.empty()) result.push_back('&');
        result += item.first;
        result.push_back('=');
        result += item.second;
    }
    return result;
}

const std::vector<MsalRequestParameterBuilder::Parameter>&
MsalRequestParameterBuilder::parameters() const noexcept {
    return parameters_;
}

std::string MsalRequestParameterBuilder::encodeURIComponent(
    std::string_view value
) {
    std::string utf8;
    utf8.reserve(value.size());

    for (std::size_t offset = 0; offset < value.size();) {
        const auto decoded = decodeUtf8OrWtf8(value, offset);
        offset += decoded.width;
        std::uint32_t codePoint = decoded.value;

        if (codePoint >= 0xd800U && codePoint <= 0xdbffU) {
            if (offset >= value.size()) {
                throw std::invalid_argument("URI malformed");
            }
            const auto low = decodeUtf8OrWtf8(value, offset);
            if (low.value < 0xdc00U || low.value > 0xdfffU) {
                throw std::invalid_argument("URI malformed");
            }
            offset += low.width;
            codePoint = 0x10000U +
                ((codePoint - 0xd800U) << 10) + (low.value - 0xdc00U);
        } else if (codePoint >= 0xdc00U && codePoint <= 0xdfffU) {
            throw std::invalid_argument("URI malformed");
        }

        appendUtf8(utf8, codePoint);
    }

    std::string result;
    result.reserve(utf8.size() * 3U);
    for (const char byteValue : utf8) {
        appendPercentEncoded(
            result,
            static_cast<std::uint8_t>(
                static_cast<unsigned char>(byteValue)
            )
        );
    }
    return result;
}

std::vector<std::string> MsalRequestParameterBuilder::normalizeScopes(
    const std::vector<std::string>& scopes
) {
    static constexpr std::array<std::string_view, 3> kOidcDefaults {
        "openid",
        "profile",
        "offline_access"
    };

    std::vector<std::string> result;
    result.reserve(scopes.size() + kOidcDefaults.size());
    std::unordered_set<std::string> seen;

    const auto append = [&](std::string scope) {
        scope = trimEcmaWhitespace(scope);
        if (!scope.empty() && seen.insert(scope).second) {
            result.push_back(std::move(scope));
        }
    };

    for (const auto& scope : scopes) append(scope);
    for (const auto scope : kOidcDefaults) append(std::string(scope));
    return result;
}

MsalRequestBuilder::MsalRequestBuilder(MsalRequestBuilderOptions options)
    : clientId_(std::move(options.clientId)),
      scopes_(std::move(options.scopes)),
      correlationId_(std::move(options.correlationId)),
      platform_(std::move(options.platform)),
      authority_(normalizedAuthority(std::move(options.authority))),
      currentTelemetry_(std::move(options.currentTelemetry)),
      lastTelemetry_(std::move(options.lastTelemetry)) {
    if (correlationId_.empty()) {
        correlationId_ = options.correlationIdFactory
            ? options.correlationIdFactory()
            : generateCorrelationId();
    }
    tokenQueryCorrelationId_ = options.tokenQueryCorrelationId.value_or(
        correlationId_
    );
    deviceCodeBodyCorrelationId_ =
        options.deviceCodeBodyCorrelationId.value_or(correlationId_);
}

MsalHttpRequest MsalRequestBuilder::deviceCodeRequest() const {
    MsalRequestParameterBuilder body;
    body.addScopes(scopes_);
    body.addClientId(clientId_);
    MsalHttpRequest request;
    request.url = deviceCodeEndpoint();
    request.headers = formHeaders();
    request.body = body.createQueryString();
    return request;
}

MsalHttpRequest MsalRequestBuilder::deviceCodeTokenRequest(
    std::string_view deviceCode
) const {
    MsalRequestParameterBuilder query;
    query.addCorrelationId(tokenQueryCorrelationId_);

    MsalRequestParameterBuilder body;
    body.addScopes(scopes_);
    body.addClientId(clientId_);
    body.addGrantType("device_code");
    body.addDeviceCode(deviceCode);
    body.addCorrelationId(deviceCodeBodyCorrelationId_);
    body.addClientInfo();
    body.addLibraryInfo(platform_);
    body.addThrottling();
    body.addServerTelemetry(
        MsalTelemetryOperation::DeviceCodePolling,
        lastTelemetry_,
        currentTelemetry_
    );

    MsalHttpRequest request;
    request.url = appendQuery(tokenEndpoint(), query.createQueryString());
    request.headers = formHeaders();
    request.body = body.createQueryString();
    return request;
}

MsalHttpRequest MsalRequestBuilder::refreshTokenRequest(
    std::string_view refreshToken
) const {
    MsalRequestParameterBuilder query;
    query.addCorrelationId(correlationId_);

    MsalRequestParameterBuilder body;
    body.addClientId(clientId_);
    body.addScopes(scopes_);
    body.addGrantType("refresh_token");
    body.addClientInfo();
    body.addLibraryInfo(platform_);
    body.addThrottling();
    body.addServerTelemetry(
        MsalTelemetryOperation::RefreshToken,
        lastTelemetry_,
        currentTelemetry_
    );
    body.addRefreshToken(refreshToken);

    MsalHttpRequest request;
    request.url = appendQuery(tokenEndpoint(), query.createQueryString());
    request.headers = formHeaders();
    request.body = body.createQueryString();
    return request;
}

const std::string& MsalRequestBuilder::clientId() const noexcept {
    return clientId_;
}

const std::vector<std::string>& MsalRequestBuilder::scopes() const noexcept {
    return scopes_;
}

const std::string& MsalRequestBuilder::correlationId() const noexcept {
    return correlationId_;
}

const MsalPlatformInfo& MsalRequestBuilder::platform() const noexcept {
    return platform_;
}

const std::string& MsalRequestBuilder::authority() const noexcept {
    return authority_;
}

std::string MsalRequestBuilder::deviceCodeEndpoint() const {
    return authority_ + "/oauth2/v2.0/devicecode";
}

std::string MsalRequestBuilder::tokenEndpoint() const {
    return authority_ + "/oauth2/v2.0/token";
}

std::string MsalRequestBuilder::generateCorrelationId() {
    std::array<std::uint8_t, 16> bytes {};
    std::random_device random;
    for (auto& byteValue : bytes) {
        byteValue = static_cast<std::uint8_t>(random());
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result.push_back('-');
        }
        result.push_back(kHex[(bytes[index] >> 4) & 0x0fU]);
        result.push_back(kHex[bytes[index] & 0x0fU]);
    }
    return result;
}

std::vector<std::pair<std::string, std::string>>
MsalRequestBuilder::formHeaders() {
    return {{ std::string(kContentTypeName), std::string(kContentTypeValue) }};
}

} // namespace bedrock
