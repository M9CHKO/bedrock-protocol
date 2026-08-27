#include <bedrock/auth/LiveTokenManager.hpp>
#include <bedrock/auth/XboxLiveAuth.hpp>

#include <cmath>
#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

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

bool check(bool condition, const std::string& message) {
    if (!condition) std::cerr << "[LIVE-TOKEN] " << message << "\n";
    return condition;
}

bedrock::JsRuntimeValue property(
    const bedrock::JsRuntimeValue& value,
    std::string_view name
) {
    const auto* found = value.get(name);
    return found ? *found : bedrock::JsRuntimeValue::undefined();
}

struct MemoryCacheState {
    bedrock::JsRuntimeValue value = bedrock::JsRuntimeValue::object();
    int getCalls = 0;
    int partialCalls = 0;
};

bedrock::AuthCachePtr makeMemoryCache(
    const std::shared_ptr<MemoryCacheState>& state
) {
    return std::make_shared<bedrock::AuthCache>(
        [state] {
            ++state->getCalls;
            return bedrock::makeReadyAuthCacheFuture(state->value);
        },
        [state](bedrock::AuthCacheValue update) {
            ++state->partialCalls;
            auto merged = bedrock::JsRuntimeValue::object();
            if (!state->value.isNull() && !state->value.isUndefined()) {
                for (const auto& item : state->value.ownProperties()) {
                    merged.set(item.key, item.value);
                }
            }
            if (!update.isNull() && !update.isUndefined()) {
                for (const auto& item : update.ownProperties()) {
                    merged.set(item.key, item.value);
                }
            }
            state->value = std::move(merged);
            return bedrock::makeReadyAuthCacheFuture();
        }
    );
}

bedrock::XboxTokenHttpResponse response(
    int status,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers = {}
) {
    bedrock::XboxTokenHttpResponse result;
    result.status = status;
    result.statusText = status == 200 ? "OK" :
        (status == 400 ? "Bad Request" : "Error");
    result.bodyText = std::move(body);
    result.headers = std::move(headers);
    return result;
}

class ScriptedHttpClient final : public bedrock::IXboxTokenHttpClient {
public:
    explicit ScriptedHttpClient(
        std::shared_ptr<bedrock::JsMicrotaskQueue> queue
    ) : queue_(std::move(queue)) {}

    bedrock::JsPromise<bedrock::XboxTokenHttpResponse> fetch(
        bedrock::XboxTokenHttpRequest request
    ) override {
        requests.push_back(std::move(request));
        if (responses.empty()) {
            return bedrock::JsPromise<bedrock::XboxTokenHttpResponse>::rejected(
                queue_,
                "unexpected LiveTokenManager fetch"
            );
        }
        auto next = std::move(responses.front());
        responses.pop_front();
        return bedrock::JsPromise<bedrock::XboxTokenHttpResponse>::resolved(
            queue_,
            std::move(next)
        );
    }

    std::deque<bedrock::XboxTokenHttpResponse> responses;
    std::vector<bedrock::XboxTokenHttpRequest> requests;

private:
    std::shared_ptr<bedrock::JsMicrotaskQueue> queue_;
};

bedrock::JsRuntimeValue cachedToken(
    double obtainedOn,
    double expiresIn,
    std::string access,
    std::string refresh
) {
    return bedrock::JsRuntimeValue::object({
        {"token", bedrock::JsRuntimeValue::object({
            {"access_token", bedrock::JsRuntimeValue::string(std::move(access))},
            {"refresh_token", bedrock::JsRuntimeValue::string(std::move(refresh))},
            {"expires_in", bedrock::JsRuntimeValue::number(expiresIn)},
            {"obtainedOn", bedrock::JsRuntimeValue::number(obtainedOn)}
        })}
    });
}

