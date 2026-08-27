#include <bedrock/auth/MinecraftBedrockServicesManager.hpp>
#include <bedrock/auth/MinecraftBedrockTokenManager.hpp>
#include <bedrock/auth/PlayfabTokenManager.hpp>

#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

bool check(bool condition, const std::string& message) {
    if (!condition) std::cerr << "[BEDROCK-TOKENS] " << message << "\n";
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
    std::string statusText = {}
) {
    bedrock::XboxTokenHttpResponse result;
    result.status = status;
    result.statusText = statusText.empty()
        ? (status == 200 ? "OK" : "Error")
        : std::move(statusText);
    result.bodyText = std::move(body);
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
                "unexpected Bedrock token-manager fetch"
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

bedrock::BedrockTokenManagerDependencies dependencies(
    const std::shared_ptr<bedrock::JsMicrotaskQueue>& queue,
    const std::shared_ptr<ScriptedHttpClient>& http,
    const std::shared_ptr<double>& clock
) {
    return bedrock::BedrockTokenManagerDependencies {
        .httpClient = http,
        .microtaskQueue = queue,
        .dateNowMilliseconds = [clock] { return *clock; }
    };
}

bool checkBedrockAuthenticationManager() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto clock = std::make_shared<double>(1'000'000.0);
    auto cacheState = std::make_shared<MemoryCacheState>();
    auto http = std::make_shared<ScriptedHttpClient>(queue);
    const std::string jwt = "e30.eyJleHAiOjIwMDB9.signature";
    auto chain = bedrock::JsRuntimeValue::array({
        bedrock::JsRuntimeValue::string(jwt),
        bedrock::JsRuntimeValue::string("tail")
    });
    cacheState->value = bedrock::JsRuntimeValue::object({
        {"mca", bedrock::JsRuntimeValue::object({
            {"chain", chain},
            {"obtainedOn", bedrock::JsRuntimeValue::number(10.0)}
        })}
    });

    bedrock::MinecraftBedrockTokenManager manager(
        makeMemoryCache(cacheState),
        dependencies(queue, http, clock)
    );
    const auto cached = manager.getCachedAccessToken().get();
    ok &= check(
        property(cached, "valid").isBool() &&
            property(cached, "valid").boolValue() &&
            property(cached, "until").isDate() &&
            property(cached, "until").dateMilliseconds() == 2'000'000.0 &&
            property(cached, "chain").sharesIdentityWith(chain),
        "Bedrock JWT cache decoding/expiry/result shape diverged"
    );
    ok &= check(
        manager.verifyTokens().get().boolValue(),
        "valid Bedrock chain did not satisfy verifyTokens"
    );
    manager.forceRefresh = bedrock::JsRuntimeValue::boolean(true);
    ok &= check(
        !manager.verifyTokens().get().boolValue(),
        "forceRefresh did not invalidate a cached Bedrock chain"
    );
    manager.forceRefresh = bedrock::JsRuntimeValue::undefined();

    http->responses.push_back(response(
        200,
        std::string("{\"chain\":[\"") + jwt +
            "\",\"fresh\"],\"extra\":\"ok\"}"
    ));
    const auto fetched = manager.getAccessToken(
        bedrock::JsRuntimeValue::string("PUBLIC-KEY"),
        bedrock::JsRuntimeValue::object({
            {"userHash", bedrock::JsRuntimeValue::string("USER")},
            {"XSTSToken", bedrock::JsRuntimeValue::string("XSTS")}
        })
    ).get();
    ok &= check(
        property(fetched, "extra").stringValue() == "ok" &&
            http->requests.size() == 1,
        "Bedrock authentication response was not returned"
    );
    if (!http->requests.empty()) {
        const auto& request = http->requests.front();
        const auto contentType = request.header("Content-Type");
        const auto userAgent = request.header("User-Agent");
        const auto authorization = request.header("Authorization");
        ok &= check(
            request.method == "post" &&
                request.url ==
                    bedrock::MinecraftBedrockTokenManager::
                        AuthenticationEndpoint &&
                request.body ==
                    R"({"identityPublicKey":"PUBLIC-KEY"})" &&
                contentType && *contentType == "application/json" &&
                userAgent && *userAgent == "MCPE/UWP" &&
                authorization &&
                *authorization == "XBL3.0 x=USER;XSTS",
            "Bedrock authentication fetch init differs from the JS source"
        );
    }
    const auto stored = property(cacheState->value, "mca");
    ok &= check(
        cacheState->partialCalls == 1 &&
            property(stored, "extra").stringValue() == "ok" &&
            property(stored, "obtainedOn").numberValue() == *clock,
        "Bedrock response was not shallow-spread into the mca cache"
    );

    auto errorState = std::make_shared<MemoryCacheState>();
    auto errorHttp = std::make_shared<ScriptedHttpClient>(queue);
    errorHttp->responses.push_back(response(
        401,
        "denied",
        "Unauthorized"
    ));
    bedrock::MinecraftBedrockTokenManager failing(
        makeMemoryCache(errorState),
        dependencies(queue, errorHttp, clock)
    );
    std::string errorMessage;
    try {
        (void) failing.getAccessToken(
            bedrock::JsRuntimeValue::string("key"),
            bedrock::JsRuntimeValue::object()
        ).get();
    } catch (const std::exception& error) {
        errorMessage = error.what();
    }
    ok &= check(
        errorMessage == "401 Unauthorized denied" &&
            errorState->partialCalls == 0,
        "Bedrock checkStatus error/cache ordering diverged"
    );
    return ok;
}

