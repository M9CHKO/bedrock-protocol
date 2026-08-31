#include <bedrock/auth/NativeBedrockAuthflow.hpp>

#include <bedrock/auth/BedrockAuthJwt.hpp>
#include <bedrock/auth/FileAuthCache.hpp>

#include <cstdlib>
#include <future>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace bedrock {

namespace {

std::filesystem::path prismarineAuthSourceDirectory() {
    const auto findFrom = [](std::filesystem::path cursor)
        -> std::optional<std::filesystem::path> {
        std::error_code error;
        if (!std::filesystem::is_directory(cursor, error)) {
            cursor = cursor.parent_path();
        }
        for (int depth = 0; depth < 12 && !cursor.empty(); ++depth) {
            auto candidate = cursor / "node_modules" /
                "prismarine-auth" / "src";
            error.clear();
            if (std::filesystem::is_directory(candidate, error) && !error) {
                return candidate.lexically_normal();
            }
            const auto parent = cursor.parent_path();
            if (parent == cursor) break;
            cursor = parent;
        }
        return std::nullopt;
    };

    std::error_code error;
    auto source = std::filesystem::absolute(
        std::filesystem::path(__FILE__),
        error
    );
    if (!error) {
        if (auto located = findFrom(std::move(source))) return *located;
    }
    error.clear();
    auto current = std::filesystem::current_path(error);
    if (!error) {
        if (auto located = findFrom(std::move(current))) return *located;
    }
    return std::filesystem::path("node_modules") /
        "prismarine-auth" / "src";
}

std::string stringProperty(
    const JsRuntimeValue& value,
    std::string_view name
) {
    const auto* property = value.get(name);
    return property && property->isString()
        ? property->stringValue()
        : std::string();
}

MinecraftBedrockTokenChains bedrockChainsFromRuntime(
    const JsRuntimeValue& value
) {
    if (!value.isArray()) {
        throw std::runtime_error(
            "getMinecraftBedrockToken did not return an array"
        );
    }
    MinecraftBedrockTokenChains chains;
    chains.reserve(value.length());
    for (std::size_t index = 0; index < value.length(); ++index) {
        const auto* token = value.get(index);
        if (!token || !token->isString()) {
            throw std::runtime_error(
                "getMinecraftBedrockToken returned a non-string chain"
            );
        }
        chains.push_back(token->stringValue());
    }
    return chains;
}

XboxToken xboxTokenFromRuntime(const JsRuntimeValue& value) {
    if (!value.isObject()) {
        throw std::runtime_error("getXboxToken did not return an object");
    }
    return XboxToken {
        .userXUID = stringProperty(value, "userXUID"),
        .userHash = stringProperty(value, "userHash"),
        .XSTSToken = stringProperty(value, "XSTSToken"),
        .expiresOn = stringProperty(value, "expiresOn")
    };
}

AuthflowPtr adaptMicrosoftAuthflow(
    const std::shared_ptr<MicrosoftAuthFlow>& flow
) {
    return std::make_shared<Authflow>(
        [flow](std::string publicKey) {
            return std::async(
                std::launch::async,
                [flow, publicKey = std::move(publicKey)]() mutable {
                    return bedrockChainsFromRuntime(
                        flow->getMinecraftBedrockToken(
                            JsRuntimeValue::string(std::move(publicKey))
                        ).get()
                    );
                }
            );
        },
        [flow](std::string relyingParty, bool forceRefresh) {
            return std::async(
                std::launch::async,
                [
                    flow,
                    relyingParty = std::move(relyingParty),
                    forceRefresh
                ]() mutable {
                    return xboxTokenFromRuntime(
                        flow->getXboxToken(
                            JsRuntimeValue::string(std::move(relyingParty)),
                            JsRuntimeValue::boolean(forceRefresh)
                        ).get()
                    );
                }
            );
        }
    );
}

} // namespace

std::filesystem::path defaultBedrockProfilesFolder() {
#if defined(_WIN32)
    std::filesystem::path root;
    if (const char* appData = std::getenv("APPDATA")) {
        if (*appData) root = appData;
    }
    if (root.empty()) {
        if (const char* userProfile = std::getenv("USERPROFILE")) {
            if (*userProfile) {
                root = std::filesystem::path(userProfile) /
                    "AppData" / "Roaming";
            }
        }
    }
    if (!root.empty()) return root / ".minecraft" / "nmp-cache";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        if (*home) {
            return std::filesystem::path(home) / "Library" /
                "Application Support" / "minecraft" / "nmp-cache";
        }
    }