bool checkCachedAndRefreshFlow() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto clock = std::make_shared<double>(1'000'000.0);
    auto cacheState = std::make_shared<MemoryCacheState>();
    cacheState->value = cachedToken(
        *clock,
        5'000.0,
        "ACCESS",
        "R T/+?"
    );
    auto http = std::make_shared<ScriptedHttpClient>(queue);

    auto scopes = bedrock::JsRuntimeValue::array({
        bedrock::JsRuntimeValue::string(
            "service::user.auth.xboxlive.com::MBI_SSL"
        )
    });
    bedrock::LiveTokenManager manager(
        bedrock::JsRuntimeValue::string("C ID/+?"),
        scopes,
        makeMemoryCache(cacheState),
        bedrock::LiveTokenManagerDependencies {
            .httpClient = http,
            .microtaskQueue = queue,
            .dateNowMilliseconds = [clock] { return *clock; }
        }
    );

    ok &= check(
        manager.scopes.sharesIdentityWith(scopes) &&
            manager.forceRefresh.isUndefined() &&
            manager.polling.isUndefined(),
        "constructor fields differ from LiveTokenManager.js"
    );

    const auto access = manager.getAccessToken().get();
    const auto refresh = manager.getRefreshToken().get();
    ok &= check(
        property(access, "valid").isBool() &&
            property(access, "valid").boolValue() &&
            property(access, "until").numberValue() == 5'000.0 &&
            property(access, "token").stringValue() == "ACCESS" &&
            property(refresh, "valid").boolValue() &&
            property(refresh, "token").stringValue() == "R T/+?",
        "cached token shape or obtainedOn+expires_in arithmetic diverged"
    );
    ok &= check(
        manager.verifyTokens().get().boolValue() && http->requests.empty(),
        "valid cached token did not satisfy verifyTokens"
    );

    http->responses.push_back(response(
        200,
        R"({"access_token":"NEW","refresh_token":"NEW_RT","expires_in":777})"
    ));
    const auto refreshed = manager.refreshTokens().get();
    ok &= check(
        property(refreshed, "access_token").stringValue() == "NEW" &&
            http->requests.size() == 1,
        "refreshTokens did not return the parsed token"
    );
    if (!http->requests.empty()) {
        const auto& request = http->requests.front();
        ok &= check(
            request.method == "post" &&
                request.url == bedrock::LiveTokenManager::LiveTokenRequest &&
                request.body ==
                    "scope=service%3A%3Auser.auth.xboxlive.com%3A%3AMBI_SSL&"
                    "client_id=C+ID%2F%2B%3F&grant_type=refresh_token&"
                    "refresh_token=R+T%2F%2B%3F" &&
                request.header("Content-Type") &&
                *request.header("Content-Type") ==
                    "application/x-www-form-urlencoded" &&
                request.credentials.isString() &&
                request.credentials.stringValue() == "include",
            "refresh fetch init/URLSearchParams encoding mismatch"
        );
    }

    const auto cacheToken = property(cacheState->value, "token");
    ok &= check(
        cacheState->partialCalls == 1 &&
            property(cacheToken, "access_token").stringValue() == "NEW" &&
            property(cacheToken, "obtainedOn").numberValue() == *clock,
        "refresh response was not shallow-spread into the Live cache"
    );

    auto missingState = std::make_shared<MemoryCacheState>();
    bedrock::LiveTokenManager missing(
        bedrock::JsRuntimeValue::string("CID"),
        bedrock::JsRuntimeValue::array(),
        makeMemoryCache(missingState),
        bedrock::LiveTokenManagerDependencies {
            .httpClient = http,
            .microtaskQueue = queue,
            .dateNowMilliseconds = [clock] { return *clock; }
        }
    );
    std::string missingError;
    try {
        (void) missing.refreshTokens().get();
    } catch (const std::exception& error) {
        missingError = error.what();
    }
    ok &= check(
        missingError == "Cannot refresh without refresh token",
        "missing refresh-token rejection mismatch"
    );
    return ok;
}

