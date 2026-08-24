#include <bedrock/auth/MsalCachePlugin.hpp>
#include <bedrock/auth/MsalError.hpp>
#include <bedrock/auth/MsalHttpClient.hpp>
#include <bedrock/auth/NativeMsalPublicClientApplication.hpp>

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

constexpr const char* kIdToken =
    "eyJhbGciOiJub25lIn0."
    "eyJzdWIiOiJzdWIiLCJ0aWQiOiJ0ZW5hbnQiLCJwcmVmZXJyZWRfdXNlcm5hbWUi"
    "OiJ1QGV4YW1wbGUiLCJuYW1lIjoiVXNlciJ9.x";
constexpr const char* kClientInfo =
    "eyJ1aWQiOiJ1aWQiLCJ1dGlkIjoidXRpZCJ9";

struct ScriptedOutcome {
    bedrock::MsalHttpResponse response;
    std::exception_ptr error;
};

class ScriptedHttpClient final : public bedrock::IMsalHttpClient {
public:
    ScriptedHttpClient(
        std::shared_ptr<bedrock::JsMicrotaskQueue> queue,
        std::shared_ptr<std::vector<std::string>> events
    ) : queue_(std::move(queue)), events_(std::move(events)) {}

    bedrock::JsPromise<bedrock::MsalHttpResponse> send(
        bedrock::MsalHttpRequest request
    ) override {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
        events_->push_back("http:" + request.url);
        if (outcomes_.empty()) {
            return bedrock::JsPromise<bedrock::MsalHttpResponse>::rejected(
                queue_,
                "Unexpected fake MSAL request"
            );
        }
        auto outcome = std::move(outcomes_.front());
        outcomes_.pop_front();
        if (outcome.error) {
            return bedrock::JsPromise<bedrock::MsalHttpResponse>::rejected(
                queue_,
                std::move(outcome.error)
            );
        }
        return bedrock::JsPromise<bedrock::MsalHttpResponse>::resolved(
            queue_,
            std::move(outcome.response)
        );
    }

    void push(bedrock::MsalHttpResponse response) {
        std::lock_guard<std::mutex> lock(mutex_);
        outcomes_.push_back({std::move(response), {}});
    }

    void pushError(std::exception_ptr error) {
        std::lock_guard<std::mutex> lock(mutex_);
        outcomes_.push_back({{}, std::move(error)});
    }

    std::vector<bedrock::MsalHttpRequest> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    std::shared_ptr<bedrock::JsMicrotaskQueue> queue_;
    std::shared_ptr<std::vector<std::string>> events_;
    mutable std::mutex mutex_;
    std::deque<ScriptedOutcome> outcomes_;
    std::vector<bedrock::MsalHttpRequest> requests_;
};

bedrock::MsalHttpResponse response(
    int status,
    bedrock::JsRuntimeValue body,
    std::vector<std::pair<std::string, std::string>> headers = {}
) {
    bedrock::MsalHttpResponse value;
    value.status = status;
    value.headers = std::move(headers);
    value.body = std::move(body);
    return value;
}

bedrock::JsRuntimeValue deviceCodeBody() {
    return bedrock::JsRuntimeValue::object({
        {"user_code", bedrock::JsRuntimeValue::string("UC")},
        {"device_code", bedrock::JsRuntimeValue::string("DC")},
        {"verification_uri", bedrock::JsRuntimeValue::string("URI")},
        {"expires_in", bedrock::JsRuntimeValue::number(900)},
        {"interval", bedrock::JsRuntimeValue::number(0)},
        {"message", bedrock::JsRuntimeValue::string("MSG")}
    });
}

bedrock::JsRuntimeValue tokenBody(
    std::string accessToken,
    std::string refreshToken
) {
    return bedrock::JsRuntimeValue::object({
        {"token_type", bedrock::JsRuntimeValue::string("Bearer")},
        {"scope", bedrock::JsRuntimeValue::string(
            "XboxLive.signin offline_access openid profile"
        )},
        {"expires_in", bedrock::JsRuntimeValue::number(3600)},
        {"ext_expires_in", bedrock::JsRuntimeValue::number(3600)},
        {"access_token", bedrock::JsRuntimeValue::string(
            std::move(accessToken)
        )},
        {"refresh_token", bedrock::JsRuntimeValue::string(
            std::move(refreshToken)
        )},
        {"id_token", bedrock::JsRuntimeValue::string(kIdToken)},
        {"client_info", bedrock::JsRuntimeValue::string(kClientInfo)}
    });
}

