#include <bedrock/RakNetPing.hpp>
#include <bedrock/auth/JsRuntimeValue.hpp>
#include <bedrock/auth/XboxTokenManager.hpp>
#include <bedrock/bedrock.hpp>

#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(BEDROCK_ANDROID_RELEASE_BUILD) && !defined(__OPTIMIZE__)
#error "The distributable Android relay must compile native code with optimization"
#endif

namespace {

constexpr char LogTag[] = "CpeRelayNative";

#if defined(BEDROCK_ANDROID_RELEASE_BUILD)
constexpr std::string_view NativeBuildType = "release";
#else
constexpr std::string_view NativeBuildType = "debug";
#endif

#if defined(__OPTIMIZE__)
constexpr bool NativeCompilerOptimized = true;
#else
constexpr bool NativeCompilerOptimized = false;
#endif

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
            "Token", "token", "content_key", "client_secret",
            "password", "device_code", "user_code", "Authorization",
            "Cookie"
        }) {
        message = redactJsonValue(std::move(message), key);
    }
    static const std::regex assignedSecret(
        R"(((access_token|refresh_token|identitytoken|token|content_key|client_secret|password|device_code|user_code|authorization|cookie)\s*[:=]\s*)("[^"]*"|[^&\s,;]+))",
        std::regex::icase
    );
    static const std::regex queryValue(
        R"(([?&][^=&#\s]+)=([^&\s#]+))",
        std::regex::icase
    );
    static const std::regex bearer(
        R"(\bBearer\s+[A-Za-z0-9._~+/=-]+)",
        std::regex::icase
    );
    static const std::regex xbl(
        R"(XBL3\.0\s+x=[^\s"']+)",
        std::regex::icase
    );
    static const std::regex jwt(
        R"(\beyJ[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{10,}\b)"
    );
    message = std::regex_replace(message, assignedSecret, "$1<redacted>");
    message = std::regex_replace(message, queryValue, "$1=<redacted>");
    message = std::regex_replace(message, bearer, "Bearer <redacted>");
    message = std::regex_replace(message, xbl, "XBL3.0 x=<redacted>");
    message = std::regex_replace(message, jwt, "<redacted-jwt>");
    constexpr std::size_t MaxUiErrorLength = 1200;
    if (message.size() > MaxUiErrorLength) {
        message.resize(MaxUiErrorLength);
        message += "…";
    }
    return message;
}

int64_t unixMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

