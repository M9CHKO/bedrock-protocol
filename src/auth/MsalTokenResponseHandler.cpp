#include <bedrock/auth/MsalTokenResponseHandler.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace bedrock {
namespace {

using EntityMap = MsalSerializableTokenCache::EntityMap;

constexpr std::array<std::string_view, 3> kOidcScopes {
    "openid",
    "profile",
    "offline_access"
};

const JsRuntimeValue* property(
    const JsRuntimeValue& value,
    std::string_view name
) {
    if (!value.isObject() && !value.isArray()) return nullptr;
    return value.get(name);
}

std::string numberToString(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0.0 ? "-Infinity" : "Infinity";
    const auto serialized = JsRuntimeJson::stringify(
        JsRuntimeValue::number(value)
    );
    return serialized.value_or("NaN");
}

std::string jsToString(const JsRuntimeValue& value) {
    switch (value.kind()) {
        case JsRuntimeValue::Kind::Undefined: return "undefined";
        case JsRuntimeValue::Kind::Null: return "null";
        case JsRuntimeValue::Kind::Bool:
            return value.boolValue() ? "true" : "false";
        case JsRuntimeValue::Kind::Number:
            return numberToString(value.numberValue());
        case JsRuntimeValue::Kind::String: return value.stringValue();
        case JsRuntimeValue::Kind::Array: {
            std::string result;
            const auto& elements = value.arrayNode()->elements();
            for (std::size_t index = 0; index < elements.size(); ++index) {
                if (index != 0) result.push_back(',');
                if (!elements[index] || elements[index]->isUndefined() ||
                    elements[index]->isNull()) {
                    continue;
                }
                result += jsToString(*elements[index]);
            }
            return result;
        }
        case JsRuntimeValue::Kind::Date:
            return value.dateIsValid()
                ? value.dateIsoString()
                : "Invalid Date";
        case JsRuntimeValue::Kind::Map: return "[object Map]";
        case JsRuntimeValue::Kind::Function:
            return "function () { [native code] }";
        case JsRuntimeValue::Kind::Object:
        case JsRuntimeValue::Kind::Opaque:
            return "[object Object]";
    }
    return {};
}

std::string propertyStringOrEmpty(
    const JsRuntimeValue& value,
    std::string_view name
) {
    const auto* found = property(value, name);
    return found && found->truthy() ? jsToString(*found) : std::string();
}

JsRuntimeValue propertyValueOrUndefined(
    const JsRuntimeValue& value,
    std::string_view name
) {
    const auto* found = property(value, name);
    return found ? *found : JsRuntimeValue::undefined();
}

std::string lowerAscii(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

std::string serializedField(
    const JsRuntimeValue& entity,
    std::string_view name
) {
    const auto* found = property(entity, name);
    return found && !found->isUndefined()
        ? jsToString(*found)
        : std::string();
}

std::string base64Decode(std::string_view input) {
    const auto sextet = [](unsigned char character) -> int {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<int>(character - 'A');
        }
        if (character >= 'a' && character <= 'z') {
            return static_cast<int>(character - 'a') + 26;
        }
        if (character >= '0' && character <= '9') {
            return static_cast<int>(character - '0') + 52;
        }
        if (character == '+' || character == '-') return 62;
        if (character == '/' || character == '_') return 63;
        return -1;
    };

    std::string result;
    result.reserve(input.size() * 3U / 4U + 3U);
    std::uint32_t accumulator = 0;
    unsigned bitCount = 0;
    for (const char rawCharacter : input) {
        const auto character = static_cast<unsigned char>(rawCharacter);
        if (character == '=') break;
        const int decoded = sextet(character);
        if (decoded < 0) continue;
        accumulator = (accumulator << 6U) |
            static_cast<std::uint32_t>(decoded);
        bitCount += 6U;
        while (bitCount >= 8U) {
            bitCount -= 8U;
            result.push_back(static_cast<char>(
                (accumulator >> bitCount) & 0xffU
            ));
        }
    }
    return result;
}

[[noreturn]] void throwClientError(
    std::string code,
    std::string description
) {
    throw MsalClientAuthError(
        std::move(code),
        std::move(description)
    );
}

JsRuntimeValue parseClientInfo(std::string_view encoded) {
    if (encoded.empty()) {
        throwClientError("client_info_empty_error", "The client info was empty");
    }
    try {
        return JsRuntimeJson::parse(base64Decode(encoded));
    } catch (...) {
        throwClientError(
            "client_info_decoding_error",
            "The client info could not be parsed/decoded correctly"
        );
    }
}

bool containsTokenWhitespace(std::string_view value) {
    return value.find_first_of(" \t\n\r\f\v") != std::string_view::npos;
}

JsRuntimeValue extractTokenClaims(std::string_view token) {
    if (token.empty()) {
        throwClientError("null_or_empty_token", "The token is null or empty");
    }
    const auto firstDot = token.find('.');
    const auto secondDot = firstDot == std::string_view::npos
        ? std::string_view::npos
        : token.find('.', firstDot + 1U);
    if (firstDot == std::string_view::npos ||
        secondDot == std::string_view::npos ||
        token.find('.', secondDot + 1U) != std::string_view::npos ||
        secondDot == firstDot + 1U || containsTokenWhitespace(token)) {
        throwClientError("token_parsing_error", "Token cannot be parsed");
    }

    try {
        return JsRuntimeJson::parse(base64Decode(token.substr(
            firstDot + 1U,
            secondDot - firstDot - 1U
        )));
    } catch (...) {
        throwClientError("token_parsing_error", "Token cannot be parsed");
    }
}

std::string claimString(
    const JsRuntimeValue& claims,
    std::string_view name
) {
    return propertyStringOrEmpty(claims, name);
}

std::string claimsTenantId(const JsRuntimeValue& claims) {
    auto result = claimString(claims, "tid");
    if (result.empty()) result = claimString(claims, "tfp");
    if (result.empty()) result = claimString(claims, "acr");
    return result;
}

std::string homeAccountId(
    const std::string& rawClientInfo,
    const JsRuntimeValue& claims
) {
    if (!rawClientInfo.empty()) {
        try {
            const auto clientInfo = parseClientInfo(rawClientInfo);
            const auto uid = propertyStringOrEmpty(clientInfo, "uid");
            const auto utid = propertyStringOrEmpty(clientInfo, "utid");
            if (!uid.empty() && !utid.empty()) return uid + "." + utid;
        } catch (...) {
            // AccountEntity.generateHomeAccountId deliberately swallows a
            // malformed client_info and falls back to the subject claim.
        }
    }
    return claimString(claims, "sub");
}

std::string trimAscii(std::string_view value) {
    const auto whitespace = [](char character) {
        return character == ' ' || character == '\t' || character == '\n' ||
            character == '\r' || character == '\f' || character == '\v';
    };
    std::size_t begin = 0;
    while (begin < value.size() && whitespace(value[begin])) ++begin;
    std::size_t end = value.size();
    while (end > begin && whitespace(value[end - 1U])) --end;
    return std::string(value.substr(begin, end - begin));
}

std::vector<std::string> scopeSetFromArray(
    const std::vector<std::string>& scopes
) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    for (const auto& rawScope : scopes) {
        auto scope = trimAscii(rawScope);
        if (!scope.empty() && seen.insert(scope).second) {
            result.push_back(std::move(scope));
        }
    }
    return result;
}