std::string stringProperty(
    const bedrock::JsRuntimeValue& value,
    const char* key
) {
    const auto* member = value.get(key);
    return member && member->isString() ? member->stringValue() : std::string();
}

struct Fixture {
    std::shared_ptr<bedrock::JsMicrotaskQueue> queue =
        bedrock::JsMicrotaskQueue::create();
    std::shared_ptr<std::vector<std::string>> events =
        std::make_shared<std::vector<std::string>>();
    std::shared_ptr<std::vector<std::string>> persisted =
        std::make_shared<std::vector<std::string>>();
    std::shared_ptr<ScriptedHttpClient> http =
        std::make_shared<ScriptedHttpClient>(queue, events);
    std::shared_ptr<bedrock::JsRuntimeValue> config;
    std::shared_ptr<bedrock::NativeMsalPublicClientApplication> app;

    Fixture() {
        const auto queueValue = queue;
        const auto eventsValue = events;
        const auto persistedValue = persisted;
        auto before = bedrock::JsRuntimeValue::namedFunction<
            bedrock::MsalCacheHookSignature
        >("beforeCacheAccess", [queueValue, eventsValue](
            bedrock::TokenCacheContextPtr context
        ) {
            eventsValue->push_back("before:true");
            (*context->tokenCache)->deserialize(
                "{\"Account\":{},\"IdToken\":{},\"AccessToken\":{},"
                "\"RefreshToken\":{},\"AppMetadata\":{}}"
            );
            return bedrock::JsPromise<void>::resolved(queueValue);
        });
        auto after = bedrock::JsRuntimeValue::namedFunction<
            bedrock::MsalCacheHookSignature
        >("afterCacheAccess", [queueValue, eventsValue, persistedValue](
            bedrock::TokenCacheContextPtr context
        ) {
            eventsValue->push_back(
                context->cacheHasChanged ? "after:true" : "after:false"
            );
            persistedValue->push_back((*context->tokenCache)->serialize());
            return bedrock::JsPromise<void>::resolved(queueValue);
        });
        auto plugin = bedrock::JsRuntimeValue::object({
            {"beforeCacheAccess", std::move(before)},
            {"afterCacheAccess", std::move(after)}
        });
        config = std::make_shared<bedrock::JsRuntimeValue>(
            bedrock::JsRuntimeValue::object({
                {"auth", bedrock::JsRuntimeValue::object({
                    {"clientId", bedrock::JsRuntimeValue::string("CID")},
                    {"authority", bedrock::JsRuntimeValue::string(
                        "https://login.microsoftonline.com/consumers"
                    )}
                })},
                {"cache", bedrock::JsRuntimeValue::object({
                    {"cachePlugin", std::move(plugin)}
                })}
            })
        );

        bedrock::NativeMsalDependencies dependencies;
        dependencies.microtaskQueue = queue;
        dependencies.httpClient = http;
        dependencies.dateNowMilliseconds = [] { return 1000000.0; };
        dependencies.delayMilliseconds = [queueValue](std::int64_t) {
            return bedrock::JsPromise<void>::resolved(queueValue);
        };
        dependencies.correlationIdFactory = [] {
            return std::string("11111111-1111-4111-8111-111111111111");
        };
        app = std::make_shared<bedrock::NativeMsalPublicClientApplication>(
            config,
            std::move(dependencies)
        );
    }
};

