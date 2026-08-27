#include <bedrock/auth/MinecraftBedrockServicesManager.hpp>
#include <bedrock/auth/MinecraftBedrockTokenManager.hpp>
#include <bedrock/auth/PlayfabTokenManager.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bedrock {

struct MinecraftBedrockTokenManagerState {
    explicit MinecraftBedrockTokenManagerState(AuthCachePtr cacheValue)
        : cache(std::move(cacheValue)) {}

    AuthCachePtr cache;
    JsRuntimeValue forceRefresh = JsRuntimeValue::undefined();
    XboxTokenHttpClientPtr httpClient;
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue;
    std::function<double()> dateNowMilliseconds;
    BedrockTokenManagerObservers observers;
};

struct MinecraftBedrockServicesTokenManagerState {
    explicit MinecraftBedrockServicesTokenManagerState(AuthCachePtr cacheValue)
        : cache(std::move(cacheValue)) {}

    AuthCachePtr cache;
    XboxTokenHttpClientPtr httpClient;
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue;
    std::function<double()> dateNowMilliseconds;
    BedrockTokenManagerObservers observers;
};

struct PlayfabTokenManagerState {
    explicit PlayfabTokenManagerState(AuthCachePtr cacheValue)
        : cache(std::move(cacheValue)) {}

    AuthCachePtr cache;
    XboxTokenHttpClientPtr httpClient;
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue;
    std::function<double()> dateNowMilliseconds;
    BedrockTokenManagerObservers observers;
};