bool checkBedrockServicesManager() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto clock = std::make_shared<double>(1'000'000.0);
    auto cacheState = std::make_shared<MemoryCacheState>();
    auto http = std::make_shared<ScriptedHttpClient>(queue);
    bedrock::MinecraftBedrockServicesTokenManager manager(
        makeMemoryCache(cacheState),
        dependencies(queue, http, clock)
    );

    const auto missing = manager.getCachedAccessToken().get();
    ok &= check(
        missing.isObject() &&
            property(missing, "valid").isBool() &&
            !property(missing, "valid").boolValue() &&
            missing.ownProperties().size() == 1,
        "missing mcs cache did not return exactly { valid: false }"
    );

    auto cachedData = bedrock::JsRuntimeValue::object({
        {"mcToken", bedrock::JsRuntimeValue::string("CACHED-MC")},
        {
            "validUntil",
            bedrock::JsRuntimeValue::string("1970-01-01T00:33:20.000Z")
        },
        {"treatmentContext", bedrock::JsRuntimeValue::string("cached")}
    });
    cacheState->value.set("mcs", cachedData);
    const auto cached = manager.getCachedAccessToken().get();
    ok &= check(
        property(cached, "valid").boolValue() &&
            property(cached, "until").isDate() &&
            property(cached, "until").dateMilliseconds() == 2'000'000.0 &&
            property(cached, "token").stringValue() == "CACHED-MC" &&
            property(cached, "data").sharesIdentityWith(cachedData),
        "mcs ISO expiry/cache result shape diverged"
    );

    const std::string serviceResponse =
        R"({"result":{"authorizationHeader":"MC-TOKEN","validUntil":"1970-01-01T00:33:20.000Z","treatments":["A"],"configurations":{"flag":true},"treatmentContext":"CTX"}})";
    http->responses.push_back(response(200, serviceResponse));
    const auto fetched = manager.getAccessToken(
        bedrock::JsRuntimeValue::string("SESSION")
    ).get();
    ok &= check(
        property(fetched, "mcToken").stringValue() == "MC-TOKEN" &&
            property(fetched, "treatmentContext").stringValue() == "CTX" &&
            http->requests.size() == 1,
        "mcs response fields were not projected into tokenResponse"
    );
    if (!http->requests.empty()) {
        const auto& request = http->requests[0];
        ok &= check(
            request.method == "post" &&
                request.url ==
                    bedrock::MinecraftBedrockServicesTokenManager::
                        SessionStartEndpoint &&
                request.body ==
                    R"({"device":{"applicationType":"MinecraftPE","gameVersion":"1.20.62","id":"c1681ad3-415e-30cd-abd3-3b8f51e771d1","memory":"8589934592","platform":"Windows10","playFabTitleId":"20CA2","storePlatform":"uwp.store","type":"Windows10"},"user":{"token":"SESSION","tokenType":"PlayFab"}})" &&
                request.header("Content-Type") &&
                *request.header("Content-Type") == "application/json",
            "mcs default device/session request differs from JS"
        );
    }
    const auto stored = property(cacheState->value, "mcs");
    ok &= check(
        cacheState->partialCalls == 1 &&
            property(stored, "mcToken").stringValue() == "MC-TOKEN" &&
            property(stored, "configurations").isObject(),
        "mcs tokenResponse was not cached under mcs"
    );

    http->responses.push_back(response(200, serviceResponse));
    auto options = bedrock::JsRuntimeValue::object({
        {"applicationType", bedrock::JsRuntimeValue::null()},
        {"version", bedrock::JsRuntimeValue::string("9.9")},
        {"deviceId", bedrock::JsRuntimeValue::string("DEVICE")},
        {"deviceMemory", bedrock::JsRuntimeValue::number(0)},
        {"platform", bedrock::JsRuntimeValue::boolean(false)},
        {"playFabtitleId", bedrock::JsRuntimeValue::string("TITLE")},
        {"storePlatform", bedrock::JsRuntimeValue::string("")}
    });
    (void) manager.getAccessToken(
        bedrock::JsRuntimeValue::string("S2"),
        options
    ).get();
    ok &= check(
        http->requests.size() == 2 &&
            http->requests[1].body ==
                R"({"device":{"applicationType":"MinecraftPE","gameVersion":"9.9","id":"DEVICE","memory":0,"platform":false,"playFabTitleId":"TITLE","storePlatform":"","type":"Windows10"},"user":{"token":"S2","tokenType":"PlayFab"}})",
        "mcs nullish-coalescing option semantics differ from JS"
    );
    return ok;
}