bool checkSuccessFlow() {
    Fixture fixture;
    fixture.http->push(response(200, deviceCodeBody()));
    fixture.http->push(response(200, tokenBody("AT", "RT")));
    fixture.http->push(response(
        200,
        tokenBody("AT2", "RT2"),
        {{"x-ms-request-id", "RID"}}
    ));

    auto deviceRequest = bedrock::JsRuntimeValue::object({
        {"scopes", bedrock::JsRuntimeValue::array({
            bedrock::JsRuntimeValue::string("XboxLive.signin"),
            bedrock::JsRuntimeValue::string("offline_access")
        })},
        {"deviceCodeCallback", bedrock::JsRuntimeValue::namedFunction<
            void(bedrock::JsRuntimeValue)
        >("deviceCodeCallback", [events = fixture.events](
            bedrock::JsRuntimeValue value
        ) {
            events->push_back(
                "callback:" + stringProperty(value, "deviceCode")
            );
        })}
    });
    const auto devicePromise = fixture.app->acquireTokenByDeviceCode(
        deviceRequest
    );
    const auto* synchronousScheme = deviceRequest.get(
        "authenticationScheme"
    );
    if (!synchronousScheme || synchronousScheme->stringValue() != "Bearer") {
        std::cerr << "[FAIL] Native MSAL synchronous request mutation\n";
        return false;
    }
    const auto deviceResult = devicePromise.get();

    if (stringProperty(deviceResult, "authority") !=
            "https://login.microsoftonline.com/consumers/" ||
        stringProperty(deviceResult, "uniqueId") != "sub" ||
        stringProperty(deviceResult, "tenantId") != "tenant" ||
        stringProperty(deviceResult, "accessToken") != "AT" ||
        stringProperty(deviceResult, "correlationId") !=
            "11111111-1111-4111-8111-111111111111" ||
        stringProperty(deviceResult, "requestId") != "") {
        std::cerr << "[FAIL] Native MSAL device AuthenticationResult\n";
        return false;
    }
    const auto* expiresOn = deviceResult.get("expiresOn");
    const auto* extExpiresOn = deviceResult.get("extExpiresOn");
    if (!expiresOn || !expiresOn->isDate() ||
        expiresOn->dateIsoString() != "1970-01-01T01:16:40.000Z" ||
        !extExpiresOn || !extExpiresOn->isDate() ||
        extExpiresOn->dateIsoString() != "1970-01-01T02:16:40.000Z") {
        std::cerr << "[FAIL] Native MSAL Date result semantics\n";
        return false;
    }
    const auto* account = deviceResult.get("account");
    const auto* profiles = account ? account->get("tenantProfiles") : nullptr;
    if (!account || stringProperty(*account, "homeAccountId") != "uid.utid" ||
        stringProperty(*account, "environment") != "login.windows.net" ||
        !profiles || !profiles->isMap() || profiles->mapSize() != 1) {
        std::cerr << "[FAIL] Native MSAL account/tenantProfiles result\n";
        return false;
    }

    const auto* deviceScopes = deviceRequest.get("scopes");
    if (!deviceScopes || deviceScopes->length() != 5 ||
        stringProperty(deviceRequest, "correlationId") !=
            "11111111-1111-4111-8111-111111111111" ||
        stringProperty(deviceRequest, "authority") !=
            "https://login.microsoftonline.com/consumers") {
        std::cerr << "[FAIL] Native MSAL device Object.assign mutation\n";
        return false;
    }

    auto refreshRequest = bedrock::JsRuntimeValue::object({
        {"refreshToken", bedrock::JsRuntimeValue::string("R T/+?")},
        {"scopes", bedrock::JsRuntimeValue::array({
            bedrock::JsRuntimeValue::string("XboxLive.signin"),
            bedrock::JsRuntimeValue::string("offline_access")
        })},
        {"correlationId", bedrock::JsRuntimeValue::string(
            "22222222-2222-4222-8222-222222222222"
        )}
    });
    const auto refreshResult = fixture.app->acquireTokenByRefreshToken(
        refreshRequest
    ).get();
    if (stringProperty(refreshResult, "accessToken") != "AT2" ||
        stringProperty(refreshResult, "requestId") != "RID" ||
        stringProperty(refreshResult, "correlationId") !=
            "22222222-2222-4222-8222-222222222222") {
        std::cerr << "[FAIL] Native MSAL refresh AuthenticationResult\n";
        return false;
    }
    const auto* refreshScopes = refreshRequest.get("scopes");
    if (!refreshScopes || refreshScopes->length() != 2 ||
        refreshRequest.hasOwn("authority") ||
        stringProperty(refreshRequest, "authenticationScheme") != "Bearer") {
        std::cerr << "[FAIL] Native MSAL refresh spread/source mutation\n";
        return false;
    }

    const auto requests = fixture.http->requests();
    if (requests.size() != 3 ||
        requests[0].url !=
            "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode" ||
        requests[0].body !=
            "scope=XboxLive.signin%20offline_access%20openid%20profile&client_id=CID" ||
        requests[1].url !=
            "https://login.microsoftonline.com/consumers/oauth2/v2.0/token?client-request-id=11111111-1111-4111-8111-111111111111" ||
        requests[1].body !=
            "scope=XboxLive.signin%20offline_access%20openid%20profile&client_id=CID&grant_type=device_code&device_code=DC&client-request-id=11111111-1111-4111-8111-111111111111&client_info=1&x-client-SKU=msal.js.node&x-client-VER=2.16.3&x-client-OS=win32&x-client-CPU=x64&x-ms-lib-capability=retry-after, h429&x-client-current-telemetry=5|671,0,,,|,&x-client-last-telemetry=5|0|||0,0" ||
        requests[2].url !=
            "https://login.microsoftonline.com/consumers/oauth2/v2.0/token?client-request-id=22222222-2222-4222-8222-222222222222" ||
        requests[2].body !=
            "client_id=CID&scope=XboxLive.signin%20offline_access%20openid%20profile&grant_type=refresh_token&client_info=1&x-client-SKU=msal.js.node&x-client-VER=2.16.3&x-client-OS=win32&x-client-CPU=x64&x-ms-lib-capability=retry-after, h429&x-client-current-telemetry=5|872,0,,,|,&x-client-last-telemetry=5|0|||0,0&refresh_token=R%20T%2F%2B%3F") {
        std::cerr << "[FAIL] Native MSAL exact HTTP requests\n";
        return false;
    }

    const std::vector<std::string> expectedEvents {
        "http:https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode",
        "callback:DC",
        "http:https://login.microsoftonline.com/consumers/oauth2/v2.0/token?client-request-id=11111111-1111-4111-8111-111111111111",
        "before:true",
        "after:true",
        "http:https://login.microsoftonline.com/consumers/oauth2/v2.0/token?client-request-id=22222222-2222-4222-8222-222222222222",
        "before:true",
        "after:true"
    };
    if (*fixture.events != expectedEvents || fixture.persisted->size() != 2 ||
        fixture.persisted->back().find("\"secret\":\"AT2\"") ==
            std::string::npos ||
        fixture.persisted->back().find("\"secret\":\"RT2\"") ==
            std::string::npos) {
        std::cerr << "[FAIL] Native MSAL hook/cache ordering\n";
        return false;
    }
    return true;
}

