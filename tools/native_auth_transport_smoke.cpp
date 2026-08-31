#include <bedrock/auth/NativeBedrockAuthflow.hpp>
#include <bedrock/bedrock.hpp>

#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[NATIVE-AUTH-TRANSPORT] " << message << "\n";
    }
    return condition;
}

bedrock::AuthCachePtr makeMemoryCache() {
    auto value = std::make_shared<bedrock::JsRuntimeValue>(
        bedrock::JsRuntimeValue::object()
    );
    return std::make_shared<bedrock::AuthCache>(
        [value] {
            return bedrock::makeReadyAuthCacheFuture(*value);
        },
        [value](bedrock::AuthCacheValue update) {
            if (update.isObject()) {
                for (const auto& property : update.ownProperties()) {
                    value->set(property.key, property.value);
                }
            }
            return bedrock::makeReadyAuthCacheFuture();
        },
        [value] {
            *value = bedrock::JsRuntimeValue::object();
            return bedrock::makeReadyAuthCacheFuture(*value);
        },
        [value](bedrock::AuthCacheValue replacement) {
            *value = std::move(replacement);
            return bedrock::makeReadyAuthCacheFuture();
        }
    );
}

class QueueBoundHttpClient final : public bedrock::IXboxTokenHttpClient {
public:
    explicit QueueBoundHttpClient(
        std::shared_ptr<bedrock::JsMicrotaskQueue> queue
    ) : queue(std::move(queue)) {}

    bedrock::JsPromise<bedrock::XboxTokenHttpResponse> fetch(
        bedrock::XboxTokenHttpRequest
    ) override {
        ++fetchCalls;
        return bedrock::JsPromise<bedrock::XboxTokenHttpResponse>::rejected(
            queue,
            "network access was not expected in constructor smoke"
        );
    }

    std::shared_ptr<bedrock::JsMicrotaskQueue> queue;
    int fetchCalls = 0;
};

bool injectedTransportIsSharedByTheWholeGraph() {
    bool ok = true;
    int factoryCalls = 0;
    std::shared_ptr<QueueBoundHttpClient> http;

    bedrock::NativeBedrockAuthflowOptions options;
    options.username = "AndroidRelay";
    options.profilesFolder = bedrock::AuthCacheFactory(
        [](bedrock::AuthCacheFactoryOptions) {
            return makeMemoryCache();
        }
    );
    options.authTitle = std::string(
        bedrock::Titles::MinecraftNintendoSwitch
    );
    options.deviceType = "Nintendo";
    options.flow = "live";
    options.httpClientFactory = [&](auto queue) {
        ++factoryCalls;
        http = std::make_shared<QueueBoundHttpClient>(std::move(queue));
        return http;
    };

    auto runtime = bedrock::createNativeBedrockAuthflow(options, {});
    ok &= check(factoryCalls == 1, "transport factory was not called once");
    ok &= check(http != nullptr, "transport factory result was lost");
    ok &= check(
        runtime.live && runtime.live->microtaskQueue() == http->queue,
        "Live manager did not use the factory queue"
    );
    ok &= check(
        runtime.live && runtime.live->httpClient() == http,
        "Live manager replaced the injected transport"
    );
    ok &= check(
        runtime.xbox && runtime.xbox->httpClient() == http,
        "Xbox manager replaced the injected transport"
    );
    ok &= check(
        runtime.bedrock && runtime.bedrock->httpClient() == http,
        "Bedrock token manager replaced the injected transport"
    );
    ok &= check(
        runtime.bedrockServices &&
            runtime.bedrockServices->httpClient() == http,
        "Bedrock services manager replaced the injected transport"
    );
    ok &= check(
        runtime.playfab && runtime.playfab->httpClient() == http,
        "PlayFab manager replaced the injected transport"
    );
    ok &= check(http && http->fetchCalls == 0,
                "auth constructor unexpectedly performed network I/O");
    return ok;
}

bool relayFacadePropagatesFactory() {
    bedrock::RelayOptions options;
    options.version = "1.21.100";
    options.offline = true;
    options.destination.host = "cpe.ign.gg";
    options.destination.port = 19132;
    options.destination.offline = false;
    options.advanced.httpClientFactory = [](auto queue) {
        return std::make_shared<QueueBoundHttpClient>(std::move(queue));
    };

    bedrock::Relay relay(std::move(options));
    return check(
        static_cast<bool>(
            relay.live().options().upstream.httpClientFactory
        ),
        "RelayOptions dropped the platform HTTP transport factory"
    );
}

bool nullFactoryResultIsRejected() {
    bedrock::NativeBedrockAuthflowOptions options;
    options.username = "AndroidRelay";
    options.profilesFolder = bedrock::AuthCacheFactory(
        [](bedrock::AuthCacheFactoryOptions) {
            return makeMemoryCache();
        }
    );
    options.authTitle = std::string(
        bedrock::Titles::MinecraftNintendoSwitch
    );
    options.deviceType = "Nintendo";
    options.flow = "live";
    options.httpClientFactory = [](auto) {
        return bedrock::XboxTokenHttpClientPtr {};
    };

    try {
        (void) bedrock::createNativeBedrockAuthflow(options, {});
    } catch (const std::exception& error) {
        return check(
            std::string(error.what()).find("httpClientFactory") !=
                std::string::npos,
            "null factory result produced an unrelated error"
        );
    }
    return check(false, "null factory result was accepted");
}

} // namespace

int main() {
    bool ok = true;
    ok &= injectedTransportIsSharedByTheWholeGraph();
    ok &= relayFacadePropagatesFactory();
    ok &= nullFactoryResultIsRejected();
    if (ok) {
        std::cout << "native auth transport smoke passed\n";
        return 0;
    }
    return 1;
}