std::vector<std::string> scopeSetFromString(std::string_view scopes) {
    std::vector<std::string> split;
    for (std::size_t offset = 0;;) {
        const auto separator = scopes.find(' ', offset);
        split.emplace_back(scopes.substr(
            offset,
            separator == std::string_view::npos
                ? scopes.size() - offset
                : separator - offset
        ));
        if (separator == std::string_view::npos) break;
        offset = separator + 1U;
    }
    return scopeSetFromArray(split);
}

std::string joinScopes(const std::vector<std::string>& scopes) {
    std::string result;
    for (const auto& scope : scopes) {
        if (!result.empty()) result.push_back(' ');
        result += scope;
    }
    return result;
}

double parseInt10(std::string_view value) {
    auto trimmed = trimAscii(value);
    std::size_t offset = 0;
    bool negative = false;
    if (offset < trimmed.size() &&
        (trimmed[offset] == '+' || trimmed[offset] == '-')) {
        negative = trimmed[offset] == '-';
        ++offset;
    }
    const std::size_t digitsBegin = offset;
    double result = 0.0;
    while (offset < trimmed.size() &&
        trimmed[offset] >= '0' && trimmed[offset] <= '9') {
        result = result * 10.0 +
            static_cast<double>(trimmed[offset] - '0');
        ++offset;
    }
    if (offset == digitsBegin) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return negative ? -result : result;
}

double numericResponseValue(
    const JsRuntimeValue& response,
    std::string_view name,
    double fallback
) {
    const auto* found = property(response, name);
    if (!found) return fallback;
    double value = std::numeric_limits<double>::quiet_NaN();
    if (found->isString()) {
        value = parseInt10(found->stringValue());
    } else if (found->isNumber()) {
        value = found->numberValue();
    } else if (found->isBool()) {
        value = found->boolValue() ? 1.0 : 0.0;
    }
    return value != 0.0 && !std::isnan(value) ? value : fallback;
}

bool tenantIdMatchesHomeTenant(
    const std::string& tenantId,
    const std::string& homeId
) {
    const auto dot = homeId.find('.');
    if (tenantId.empty() || dot == std::string::npos) return false;
    const auto nextDot = homeId.find('.', dot + 1U);
    return tenantId == homeId.substr(
        dot + 1U,
        nextDot == std::string::npos
            ? std::string::npos
            : nextDot - dot - 1U
    );
}

JsRuntimeValue buildTenantProfile(
    const std::string& homeId,
    const std::string& localAccountId,
    const std::string& tenantId,
    const JsRuntimeValue* claims
) {
    auto result = JsRuntimeValue::object();
    if (claims && claims->truthy()) {
        const auto claimsTenant = claimsTenantId(*claims);
        const auto claimsLocal = [&] {
            auto value = claimString(*claims, "oid");
            if (value.empty()) value = claimString(*claims, "sub");
            return value;
        }();
        result.set("tenantId", JsRuntimeValue::string(claimsTenant));
        result.set("localAccountId", JsRuntimeValue::string(claimsLocal));
        const auto* name = property(*claims, "name");
        result.set(
            "name",
            name ? *name : JsRuntimeValue::undefined()
        );
        result.set(
            "isHomeTenant",
            JsRuntimeValue::boolean(
                tenantIdMatchesHomeTenant(claimsTenant, homeId)
            )
        );
    } else {
        result.set("tenantId", JsRuntimeValue::string(tenantId));
        result.set(
            "localAccountId",
            JsRuntimeValue::string(localAccountId)
        );
        result.set(
            "isHomeTenant",
            JsRuntimeValue::boolean(
                tenantIdMatchesHomeTenant(tenantId, homeId)
            )
        );
    }
    return result;
}

std::vector<JsRuntimeValue> deserializeTenantProfiles(
    const JsRuntimeValue& serializedAccount
) {
    std::vector<JsRuntimeValue> result;
    const auto* profiles = property(serializedAccount, "tenantProfiles");
    if (!profiles || profiles->isUndefined() || profiles->isNull()) {
        return result;
    }
    if (!profiles->isArray()) {
        throw std::runtime_error("serialized tenantProfiles.map is not a function");
    }
    for (const auto& element : profiles->arrayNode()->elements()) {
        if (!element || !element->isString()) {
            throw std::runtime_error(
                "serialized tenant profile is not a JSON string"
            );
        }
        result.push_back(JsRuntimeJson::parse(element->stringValue()));
    }
    return result;
}