bool checkExactErrors() {
    Fixture slow;
    slow.http->push(response(200, deviceCodeBody()));
    slow.http->push(response(400, bedrock::JsRuntimeValue::object({
        {"error", bedrock::JsRuntimeValue::string("slow_down")}
    })));
    auto request = bedrock::JsRuntimeValue::object({
        {"scopes", bedrock::JsRuntimeValue::array({
            bedrock::JsRuntimeValue::string("XboxLive.signin"),
            bedrock::JsRuntimeValue::string("offline_access")
        })},
        {"correlationId", bedrock::JsRuntimeValue::string(
            "11111111-1111-4111-8111-111111111111"
        )},
        {"deviceCodeCallback", bedrock::JsRuntimeValue::namedFunction<
            void(bedrock::JsRuntimeValue)
        >("deviceCodeCallback", [](bedrock::JsRuntimeValue) {})}
    });
    try {
        (void) slow.app->acquireTokenByDeviceCode(request).get();
        std::cerr << "[FAIL] Native MSAL slow_down did not reject\n";
        return false;
    } catch (const bedrock::MsalAuthError& error) {
        const auto serialized = error.jsonStringify();
        const std::string expected =
            "{\"errorCode\":\"post_request_failed\",\"errorMessage\":"
            "\"Post request failed from the network, could be a 4xx/5xx or "
            "a network unavailability. Please check the exact error code for "
            "details. slow_down\",\"subError\":\"\",\"name\":\"AuthError\","
            "\"correlationId\":\"11111111-1111-4111-8111-111111111111\"}";
        if (!serialized || *serialized != expected ||
            slow.events->size() != 2) {
            std::cerr << "[FAIL] Native MSAL slow_down error shape: "
                      << (serialized ? *serialized : "<undefined>")
                      << " events=" << slow.events->size() << "\n";
            return false;
        }
    }

    slow.http->push(response(200, tokenBody("AT-AFTER-FAIL", "RT-AFTER-FAIL")));
    auto refreshAfterFailure = bedrock::JsRuntimeValue::object({
        {"refreshToken", bedrock::JsRuntimeValue::string("R")},
        {"scopes", bedrock::JsRuntimeValue::array({
            bedrock::JsRuntimeValue::string("XboxLive.signin"),
            bedrock::JsRuntimeValue::string("offline_access")
        })},
        {"correlationId", bedrock::JsRuntimeValue::string(
            "22222222-2222-4222-8222-222222222222"
        )}
    });
    (void) slow.app->acquireTokenByRefreshToken(
        refreshAfterFailure
    ).get();
    const auto telemetryRequests = slow.http->requests();
    const std::string expectedLastTelemetry =
        "x-client-last-telemetry=5|0|671,"
        "11111111-1111-4111-8111-111111111111|post_request_failed|1,0";
    if (telemetryRequests.size() != 3 ||
        telemetryRequests[2].body.find(expectedLastTelemetry) ==
            std::string::npos ||
        !slow.app->serverTelemetryCache()->getServerTelemetry(
            "server-telemetry-CID"
        ).isNull()) {
        std::cerr << "[FAIL] Native MSAL server telemetry lifecycle\n";
        return false;
    }

    Fixture network;
    network.http->pushError(std::make_exception_ptr(
        std::runtime_error("socket exploded")
    ));
    try {
        (void) network.app->acquireTokenByDeviceCode(request).get();
        std::cerr << "[FAIL] Native MSAL network failure did not reject\n";
        return false;
    } catch (const bedrock::MsalAuthError& error) {
        const auto serialized = error.jsonStringify();
        const std::string expected =
            "{\"errorCode\":\"network_error\",\"errorMessage\":"
            "\"Network request failed\",\"subError\":\"\",\"name\":"
            "\"ClientAuthError\",\"correlationId\":"
            "\"11111111-1111-4111-8111-111111111111\"}";
        if (!serialized || *serialized != expected) {
            std::cerr << "[FAIL] Native MSAL network error shape\n";
            return false;
        }
    }
    return true;
}

