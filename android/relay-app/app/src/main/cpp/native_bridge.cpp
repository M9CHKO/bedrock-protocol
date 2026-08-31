#include <bedrock/RakNetPing.hpp>
#include <bedrock/auth/JsRuntimeValue.hpp>
#include <bedrock/auth/XboxTokenManager.hpp>
#include <bedrock/bedrock.hpp>

#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr char LogTag[] = "CpeRelayNative";

JavaVM* javaVm = nullptr;
jclass nativeBridgeClass = nullptr;
jmethodID httpFetchMethod = nullptr;
std::mutex javaBridgeMutex;

std::string jsonString(const bedrock::JsRuntimeValue& value) {
    return bedrock::JsRuntimeJson::stringify(value).value_or("null");
}

const bedrock::JsRuntimeValue* property(
    const bedrock::JsRuntimeValue& value,
    std::string_view name
) {
    return value.get(name);
}

std::string stringProperty(
    const bedrock::JsRuntimeValue& value,
    std::string_view name
) {
    const auto* found = property(value, name);
    return found && found->isString() ? found->stringValue() : std::string();
}

std::string redactJsonValue(std::string text, std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\"";
    std::size_t offset = 0;
    while ((offset = text.find(marker, offset)) != std::string::npos) {
        auto colon = text.find(':', offset + marker.size());
        if (colon == std::string::npos) break;
        auto quote = text.find('"', colon + 1);
        if (quote == std::string::npos) break;
        auto end = quote + 1;
        bool escaped = false;
        for (; end < text.size(); ++end) {
            const char byte = text[end];
            if (byte == '"' && !escaped) break;
            if (byte == '\\' && !escaped) {
                escaped = true;
            } else {
                escaped = false;
            }
        }
        if (end >= text.size()) break;
        text.replace(quote + 1, end - quote - 1, "<redacted>");
        offset = quote + 12;
    }
    return text;
}

std::string safeMessage(std::string message) {
    for (const std::string_view key : {
            "access_token", "refresh_token", "identityToken",
            "Token", "token", "content_key"
        }) {
        message = redactJsonValue(std::move(message), key);
    }
    constexpr std::size_t MaxUiErrorLength = 1200;
    if (message.size() > MaxUiErrorLength) {
        message.resize(MaxUiErrorLength);
        message += "…";
    }
    return message;
}

class AttachedEnvironment {
public:
    AttachedEnvironment() {
        if (!javaVm) {
            throw std::runtime_error("Android Java VM is unavailable");
        }
        const auto status = javaVm->GetEnv(
            reinterpret_cast<void**>(&environment_),
            JNI_VERSION_1_6
        );
        if (status == JNI_EDETACHED) {
            if (javaVm->AttachCurrentThread(&environment_, nullptr) != JNI_OK) {
                throw std::runtime_error("Failed to attach HTTP worker to Java VM");
            }
            attached_ = true;
        } else if (status != JNI_OK || !environment_) {
            throw std::runtime_error("Failed to obtain Android JNI environment");
        }
    }

    ~AttachedEnvironment() {
        if (attached_ && javaVm) {
            javaVm->DetachCurrentThread();
        }
    }

    JNIEnv* get() const noexcept { return environment_; }

private:
    JNIEnv* environment_ = nullptr;
    bool attached_ = false;
};

std::string javaThrowableText(JNIEnv* environment, jthrowable throwable) {
    if (!throwable) return "Android HTTP transport failed";
    jclass throwableClass = environment->FindClass("java/lang/Throwable");
    if (!throwableClass) {
        environment->ExceptionClear();
        return "Android HTTP transport threw an exception";
    }
    jmethodID toString = environment->GetMethodID(
        throwableClass,
        "toString",
        "()Ljava/lang/String;"
    );
    if (!toString) {
        environment->ExceptionClear();
        environment->DeleteLocalRef(throwableClass);
        return "Android HTTP transport threw an exception";
    }
    auto result = static_cast<jstring>(environment->CallObjectMethod(
        throwable,
        toString
    ));
    if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
        result = nullptr;
    }
    std::string text = "Android HTTP transport failed";
    if (result) {
        const char* bytes = environment->GetStringUTFChars(result, nullptr);
        if (bytes) {
            text = bytes;
            environment->ReleaseStringUTFChars(result, bytes);
        }
        environment->DeleteLocalRef(result);
    }
    environment->DeleteLocalRef(throwableClass);
    return safeMessage(std::move(text));
}