JsRuntimeValue serializeTenantProfiles(
    const std::vector<JsRuntimeValue>& profiles
) {
    auto result = JsRuntimeValue::array();
    for (const auto& profile : profiles) {
        const auto serialized = JsRuntimeJson::stringify(profile);
        if (!serialized) {
            throw std::runtime_error("tenant profile is not serializable");
        }
        result.push(JsRuntimeValue::string(*serialized));
    }
    return result;
}

JsRuntimeValue projectAccount(const JsRuntimeValue& account) {
    static constexpr std::array<std::string_view, 11> fields {
        "home_account_id",
        "environment",
        "realm",
        "local_account_id",
        "username",
        "authority_type",
        "name",
        "client_info",
        "last_modification_time",
        "last_modification_app",
        "tenantProfiles"
    };
    auto result = JsRuntimeValue::object();
    for (const auto field : fields) {
        result.set(
            std::string(field),
            propertyValueOrUndefined(account, field)
        );
    }
    return result;
}

bool serializedAccountIsValid(const JsRuntimeValue& account) {
    static constexpr std::array<std::string_view, 6> required {
        "home_account_id",
        "environment",
        "realm",
        "local_account_id",
        "username",
        "authority_type"
    };
    if (!account.isObject()) return false;
    return std::all_of(
        required.begin(),
        required.end(),
        [&](std::string_view field) { return account.hasOwn(field); }
    );
}

std::optional<JsRuntimeValue> findBaseAccount(
    const MsalSerializableTokenCache& cache,
    const std::string& homeId
) {
    const auto accounts = cache.entityMap(EntityMap::Account);
    for (const auto& item : accounts.ownProperties()) {
        if (item.key.starts_with(homeId) &&
            serializedAccountIsValid(item.value)) {
            return projectAccount(item.value);
        }
    }
    return std::nullopt;
}

std::string firstEmail(const JsRuntimeValue& claims) {
    const auto* emails = property(claims, "emails");
    if (!emails || !emails->truthy() || !emails->isArray()) return {};
    const auto* first = emails->get(0U);
    return first && first->truthy() ? jsToString(*first) : std::string();
}

JsRuntimeValue createAccount(
    const MsalTokenResponseContext& context,
    const std::string& homeId,
    const std::string& rawClientInfo,
    const JsRuntimeValue& claims
) {
    std::string uid;
    std::string utid;
    if (!rawClientInfo.empty()) {
        const auto clientInfo = parseClientInfo(rawClientInfo);
        uid = propertyStringOrEmpty(clientInfo, "uid");
        utid = propertyStringOrEmpty(clientInfo, "utid");
    }

    const auto claimTenant = claimsTenantId(claims);
    const auto realm = !utid.empty() ? utid : claimTenant;
    auto localAccountId = uid;
    if (localAccountId.empty()) localAccountId = claimString(claims, "oid");
    if (localAccountId.empty()) localAccountId = claimString(claims, "sub");

    auto username = claimString(claims, "preferred_username");
    if (username.empty()) username = claimString(claims, "upn");
    if (username.empty()) username = firstEmail(claims);

    auto account = JsRuntimeValue::object();
    account.set("home_account_id", JsRuntimeValue::string(homeId));
    account.set(
        "environment",
        JsRuntimeValue::string(context.preferredCacheEnvironment)
    );
    account.set("realm", JsRuntimeValue::string(realm));
    account.set(
        "local_account_id",
        JsRuntimeValue::string(localAccountId)
    );
    account.set("username", JsRuntimeValue::string(username));
    account.set("authority_type", JsRuntimeValue::string("MSSTS"));
    account.set(
        "name",
        JsRuntimeValue::string(claimString(claims, "name"))
    );
    account.set(
        "client_info",
        rawClientInfo.empty()
            ? JsRuntimeValue::undefined()
            : JsRuntimeValue::string(rawClientInfo)
    );
    account.set("last_modification_time", JsRuntimeValue::undefined());
    account.set("last_modification_app", JsRuntimeValue::undefined());

    std::vector<JsRuntimeValue> profiles;
    profiles.push_back(buildTenantProfile(
        homeId,
        localAccountId,
        realm,
        &claims
    ));
    account.set("tenantProfiles", serializeTenantProfiles(profiles));
    return account;
}

struct AccountRecord {
    JsRuntimeValue serialized = JsRuntimeValue::undefined();
    std::string key;
};

std::string accountCacheKey(const JsRuntimeValue& account) {
    const auto homeId = serializedField(account, "home_account_id");
    const auto environment = serializedField(account, "environment");
    const auto realm = serializedField(account, "realm");
    std::string homeTenant;
    const auto dot = homeId.find('.');
    if (dot != std::string::npos) {
        const auto nextDot = homeId.find('.', dot + 1U);
        homeTenant = homeId.substr(
            dot + 1U,
            nextDot == std::string::npos
                ? std::string::npos
                : nextDot - dot - 1U
        );
    }
    return lowerAscii(
        homeId + "-" + environment + "-" +
        (!homeTenant.empty() ? homeTenant : realm)
    );
}

