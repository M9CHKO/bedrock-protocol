#include <bedrock/auth/LiveTokenManager.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bedrock {

struct LiveTokenManagerState {
    LiveTokenManagerState(
        JsRuntimeValue clientIdValue,
        JsRuntimeValue scopesValue,
        AuthCachePtr cacheValue
    ) : clientId(std::move(clientIdValue)),
        scopes(std::move(scopesValue)),
        cache(std::move(cacheValue)) {}

    JsRuntimeValue clientId;
    JsRuntimeValue scopes;
    AuthCachePtr cache;
    JsRuntimeValue forceRefresh = JsRuntimeValue::undefined();
    JsRuntimeValue polling = JsRuntimeValue::undefined();
    XboxTokenHttpClientPtr httpClient;
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue;
    std::function<double()> dateNowMilliseconds;
    std::function<JsPromise<void>(double)> delay;
    LiveTokenManagerObservers observers;
};

namespace {

using ValuePromise = JsPromise<JsRuntimeValue>;

struct DecodedCodePoint {
    std::uint32_t value = 0;
    std::size_t width = 0;
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
    if (first < 0x80U) return {first, 1};
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

std::vector<std::uint16_t> utf16Units(std::string_view input) {
    std::vector<std::uint16_t> result;
    for (std::size_t offset = 0; offset < input.size();) {
        auto decoded = decodeUtf8OrWtf8(input, offset);
        offset += decoded.width;
        if (decoded.value <= 0xffffU) {
            result.push_back(static_cast<std::uint16_t>(decoded.value));
            continue;
        }
        decoded.value -= 0x10000U;
        result.push_back(static_cast<std::uint16_t>(
            0xd800U + (decoded.value >> 10)
        ));
        result.push_back(static_cast<std::uint16_t>(
            0xdc00U + (decoded.value & 0x3ffU)
        ));
    }
    return result;
}

void appendWtf8CodeUnit(std::string& output, std::uint16_t unit) {
    if (unit <= 0x7fU) {
        output.push_back(static_cast<char>(unit));
    } else if (unit <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (unit >> 6)));
        output.push_back(static_cast<char>(0x80U | (unit & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xe0U | (unit >> 12)));
        output.push_back(static_cast<char>(
            0x80U | ((unit >> 6) & 0x3fU)
        ));
        output.push_back(static_cast<char>(0x80U | (unit & 0x3fU)));
    }
}

void appendUtf8Scalar(std::string& output, std::uint32_t value) {
    if (value <= 0x7fU) {
        output.push_back(static_cast<char>(value));
    } else if (value <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (value >> 6)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else if (value <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (value >> 12)));
        output.push_back(static_cast<char>(
            0x80U | ((value >> 6) & 0x3fU)
        ));
        output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (value >> 18)));
        output.push_back(static_cast<char>(
            0x80U | ((value >> 12) & 0x3fU)
        ));
        output.push_back(static_cast<char>(
            0x80U | ((value >> 6) & 0x3fU)
        ));
        output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    }
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

JsRuntimeValue destructuredToken(const JsRuntimeValue& cached) {
    if (cached.isUndefined() || cached.isNull()) {
        throw std::runtime_error(
            "Cannot destructure property 'token' of '(intermediate value)' "
            "as it is " + std::string(cached.isNull() ? "null." : "undefined.")
        );
    }
    return getProperty(cached, "token");
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
    if (value.isArray()) return stringToNumber(jsArrayJoin(value));
    if (value.isDate()) return value.dateMilliseconds();
    return std::numeric_limits<double>::quiet_NaN();
}

JsRuntimeValue toPrimitiveForAddition(const JsRuntimeValue& value) {
    if (!value.isObject() && !value.isArray() && !value.isDate() &&
        !value.isMap() && !value.isFunction() && !value.isOpaque()) {
        return value;
    }
    return JsRuntimeValue::string(jsToString(value));
}

JsRuntimeValue jsAdd(
    const JsRuntimeValue& leftValue,
    const JsRuntimeValue& rightValue
) {
    const auto left = toPrimitiveForAddition(leftValue);
    const auto right = toPrimitiveForAddition(rightValue);
    if (left.isString() || right.isString()) {
        return JsRuntimeValue::string(jsToString(left) + jsToString(right));
    }
    return JsRuntimeValue::number(toNumber(left) + toNumber(right));
}

