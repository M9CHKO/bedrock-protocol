#include <bedrock/auth/BedrockAuthJwt.hpp>
#include <bedrock/auth/MicrosoftAuthFlow.hpp>

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
    if (!condition) std::cerr << "[MICROSOFT-AUTH-FLOW] " << message << "\n";
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
            for (const auto& item : state->value.ownProperties()) {
                merged.set(item.key, item.value);
            }
            for (const auto& item : update.ownProperties()) {
                merged.set(item.key, item.value);
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
    result.statusText = status == 200 ? "OK" : "Error";
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
                "unexpected MicrosoftAuthFlow fetch"
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

bedrock::XboxProofKey fixedProofKey() {
    constexpr std::string_view privatePem =
        "-----BEGIN PRIVATE KEY-----\n"
        "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgAAAAAAAAAAAAAAAA\n"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAGhRANCAARrF9Hy4SxCR/i85uVjpEDydwN9gS3r\n"
        "M6D0oTlF2JjClk/jQuL+Gn+bjufrSnwPnhYrzjNXazFezsu2QGg3v1H1\n"
        "-----END PRIVATE KEY-----\n";
    return bedrock::XboxProofKey::fromPrivateKeyPem(privatePem);
}

std::string jwt(std::string payload) {
    std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
    return "e30." + bedrock::BedrockAuthJwt::base64Url(bytes) + ".sig";
}

struct FlowFixture {
    explicit FlowFixture(
        std::string flowName = "live",
        bool titleAuth = true
    ) : queue(bedrock::JsMicrotaskQueue::create()),
        http(std::make_shared<ScriptedHttpClient>(queue)),
        clock(std::make_shared<double>(1'700'000'000'000.0)),
        liveCache(std::make_shared<MemoryCacheState>()),
        xboxCache(std::make_shared<MemoryCacheState>()),
        bedrockCache(std::make_shared<MemoryCacheState>()),
        servicesCache(std::make_shared<MemoryCacheState>()),
        playfabCache(std::make_shared<MemoryCacheState>()) {
        liveCache->value = bedrock::JsRuntimeValue::object({
            {"token", bedrock::JsRuntimeValue::object({
                {"access_token", bedrock::JsRuntimeValue::string("MSA")},
                {"refresh_token", bedrock::JsRuntimeValue::string("RT")},
                {"expires_in", bedrock::JsRuntimeValue::number(5'000'000)},
                {"obtainedOn", bedrock::JsRuntimeValue::number(*clock)}
            })}
        });
        live = std::make_shared<bedrock::LiveTokenManager>(
            bedrock::JsRuntimeValue::string("APP"),
            bedrock::JsRuntimeValue::array({
                bedrock::JsRuntimeValue::string(
                    "service::user.auth.xboxlive.com::MBI_SSL"
                )
            }),
            makeMemoryCache(liveCache),
            bedrock::LiveTokenManagerDependencies {
                .httpClient = http,
                .microtaskQueue = queue,
                .dateNowMilliseconds = [value = clock] { return *value; },
                .delay = [queueValue = queue](double) {
                    return bedrock::JsPromise<void>::resolved(queueValue);
                }
            }
        );
        xbox = std::make_shared<bedrock::XboxTokenManager>(
            fixedProofKey(),
            makeMemoryCache(xboxCache),
            bedrock::XboxTokenManagerDependencies {
                .httpClient = http,
                .microtaskQueue = queue,
                .dateNowMilliseconds = [value = clock] { return *value; }
            }
        );
        const auto managerDependencies =
            bedrock::BedrockTokenManagerDependencies {
                .httpClient = http,
                .microtaskQueue = queue,
                .dateNowMilliseconds = [value = clock] { return *value; }
            };
        bedrockManager =
            std::make_shared<bedrock::MinecraftBedrockTokenManager>(
                makeMemoryCache(bedrockCache),
                managerDependencies
            );
        services = std::make_shared<
            bedrock::MinecraftBedrockServicesTokenManager
        >(makeMemoryCache(servicesCache), managerDependencies);
        playfab = std::make_shared<bedrock::PlayfabTokenManager>(
            makeMemoryCache(playfabCache),
            managerDependencies
        );
        options = bedrock::JsRuntimeValue::object({
            {"flow", bedrock::JsRuntimeValue::string(flowName)},
            {"authTitle", bedrock::JsRuntimeValue::string("APP")},
            {"deviceType", bedrock::JsRuntimeValue::string("Nintendo")}
        });
        auth = std::make_shared<bedrock::MicrosoftAuthFlow>(
            bedrock::JsRuntimeValue::string("User"),
            options,
            bedrock::MicrosoftAuthFlowManagers {
                .live = live,
                .xbox = xbox,
                .bedrock = bedrockManager,
                .bedrockServices = services,
                .playfab = playfab
            },
            titleAuth
                ? bedrock::JsRuntimeValue::boolean(true)
                : bedrock::JsRuntimeValue::undefined(),
            bedrock::MicrosoftAuthFlowDependencies {
                .microtaskQueue = queue,
                .delay = [this](double milliseconds) {
                    delays.push_back(milliseconds);
                    return bedrock::JsPromise<void>::resolved(queue);
                },
                .observers = {
                    .consoleInfo = [this](const std::string& message) {
                        info.push_back(message);
                    }
                }
            }
        );
    }

    void seedXsts(std::string_view relyingParty, std::string token = "XSTS") {
        const auto key = bedrock::XboxTokenManager::relyingPartyCacheKey(
            bedrock::JsRuntimeValue::string(relyingParty)
        );
        xboxCache->value.set(key, bedrock::JsRuntimeValue::object({
            {"userXUID", bedrock::JsRuntimeValue::string("42")},
            {"userHash", bedrock::JsRuntimeValue::string("HASH")},
            {"XSTSToken", bedrock::JsRuntimeValue::string(std::move(token))},
            {
                "expiresOn",
                bedrock::JsRuntimeValue::string("2030-01-01T00:00:00Z")
            }
        }));
    }

    std::shared_ptr<bedrock::JsMicrotaskQueue> queue;
    std::shared_ptr<ScriptedHttpClient> http;
    std::shared_ptr<double> clock;
    std::shared_ptr<MemoryCacheState> liveCache;
    std::shared_ptr<MemoryCacheState> xboxCache;
    std::shared_ptr<MemoryCacheState> bedrockCache;
    std::shared_ptr<MemoryCacheState> servicesCache;
    std::shared_ptr<MemoryCacheState> playfabCache;
    std::shared_ptr<bedrock::LiveTokenManager> live;
    std::shared_ptr<bedrock::XboxTokenManager> xbox;
    std::shared_ptr<bedrock::MinecraftBedrockTokenManager> bedrockManager;
    std::shared_ptr<bedrock::MinecraftBedrockServicesTokenManager> services;
    std::shared_ptr<bedrock::PlayfabTokenManager> playfab;
    bedrock::JsRuntimeValue options;
    std::shared_ptr<bedrock::MicrosoftAuthFlow> auth;
    std::vector<double> delays;
    std::vector<std::string> info;
};

bool checkNormalXboxFlow() {
    bool ok = true;
    FlowFixture fixture;
    fixture.http->responses.push_back(response(
        200,
        R"({"Token":"USER","NotAfter":"2030-01-01T00:00:00Z"})"
    ));
    fixture.http->responses.push_back(response(
        200,
        R"({"Token":"DEVICE","NotAfter":"2030-01-01T00:00:00Z"})"
    ));
    fixture.http->responses.push_back(response(
        200,
        R"({"Token":"TITLE","NotAfter":"2030-01-01T00:00:00Z"})"
    ));
    fixture.http->responses.push_back(response(
        200,
        R"({"DisplayClaims":{"xui":[{"xid":"42","uhs":"HASH"}]},"Token":"XSTS","NotAfter":"2030-01-01T00:00:00Z"})"
    ));

    const auto result = fixture.auth->getXboxToken(
        bedrock::JsRuntimeValue::string(
            bedrock::MicrosoftAuthFlow::BedrockXstsRelyingParty
        )
    ).get();
    ok &= check(
        property(result, "XSTSToken").stringValue() == "XSTS" &&
            property(result, "userHash").stringValue() == "HASH" &&
            fixture.http->requests.size() == 4 &&
            fixture.delays.empty(),
        "normal Live -> user/device/title/XSTS orchestration failed"
    );
    const std::vector<std::string> expectedUrls {
        "https://user.auth.xboxlive.com/user/authenticate",
        "https://device.auth.xboxlive.com/device/authenticate",
        "https://title.auth.xboxlive.com/title/authenticate",
        "https://xsts.auth.xboxlive.com/xsts/authorize"
    };
    for (std::size_t index = 0;
         index < expectedUrls.size() && index < fixture.http->requests.size();
         ++index) {
        ok &= check(
            fixture.http->requests[index].url == expectedUrls[index],
            "normal Xbox request order differs from MicrosoftAuthFlow.js"
        );
    }
    const auto before = fixture.http->requests.size();
    const auto cached = fixture.auth->getXboxToken(
        bedrock::JsRuntimeValue::string(
            bedrock::MicrosoftAuthFlow::BedrockXstsRelyingParty
        )
    ).get();
    ok &= check(
        property(cached, "XSTSToken").stringValue() == "XSTS" &&
            fixture.http->requests.size() == before,
        "valid relying-party XSTS cache did not short-circuit"
    );
    return ok;
}

bool checkXboxRetry() {
    bool ok = true;
    FlowFixture fixture;
    fixture.http->responses.push_back(response(500, R"({"failure":true})"));
    fixture.http->responses.push_back(response(
        200,
        R"({"access_token":"MSA-2","refresh_token":"RT-2","expires_in":5000000})"
    ));
    fixture.http->responses.push_back(response(
        200,
        R"({"Token":"USER","NotAfter":"2030-01-01T00:00:00Z"})"
    ));
    fixture.http->responses.push_back(response(
        200,
        R"({"Token":"DEVICE","NotAfter":"2030-01-01T00:00:00Z"})"
    ));
    fixture.http->responses.push_back(response(
        200,
        R"({"Token":"TITLE","NotAfter":"2030-01-01T00:00:00Z"})"
    ));
    fixture.http->responses.push_back(response(
        200,
        R"({"DisplayClaims":{"xui":[{"xid":"42","uhs":"HASH"}]},"Token":"RETRIED","NotAfter":"2030-01-01T00:00:00Z"})"
    ));
    const auto result = fixture.auth->getXboxToken(
        bedrock::JsRuntimeValue::string(
            bedrock::MicrosoftAuthFlow::BedrockXstsRelyingParty
        )
    ).get();
    ok &= check(
        property(result, "XSTSToken").stringValue() == "RETRIED" &&
            fixture.delays == std::vector<double> {2000.0} &&
            fixture.live->forceRefresh.truthy() &&
            fixture.http->requests.size() == 6 &&
            fixture.http->requests[1].url ==
                bedrock::LiveTokenManager::LiveTokenRequest,
        "Xbox retry did not delay/force-refresh MSA/re-run once"
    );
    return ok;
}

bool checkSisuFlow() {
    bool ok = true;
    FlowFixture fixture("sisu");
    fixture.http->responses.push_back(response(
        200,
        R"({"Token":"DEVICE","NotAfter":"2030-01-01T00:00:00Z"})"
    ));
    fixture.http->responses.push_back(response(
        200,
        R"({"UserToken":{"Token":"USER","NotAfter":"2030-01-01T00:00:00Z"},"TitleToken":{"Token":"TITLE","NotAfter":"2030-01-01T00:00:00Z"},"AuthorizationToken":{"DisplayClaims":{"xui":[{"xid":"42","uhs":"HASH"}]},"Token":"SISU","NotAfter":"2030-01-01T00:00:00Z"}})"
    ));
    const auto result = fixture.auth->getXboxToken(
        bedrock::JsRuntimeValue::string(
            bedrock::MicrosoftAuthFlow::BedrockXstsRelyingParty
        )
    ).get();
    ok &= check(
        property(result, "XSTSToken").stringValue() == "SISU" &&
            fixture.http->requests.size() == 2 &&
            fixture.http->requests[0].url ==
                "https://device.auth.xboxlive.com/device/authenticate" &&
            fixture.http->requests[1].url ==
                bedrock::XboxTokenManager::SisuAuthorizeEndpoint,
        "Sisu flow did not bypass ordinary user/title/XSTS sequence"
    );
    return ok;
}

bool checkBedrockChainRetry() {
    bool ok = true;
    FlowFixture fixture;
    fixture.seedXsts(bedrock::MicrosoftAuthFlow::BedrockXstsRelyingParty);
    const auto noTitle = jwt(R"({"extraData":{}})");
    const auto withTitle = jwt(R"({"extraData":{"titleId":"APP"}})");
    fixture.http->responses.push_back(response(
        200,
        std::string("{\"chain\":[\"first\",\"") + noTitle + "\"]}"
    ));
    fixture.http->responses.push_back(response(
        200,
        std::string("{\"chain\":[\"first\",\"") + withTitle + "\"]}"
    ));
    const auto chain = fixture.auth->getMinecraftBedrockToken(
        bedrock::JsRuntimeValue::string("PUBLIC")
    ).get();
    const auto* second = chain.get(1);
    ok &= check(
        chain.isArray() && chain.length() == 2 && second &&
            second->stringValue() == withTitle &&
            fixture.http->requests.size() == 2 &&
            fixture.http->requests[0].url ==
                bedrock::MinecraftBedrockTokenManager::
                    AuthenticationEndpoint &&
            fixture.delays == std::vector<double> {2000.0} &&
            fixture.xbox->forceRefresh.truthy() &&
            fixture.bedrockCache->partialCalls == 2,
        "Bedrock chain titleId validation/retry differs from JS"
    );

    FlowFixture missing;
    std::string errorMessage;
    try {
        (void) missing.auth->getMinecraftBedrockToken(
            bedrock::JsRuntimeValue::undefined()
        ).get();
    } catch (const std::exception& error) {
        errorMessage = error.what();
    }
    ok &= check(
        errorMessage ==
            "Need to specifiy a ECDH x509 URL encoded public key" &&
            missing.http->requests.empty(),
        "Bedrock public-key validation/error spelling differs from JS"
    );
    return ok;
}

bool checkPlayfabAndServices() {
    bool ok = true;
    FlowFixture fixture;
    fixture.seedXsts(bedrock::MicrosoftAuthFlow::PlayfabRelyingParty);
    fixture.playfabCache->value.set(
        "pfb",
        bedrock::JsRuntimeValue::object({
            {"SessionTicket", bedrock::JsRuntimeValue::string("CACHED")},
            {"EntityToken", bedrock::JsRuntimeValue::object({
                {
                    "TokenExpiration",
                    bedrock::JsRuntimeValue::string(
                        "2030-01-01T00:00:00Z"
                    )
                }
            })}
        })
    );
    fixture.http->responses.push_back(response(
        200,
        R"({"data":{"SessionTicket":"FRESH","EntityToken":{"TokenExpiration":"2030-01-01T00:00:00Z"}}})"
    ));
    const auto playfab = fixture.auth->getPlayfabLogin().get();
    ok &= check(
        property(playfab, "SessionTicket").stringValue() == "FRESH" &&
            fixture.http->requests.size() == 1 &&
            fixture.http->requests[0].url ==
                bedrock::PlayfabTokenManager::LoginWithXboxEndpoint,
        "PlayFab's intentionally un-awaited cache lookup was not preserved"
    );

    fixture.servicesCache->value.set(
        "mcs",
        bedrock::JsRuntimeValue::object({
            {"mcToken", bedrock::JsRuntimeValue::string("CACHED-MCS")},
            {
                "validUntil",
                bedrock::JsRuntimeValue::string("2030-01-01T00:00:00Z")
            }
        })
    );
    const auto beforeCachedMcs = fixture.http->requests.size();
    const auto cachedMcs = fixture.auth->getMinecraftBedrockServicesToken(
        bedrock::JsRuntimeValue::object({
            {"verison", bedrock::JsRuntimeValue::string("9.9")}
        })
    ).get();
    ok &= check(
        property(cachedMcs, "mcToken").stringValue() == "CACHED-MCS" &&
            fixture.http->requests.size() == beforeCachedMcs,
        "valid Bedrock Services cache did not short-circuit"
    );

    fixture.servicesCache->value = bedrock::JsRuntimeValue::object();
    fixture.http->responses.push_back(response(
        200,
        R"({"data":{"SessionTicket":"PF-SESSION","EntityToken":{"TokenExpiration":"2030-01-01T00:00:00Z"}}})"
    ));
    fixture.http->responses.push_back(response(
        200,
        R"({"result":{"authorizationHeader":"MCS","validUntil":"2030-01-01T00:00:00Z","treatments":[],"configurations":{},"treatmentContext":"CTX"}})"
    ));
    const auto mcs = fixture.auth->getMinecraftBedrockServicesToken(
        bedrock::JsRuntimeValue::object({
            {"verison", bedrock::JsRuntimeValue::string("9.9")}
        })
    ).get();
    ok &= check(
        property(mcs, "mcToken").stringValue() == "MCS" &&
            fixture.http->requests.size() == beforeCachedMcs + 2 &&
            fixture.http->requests.back().body.find(
                R"("gameVersion":"1.20.62")"
            ) != std::string::npos &&
            fixture.http->requests.back().body.find("9.9") ==
                std::string::npos,
        "Bedrock Services `verison` typo/default-version behavior diverged"
    );
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= checkNormalXboxFlow();
    ok &= checkXboxRetry();
    ok &= checkSisuFlow();
    ok &= checkBedrockChainRetry();
    ok &= checkPlayfabAndServices();
    if (!ok) return 1;
    std::cout << "Microsoft auth flow smoke checks passed\n";
    return 0;
}