std::string callAndroidHttp(std::string requestJson) {
    AttachedEnvironment attached;
    auto* environment = attached.get();

    jclass bridge = nullptr;
    jmethodID method = nullptr;
    {
        std::lock_guard lock(javaBridgeMutex);
        bridge = nativeBridgeClass;
        method = httpFetchMethod;
    }
    if (!bridge || !method) {
        throw std::runtime_error("Android HTTP bridge is not initialized");
    }

    jstring request = environment->NewStringUTF(requestJson.c_str());
    if (!request) {
        throw std::runtime_error("Failed to allocate Android HTTP request");
    }
    auto response = static_cast<jstring>(environment->CallStaticObjectMethod(
        bridge,
        method,
        request
    ));
    environment->DeleteLocalRef(request);

    if (environment->ExceptionCheck()) {
        jthrowable throwable = environment->ExceptionOccurred();
        environment->ExceptionClear();
        auto message = javaThrowableText(environment, throwable);
        if (throwable) environment->DeleteLocalRef(throwable);
        throw std::runtime_error(std::move(message));
    }
    if (!response) {
        throw std::runtime_error("Android HTTP transport returned null");
    }

    const char* responseBytes = environment->GetStringUTFChars(
        response,
        nullptr
    );
    if (!responseBytes) {
        environment->DeleteLocalRef(response);
        throw std::runtime_error("Failed to read Android HTTP response");
    }
    std::string result(responseBytes);
    environment->ReleaseStringUTFChars(response, responseBytes);
    environment->DeleteLocalRef(response);
    return result;
}

class AndroidXboxTokenHttpClient final
    : public bedrock::IXboxTokenHttpClient {
public:
    explicit AndroidXboxTokenHttpClient(
        std::shared_ptr<bedrock::JsMicrotaskQueue> queue
    ) : queue_(std::move(queue)) {}

    bedrock::JsPromise<bedrock::XboxTokenHttpResponse> fetch(
        bedrock::XboxTokenHttpRequest request
    ) override {
        auto queue = queue_;
        return bedrock::JsPromise<bedrock::XboxTokenHttpResponse>::
            fromSynchronous(queue, [request = std::move(request)]() mutable {
                auto headerArray = bedrock::JsRuntimeValue::array();
                for (auto& [name, value] : request.headers) {
                    headerArray.push(bedrock::JsRuntimeValue::array({
                        bedrock::JsRuntimeValue::string(std::move(name)),
                        bedrock::JsRuntimeValue::string(std::move(value))
                    }));
                }
                auto wireRequest = bedrock::JsRuntimeValue::object({
                    {"method", bedrock::JsRuntimeValue::string(
                        std::move(request.method)
                    )},
                    {"url", bedrock::JsRuntimeValue::string(
                        std::move(request.url)
                    )},
                    {"headers", std::move(headerArray)},
                    {"body", bedrock::JsRuntimeValue::string(
                        std::move(request.body)
                    )}
                });

                auto parsed = bedrock::JsRuntimeJson::parse(
                    callAndroidHttp(jsonString(wireRequest))
                );
                if (!parsed.isObject()) {
                    throw std::runtime_error(
                        "Android HTTP response is not an object"
                    );
                }

                const auto* status = property(parsed, "status");
                if (!status || !status->isNumber()) {
                    throw std::runtime_error(
                        "Android HTTP response has no status"
                    );
                }
                bedrock::XboxTokenHttpResponse response;
                response.status = static_cast<int>(status->numberValue());
                response.statusText = stringProperty(parsed, "statusText");
                response.bodyText = stringProperty(parsed, "bodyText");

                const auto* headers = property(parsed, "headers");
                if (headers && headers->isArray()) {
                    for (std::size_t index = 0;
                         index < headers->length(); ++index) {
                        const auto* pair = headers->get(index);
                        if (!pair || !pair->isArray() || pair->length() < 2) {
                            continue;
                        }
                        const auto* name = pair->get(0);
                        const auto* value = pair->get(1);
                        if (name && value && name->isString() &&
                            value->isString()) {
                            response.headers.emplace_back(
                                name->stringValue(),
                                value->stringValue()
                            );
                        }
                    }
                }
                return response;
            });
    }

private:
    std::shared_ptr<bedrock::JsMicrotaskQueue> queue_;
};