bool checkAuthflowRuntimeIntegration() {
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto cacheState = std::make_shared<MemoryCacheState>();
    auto http = std::make_shared<ScriptedHttpClient>(queue);
    auto runtime = bedrock::XboxLiveAuth::initializePrismarineAuthFlowRuntime(
        bedrock::XboxLiveAuthFlowOptions {
            .authTitle = "LIVE_CLIENT",
            .flow = "live"
        },
        {},
        makeMemoryCache(cacheState),
        {},
        queue,
        {},
        bedrock::LiveTokenManagerDependencies {
            .httpClient = http,
            .microtaskQueue = queue,
            .dateNowMilliseconds = [] { return 1'000.0; }
        }
    );
    return check(
        !runtime.effectiveMsalConfig && runtime.live && !runtime.msa &&
            runtime.doTitleAuth &&
            runtime.live->clientId.isString() &&
            runtime.live->clientId.stringValue() == "LIVE_CLIENT" &&
            runtime.live->scopes.isArray() &&
            runtime.live->scopes.length() == 1 &&
            property(runtime.live->scopes, "0").stringValue() ==
                "service::user.auth.xboxlive.com::MBI_SSL",
        "MicrosoftAuthFlow live branch did not retain LiveTokenManager"
    );
}

bool checkVerifyFailureFlow() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto clock = std::make_shared<double>(2'000'000.0);
    auto cacheState = std::make_shared<MemoryCacheState>();
    cacheState->value = cachedToken(
        1'000'000.0,
        1'000.0,
        "OLD",
        "REFRESH"
    );
    auto http = std::make_shared<ScriptedHttpClient>(queue);
    http->responses.push_back(response(400, "denied"));
    auto warnings = std::make_shared<std::vector<std::string>>();

    bedrock::LiveTokenManager manager(
        bedrock::JsRuntimeValue::string("CID"),
        bedrock::JsRuntimeValue::string("scope"),
        makeMemoryCache(cacheState),
        bedrock::LiveTokenManagerDependencies {
            .httpClient = http,
            .microtaskQueue = queue,
            .dateNowMilliseconds = [clock] { return *clock; },
            .observers = bedrock::LiveTokenManagerObservers {
                .warn = [warnings](
                    const std::string& label,
                    std::exception_ptr error
                ) {
                    warnings->push_back(label + ":" + exceptionMessage(error));
                }
            }
        }
    );

    const auto verified = manager.verifyTokens().get();
    ok &= check(
        verified.isBool() && !verified.boolValue() &&
            *warnings == std::vector<std::string>({
                "Error refreshing token:400 Bad Request denied"
            }),
        "expired-token refresh failure did not warn and resolve false"
    );
    return ok;
}