uint64_t packetHash(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 1469598103934665603ull;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool isItemInteractionBreadcrumb(std::string_view name) {
    return name == "inventory_transaction" ||
        name == "item_stack_request" ||
        name == "item_stack_response" ||
        name == "inventory_content" ||
        name == "inventory_slot" ||
        name == "mob_equipment" ||
        name == "mob_armor_equipment" ||
        name == "add_item_entity" ||
        name == "take_item_entity" ||
        name == "update_equipment" ||
        name == "interact" ||
        name == "animate" ||
        name == "player_action" ||
        name == "player_hotbar" ||
        name == "container_open" ||
        name == "container_close" ||
        name == "container_set_data" ||
        name == "container_registry_cleanup" ||
        name == "set_player_inventory_options" ||
        name == "crafting_event" ||
        name == "completed_using_item" ||
        name == "client_start_item_cooldown" ||
        name == "gui_data_pick_item";
}

bool isClientboundEquipmentFlood(std::string_view name) {
    return name == "mob_equipment" || name == "mob_armor_equipment";
}

bool shouldRecordEquipmentSample(uint64_t sampleIndex) {
    return sampleIndex <= 8 || sampleIndex % 128 == 0;
}

bool isFlightPacket(std::string_view name) {
    return isItemInteractionBreadcrumb(name) ||
        name == "start_game" ||
        name == "item_registry" ||
        name == "creative_content" ||
        name == "crafting_data" ||
        name == "trim_data" ||
        name == "clientbound_map_item_data" ||
        name == "resource_packs_info" ||
        name == "resource_pack_data_info" ||
        name == "resource_pack_chunk_data" ||
        name == "resource_pack_stack" ||
        name == "resource_pack_client_response" ||
        name == "resource_pack_chunk_request" ||
        name == "network_settings" ||
        name == "server_to_client_handshake" ||
        name == "client_to_server_handshake" ||
        name == "request_network_settings" ||
        name == "login" ||
        name == "play_status" ||
        name == "disconnect";
}

std::string packetBreadcrumb(
    std::string_view direction,
    const bedrock::VersionedGamePacket& packet
) {
    std::ostringstream detail;
    detail << "direction=" << direction
           << " packet=" << packet.name
           << " fullBytes=" << packet.fullPacket.size()
           << " payloadBytes=" << packet.payload.size()
           << " hash=0x" << std::hex << packetHash(packet.fullPacket);
    return detail.str();
}

std::string transportBreadcrumb(
    const bedrock::BedrockServerTransportEvent& event
) {
    std::ostringstream detail;
    detail << "kind="
           << bedrock::bedrockServerTransportEventKindName(event.kind)
           << " peer=" << event.peer.address << ':' << event.peer.port
           << " guid=" << event.peer.clientGuid
           << " raknet_id=" << static_cast<unsigned>(event.raknetPacketId)
           << " game_id=" << event.gamePacketId
           << " bytes=" << event.byteLength
           << " hash=0x" << std::hex << event.byteHash << std::dec;
    if (!event.packetName.empty()) {
        detail << " packet=" << event.packetName;
    }
    if (!event.message.empty()) {
        detail << " detail="
               << (event.kind == bedrock::BedrockServerTransportEventKind::Error
                       ? safeMessage(event.message)
                       : event.message);
    }
    return detail.str();
}

std::string rakNetStatisticsBreadcrumb(
    const bedrock::RakNetServerPeerStatistics& statistics
) {
    std::ostringstream detail;
    detail << std::boolalpha
           << "peerKnown=" << statistics.peerKnown
           << " nativeActive=" << statistics.nativeActive
           << " statisticsAvailable=" << statistics.statisticsAvailable
           << " connectionState=" << statistics.connectionState
           << " userPushed=" << statistics.userMessageBytesPushed
           << " userSent=" << statistics.userMessageBytesSent
           << " userResent=" << statistics.userMessageBytesResent
           << " actualSent=" << statistics.actualBytesSent
           << " actualReceived=" << statistics.actualBytesReceived
           << " sendQueueMessages=" << statistics.sendBufferMessages
           << " sendQueueBytes=" << statistics.sendBufferBytes
           << " resendMessages=" << statistics.resendBufferMessages
           << " resendBytes=" << statistics.resendBufferBytes;
    return detail.str();
}

bool isLoginStagePacket(std::string_view name) {
    return name == "request_network_settings" ||
        name == "login" ||
        name == "client_to_server_handshake" ||
        name == "resource_pack_client_response" ||
        name == "set_local_player_as_initialized";
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
    struct FlightRecord {
        uint64_t sequence = 0;
        int64_t timestampMs = 0;
        std::string component;
        std::string message;
    };

    mutable std::mutex mutex;
    bool running = false;
    bool listening = false;
    bool pingDone = false;
    bool pingOk = false;
    bool destinationPingDone = false;
    bool destinationPingOk = false;
    bool upstreamReady = false;
    uint16_t boundPort = 0;
    std::size_t downstreamConnections = 0;
    std::size_t downstreamJoinedCount = 0;
    std::size_t upstreamStartedCount = 0;
    std::size_t upstreamReadyCount = 0;
    std::string destinationHost;
    uint16_t destinationPort = 19132;
    std::string version = "1.21.100";
    std::string destinationGameVersion;
    int destinationProtocolVersion = -1;
    int64_t destinationLatencyMs = 0;
    int64_t relayStartedAt = 0;
    int64_t downstreamConnectedAt = 0;
    std::string lastError;
    std::deque<bedrock::JsRuntimeValue> events;
    uint64_t droppedEvents = 0;
    std::deque<FlightRecord> flight;
    uint64_t flightSequence = 0;
    uint64_t lastFlushedFlightSequence = 0;
    std::atomic<uint64_t> clientboundEquipmentPackets {0};
    std::atomic<uint64_t> clientboundEquipmentTransportPackets {0};
    std::atomic<uint64_t> clientboundEquipmentForwardedPackets {0};

    void push(bedrock::JsRuntimeValue event) {
        std::lock_guard lock(mutex);
        if (event.isObject()) {
            if (!event.get("timestampMs")) {
                const auto now = unixMilliseconds();
                event.set("timestampMs", bedrock::JsRuntimeValue::number(
                    static_cast<double>(now)
                ));
            }
            if (!event.get("level")) {
                event.set("level", bedrock::JsRuntimeValue::string("INFO"));
            }
            if (!event.get("component")) {
                event.set("component", bedrock::JsRuntimeValue::string("native"));
            }
        }
        constexpr std::size_t MaximumEvents = 1024;
        if (events.size() == MaximumEvents) {
            events.pop_front();
            ++droppedEvents;
        }
        events.push_back(std::move(event));
    }

    void resetFlight() {
        std::lock_guard lock(mutex);
        flight.clear();
        lastFlushedFlightSequence = flightSequence;
    }

    void recordFlight(std::string component, std::string message) {
        std::lock_guard lock(mutex);
        constexpr std::size_t MaximumFlightRecords = 768;
        constexpr std::size_t MaximumFlightMessage = 2048;
        if (flight.size() == MaximumFlightRecords) {
            flight.pop_front();
        }
        if (message.size() > MaximumFlightMessage) {
            message.resize(MaximumFlightMessage);
            message += "…<truncated>";
        }
        flight.push_back({
            ++flightSequence,
            unixMilliseconds(),
            std::move(component),
            std::move(message)
        });
    }

    void flushFlight(std::string reason) {
        std::vector<FlightRecord> snapshot;
        uint64_t firstSequence = 0;
        uint64_t lastSequence = 0;
        {
            std::lock_guard lock(mutex);
            if (flight.empty() ||
                flightSequence == lastFlushedFlightSequence) {
                return;
            }
            auto firstUnflushed = flight.begin();
            while (firstUnflushed != flight.end() &&
                   firstUnflushed->sequence <= lastFlushedFlightSequence) {
                ++firstUnflushed;
            }
            if (firstUnflushed == flight.end()) {
                return;
            }
            snapshot.assign(firstUnflushed, flight.end());
            firstSequence = snapshot.front().sequence;
            lastSequence = snapshot.back().sequence;
            lastFlushedFlightSequence = flightSequence;
        }

        push(
            "flight_dump",
            "reason=" + reason + " records=" +
                std::to_string(snapshot.size()) + " sequence=" +
                std::to_string(firstSequence) + ".." +
                std::to_string(lastSequence),
            "WARN",
            "flight"
        );
        for (auto& record : snapshot) {
            push(bedrock::JsRuntimeValue::object({
                {"type", bedrock::JsRuntimeValue::string("flight")},
                {"level", bedrock::JsRuntimeValue::string("DEBUG")},
                {"component", bedrock::JsRuntimeValue::string(
                    "flight." + record.component
                )},
                {"timestampMs", bedrock::JsRuntimeValue::number(
                    static_cast<double>(record.timestampMs)
                )},
                {"message", bedrock::JsRuntimeValue::string(
                    "sequence=" + std::to_string(record.sequence) + " " +
                    std::move(record.message)
                )}
            }));
        }
    }

    void push(
        std::string type,
        std::string message,
        std::string level = "INFO",
        std::string component = "relay"
    ) {
        push(bedrock::JsRuntimeValue::object({
            {"type", bedrock::JsRuntimeValue::string(std::move(type))},
            {"level", bedrock::JsRuntimeValue::string(std::move(level))},
            {"component", bedrock::JsRuntimeValue::string(
                std::move(component)
            )},
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
        {"destinationPingDone", bedrock::JsRuntimeValue::boolean(
            state->destinationPingDone
        )},
        {"destinationPingOk", bedrock::JsRuntimeValue::boolean(
            state->destinationPingOk
        )},
        {"upstreamReady", bedrock::JsRuntimeValue::boolean(
            state->upstreamReady
        )},
        {"boundPort", bedrock::JsRuntimeValue::number(state->boundPort)},
        {"downstreamConnections", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->downstreamConnections)
        )},
        {"downstreamJoinedCount", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->downstreamJoinedCount)
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
        {"version", bedrock::JsRuntimeValue::string(state->version)},
        {"destinationGameVersion", bedrock::JsRuntimeValue::string(
            state->destinationGameVersion
        )},
        {"destinationProtocolVersion", bedrock::JsRuntimeValue::number(
            state->destinationProtocolVersion
        )},
        {"destinationLatencyMs", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->destinationLatencyMs)
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
        : state_(std::move(state)),
          loginWatchdogThread_([this]() { runLoginWatchdog(); }) {}

    ~RelayController() {
        stop();
        stopLoginWatchdog();
    }

    void start(
        const std::string& destinationHost,
        uint16_t destinationPort,
        const std::string& version,
        const std::filesystem::path& cacheDirectory
    ) {
        bedrock::RelayOptions options;
        options.version = version;
        options.host = "0.0.0.0";
        options.port = 19132;
        options.motd = "CPE Relay Android";
        // Keep one overlap slot so a new Minecraft transport can replace a
        // stale Android UDP session before its timeout expires.
        options.maxPlayers = 2;
        options.offline = true;
        options.forceSingle = true;
        options.replaceExisting = true;
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

        state_->push(
            "relay_start",
            "local=0.0.0.0:19132 destination=" + destinationHost + ":" +
                std::to_string(destinationPort) +
                " version=" + version +
                " forceSingle=true replaceExisting=true" +
                " nativeBuild=" + std::string(NativeBuildType) +
                " compilerOptimized=" +
                (NativeCompilerOptimized ? "true" : "false"),
            "INFO",
            "lifecycle"
        );

        auto relay = std::make_unique<bedrock::Relay>(std::move(options));

        relay->live().server().onTransport([this, state](
            const bedrock::BedrockServerTransportEvent& event
        ) {
            if (event.kind == bedrock::BedrockServerTransportEventKind::Open) {
                state->resetFlight();
            }
            bool record = true;
            uint64_t sampleIndex = 0;
            if (event.kind ==
                bedrock::BedrockServerTransportEventKind::Receive) {
                std::lock_guard lock(state->mutex);
                // Raw RakNet payloads are essential before Bedrock login but
                // become high-volume duplicates once decoded packet events
                // are available. Keep exceptional receive breadcrumbs, such
                // as a safely ignored encrypted retransmission.
                record = state->downstreamJoinedCount == 0 ||
                    !event.message.empty();
            } else if (
                event.kind ==
                    bedrock::BedrockServerTransportEventKind::SendPacket &&
                isClientboundEquipmentFlood(event.packetName)
            ) {
                sampleIndex =
                    state->clientboundEquipmentTransportPackets.fetch_add(1) + 1;
                record = shouldRecordEquipmentSample(sampleIndex);
            } else if ((event.kind ==
                            bedrock::BedrockServerTransportEventKind::DecodedPacket ||
                        event.kind ==
                            bedrock::BedrockServerTransportEventKind::SendPacket) &&
                       !isFlightPacket(event.packetName)) {
                record = false;
            }
            const bool publishLoginStage =
                event.kind ==
                    bedrock::BedrockServerTransportEventKind::DecodedPacket &&
                isLoginStagePacket(event.packetName);
            const bool publishError = event.kind ==
                bedrock::BedrockServerTransportEventKind::Error;
            std::string breadcrumb;
            if (record || publishLoginStage || publishError) {
                breadcrumb = transportBreadcrumb(event);
                if (sampleIndex != 0) {
                    breadcrumb += " sample_index=" +
                        std::to_string(sampleIndex);
                }
            }
            if (record) {
                state->recordFlight("transport", breadcrumb);
            }

            if (event.kind ==
                bedrock::BedrockServerTransportEventKind::DecodedPacket) {
                noteLoginStage(event.peer, event.packetName);
                if (publishLoginStage) {
                    state->push(
                        "local_login_stage",
                        breadcrumb,
                        "DEBUG",
                        "downstream"
                    );
                }
            } else if (event.kind ==
                bedrock::BedrockServerTransportEventKind::Error) {
                state->push(
                    "transport_error",
                    breadcrumb,
                    "ERROR",
                    "transport"
                );
                state->flushFlight("transport_error");
            }
        });
        relay->live().server().onLoggingIn([this, state](
            const bedrock::BedrockServerLoggingInEvent& event
        ) {
            completeLoginWatchdog(event.connection.peer, "login_decoded");
            state->push(
                "local_login_stage",
                "packet=login decoded=true protocol=" +
                    std::to_string(event.login.protocolVersion),
                "INFO",
                "downstream"
            );
        });

        relay->onConnect([this, state](bedrock::RelayPlayer& player) {
            state->clientboundEquipmentPackets = 0;
            state->clientboundEquipmentTransportPackets = 0;
            state->clientboundEquipmentForwardedPackets = 0;
            int64_t relayElapsed = 0;
            {
                std::lock_guard lock(state->mutex);
                state->downstreamConnectedAt = unixMilliseconds();
                relayElapsed = state->relayStartedAt == 0
                    ? 0
                    : state->downstreamConnectedAt - state->relayStartedAt;
            }
            state->push(
                "connect",
                "downstream_session=" + player.sessionId() +
                    " elapsedSinceRelayStartMs=" +
                    std::to_string(relayElapsed),
                "INFO",
                "downstream"
            );
            armLoginWatchdog(player.connection, player.sessionId());
        });
        relay->onJoin([state](
            bedrock::RelayPlayer&,
            bedrock::BedrockNetworkClient&
        ) {
            int64_t elapsed = 0;
            {
                std::lock_guard lock(state->mutex);
                elapsed = state->downstreamConnectedAt == 0
                    ? 0
                    : unixMilliseconds() - state->downstreamConnectedAt;
            }
            state->push(
                "upstream_ready",
                "Destination Bedrock session is ready; "
                    "elapsedSinceDownstreamConnectMs=" +
                    std::to_string(elapsed),
                "INFO",
                "upstream"
            );
        });
        relay->onDisconnect([this, state](bedrock::RelayPlayer& player) {
            cancelLoginWatchdog(player.connection.peer);
            state->push(
                "disconnect",
                "downstream_session=" + player.sessionId() +
                    "; matching upstream was notified and closed; "
                    "clientbound_equipment_packets=" +
                    std::to_string(
                        state->clientboundEquipmentPackets.load()
                    ),
                "INFO",
                "lifecycle"
            );
            state->flushFlight("downstream_disconnect");
        });
        relay->onError([state](const std::string& message) {
            {
                std::lock_guard lock(state->mutex);
                state->lastError = safeMessage(message);
            }
            state->push("error", message, "ERROR", "relay");
            state->flushFlight("relay_error");
        });
        relay->onParseError([state](const bedrock::RelayParseError& error) {
            state->push(
                "parse_error",
                "direction=" + std::string(
                    error.direction == bedrock::BedrockRelayDirection::Clientbound
                        ? "clientbound"
                        : "serverbound"
                ) + " session=" + error.sessionId +
                    " packet=" + error.packetName +
                    " policy=" + bedrock::relayParseErrorPolicyName(error.policy) +
                    " error=" + error.message,
                "ERROR",
                "packet"
            );
            state->flushFlight("relay_parse_error");
        });
        relay->live().onServerbound([state](bedrock::BedrockRelayPacketEvent& event) {
            if (isFlightPacket(event.packet.name)) {
                state->recordFlight(
                    "relay_seen",
                    packetBreadcrumb("serverbound", event.packet)
                );
            }
            if (!isItemInteractionBreadcrumb(event.packet.name)) return;
            state->push(
                "packet",
                packetBreadcrumb("serverbound", event.packet),
                "DEBUG",
                "packet"
            );
        });
        relay->live().onClientbound([state](bedrock::BedrockRelayPacketEvent& event) {
            uint64_t sampleIndex = 0;
            bool sampled = true;
            if (isClientboundEquipmentFlood(event.packet.name)) {
                sampleIndex =
                    state->clientboundEquipmentPackets.fetch_add(1) + 1;
                sampled = shouldRecordEquipmentSample(sampleIndex);
            }
            if (sampled && isFlightPacket(event.packet.name)) {
                state->recordFlight(
                    "relay_seen",
                    packetBreadcrumb("clientbound", event.packet)
                );
            }
            if (isItemInteractionBreadcrumb(event.packet.name)) {
                // A populated server can emit hundreds of equipment updates
                // immediately after the registry. Keep enough samples for
                // diagnosis without flooding either the live log or the
                // post-error flight recorder.
                if (!sampled) return;
                auto breadcrumb = packetBreadcrumb(
                    "clientbound",
                    event.packet
                );
                if (sampleIndex != 0) {
                    breadcrumb += " sample_index=" +
                        std::to_string(sampleIndex);
                }
                state->push(
                    "packet",
                    std::move(breadcrumb),
                    "DEBUG",
                    "packet"
                );
            }
            if (event.packet.name == "start_game" ||
                event.packet.name == "item_registry" ||
                event.packet.name == "play_status") {
                state->push(
                    "join_stage",
                    packetBreadcrumb("clientbound", event.packet),
                    "DEBUG",
                    "join"
                );
            }
        });
        relay->live().onForwarded([state](
            const bedrock::BedrockRelayPacketEvent& event
        ) {
            bool record = isFlightPacket(event.packet.name);
            uint64_t sampleIndex = 0;
            if (event.direction ==
                    bedrock::BedrockRelayDirection::Clientbound &&
                isClientboundEquipmentFlood(event.packet.name)) {
                sampleIndex =
                    state->clientboundEquipmentForwardedPackets.fetch_add(1) + 1;
                record = record && shouldRecordEquipmentSample(sampleIndex);
            }
            if (record) {
                auto breadcrumb = packetBreadcrumb(
                    event.direction ==
                            bedrock::BedrockRelayDirection::Clientbound
                        ? "clientbound"
                        : "serverbound",
                    event.packet
                );
                if (sampleIndex != 0) {
                    breadcrumb += " sample_index=" +
                        std::to_string(sampleIndex);
                }
                state->recordFlight(
                    "relay_forwarded",
                    std::move(breadcrumb)
                );
            }
        });
        relay->onStatus([state](const bedrock::BedrockLiveRelayStatus& status) {
            bool becameReady = false;
            bool changed = false;
            bool downstreamJoined = false;
            bool upstreamStarted = false;
            int64_t elapsed = 0;
            {
                std::lock_guard lock(state->mutex);
                becameReady = status.upstreamReady && !state->upstreamReady;
                downstreamJoined = status.downstreamJoinedCount != 0 &&
                    state->downstreamJoinedCount == 0;
                upstreamStarted = status.upstreamStartedCount != 0 &&
                    state->upstreamStartedCount == 0;
                elapsed = state->downstreamConnectedAt == 0
                    ? 0
                    : unixMilliseconds() - state->downstreamConnectedAt;
                changed = state->listening != status.listening ||
                    state->boundPort != status.boundPort ||
                    state->downstreamConnections != status.downstreamConnections ||
                    state->downstreamJoinedCount != status.downstreamJoinedCount ||
                    state->upstreamStartedCount != status.upstreamStartedCount ||
                    state->upstreamReadyCount != status.upstreamReadyCount;
                state->listening = status.listening;
                state->boundPort = status.boundPort;
                state->downstreamConnections = status.downstreamConnections;
                state->downstreamJoinedCount = status.downstreamJoinedCount;
                state->upstreamStartedCount = status.upstreamStartedCount;
                state->upstreamReadyCount = status.upstreamReadyCount;
                state->upstreamReady = status.upstreamReady;
            }
            if (changed) {
                std::ostringstream detail;
                detail << "listening=" << (status.listening ? "true" : "false")
                       << " boundPort=" << status.boundPort
                       << " downstream=" << status.downstreamConnections
                       << " downstreamJoined=" << status.downstreamJoinedCount
                       << " upstreamStarted=" << status.upstreamStartedCount
                       << " upstreamReady=" << status.upstreamReadyCount;
                state->push(
                    "status",
                    detail.str(),
                    "DEBUG",
                    "lifecycle"
                );
            }
            if (downstreamJoined) {
                state->push(
                    "join_stage",
                    "downstream handshake complete; elapsedMs=" +
                        std::to_string(elapsed),
                    "INFO",
                    "join"
                );
            }
            if (upstreamStarted) {
                state->push(
                    "join_stage",
                    "upstream authentication/connection started; elapsedMs=" +
                        std::to_string(elapsed),
                    "INFO",
                    "join"
                );
            }
            if (becameReady) {
                state->push(
                    "upstream_ready",
                    "Destination Bedrock session became ready",
                    "INFO",
                    "upstream"
                );
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
            listener.host + ":" + std::to_string(listener.port),
            "INFO",
            "listener"
        );

        {
            std::lock_guard lock(relayMutex_);
            relay_ = std::move(relay);
        }

        auto localPing = std::async(std::launch::async, [port = listener.port]() {
            return bedrock::RakNetPinger::ping("127.0.0.1", port, 1200);
        });
        auto destinationPing = std::async(
            std::launch::async,
            [destinationHost, destinationPort]() {
                const auto started = std::chrono::steady_clock::now();
                auto pong = bedrock::RakNetPinger::ping(
                    destinationHost,
                    destinationPort,
                    1500
                );
                const auto elapsed = std::chrono::duration_cast<
                    std::chrono::milliseconds
                >(std::chrono::steady_clock::now() - started).count();
                return std::make_pair(std::move(pong), elapsed);
            }
        );
        const auto pong = localPing.get();
        {
            std::lock_guard lock(state_->mutex);
            state_->pingDone = true;
            state_->pingOk = pong.ok;
        }
        state_->push(
            pong.ok ? "ping_ok" : "ping_warning",
            pong.ok ? "Local RakNet pong verified on 127.0.0.1:" +
                std::to_string(listener.port) : safeMessage(pong.error),
            pong.ok ? "INFO" : "WARN",
            "listener"
        );

        auto destinationResult = destinationPing.get();
        const auto& upstreamPong = destinationResult.first;
        const auto destinationLatencyMs = destinationResult.second;
        {
            std::lock_guard lock(state_->mutex);
            state_->destinationPingDone = true;
            state_->destinationPingOk = upstreamPong.ok;
            state_->destinationGameVersion = upstreamPong.gameVersion;
            state_->destinationProtocolVersion = upstreamPong.protocolVersion;
            state_->destinationLatencyMs = destinationLatencyMs;
        }
        if (upstreamPong.ok) {
            const bool versionMismatch = !upstreamPong.gameVersion.empty() &&
                upstreamPong.gameVersion != version;
            state_->push(
                versionMismatch ? "destination_version_warning" : "destination_ping_ok",
                "destination=" + destinationHost + ":" +
                    std::to_string(destinationPort) +
                    " gameVersion=" + upstreamPong.gameVersion +
                    " protocol=" + std::to_string(upstreamPong.protocolVersion) +
                    " latencyMs=" + std::to_string(destinationLatencyMs) +
                    (versionMismatch
                        ? " selectedVersion=" + version + " mismatch=true"
                        : " mismatch=false"),
                versionMismatch ? "WARN" : "INFO",
                "destination"
            );
        } else {
            state_->push(
                "destination_ping_warning",
                "destination=" + destinationHost + ":" +
                    std::to_string(destinationPort) + " error=" +
                    safeMessage(upstreamPong.error),
                "WARN",
                "destination"
            );
        }
    }

    void stop() {
        if (stopped_.exchange(true)) return;
        stopLoginWatchdog();
        std::unique_ptr<bedrock::Relay> relay;
        {
            std::lock_guard lock(relayMutex_);
            relay = std::move(relay_);
        }
        if (relay) {
            try {
                relay->close("Android relay stopped");
            } catch (const std::exception& error) {
                state_->push(
                    "error",
                    "Relay close failed: " + std::string(error.what()),
                    "ERROR",
                    "lifecycle"
                );
            } catch (...) {
                state_->push(
                    "error",
                    "Relay close failed with an unknown native exception",
                    "ERROR",
                    "lifecycle"
                );
            }
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->running = false;
            state_->listening = false;
            state_->upstreamReady = false;
            state_->destinationPingDone = false;
            state_->destinationPingOk = false;
            state_->destinationLatencyMs = 0;
            state_->downstreamConnections = 0;
            state_->downstreamJoinedCount = 0;
            state_->upstreamStartedCount = 0;
            state_->upstreamReadyCount = 0;
        }
        state_->push("stopped", "Relay stopped", "INFO", "lifecycle");
    }

private:
    struct PendingLogin {
        bedrock::BedrockServerConnection connection;
        std::string sessionId;
        std::string stage = "raknet_open";
        std::chrono::steady_clock::time_point deadline;
        uint64_t generation = 0;
    };

    std::shared_ptr<RelayState> state_;
    std::mutex relayMutex_;
    std::unique_ptr<bedrock::Relay> relay_;
    std::atomic<bool> stopped_ {false};
    std::mutex loginWatchdogMutex_;
    std::condition_variable loginWatchdogCv_;
    std::optional<PendingLogin> pendingLogin_;
    uint64_t loginWatchdogGeneration_ = 0;
    bool loginWatchdogStopping_ = false;
    std::thread loginWatchdogThread_;

    static bool samePeer(
        const bedrock::RakNetServerPeer& lhs,
        const bedrock::RakNetServerPeer& rhs
    ) {
        return lhs.address == rhs.address && lhs.port == rhs.port &&
            lhs.clientGuid == rhs.clientGuid;
    }

    void armLoginWatchdog(
        const bedrock::BedrockServerConnection& connection,
        std::string sessionId
    ) {
        {
            std::lock_guard lock(loginWatchdogMutex_);
            if (loginWatchdogStopping_) return;
            pendingLogin_ = PendingLogin {
                connection,
                std::move(sessionId),
                "raknet_open",
                std::chrono::steady_clock::now() +
                    std::chrono::seconds(15),
                ++loginWatchdogGeneration_
            };
        }
        loginWatchdogCv_.notify_all();
    }

    void noteLoginStage(
        const bedrock::RakNetServerPeer& peer,
        const std::string& stage
    ) {
        std::lock_guard lock(loginWatchdogMutex_);
        if (!pendingLogin_ ||
            !samePeer(pendingLogin_->connection.peer, peer)) return;
        pendingLogin_->stage = stage;
    }

    void completeLoginWatchdog(
        const bedrock::RakNetServerPeer& peer,
        const std::string& stage
    ) {
        {
            std::lock_guard lock(loginWatchdogMutex_);
            if (!pendingLogin_ ||
                !samePeer(pendingLogin_->connection.peer, peer)) return;
            pendingLogin_->stage = stage;
            pendingLogin_.reset();
            ++loginWatchdogGeneration_;
        }
        loginWatchdogCv_.notify_all();
    }

    void cancelLoginWatchdog(const bedrock::RakNetServerPeer& peer) {
        completeLoginWatchdog(peer, "transport_closed");
    }

    void runLoginWatchdog() {
        std::unique_lock lock(loginWatchdogMutex_);
        while (!loginWatchdogStopping_) {
            if (!pendingLogin_) {
                loginWatchdogCv_.wait(lock, [this]() {
                    return loginWatchdogStopping_ || pendingLogin_.has_value();
                });
                continue;
            }

            const auto generation = pendingLogin_->generation;
            const auto deadline = pendingLogin_->deadline;
            if (loginWatchdogCv_.wait_until(lock, deadline, [this, generation]() {
                    return loginWatchdogStopping_ || !pendingLogin_ ||
                        pendingLogin_->generation != generation;
                })) {
                continue;
            }

            const auto timedOut = *pendingLogin_;
            pendingLogin_.reset();
            ++loginWatchdogGeneration_;
            lock.unlock();

            std::string rakNetDetail = "unavailable";
            {
                std::lock_guard relayLock(relayMutex_);
                if (relay_) {
                    rakNetDetail = rakNetStatisticsBreadcrumb(
                        relay_->live().server().transportStatistics(
                            timedOut.connection
                        )
                    );
                }
            }

            state_->push(
                "local_login_timeout",
                "downstream_session=" + timedOut.sessionId +
                    " last_stage=" + timedOut.stage +
                    " timeoutMs=15000 raknet={" + rakNetDetail +
                    "}; closing stale local transport so "
                    "the next Minecraft attempt is not blocked",
                "ERROR",
                "watchdog"
            );
            state_->flushFlight("local_login_timeout");
            {
                std::lock_guard relayLock(relayMutex_);
                if (relay_) {
                    relay_->live().disconnectDownstream(
                        timedOut.connection,
                        "Local Minecraft login timed out"
                    );
                }
            }

            lock.lock();
        }
    }

    void stopLoginWatchdog() {
        {
            std::lock_guard lock(loginWatchdogMutex_);
            if (loginWatchdogStopping_) return;
            loginWatchdogStopping_ = true;
            pendingLogin_.reset();
            ++loginWatchdogGeneration_;
        }
        loginWatchdogCv_.notify_all();
        if (loginWatchdogThread_.joinable() &&
            loginWatchdogThread_.get_id() != std::this_thread::get_id()) {
            loginWatchdogThread_.join();
        }
    }
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
    jstring versionValue,
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
        const auto version = fromJavaString(environment, versionValue);
        if (destinationHost.empty()) {
            throw std::runtime_error("Destination host is empty");
        }
        if (destinationPortValue < 1 || destinationPortValue > 65535) {
            throw std::runtime_error("Destination port is out of range");
        }
        if (cacheDirectory.empty()) {
            throw std::runtime_error("Authentication cache path is empty");
        }
        if (version.empty() || !bedrock::supportsVersion(version)) {
            throw std::runtime_error(
                "Unsupported Minecraft Bedrock version: " + version
            );
        }

        state->destinationHost = destinationHost;
        state->destinationPort = static_cast<uint16_t>(destinationPortValue);
        state->version = version;
        state->relayStartedAt = unixMilliseconds();
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
            version,
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
        state->push("error", message, "ERROR", "native");
        auto result = snapshotValue(state, false);
        result.set("error", bedrock::JsRuntimeValue::string(message));
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "%s", message.c_str());
        return toJavaString(environment, jsonString(result));
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_supportedVersions(
    JNIEnv* environment,
    jclass
) {
    std::vector<bedrock::JsRuntimeValue> versions;
    for (auto& version : bedrock::versions()) {
        versions.push_back(bedrock::JsRuntimeValue::string(
            std::move(version)
        ));
    }
    return toJavaString(
        environment,
        jsonString(bedrock::JsRuntimeValue::array(std::move(versions)))
    );
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
        events.reserve(state->events.size() + (state->droppedEvents == 0 ? 0 : 1));
        if (state->droppedEvents != 0) {
            events.push_back(bedrock::JsRuntimeValue::object({
                {"type", bedrock::JsRuntimeValue::string("log_overflow")},
                {"level", bedrock::JsRuntimeValue::string("WARN")},
                {"component", bedrock::JsRuntimeValue::string("diagnostics")},
                {"timestampMs", bedrock::JsRuntimeValue::number(
                    static_cast<double>(unixMilliseconds())
                )},
                {"message", bedrock::JsRuntimeValue::string(
                    "Native diagnostic queue dropped " +
                    std::to_string(state->droppedEvents) +
                    " oldest events"
                )}
            }));
            state->droppedEvents = 0;
        }
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