bool checkCancellationAndHookFinally() {
    Fixture cancelled;
    cancelled.http->push(response(200, deviceCodeBody()));
    auto cancelRequest = bedrock::JsRuntimeValue::object({
        {"scopes", bedrock::JsRuntimeValue::array({
            bedrock::JsRuntimeValue::string("XboxLive.signin"),
            bedrock::JsRuntimeValue::string("offline_access")
        })},
        {"correlationId", bedrock::JsRuntimeValue::string(
            "11111111-1111-4111-8111-111111111111"
        )}
    });
    std::weak_ptr<bedrock::JsRuntimeObject> cancelTarget =
        cancelRequest.objectNode();
    cancelRequest.set(
        "deviceCodeCallback",
        bedrock::JsRuntimeValue::namedFunction<
            void(bedrock::JsRuntimeValue)
        >("deviceCodeCallback", [cancelTarget](bedrock::JsRuntimeValue) {
            if (const auto target = cancelTarget.lock()) {
                target->set(
                    "cancel",
                    bedrock::JsRuntimeValue::boolean(true)
                );
            }
        })
    );
    try {
        (void) cancelled.app->acquireTokenByDeviceCode(
            cancelRequest
        ).get();
        std::cerr << "[FAIL] Native MSAL cancellation did not reject\n";
        return false;
    } catch (const bedrock::MsalAuthError& error) {
        const auto serialized = error.jsonStringify();
        const std::string expected =
            "{\"errorCode\":\"device_code_polling_cancelled\","
            "\"errorMessage\":\"Caller has cancelled token endpoint polling "
            "during device code flow by setting DeviceCodeRequest.cancel = "
            "true.\",\"subError\":\"\",\"name\":\"ClientAuthError\","
            "\"correlationId\":\"11111111-1111-4111-8111-111111111111\"}";
        if (!serialized || *serialized != expected ||
            cancelled.http->requests().size() != 1) {
            std::cerr << "[FAIL] Native MSAL cancellation error shape\n";
            return false;
        }
    }

    Fixture hooks;
    auto* cache = hooks.config->get("cache");
    auto* plugin = cache ? cache->get("cachePlugin") : nullptr;
    if (!plugin) {
        std::cerr << "[FAIL] Native MSAL test plugin unavailable\n";
        return false;
    }
    const auto queue = hooks.queue;
    const auto events = hooks.events;
    plugin->set(
        "beforeCacheAccess",
        bedrock::JsRuntimeValue::namedFunction<
            bedrock::MsalCacheHookSignature
        >("beforeCacheAccess", [queue, events](
            bedrock::TokenCacheContextPtr
        ) {
            events->push_back("before-error");
            return bedrock::JsPromise<void>::rejected(
                queue,
                "before exploded"
            );
        })
    );
    plugin->set(
        "afterCacheAccess",
        bedrock::JsRuntimeValue::namedFunction<
            bedrock::MsalCacheHookSignature
        >("afterCacheAccess", [queue, events](
            bedrock::TokenCacheContextPtr
        ) {
            events->push_back("after-error");
            return bedrock::JsPromise<void>::rejected(
                queue,
                "after exploded"
            );
        })
    );
    hooks.http->push(response(200, deviceCodeBody()));
    hooks.http->push(response(200, tokenBody("AT", "RT")));
    auto hookRequest = bedrock::JsRuntimeValue::object({
        {"scopes", bedrock::JsRuntimeValue::array({
            bedrock::JsRuntimeValue::string("XboxLive.signin"),
            bedrock::JsRuntimeValue::string("offline_access")
        })},
        {"correlationId", bedrock::JsRuntimeValue::string(
            "11111111-1111-4111-8111-111111111111"
        )},
        {"deviceCodeCallback", bedrock::JsRuntimeValue::namedFunction<
            void(bedrock::JsRuntimeValue)
        >("deviceCodeCallback", [](bedrock::JsRuntimeValue) {})}
    });
    try {
        (void) hooks.app->acquireTokenByDeviceCode(hookRequest).get();
        std::cerr << "[FAIL] Native MSAL hook errors did not reject\n";
        return false;
    } catch (const std::runtime_error& error) {
        const std::vector<std::string> suffix {
            "before-error",
            "after-error"
        };
        if (std::string(error.what()) != "after exploded" ||
            hooks.events->size() < 2 ||
            std::vector<std::string>(
                hooks.events->end() - 2,
                hooks.events->end()
            ) != suffix) {
            std::cerr << "[FAIL] Native MSAL finally override semantics\n";
            return false;
        }
    }
    return true;
}

