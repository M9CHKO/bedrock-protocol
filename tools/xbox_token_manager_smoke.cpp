#include <bedrock/auth/XboxTokenManager.hpp>

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
    if (!condition) std::cerr << "[XBOX-TOKEN] " << message << "\n";
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
    int partialCalls = 0;
};

bedrock::AuthCachePtr makeMemoryCache(
    const std::shared_ptr<MemoryCacheState>& state
) {
    return std::make_shared<bedrock::AuthCache>(
        [state] {
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
                "unexpected XboxTokenManager fetch"
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

class ScriptedReplayHttpClient final : public bedrock::IMsalHttpClient {
public:
    explicit ScriptedReplayHttpClient(
        std::shared_ptr<bedrock::JsMicrotaskQueue> queue
    ) : queue_(std::move(queue)) {}

    bedrock::JsPromise<bedrock::MsalHttpResponse> send(
        bedrock::MsalHttpRequest request
    ) override {
        requests.push_back(std::move(request));
        if (responses.empty()) {
            return bedrock::JsPromise<bedrock::MsalHttpResponse>::rejected(
                queue_,
                "unexpected XboxReplay fetch"
            );
        }
        auto next = std::move(responses.front());
        responses.pop_front();
        return bedrock::JsPromise<bedrock::MsalHttpResponse>::resolved(
            queue_,
            std::move(next)
        );
    }

    std::deque<bedrock::MsalHttpResponse> responses;
    std::vector<bedrock::MsalHttpRequest> requests;

private:
    std::shared_ptr<bedrock::JsMicrotaskQueue> queue_;
};

bedrock::XboxTokenHttpResponse response(
    int status,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers = {}
) {
    bedrock::XboxTokenHttpResponse result;
    result.status = status;
    result.statusText = status == 200 ? "OK" : "Unauthorized";
    result.bodyText = std::move(body);
    result.headers = std::move(headers);
    return result;
}

bedrock::MsalHttpResponse replayResponse(
    int status,
    std::string body,
    std::string url = {},
    std::vector<std::string> cookies = {}
) {
    bedrock::MsalHttpResponse result;
    result.status = status;
    result.url = std::move(url);
    result.bodyText = std::move(body);
    if (!cookies.empty()) {
        std::vector<bedrock::JsRuntimeValue> values;
        for (auto& cookie : cookies) {
            values.push_back(bedrock::JsRuntimeValue::string(
                std::move(cookie)
            ));
        }
        result.headersObject.set(
            "set-cookie",
            bedrock::JsRuntimeValue::array(std::move(values))
        );
    }
    return result;
}

bedrock::XboxProofKey fixedProofKey() {
    constexpr std::string_view privatePem =
        "-----BEGIN PRIVATE KEY-----\n"
        "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgAAAAAAAAAAAAAAAA\n"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAGhRANCAARrF9Hy4SxCR/i85uVjpEDydwN9gS3r\n"
        "M6D0oTlF2JjClk/jQuL+Gn+bjufrSnwPnhYrzjNXazFezsu2QGg3v1H1\n"
        "-----END PRIVATE KEY-----\n";
    return bedrock::XboxProofKey::fromPrivateKeyPem(privatePem);
}

bool checkSisuSuccess() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto cacheState = std::make_shared<MemoryCacheState>();
    auto http = std::make_shared<ScriptedHttpClient>(queue);
    http->responses.push_back(response(
        200,
        R"({"UserToken":{"Token":"USER","NotAfter":"2030-01-01T00:00:00Z"},"TitleToken":{"Token":"TITLE","NotAfter":"2030-01-01T00:00:00Z"},"AuthorizationToken":{"DisplayClaims":{"xui":[{"xid":"","uhs":"HASH"}]},"Token":"XSTS","NotAfter":"2030-01-01T00:00:00Z"}})"
    ));
    bedrock::XboxTokenManager manager(
        fixedProofKey(),
        makeMemoryCache(cacheState),
        bedrock::XboxTokenManagerDependencies {
            .httpClient = http,
            .microtaskQueue = queue,
            .dateNowMilliseconds = [] { return 1'700'000'000'123.0; }
        }
    );
    const auto options = bedrock::JsRuntimeValue::object({
        {"authTitle", bedrock::JsRuntimeValue::string("APP")},
        {
            "relyingParty",
            bedrock::JsRuntimeValue::string(
                "https://multiplayer.minecraft.net/"
            )
        }
    });
    const auto result = manager.doSisuAuth(
        bedrock::JsRuntimeValue::string("MSA"),
        bedrock::JsRuntimeValue::string("DEVICE"),
        options
    ).get();
    ok &= check(
        property(result, "userXUID").isNull() &&
            property(result, "userHash").stringValue() == "HASH" &&
            property(result, "XSTSToken").stringValue() == "XSTS" &&
            property(result, "expiresOn").stringValue() ==
                "2030-01-01T00:00:00Z",
        "Sisu AuthorizationToken projection differs from JS"
    );
    ok &= check(
        http->requests.size() == 1,
        "Sisu did not issue exactly one request"
    );
    if (!http->requests.empty()) {
        const auto& request = http->requests.front();
        const auto signature = request.header("Signature");
        ok &= check(
            request.method == "post" &&
                request.url ==
                    bedrock::XboxTokenManager::SisuAuthorizeEndpoint &&
                request.headers.size() == 1 && signature &&
                !signature->empty() &&
                request.body ==
                    R"({"AccessToken":"t=MSA","AppId":"APP","DeviceToken":"DEVICE","Sandbox":"RETAIL","UseModernGamertag":true,"SiteName":"user.auth.xboxlive.com","RelyingParty":"https://multiplayer.minecraft.net/","ProofKey":{"kty":"EC","x":"axfR8uEsQkf4vOblY6RA8ncDfYEt6zOg9KE5RdiYwpY","y":"T-NC4v4af5uO5-tKfA-eFivOM1drMV7Oy7ZAaDe_UfU","crv":"P-256","alg":"ES256","use":"sig"}})",
            "Sisu fetch init/body differs from XboxTokenManager.js"
        );
    }

    const auto cacheKey = bedrock::XboxTokenManager::relyingPartyCacheKey(
        property(options, "relyingParty")
    );
    const auto cachedXsts = property(cacheState->value, cacheKey);
    ok &= check(
        cacheState->partialCalls == 1 &&
            property(property(cacheState->value, "userToken"), "Token")
                .stringValue() == "USER" &&
            property(property(cacheState->value, "titleToken"), "Token")
                .stringValue() == "TITLE" &&
            property(cachedXsts, "XSTSToken").stringValue() == "XSTS",
        "Sisu user/title/dynamic-XSTS cache update differs from JS"
    );
    return ok;
}

bool checkSisuError() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto cacheState = std::make_shared<MemoryCacheState>();
    auto http = std::make_shared<ScriptedHttpClient>(queue);
    http->responses.push_back(response(
        401,
        R"({"Identity":"0"})",
        {{"X-Err", "  +2148916233trailing"}}
    ));
    bedrock::XboxTokenManager manager(
        fixedProofKey(),
        makeMemoryCache(cacheState),
        bedrock::XboxTokenManagerDependencies {
            .httpClient = http,
            .microtaskQueue = queue,
            .dateNowMilliseconds = [] { return 1'700'000'000'123.0; }
        }
    );
    std::string errorMessage;
    try {
        (void) manager.doSisuAuth(
            bedrock::JsRuntimeValue::string("MSA"),
            bedrock::JsRuntimeValue::string("DEVICE"),
            bedrock::JsRuntimeValue::object({
                {
                    "relyingParty",
                    bedrock::JsRuntimeValue::string("rp")
                }
            })
        ).get();
    } catch (const std::exception& error) {
        errorMessage = error.what();
    }
    ok &= check(
        errorMessage ==
            "Your account currently does not have an Xbox profile. Please "
            "create one at https://signup.live.com/signup" &&
            cacheState->partialCalls == 0,
        "Sisu x-err parseInt/checkTokenError ordering differs from JS"
    );
    return ok;
}

bool checkReplaySuccess() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto cacheState = std::make_shared<MemoryCacheState>();
    auto xboxHttp = std::make_shared<ScriptedHttpClient>(queue);
    auto replayHttp = std::make_shared<ScriptedReplayHttpClient>(queue);
    replayHttp->responses.push_back(replayResponse(
        200,
        R"(<script>sFTTag:'ignored value="P+P&FT"/>';</script><script>urlPost:'https://login.live.com/post.srf'</script>)",
        "https://login.live.com/oauth20_authorize.srf",
        {"A=1; Path=/", "B=two; Secure"}
    ));
    replayHttp->responses.push_back(replayResponse(
        200,
        "",
        "https://login.live.com/oauth20_desktop.srf#"
        "access_token=RPS%2BTICKET&token_type=bearer&expires_in=3600&"
        "scope=x"
    ));
    replayHttp->responses.push_back(replayResponse(
        200,
        R"({"Token":"USER-TOKEN","NotAfter":"2030-01-01T00:00:00Z","DisplayClaims":{"xui":[{"uhs":"HASH"}]}})"
    ));
    xboxHttp->responses.push_back(response(
        200,
        R"({"DisplayClaims":{"xui":[{"xid":"42","uhs":"HASH"}]},"Token":"XSTS","NotAfter":"2030-01-01T00:00:00Z"})"
    ));
    bedrock::XboxTokenManager manager(
        fixedProofKey(),
        makeMemoryCache(cacheState),
        bedrock::XboxTokenManagerDependencies {
            .httpClient = xboxHttp,
            .microtaskQueue = queue,
            .dateNowMilliseconds = [] { return 1'700'000'000'123.0; },
            .replayHttpClient = replayHttp
        }
    );
    const auto options = bedrock::JsRuntimeValue::object({
        {
            "relyingParty",
            bedrock::JsRuntimeValue::string(
                "https://multiplayer.minecraft.net/"
            )
        }
    });
    const auto result = manager.doReplayAuth(
        bedrock::JsRuntimeValue::string("user+tag@example.com"),
        bedrock::JsRuntimeValue::string("p a&ss"),
        options
    ).get();
    ok &= check(
        property(result, "XSTSToken").stringValue() == "XSTS" &&
            replayHttp->requests.size() == 3 &&
            xboxHttp->requests.size() == 1 &&
            cacheState->partialCalls == 2,
        "password replay pipeline did not reach/cache signed XSTS"
    );
    if (replayHttp->requests.size() == 3) {
        const auto& preAuth = replayHttp->requests[0];
        const auto& login = replayHttp->requests[1];
        const auto& exchange = replayHttp->requests[2];
        ok &= check(
            preAuth.method == "GET" && preAuth.decompress &&
                preAuth.maxRedirects == 0 &&
                preAuth.url ==
                    "https://login.live.com/oauth20_authorize.srf?"
                    "client_id=000000004C12AE6F&"
                    "redirect_uri=https%3A%2F%2Flogin.live.com%2F"
                    "oauth20_desktop.srf&scope=service%3A%3Auser.auth."
                    "xboxlive.com%3A%3AMBI_SSL&display=touch&"
                    "response_type=token&locale=en",
            "XboxReplay preAuth URL/fetch options differ from the package"
        );
        ok &= check(
            login.method == "POST" && login.maxRedirects == 1 &&
                login.url == "https://login.live.com/post.srf" &&
                login.body ==
                    "login=user%2Btag%40example.com&"
                    "loginfmt=user%2Btag%40example.com&"
                    "passwd=p%20a%26ss&PPFT=P%2BP%26FT" &&
                login.header("Cookie") &&
                *login.header("Cookie") == "A=1; B=two",
            "XboxReplay credential form/cookie/redirect init differs"
        );
        ok &= check(
            exchange.method == "POST" &&
                exchange.url ==
                    "https://user.auth.xboxlive.com/user/authenticate" &&
                exchange.header("x-xbl-contract-version") &&
                *exchange.header("x-xbl-contract-version") == "0" &&
                exchange.body ==
                    R"({"RelyingParty":"http://auth.xboxlive.com","TokenType":"JWT","Properties":{"AuthMethod":"RPS","SiteName":"user.auth.xboxlive.com","RpsTicket":"RPS+TICKET"}})",
            "XboxReplay RPS user-token exchange differs"
        );
    }
    const auto cachedUser = property(cacheState->value, "userToken");
    const auto xstsKey = bedrock::XboxTokenManager::relyingPartyCacheKey(
        property(options, "relyingParty")
    );
    ok &= check(
        property(cachedUser, "Token").stringValue() == "USER-TOKEN" &&
            property(property(cacheState->value, xstsKey), "XSTSToken")
                .stringValue() == "XSTS",
        "Replay user/XSTS cache entries differ"
    );
    return ok;
}