AccountRecord buildAccountRecord(
    const MsalTokenResponseContext& context,
    const MsalSerializableTokenCache& cache,
    const std::string& homeId,
    const std::string& rawClientInfo,
    const JsRuntimeValue& claims
) {
    auto account = findBaseAccount(cache, homeId).value_or(
        createAccount(context, homeId, rawClientInfo, claims)
    );
    auto profiles = deserializeTenantProfiles(account);
    auto tenantId = claimsTenantId(claims);
    if (tenantId.empty()) tenantId = serializedField(account, "realm");
    const bool profileExists = std::any_of(
        profiles.begin(),
        profiles.end(),
        [&](const JsRuntimeValue& profileValue) {
            return propertyStringOrEmpty(profileValue, "tenantId") == tenantId;
        }
    );
    if (!tenantId.empty() && !profileExists) {
        profiles.push_back(buildTenantProfile(
            homeId,
            serializedField(account, "local_account_id"),
            tenantId,
            &claims
        ));
    }
    account.set("tenantProfiles", serializeTenantProfiles(profiles));
    return { account, accountCacheKey(account) };
}

std::string credentialKey(const JsRuntimeValue& credential) {
    const auto homeId = serializedField(credential, "home_account_id");
    const auto environment = serializedField(credential, "environment");
    const auto type = serializedField(credential, "credential_type");
    const auto client = serializedField(credential, "client_id");
    const auto family = serializedField(credential, "family_id");
    const auto realm = serializedField(credential, "realm");
    const auto target = serializedField(credential, "target");
    const auto claimsHash = serializedField(
        credential,
        "requestedClaimsHash"
    );
    const auto tokenType = serializedField(credential, "token_type");
    const auto clientOrFamily = type == "RefreshToken" && !family.empty()
        ? family
        : client;
    const auto scheme = !tokenType.empty() &&
            lowerAscii(tokenType) != "bearer"
        ? lowerAscii(tokenType)
        : std::string();
    return lowerAscii(
        homeId + "-" + environment + "-" + type + "-" +
        clientOrFamily + "-" + realm + "-" + target + "-" +
        claimsHash + "-" + scheme
    );
}

std::string appMetadataKey(const JsRuntimeValue& metadata) {
    return lowerAscii(
        "appmetadata-" + serializedField(metadata, "environment") + "-" +
        serializedField(metadata, "client_id")
    );
}

struct CacheRecord {
    std::optional<AccountRecord> account;
    std::optional<JsRuntimeValue> idToken;
    std::optional<JsRuntimeValue> accessToken;
    std::optional<JsRuntimeValue> refreshToken;
    std::optional<JsRuntimeValue> appMetadata;
    JsRuntimeValue idTokenClaims = JsRuntimeValue::undefined();
    std::vector<std::string> responseScopes;
    double expiresOnSeconds = 0.0;
    double extendedExpiresOnSeconds = 0.0;
    std::optional<double> refreshOnSeconds;
};

CacheRecord generateCacheRecord(
    const MsalTokenResponseContext& context,
    const JsRuntimeValue& response,
    const std::string& homeId,
    const JsRuntimeValue& claims,
    const MsalSerializableTokenCache& cache
) {
    CacheRecord record;
    record.idTokenClaims = claims;
    const auto claimTenant = claimsTenantId(claims);
    const auto rawClientInfo = propertyStringOrEmpty(response, "client_info");

    const auto* rawIdToken = property(response, "id_token");
    if (rawIdToken && rawIdToken->truthy() && claims.truthy()) {
        auto idToken = JsRuntimeValue::object();
        idToken.set("home_account_id", JsRuntimeValue::string(homeId));
        idToken.set(
            "environment",
            JsRuntimeValue::string(context.preferredCacheEnvironment)
        );
        idToken.set("credential_type", JsRuntimeValue::string("IdToken"));
        idToken.set("client_id", JsRuntimeValue::string(context.clientId));
        idToken.set("secret", JsRuntimeValue::string(jsToString(*rawIdToken)));
        idToken.set("realm", JsRuntimeValue::string(claimTenant));
        record.idToken = std::move(idToken);
        record.account = buildAccountRecord(
            context,
            cache,
            homeId,
            rawClientInfo,
            claims
        );
    }

    const auto* rawAccessToken = property(response, "access_token");
    if (rawAccessToken && rawAccessToken->truthy()) {
        const auto* rawScope = property(response, "scope");
        record.responseScopes = rawScope && rawScope->truthy()
            ? scopeSetFromString(jsToString(*rawScope))
            : scopeSetFromArray(context.normalizedRequestScopes);
        const auto expiresIn = numericResponseValue(
            response,
            "expires_in",
            0.0
        );
        const auto extendedExpiresIn = numericResponseValue(
            response,
            "ext_expires_in",
            0.0
        );
        const auto refreshIn = numericResponseValue(
            response,
            "refresh_in",
            std::numeric_limits<double>::quiet_NaN()
        );
        record.expiresOnSeconds = context.reqTimestampSeconds + expiresIn;
        record.extendedExpiresOnSeconds =
            record.expiresOnSeconds + extendedExpiresIn;
        if (!std::isnan(refreshIn) && refreshIn > 0.0) {
            record.refreshOnSeconds = context.reqTimestampSeconds + refreshIn;
        }

        const auto realm = !claimTenant.empty()
            ? claimTenant
            : context.authorityTenant;
        auto tokenType = propertyStringOrEmpty(response, "token_type");
        if (tokenType.empty()) tokenType = "Bearer";
        const bool bearer = lowerAscii(tokenType) == "bearer";

        auto accessToken = JsRuntimeValue::object();
        accessToken.set("home_account_id", JsRuntimeValue::string(homeId));
        accessToken.set(
            "environment",
            JsRuntimeValue::string(context.preferredCacheEnvironment)
        );
        accessToken.set(
            "credential_type",
            JsRuntimeValue::string(
                bearer ? "AccessToken" : "AccessToken_With_AuthScheme"
            )
        );
        accessToken.set("client_id", JsRuntimeValue::string(context.clientId));
        accessToken.set(
            "secret",
            JsRuntimeValue::string(jsToString(*rawAccessToken))
        );
        accessToken.set("realm", JsRuntimeValue::string(realm));
        accessToken.set(
            "target",
            JsRuntimeValue::string(joinScopes(record.responseScopes))
        );
        accessToken.set(
            "cached_at",
            JsRuntimeValue::string(numberToString(
                context.nowSecondsCallback
                    ? context.nowSecondsCallback()
                    : context.nowSeconds
            ))
        );
        accessToken.set(
            "expires_on",
            JsRuntimeValue::string(numberToString(record.expiresOnSeconds))
        );
        accessToken.set(
            "extended_expires_on",
            JsRuntimeValue::string(
                numberToString(record.extendedExpiresOnSeconds)
            )
        );
        accessToken.set(
            "refresh_on",
            record.refreshOnSeconds
                ? JsRuntimeValue::string(numberToString(*record.refreshOnSeconds))
                : JsRuntimeValue::undefined()
        );
        accessToken.set("key_id", JsRuntimeValue::undefined());
        accessToken.set("token_type", JsRuntimeValue::string(tokenType));
        accessToken.set("requestedClaims", JsRuntimeValue::undefined());
        accessToken.set("requestedClaimsHash", JsRuntimeValue::undefined());
        accessToken.set("userAssertionHash", JsRuntimeValue::undefined());
        record.accessToken = std::move(accessToken);
    }

    const auto* rawRefreshToken = property(response, "refresh_token");
    if (rawRefreshToken && rawRefreshToken->truthy()) {
        // createRefreshTokenEntity computes expiresOn from
        // refresh_token_expires_in, but msal-node's Serializer omits it.
        (void) numericResponseValue(
            response,
            "refresh_token_expires_in",
            0.0
        );
        const auto family = propertyStringOrEmpty(response, "foci");
        auto refreshToken = JsRuntimeValue::object();
        refreshToken.set("home_account_id", JsRuntimeValue::string(homeId));
        refreshToken.set(
            "environment",
            JsRuntimeValue::string(context.preferredCacheEnvironment)
        );
        refreshToken.set(
            "credential_type",
            JsRuntimeValue::string("RefreshToken")
        );
        refreshToken.set("client_id", JsRuntimeValue::string(context.clientId));
        refreshToken.set(
            "secret",
            JsRuntimeValue::string(jsToString(*rawRefreshToken))
        );
        refreshToken.set(
            "family_id",
            family.empty()
                ? JsRuntimeValue::undefined()
                : JsRuntimeValue::string(family)
        );
        refreshToken.set("target", JsRuntimeValue::undefined());
        refreshToken.set("realm", JsRuntimeValue::undefined());
        record.refreshToken = std::move(refreshToken);
    }

    const auto family = propertyStringOrEmpty(response, "foci");
    if (!family.empty()) {
        auto metadata = JsRuntimeValue::object();
        metadata.set("client_id", JsRuntimeValue::string(context.clientId));
        metadata.set(
            "environment",
            JsRuntimeValue::string(context.preferredCacheEnvironment)
        );
        metadata.set("family_id", JsRuntimeValue::string(family));
        record.appMetadata = std::move(metadata);
    }
    return record;
}