bool checkLiveDeviceRequestReads() {
    Fixture fixture;
    fixture.http->push(response(200, deviceCodeBody()));
    auto fallbackBody = tokenBody("AT-LIVE", "RT-LIVE");
    fallbackBody.objectNode()->erase("scope");
    fixture.http->push(response(200, std::move(fallbackBody)));

    auto request = bedrock::JsRuntimeValue::object({
        {"scopes", bedrock::JsRuntimeValue::array({
            bedrock::JsRuntimeValue::string("XboxLive.signin"),
            bedrock::JsRuntimeValue::string("offline_access")
        })},
        {"correlationId", bedrock::JsRuntimeValue::string(
            "11111111-1111-4111-8111-111111111111"
        )}
    });
    std::weak_ptr<bedrock::JsRuntimeObject> target = request.objectNode();
    request.set(
        "deviceCodeCallback",
        bedrock::JsRuntimeValue::namedFunction<
            void(bedrock::JsRuntimeValue)
        >("deviceCodeCallback", [target](bedrock::JsRuntimeValue) {
            if (const auto value = target.lock()) {
                value->set(
                    "correlationId",
                    bedrock::JsRuntimeValue::string(
                        "33333333-3333-4333-8333-333333333333"
                    )
                );
                value->set(
                    "scopes",
                    bedrock::JsRuntimeValue::array({
                        bedrock::JsRuntimeValue::string("Changed.Scope"),
                        bedrock::JsRuntimeValue::string("offline_access")
                    })
                );
            }
        })
    );

    const auto result = fixture.app->acquireTokenByDeviceCode(request).get();
    const auto requests = fixture.http->requests();
    const auto* scopes = result.get("scopes");
    if (requests.size() != 2 ||
        requests[0].body.find("XboxLive.signin") == std::string::npos ||
        requests[1].url.find(
            "client-request-id=33333333-3333-4333-8333-333333333333"
        ) == std::string::npos ||
        requests[1].body.find(
            "scope=Changed.Scope%20offline_access%20openid%20profile"
        ) == std::string::npos ||
        requests[1].body.find(
            "client-request-id=33333333-3333-4333-8333-333333333333"
        ) == std::string::npos ||
        stringProperty(result, "correlationId") !=
            "33333333-3333-4333-8333-333333333333" ||
        !scopes || scopes->length() != 4 ||
        !scopes->get(0) || scopes->get(0)->stringValue() != "Changed.Scope") {
        std::cerr << "[FAIL] Native MSAL live callback request reads\n";
        return false;
    }
    return true;
}