struct RelayState {
    mutable std::mutex mutex;
    bool running = false;
    bool listening = false;
    bool pingDone = false;
    bool pingOk = false;
    bool upstreamReady = false;
    uint16_t boundPort = 0;
    std::size_t downstreamConnections = 0;
    std::size_t upstreamStartedCount = 0;
    std::size_t upstreamReadyCount = 0;
    std::string destinationHost;
    uint16_t destinationPort = 19132;
    std::string lastError;
    std::deque<bedrock::JsRuntimeValue> events;

    void push(bedrock::JsRuntimeValue event) {
        std::lock_guard lock(mutex);
        constexpr std::size_t MaximumEvents = 256;
        if (events.size() == MaximumEvents) events.pop_front();
        events.push_back(std::move(event));
    }

    void push(std::string type, std::string message) {
        push(bedrock::JsRuntimeValue::object({
            {"type", bedrock::JsRuntimeValue::string(std::move(type))},
            {"message", bedrock::JsRuntimeValue::string(
                safeMessage(std::move(message))
            )}
        }));
    }
};

bedrock::JsRuntimeValue snapshotValue(
    const std::shared_ptr<RelayState>& state,
    std::optional<bool> ok = std::nullopt
) {
    std::lock_guard lock(state->mutex);
    auto result = bedrock::JsRuntimeValue::object({
        {"running", bedrock::JsRuntimeValue::boolean(state->running)},
        {"listening", bedrock::JsRuntimeValue::boolean(state->listening)},
        {"pingDone", bedrock::JsRuntimeValue::boolean(state->pingDone)},
        {"pingOk", bedrock::JsRuntimeValue::boolean(state->pingOk)},
        {"upstreamReady", bedrock::JsRuntimeValue::boolean(
            state->upstreamReady
        )},
        {"boundPort", bedrock::JsRuntimeValue::number(state->boundPort)},
        {"downstreamConnections", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->downstreamConnections)
        )},
        {"upstreamStartedCount", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->upstreamStartedCount)
        )},
        {"upstreamReadyCount", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->upstreamReadyCount)
        )},
        {"destinationHost", bedrock::JsRuntimeValue::string(
            state->destinationHost
        )},
        {"destinationPort", bedrock::JsRuntimeValue::number(
            state->destinationPort
        )},
        {"error", bedrock::JsRuntimeValue::string(state->lastError)}
    });
    if (ok.has_value()) {
        result.set("ok", bedrock::JsRuntimeValue::boolean(*ok));
    }
    return result;
}

class RelayController {
public:
    explicit RelayController(std::shared_ptr<RelayState> state)
        : state_(std::move(state)) {}

    ~RelayController() { stop(); }