bool checkForceRefreshAndInitialDeviceFailure() {
    bool ok = true;
    {
        auto queue = bedrock::JsMicrotaskQueue::create();
        auto clock = std::make_shared<double>(2'500'000.0);
        auto cacheState = std::make_shared<MemoryCacheState>();
        cacheState->value = cachedToken(
            *clock,
            5'000.0,
            "VALID",
            "REFRESH"
        );
        auto http = std::make_shared<ScriptedHttpClient>(queue);
        http->responses.push_back(response(400, "forced refresh failed"));
        auto warnings = std::make_shared<int>(0);
        bedrock::LiveTokenManager manager(
            bedrock::JsRuntimeValue::string("CID"),
            bedrock::JsRuntimeValue::string("scope"),
            makeMemoryCache(cacheState),
            bedrock::LiveTokenManagerDependencies {
                .httpClient = http,
                .microtaskQueue = queue,
                .dateNowMilliseconds = [clock] { return *clock; },
                .observers = bedrock::LiveTokenManagerObservers {
                    .warn = [warnings](
                        const std::string&,
                        std::exception_ptr
                    ) { ++*warnings; }
                }
            }
        );
        manager.forceRefresh = bedrock::JsRuntimeValue::boolean(true);
        ok &= check(
            manager.verifyTokens().get().boolValue() &&
                http->requests.size() == 1 && *warnings == 0,
            "forceRefresh failure was not silently ignored before cache check"
        );
    }

    {
        auto queue = bedrock::JsMicrotaskQueue::create();
        auto cacheState = std::make_shared<MemoryCacheState>();
        auto http = std::make_shared<ScriptedHttpClient>(queue);
        http->responses.push_back(response(500, "device endpoint failed"));
        auto warnedBody = std::make_shared<std::string>();
        bedrock::LiveTokenManager manager(
            bedrock::JsRuntimeValue::string("CID"),
            bedrock::JsRuntimeValue::string("scope"),
            makeMemoryCache(cacheState),
            bedrock::LiveTokenManagerDependencies {
                .httpClient = http,
                .microtaskQueue = queue,
                .dateNowMilliseconds = [] { return 100.0; },
                .observers = bedrock::LiveTokenManagerObservers {
                    .consoleWarnText = [warnedBody](const std::string& body) {
                        *warnedBody = body;
                    }
                }
            }
        );
        std::string errorText;
        try {
            (void) manager.authDeviceCode(
                [](bedrock::JsRuntimeValue) {}
            ).get();
        } catch (const std::exception& error) {
            errorText = error.what();
        }
        ok &= check(
            errorText == "Failed to request live.com device code" &&
                *warnedBody == "device endpoint failed" &&
                manager.polling.isUndefined(),
            "non-200 device-code response ordering/error mismatch"
        );
    }
    return ok;
}

bool checkDeviceCodeFlow() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto clock = std::make_shared<double>(3'000'000.0);
    auto cacheState = std::make_shared<MemoryCacheState>();
    auto http = std::make_shared<ScriptedHttpClient>(queue);
    http->responses.push_back(response(
        200,
        R"({"verification_uri":"https://microsoft.com/link","user_code":"AB CD","device_code":"DEV /+?","expires_in":600,"interval":2})",
        {{"set-cookie", "uaid=cookie-value; Path=/; Secure"}}
    ));
    http->responses.push_back(response(
        400,
        R"({"error":"authorization_pending","error_description":"wait"})"
    ));
    http->responses.push_back(response(
        200,
        R"({"access_token":"DEVICE_ACCESS","refresh_token":"DEVICE_REFRESH","expires_in":3600})"
    ));
    auto delays = std::make_shared<std::vector<double>>();
    auto callbackValue = std::make_shared<bedrock::JsRuntimeValue>(
        bedrock::JsRuntimeValue::undefined()
    );
    auto pendingDebug = std::make_shared<int>(0);

    bedrock::LiveTokenManager manager(
        bedrock::JsRuntimeValue::string("CID"),
        bedrock::JsRuntimeValue::array({
            bedrock::JsRuntimeValue::string("scope one")
        }),
        makeMemoryCache(cacheState),
        bedrock::LiveTokenManagerDependencies {
            .httpClient = http,
            .microtaskQueue = queue,
            .dateNowMilliseconds = [clock] { return *clock; },
            .delay = [queue, delays](double milliseconds) {
                delays->push_back(milliseconds);
                return bedrock::JsPromise<void>::resolved(queue);
            },
            .observers = bedrock::LiveTokenManagerObservers {
                .debug = [pendingDebug](
                    const std::string& label,
                    const std::vector<bedrock::JsRuntimeValue>&
                ) {
                    if (label == "[live] Still waiting:") ++*pendingDebug;
                }
            }
        }
    );

    const auto result = manager.authDeviceCode([
        callbackValue
    ](bedrock::JsRuntimeValue value) {
        *callbackValue = std::move(value);
    }).get();

    ok &= check(
        property(result, "accessToken").stringValue() == "DEVICE_ACCESS" &&
            manager.polling.isBool() && !manager.polling.boolValue() &&
            *delays == std::vector<double>({2'000.0, 2'000.0}) &&
            *pendingDebug == 1,
        "device-code polling/result state mismatch"
    );
    ok &= check(
        property(*callbackValue, "message").stringValue() ==
            "To sign in, use a web browser to open the page "
            "https://microsoft.com/link and use the code AB CD or visit "
            "http://microsoft.com/link?otc=AB CD",
        "device-code callback message mismatch"
    );
    ok &= check(
        http->requests.size() == 3,
        "device-code flow issued an unexpected number of fetches"
    );
    if (http->requests.size() == 3) {
        ok &= check(
            http->requests[0].url ==
                bedrock::LiveTokenManager::LiveDeviceCodeRequest &&
                http->requests[0].body ==
                    "scope=scope+one&client_id=CID&response_type=device_code" &&
                http->requests[0].credentials.isString() &&
                http->requests[1].url ==
                    "https://login.live.com/oauth20_token.srf?client_id=CID" &&
                http->requests[1].body ==
                    "client_id=CID&device_code=DEV+%2F%2B%3F&grant_type="
                    "urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code" &&
                http->requests[1].header("Cookie") &&
                *http->requests[1].header("Cookie") == "uaid=cookie-value" &&
                http->requests[1].credentials.isUndefined() &&
                http->requests[2].header("Cookie") &&
                *http->requests[2].header("Cookie") == "uaid=cookie-value",
            "device-code fetch init/cookie propagation mismatch"
        );
    }

    const auto token = property(cacheState->value, "token");
    ok &= check(
        cacheState->partialCalls == 1 &&
            property(token, "access_token").stringValue() ==
                "DEVICE_ACCESS" &&
            property(token, "obtainedOn").numberValue() == *clock,
        "device token was not written through updateCache"
    );
    return ok;
}