bool environmentMatches(std::string left, std::string right) {
    left = lowerAscii(std::move(left));
    right = lowerAscii(std::move(right));
    if (left == right) return true;
    static const std::unordered_set<std::string> publicCloudAliases {
        "login.microsoftonline.com",
        "login.windows.net",
        "login.microsoft.com",
        "sts.windows.net"
    };
    return publicCloudAliases.contains(left) &&
        publicCloudAliases.contains(right);
}

bool containsScopeCaseInsensitive(
    const std::vector<std::string>& scopes,
    std::string_view wanted
) {
    const auto lowerWanted = lowerAscii(std::string(wanted));
    return std::any_of(
        scopes.begin(),
        scopes.end(),
        [&](const std::string& scope) {
            return lowerAscii(scope) == lowerWanted;
        }
    );
}

bool containsOnlyOidcScopes(const std::vector<std::string>& scopes) {
    std::size_t count = 0;
    for (const auto scope : kOidcScopes) {
        if (containsScopeCaseInsensitive(scopes, scope)) ++count;
    }
    const bool hasEmail = containsScopeCaseInsensitive(scopes, "email");
    if (hasEmail) ++count;
    return count == scopes.size();
}

std::vector<std::string> currentScopesForIntersection(
    std::vector<std::string> scopes
) {
    if (!containsOnlyOidcScopes(scopes)) {
        for (const auto oidc : kOidcScopes) {
            scopes.erase(
                std::remove(scopes.begin(), scopes.end(), std::string(oidc)),
                scopes.end()
            );
        }
        scopes.erase(
            std::remove(scopes.begin(), scopes.end(), "email"),
            scopes.end()
        );
    }
    return scopes;
}

bool intersectingScopes(
    const std::vector<std::string>& left,
    const std::vector<std::string>& right
) {
    std::unordered_set<std::string> unionScopes;
    for (const auto& scope : right) {
        unionScopes.insert(lowerAscii(scope));
    }
    for (const auto& scope : left) {
        unionScopes.insert(lowerAscii(scope));
    }
    return unionScopes.size() < left.size() + right.size();
}