    void start(
        const std::string& destinationHost,
        uint16_t destinationPort,
        const std::filesystem::path& cacheDirectory
    ) {
        bedrock::RelayOptions options;
        options.version = "1.21.100";
        options.host = "0.0.0.0";
        options.port = 19132;
        options.motd = "CPE Relay Android";
        options.maxPlayers = 1;
        options.offline = true;
        options.forceSingle = true;
        options.logging = false;
        options.enableChunkCaching = false;
        options.destination.host = destinationHost;
        options.destination.port = destinationPort;
        options.destination.offline = false;
        options.profilesFolder = cacheDirectory;
        // The autonomous Android application supports the standard device-code
        // flow and never falls back to the process-based MSAL/curl paths.
        options.flow = "live";
        options.advanced.httpClientFactory = [](auto queue) {
            return std::make_shared<AndroidXboxTokenHttpClient>(
                std::move(queue)
            );
        };

        auto state = state_;
        options.onMsaCode = [state](const bedrock::XboxDeviceCodeInfo& code) {
            state->push(bedrock::JsRuntimeValue::object({
                {"type", bedrock::JsRuntimeValue::string("msa_code")},
                {"verificationUri", bedrock::JsRuntimeValue::string(
                    code.verificationUri
                )},
                {"userCode", bedrock::JsRuntimeValue::string(code.userCode)},
                {"message", bedrock::JsRuntimeValue::string(code.message)}
            }));
        };

        auto relay = std::make_unique<bedrock::Relay>(std::move(options));
        relay->onConnect([state](bedrock::RelayPlayer& player) {
            state->push("connect", player.sessionId());
        });
        relay->onJoin([state](
            bedrock::RelayPlayer&,
            bedrock::BedrockNetworkClient&
        ) {
            state->push("upstream_ready", "Destination session is ready");
        });
        relay->onDisconnect([state](bedrock::RelayPlayer& player) {
            state->push("disconnect", player.sessionId());
        });
        relay->onError([state](const std::string& message) {
            {
                std::lock_guard lock(state->mutex);
                state->lastError = safeMessage(message);
            }
            state->push("error", message);
        });
        relay->onStatus([state](const bedrock::BedrockLiveRelayStatus& status) {
            bool becameReady = false;
            {
                std::lock_guard lock(state->mutex);
                becameReady = status.upstreamReady && !state->upstreamReady;
                state->listening = status.listening;
                state->boundPort = status.boundPort;
                state->downstreamConnections = status.downstreamConnections;
                state->upstreamStartedCount = status.upstreamStartedCount;
                state->upstreamReadyCount = status.upstreamReadyCount;
                state->upstreamReady = status.upstreamReady;
            }
            if (becameReady) {
                state->push("upstream_ready", "Destination session is ready");
            }
        });

        const auto listener = relay->listen();
        {
            std::lock_guard lock(state_->mutex);
            state_->running = true;
            state_->listening = true;
            state_->boundPort = listener.port;
        }
        state_->push(
            "listening",
            listener.host + ":" + std::to_string(listener.port)
        );

        {
            std::lock_guard lock(relayMutex_);
            relay_ = std::move(relay);
        }

        const auto pong = bedrock::RakNetPinger::ping(
            "127.0.0.1",
            listener.port,
            1200
        );
        {
            std::lock_guard lock(state_->mutex);
            state_->pingDone = true;
            state_->pingOk = pong.ok;
        }
        state_->push(
            pong.ok ? "ping_ok" : "ping_warning",
            pong.ok ? "Local RakNet pong verified" : safeMessage(pong.error)
        );
    }

    void stop() {
        if (stopped_.exchange(true)) return;
        std::unique_ptr<bedrock::Relay> relay;
        {
            std::lock_guard lock(relayMutex_);
            relay = std::move(relay_);
        }
        if (relay) {
            try {
                relay->close("Android relay stopped");
            } catch (...) {
            }
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->running = false;
            state_->listening = false;
            state_->upstreamReady = false;
            state_->downstreamConnections = 0;
            state_->upstreamStartedCount = 0;
            state_->upstreamReadyCount = 0;
        }
        state_->push("stopped", "Relay stopped");
    }

private:
    std::shared_ptr<RelayState> state_;
    std::mutex relayMutex_;
    std::unique_ptr<bedrock::Relay> relay_;
    std::atomic<bool> stopped_ {false};
};

std::mutex controllerMutex;
std::shared_ptr<RelayController> controller;
std::shared_ptr<RelayState> currentState = std::make_shared<RelayState>();