bool checkReplayError() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto cacheState = std::make_shared<MemoryCacheState>();
    auto xboxHttp = std::make_shared<ScriptedHttpClient>(queue);
    auto replayHttp = std::make_shared<ScriptedReplayHttpClient>(queue);
    replayHttp->responses.push_back(replayResponse(
        200,
        "<html>changed login page</html>"
    ));
    bedrock::XboxTokenManager manager(
        fixedProofKey(),
        makeMemoryCache(cacheState),
        bedrock::XboxTokenManagerDependencies {
            .httpClient = xboxHttp,
            .microtaskQueue = queue,
            .dateNowMilliseconds = [] { return 1'700'000'000'123.0; },
            .replayHttpClient = replayHttp
        }
    );
    std::string errorMessage;
    try {
        (void) manager.doReplayAuth(
            bedrock::JsRuntimeValue::string("email"),
            bedrock::JsRuntimeValue::string("password")
        ).get();
    } catch (const std::exception& error) {
        errorMessage = error.what();
    }
    ok &= check(
        errorMessage ==
            "Could not match \"PPFT\" parameter, please fill an issue on "
            "https://bit.ly/xr-xbl-auth-create-issue" &&
            replayHttp->requests.size() == 1 &&
            xboxHttp->requests.empty() &&
            cacheState->partialCalls == 0,
        "XboxReplay preAuth parse failure/error precedence differs"
    );
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= checkSisuSuccess();
    ok &= checkSisuError();
    ok &= checkReplaySuccess();
    ok &= checkReplayError();
    if (!ok) return 1;
    std::cout << "Xbox token manager smoke checks passed\n";
    return 0;
}