bool accessTokenMatchesForReplacement(
    const JsRuntimeValue& oldToken,
    const JsRuntimeValue& newToken,
    const std::vector<std::string>& currentScopes
) {
    if (!oldToken.isObject()) return false;
    const auto newClient = serializedField(newToken, "client_id");
    const auto newHome = serializedField(newToken, "home_account_id");
    const auto newEnvironment = serializedField(newToken, "environment");
    const auto newRealm = serializedField(newToken, "realm");
    const auto newCredential = serializedField(newToken, "credential_type");
    const auto newClaimsHash = serializedField(
        newToken,
        "requestedClaimsHash"
    );
    const auto oldClaimsHash = serializedField(
        oldToken,
        "requestedClaimsHash"
    );

    if (serializedField(oldToken, "client_id") != newClient ||
        serializedField(oldToken, "home_account_id") != newHome ||
        !environmentMatches(
            serializedField(oldToken, "environment"),
            newEnvironment
        ) ||
        lowerAscii(serializedField(oldToken, "realm")) !=
            lowerAscii(newRealm) ||
        lowerAscii(serializedField(oldToken, "credential_type")) !=
            lowerAscii(newCredential) ||
        oldClaimsHash != newClaimsHash) {
        return false;
    }

    const auto oldScopes = scopeSetFromString(
        serializedField(oldToken, "target")
    );
    return intersectingScopes(oldScopes, currentScopes);
}

void saveCacheRecord(
    MsalSerializableTokenCache& cache,
    const CacheRecord& record
) {
    if (record.account) {
        cache.setEntity(
            EntityMap::Account,
            record.account->key,
            record.account->serialized
        );
    }
    if (record.idToken) {
        cache.setEntity(
            EntityMap::IdToken,
            credentialKey(*record.idToken),
            *record.idToken
        );
    }
    if (record.accessToken) {
        const auto currentScopes = currentScopesForIntersection(
            record.responseScopes
        );
        const auto existing = cache.entityMap(EntityMap::AccessToken);
        std::vector<std::string> removals;
        const auto newKeyLower = lowerAscii(credentialKey(*record.accessToken));
        const auto newClient = lowerAscii(serializedField(
            *record.accessToken,
            "client_id"
        ));
        const auto newHome = lowerAscii(serializedField(
            *record.accessToken,
            "home_account_id"
        ));
        const auto newRealm = lowerAscii(serializedField(
            *record.accessToken,
            "realm"
        ));
        for (const auto& item : existing.ownProperties()) {
            const auto key = lowerAscii(item.key);
            if (key.find(newClient) == std::string::npos ||
                key.find(newHome) == std::string::npos ||
                key.find(newRealm) == std::string::npos) {
                continue;
            }
            if (accessTokenMatchesForReplacement(
                    item.value,
                    *record.accessToken,
                    currentScopes
                )) {
                removals.push_back(item.key);
            }
        }
        for (const auto& key : removals) {
            cache.removeEntity(EntityMap::AccessToken, key);
        }
        cache.setEntity(
            EntityMap::AccessToken,
            newKeyLower,
            *record.accessToken
        );
    }
    if (record.refreshToken) {
        cache.setEntity(
            EntityMap::RefreshToken,
            credentialKey(*record.refreshToken),
            *record.refreshToken
        );
    }
    if (record.appMetadata) {
        cache.setEntity(
            EntityMap::AppMetadata,
            appMetadataKey(*record.appMetadata),
            *record.appMetadata
        );
    }
}

JsRuntimeValue accountInfo(
    const AccountRecord& accountRecord,
    const JsRuntimeValue& claims,
    const JsRuntimeValue& idToken,
    const JsRuntimeValue* nativeAccountId
) {
    const auto& account = accountRecord.serialized;
    auto info = JsRuntimeValue::object();
    info.set(
        "homeAccountId",
        propertyValueOrUndefined(account, "home_account_id")
    );
    info.set(
        "environment",
        propertyValueOrUndefined(account, "environment")
    );
    info.set("tenantId", propertyValueOrUndefined(account, "realm"));
    info.set("username", propertyValueOrUndefined(account, "username"));
    info.set(
        "localAccountId",
        propertyValueOrUndefined(account, "local_account_id")
    );
    info.set("name", propertyValueOrUndefined(account, "name"));
    info.set(
        "nativeAccountId",
        nativeAccountId ? *nativeAccountId : JsRuntimeValue::undefined()
    );
    info.set(
        "authorityType",
        propertyValueOrUndefined(account, "authority_type")
    );

    auto tenantProfileMap = JsRuntimeValue::map();
    for (const auto& profileValue : deserializeTenantProfiles(account)) {
        const auto key = propertyValueOrUndefined(profileValue, "tenantId");
        tenantProfileMap.mapSet(key, profileValue);
    }
    info.set("tenantProfiles", std::move(tenantProfileMap));

    if (claims.truthy()) {
        const auto claimsProfile = buildTenantProfile(
            serializedField(account, "home_account_id"),
            serializedField(account, "local_account_id"),
            serializedField(account, "realm"),
            &claims
        );
        info.set(
            "tenantId",
            propertyValueOrUndefined(claimsProfile, "tenantId")
        );
        info.set(
            "localAccountId",
            propertyValueOrUndefined(claimsProfile, "localAccountId")
        );
        info.set("name", propertyValueOrUndefined(claimsProfile, "name"));
        info.set("idTokenClaims", claims);
        info.set("idToken", idToken);
    }
    return info;
}