void initializeJavaBridge(JNIEnv* environment, jclass bridgeClass) {
    std::lock_guard lock(javaBridgeMutex);
    if (!nativeBridgeClass) {
        nativeBridgeClass = static_cast<jclass>(
            environment->NewGlobalRef(bridgeClass)
        );
        if (!nativeBridgeClass) {
            throw std::runtime_error("Failed to retain NativeBridge class");
        }
    }
    if (!httpFetchMethod) {
        httpFetchMethod = environment->GetStaticMethodID(
            nativeBridgeClass,
            "httpFetch",
            "(Ljava/lang/String;)Ljava/lang/String;"
        );
        if (!httpFetchMethod || environment->ExceptionCheck()) {
            environment->ExceptionClear();
            throw std::runtime_error("NativeBridge.httpFetch is unavailable");
        }
    }
}

std::string fromJavaString(JNIEnv* environment, jstring value) {
    if (!value) return {};
    const char* bytes = environment->GetStringUTFChars(value, nullptr);
    if (!bytes) throw std::runtime_error("Failed to read Java string");
    std::string result(bytes);
    environment->ReleaseStringUTFChars(value, bytes);
    return result;
}

jstring toJavaString(JNIEnv* environment, const std::string& value) {
    return environment->NewStringUTF(value.c_str());
}

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    javaVm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_startRelay(
    JNIEnv* environment,
    jclass bridgeClass,
    jstring destinationHostValue,
    jint destinationPortValue,
    jstring cacheDirectoryValue
) {
    auto state = std::make_shared<RelayState>();
    try {
        initializeJavaBridge(environment, bridgeClass);
        const auto destinationHost = fromJavaString(
            environment,
            destinationHostValue
        );
        const auto cacheDirectory = fromJavaString(
            environment,
            cacheDirectoryValue
        );
        if (destinationHost.empty()) {
            throw std::runtime_error("Destination host is empty");
        }
        if (destinationPortValue < 1 || destinationPortValue > 65535) {
            throw std::runtime_error("Destination port is out of range");
        }
        if (cacheDirectory.empty()) {
            throw std::runtime_error("Authentication cache path is empty");
        }

        state->destinationHost = destinationHost;
        state->destinationPort = static_cast<uint16_t>(destinationPortValue);
        state->running = true;

        std::shared_ptr<RelayController> previous;
        {
            std::lock_guard lock(controllerMutex);
            previous = std::move(controller);
            currentState = state;
        }
        if (previous) previous->stop();

        auto next = std::make_shared<RelayController>(state);
        next->start(
            destinationHost,
            static_cast<uint16_t>(destinationPortValue),
            std::filesystem::path(cacheDirectory)
        );
        {
            std::lock_guard lock(controllerMutex);
            controller = std::move(next);
        }
        return toJavaString(
            environment,
            jsonString(snapshotValue(state, true))
        );
    } catch (const std::exception& error) {
        const auto message = safeMessage(error.what());
        {
            std::lock_guard lock(state->mutex);
            state->running = false;
            state->listening = false;
            state->lastError = message;
        }
        state->push("error", message);
        auto result = snapshotValue(state, false);
        result.set("error", bedrock::JsRuntimeValue::string(message));
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "%s", message.c_str());
        return toJavaString(environment, jsonString(result));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_stopRelay(
    JNIEnv*,
    jclass
) {
    std::shared_ptr<RelayController> previous;
    {
        std::lock_guard lock(controllerMutex);
        previous = std::move(controller);
    }
    if (previous) previous->stop();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_snapshot(
    JNIEnv* environment,
    jclass
) {
    std::shared_ptr<RelayState> state;
    {
        std::lock_guard lock(controllerMutex);
        state = currentState;
    }
    return toJavaString(environment, jsonString(snapshotValue(state)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_pollEvents(
    JNIEnv* environment,
    jclass
) {
    std::shared_ptr<RelayState> state;
    {
        std::lock_guard lock(controllerMutex);
        state = currentState;
    }
    std::vector<bedrock::JsRuntimeValue> events;
    {
        std::lock_guard lock(state->mutex);
        events.reserve(state->events.size());
        while (!state->events.empty()) {
            events.push_back(std::move(state->events.front()));
            state->events.pop_front();
        }
    }
    return toJavaString(
        environment,
        jsonString(bedrock::JsRuntimeValue::array(std::move(events)))
    );
}
