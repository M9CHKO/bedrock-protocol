#include <bedrock/auth/XboxTokenManager.hpp>

#include <bedrock/auth/BedrockAuthJwt.hpp>
#include <bedrock/auth/MsalHttpClient.hpp>
#include <bedrock/auth/UuidV3.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bedrock {

struct XboxTokenManagerState {
    XboxTokenManagerState(XboxProofKey keyValue, AuthCachePtr cacheValue)
        : key(std::move(keyValue)), cache(std::move(cacheValue)) {}

    XboxProofKey key;
    JsRuntimeValue jwk = JsRuntimeValue::undefined();
    AuthCachePtr cache;
    JsRuntimeValue headers = JsRuntimeValue::undefined();
    JsRuntimeValue forceRefresh = JsRuntimeValue::undefined();
    XboxTokenHttpClientPtr httpClient;
    MsalHttpClientPtr replayHttpClient;
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue;
    std::function<double()> dateNowMilliseconds;
};

namespace {

using ValuePromise = JsPromise<JsRuntimeValue>;

constexpr std::string_view kXboxDeviceAuth =
    "https://device.auth.xboxlive.com/device/authenticate";
constexpr std::string_view kXboxTitleAuth =
    "https://title.auth.xboxlive.com/title/authenticate";
constexpr std::string_view kXboxUserAuth =
    "https://user.auth.xboxlive.com/user/authenticate";
constexpr std::string_view kSisuAuthorize =
    "https://sisu.xboxlive.com/authorize";
constexpr std::string_view kXstsAuthorize =
    "https://xsts.auth.xboxlive.com/xsts/authorize";

bool asciiCaseInsensitiveEqual(
    std::string_view left,
    std::string_view right
) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        auto lower = [](unsigned char byte) {
            return byte >= 'A' && byte <= 'Z'
                ? static_cast<unsigned char>(byte + ('a' - 'A'))
                : byte;
        };
        if (lower(static_cast<unsigned char>(left[index])) !=
            lower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

struct DecodedCodePoint {
    std::uint32_t value;
    std::size_t width;
};

DecodedCodePoint decodeUtf8OrWtf8(
    std::string_view input,
    std::size_t offset
) {
    if (offset >= input.size()) {
        throw std::invalid_argument("invalid JavaScript string");
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
        if (first == 0xe0U && second < 0xa0U) {
            throw std::invalid_argument("invalid JavaScript string");
        }
        return {
            ((first & 0x0fU) << 12) |
                ((second & 0x3fU) << 6) | (byte(offset + 2) & 0x3fU),
            3
        };
    }
    if (first >= 0xf0U && first <= 0xf4U &&
        continuation(offset + 1) && continuation(offset + 2) &&
        continuation(offset + 3)) {
        const auto second = byte(offset + 1);
        if ((first == 0xf0U && second < 0x90U) ||
            (first == 0xf4U && second >= 0x90U)) {
            throw std::invalid_argument("invalid JavaScript string");
        }
        return {
            ((first & 0x07U) << 18) |
                ((second & 0x3fU) << 12) |
                ((byte(offset + 2) & 0x3fU) << 6) |
                (byte(offset + 3) & 0x3fU),
            4
        };
    }
    throw std::invalid_argument("invalid JavaScript string");
}

void appendWtf8(std::string& output, std::uint16_t unit) {
    if (unit <= 0x7fU) {
        output.push_back(static_cast<char>(unit));
    } else if (unit <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (unit >> 6)));
        output.push_back(static_cast<char>(0x80U | (unit & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xe0U | (unit >> 12)));
        output.push_back(static_cast<char>(0x80U | ((unit >> 6) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (unit & 0x3fU)));
    }
}

std::vector<std::uint16_t> utf16Units(std::string_view input) {
    std::vector<std::uint16_t> units;
    for (std::size_t offset = 0; offset < input.size();) {
        auto decoded = decodeUtf8OrWtf8(input, offset);
        offset += decoded.width;
        if (decoded.value <= 0xffffU) {
            units.push_back(static_cast<std::uint16_t>(decoded.value));
        } else {
            decoded.value -= 0x10000U;
            units.push_back(static_cast<std::uint16_t>(
                0xd800U + (decoded.value >> 10)
            ));
            units.push_back(static_cast<std::uint16_t>(
                0xdc00U + (decoded.value & 0x3ffU)
            ));
        }
    }
    return units;
}

std::string jsNumberToString(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    return JsRuntimeJson::stringify(
        JsRuntimeValue::number(value)
    ).value_or("undefined");
}

std::string jsToString(const JsRuntimeValue& value);

std::string jsArrayJoin(const JsRuntimeValue& value) {
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
    if (value.isArray()) return jsArrayJoin(value);
    if (value.isDate()) {
        return value.dateIsValid() ? value.dateIsoString() : "Invalid Date";
    }
    if (value.isMap()) return "[object Map]";
    if (value.isFunction()) {
        const auto* name = value.get("name");
        return "function " +
            (name && name->isString() ? name->stringValue() : std::string()) +
            "() { [native code] }";
    }
    return "[object Object]";
}

JsRuntimeValue stringIndexValue(
    std::string_view value,
    std::size_t index
) {
    const auto units = utf16Units(value);
    if (index >= units.size()) return JsRuntimeValue::undefined();
    std::string result;
    appendWtf8(result, units[index]);
    return JsRuntimeValue::string(std::move(result));
}

JsRuntimeValue getProperty(
    const JsRuntimeValue& object,
    std::string_view property
) {
    if (object.isUndefined() || object.isNull()) {
        throw std::runtime_error(
            std::string("Cannot read properties of ") +
            (object.isNull() ? "null" : "undefined") + " (reading '" +
            std::string(property) + "')"
        );
    }
    if (object.isString()) {
        if (property == "length") {
            return JsRuntimeValue::number(static_cast<double>(
                utf16Units(object.stringValue()).size()
            ));
        }
        std::size_t index = 0;
        if (!property.empty() &&
            std::all_of(property.begin(), property.end(), [](char byte) {
                return byte >= '0' && byte <= '9';
            })) {
            try {
                index = static_cast<std::size_t>(std::stoull(
                    std::string(property)
                ));
                return stringIndexValue(object.stringValue(), index);
            } catch (const std::out_of_range&) {
                return JsRuntimeValue::undefined();
            }
        }
    }
    const auto* found = object.get(property);
    return found ? *found : JsRuntimeValue::undefined();
}

JsRuntimeValue getIndex(const JsRuntimeValue& object, std::size_t index) {
    if (object.isUndefined() || object.isNull()) {
        throw std::runtime_error(
            std::string("Cannot read properties of ") +
            (object.isNull() ? "null" : "undefined") + " (reading '" +
            std::to_string(index) + "')"
        );
    }
    if (object.isArray()) {
        const auto* found = object.get(index);
        return found ? *found : JsRuntimeValue::undefined();
    }
    if (object.isString()) return stringIndexValue(object.stringValue(), index);
    const auto* found = object.get(std::to_string(index));
    return found ? *found : JsRuntimeValue::undefined();
}

JsRuntimeValue makeJwk(const XboxProofKeyJwk& value) {
    return JsRuntimeValue::object({
        {"kty", JsRuntimeValue::string(value.kty)},
        {"x", JsRuntimeValue::string(value.x)},
        {"y", JsRuntimeValue::string(value.y)},
        {"crv", JsRuntimeValue::string(value.crv)},
        {"alg", JsRuntimeValue::string(value.alg)},
        {"use", JsRuntimeValue::string(value.use)}
    });
}

JsRuntimeValue spreadObject(const JsRuntimeValue& source) {
    auto result = JsRuntimeValue::object();
    if (source.isNull() || source.isUndefined()) return result;
    if (source.isString()) {
        const auto units = utf16Units(source.stringValue());
        for (std::size_t index = 0; index < units.size(); ++index) {
            std::string character;
            appendWtf8(character, units[index]);
            result.set(
                std::to_string(index),
                JsRuntimeValue::string(std::move(character))
            );
        }
        return result;
    }
    for (const auto& property : source.ownProperties()) {
        result.set(property.key, property.value);
    }
    return result;
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

std::string stringifyRequired(const JsRuntimeValue& value) {
    const auto result = JsRuntimeJson::stringify(value);
    if (!result) return "undefined";
    return *result;
}

double systemDateNowMilliseconds() {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

std::int64_t dateNowAsInteger(
    const std::shared_ptr<XboxTokenManagerState>& state
) {
    const double now = state->dateNowMilliseconds();
    if (!std::isfinite(now) ||
        now < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        now > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("Date.now() returned an invalid value");
    }
    return static_cast<std::int64_t>(now);
}

std::string nextUuid(const std::shared_ptr<XboxTokenManagerState>& state) {
    return uuidFrom(jsNumberToString(state->dateNowMilliseconds()));
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
        day > static_cast<int>(daysInMonth(year, static_cast<unsigned>(month)))) {
        return std::nullopt;
    }

    int hour = 0;
    int minute = 0;
    int second = 0;
    int milliseconds = 0;
    int timezoneMinutes = 0;
    if (offset == value.size()) {
        // Date-only ISO strings are UTC in ECMAScript.
    } else {
        if (value[offset] != 'T' && value[offset] != 't' &&
            value[offset] != ' ') {
            return std::nullopt;
        }
        ++offset;
        if (!parseDigits(value, offset, 2, hour) ||
            offset >= value.size() || value[offset++] != ':' ||
            !parseDigits(value, offset, 2, minute) ||
            offset >= value.size() || value[offset++] != ':' ||
            !parseDigits(value, offset, 2, second)) {
            return std::nullopt;
        }
        if (hour > 23 || minute > 59 || second > 59) {
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
        if (offset >= value.size()) {
            // The Xbox service emits Z. Local-time parsing is intentionally
            // not used for cache entries because it would make persistence
            // machine-timezone dependent.
            return std::nullopt;
        }
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
    if (value.isNull()) return 0;
    if (value.isBool()) return value.boolValue() ? 1 : 0;
    if (value.isNumber()) return value.numberValue();
    const auto parsed = parseIsoDate(jsToString(value));
    return parsed.value_or(std::numeric_limits<double>::quiet_NaN());
}

bool checkIfValid(
    const JsRuntimeValue& expires,
    const std::shared_ptr<XboxTokenManagerState>& state
) {
    const double remaining =
        newDateMilliseconds(expires) - state->dateNowMilliseconds();
    return remaining > 1000;
}

std::vector<std::uint8_t> latin1Bytes(std::string_view value) {
    std::vector<std::uint8_t> result;
    for (const auto unit : utf16Units(value)) {
        result.push_back(static_cast<std::uint8_t>(unit & 0xffU));
    }
    return result;
}

std::string sha1Prefix(std::string_view value) {
    const auto bytes = latin1Bytes(value);
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
    unsigned int length = 0;
    if (EVP_Digest(
            bytes.data(),
            bytes.size(),
            digest.data(),
            &length,
            EVP_sha1(),
            nullptr
        ) != 1 || length != 20) {
        throw std::runtime_error("SHA-1 relying-party hash failed");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(6);
    for (std::size_t index = 0; index < 3; ++index) {
        result.push_back(hex[(digest[index] >> 4U) & 0x0fU]);
        result.push_back(hex[digest[index] & 0x0fU]);
    }
    return result;
}

std::string statusTextFor(int status) {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return {};
    }
}

JsRuntimeValue checkStatus(const XboxTokenHttpResponse& response) {
    if (response.ok()) return JsRuntimeJson::parse(response.bodyText);
    throw std::runtime_error(
        std::to_string(response.status) + " " + response.statusText + " " +
        response.bodyText
    );
}

JsPromise<void> setCachedTokenState(
    const std::shared_ptr<XboxTokenManagerState>& state,
    JsRuntimeValue data
) {
    return JsPromise<void>::fromSynchronous(
        state->microtaskQueue,
        [state, data = std::move(data)]() mutable {
            if (!state->cache) {
                throw std::runtime_error(
                    "Cannot read properties of null (reading "
                    "'setCachedPartial')"
                );
            }
            const auto method = state->cache->requireSetCachedPartialMethod();
            return method(std::move(data));
        }
    );
}

JsPromise<JsRuntimeValue> getCachedState(
    const std::shared_ptr<XboxTokenManagerState>& state
) {
    return JsPromise<JsRuntimeValue>::fromSynchronous(
        state->microtaskQueue,
        [state] {
            if (!state->cache) {
                throw std::runtime_error(
                    "Cannot read properties of null (reading 'getCached')"
                );
            }
            const auto method = state->cache->requireGetCachedMethod();
            return method();
        }
    );
}

std::vector<std::uint8_t> signState(
    const std::shared_ptr<XboxTokenManagerState>& state,
    std::string_view url,
    std::string_view authorizationToken,
    std::string_view payload
) {
    return state->key.signAt(
        url,
        authorizationToken,
        payload,
        dateNowAsInteger(state)
    );
}

std::string signatureBase64(
    const std::shared_ptr<XboxTokenManagerState>& state,
    std::string_view url,
    std::string_view payload
) {
    return BedrockAuthJwt::base64(signState(state, url, "", payload));
}

XboxTokenHttpRequest makeRequest(
    std::string url,
    JsRuntimeValue headers,
    std::string body
) {
    XboxTokenHttpRequest request;
    request.url = std::move(url);
    request.headersObject = headers;
    request.headers = headersFromObject(headers);
    request.body = std::move(body);
    return request;
}

JsRuntimeValue normalizedDefaultOptions(JsRuntimeValue options) {
    return options.isUndefined() ? JsRuntimeValue::object() : options;
}

JsRuntimeValue makeXstsResult(const JsRuntimeValue& response) {
    const auto displayClaims = getProperty(response, "DisplayClaims");
    const auto xui = getProperty(displayClaims, "xui");
    const auto first = getIndex(xui, 0);
    const auto xid = getProperty(first, "xid");
    return JsRuntimeValue::object({
        {
            "userXUID",
            xid.truthy() ? xid : JsRuntimeValue::null()
        },
        {"userHash", getProperty(first, "uhs")},
        {"XSTSToken", getProperty(response, "Token")},
        {"expiresOn", getProperty(response, "NotAfter")}
    });
}

JsRuntimeValue makeSisuXstsResult(const JsRuntimeValue& response) {
    const auto authorizationToken = getProperty(
        response,
        "AuthorizationToken"
    );
    const auto displayClaims = getProperty(
        authorizationToken,
        "DisplayClaims"
    );
    const auto first = getIndex(getProperty(displayClaims, "xui"), 0);
    const auto xid = getProperty(first, "xid");
    return JsRuntimeValue::object({
        {"userXUID", xid.truthy() ? xid : JsRuntimeValue::null()},
        {"userHash", getProperty(first, "uhs")},
        {"XSTSToken", getProperty(authorizationToken, "Token")},
        {"expiresOn", getProperty(authorizationToken, "NotAfter")}
    });
}

JsRuntimeValue parseIntHeader(const std::string* header) {
    if (!header) {
        return JsRuntimeValue::number(
            std::numeric_limits<double>::quiet_NaN()
        );
    }
    const auto& value = *header;
    std::size_t offset = 0;
    const auto whitespace = [](char byte) {
        return byte == ' ' || byte == '\t' || byte == '\n' ||
            byte == '\r' || byte == '\f' || byte == '\v';
    };
    while (offset < value.size() && whitespace(value[offset])) ++offset;
    bool negative = false;
    if (offset < value.size() &&
        (value[offset] == '+' || value[offset] == '-')) {
        negative = value[offset] == '-';
        ++offset;
    }
    int radix = 10;
    if (offset + 1 < value.size() && value[offset] == '0' &&
        (value[offset + 1] == 'x' || value[offset + 1] == 'X')) {
        radix = 16;
        offset += 2;
    }
    double result = 0;
    bool any = false;
    for (; offset < value.size(); ++offset) {
        const char byte = value[offset];
        int digit = -1;
        if (byte >= '0' && byte <= '9') digit = byte - '0';
        else if (byte >= 'a' && byte <= 'f') digit = byte - 'a' + 10;
        else if (byte >= 'A' && byte <= 'F') digit = byte - 'A' + 10;
        if (digit < 0 || digit >= radix) break;
        any = true;
        result = result * static_cast<double>(radix) + digit;
    }
    if (!any) {
        return JsRuntimeValue::number(
            std::numeric_limits<double>::quiet_NaN()
        );
    }
    return JsRuntimeValue::number(negative ? -result : result);
}

class XboxReplayError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string exceptionMessage(std::exception_ptr error) {
    try {
        if (error) std::rethrow_exception(error);
    } catch (const std::exception& caught) {
        return caught.what();
    } catch (...) {
        return "non-standard exception";
    }
    return {};
}

bool isXboxReplayError(std::exception_ptr error) {
    try {
        if (error) std::rethrow_exception(error);
    } catch (const XboxReplayError&) {
        return true;
    } catch (...) {
    }
    return false;
}

std::string queryEscape(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char byte : value) {
        const bool unescaped =
            (byte >= 'a' && byte <= 'z') ||
            (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') ||
            byte == '!' || byte == '\'' || byte == '(' || byte == ')' ||
            byte == '*' || byte == '-' || byte == '.' || byte == '_' ||
            byte == '~';
        if (unescaped) {
            result.push_back(static_cast<char>(byte));
        } else {
            result.push_back('%');
            result.push_back(hex[(byte >> 4U) & 0x0fU]);
            result.push_back(hex[byte & 0x0fU]);
        }
    }
    return result;
}

std::string queryUnescape(std::string_view value) {
    const auto hexDigit = [](char byte) -> int {
        if (byte >= '0' && byte <= '9') return byte - '0';
        if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
        if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
        return -1;
    };
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '+') {
            result.push_back(' ');
            continue;
        }
        if (value[index] == '%' && index + 2 < value.size()) {
            const int high = hexDigit(value[index + 1]);
            const int low = hexDigit(value[index + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        result.push_back(value[index]);
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> replayBaseHeaders() {
    return {
        {"Accept", "application/json, text/plain, */*"},
        {"Accept-encoding", "gzip"},
        {"Accept-Language", "en-US"},
        {
            "User-Agent",
            "Mozilla/5.0 (XboxReplay; XboxLiveAuth/3.0) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/71.0.3578.98 Safari/537.36"
        }
    };
}

MsalHttpRequest replayRequest(
    std::string method,
    std::string url,
    std::vector<std::pair<std::string, std::string>> headers,
    std::string body = {},
    int maxRedirects = 0
) {
    MsalHttpRequest request;
    request.method = std::move(method);
    request.url = std::move(url);
    request.headers = std::move(headers);
    request.body = std::move(body);
    request.maxRedirects = maxRedirects;
    request.decompress = true;
    return request;
}

std::string firstCookiePart(std::string_view cookie) {
    return std::string(cookie.substr(0, cookie.find(';')));
}

std::string replayCookies(const MsalHttpResponse& response) {
    std::vector<std::string> cookies;
    const auto* values = response.headersObject.get("set-cookie");
    if (values && values->isArray()) {
        for (std::size_t index = 0; index < values->length(); ++index) {
            const auto* item = values->get(index);
            if (item && item->isString()) {
                cookies.push_back(firstCookiePart(item->stringValue()));
            }
        }
    } else if (values && values->isString()) {
        cookies.push_back(firstCookiePart(values->stringValue()));
    } else if (const auto* scalar = response.header("set-cookie")) {
        cookies.push_back(firstCookiePart(*scalar));
    }
    std::string result;
    for (std::size_t index = 0; index < cookies.size(); ++index) {
        if (index) result += "; ";
        result += cookies[index];
    }
    return result;
}

std::optional<std::string> htmlAttributeMatch(
    const std::string& body,
    const std::regex& pattern
) {
    std::smatch match;
    if (!std::regex_search(body, match, pattern) || match.size() < 2) {
        return std::nullopt;
    }
    return match[1].str();
}

JsRuntimeValue parseHashQuery(std::string_view hash) {
    auto result = JsRuntimeValue::object();
    std::size_t offset = 0;
    while (offset <= hash.size()) {
        const auto separator = hash.find('&', offset);
        const auto entry = hash.substr(
            offset,
            separator == std::string_view::npos
                ? std::string_view::npos
                : separator - offset
        );
        const auto equals = entry.find('=');
        auto key = queryUnescape(entry.substr(0, equals));
        auto value = equals == std::string_view::npos
            ? std::string()
            : queryUnescape(entry.substr(equals + 1));
        result.set(std::move(key), JsRuntimeValue::string(std::move(value)));
        if (separator == std::string_view::npos) break;
        offset = separator + 1;
    }
    const auto expires = getProperty(result, "expires_in");
    if (expires.isString()) {
        char* end = nullptr;
        const double number = std::strtod(expires.stringValue().c_str(), &end);
        result.set(
            "expires_in",
            JsRuntimeValue::number(
                end == expires.stringValue().c_str() ? 0.0 : number
            )
        );
    }
    return result;
}

ValuePromise replayPreAuth(
    const std::shared_ptr<XboxTokenManagerState>& state
) {
    const std::string url =
        "https://login.live.com/oauth20_authorize.srf?"
        "client_id=000000004C12AE6F&"
        "redirect_uri=https%3A%2F%2Flogin.live.com%2Foauth20_desktop.srf&"
        "scope=service%3A%3Auser.auth.xboxlive.com%3A%3AMBI_SSL&"
        "display=touch&response_type=token&locale=en";
    return state->replayHttpClient->send(replayRequest(
        "GET",
        url,
        replayBaseHeaders()
    )).then([](const MsalHttpResponse& response) {
        if (response.status != 200) {
            throw XboxReplayError("Pre-authentication failed.");
        }
        // Keep the source regex behavior while accepting insignificant space
        // before the closing quote used by newer Live pages.
        const auto ppft = htmlAttributeMatch(
            response.bodyText,
            std::regex(R"REGEX(sFTTag:'.*value="(.*)"/>)REGEX")
        );
        const auto urlPost = htmlAttributeMatch(
            response.bodyText,
            std::regex(R"REGEX(urlPost:'([^']+))REGEX")
        );
        if (!ppft) {
            throw XboxReplayError(
                "Could not match \"PPFT\" parameter, please fill an issue "
                "on https://bit.ly/xr-xbl-auth-create-issue"
            );
        }
        if (!urlPost) {
            throw XboxReplayError(
                "Could not match \"urlPost\" parameter, please fill an "
                "issue on https://bit.ly/xr-xbl-auth-create-issue"
            );
        }
        return JsRuntimeValue::object({
            {"cookie", JsRuntimeValue::string(replayCookies(response))},
            {"matches", JsRuntimeValue::object({
                {"PPFT", JsRuntimeValue::string(*ppft)},
                {"urlPost", JsRuntimeValue::string(*urlPost)}
            })}
        });
    }).catchError([state](std::exception_ptr error) -> ValuePromise {
        if (isXboxReplayError(error)) std::rethrow_exception(error);
        throw XboxReplayError(exceptionMessage(error));
    });
}

ValuePromise replayLogUser(
    const std::shared_ptr<XboxTokenManagerState>& state,
    JsRuntimeValue preAuth,
    const JsRuntimeValue& email,
    const JsRuntimeValue& password
) {
    const auto matches = getProperty(preAuth, "matches");
    const auto urlPost = jsToString(getProperty(matches, "urlPost"));
    const auto emailText = jsToString(email);
    const std::string body =
        "login=" + queryEscape(emailText) +
        "&loginfmt=" + queryEscape(emailText) +
        "&passwd=" + queryEscape(jsToString(password)) +
        "&PPFT=" + queryEscape(jsToString(getProperty(matches, "PPFT")));
    auto headers = replayBaseHeaders();
    headers.emplace_back(
        "Content-Type",
        "application/x-www-form-urlencoded"
    );
    headers.emplace_back(
        "Cookie",
        jsToString(getProperty(preAuth, "cookie"))
    );
    return state->replayHttpClient->send(replayRequest(
        "POST",
        urlPost,
        std::move(headers),
        body,
        1
    )).then([urlPost](const MsalHttpResponse& response) {
        if (response.status != 200) {
            throw XboxReplayError("Authentication failed.");
        }
        const auto& responseUrl = response.url;
        if (responseUrl == urlPost) {
            throw XboxReplayError("Invalid credentials.");
        }
        const auto hashSeparator = responseUrl.find('#');
        if (hashSeparator == std::string::npos) {
            const bool identityConfirmation =
                response.bodyText.find("id=\"fmHF\"") !=
                    std::string::npos &&
                response.bodyText.find("identity/confirm") !=
                    std::string::npos;
            throw XboxReplayError(
                identityConfirmation
                    ? "Activity confirmation required, please refer to "
                        "https://bit.ly/xr-xbl-auth-err-activity"
                    : "Invalid credentials or 2FA enabled, please refer to "
                        "https://bit.ly/xr-xbl-auth-err-2fa"
            );
        }
        return parseHashQuery(responseUrl.substr(hashSeparator + 1));
    }).catchError([](std::exception_ptr error) -> ValuePromise {
        if (isXboxReplayError(error)) std::rethrow_exception(error);
        throw XboxReplayError(exceptionMessage(error));
    });
}

ValuePromise replayExchangeUserToken(
    const std::shared_ptr<XboxTokenManagerState>& state,
    const JsRuntimeValue& rpsTicket
) {
    auto headers = replayBaseHeaders();
    for (auto& [name, value] : headers) {
        if (name == "Accept") {
            value = "application/json";
            break;
        }
    }
    headers.emplace_back("x-xbl-contract-version", "0");
    headers.emplace_back("Content-Type", "application/json");
    const auto body = stringifyRequired(JsRuntimeValue::object({
        {
            "RelyingParty",
            JsRuntimeValue::string("http://auth.xboxlive.com")
        },
        {"TokenType", JsRuntimeValue::string("JWT")},
        {"Properties", JsRuntimeValue::object({
            {"AuthMethod", JsRuntimeValue::string("RPS")},
            {
                "SiteName",
                JsRuntimeValue::string("user.auth.xboxlive.com")
            },
            {"RpsTicket", rpsTicket}
        })}
    }));
    return state->replayHttpClient->send(replayRequest(
        "POST",
        "https://user.auth.xboxlive.com/user/authenticate",
        std::move(headers),
        body
    )).then([](const MsalHttpResponse& response) {
        if (response.status != 200) {
            throw XboxReplayError(
                "Could not exchange specified \"RpsTicket\""
            );
        }
        return JsRuntimeJson::parse(response.bodyText);
    }).catchError([](std::exception_ptr error) -> ValuePromise {
        if (isXboxReplayError(error)) std::rethrow_exception(error);
        throw XboxReplayError(exceptionMessage(error));
    });
}

[[noreturn]] void throwTokenError(
    const JsRuntimeValue& errorCode,
    const JsRuntimeValue& response
) {
    const std::string keyValue = jsToString(errorCode);
    struct KnownError {
        std::string_view code;
        std::string_view message;
    };
    static constexpr KnownError knownErrors[] = {
        {
            "2148916227",
            "Your account was banned by Xbox for violating one or more "
            "Community Standards for Xbox and is unable to be used."
        },
        {
            "2148916229",
            "Your account is currently restricted and your guardian has not "
            "given you permission to play online. Login to "
            "https://account.microsoft.com/family/ and have your guardian "
            "change your permissions."
        },
        {
            "2148916233",
            "Your account currently does not have an Xbox profile. Please "
            "create one at https://signup.live.com/signup"
        },
        {
            "2148916234",
            "Your account has not accepted Xbox's Terms of Service. Please "
            "login and accept them."
        },
        {
            "2148916235",
            "Your account resides in a region that Xbox has not authorized "
            "use from. Xbox has blocked your attempt at logging in."
        },
        {
            "2148916236",
            "Your account requires proof of age. Please login to "
            "https://login.live.com/login.srf and provide proof of age."
        },
        {
            "2148916237",
            "Your account has reached the its limit for playtime. Your "
            "account has been blocked from logging in."
        },
        {
            "2148916238",
            "The account date of birth is under 18 years and cannot proceed "
            "unless the account is added to a family by an adult."
        }
    };
    for (const auto& known : knownErrors) {
        if (known.code == keyValue) {
            throw std::runtime_error(std::string(known.message));
        }
    }
    throw std::runtime_error(
        "Xbox Live authentication failed to obtain a XSTS token. XErr: " +
        keyValue + "\n" + stringifyRequired(response)
    );
}

ValuePromise getXstsTokenState(
    const std::shared_ptr<XboxTokenManagerState>& state,
    JsRuntimeValue tokens,
    JsRuntimeValue options
) {
    options = normalizedDefaultOptions(std::move(options));
    return ValuePromise::fromSynchronous(
        state->microtaskQueue,
        [state,
         tokens = std::move(tokens),
         options = std::move(options)]() mutable {
            const auto userToken = getProperty(tokens, "userToken");
            auto payload = JsRuntimeValue::object({
                {"RelyingParty", getProperty(options, "relyingParty")},
                {"TokenType", JsRuntimeValue::string("JWT")},
                {"Properties", JsRuntimeValue::object({
                    {"UserTokens", JsRuntimeValue::array({userToken})},
                    {"DeviceToken", getProperty(tokens, "deviceToken")},
                    {"TitleToken", getProperty(tokens, "titleToken")},
                    {
                        "OptionalDisplayClaims",
                        getProperty(options, "optionalDisplayClaims")
                    },
                    {"ProofKey", state->jwk},
                    {"SandboxId", JsRuntimeValue::string("RETAIL")}
                })}
            });
            const auto body = stringifyRequired(payload);
            auto requestHeaders = spreadObject(state->headers);
            requestHeaders.set(
                "Signature",
                JsRuntimeValue::string(signatureBase64(
                    state,
                    kXstsAuthorize,
                    body
                ))
            );

            return state->httpClient->fetch(makeRequest(
                std::string(kXstsAuthorize),
                std::move(requestHeaders),
                body
            )).then(
                [](const XboxTokenHttpResponse& httpResponse) {
                    const auto response = JsRuntimeJson::parse(
                        httpResponse.bodyText
                    );
                    return std::pair<XboxTokenHttpResponse, JsRuntimeValue>(
                        httpResponse,
                        response
                    );
                }
            ).then(
                [state, options](
                    const std::pair<XboxTokenHttpResponse, JsRuntimeValue>& pair
                ) {
                    const auto& httpResponse = pair.first;
                    const auto& response = pair.second;
                    if (!httpResponse.ok()) {
                        throwTokenError(
                            getProperty(response, "XErr"),
                            response
                        );
                    }

                    const auto xsts = makeXstsResult(response);
                    auto cached = JsRuntimeValue::object();
                    cached.set(
                        XboxTokenManager::relyingPartyCacheKey(
                            getProperty(options, "relyingParty")
                        ),
                        xsts
                    );
                    return setCachedTokenState(
                        state,
                        std::move(cached)
                    ).then([xsts] { return xsts; });
                }
            );
        }
    );
}

} // namespace

struct CurlXboxTokenHttpClient::Impl {
    explicit Impl(std::shared_ptr<JsMicrotaskQueue> queue)
        : client(std::make_shared<CurlMsalHttpClient>(std::move(queue))) {}

    std::shared_ptr<CurlMsalHttpClient> client;
};

const std::string* XboxTokenHttpRequest::header(
    std::string_view name
) const noexcept {
    for (auto iterator = headers.rbegin(); iterator != headers.rend();
         ++iterator) {
        if (asciiCaseInsensitiveEqual(iterator->first, name)) {
            return &iterator->second;
        }
    }
    return nullptr;
}

const std::string* XboxTokenHttpResponse::header(
    std::string_view name
) const noexcept {
    for (const auto& item : headers) {
        if (asciiCaseInsensitiveEqual(item.first, name)) return &item.second;
    }
    return nullptr;
}

CurlXboxTokenHttpClient::CurlXboxTokenHttpClient(
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue
) : impl_(std::make_shared<Impl>(std::move(microtaskQueue))) {}

JsPromise<XboxTokenHttpResponse> CurlXboxTokenHttpClient::fetch(
    XboxTokenHttpRequest request
) {
    MsalHttpRequest nativeRequest;
    nativeRequest.method = request.method;
    nativeRequest.url = std::move(request.url);
    nativeRequest.headers = std::move(request.headers);
    nativeRequest.body = std::move(request.body);
    if (!nativeRequest.header("Content-Type")) {
        // WHATWG fetch extracts a string BodyInit as text/plain when the
        // caller did not supply a content type.
        nativeRequest.headers.emplace_back(
            "Content-Type",
            "text/plain;charset=UTF-8"
        );
    }

    return impl_->client->send(std::move(nativeRequest)).then(
        [](const MsalHttpResponse& response) {
            XboxTokenHttpResponse result;
            result.status = response.status;
            result.statusText = statusTextFor(response.status);
            result.headers = response.headers;
            result.bodyText = response.bodyText;
            return result;
        }
    );
}

XboxTokenManager::XboxTokenManager(
    XboxProofKey ecKey,
    AuthCachePtr cacheValue
) : XboxTokenManager(
        std::move(ecKey),
        std::move(cacheValue),
        {}
    ) {}

XboxTokenManager::XboxTokenManager(
    XboxProofKey ecKey,
    AuthCachePtr cacheValue,
    XboxTokenManagerDependencies dependencies
) : state_(std::make_shared<XboxTokenManagerState>(
        std::move(ecKey),
        std::move(cacheValue)
    )),
    key(state_->key),
    jwk(state_->jwk),
    cache(state_->cache),
    headers(state_->headers),
    forceRefresh(state_->forceRefresh) {
    if (!dependencies.microtaskQueue) {
        dependencies.microtaskQueue = JsMicrotaskQueue::create();
    }
    state_->microtaskQueue = std::move(dependencies.microtaskQueue);
    state_->dateNowMilliseconds = dependencies.dateNowMilliseconds
        ? std::move(dependencies.dateNowMilliseconds)
        : systemDateNowMilliseconds;
    state_->httpClient = dependencies.httpClient
        ? std::move(dependencies.httpClient)
        : std::make_shared<CurlXboxTokenHttpClient>(
            state_->microtaskQueue
        );
    state_->replayHttpClient = dependencies.replayHttpClient
        ? std::move(dependencies.replayHttpClient)
        : std::make_shared<CurlMsalHttpClient>(state_->microtaskQueue);

    state_->jwk = makeJwk(state_->key.jwk());
    state_->headers = JsRuntimeValue::object({
        {
            "Cache-Control",
            JsRuntimeValue::string("no-store, must-revalidate, no-cache")
        },
        {"x-xbl-contract-version", JsRuntimeValue::number(1)}
    });
}

JsPromise<void> XboxTokenManager::setCachedToken(JsRuntimeValue data) {
    return setCachedTokenState(state_, std::move(data));
}

JsPromise<JsRuntimeValue> XboxTokenManager::getCachedTokens(
    JsRuntimeValue relyingParty
) {
    const auto state = state_;
    return getCachedState(state).then(
        [state, relyingParty = std::move(relyingParty)](
            const JsRuntimeValue& cachedTokens
        ) {
            const auto xstsHash = XboxTokenManager::relyingPartyCacheKey(
                relyingParty
            );
            auto result = JsRuntimeValue::object();
            for (const std::string_view token : {
                    std::string_view("userToken"),
                    std::string_view("titleToken"),
                    std::string_view("deviceToken")
                }) {
                const auto cached = getProperty(cachedTokens, token);
                if (cached.truthy() && checkIfValid(
                        getProperty(cached, "NotAfter"),
                        state
                    )) {
                    result.set(std::string(token), JsRuntimeValue::object({
                        {"valid", JsRuntimeValue::boolean(true)},
                        {"token", getProperty(cached, "Token")},
                        {"data", cached}
                    }));
                } else {
                    result.set(std::string(token), JsRuntimeValue::object({
                        {"valid", JsRuntimeValue::boolean(false)}
                    }));
                }
            }

            const auto cachedXsts = getProperty(cachedTokens, xstsHash);
            if (cachedXsts.truthy() && checkIfValid(
                    getProperty(cachedXsts, "expiresOn"),
                    state
                )) {
                result.set("xstsToken", JsRuntimeValue::object({
                    {"valid", JsRuntimeValue::boolean(true)},
                    {"data", cachedXsts}
                }));
            } else {
                result.set("xstsToken", JsRuntimeValue::object({
                    {"valid", JsRuntimeValue::boolean(false)}
                }));
            }
            return result;
        }
    );
}

void XboxTokenManager::checkTokenError(
    const JsRuntimeValue& errorCode,
    const JsRuntimeValue& response
) const {
    throwTokenError(errorCode, response);
}

JsPromise<JsRuntimeValue> XboxTokenManager::getUserToken(
    JsRuntimeValue accessToken,
    JsRuntimeValue azure
) {
    const auto state = state_;
    return JsPromise<JsRuntimeValue>::fromSynchronous(
        state->microtaskQueue,
        [state,
         accessToken = std::move(accessToken),
         azure = std::move(azure)]() mutable {
            const std::string preamble = azure.truthy() ? "d=" : "t=";
            auto payload = JsRuntimeValue::object({
                {
                    "RelyingParty",
                    JsRuntimeValue::string("http://auth.xboxlive.com")
                },
                {"TokenType", JsRuntimeValue::string("JWT")},
                {"Properties", JsRuntimeValue::object({
                    {"AuthMethod", JsRuntimeValue::string("RPS")},
                    {
                        "SiteName",
                        JsRuntimeValue::string("user.auth.xboxlive.com")
                    },
                    {
                        "RpsTicket",
                        JsRuntimeValue::string(
                            preamble + jsToString(accessToken)
                        )
                    }
                })}
            });
            const auto body = stringifyRequired(payload);
            const auto signature = signatureBase64(
                state,
                kXboxUserAuth,
                body
            );
            auto requestHeaders = spreadObject(state->headers);
            requestHeaders.set(
                "signature",
                JsRuntimeValue::string(signature)
            );
            requestHeaders.set(
                "Content-Type",
                JsRuntimeValue::string("application/json")
            );
            requestHeaders.set(
                "accept",
                JsRuntimeValue::string("application/json")
            );
            requestHeaders.set(
                "x-xbl-contract-version",
                JsRuntimeValue::string("2")
            );

            return state->httpClient->fetch(makeRequest(
                std::string(kXboxUserAuth),
                std::move(requestHeaders),
                body
            )).then([state](const XboxTokenHttpResponse& response) {
                return checkStatus(response);
            }).then([state](const JsRuntimeValue& response) {
                auto cached = JsRuntimeValue::object({
                    {"userToken", response}
                });
                return setCachedTokenState(state, std::move(cached)).then(
                    [response] {
                        return getProperty(response, "Token");
                    }
                );
            });
        }
    );
}

JsPromise<JsRuntimeValue> XboxTokenManager::doSisuAuth(
    JsRuntimeValue accessToken,
    JsRuntimeValue deviceToken,
    JsRuntimeValue options
) {
    const auto state = state_;
    options = normalizedDefaultOptions(std::move(options));
    return JsPromise<JsRuntimeValue>::fromSynchronous(
        state->microtaskQueue,
        [state,
         accessToken = std::move(accessToken),
         deviceToken = std::move(deviceToken),
         options = std::move(options)]() mutable {
            auto payload = JsRuntimeValue::object({
                {
                    "AccessToken",
                    JsRuntimeValue::string(
                        "t=" + jsToString(accessToken)
                    )
                },
                {"AppId", getProperty(options, "authTitle")},
                {"DeviceToken", deviceToken},
                {"Sandbox", JsRuntimeValue::string("RETAIL")},
                {"UseModernGamertag", JsRuntimeValue::boolean(true)},
                {
                    "SiteName",
                    JsRuntimeValue::string("user.auth.xboxlive.com")
                },
                {"RelyingParty", getProperty(options, "relyingParty")},
                {"ProofKey", state->jwk}
            });
            const auto body = stringifyRequired(payload);
            auto requestHeaders = JsRuntimeValue::object({
                {
                    "Signature",
                    JsRuntimeValue::string(signatureBase64(
                        state,
                        kSisuAuthorize,
                        body
                    ))
                }
            });
            return state->httpClient->fetch(makeRequest(
                std::string(kSisuAuthorize),
                std::move(requestHeaders),
                body
            )).then([](const XboxTokenHttpResponse& httpResponse) {
                return std::pair<XboxTokenHttpResponse, JsRuntimeValue>(
                    httpResponse,
                    JsRuntimeJson::parse(httpResponse.bodyText)
                );
            }).then([state, options](
                const std::pair<XboxTokenHttpResponse, JsRuntimeValue>& pair
            ) {
                const auto& httpResponse = pair.first;
                const auto& response = pair.second;
                if (!httpResponse.ok()) {
                    throwTokenError(
                        parseIntHeader(httpResponse.header("x-err")),
                        response
                    );
                }

                const auto xsts = makeSisuXstsResult(response);
                auto cached = JsRuntimeValue::object({
                    {"userToken", getProperty(response, "UserToken")},
                    {"titleToken", getProperty(response, "TitleToken")}
                });
                cached.set(
                    XboxTokenManager::relyingPartyCacheKey(
                        getProperty(options, "relyingParty")
                    ),
                    xsts
                );
                return setCachedTokenState(
                    state,
                    std::move(cached)
                ).then([xsts] { return xsts; });
            });
        }
    );
}

JsPromise<JsRuntimeValue> XboxTokenManager::doReplayAuth(
    JsRuntimeValue email,
    JsRuntimeValue password,
    JsRuntimeValue options
) {
    const auto state = state_;
    options = normalizedDefaultOptions(std::move(options));
    return ValuePromise::fromSynchronous(
        state->microtaskQueue,
        [state,
         email = std::move(email),
         password = std::move(password),
         options = std::move(options)]() mutable {
            return replayPreAuth(state).then([
                state,
                email,
                password
            ](const JsRuntimeValue& preAuth) {
                return replayLogUser(
                    state,
                    preAuth,
                    email,
                    password
                );
            }).then([state](const JsRuntimeValue& login) {
                return replayExchangeUserToken(
                    state,
                    getProperty(login, "access_token")
                );
            }).then([state, options](const JsRuntimeValue& userToken) {
                return setCachedTokenState(
                    state,
                    JsRuntimeValue::object({
                        {"userToken", userToken}
                    })
                ).then([state, options, userToken] {
                    return getXstsTokenState(
                        state,
                        JsRuntimeValue::object({
                            {
                                "userToken",
                                getProperty(userToken, "Token")
                            }
                        }),
                        options
                    );
                });
            });
        }
    );
}

std::vector<std::uint8_t> XboxTokenManager::sign(
    std::string_view url,
    std::string_view authorizationToken,
    std::string_view payload
) const {
    return signState(state_, url, authorizationToken, payload);
}

JsPromise<JsRuntimeValue> XboxTokenManager::getDeviceToken(
    JsRuntimeValue asDevice
) {
    const auto state = state_;
    return JsPromise<JsRuntimeValue>::fromSynchronous(
        state->microtaskQueue,
        [state, asDevice = std::move(asDevice)]() mutable {
            auto properties = JsRuntimeValue::object();
            properties.set(
                "AuthMethod",
                JsRuntimeValue::string("ProofOfPossession")
            );
            properties.set(
                "Id",
                JsRuntimeValue::string("{" + nextUuid(state) + "}")
            );
            const auto deviceType = getProperty(asDevice, "deviceType");
            properties.set(
                "DeviceType",
                deviceType.truthy()
                    ? deviceType
                    : JsRuntimeValue::string("Nintendo")
            );
            properties.set(
                "SerialNumber",
                JsRuntimeValue::string("{" + nextUuid(state) + "}")
            );
            const auto deviceVersion = getProperty(
                asDevice,
                "deviceVersion"
            );
            properties.set(
                "Version",
                deviceVersion.truthy()
                    ? deviceVersion
                    : JsRuntimeValue::string("0.0.0")
            );
            properties.set("ProofKey", state->jwk);
            auto payload = JsRuntimeValue::object({
                {"Properties", std::move(properties)},
                {
                    "RelyingParty",
                    JsRuntimeValue::string("http://auth.xboxlive.com")
                },
                {"TokenType", JsRuntimeValue::string("JWT")}
            });
            const auto body = stringifyRequired(payload);
            auto requestHeaders = spreadObject(state->headers);
            requestHeaders.set(
                "Signature",
                JsRuntimeValue::string(signatureBase64(
                    state,
                    kXboxDeviceAuth,
                    body
                ))
            );
            return state->httpClient->fetch(makeRequest(
                std::string(kXboxDeviceAuth),
                std::move(requestHeaders),
                body
            )).then([](const XboxTokenHttpResponse& response) {
                return checkStatus(response);
            }).then([state](const JsRuntimeValue& response) {
                auto cached = JsRuntimeValue::object({
                    {"deviceToken", response}
                });
                return setCachedTokenState(state, std::move(cached)).then(
                    [response] {
                        return getProperty(response, "Token");
                    }
                );
            });
        }
    );
}

JsPromise<JsRuntimeValue> XboxTokenManager::getXSTSToken(
    JsRuntimeValue tokens,
    JsRuntimeValue options
) {
    return getXstsTokenState(
        state_,
        std::move(tokens),
        std::move(options)
    );
}

JsPromise<JsRuntimeValue> XboxTokenManager::getTitleToken(
    JsRuntimeValue msaAccessToken,
    JsRuntimeValue deviceToken
) {
    const auto state = state_;
    return JsPromise<JsRuntimeValue>::fromSynchronous(
        state->microtaskQueue,
        [state,
         msaAccessToken = std::move(msaAccessToken),
         deviceToken = std::move(deviceToken)]() mutable {
            auto payload = JsRuntimeValue::object({
                {"Properties", JsRuntimeValue::object({
                    {"AuthMethod", JsRuntimeValue::string("RPS")},
                    {"DeviceToken", deviceToken},
                    {
                        "RpsTicket",
                        JsRuntimeValue::string(
                            "t=" + jsToString(msaAccessToken)
                        )
                    },
                    {
                        "SiteName",
                        JsRuntimeValue::string("user.auth.xboxlive.com")
                    },
                    {"ProofKey", state->jwk}
                })},
                {
                    "RelyingParty",
                    JsRuntimeValue::string("http://auth.xboxlive.com")
                },
                {"TokenType", JsRuntimeValue::string("JWT")}
            });
            const auto body = stringifyRequired(payload);
            auto requestHeaders = spreadObject(state->headers);
            requestHeaders.set(
                "Signature",
                JsRuntimeValue::string(signatureBase64(
                    state,
                    kXboxTitleAuth,
                    body
                ))
            );
            return state->httpClient->fetch(makeRequest(
                std::string(kXboxTitleAuth),
                std::move(requestHeaders),
                body
            )).then([](const XboxTokenHttpResponse& response) {
                return checkStatus(response);
            }).then([state](const JsRuntimeValue& response) {
                auto cached = JsRuntimeValue::object({
                    {"titleToken", response}
                });
                return setCachedTokenState(state, std::move(cached)).then(
                    [response] {
                        return getProperty(response, "Token");
                    }
                );
            });
        }
    );
}

std::shared_ptr<JsMicrotaskQueue>
XboxTokenManager::microtaskQueue() const noexcept {
    return state_->microtaskQueue;
}

XboxTokenHttpClientPtr XboxTokenManager::httpClient() const noexcept {
    return state_->httpClient;
}

std::string XboxTokenManager::relyingPartyCacheKey(
    const JsRuntimeValue& relyingParty
) {
    if (relyingParty.isNull() || relyingParty.isUndefined()) {
        return sha1Prefix("");
    }
    if (!relyingParty.isString()) {
        throw std::invalid_argument(
            "The data argument must be of type string or an instance of "
            "Buffer, TypedArray, or DataView"
        );
    }
    return sha1Prefix(relyingParty.stringValue());
}

} // namespace bedrock