#else
    if (const char* home = std::getenv("HOME")) {
        if (*home) {
            return std::filesystem::path(home) /
                ".minecraft" / "nmp-cache";
        }
    }
#endif
    return std::filesystem::current_path() /
        ".minecraft" / "nmp-cache";
}

void validateNativeBedrockAuthflowOptions(
    NativeBedrockAuthflowOptions& options
) {
    if (!options.profilesFolder.truthy()) {
        options.profilesFolder.setResolved(defaultBedrockProfilesFolder());
    }
    if (!options.authTitle.hasOwn() || options.authTitle.isUndefined()) {
        options.authTitle = std::string(Titles::MinecraftNintendoSwitch);
        options.deviceType = "Nintendo";
        options.flow = "live";
    }
}

void validateNativeBedrockAuthflowPresence(
    const NativeBedrockAuthflowOptions& options
) {
    XboxLiveAuth::validatePrismarineAuthFlowPresence(
        XboxLiveAuthFlowOptions {
            .authTitle = options.authTitle.value_or(""),
            .deviceType = options.deviceType,
            .flow = options.flow
        }
    );
}

std::filesystem::path initializeNativeBedrockAuthCacheRoot(
    const ProfilesFolderOption& profilesFolder
) {
    if (profilesFolder.isFactory()) return {};

    if (profilesFolder.isBoolean() && profilesFolder.booleanValue()) {
        return prismarineAuthSourceDirectory();
    }

    const auto selectedRoot = profilesFolder.path();
    std::error_code error;
    const bool exists = std::filesystem::exists(selectedRoot, error);
    if (error) return prismarineAuthSourceDirectory();
    if (!exists) {
        std::filesystem::create_directories(selectedRoot, error);
        if (error) return prismarineAuthSourceDirectory();
    }
    return selectedRoot;
}