JsRuntimeValue buildAuthenticationResult(
    const MsalTokenResponseContext& context,
    const CacheRecord& record,
    const JsRuntimeValue& serverResponse,
    bool includeServerResponse
) {
    auto responseScopes = JsRuntimeValue::array();
    for (const auto& scope : record.responseScopes) {
        responseScopes.push(JsRuntimeValue::string(scope));
    }

    const auto idToken = record.idToken
        ? propertyValueOrUndefined(*record.idToken, "secret")
        : JsRuntimeValue::string("");
    const auto accessToken = record.accessToken
        ? propertyValueOrUndefined(*record.accessToken, "secret")
        : JsRuntimeValue::string("");

    const JsRuntimeValue* nativeAccountId = nullptr;
    if (includeServerResponse && record.account) {
        const auto* spaAccountId = property(serverResponse, "spa_accountid");
        if (spaAccountId && spaAccountId->truthy()) {
            nativeAccountId = spaAccountId;
        }
    }

    JsRuntimeValue account = JsRuntimeValue::null();
    if (record.account) {
        account = accountInfo(
            *record.account,
            record.idTokenClaims,
            idToken,
            nativeAccountId
        );
    }

    auto uniqueId = claimString(record.idTokenClaims, "oid");
    if (uniqueId.empty()) uniqueId = claimString(record.idTokenClaims, "sub");
    const auto tenantId = claimString(record.idTokenClaims, "tid");
    std::string familyId;
    if (record.appMetadata &&
        serializedField(*record.appMetadata, "family_id") == "1") {
        familyId = "1";
    }

    auto result = JsRuntimeValue::object();
    result.set("authority", JsRuntimeValue::string(context.canonicalAuthority));
    result.set("uniqueId", JsRuntimeValue::string(uniqueId));
    result.set("tenantId", JsRuntimeValue::string(tenantId));
    result.set("scopes", std::move(responseScopes));
    result.set("account", std::move(account));
    result.set("idToken", idToken);
    result.set(
        "idTokenClaims",
        record.idTokenClaims.truthy()
            ? record.idTokenClaims
            : JsRuntimeValue::object()
    );
    result.set("accessToken", accessToken);
    result.set("fromCache", JsRuntimeValue::boolean(false));
    result.set(
        "expiresOn",
        record.accessToken
            ? JsRuntimeValue::date(record.expiresOnSeconds * 1000.0)
            : JsRuntimeValue::null()
    );
    result.set(
        "extExpiresOn",
        record.accessToken
            ? JsRuntimeValue::date(record.extendedExpiresOnSeconds * 1000.0)
            : JsRuntimeValue::undefined()
    );
    result.set(
        "refreshOn",
        record.refreshOnSeconds
            ? JsRuntimeValue::date(*record.refreshOnSeconds * 1000.0)
            : JsRuntimeValue::undefined()
    );
    result.set(
        "correlationId",
        context.authenticationResultCorrelationId
            ? *context.authenticationResultCorrelationId
            : JsRuntimeValue::string(context.correlationId)
    );
    result.set("requestId", JsRuntimeValue::string(context.requestId));
    result.set("familyId", JsRuntimeValue::string(familyId));
    result.set(
        "tokenType",
        record.accessToken
            ? propertyValueOrUndefined(*record.accessToken, "token_type")
            : JsRuntimeValue::string("")
    );
    result.set("state", JsRuntimeValue::string(""));
    result.set("cloudGraphHostName", JsRuntimeValue::string(""));
    result.set("msGraphHost", JsRuntimeValue::string(""));
    result.set(
        "code",
        includeServerResponse
            ? propertyValueOrUndefined(serverResponse, "spa_code")
            : JsRuntimeValue::undefined()
    );
    result.set("fromNativeBroker", JsRuntimeValue::boolean(false));
    return result;
}

bool interactionRequired(
    const std::string& errorCode,
    const std::string& errorDescription,
    const std::string& subError
) {
    static const std::array<std::string_view, 4> errorCodes {
        "interaction_required",
        "consent_required",
        "login_required",
        "bad_token"
    };
    static const std::array<std::string_view, 6> subErrors {
        "message_only",
        "additional_action",
        "basic_action",
        "user_password_expired",
        "consent_required",
        "bad_token"
    };
    if (std::find(errorCodes.begin(), errorCodes.end(), errorCode) !=
        errorCodes.end()) {
        return true;
    }
    if (std::find(subErrors.begin(), subErrors.end(), subError) !=
        subErrors.end()) {
        return true;
    }
    return std::any_of(
        errorCodes.begin(),
        errorCodes.end(),
        [&](std::string_view code) {
            return errorDescription.find(code) != std::string::npos;
        }
    );
}

JsRuntimeValue firstErrorNumber(const JsRuntimeValue& response) {
    const auto* codes = property(response, "error_codes");
    if (!codes || !codes->isArray() || codes->length() == 0U) {
        return JsRuntimeValue::undefined();
    }
    const auto* first = codes->get(0U);
    return first ? *first : JsRuntimeValue::undefined();
}

JsRuntimeValue responseStatus(const JsRuntimeValue& response) {
    const auto* status = property(response, "status");
    return status ? *status : JsRuntimeValue::undefined();
}

} // namespace

struct MsalPreparedTokenResponse::State {
    MsalTokenResponseContext context;
    JsRuntimeValue serverResponse = JsRuntimeValue::undefined();
    CacheRecord cacheRecord;
};

MsalInteractionRequiredAuthError::MsalInteractionRequiredAuthError(
    std::string errorCode,
    std::string errorMessage,
    std::string subError,
    std::string timestamp,
    std::string traceId,
    std::string correlationId,
    std::string claims,
    JsRuntimeValue errorNo
) : MsalAuthError(
        std::move(errorCode),
        std::move(errorMessage),
        std::move(subError),
        "InteractionRequiredAuthError"
    ),
    timestamp_(std::move(timestamp)),
    traceId_(std::move(traceId)),
    claims_(std::move(claims)),
    errorNo_(std::move(errorNo)) {
    // The subclass assignments occur in this order. `name` was inserted by
    // AuthError and retains that earlier property position when overwritten.
    setEnumerable("timestamp", JsRuntimeValue::string(timestamp_));
    setEnumerable("traceId", JsRuntimeValue::string(traceId_));
    setCorrelationId(std::move(correlationId));
    setEnumerable("claims", JsRuntimeValue::string(claims_));
    setEnumerable("errorNo", errorNo_);
}

MsalPreparedTokenResponse::MsalPreparedTokenResponse(
    std::shared_ptr<const State> state
) : state_(std::move(state)) {}

bool MsalPreparedTokenResponse::valid() const noexcept {
    return static_cast<bool>(state_);
}