double newDateMilliseconds(const JsRuntimeValue& value) {
    if (value.isDate()) return value.dateMilliseconds();
    if (value.isNumber()) {
        return JsRuntimeValue::date(value.numberValue()).dateMilliseconds();
    }
    // Normal Live cache entries are numbers. For all non-date strings we keep
    // Date.parse's invalid result instead of incorrectly applying Number().
    return std::numeric_limits<double>::quiet_NaN();
}

std::string formEncode(const JsRuntimeValue& value) {
    const auto units = utf16Units(jsToString(value));
    std::string usv;
    for (std::size_t index = 0; index < units.size(); ++index) {
        std::uint32_t scalar = units[index];
        if (scalar >= 0xd800U && scalar <= 0xdbffU) {
            if (index + 1 < units.size() && units[index + 1] >= 0xdc00U &&
                units[index + 1] <= 0xdfffU) {
                scalar = 0x10000U + ((scalar - 0xd800U) << 10) +
                    (units[++index] - 0xdc00U);
            } else {
                scalar = 0xfffdU;
            }
        } else if (scalar >= 0xdc00U && scalar <= 0xdfffU) {
            scalar = 0xfffdU;
        }
        appendUtf8Scalar(usv, scalar);
    }

    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char byte : usv) {
        const bool safe =
            (byte >= 'A' && byte <= 'Z') ||
            (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') ||
            byte == '*' || byte == '-' || byte == '.' || byte == '_';
        if (safe) {
            encoded.push_back(static_cast<char>(byte));
        } else if (byte == ' ') {
            encoded.push_back('+');
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[(byte >> 4U) & 0x0fU]);
            encoded.push_back(hex[byte & 0x0fU]);
        }
    }
    return encoded;
}

std::string formBody(
    const std::vector<std::pair<std::string_view, JsRuntimeValue>>& fields
) {
    std::string result;
    for (const auto& field : fields) {
        if (!result.empty()) result.push_back('&');
        result += formEncode(JsRuntimeValue::string(field.first));
        result.push_back('=');
        result += formEncode(field.second);
    }
    return result;
}

JsRuntimeValue formHeaders() {
    return JsRuntimeValue::object({
        {
            "Content-Type",
            JsRuntimeValue::string("application/x-www-form-urlencoded")
        }
    });
}

XboxTokenHttpRequest makeFormRequest(
    std::string url,
    std::string body,
    bool includeCredentials,
    std::optional<std::string> cookie = std::nullopt
) {
    XboxTokenHttpRequest request;
    request.method = "post";
    request.url = std::move(url);
    request.body = std::move(body);
    request.headersObject = formHeaders();
    request.headers.emplace_back(
        "Content-Type",
        "application/x-www-form-urlencoded"
    );
    if (cookie) {
        request.headersObject.set("Cookie", JsRuntimeValue::string(*cookie));
        request.headers.emplace_back("Cookie", std::move(*cookie));
    }
    if (includeCredentials) {
        request.credentials = JsRuntimeValue::string("include");
    }
    return request;
}

JsRuntimeValue requestDebugValue(const XboxTokenHttpRequest& request) {
    auto result = JsRuntimeValue::object({
        {"method", JsRuntimeValue::string(request.method)},
        {"body", JsRuntimeValue::string(request.body)},
        {"headers", request.headersObject}
    });
    if (!request.credentials.isUndefined()) {
        result.set("credentials", request.credentials);
    }
    return result;
}

