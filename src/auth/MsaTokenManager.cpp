#include <bedrock/auth/MsaTokenManager.hpp>
#include <bedrock/auth/MsalError.hpp>
#include <bedrock/auth/NativeMsalPublicClientApplication.hpp>

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace bedrock {
namespace {

using ValuePromise = JsPromise<JsRuntimeValue>;

std::vector<std::uint16_t> utf16CodeUnits(std::string_view input) {
    std::vector<std::uint16_t> result;
    for (std::size_t offset = 0; offset < input.size();) {
        const auto first = static_cast<std::uint8_t>(input[offset]);
        std::uint32_t codePoint = first;
        std::size_t width = 1;
        const auto continuation = [&](std::size_t index) {
            return index < input.size() &&
                (static_cast<std::uint8_t>(input[index]) & 0xc0U) == 0x80U;
        };

        if (first >= 0xc2U && first <= 0xdfU && continuation(offset + 1)) {
            codePoint = ((first & 0x1fU) << 6) |
                (static_cast<std::uint8_t>(input[offset + 1]) & 0x3fU);
            width = 2;
        } else if (first >= 0xe0U && first <= 0xefU &&
            continuation(offset + 1) && continuation(offset + 2)) {
            const auto second =
                static_cast<std::uint8_t>(input[offset + 1]);
            if ((first != 0xe0U || second >= 0xa0U)) {
                codePoint = ((first & 0x0fU) << 12) |
                    ((second & 0x3fU) << 6) |
                    (static_cast<std::uint8_t>(input[offset + 2]) & 0x3fU);
                width = 3;
            }
        } else if (first >= 0xf0U && first <= 0xf4U &&
            continuation(offset + 1) && continuation(offset + 2) &&
            continuation(offset + 3)) {
            const auto second =
                static_cast<std::uint8_t>(input[offset + 1]);
            if ((first != 0xf0U || second >= 0x90U) &&
                (first != 0xf4U || second < 0x90U)) {
                codePoint = ((first & 0x07U) << 18) |
                    ((second & 0x3fU) << 12) |
                    ((static_cast<std::uint8_t>(input[offset + 2]) & 0x3fU)
                        << 6) |
                    (static_cast<std::uint8_t>(input[offset + 3]) & 0x3fU);
                width = 4;
            }
        }

        if (codePoint <= 0xffffU) {
            result.push_back(static_cast<std::uint16_t>(codePoint));
        } else {
            codePoint -= 0x10000U;
            result.push_back(static_cast<std::uint16_t>(
                0xd800U + (codePoint >> 10)
            ));
            result.push_back(static_cast<std::uint16_t>(
                0xdc00U + (codePoint & 0x3ffU)
            ));
        }
        offset += width;
    }
    return result;
}

std::string wtf8CodeUnit(std::uint16_t unit) {
    std::string result;
    if (unit <= 0x7fU) {
        result.push_back(static_cast<char>(unit));
    } else if (unit <= 0x7ffU) {
        result.push_back(static_cast<char>(0xc0U | (unit >> 6)));
        result.push_back(static_cast<char>(0x80U | (unit & 0x3fU)));
    } else {
        result.push_back(static_cast<char>(0xe0U | (unit >> 12)));
        result.push_back(static_cast<char>(0x80U | ((unit >> 6) & 0x3fU)));
        result.push_back(static_cast<char>(0x80U | (unit & 0x3fU)));
    }
    return result;
}

bool canonicalArrayIndex(std::string_view key, std::uint32_t& result) {
    if (key.empty() || (key.size() > 1 && key.front() == '0')) return false;
    std::uint64_t parsed = 0;
    const auto conversion = std::from_chars(
        key.data(),
        key.data() + key.size(),
        parsed
    );
    if (conversion.ec != std::errc() ||
        conversion.ptr != key.data() + key.size() ||
        parsed > 4294967294ULL) {
        return false;
    }
    result = static_cast<std::uint32_t>(parsed);
    return true;
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
    if (const auto* found = value.get(property)) return *found;

    if (value.isString()) {
        const auto units = utf16CodeUnits(value.stringValue());
        if (property == "length") {
            return JsRuntimeValue::number(static_cast<double>(units.size()));
        }
        std::uint32_t index = 0;
        if (canonicalArrayIndex(property, index) && index < units.size()) {
            return JsRuntimeValue::string(wtf8CodeUnit(units[index]));
        }
    }
    return JsRuntimeValue::undefined();
}

void setProperty(
    JsRuntimeValue& value,
    std::string property,
    JsRuntimeValue member
) {
    if (value.isObject() || value.isArray()) {
        value.set(std::move(property), std::move(member));
        return;
    }
    if (value.isUndefined() || value.isNull()) {
        throw std::runtime_error(
            std::string("Cannot set properties of ") +
            (value.isNull() ? "null" : "undefined") + " (setting '" +
            property + "')"
        );
    }
    if (value.isBool()) {
        throw std::runtime_error(
            "Cannot create property '" + property + "' on boolean '" +
            (value.boolValue() ? std::string("true") : std::string("false")) +
            "'"
        );
    }
    throw std::runtime_error(
        "Cannot create property '" + property + "' on primitive value"
    );
}

std::vector<JsRuntimeValue> objectValues(const JsRuntimeValue& value) {
    if (value.isUndefined() || value.isNull()) {
        throw std::runtime_error("Cannot convert undefined or null to object");
    }
    std::vector<JsRuntimeValue> result;
    if (value.isObject() || value.isArray()) {
        const auto properties = value.ownProperties();
        result.reserve(properties.size());
        for (const auto& property : properties) {
            result.push_back(property.value);
        }
        return result;
    }
    if (value.isString()) {
        const auto units = utf16CodeUnits(value.stringValue());
        result.reserve(units.size());
        for (const auto unit : units) {
            result.push_back(JsRuntimeValue::string(wtf8CodeUnit(unit)));
        }
    }
    return result;
}

std::string trimAsciiWhitespace(std::string value) {
    const auto whitespace = [](unsigned char character) {
        return character == ' ' || character == '\t' || character == '\n' ||
            character == '\r' || character == '\f' || character == '\v';
    };
    std::size_t begin = 0;
    while (begin < value.size() && whitespace(value[begin])) ++begin;
    std::size_t end = value.size();
    while (end > begin && whitespace(value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

std::string primitiveToString(const JsRuntimeValue& value);

std::string arrayToString(const JsRuntimeValue& value) {
    std::string result;
    const auto& elements = value.arrayNode()->elements();
    for (std::size_t index = 0; index < elements.size(); ++index) {
        if (index) result.push_back(',');
        if (!elements[index] || elements[index]->isUndefined() ||
            elements[index]->isNull()) {
            continue;
        }
        result += primitiveToString(*elements[index]);
    }
    return result;
}

std::string primitiveToString(const JsRuntimeValue& value) {
    if (value.isUndefined()) return "undefined";
    if (value.isNull()) return "null";
    if (value.isBool()) return value.boolValue() ? "true" : "false";
    if (value.isString()) return value.stringValue();
    if (value.isNumber()) {
        const auto serialized = JsRuntimeJson::stringify(value);
        return serialized.value_or("undefined");
    }
    if (value.isArray()) return arrayToString(value);
    if (value.isFunction()) return "function () { [native code] }";
    return "[object Object]";
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
    return std::numeric_limits<double>::quiet_NaN();
}

double timeClip(double value) {
    if (!std::isfinite(value) || std::fabs(value) > 8.64e15) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::trunc(value);
}

double dateNow(const std::shared_ptr<MsaTokenManagerRuntimeState>& runtime) {
    if (runtime->observers.dateNowMilliseconds) {
        return runtime->observers.dateNowMilliseconds();
    }
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

void debug(
    const std::shared_ptr<MsaTokenManagerRuntimeState>& runtime,
    const std::string& label,
    std::vector<JsRuntimeValue> arguments = {}
) {
    if (runtime->observers.debug) {
        runtime->observers.debug(label, arguments);
    }
}

std::optional<std::string> stringifyException(std::exception_ptr error) {
    return stringifyMsalException(std::move(error));
}

void warn(
    const std::shared_ptr<MsaTokenManagerRuntimeState>& runtime,
    const std::string& message,
    std::exception_ptr error
) {
    if (runtime->observers.warn) runtime->observers.warn(message, error);
}

void consoleDebug(
    const std::shared_ptr<MsaTokenManagerRuntimeState>& runtime,
    const std::optional<std::string>& serialized
) {
    if (runtime->observers.consoleDebug) {
        runtime->observers.consoleDebug(serialized);
    }
}

AuthCache::GetCachedMethod requireGetCached(
    const std::shared_ptr<MsaTokenManagerState>& manager
) {
    if (!manager->cache) {
        throw std::runtime_error(
            "Cannot read properties of null (reading 'getCached')"
        );
    }
    if (!manager->cache->hasGetCachedMethod()) {
        throw std::runtime_error(
            "this.cache.getCached is not a function"
        );
    }
    return manager->cache->requireGetCachedMethod();
}

AuthCache::SetCachedPartialMethod requireSetCachedPartial(
    const std::shared_ptr<MsaTokenManagerState>& manager
) {
    if (!manager->cache) {
        throw std::runtime_error(
            "Cannot read properties of null (reading 'setCachedPartial')"
        );
    }
    if (!manager->cache->hasSetCachedPartialMethod()) {
        throw std::runtime_error(
            "this.cache.setCachedPartial is not a function"
        );
    }
    return manager->cache->requireSetCachedPartialMethod();
}

JsRuntimeValue destructuredMember(
    const JsRuntimeValue& intermediate,
    const char* member
) {
    if (intermediate.isUndefined() || intermediate.isNull()) {
        throw std::runtime_error(
            std::string("Cannot destructure property '") + member +
            "' of '(intermediate value)' as it is " +
            (intermediate.isNull() ? "null." : "undefined.")
        );
    }
    return getProperty(intermediate, member);
}

std::optional<JsRuntimeValue> firstMatchingAccount(
    const JsRuntimeValue& tokens,
    const std::shared_ptr<MsaTokenManagerState>& manager
) {
    const auto values = objectValues(tokens);
    std::vector<JsRuntimeValue> matches;
    for (const auto& value : values) {
        const auto clientId = getProperty(value, "client_id");
        // The source reads this.msaClientId for every filter callback.
        if (clientId.strictlyEquals(manager->msaClientId)) {
            matches.push_back(value);
        }
    }
    if (matches.empty()) return std::nullopt;
    return matches.front();
}

ValuePromise cachedToken(
    const std::shared_ptr<MsaTokenManagerState>& manager,
    const std::shared_ptr<MsaTokenManagerRuntimeState>& runtime,
    bool accessToken
) {
    try {
        auto getCached = requireGetCached(manager);
        auto cached = ValuePromise::fromFuture(
            runtime->microtaskQueue,
            getCached()
        );
        return cached.then([manager, runtime, accessToken](
            const JsRuntimeValue& value
        ) {
            const char* root = accessToken ? "AccessToken" : "RefreshToken";
            auto tokens = destructuredMember(value, root);
            if (!tokens.truthy()) return JsRuntimeValue::undefined();

            auto account = firstMatchingAccount(tokens, manager);
            if (!account || !account->truthy()) {
                debug(
                    runtime,
                    accessToken
                        ? "[msa] No valid access token found"
                        : "[msa] No valid refresh token found",
                    {tokens}
                );
                return JsRuntimeValue::undefined();
            }

            if (!accessToken) {
                return JsRuntimeValue::object({
                    {"token", getProperty(*account, "secret")}
                });
            }

            const auto expiresOn = getProperty(*account, "expires_on");
            const double until = timeClip(toNumber(expiresOn) * 1000.0) -
                dateNow(runtime);
            return JsRuntimeValue::object({
                {"valid", JsRuntimeValue::boolean(until > 1000.0)},
                {"until", JsRuntimeValue::number(until)},
                {"token", getProperty(*account, "secret")}
            });
        });
    } catch (...) {
        return ValuePromise::rejected(
            runtime->microtaskQueue,
            std::current_exception()
        );
    }
}

ValuePromise refreshTokens(
    const std::shared_ptr<MsaTokenManagerState>& manager,
    const std::shared_ptr<MsaTokenManagerRuntimeState>& runtime
) {
    auto refresh = cachedToken(manager, runtime, false);
    return refresh.then([manager, runtime](const JsRuntimeValue& rtoken) {
        if (!rtoken.truthy()) {
            throw std::runtime_error("Cannot refresh without refresh token");
        }

        auto request = JsRuntimeValue::object({
            {"refreshToken", getProperty(rtoken, "token")},
            {"scopes", runtime->scopes}
        });

        return ValuePromise::create(
            runtime->microtaskQueue,
            [runtime, request = std::move(request)](
                ValuePromise::ResolveFunction resolve,
                ValuePromise::RejectFunction reject
            ) mutable {
                if (!runtime->msalApp) {
                    throw std::runtime_error(
                        "Cannot read properties of null (reading "
                        "'acquireTokenByRefreshToken')"
                    );
                }
                auto acquired = runtime->msalApp->
                    acquireTokenByRefreshToken(std::move(request));
                auto success = acquired.then(
                    [runtime, resolve = std::move(resolve)](
                        const JsRuntimeValue& response
                    ) mutable {
                        const auto serialized =
                            JsRuntimeJson::stringify(response);
                        debug(
                            runtime,
                            "[msa] refreshed token",
                            {serialized
                                ? JsRuntimeValue::string(*serialized)
                                : JsRuntimeValue::undefined()}
                        );
                        resolve(response);
                        return JsRuntimeValue::undefined();
                    }
                );
                (void) success.catchError(
                    [runtime, reject = std::move(reject)](
                        std::exception_ptr error
                    ) mutable {
                        const auto serialized = stringifyException(error);
                        debug(
                            runtime,
                            "[msa] failed to refresh",
                            {serialized
                                ? JsRuntimeValue::string(*serialized)
                                : JsRuntimeValue::undefined()}
                        );
                        reject(error);
                        return JsRuntimeValue::undefined();
                    }
                );
            }
        );
    });
}

void rejectFromIgnored(
    const std::shared_ptr<MsaTokenManagerRuntimeState>& runtime,
    std::exception_ptr error
) {
    if (runtime->observers.unhandledRejection) {
        runtime->observers.unhandledRejection(error);
    }
}

void beginVerificationAfterAccess(
    const std::shared_ptr<MsaTokenManagerState>& manager,
    const std::shared_ptr<MsaTokenManagerRuntimeState>& runtime,
    ValuePromise::ResolveFunction resolve,
    ValuePromise::RejectFunction reject
) {
    auto access = cachedToken(manager, runtime, true);
    auto accessStep = access.then(
        [manager, runtime, resolve, reject](const JsRuntimeValue& at) mutable {
            auto refresh = cachedToken(manager, runtime, false);
            auto refreshStep = refresh.then(
                [manager, runtime, at, resolve, reject](
                    const JsRuntimeValue& rt
                ) mutable {
                    if (!at.truthy() || !rt.truthy()) {
                        resolve(JsRuntimeValue::boolean(false));
                        return JsRuntimeValue::undefined();
                    }

                    debug(runtime, "[msa] have at, rt", {at, rt});
                    if (getProperty(at, "valid").truthy()) {
                        resolve(JsRuntimeValue::boolean(true));
                        return JsRuntimeValue::undefined();
                    }

                    auto renewed = refreshTokens(manager, runtime);
                    auto renewedSuccess = renewed.then(
                        [resolve](const JsRuntimeValue&) mutable {
                            resolve(JsRuntimeValue::boolean(true));
                            return JsRuntimeValue::undefined();
                        }
                    );
                    (void) renewedSuccess.catchError(
                        [runtime, resolve, reject](
                            std::exception_ptr error
                        ) mutable {
                            try {
                                warn(runtime, "Error refreshing token", error);
                                resolve(JsRuntimeValue::boolean(false));
                            } catch (...) {
                                reject(std::current_exception());
                            }
                            return JsRuntimeValue::undefined();
                        }
                    );
                    return JsRuntimeValue::undefined();
                }
            );
            (void) refreshStep.catchError(
                [reject](std::exception_ptr error) mutable {
                    reject(error);
                    return JsRuntimeValue::undefined();
                }
            );
            return JsRuntimeValue::undefined();
        }
    );
    (void) accessStep.catchError(
        [reject](std::exception_ptr error) mutable {
            reject(error);
            return JsRuntimeValue::undefined();
        }
    );
}

ValuePromise verifyTokens(
    const std::shared_ptr<MsaTokenManagerState>& manager,
    const std::shared_ptr<MsaTokenManagerRuntimeState>& runtime
) {
    return ValuePromise::create(
        runtime->microtaskQueue,
        [manager, runtime](
            ValuePromise::ResolveFunction resolve,
            ValuePromise::RejectFunction reject
        ) mutable {
            if (!runtime->forceRefresh.truthy()) {
                beginVerificationAfterAccess(
                    manager,
                    runtime,
                    std::move(resolve),
                    std::move(reject)
                );
                return;
            }

            auto initial = refreshTokens(manager, runtime);
            auto proceed = [manager, runtime, resolve, reject]() mutable {
                beginVerificationAfterAccess(
                    manager,
                    runtime,
                    std::move(resolve),
                    std::move(reject)
                );
                return JsRuntimeValue::undefined();
            };
            auto success = initial.then(
                [proceed](const JsRuntimeValue&) mutable {
                    return proceed();
                }
            );
            (void) success.catchError(
                [proceed](std::exception_ptr) mutable {
                    return proceed();
                }
            );
        }
    );
}

ValuePromise authDeviceCode(
    const std::shared_ptr<MsaTokenManagerState>& manager,
    const std::shared_ptr<MsaTokenManagerRuntimeState>& runtime,
    std::function<void(const JsRuntimeValue&)> dataCallback
) {
    using DeviceCodeCallback = void(JsRuntimeValue);
    auto request = JsRuntimeValue::object({
        {"deviceCodeCallback", JsRuntimeValue::namedFunction<
            DeviceCodeCallback
        >("deviceCodeCallback", [runtime, dataCallback](
            JsRuntimeValue response
        ) {
            debug(runtime, "[msa] device_code response: ", {response});
            if (!dataCallback) {
                throw std::runtime_error("dataCallback is not a function");
            }
            dataCallback(response);
        })},
        {"scopes", runtime->scopes}
    });

    return ValuePromise::create(
        runtime->microtaskQueue,
        [manager, runtime, request = std::move(request)](
            ValuePromise::ResolveFunction resolve,
            ValuePromise::RejectFunction reject
        ) mutable {
            if (!runtime->msalApp) {
                throw std::runtime_error(
                    "Cannot read properties of null (reading "
                    "'acquireTokenByDeviceCode')"
                );
            }
            auto acquired = runtime->msalApp->
                acquireTokenByDeviceCode(std::move(request));
            auto success = acquired.then(
                [manager, runtime, resolve](
                    const JsRuntimeValue& response
                ) mutable {
                    const auto serialized = JsRuntimeJson::stringify(response);
                    debug(
                        runtime,
                        "[msa] device_code resp",
                        {serialized
                            ? JsRuntimeValue::string(*serialized)
                            : JsRuntimeValue::undefined()}
                    );

                    auto getCached = requireGetCached(manager);
                    auto cached = ValuePromise::fromFuture(
                        runtime->microtaskQueue,
                        getCached()
                    );
                    auto inner = cached.then(
                        [manager, runtime, response, resolve](
                            const JsRuntimeValue& value
                        ) mutable {
                            auto mutableValue = value;
                            const auto account = getProperty(
                                mutableValue,
                                "Account"
                            );
                            if (!account.truthy()) {
                                setProperty(
                                    mutableValue,
                                    "Account",
                                    JsRuntimeValue::object({
                                        {"", getProperty(response, "account")}
                                    })
                                );
                                auto setCachedPartial =
                                    requireSetCachedPartial(manager);
                                auto ignored = JsPromise<void>::fromFuture(
                                    runtime->microtaskQueue,
                                    setCachedPartial(mutableValue)
                                );
                                if (runtime->observers.unhandledRejection) {
                                    (void) ignored.catchError(
                                        [runtime](std::exception_ptr error) {
                                            rejectFromIgnored(runtime, error);
                                        }
                                    );
                                }
                            }
                            resolve(response);
                            return JsRuntimeValue::undefined();
                        }
                    );
                    if (runtime->observers.unhandledRejection) {
                        (void) inner.catchError(
                            [runtime](std::exception_ptr error) {
                                rejectFromIgnored(runtime, error);
                                return JsRuntimeValue::undefined();
                            }
                        );
                    }
                    return JsRuntimeValue::undefined();
                }
            );

            (void) success.catchError(
                [runtime, reject](std::exception_ptr error) mutable {
                    warn(
                        runtime,
                        "[msa] Error getting device code. Ensure your supplied "
                        "`authTitle` token (or clientId in your supplied MSAL "
                        "config) is valid and that it has permission to do "
                        "non-interactive code based auth.",
                        error
                    );
                    const auto serialized = stringifyException(error);
                    consoleDebug(runtime, serialized);
                    reject(error);
                    return JsRuntimeValue::undefined();
                }
            );
        }
    );
}

} // namespace

MsaTokenManager::MsaTokenManager(
    std::shared_ptr<JsRuntimeValue> msalConfigValue,
    JsRuntimeValue scopesValue,
    AuthCachePtr cacheValue
) : MsaTokenManager(
        std::move(msalConfigValue),
        std::move(scopesValue),
        std::move(cacheValue),
        {},
        {},
        {}
    ) {}

MsaTokenManager::MsaTokenManager(
    std::shared_ptr<JsRuntimeValue> msalConfigValue,
    JsRuntimeValue scopesValue,
    AuthCachePtr cacheValue,
    MsalPublicClientApplicationFactory applicationFactory,
    std::shared_ptr<JsMicrotaskQueue> microtaskQueueValue,
    MsaTokenManagerObservers observers
) : managerState_(std::make_shared<MsaTokenManagerState>()),
    runtimeState_(std::make_shared<MsaTokenManagerRuntimeState>()),
    msaClientId(managerState_->msaClientId),
    scopes(runtimeState_->scopes),
    cache(managerState_->cache),
    forceRefresh(runtimeState_->forceRefresh),
    msaCache(runtimeState_->msaCache),
    msalApp(runtimeState_->msalApp),
    msalConfig(runtimeState_->msalConfig) {
    if (!microtaskQueueValue) {
        microtaskQueueValue = JsMicrotaskQueue::create();
    }
    runtimeState_->microtaskQueue = std::move(microtaskQueueValue);
    runtimeState_->observers = std::move(observers);

    const auto configValue = msalConfigValue
        ? *msalConfigValue
        : JsRuntimeValue::undefined();
    const auto auth = getProperty(configValue, "auth");
    managerState_->msaClientId = getProperty(auth, "clientId");
    runtimeState_->scopes = std::move(scopesValue);
    managerState_->cache = std::move(cacheValue);

    auto plugin = makeMsaTokenManagerCachePlugin(
        managerState_,
        runtimeState_->microtaskQueue
    );
    runtimeState_->cachePlugin = std::move(plugin.cachePlugin);

    if (!msalConfigValue) {
        throw std::runtime_error(
            "Cannot read properties of undefined (reading 'cache')"
        );
    }
    msalConfigValue->set("cache", JsRuntimeValue::object({
        {"cachePlugin", runtimeState_->cachePlugin}
    }));

    if (applicationFactory) {
        runtimeState_->msalApp = applicationFactory(msalConfigValue);
    } else {
        NativeMsalDependencies dependencies;
        dependencies.microtaskQueue = runtimeState_->microtaskQueue;
        runtimeState_->msalApp =
            std::make_shared<NativeMsalPublicClientApplication>(
                msalConfigValue,
                std::move(dependencies)
            );
    }
    runtimeState_->msalConfig = std::move(msalConfigValue);
}

JsRuntimeValue MsaTokenManager::getUsers() {
    const auto accounts = getProperty(runtimeState_->msaCache, "Account");
    auto users = JsRuntimeValue::array();
    if (!accounts.truthy()) return users;
    for (auto& account : objectValues(accounts)) {
        users.push(std::move(account));
    }
    return users;
}

ValuePromise MsaTokenManager::getAccessToken() {
    return cachedToken(managerState_, runtimeState_, true);
}

ValuePromise MsaTokenManager::getRefreshToken() {
    return cachedToken(managerState_, runtimeState_, false);
}

ValuePromise MsaTokenManager::refreshTokens() {
    return ::bedrock::refreshTokens(managerState_, runtimeState_);
}

ValuePromise MsaTokenManager::verifyTokens() {
    return ::bedrock::verifyTokens(managerState_, runtimeState_);
}

ValuePromise MsaTokenManager::authDeviceCode(
    std::function<void(const JsRuntimeValue& response)> dataCallback
) {
    return ::bedrock::authDeviceCode(
        managerState_,
        runtimeState_,
        std::move(dataCallback)
    );
}

const JsRuntimeValue& MsaTokenManager::installedCachePlugin() const noexcept {
    return runtimeState_->cachePlugin;
}

std::shared_ptr<JsMicrotaskQueue>
MsaTokenManager::microtaskQueue() const noexcept {
    return runtimeState_->microtaskQueue;
}

std::shared_ptr<MsaTokenManagerState>
MsaTokenManager::sharedManagerState() const noexcept {
    return managerState_;
}

} // namespace bedrock