bool checkPlayfabManager() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto clock = std::make_shared<double>(1'000'000.0);
    auto cacheState = std::make_shared<MemoryCacheState>();
    auto http = std::make_shared<ScriptedHttpClient>(queue);
    auto entity = bedrock::JsRuntimeValue::object({
        {
            "TokenExpiration",
            bedrock::JsRuntimeValue::string("1970-01-01T00:33:20.000Z")
        }
    });
    auto cachedData = bedrock::JsRuntimeValue::object({
        {"EntityToken", entity},
        {"SessionTicket", bedrock::JsRuntimeValue::string("CACHED-PF")}
    });
    cacheState->value = bedrock::JsRuntimeValue::object({
        {"pfb", cachedData}
    });

    bedrock::PlayfabTokenManager manager(
        makeMemoryCache(cacheState),
        dependencies(queue, http, clock)
    );
    const auto cached = manager.getCachedAccessToken().get();
    ok &= check(
        property(cached, "valid").boolValue() &&
            property(cached, "until").dateMilliseconds() == 2'000'000.0 &&
            property(cached, "data").sharesIdentityWith(cachedData),
        "PlayFab nested EntityToken expiry/cache shape diverged"
    );

    http->responses.push_back(response(
        500,
        R"({"data":{"SessionTicket":"PF-SESSION","EntityToken":{"TokenExpiration":"1970-01-01T00:33:20.000Z"}}})",
        "Server Error"
    ));
    const auto fetched = manager.getAccessToken(
        bedrock::JsRuntimeValue::object({
            {"userHash", bedrock::JsRuntimeValue::string("USER")},
            {"XSTSToken", bedrock::JsRuntimeValue::string("XSTS")}
        })
    ).get();
    ok &= check(
        property(fetched, "SessionTicket").stringValue() == "PF-SESSION" &&
            cacheState->partialCalls == 1,
        "PlayFab did not parse/cache a non-2xx JSON body like the JS source"
    );
    if (!http->requests.empty()) {
        const auto& request = http->requests.front();
        ok &= check(
            request.method == "post" &&
                request.url == bedrock::PlayfabTokenManager::LoginWithXboxEndpoint &&
                request.header("Content-Type") &&
                *request.header("Content-Type") == "application/json" &&
                request.body ==
                    R"({"CreateAccount":true,"EncryptedRequest":null,"InfoRequestParameters":{"GetCharacterInventories":false,"GetCharacterList":false,"GetPlayerProfile":true,"GetPlayerStatistics":false,"GetTitleData":false,"GetUserAccountInfo":true,"GetUserData":false,"GetUserInventory":false,"GetUserReadOnlyData":false,"GetUserVirtualCurrency":false,"PlayerStatisticNames":null,"ProfileConstraints":null,"TitleDataKeys":null,"UserDataKeys":null,"UserReadOnlyDataKeys":null},"PlayerSecret":null,"TitleId":"20CA2","XboxToken":"XBL3.0 x=USER;XSTS"})",
            "PlayFab LoginWithXbox request differs from JS"
        );
    }
    const auto stored = property(cacheState->value, "pfb");
    ok &= check(
        property(stored, "SessionTicket").stringValue() == "PF-SESSION",
        "PlayFab response data was not cached under pfb"
    );
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= checkBedrockAuthenticationManager();
    ok &= checkBedrockServicesManager();
    ok &= checkPlayfabManager();
    if (!ok) return 1;
    std::cout << "Bedrock-only token manager smoke checks passed\n";
    return 0;
}