namespace {

using ValuePromise = JsPromise<JsRuntimeValue>;

std::string jsNumberToString(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    return JsRuntimeJson::stringify(
        JsRuntimeValue::number(value)
    ).value_or("undefined");
}

std::string jsToString(const JsRuntimeValue& value);

std::string arrayToString(const JsRuntimeValue& value) {
    std::string result;
    for (std::size_t index = 0; index < value.length(); ++index) {
        if (index) result.push_back(',');
        const auto* item = value.get(index);
        if (!item || item->isUndefined() || item->isNull()) continue;
        result += jsToString(*item);
    }
    return result;
}

std::string jsToString(const JsRuntimeValue& value) {
    if (value.isUndefined()) return "undefined";
    if (value.isNull()) return "null";
    if (value.isBool()) return value.boolValue() ? "true" : "false";
    if (value.isNumber()) return jsNumberToString(value.numberValue());
    if (value.isString()) return value.stringValue();
    if (value.isArray()) return arrayToString(value);
    if (value.isDate()) {
        return value.dateIsValid() ? value.dateIsoString() : "Invalid Date";
    }
    if (value.isMap()) return "[object Map]";
    if (value.isFunction()) return "function () { [native code] }";
    return "[object Object]";
}

std::string trimAsciiWhitespace(std::string value) {
    const auto whitespace = [](unsigned char byte) {
        return byte == ' ' || byte == '\t' || byte == '\n' ||
            byte == '\r' || byte == '\f' || byte == '\v';
    };
    std::size_t begin = 0;
    while (begin < value.size() && whitespace(value[begin])) ++begin;
    std::size_t end = value.size();
    while (end > begin && whitespace(value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

double stringToNumber(std::string value) {
    value = trimAsciiWhitespace(std::move(value));
    if (value.empty()) return 0.0;
    if (value == "Infinity" || value == "+Infinity") {
        return std::numeric_limits<double>::infinity();
    }
    if (value == "-Infinity") {
        return -std::numeric_limits<double>::infinity();
    }
    char* end = nullptr;
    const double result = std::strtod(value.c_str(), &end);
    if (!end || end != value.c_str() + value.size()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return result;
}

double toNumber(const JsRuntimeValue& value) {
    if (value.isUndefined()) return std::numeric_limits<double>::quiet_NaN();
    if (value.isNull()) return 0.0;
    if (value.isBool()) return value.boolValue() ? 1.0 : 0.0;
    if (value.isNumber()) return value.numberValue();
    if (value.isString()) return stringToNumber(value.stringValue());
    if (value.isArray()) return stringToNumber(arrayToString(value));
    if (value.isDate()) return value.dateMilliseconds();
    return std::numeric_limits<double>::quiet_NaN();
}

JsRuntimeValue getProperty(
    const JsRuntimeValue& value,
    std::string_view property
) {
    if (value.isUndefined() || value.isNull()) {
        throw std::runtime_error(
            std::string("Cannot read properties of ") +
            (value.isNull() ? "null" : "undefined") + " (reading '" +
            std::string(property) + "')"
        );
    }
    const auto* found = value.get(property);
    return found ? *found : JsRuntimeValue::undefined();
}

JsRuntimeValue getIndex(const JsRuntimeValue& value, std::size_t index) {
    if (value.isUndefined() || value.isNull()) {
        throw std::runtime_error(
            std::string("Cannot read properties of ") +
            (value.isNull() ? "null" : "undefined") + " (reading '" +
            std::to_string(index) + "')"
        );
    }
    if (value.isArray()) {
        const auto* found = value.get(index);
        return found ? *found : JsRuntimeValue::undefined();
    }
    return getProperty(value, std::to_string(index));
}

JsRuntimeValue destructuredProperty(
    const JsRuntimeValue& value,
    std::string_view property
) {
    if (value.isUndefined() || value.isNull()) {
        throw std::runtime_error(
            "Cannot destructure property '" + std::string(property) +
            "' of '(intermediate value)' as it is " +
            (value.isNull() ? std::string("null.") : std::string("undefined."))
        );
    }
    return getProperty(value, property);
}

std::int64_t daysFromCivil(int year, unsigned month, unsigned day) noexcept {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned monthPrime = month > 2 ? month - 3 : month + 9;
    const auto dayOfYear =
        (153U * monthPrime + 2U) / 5U + day - 1U;
    const auto dayOfEra = yearOfEra * 365U + yearOfEra / 4U -
        yearOfEra / 100U + dayOfYear;
    return static_cast<std::int64_t>(era) * 146097 +
        static_cast<std::int64_t>(dayOfEra) - 719468;
}

bool leapYear(int year) noexcept {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

unsigned daysInMonth(int year, unsigned month) noexcept {
    static constexpr std::array<unsigned, 12> days {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month == 2 && leapYear(year)) return 29;
    return month >= 1 && month <= 12 ? days[month - 1] : 0;
}

bool parseDigits(
    std::string_view value,
    std::size_t& offset,
    std::size_t count,
    int& result
) noexcept {
    if (offset + count > value.size()) return false;
    result = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char byte = value[offset + index];
        if (byte < '0' || byte > '9') return false;
        result = result * 10 + (byte - '0');
    }
    offset += count;
    return true;
}

std::optional<double> parseIsoDate(std::string_view value) {
    std::size_t offset = 0;
    int year = 0;
    int month = 0;
    int day = 0;
    if (!parseDigits(value, offset, 4, year) ||
        offset >= value.size() || value[offset++] != '-' ||
        !parseDigits(value, offset, 2, month) ||
        offset >= value.size() || value[offset++] != '-' ||
        !parseDigits(value, offset, 2, day)) {
        return std::nullopt;
    }
    if (month < 1 || month > 12 || day < 1 ||
        day > static_cast<int>(daysInMonth(
            year,
            static_cast<unsigned>(month)
        ))) {
        return std::nullopt;
    }

    int hour = 0;
    int minute = 0;
    int second = 0;
    int milliseconds = 0;
    int timezoneMinutes = 0;
    if (offset != value.size()) {
        if (value[offset] != 'T' && value[offset] != 't' &&
            value[offset] != ' ') {
            return std::nullopt;
        }
        ++offset;
        if (!parseDigits(value, offset, 2, hour) ||
            offset >= value.size() || value[offset++] != ':' ||
            !parseDigits(value, offset, 2, minute) ||
            offset >= value.size() || value[offset++] != ':' ||
            !parseDigits(value, offset, 2, second) ||
            hour > 23 || minute > 59 || second > 59) {
            return std::nullopt;
        }
        if (offset < value.size() && value[offset] == '.') {
            ++offset;
            int digits = 0;
            while (offset < value.size() && value[offset] >= '0' &&
                value[offset] <= '9') {
                if (digits < 3) {
                    milliseconds = milliseconds * 10 +
                        (value[offset] - '0');
                }
                ++digits;
                ++offset;
            }
            if (digits == 0) return std::nullopt;
            while (digits < 3) {
                milliseconds *= 10;
                ++digits;
            }
        }
        if (offset >= value.size()) return std::nullopt;
        if (value[offset] == 'Z' || value[offset] == 'z') {
            ++offset;
        } else if (value[offset] == '+' || value[offset] == '-') {
            const bool negative = value[offset++] == '-';
            int zoneHour = 0;
            int zoneMinute = 0;
            if (!parseDigits(value, offset, 2, zoneHour) ||
                offset >= value.size() || value[offset++] != ':' ||
                !parseDigits(value, offset, 2, zoneMinute) ||
                zoneHour > 23 || zoneMinute > 59) {
                return std::nullopt;
            }
            timezoneMinutes = zoneHour * 60 + zoneMinute;
            if (negative) timezoneMinutes = -timezoneMinutes;
        } else {
            return std::nullopt;
        }
    }
    if (offset != value.size()) return std::nullopt;

    constexpr std::int64_t millisecondsPerDay = 86400000;
    const auto days = daysFromCivil(
        year,
        static_cast<unsigned>(month),
        static_cast<unsigned>(day)
    );
    const auto withinDay = static_cast<std::int64_t>(hour) * 3600000 +
        static_cast<std::int64_t>(minute) * 60000 +
        static_cast<std::int64_t>(second) * 1000 + milliseconds;
    return static_cast<double>(
        days * millisecondsPerDay + withinDay -
        static_cast<std::int64_t>(timezoneMinutes) * 60000
    );
}

double newDateMilliseconds(const JsRuntimeValue& value) {
    if (value.isDate()) return value.dateMilliseconds();
    if (value.isUndefined()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (value.isNull()) return 0.0;
    if (value.isBool()) return value.boolValue() ? 1.0 : 0.0;
    if (value.isNumber()) {
        return JsRuntimeValue::date(value.numberValue()).dateMilliseconds();
    }
    return parseIsoDate(jsToString(value)).value_or(
        std::numeric_limits<double>::quiet_NaN()
    );
}

std::vector<std::uint8_t> decodeBase64(std::string value) {
    value.erase(
        std::remove_if(value.begin(), value.end(), [](unsigned char byte) {
            return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
        }),
        value.end()
    );
    std::replace(value.begin(), value.end(), '-', '+');
    std::replace(value.begin(), value.end(), '_', '/');
    while (value.size() % 4 != 0) value.push_back('=');
    if (value.empty()) return {};

    std::vector<std::uint8_t> decoded((value.size() / 4) * 3);
    const int length = EVP_DecodeBlock(
        decoded.data(),
        reinterpret_cast<const unsigned char*>(value.data()),
        static_cast<int>(value.size())
    );
    if (length < 0) throw std::runtime_error("Invalid base64 JWT segment");
    std::size_t resultLength = static_cast<std::size_t>(length);
    if (!value.empty() && value.back() == '=') --resultLength;
    if (value.size() >= 2 && value[value.size() - 2] == '=') --resultLength;
    decoded.resize(resultLength);
    return decoded;
}

JsRuntimeValue decodeJwtPayload(const JsRuntimeValue& jwtValue) {
    if (!jwtValue.isString()) {
        throw std::runtime_error("jwt.split is not a function");
    }
    const auto& jwt = jwtValue.stringValue();
    const auto first = jwt.find('.');
    const auto second = first == std::string::npos
        ? std::string::npos
        : jwt.find('.', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
        throw std::runtime_error("Invalid cached Bedrock JWT");
    }
    const auto bytes = decodeBase64(jwt.substr(first + 1, second - first - 1));
    return JsRuntimeJson::parse(std::string(
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size()
    ));
}

JsRuntimeValue shallowSpread(const JsRuntimeValue& source) {
    auto result = JsRuntimeValue::object();
    if (source.isUndefined() || source.isNull()) return result;
    for (const auto& property : source.ownProperties()) {
        result.set(property.key, property.value);
    }
    return result;
}

std::string stringifyRequired(const JsRuntimeValue& value) {
    return JsRuntimeJson::stringify(value).value_or("undefined");
}

JsRuntimeValue jsonHeaders(
    std::initializer_list<JsRuntimeProperty> additional = {}
) {
    auto headers = JsRuntimeValue::object({
        {"Content-Type", JsRuntimeValue::string("application/json")}
    });
    for (const auto& item : additional) {
        headers.set(item.key, item.value);
    }
    return headers;
}

std::vector<std::pair<std::string, std::string>> headersFromObject(
    const JsRuntimeValue& value
) {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& property : value.ownProperties()) {
        result.emplace_back(property.key, jsToString(property.value));
    }
    return result;
}

XboxTokenHttpRequest makeJsonRequest(
    std::string url,
    JsRuntimeValue headers,
    JsRuntimeValue body
) {
    XboxTokenHttpRequest request;
    request.method = "post";
    request.url = std::move(url);
    request.headersObject = std::move(headers);
    request.headers = headersFromObject(request.headersObject);
    request.body = stringifyRequired(body);
    return request;
}

JsRuntimeValue checkStatus(const XboxTokenHttpResponse& response) {
    if (response.ok()) return JsRuntimeJson::parse(response.bodyText);
    throw std::runtime_error(
        std::to_string(response.status) + " " + response.statusText + " " +
        response.bodyText
    );
}

double systemDateNowMilliseconds() {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

template <typename State>
void initializeDependencies(
    const std::shared_ptr<State>& state,
    BedrockTokenManagerDependencies dependencies
) {
    if (!dependencies.microtaskQueue) {
        dependencies.microtaskQueue = JsMicrotaskQueue::create();
    }
    state->microtaskQueue = std::move(dependencies.microtaskQueue);
    state->dateNowMilliseconds = dependencies.dateNowMilliseconds
        ? std::move(dependencies.dateNowMilliseconds)
        : systemDateNowMilliseconds;
    state->httpClient = dependencies.httpClient
        ? std::move(dependencies.httpClient)
        : std::make_shared<CurlXboxTokenHttpClient>(state->microtaskQueue);
    state->observers = std::move(dependencies.observers);
}

template <typename State>
void debug(
    const std::shared_ptr<State>& state,
    const std::string& label,
    std::vector<JsRuntimeValue> arguments = {}
) {
    if (state->observers.debug) {
        state->observers.debug(label, arguments);
    }
}

template <typename State>
AuthCache::GetCachedMethod requireGetCached(
    const std::shared_ptr<State>& state
) {
    if (!state->cache) {
        throw std::runtime_error(
            "Cannot read properties of null (reading 'getCached')"
        );
    }
    if (!state->cache->hasGetCachedMethod()) {
        throw std::runtime_error("this.cache.getCached is not a function");
    }
    return state->cache->requireGetCachedMethod();
}

template <typename State>
AuthCache::SetCachedPartialMethod requireSetCachedPartial(
    const std::shared_ptr<State>& state
) {
    if (!state->cache) {
        throw std::runtime_error(
            "Cannot read properties of null (reading 'setCachedPartial')"
        );
    }
    if (!state->cache->hasSetCachedPartialMethod()) {
        throw std::runtime_error(
            "this.cache.setCachedPartial is not a function"
        );
    }
    return state->cache->requireSetCachedPartialMethod();
}

template <typename State>
ValuePromise getCached(const std::shared_ptr<State>& state) {
    return ValuePromise::fromSynchronous(
        state->microtaskQueue,
        [state] {
            auto method = requireGetCached(state);
            return method();
        }
    );
}

template <typename State>
JsPromise<void> setCachedPartial(
    const std::shared_ptr<State>& state,
    JsRuntimeValue data
) {
    return JsPromise<void>::fromSynchronous(
        state->microtaskQueue,
        [state, data = std::move(data)]() mutable {
            auto method = requireSetCachedPartial(state);
            return method(std::move(data));
        }
    );
}

JsPromise<void> setMinecraftBedrockCachedAccessToken(
    const std::shared_ptr<MinecraftBedrockTokenManagerState>& state,
    JsRuntimeValue data
) {
    return JsPromise<void>::fromSynchronous(
        state->microtaskQueue,
        [state, data = std::move(data)]() mutable {
            auto method = requireSetCachedPartial(state);
            auto token = shallowSpread(data);
            token.set(
                "obtainedOn",
                JsRuntimeValue::number(state->dateNowMilliseconds())
            );
            return method(JsRuntimeValue::object({
                {"mca", std::move(token)}
            }));
        }
    );
}

JsRuntimeValue nullishOr(
    const JsRuntimeValue& value,
    JsRuntimeValue fallback
) {
    return value.isUndefined() || value.isNull()
        ? std::move(fallback)
        : value;
}

} // namespace

MinecraftBedrockTokenManager::MinecraftBedrockTokenManager(
    AuthCachePtr cacheValue
) : MinecraftBedrockTokenManager(std::move(cacheValue), {}) {}

MinecraftBedrockTokenManager::MinecraftBedrockTokenManager(
    AuthCachePtr cacheValue,
    BedrockTokenManagerDependencies dependencies
) : state_(std::make_shared<MinecraftBedrockTokenManagerState>(
        std::move(cacheValue)
    )),
    cache(state_->cache),
    forceRefresh(state_->forceRefresh) {
    initializeDependencies(state_, std::move(dependencies));
}

ValuePromise MinecraftBedrockTokenManager::getCachedAccessToken() {
    const auto state = state_;
    return getCached(state).then([state](const JsRuntimeValue& cached) {
        const auto token = destructuredProperty(cached, "mca");
        debug(state, "[mc] token cache", {token});
        if (!token.truthy()) return JsRuntimeValue::undefined();
        debug(state, "Auth token", {token});

        const auto chain = getProperty(token, "chain");
        const auto body = decodeJwtPayload(getIndex(chain, 0));
        const double expiresMilliseconds =
            toNumber(getProperty(body, "exp")) * 1000.0;
        const auto expires = JsRuntimeValue::date(expiresMilliseconds);
        const double remaining = expires.dateMilliseconds() -
            state->dateNowMilliseconds();
        return JsRuntimeValue::object({
            {"valid", JsRuntimeValue::boolean(remaining > 1000.0)},
            {"until", expires},
            {"chain", chain}
        });
    });
}

JsPromise<void> MinecraftBedrockTokenManager::setCachedAccessToken(
    JsRuntimeValue data
) {
    return setMinecraftBedrockCachedAccessToken(
        state_,
        std::move(data)
    );
}

ValuePromise MinecraftBedrockTokenManager::verifyTokens() {
    const auto state = state_;
    return getCachedAccessToken().then([state](
        const JsRuntimeValue& accessToken
    ) {
        if (!accessToken.truthy() || state->forceRefresh.truthy()) {
            return JsRuntimeValue::boolean(false);
        }
        debug(state, "[mc] have user access token", {accessToken});
        return JsRuntimeValue::boolean(
            getProperty(accessToken, "valid").truthy()
        );
    });
}

ValuePromise MinecraftBedrockTokenManager::getAccessToken(
    JsRuntimeValue clientPublicKey,
    JsRuntimeValue xsts
) {
    const auto state = state_;
    debug(state, "[mc] authing to minecraft", {clientPublicKey, xsts});
    const auto headers = jsonHeaders({
        {"User-Agent", JsRuntimeValue::string("MCPE/UWP")},
        {
            "Authorization",
            JsRuntimeValue::string(
                "XBL3.0 x=" + jsToString(getProperty(xsts, "userHash")) +
                ";" + jsToString(getProperty(xsts, "XSTSToken"))
            )
        }
    });
    auto request = makeJsonRequest(
        std::string(AuthenticationEndpoint),
        headers,
        JsRuntimeValue::object({
            {"identityPublicKey", std::move(clientPublicKey)}
        })
    );
    return state->httpClient->fetch(std::move(request)).then(
        [](const XboxTokenHttpResponse& response) {
            return checkStatus(response);
        }
    ).then([state](const JsRuntimeValue& response) {
        debug(state, "[mc] mc auth response", {response});
        return setMinecraftBedrockCachedAccessToken(
            state,
            response
        ).then([response] {
            return response;
        });
    });
}

std::shared_ptr<JsMicrotaskQueue>
MinecraftBedrockTokenManager::microtaskQueue() const noexcept {
    return state_->microtaskQueue;
}

XboxTokenHttpClientPtr
MinecraftBedrockTokenManager::httpClient() const noexcept {
    return state_->httpClient;
}

MinecraftBedrockServicesTokenManager::
MinecraftBedrockServicesTokenManager(AuthCachePtr cacheValue)
    : MinecraftBedrockServicesTokenManager(std::move(cacheValue), {}) {}

MinecraftBedrockServicesTokenManager::
MinecraftBedrockServicesTokenManager(
    AuthCachePtr cacheValue,
    BedrockTokenManagerDependencies dependencies
) : state_(std::make_shared<MinecraftBedrockServicesTokenManagerState>(
        std::move(cacheValue)
    )),
    cache(state_->cache) {
    initializeDependencies(state_, std::move(dependencies));
}

ValuePromise MinecraftBedrockServicesTokenManager::getCachedAccessToken() {
    const auto state = state_;
    return getCached(state).then([state](const JsRuntimeValue& cached) {
        const auto token = destructuredProperty(cached, "mcs");
        debug(state, "[mcs] token cache", {token});
        if (!token.truthy()) {
            return JsRuntimeValue::object({
                {"valid", JsRuntimeValue::boolean(false)}
            });
        }

        const auto expires = JsRuntimeValue::date(newDateMilliseconds(
            getProperty(token, "validUntil")
        ));
        const double remaining = expires.dateMilliseconds() -
            state->dateNowMilliseconds();
        return JsRuntimeValue::object({
            {"valid", JsRuntimeValue::boolean(remaining > 1000.0)},
            {"until", expires},
            {"token", getProperty(token, "mcToken")},
            {"data", token}
        });
    });
}

JsPromise<void> MinecraftBedrockServicesTokenManager::setCachedToken(
    JsRuntimeValue data
) {
    return setCachedPartial(state_, std::move(data));
}

ValuePromise MinecraftBedrockServicesTokenManager::getAccessToken(
    JsRuntimeValue sessionTicket,
    JsRuntimeValue options
) {
    if (options.isUndefined()) options = JsRuntimeValue::object();
    const auto state = state_;
    const auto option = [&](std::string_view name, JsRuntimeValue fallback) {
        return nullishOr(getProperty(options, name), std::move(fallback));
    };
    const auto body = JsRuntimeValue::object({
        {"device", JsRuntimeValue::object({
            {
                "applicationType",
                option("applicationType", JsRuntimeValue::string("MinecraftPE"))
            },
            {
                "gameVersion",
                option("version", JsRuntimeValue::string("1.20.62"))
            },
            {
                "id",
                option(
                    "deviceId",
                    JsRuntimeValue::string(
                        "c1681ad3-415e-30cd-abd3-3b8f51e771d1"
                    )
                )
            },
            {
                "memory",
                option("deviceMemory", JsRuntimeValue::string("8589934592"))
            },
            {
                "platform",
                option("platform", JsRuntimeValue::string("Windows10"))
            },
            {
                "playFabTitleId",
                option("playFabtitleId", JsRuntimeValue::string("20CA2"))
            },
            {
                "storePlatform",
                option("storePlatform", JsRuntimeValue::string("uwp.store"))
            },
            {
                "type",
                option("type", JsRuntimeValue::string("Windows10"))
            }
        })},
        {"user", JsRuntimeValue::object({
            {"token", std::move(sessionTicket)},
            {"tokenType", JsRuntimeValue::string("PlayFab")}
        })}
    });
    auto request = makeJsonRequest(
        std::string(SessionStartEndpoint),
        jsonHeaders(),
        body
    );
    return state->httpClient->fetch(std::move(request)).then(
        [](const XboxTokenHttpResponse& response) {
            return checkStatus(response);
        }
    ).then([state](const JsRuntimeValue& response) {
        const auto result = getProperty(response, "result");
        auto tokenResponse = JsRuntimeValue::object({
            {
                "mcToken",
                getProperty(result, "authorizationHeader")
            },
            {"validUntil", getProperty(result, "validUntil")},
            {"treatments", getProperty(result, "treatments")},
            {"configurations", getProperty(result, "configurations")},
            {"treatmentContext", getProperty(result, "treatmentContext")}
        });
        debug(state, "[mc] mc-services token response", {tokenResponse});
        return setCachedPartial(state, JsRuntimeValue::object({
            {"mcs", tokenResponse}
        })).then([tokenResponse] {
            return tokenResponse;
        });
    });
}

std::shared_ptr<JsMicrotaskQueue>
MinecraftBedrockServicesTokenManager::microtaskQueue() const noexcept {
    return state_->microtaskQueue;
}

XboxTokenHttpClientPtr
MinecraftBedrockServicesTokenManager::httpClient() const noexcept {
    return state_->httpClient;
}

PlayfabTokenManager::PlayfabTokenManager(AuthCachePtr cacheValue)
    : PlayfabTokenManager(std::move(cacheValue), {}) {}

PlayfabTokenManager::PlayfabTokenManager(
    AuthCachePtr cacheValue,
    BedrockTokenManagerDependencies dependencies
) : state_(std::make_shared<PlayfabTokenManagerState>(
        std::move(cacheValue)
    )),
    cache(state_->cache) {
    initializeDependencies(state_, std::move(dependencies));
}

JsPromise<void> PlayfabTokenManager::setCachedAccessToken(
    JsRuntimeValue data
) {
    return setCachedPartial(state_, std::move(data));
}

ValuePromise PlayfabTokenManager::getCachedAccessToken() {
    const auto state = state_;
    return getCached(state).then([state](const JsRuntimeValue& cached) {
        const auto cache = destructuredProperty(cached, "pfb");
        debug(state, "[pf] token cache", {cache});
        if (!cache.truthy()) return JsRuntimeValue::undefined();

        const auto entityToken = getProperty(cache, "EntityToken");
        const auto expires = JsRuntimeValue::date(newDateMilliseconds(
            getProperty(entityToken, "TokenExpiration")
        ));
        const double remaining = expires.dateMilliseconds() -
            state->dateNowMilliseconds();
        return JsRuntimeValue::object({
            {"valid", JsRuntimeValue::boolean(remaining > 1000.0)},
            {"until", expires},
            {"data", cache}
        });
    });
}

ValuePromise PlayfabTokenManager::getAccessToken(JsRuntimeValue xsts) {
    const auto state = state_;
    auto infoRequest = JsRuntimeValue::object({
        {"GetCharacterInventories", JsRuntimeValue::boolean(false)},
        {"GetCharacterList", JsRuntimeValue::boolean(false)},
        {"GetPlayerProfile", JsRuntimeValue::boolean(true)},
        {"GetPlayerStatistics", JsRuntimeValue::boolean(false)},
        {"GetTitleData", JsRuntimeValue::boolean(false)},
        {"GetUserAccountInfo", JsRuntimeValue::boolean(true)},
        {"GetUserData", JsRuntimeValue::boolean(false)},
        {"GetUserInventory", JsRuntimeValue::boolean(false)},
        {"GetUserReadOnlyData", JsRuntimeValue::boolean(false)},
        {"GetUserVirtualCurrency", JsRuntimeValue::boolean(false)},
        {"PlayerStatisticNames", JsRuntimeValue::null()},
        {"ProfileConstraints", JsRuntimeValue::null()},
        {"TitleDataKeys", JsRuntimeValue::null()},
        {"UserDataKeys", JsRuntimeValue::null()},
        {"UserReadOnlyDataKeys", JsRuntimeValue::null()}
    });
    auto body = JsRuntimeValue::object({
        {"CreateAccount", JsRuntimeValue::boolean(true)},
        {"EncryptedRequest", JsRuntimeValue::null()},
        {"InfoRequestParameters", std::move(infoRequest)},
        {"PlayerSecret", JsRuntimeValue::null()},
        {"TitleId", JsRuntimeValue::string("20CA2")},
        {
            "XboxToken",
            JsRuntimeValue::string(
                "XBL3.0 x=" + jsToString(getProperty(xsts, "userHash")) +
                ";" + jsToString(getProperty(xsts, "XSTSToken"))
            )
        }
    });
    auto request = makeJsonRequest(
        std::string(LoginWithXboxEndpoint),
        jsonHeaders(),
        std::move(body)
    );
    return state->httpClient->fetch(std::move(request)).then(
        [](const XboxTokenHttpResponse& response) {
            // The source intentionally calls response.json() without
            // checkStatus, so PlayFab error bodies are still cached/returned.
            return JsRuntimeJson::parse(response.bodyText);
        }
    ).then([state](const JsRuntimeValue& response) {
        const auto data = getProperty(response, "data");
        return setCachedPartial(state, JsRuntimeValue::object({
            {"pfb", data}
        })).then([data] {
            return data;
        });
    });
}

std::shared_ptr<JsMicrotaskQueue>
PlayfabTokenManager::microtaskQueue() const noexcept {
    return state_->microtaskQueue;
}

XboxTokenHttpClientPtr PlayfabTokenManager::httpClient() const noexcept {
    return state_->httpClient;
}

} // namespace bedrock