NativeBedrockAuthflowRuntime createNativeBedrockAuthflow(
    const NativeBedrockAuthflowOptions& options,
    const std::filesystem::path& effectiveCacheRoot
) {
    const XboxLiveAuthFlowOptions authFlow {
        .authTitle = options.authTitle.value_or(""),
        .deviceType = options.deviceType,
        .flow = options.flow
    };

    if (authFlow.flow == "live" ||
        authFlow.flow == "sisu" ||
        authFlow.flow != "msal" ||
        !XboxLiveAuth::isTruthyMsalConfig(options.msalConfig)) {
        XboxLiveAuth::validatePrismarineAuthFlow(authFlow);
    }

    std::unordered_map<std::string, AuthCachePtr> caches;
    const auto initializeCache = [&](const std::string& name) {
        AuthCacheFactoryOptions cacheOptions {
            .cacheName = name,
            .username = options.username
        };
        if (options.profilesFolder.isFactory()) {
            const auto& factory = options.profilesFolder.factory();
            caches[name] = (*factory)(cacheOptions);
            return;
        }
        auto cache = makeFileAuthCache(
            effectiveCacheRoot,
            std::move(cacheOptions)
        );
        if (options.forceRefresh) (void) cache->reset();
        caches[name] = std::move(cache);
    };

    const std::string firstCache = authFlow.flow == "msal"
        ? "msal"
        : authFlow.flow;
    initializeCache(firstCache);
    auto managerQueue = JsMicrotaskQueue::create();
    XboxTokenHttpClientPtr managerHttpClient;
    if (options.httpClientFactory) {
        managerHttpClient = options.httpClientFactory(managerQueue);
        if (!managerHttpClient) {
            throw std::runtime_error(
                "httpClientFactory returned a null Xbox HTTP client"
            );
        }
    }
    auto authRuntime = XboxLiveAuth::initializePrismarineAuthFlowRuntime(
        authFlow,
        options.msalConfig,
        caches[firstCache],
        {},
        managerQueue,
        {},
        LiveTokenManagerDependencies {
            .httpClient = managerHttpClient,
            .microtaskQueue = managerQueue
        }
    );
    managerQueue = authRuntime.live
        ? authRuntime.live->microtaskQueue()
        : authRuntime.msa
            ? authRuntime.msa->microtaskQueue()
            : managerQueue;
    if (!managerHttpClient) {
        managerHttpClient =
            std::make_shared<CurlXboxTokenHttpClient>(managerQueue);
    }
    auto xboxProofKey = XboxProofKey::generate();

    initializeCache("xbl");
    auto xboxTokenManager = std::make_shared<XboxTokenManager>(
        xboxProofKey,
        caches["xbl"],
        XboxTokenManagerDependencies {
            .httpClient = managerHttpClient,
            .microtaskQueue = managerQueue
        }
    );
    initializeCache("bed");
    auto bedrockTokenManager =
        std::make_shared<MinecraftBedrockTokenManager>(
            caches["bed"],
            BedrockTokenManagerDependencies {
                .httpClient = managerHttpClient,
                .microtaskQueue = managerQueue
            }
        );
    // No JavaTokenManager and no `mca` cache: this factory is intentionally
    // limited to the Bedrock side of prismarine-auth.
    initializeCache("mcs");
    auto bedrockServicesTokenManager =
        std::make_shared<MinecraftBedrockServicesTokenManager>(
            caches["mcs"],
            BedrockTokenManagerDependencies {
                .httpClient = managerHttpClient,
                .microtaskQueue = managerQueue
            }
        );
    initializeCache("pfb");
    auto playfabTokenManager = std::make_shared<PlayfabTokenManager>(
        caches["pfb"],
        BedrockTokenManagerDependencies {
            .httpClient = managerHttpClient,
            .microtaskQueue = managerQueue
        }
    );

    auto flowRuntimeOptions = JsRuntimeValue::object({
        {"authTitle", JsRuntimeValue::string(authFlow.authTitle)},
        {"deviceType", JsRuntimeValue::string(authFlow.deviceType)},
        {"flow", JsRuntimeValue::string(authFlow.flow)}
    });
    if (!options.password.empty()) {
        flowRuntimeOptions.set(
            "password",
            JsRuntimeValue::string(options.password)
        );
    }

    MicrosoftAuthFlowDependencies flowDependencies {
        .microtaskQueue = managerQueue
    };
    if (options.onMsaCode) {
        const auto callback = options.onMsaCode;
        flowDependencies.observers.codeCallback = [callback](
            const JsRuntimeValue& response
        ) {
            const auto get = [&response](
                std::string_view first,
                std::string_view second = {}
            ) {
                const auto* value = response.get(first);
                if ((!value || !value->isString()) && !second.empty()) {
                    value = response.get(second);
                }
                return value && value->isString()
                    ? value->stringValue()
                    : std::string();
            };
            callback(XboxDeviceCodeInfo {
                .verificationUri = get(
                    "verificationUri",
                    "verification_uri"
                ),
                .userCode = get("userCode", "user_code"),
                .message = get("message")
            });
        };
    }

    auto microsoft = std::make_shared<MicrosoftAuthFlow>(
        JsRuntimeValue::string(options.username),
        std::move(flowRuntimeOptions),
        MicrosoftAuthFlowManagers {
            .live = authRuntime.live,
            .msa = authRuntime.msa,
            .xbox = xboxTokenManager,
            .bedrock = bedrockTokenManager,
            .bedrockServices = bedrockServicesTokenManager,
            .playfab = playfabTokenManager
        },
        authRuntime.doTitleAuth
            ? JsRuntimeValue::boolean(true)
            : JsRuntimeValue::undefined(),
        std::move(flowDependencies)
    );

    NativeBedrockAuthflowRuntime result;
    result.authflow = adaptMicrosoftAuthflow(microsoft);
    result.effectiveMsalConfig =
        std::move(authRuntime.effectiveMsalConfig);
    result.live = std::move(authRuntime.live);
    result.msa = std::move(authRuntime.msa);
    result.xbox = std::move(xboxTokenManager);
    result.bedrock = std::move(bedrockTokenManager);
    result.bedrockServices = std::move(bedrockServicesTokenManager);
    result.playfab = std::move(playfabTokenManager);
    result.microsoft = std::move(microsoft);
    result.xboxProofKey = std::move(xboxProofKey);
    result.caches = std::move(caches);
    return result;
}

} // namespace bedrock