MsalTokenResponseHandler::MsalTokenResponseHandler(
    MsalTokenResponseContext context
) : context_(std::move(context)) {}

void MsalTokenResponseHandler::validateTokenResponse(
    const JsRuntimeValue& serverResponse
) const {
    const auto errorCode = propertyStringOrEmpty(serverResponse, "error");
    const auto errorDescription = propertyStringOrEmpty(
        serverResponse,
        "error_description"
    );
    const auto subError = propertyStringOrEmpty(serverResponse, "suberror");
    if (errorCode.empty() && errorDescription.empty() && subError.empty()) {
        return;
    }

    const auto* errorCodes = property(serverResponse, "error_codes");
    const auto timestamp = propertyStringOrEmpty(serverResponse, "timestamp");
    const auto correlation = propertyStringOrEmpty(
        serverResponse,
        "correlation_id"
    );
    const auto trace = propertyStringOrEmpty(serverResponse, "trace_id");
    const std::string diagnostic =
        "Error(s): " +
        (errorCodes && errorCodes->truthy()
            ? jsToString(*errorCodes)
            : "Not Available") +
        " - Timestamp: " +
        (timestamp.empty() ? "Not Available" : timestamp) +
        " - Description: " +
        (errorDescription.empty() ? "Not Available" : errorDescription) +
        " - Correlation ID: " +
        (correlation.empty() ? "Not Available" : correlation) +
        " - Trace ID: " +
        (trace.empty() ? "Not Available" : trace);

    if (interactionRequired(
        errorCode,
        errorDescription,
        subError
    )) {
        throw MsalInteractionRequiredAuthError(
            errorCode,
            errorDescription,
            subError,
            timestamp,
            trace,
            correlation,
            propertyStringOrEmpty(serverResponse, "claims"),
            firstErrorNumber(serverResponse)
        );
    }

    throw MsalServerError(
        errorCode,
        diagnostic,
        subError,
        firstErrorNumber(serverResponse),
        responseStatus(serverResponse)
    );
}

MsalPreparedTokenResponse
MsalTokenResponseHandler::prepareServerTokenResponse(
    const JsRuntimeValue& serverResponse,
    const MsalSerializableTokenCache& tokenCacheBeforeHook
) const {
    validateTokenResponse(serverResponse);

    JsRuntimeValue claims = JsRuntimeValue::undefined();
    const auto* rawIdToken = property(serverResponse, "id_token");
    if (rawIdToken && rawIdToken->truthy()) {
        claims = extractTokenClaims(jsToString(*rawIdToken));
    }

    const auto rawClientInfo = propertyStringOrEmpty(
        serverResponse,
        "client_info"
    );
    const auto homeId = homeAccountId(rawClientInfo, claims);
    auto normalizedServerResponse = serverResponse;
    const auto* responseKeyId = property(
        normalizedServerResponse,
        "key_id"
    );
    if (!responseKeyId || !responseKeyId->truthy()) {
        normalizedServerResponse.set(
            "key_id",
            context_.sshKid.truthy()
                ? context_.sshKid
                : JsRuntimeValue::undefined()
        );
    }
    auto state = std::make_shared<MsalPreparedTokenResponse::State>();
    state->context = context_;
    state->serverResponse = normalizedServerResponse;
    state->cacheRecord = generateCacheRecord(
        context_,
        normalizedServerResponse,
        homeId,
        claims,
        tokenCacheBeforeHook
    );
    return MsalPreparedTokenResponse(std::move(state));
}

bool MsalTokenResponseHandler::savePreparedTokenResponse(
    const MsalPreparedTokenResponse& prepared,
    MsalSerializableTokenCache& tokenCacheAfterHook
) const {
    if (!prepared.state_) {
        throw MsalClientAuthError(
            "invalid_cache_record",
            "Cache record object was null or undefined."
        );
    }
    const auto& state = *prepared.state_;
    const auto& record = state.cacheRecord;

    if (state.context.handlingRefreshTokenResponse &&
        !state.context.forceCacheRefreshTokenResponse && record.account &&
        !tokenCacheAfterHook.hasEntity(
            EntityMap::Account,
            record.account->key
        )) {
        return false;
    }

    saveCacheRecord(tokenCacheAfterHook, record);
    return true;
}

JsRuntimeValue MsalTokenResponseHandler::generateAuthenticationResult(
    const MsalPreparedTokenResponse& prepared,
    bool includeServerTokenResponse
) const {
    if (!prepared.state_) {
        throw MsalClientAuthError(
            "invalid_cache_record",
            "Cache record object was null or undefined."
        );
    }
    const auto& state = *prepared.state_;
    return buildAuthenticationResult(
        state.context,
        state.cacheRecord,
        state.serverResponse,
        includeServerTokenResponse
    );
}

JsRuntimeValue MsalTokenResponseHandler::handleServerTokenResponse(
    const JsRuntimeValue& serverResponse,
    MsalSerializableTokenCache& tokenCache
) const {
    const auto prepared = prepareServerTokenResponse(
        serverResponse,
        tokenCache
    );
    const bool saved = savePreparedTokenResponse(prepared, tokenCache);
    return generateAuthenticationResult(prepared, saved);
}

std::string MsalTokenResponseHandler::generateAccountCacheKey(
    const JsRuntimeValue& serializedAccount
) {
    return accountCacheKey(serializedAccount);
}

std::string MsalTokenResponseHandler::generateCredentialKey(
    const JsRuntimeValue& serializedCredential
) {
    return credentialKey(serializedCredential);
}

std::string MsalTokenResponseHandler::generateAppMetadataKey(
    const JsRuntimeValue& serializedAppMetadata
) {
    return appMetadataKey(serializedAppMetadata);
}

} // namespace bedrock