JsRuntimeValue spreadObject(const JsRuntimeValue& source) {
    auto result = JsRuntimeValue::object();
    if (source.isNull() || source.isUndefined()) return result;
    if (source.isString()) {
        const auto units = utf16Units(source.stringValue());
        for (std::size_t index = 0; index < units.size(); ++index) {
            std::string character;
            appendWtf8CodeUnit(character, units[index]);
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

double systemDateNowMilliseconds() {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

JsPromise<void> defaultDelay(
    const std::shared_ptr<JsMicrotaskQueue>& queue,
    double milliseconds
) {
    return JsPromise<void>::create(
        queue,
        [milliseconds](
            JsPromise<void>::ResolveFunction resolve,
            JsPromise<void>::RejectFunction reject
        ) mutable {
            double effective = milliseconds;
            if (!std::isfinite(effective) || effective < 1.0 ||
                effective > 2147483647.0) {
                effective = 1.0;
            }
            effective = std::trunc(effective);
            try {
                std::thread timer([
                    effective,
                    resolve = std::move(resolve)
                ]() mutable {
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        static_cast<std::int64_t>(effective)
                    ));
                    resolve();
                });
                timer.detach();
            } catch (...) {
                reject(std::current_exception());
            }
        }
    );
}

void debug(
    const std::shared_ptr<LiveTokenManagerState>& state,
    const std::string& label,
    std::vector<JsRuntimeValue> arguments = {}
) {
    if (state->observers.debug) {
        state->observers.debug(label, arguments);
    }
}

AuthCache::GetCachedMethod requireGetCached(
    const std::shared_ptr<LiveTokenManagerState>& state
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

AuthCache::SetCachedPartialMethod requireSetCachedPartial(
    const std::shared_ptr<LiveTokenManagerState>& state
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

ValuePromise getCached(
    const std::shared_ptr<LiveTokenManagerState>& state
) {
    return ValuePromise::fromSynchronous(
        state->microtaskQueue,
        [state] {
            auto method = requireGetCached(state);
            return method();
        }
    );
}

ValuePromise cachedToken(
    const std::shared_ptr<LiveTokenManagerState>& state,
    bool accessToken
) {
    return getCached(state).then([state, accessToken](
        const JsRuntimeValue& cached
    ) {
        const auto token = destructuredToken(cached);
        if (!token.truthy()) return JsRuntimeValue::undefined();

        const auto obtainedOn = getProperty(token, "obtainedOn");
        const auto expiresIn = getProperty(token, "expires_in");
        const double until = newDateMilliseconds(
            jsAdd(obtainedOn, expiresIn)
        ) - state->dateNowMilliseconds();
        return JsRuntimeValue::object({
            {"valid", JsRuntimeValue::boolean(until > 1000.0)},
            {"until", JsRuntimeValue::number(until)},
            {
                "token",
                getProperty(
                    token,
                    accessToken ? "access_token" : "refresh_token"
                )
            }
        });
    });
}

JsPromise<void> updateCacheState(
    const std::shared_ptr<LiveTokenManagerState>& state,
    JsRuntimeValue data
) {
    return JsPromise<void>::fromSynchronous(
        state->microtaskQueue,
        [state, data = std::move(data)]() mutable {
            // Resolve the method before evaluating the argument, matching a
            // JavaScript member-call expression.
            auto method = requireSetCachedPartial(state);
            auto token = spreadObject(data);
            token.set(
                "obtainedOn",
                JsRuntimeValue::number(state->dateNowMilliseconds())
            );
            return method(JsRuntimeValue::object({
                {"token", std::move(token)}
            }));
        }
    );
}

void observeIgnoredUpdate(
    const std::shared_ptr<LiveTokenManagerState>& state,
    JsPromise<void> update
) {
    if (!state->observers.unhandledRejection) return;
    (void) update.catchError([state](std::exception_ptr error) {
        state->observers.unhandledRejection(std::move(error));
    });
}

JsRuntimeValue checkStatus(const XboxTokenHttpResponse& response) {
    if (response.ok()) return JsRuntimeJson::parse(response.bodyText);
    throw std::runtime_error(
        std::to_string(response.status) + " " + response.statusText + " " +
        response.bodyText
    );
}

ValuePromise refreshTokensState(
    const std::shared_ptr<LiveTokenManagerState>& state
) {
    return cachedToken(state, false).then([state](
        const JsRuntimeValue& refreshToken
    ) {
        if (!refreshToken.truthy()) {
            throw std::runtime_error("Cannot refresh without refresh token");
        }

        const auto body = formBody({
            {"scope", state->scopes},
            {"client_id", state->clientId},
            {"grant_type", JsRuntimeValue::string("refresh_token")},
            {"refresh_token", getProperty(refreshToken, "token")}
        });
        auto request = makeFormRequest(
            std::string(LiveTokenManager::LiveTokenRequest),
            body,
            true
        );
        return state->httpClient->fetch(std::move(request)).then(
            [](const XboxTokenHttpResponse& response) {
                return checkStatus(response);
            }
        ).then([state](const JsRuntimeValue& token) {
            // The JavaScript source deliberately does not await updateCache.
            observeIgnoredUpdate(state, updateCacheState(state, token));
            return token;
        });
    });
}

void beginVerificationAfterAccess(
    const std::shared_ptr<LiveTokenManagerState>& state,
    ValuePromise::ResolveFunction resolve,
    ValuePromise::RejectFunction reject
) {
    auto access = cachedToken(state, true);
    auto accessStep = access.then([state, resolve, reject](
        const JsRuntimeValue& accessToken
    ) mutable {
        auto refresh = cachedToken(state, false);
        auto refreshStep = refresh.then([
            state,
            accessToken,
            resolve,
            reject
        ](const JsRuntimeValue& refreshToken) mutable {
            if (!accessToken.truthy() || !refreshToken.truthy()) {
                resolve(JsRuntimeValue::boolean(false));
                return JsRuntimeValue::undefined();
            }

            debug(state, "[live] have at, rt", {
                accessToken,
                refreshToken
            });
            if (getProperty(accessToken, "valid").truthy() &&
                refreshToken.truthy()) {
                resolve(JsRuntimeValue::boolean(true));
                return JsRuntimeValue::undefined();
            }

            auto renewed = refreshTokensState(state);
            auto renewedStep = renewed.then([resolve](
                const JsRuntimeValue&
            ) mutable {
                resolve(JsRuntimeValue::boolean(true));
                return JsRuntimeValue::undefined();
            });
            (void) renewedStep.catchError([
                state,
                resolve,
                reject
            ](std::exception_ptr error) mutable {
                try {
                    if (state->observers.warn) {
                        state->observers.warn(
                            "Error refreshing token",
                            error
                        );
                    }
                    resolve(JsRuntimeValue::boolean(false));
                } catch (...) {
                    reject(std::current_exception());
                }
                return JsRuntimeValue::undefined();
            });
            return JsRuntimeValue::undefined();
        });
        (void) refreshStep.catchError([reject](
            std::exception_ptr error
        ) mutable {
            reject(std::move(error));
            return JsRuntimeValue::undefined();
        });
        return JsRuntimeValue::undefined();
    });
    (void) accessStep.catchError([reject](std::exception_ptr error) mutable {
        reject(std::move(error));
        return JsRuntimeValue::undefined();
    });
}

ValuePromise verifyTokensState(
    const std::shared_ptr<LiveTokenManagerState>& state
) {
    return ValuePromise::create(
        state->microtaskQueue,
        [state](
            ValuePromise::ResolveFunction resolve,
            ValuePromise::RejectFunction reject
        ) mutable {
            if (!state->forceRefresh.truthy()) {
                beginVerificationAfterAccess(
                    state,
                    std::move(resolve),
                    std::move(reject)
                );
                return;
            }

            auto initial = refreshTokensState(state);
            auto proceed = [state, resolve, reject]() mutable {
                beginVerificationAfterAccess(
                    state,
                    std::move(resolve),
                    std::move(reject)
                );
                return JsRuntimeValue::undefined();
            };
            auto initialStep = initial.then([
                proceed
            ](const JsRuntimeValue&) mutable {
                return proceed();
            });
            (void) initialStep.catchError([
                proceed
            ](std::exception_ptr) mutable {
                return proceed();
            });
        }
    );
}

struct DeviceCodeOperation {
    std::shared_ptr<LiveTokenManagerState> state;
    std::function<void(JsRuntimeValue)> callback;
    double acquireTime = 0.0;
    double expireTime = 0.0;
    JsRuntimeValue response = JsRuntimeValue::undefined();
    std::vector<std::string> cookies;
};

ValuePromise pollDeviceCode(
    const std::shared_ptr<DeviceCodeOperation>& operation
);

JsPromise<std::optional<JsRuntimeValue>> pollOnce(
    const std::shared_ptr<DeviceCodeOperation>& operation
) {
    const auto state = operation->state;
    const double delayMilliseconds =
        toNumber(getProperty(operation->response, "interval")) * 1000.0;

    auto attempt = JsPromise<std::optional<JsRuntimeValue>>::fromSynchronous(
        state->microtaskQueue,
        [operation, state, delayMilliseconds] {
            return state->delay(delayMilliseconds).then([operation, state] {
                const auto body = formBody({
                    {"client_id", state->clientId},
                    {
                        "device_code",
                        getProperty(operation->response, "device_code")
                    },
                    {
                        "grant_type",
                        JsRuntimeValue::string(
                            "urn:ietf:params:oauth:grant-type:device_code"
                        )
                    }
                });
                std::string cookie;
                for (std::size_t index = 0;
                     index < operation->cookies.size(); ++index) {
                    if (index) cookie += "; ";
                    cookie += operation->cookies[index];
                }
                auto request = makeFormRequest(
                    std::string(LiveTokenManager::LiveTokenRequest) +
                        "?client_id=" + jsToString(state->clientId),
                    body,
                    false,
                    std::move(cookie)
                );
                return state->httpClient->fetch(std::move(request)).then(
                    [](const XboxTokenHttpResponse& response) {
                        return JsRuntimeJson::parse(response.bodyText);
                    }
                ).then([state](const JsRuntimeValue& response) {
                    const auto error = getProperty(response, "error");
                    if (error.truthy()) {
                        if (error.strictlyEquals(JsRuntimeValue::string(
                                "authorization_pending"
                            ))) {
                            debug(state, "[live] Still waiting:", {
                                getProperty(response, "error_description")
                            });
                            return std::optional<JsRuntimeValue> {};
                        }
                        throw std::runtime_error(
                            "Failed to acquire authorization code from "
                            "device token (" + jsToString(error) + ") - " +
                            jsToString(getProperty(
                                response,
                                "error_description"
                            ))
                        );
                    }
                    return std::optional<JsRuntimeValue> {response};
                });
            });
        }
    );

    return attempt.catchError([state](std::exception_ptr error) {
        if (state->observers.consoleDebug) {
            state->observers.consoleDebug(std::move(error));
        }
        return std::optional<JsRuntimeValue> {};
    });
}

ValuePromise pollDeviceCode(
    const std::shared_ptr<DeviceCodeOperation>& operation
) {
    const auto state = operation->state;
    if (!state->polling.truthy() ||
        !(operation->expireTime > state->dateNowMilliseconds())) {
        state->polling = JsRuntimeValue::boolean(false);
        return ValuePromise::rejected(
            state->microtaskQueue,
            "Authentication failed, timed out"
        );
    }

    return pollOnce(operation).then([operation](
        const std::optional<JsRuntimeValue>& token
    ) -> ValuePromise {
        const auto state = operation->state;
        if (!token || !token->truthy()) {
            return pollDeviceCode(operation);
        }

        observeIgnoredUpdate(state, updateCacheState(state, *token));
        state->polling = JsRuntimeValue::boolean(false);
        return ValuePromise::resolved(
            state->microtaskQueue,
            JsRuntimeValue::object({
                {"accessToken", getProperty(*token, "access_token")}
            })
        );
    });
}

ValuePromise authDeviceCodeState(
    const std::shared_ptr<LiveTokenManagerState>& state,
    std::function<void(JsRuntimeValue)> callback
) {
    auto operation = std::make_shared<DeviceCodeOperation>();
    operation->state = state;
    operation->callback = std::move(callback);
    operation->acquireTime = state->dateNowMilliseconds();

    const auto body = formBody({
        {"scope", state->scopes},
        {"client_id", state->clientId},
        {"response_type", JsRuntimeValue::string("device_code")}
    });
    auto request = makeFormRequest(
        std::string(LiveTokenManager::LiveDeviceCodeRequest),
        body,
        true
    );
    debug(state, "Requesting live device token", {
        requestDebugValue(request)
    });

    return JsPromise<XboxTokenHttpResponse>::fromSynchronous(
        state->microtaskQueue,
        [state, request = std::move(request)]() mutable {
            return state->httpClient->fetch(std::move(request));
        }
    ).then([operation](const XboxTokenHttpResponse& response) {
        const auto state = operation->state;
        if (response.status != 200) {
            if (state->observers.consoleWarnText) {
                const auto warn = state->observers.consoleWarnText;
                const auto bodyText = response.bodyText;
                state->microtaskQueue->enqueue([
                    warn,
                    bodyText
                ] {
                    warn(bodyText);
                });
            }
            throw std::runtime_error("Failed to request live.com device code");
        }
        if (const auto* cookie = response.header("set-cookie")) {
            const auto separator = cookie->find(';');
            operation->cookies.push_back(cookie->substr(0, separator));
        }
        return response;
    }).then([](const XboxTokenHttpResponse& response) {
        return checkStatus(response);
    }).then([operation](const JsRuntimeValue& response) {
        auto mutableResponse = response;
        const auto verificationUri = jsToString(getProperty(
            mutableResponse,
            "verification_uri"
        ));
        const auto userCode = jsToString(getProperty(
            mutableResponse,
            "user_code"
        ));
        mutableResponse.set(
            "message",
            JsRuntimeValue::string(
                "To sign in, use a web browser to open the page " +
                verificationUri + " and use the code " + userCode +
                " or visit http://microsoft.com/link?otc=" + userCode
            )
        );
        if (!operation->callback) {
            throw std::runtime_error("deviceCodeCallback is not a function");
        }
        operation->callback(mutableResponse);
        return mutableResponse;
    }).then([operation](const JsRuntimeValue& response) {
        operation->response = response;
        operation->expireTime = operation->acquireTime +
            toNumber(getProperty(response, "expires_in")) * 1000.0 - 100.0;
        operation->state->polling = JsRuntimeValue::boolean(true);
        return pollDeviceCode(operation);
    });
}

} // namespace

LiveTokenManager::LiveTokenManager(
    JsRuntimeValue clientIdValue,
    JsRuntimeValue scopesValue,
    AuthCachePtr cacheValue
) : LiveTokenManager(
        std::move(clientIdValue),
        std::move(scopesValue),
        std::move(cacheValue),
        {}
    ) {}

LiveTokenManager::LiveTokenManager(
    JsRuntimeValue clientIdValue,
    JsRuntimeValue scopesValue,
    AuthCachePtr cacheValue,
    LiveTokenManagerDependencies dependencies
) : state_(std::make_shared<LiveTokenManagerState>(
        std::move(clientIdValue),
        std::move(scopesValue),
        std::move(cacheValue)
    )),
    clientId(state_->clientId),
    scopes(state_->scopes),
    cache(state_->cache),
    forceRefresh(state_->forceRefresh),
    polling(state_->polling) {
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
    state_->observers = std::move(dependencies.observers);
    if (dependencies.delay) {
        state_->delay = std::move(dependencies.delay);
    } else {
        const auto queue = state_->microtaskQueue;
        state_->delay = [queue](double milliseconds) {
            return defaultDelay(queue, milliseconds);
        };
    }
}

ValuePromise LiveTokenManager::verifyTokens() {
    return verifyTokensState(state_);
}

ValuePromise LiveTokenManager::refreshTokens() {
    return refreshTokensState(state_);
}

ValuePromise LiveTokenManager::getAccessToken() {
    return cachedToken(state_, true);
}

ValuePromise LiveTokenManager::getRefreshToken() {
    return cachedToken(state_, false);
}

JsPromise<void> LiveTokenManager::updateCache(JsRuntimeValue data) {
    return updateCacheState(state_, std::move(data));
}

ValuePromise LiveTokenManager::authDeviceCode(
    std::function<void(JsRuntimeValue response)> deviceCodeCallback
) {
    return authDeviceCodeState(state_, std::move(deviceCodeCallback));
}

std::shared_ptr<JsMicrotaskQueue>
LiveTokenManager::microtaskQueue() const noexcept {
    return state_->microtaskQueue;
}

XboxTokenHttpClientPtr LiveTokenManager::httpClient() const noexcept {
    return state_->httpClient;
}

} // namespace bedrock