bool checkPollingErrorAndTimeout() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto clock = std::make_shared<double>(4'000'000.0);
    auto cacheState = std::make_shared<MemoryCacheState>();
    auto http = std::make_shared<ScriptedHttpClient>(queue);
    http->responses.push_back(response(
        200,
        R"({"verification_uri":"uri","user_code":"code","device_code":"device","expires_in":1,"interval":1})"
    ));
    http->responses.push_back(response(
        400,
        R"({"error":"access_denied","error_description":"no"})"
    ));
    auto debugErrors = std::make_shared<std::vector<std::string>>();

    bedrock::LiveTokenManager manager(
        bedrock::JsRuntimeValue::string("CID"),
        bedrock::JsRuntimeValue::string("scope"),
        makeMemoryCache(cacheState),
        bedrock::LiveTokenManagerDependencies {
            .httpClient = http,
            .microtaskQueue = queue,
            .dateNowMilliseconds = [clock] { return *clock; },
            .delay = [queue, clock](double) {
                *clock += 1'000.0;
                return bedrock::JsPromise<void>::resolved(queue);
            },
            .observers = bedrock::LiveTokenManagerObservers {
                .consoleDebug = [debugErrors](std::exception_ptr error) {
                    debugErrors->push_back(exceptionMessage(error));
                }
            }
        }
    );

    std::string timeout;
    try {
        (void) manager.authDeviceCode([](bedrock::JsRuntimeValue) {}).get();
    } catch (const std::exception& error) {
        timeout = error.what();
    }
    ok &= check(
        timeout == "Authentication failed, timed out" &&
            *debugErrors == std::vector<std::string>({
                "Failed to acquire authorization code from device token "
                "(access_denied) - no"
            }) &&
            manager.polling.isBool() && !manager.polling.boolValue(),
        "poll errors were not swallowed until the JavaScript timeout branch"
    );
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= checkCachedAndRefreshFlow();
    ok &= checkAuthflowRuntimeIntegration();
    ok &= checkVerifyFailureFlow();
    ok &= checkForceRefreshAndInitialDeviceFailure();
    ok &= checkDeviceCodeFlow();
    ok &= checkPollingErrorAndTimeout();
    if (ok) {
        std::cout << "[LIVE-TOKEN] cache/refresh/device/poll parity ok\n";
    }
    return ok ? 0 : 1;
}