bool checkClearedLiveCorrelationId() {
    Fixture fixture;
    fixture.http->push(response(200, deviceCodeBody()));
    fixture.http->push(response(200, tokenBody("AT-CLEAR", "RT-CLEAR")));

    auto request = bedrock::JsRuntimeValue::object({
        {"scopes", bedrock::JsRuntimeValue::array({
            bedrock::JsRuntimeValue::string("XboxLive.signin"),
            bedrock::JsRuntimeValue::string("offline_access")
        })},
        {"correlationId", bedrock::JsRuntimeValue::string(
            "22222222-2222-4222-8222-222222222222"
        )}
    });
    std::weak_ptr<bedrock::JsRuntimeObject> target = request.objectNode();
    request.set(
        "deviceCodeCallback",
        bedrock::JsRuntimeValue::namedFunction<
            void(bedrock::JsRuntimeValue)
        >("deviceCodeCallback", [target](bedrock::JsRuntimeValue) {
            if (const auto value = target.lock()) {
                value->set(
                    "correlationId",
                    bedrock::JsRuntimeValue::undefined()
                );
            }
        })
    );

    const auto result = fixture.app->acquireTokenByDeviceCode(request).get();
    const auto requests = fixture.http->requests();
    const auto* resultCorrelation = result.get("correlationId");
    if (requests.size() != 2 ||
        requests[1].url.find("client-request-id=undefined") ==
            std::string::npos ||
        requests[1].body.find(
            "client-request-id=11111111-1111-4111-8111-111111111111"
        ) == std::string::npos ||
        !resultCorrelation || !resultCorrelation->isUndefined()) {
        std::cerr << "[FAIL] Native MSAL cleared correlation divergence\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!checkSuccessFlow()) return 1;
    if (!checkExactErrors()) return 1;
    if (!checkCancellationAndHookFinally()) return 1;
    if (!checkLiveDeviceRequestReads()) return 1;
    if (!checkClearedLiveCorrelationId()) return 1;
    std::cout << "[MSAL-NATIVE] device/refresh/cache/error parity ok\n";
    return 0;
}
