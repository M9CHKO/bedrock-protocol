#include <bedrock/auth/NativeMsalPublicClientApplication.hpp>

#include <bedrock/auth/MsalError.hpp>
#include <bedrock/auth/MsalRequestParameterBuilder.hpp>
#include <bedrock/auth/MsalTokenResponseHandler.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bedrock {
namespace {

using ValuePromise = JsPromise<JsRuntimeValue>;

constexpr std::string_view kDefaultAuthority =
    "https://login.microsoftonline.com/common";
constexpr std::string_view kNetworkError = "network_error";
constexpr std::string_view kNetworkErrorMessage = "Network request failed";
constexpr std::string_view kPostRequestFailed = "post_request_failed";
constexpr std::string_view kPostRequestFailedMessage =
    "Post request failed from the network, could be a 4xx/5xx or a network "
    "unavailability. Please check the exact error code for details.";
constexpr std::string_view kDeviceCodePollingCancelled =
    "device_code_polling_cancelled";
constexpr std::string_view kDeviceCodePollingCancelledMessage =
    "Caller has cancelled token endpoint polling during device code flow by "
    "setting DeviceCodeRequest.cancel = true.";
constexpr std::string_view kUserTimeoutReached = "user_timeout_reached";
constexpr std::string_view kUserTimeoutReachedMessage =
    "User defined timeout for device code polling reached";
constexpr std::string_view kDeviceCodeExpired = "device_code_expired";
constexpr std::string_view kDeviceCodeExpiredMessage =
    "Device code is expired.";
constexpr std::string_view kDeviceCodeUnknownError =
    "device_code_unknown_error";
constexpr std::string_view kDeviceCodeUnknownErrorMessage =
    "Device code stopped polling for unknown reasons.";

const JsRuntimeValue& undefinedValue() {
    static const JsRuntimeValue value = JsRuntimeValue::undefined();
    return value;
}

const JsRuntimeValue& property(
    const JsRuntimeValue& value,
    std::string_view key
) {
    if (value.isUndefined() || value.isNull()) {
        throw std::runtime_error(
            std::string("Cannot read properties of ") +
            (value.isNull() ? "null" : "undefined") + " (reading '" +
            std::string(key) + "')"
        );
    }
    const auto* result = value.get(key);
    return result ? *result : undefinedValue();
}

void assignProperty(
    JsRuntimeValue& value,
    std::string key,
    JsRuntimeValue member
) {
    if (value.isObject() || value.isArray()) {
        value.set(std::move(key), std::move(member));
        return;
    }
    if (value.isUndefined() || value.isNull()) {
        throw std::runtime_error(
            std::string("Cannot set properties of ") +
            (value.isNull() ? "null" : "undefined") + " (setting '" +
            key + "')"
        );
    }
    throw std::runtime_error(
        "Cannot create property '" + key + "' on primitive value"
    );
}

JsRuntimeValue objectSpread(const JsRuntimeValue& value) {
    auto result = JsRuntimeValue::object();
    if (value.isUndefined() || value.isNull()) return result;
    for (const auto& item : value.ownProperties()) {
        result.set(item.key, item.value);
    }
    return result;
}

void objectAssign(JsRuntimeValue& target, const JsRuntimeValue& source) {
    if (!target.isObject() && !target.isArray()) {
        throw std::runtime_error("Object.assign target is not an object");
    }
    for (const auto& item : source.ownProperties()) {
        target.set(item.key, item.value);
    }
}

std::string primitiveToString(const JsRuntimeValue& value) {
    if (value.isString()) return value.stringValue();
    if (value.isUndefined()) return "undefined";
    if (value.isNull()) return "null";
    if (value.isBool()) return value.boolValue() ? "true" : "false";
    if (value.isNumber()) {
        return JsRuntimeJson::stringify(value).value_or("undefined");
    }
    if (value.isArray()) {
        std::string output;
        const auto& elements = value.arrayNode()->elements();
        for (std::size_t index = 0; index < elements.size(); ++index) {
            if (index) output.push_back(',');
            if (!elements[index] || elements[index]->isUndefined() ||
                elements[index]->isNull()) {
                continue;
            }
            output += primitiveToString(*elements[index]);
        }
        return output;
    }
    if (value.isFunction()) return "function () { [native code] }";
    return "[object Object]";
}

double numberValue(const JsRuntimeValue& value) {
    if (value.isNumber()) return value.numberValue();
    if (value.isUndefined()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (value.isNull()) return 0.0;
    if (value.isBool()) return value.boolValue() ? 1.0 : 0.0;
    if (value.isString()) {
        if (value.stringValue().empty()) return 0.0;
        char* end = nullptr;
        const auto result = std::strtod(value.stringValue().c_str(), &end);
        if (!end || end != value.stringValue().c_str() +
                value.stringValue().size()) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return result;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

std::vector<JsRuntimeValue> arrayElements(const JsRuntimeValue& value) {
    if (!value.isArray()) {
        throw std::runtime_error("authRequest.scopes is not iterable");
    }
    std::vector<JsRuntimeValue> result;
    const auto& elements = value.arrayNode()->elements();
    result.reserve(elements.size());
    for (const auto& item : elements) {
        result.push_back(item.value_or(JsRuntimeValue::undefined()));
    }
    return result;
}

std::vector<std::string> scopeStrings(const JsRuntimeValue& value) {
    std::vector<std::string> result;
    for (const auto& item : arrayElements(value)) {
        if (!item.isString()) {
            // StringUtils.trimArrayEntries calls item.trim() directly.
            throw std::runtime_error("item.trim is not a function");
        }
        result.push_back(item.stringValue());
    }
    return result;
}

JsRuntimeValue initializedBaseRequest(
    const JsRuntimeValue& request,
    const std::string& configuredAuthority,
    const std::function<std::string()>& correlationIdFactory
) {
    auto result = objectSpread(request);

    auto scopes = JsRuntimeValue::array();
    const auto& suppliedScopes = property(request, "scopes");
    if (suppliedScopes.truthy()) {
        for (auto item : arrayElements(suppliedScopes)) {
            scopes.push(std::move(item));
        }
    }
    scopes.push(JsRuntimeValue::string("openid"));
    scopes.push(JsRuntimeValue::string("profile"));
    scopes.push(JsRuntimeValue::string("offline_access"));
    result.set("scopes", std::move(scopes));

    const auto& suppliedCorrelation = property(request, "correlationId");
    result.set(
        "correlationId",
        suppliedCorrelation.truthy()
            ? suppliedCorrelation
            : JsRuntimeValue::string(correlationIdFactory())
    );

    const auto& suppliedAuthority = property(request, "authority");
    result.set(
        "authority",
        suppliedAuthority.truthy()
            ? suppliedAuthority
            : JsRuntimeValue::string(configuredAuthority)
    );
    return result;
}

std::string normalizedAuthority(std::string authority) {
    if (authority.empty()) authority = std::string(kDefaultAuthority);
    while (!authority.empty() && authority.back() == '/') {
        authority.pop_back();
    }
    return authority;
}

std::string canonicalAuthority(const std::string& authority) {
    return normalizedAuthority(authority) + "/";
}

std::string authorityTenant(const std::string& authority) {
    const auto normalized = normalizedAuthority(authority);
    const auto slash = normalized.find_last_of('/');
    return slash == std::string::npos
        ? std::string()
        : normalized.substr(slash + 1);
}

std::string preferredCacheEnvironment(const std::string& authority) {
    constexpr std::string_view schemeSeparator = "://";
    auto begin = authority.find(schemeSeparator);
    begin = begin == std::string::npos
        ? 0
        : begin + schemeSeparator.size();
    const auto end = authority.find_first_of("/:?#", begin);
    auto host = authority.substr(begin, end - begin);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (host == "login.microsoftonline.com") return "login.windows.net";
    return host;
}

std::int64_t jsNowSeconds(const std::function<double()>& clock) {
    const auto seconds = clock() / 1000.0;
    if (!std::isfinite(seconds)) return 0;
    // Math.round(x) is floor(x + 0.5), including the negative half case.
    return static_cast<std::int64_t>(std::floor(seconds + 0.5));
}

MsalHttpResponse sendRequest(
    const MsalHttpClientPtr& client,
    MsalHttpRequest request
) {
    try {
        if (!client) throw std::runtime_error("network client is null");
        return client->send(std::move(request)).get();
    } catch (const MsalAuthError&) {
        throw;
    } catch (...) {
        throw MsalClientAuthError(
            std::string(kNetworkError),
            std::string(kNetworkErrorMessage)
        );
    }
}

JsRuntimeValue deviceCodeCallbackValue(const JsRuntimeValue& body) {
    return JsRuntimeValue::object({
        {"userCode", property(body, "user_code")},
        {"deviceCode", property(body, "device_code")},
        {"verificationUri", property(body, "verification_uri")},
        {"expiresIn", property(body, "expires_in")},
        {"interval", property(body, "interval")},
        {"message", property(body, "message")}
    });
}

void invokeDeviceCodeCallback(
    const JsRuntimeValue& callback,
    JsRuntimeValue value
) {
    if (const auto* function = callback.functionIf<void(JsRuntimeValue)>()) {
        (*function)(std::move(value));
        return;
    }
    if (const auto* function = callback.functionIf<
            JsRuntimeValue(JsRuntimeValue)>()) {
        (void) (*function)(std::move(value));
        return;
    }
    if (const auto* function = callback.functionIf<
            JsPromise<JsRuntimeValue>(JsRuntimeValue)>()) {
        // DeviceCodeClient deliberately does not await the callback result.
        (void) (*function)(std::move(value));
        return;
    }
    throw std::runtime_error("request.deviceCodeCallback is not a function");
}

void invokeCacheHook(
    const JsRuntimeValue& plugin,
    std::string_view method,
    const TokenCacheContextPtr& context
) {
    const auto* member = plugin.get(method);
    if (!member || !member->isFunctionOf<MsalCacheHookSignature>()) {
        throw std::runtime_error(
            "this.persistencePlugin." + std::string(method) +
            " is not a function"
        );
    }
    member->call<MsalCacheHookSignature>(context).get();
}

JsRuntimeValue processTokenResponse(
    MsalTokenResponseContext context,
    const JsRuntimeValue& serverResponse,
    const JsRuntimeValue& cachePlugin,
    const std::shared_ptr<MsalSerializableTokenCache>& tokenCache
) {
    MsalTokenResponseHandler handler(std::move(context));
    const auto prepared = handler.prepareServerTokenResponse(
        serverResponse,
        *tokenCache
    );

    if (!cachePlugin.truthy()) {
        const bool includeServerResponse = handler.savePreparedTokenResponse(
            prepared,
            *tokenCache
        );
        return handler.generateAuthenticationResult(
            prepared,
            includeServerResponse
        );
    }

    auto cacheContext = std::make_shared<TokenCacheContext>();
    cacheContext->tokenCache =
        std::static_pointer_cast<ISerializableTokenCache>(tokenCache);
    cacheContext->cacheHasChanged = true;

    bool includeServerResponse = false;
    bool generatedBeforeAfterHook = false;
    JsRuntimeValue earlyResult = JsRuntimeValue::undefined();
    std::exception_ptr pendingError;
    try {
        invokeCacheHook(cachePlugin, "beforeCacheAccess", cacheContext);
        includeServerResponse = handler.savePreparedTokenResponse(
            prepared,
            *tokenCache
        );
        // ResponseHandler returns its account-missing refresh result from
        // inside the try. JavaScript therefore generates that result before
        // the finally/afterCacheAccess hook, while the normal result is made
        // after the hook.
        if (!includeServerResponse) {
            earlyResult = handler.generateAuthenticationResult(
                prepared,
                false
            );
            generatedBeforeAfterHook = true;
        }
    } catch (...) {
        pendingError = std::current_exception();
    }

    // ResponseHandler resolves afterCacheAccess dynamically in finally. Its
    // rejection replaces an error from beforeCacheAccess/saveCacheRecord.
    invokeCacheHook(cachePlugin, "afterCacheAccess", cacheContext);
    if (pendingError) std::rethrow_exception(pendingError);
    if (generatedBeforeAfterHook) return earlyResult;

    return handler.generateAuthenticationResult(
        prepared,
        includeServerResponse
    );
}

std::string requiredStringProperty(
    const JsRuntimeValue& value,
    std::string_view key
) {
    return primitiveToString(property(value, key));
}

std::string optionalStringProperty(
    const JsRuntimeValue& value,
    std::string_view key
) {
    const auto& member = property(value, key);
    return member.isUndefined() || member.isNull()
        ? std::string()
        : primitiveToString(member);
}

[[noreturn]] void rethrowWithTelemetry(
    std::exception_ptr failure,
    const JsRuntimeValue& correlationId,
    MsalServerTelemetryManager& telemetry
) {
    try {
        std::rethrow_exception(std::move(failure));
    } catch (MsalAuthError& error) {
        error.setCorrelationId(correlationId);
        telemetry.cacheFailedRequest(MsalServerTelemetryError::authError(
            error.errorCode(),
            error.errorMessage(),
            error.subError()
        ));
        throw;
    } catch (const std::exception& error) {
        telemetry.cacheFailedRequest(
            MsalServerTelemetryError::error(error.what())
        );
        throw;
    } catch (...) {
        telemetry.cacheUnknownFailure();
        throw;
    }
}

} // namespace

NativeMsalPublicClientApplication::NativeMsalPublicClientApplication(
    std::shared_ptr<JsRuntimeValue> configuration,
    NativeMsalDependencies dependencies
) : sourceConfiguration_(std::move(configuration)),
    dependencies_(std::move(dependencies)) {
    if (!dependencies_.microtaskQueue) {
        dependencies_.microtaskQueue = JsMicrotaskQueue::create();
    }
    if (!dependencies_.dateNowMilliseconds) {
        dependencies_.dateNowMilliseconds = [] {
            return static_cast<double>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count()
            );
        };
    }
    if (!dependencies_.correlationIdFactory) {
        dependencies_.correlationIdFactory = [] {
            return MsalRequestBuilder::generateCorrelationId();
        };
    }
    if (!dependencies_.delayMilliseconds) {
        const auto queue = dependencies_.microtaskQueue;
        dependencies_.delayMilliseconds = [queue](double milliseconds) {
            return JsPromise<void>::fromFuture(
                queue,
                std::async(std::launch::async, [milliseconds] {
                    if (milliseconds > 0) {
                        std::this_thread::sleep_for(
                            std::chrono::duration<double, std::milli>(
                                milliseconds
                            )
                        );
                    } else {
                        std::this_thread::yield();
                    }
                })
            );
        };
    }
    const auto configValue = sourceConfiguration_
        ? *sourceConfiguration_
        : JsRuntimeValue::undefined();
    const auto& auth = property(configValue, "auth");
    clientId_ = optionalStringProperty(auth, "clientId");
    const auto configuredAuthority = optionalStringProperty(auth, "authority");
    authority_ = normalizedAuthority(configuredAuthority);

    const auto& cache = property(configValue, "cache");
    if (!cache.isUndefined() && !cache.isNull()) {
        cachePlugin_ = property(cache, "cachePlugin");
    }

    // Node's `{ ...systemOptions, ...system }` lets a supplied networkClient
    // replace the default HttpClient. Native callers express that same object
    // identity as an opaque IMsalHttpClient value.
    const auto& system = property(configValue, "system");
    bool explicitUnavailableNetworkClient = false;
    if (!dependencies_.httpClient && !system.isUndefined() &&
        !system.isNull() && system.hasOwn("networkClient")) {
        const auto& configuredClient = property(system, "networkClient");
        dependencies_.httpClient =
            configuredClient.opaqueIf<IMsalHttpClient>();
        explicitUnavailableNetworkClient = !dependencies_.httpClient;
    }
    if (!dependencies_.httpClient && !explicitUnavailableNetworkClient) {
        dependencies_.httpClient = std::make_shared<CurlMsalHttpClient>(
            dependencies_.microtaskQueue
        );
    }
    tokenCache_ = std::make_shared<MsalSerializableTokenCache>();
    serverTelemetryCache_ =
        std::make_shared<MsalServerTelemetryMemoryCache>();
}

ValuePromise NativeMsalPublicClientApplication::acquireTokenByDeviceCode(
    JsRuntimeValue request
) {
    // With claims-based caching disabled (the msal-node default),
    // initializeBaseRequest performs all request reads before returning its
    // already-resolved promise. Keep that synchronous prefix observable, then
    // apply Object.assign in the worker at the outer await continuation.
    JsRuntimeValue baseRequest;
    try {
        assignProperty(
            request,
            "authenticationScheme",
            JsRuntimeValue::string("Bearer")
        );
        baseRequest = initializedBaseRequest(
            request,
            authority_,
            dependencies_.correlationIdFactory
        );
    } catch (...) {
        return ValuePromise::rejected(
            dependencies_.microtaskQueue,
            std::current_exception()
        );
    }

    const auto clientId = clientId_;
    const auto cachePlugin = cachePlugin_;
    const auto tokenCache = tokenCache_;
    const auto serverTelemetryCache = serverTelemetryCache_;
    const auto dependencies = dependencies_;

    return ValuePromise::fromFuture(
        dependencies.microtaskQueue,
        std::async(
            std::launch::async,
            [request = std::move(request), clientId,
             cachePlugin, tokenCache, serverTelemetryCache,
             dependencies, baseRequest = std::move(baseRequest)]() mutable {
                std::string correlationId;
                std::unique_ptr<MsalServerTelemetryManager> telemetry;
                try {
                    objectAssign(request, baseRequest);

                    correlationId = requiredStringProperty(
                        request,
                        "correlationId"
                    );
                    MsalServerTelemetryRequest telemetryRequest;
                    telemetryRequest.clientId = clientId;
                    telemetryRequest.apiId =
                        MsalServerTelemetryManager::kDeviceCodeApiId;
                    telemetryRequest.correlationId = correlationId;
                    telemetry = std::make_unique<
                        MsalServerTelemetryManager
                    >(std::move(telemetryRequest), serverTelemetryCache);

                    const auto authority = normalizedAuthority(
                        requiredStringProperty(request, "authority")
                    );
                    const auto rawScopes = scopeStrings(
                        property(request, "scopes")
                    );

                    MsalRequestBuilderOptions initialBuilderOptions;
                    initialBuilderOptions.clientId = clientId;
                    initialBuilderOptions.scopes = rawScopes;
                    initialBuilderOptions.correlationId = correlationId;
                    initialBuilderOptions.authority = authority;
                    MsalRequestBuilder initialRequestBuilder(
                        std::move(initialBuilderOptions)
                    );

                    const auto deviceResponse = sendRequest(
                        dependencies.httpClient,
                        initialRequestBuilder.deviceCodeRequest()
                    );
                    const auto callbackValue = deviceCodeCallbackValue(
                        deviceResponse.body
                    );
                    invokeDeviceCodeCallback(
                        property(request, "deviceCodeCallback"),
                        callbackValue
                    );

                    const auto reqTimestamp = jsNowSeconds(
                        dependencies.dateNowMilliseconds
                    );
                    const auto deviceCode = requiredStringProperty(
                        callbackValue,
                        "deviceCode"
                    );
                    // DeviceCodeClient creates the body and endpoint once,
                    // after the callback, and reads scopes/correlationId from
                    // the still-live request again at that point.
                    const auto& livePollCorrelation = property(
                        request,
                        "correlationId"
                    );
                    const auto pollQueryCorrelationId = primitiveToString(
                        livePollCorrelation
                    );
                    const auto pollBodyCorrelationId =
                        livePollCorrelation.truthy()
                            ? pollQueryCorrelationId
                            : dependencies.correlationIdFactory();
                    const auto pollScopes = scopeStrings(
                        property(request, "scopes")
                    );
                    MsalRequestBuilderOptions pollBuilderOptions;
                    pollBuilderOptions.clientId = clientId;
                    pollBuilderOptions.scopes = pollScopes;
                    pollBuilderOptions.correlationId = pollBodyCorrelationId;
                    pollBuilderOptions.tokenQueryCorrelationId =
                        pollQueryCorrelationId;
                    pollBuilderOptions.deviceCodeBodyCorrelationId =
                        pollBodyCorrelationId;
                    pollBuilderOptions.authority = authority;
                    pollBuilderOptions.currentTelemetry =
                        telemetry->currentTelemetry();
                    pollBuilderOptions.lastTelemetry =
                        telemetry->lastTelemetry();
                    MsalRequestBuilder pollRequestBuilder(
                        std::move(pollBuilderOptions)
                    );
                    const auto pollRequest =
                        pollRequestBuilder.deviceCodeTokenRequest(deviceCode);

                    const auto& timeoutMember = property(request, "timeout");
                    const bool hasTimeout = timeoutMember.truthy();
                    const double timeout = hasTimeout
                        ? static_cast<double>(jsNowSeconds(
                            dependencies.dateNowMilliseconds
                        )) + numberValue(timeoutMember)
                        : 0.0;
                    const double expiration =
                        static_cast<double>(jsNowSeconds(
                            dependencies.dateNowMilliseconds
                        )) + numberValue(
                            property(callbackValue, "expiresIn")
                        );
                    const double pollingMilliseconds = numberValue(
                        property(callbackValue, "interval")
                    ) * 1000.0;

                    JsRuntimeValue serverResponse = JsRuntimeValue::undefined();
                    bool completed = false;
                    while (true) {
                        if (property(request, "cancel").truthy()) {
                            throw MsalClientAuthError(
                                std::string(kDeviceCodePollingCancelled),
                                std::string(kDeviceCodePollingCancelledMessage)
                            );
                        }
                        if (hasTimeout && timeout < expiration &&
                            static_cast<double>(jsNowSeconds(
                                dependencies.dateNowMilliseconds
                            )) > timeout) {
                            throw MsalClientAuthError(
                                std::string(kUserTimeoutReached),
                                std::string(kUserTimeoutReachedMessage)
                            );
                        }
                        if (static_cast<double>(jsNowSeconds(
                                dependencies.dateNowMilliseconds
                            )) > expiration) {
                            throw MsalClientAuthError(
                                std::string(kDeviceCodeExpired),
                                std::string(kDeviceCodeExpiredMessage)
                            );
                        }

                        const auto response = sendRequest(
                            dependencies.httpClient,
                            pollRequest
                        );
                        if (response.status < 500 && response.status != 429) {
                            telemetry->clearTelemetryCache();
                        }
                        const auto* error = response.body.truthy()
                            ? response.body.get("error")
                            : nullptr;
                        if (error && error->truthy()) {
                            const auto errorCode = primitiveToString(*error);
                            if (errorCode == "authorization_pending") {
                                dependencies.delayMilliseconds(
                                    pollingMilliseconds
                                ).get();
                                continue;
                            }
                            throw MsalAuthError(
                                std::string(kPostRequestFailed),
                                std::string(kPostRequestFailedMessage) + " " +
                                    errorCode
                            );
                        }
                        serverResponse = response.body;
                        completed = true;
                        break;
                    }

                    if (!completed) {
                        throw MsalClientAuthError(
                            std::string(kDeviceCodeUnknownError),
                            std::string(kDeviceCodeUnknownErrorMessage)
                        );
                    }

                    MsalTokenResponseContext context;
                    const auto& resultCorrelationValue = property(
                        request,
                        "correlationId"
                    );
                    const auto resultCorrelationId = primitiveToString(
                        resultCorrelationValue
                    );
                    const auto resultScopes = scopeStrings(
                        property(request, "scopes")
                    );
                    context.clientId = clientId;
                    context.canonicalAuthority = canonicalAuthority(authority);
                    context.preferredCacheEnvironment =
                        preferredCacheEnvironment(authority);
                    context.authorityTenant = authorityTenant(authority);
                    context.normalizedRequestScopes =
                        MsalRequestParameterBuilder::normalizeScopes(
                            resultScopes
                        );
                    context.correlationId = resultCorrelationId;
                    context.authenticationResultCorrelationId =
                        resultCorrelationValue;
                    context.sshKid = property(request, "sshKid");
                    context.reqTimestampSeconds =
                        static_cast<double>(reqTimestamp);
                    context.nowSecondsCallback = [clock =
                            dependencies.dateNowMilliseconds] {
                        return static_cast<double>(jsNowSeconds(clock));
                    };
                    return processTokenResponse(
                        std::move(context),
                        serverResponse,
                        cachePlugin,
                        tokenCache
                    );
                } catch (...) {
                    if (telemetry) {
                        auto errorCorrelationId = JsRuntimeValue::string(
                            correlationId
                        );
                        try {
                            errorCorrelationId = property(
                                request,
                                "correlationId"
                            );
                        } catch (...) {
                            // AuthError.setCorrelationId receives the live
                            // property only if that Get itself succeeds.
                        }
                        rethrowWithTelemetry(
                            std::current_exception(),
                            errorCorrelationId,
                            *telemetry
                        );
                    }
                    throw;
                }
            }
        )
    );
}

ValuePromise NativeMsalPublicClientApplication::acquireTokenByRefreshToken(
    JsRuntimeValue request
) {
    // In `{ ...request, ...(await initializeBaseRequest(request)) }`, the
    // first spread and initializeBaseRequest's default synchronous prefix both
    // run before the public async method yields.
    JsRuntimeValue requestSpread;
    JsRuntimeValue baseRequest;
    try {
        requestSpread = objectSpread(request);
        assignProperty(
            request,
            "authenticationScheme",
            JsRuntimeValue::string("Bearer")
        );
        baseRequest = initializedBaseRequest(
            request,
            authority_,
            dependencies_.correlationIdFactory
        );
    } catch (...) {
        return ValuePromise::rejected(
            dependencies_.microtaskQueue,
            std::current_exception()
        );
    }

    const auto clientId = clientId_;
    const auto cachePlugin = cachePlugin_;
    const auto tokenCache = tokenCache_;
    const auto serverTelemetryCache = serverTelemetryCache_;
    const auto dependencies = dependencies_;

    return ValuePromise::fromFuture(
        dependencies.microtaskQueue,
        std::async(
            std::launch::async,
            [request = std::move(request), clientId,
             cachePlugin, tokenCache, serverTelemetryCache,
             dependencies, requestSpread = std::move(requestSpread),
             baseRequest = std::move(baseRequest)]() mutable {
                std::string correlationId;
                std::unique_ptr<MsalServerTelemetryManager> telemetry;
                try {
                    auto validRequest = std::move(requestSpread);
                    objectAssign(validRequest, baseRequest);
                    validRequest.set(
                        "authenticationScheme",
                        JsRuntimeValue::string("Bearer")
                    );

                    correlationId = requiredStringProperty(
                        validRequest,
                        "correlationId"
                    );
                    MsalServerTelemetryRequest telemetryRequest;
                    telemetryRequest.clientId = clientId;
                    telemetryRequest.apiId =
                        MsalServerTelemetryManager::kRefreshTokenApiId;
                    telemetryRequest.correlationId = correlationId;
                    telemetry = std::make_unique<
                        MsalServerTelemetryManager
                    >(std::move(telemetryRequest), serverTelemetryCache);

                    const auto authority = normalizedAuthority(
                        requiredStringProperty(validRequest, "authority")
                    );
                    const auto rawScopes = scopeStrings(
                        property(validRequest, "scopes")
                    );
                    const auto normalizedScopes =
                        MsalRequestParameterBuilder::normalizeScopes(rawScopes);
                    const auto refreshToken = requiredStringProperty(
                        validRequest,
                        "refreshToken"
                    );

                    MsalRequestBuilderOptions builderOptions;
                    builderOptions.clientId = clientId;
                    builderOptions.scopes = rawScopes;
                    builderOptions.correlationId = correlationId;
                    builderOptions.authority = authority;
                    builderOptions.currentTelemetry =
                        telemetry->currentTelemetry();
                    builderOptions.lastTelemetry =
                        telemetry->lastTelemetry();
                    MsalRequestBuilder requestBuilder(
                        std::move(builderOptions)
                    );

                    const auto reqTimestamp = jsNowSeconds(
                        dependencies.dateNowMilliseconds
                    );
                    const auto response = sendRequest(
                        dependencies.httpClient,
                        requestBuilder.refreshTokenRequest(refreshToken)
                    );
                    if (response.status < 500 && response.status != 429) {
                        telemetry->clearTelemetryCache();
                    }

                    MsalTokenResponseContext context;
                    context.clientId = clientId;
                    context.canonicalAuthority = canonicalAuthority(authority);
                    context.preferredCacheEnvironment =
                        preferredCacheEnvironment(authority);
                    context.authorityTenant = authorityTenant(authority);
                    context.normalizedRequestScopes = normalizedScopes;
                    context.correlationId = correlationId;
                    context.authenticationResultCorrelationId = property(
                        validRequest,
                        "correlationId"
                    );
                    context.sshKid = property(validRequest, "sshKid");
                    if (const auto* requestId =
                            response.header("x-ms-request-id")) {
                        context.requestId = *requestId;
                    }
                    context.reqTimestampSeconds =
                        static_cast<double>(reqTimestamp);
                    context.nowSecondsCallback = [clock =
                            dependencies.dateNowMilliseconds] {
                        return static_cast<double>(jsNowSeconds(clock));
                    };
                    context.handlingRefreshTokenResponse = true;
                    context.forceCacheRefreshTokenResponse = property(
                        validRequest,
                        "forceCache"
                    ).truthy();
                    return processTokenResponse(
                        std::move(context),
                        response.body,
                        cachePlugin,
                        tokenCache
                    );
                } catch (...) {
                    if (telemetry) {
                        rethrowWithTelemetry(
                            std::current_exception(),
                            JsRuntimeValue::string(correlationId),
                            *telemetry
                        );
                    }
                    throw;
                }
            }
        )
    );
}

double NativeMsalPublicClientApplication::dateNowMilliseconds() const {
    return dependencies_.dateNowMilliseconds();
}

std::int64_t NativeMsalPublicClientApplication::nowSeconds() const {
    return jsNowSeconds(dependencies_.dateNowMilliseconds);
}

std::string NativeMsalPublicClientApplication::createCorrelationId() const {
    return dependencies_.correlationIdFactory();
}

JsPromise<void> NativeMsalPublicClientApplication::delay(
    double milliseconds
) const {
    return dependencies_.delayMilliseconds(milliseconds);
}

} // namespace bedrock
