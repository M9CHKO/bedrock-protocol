#include <bedrock/RakNetPing.hpp>
#include <bedrock/auth/JsRuntimeValue.hpp>
#include <bedrock/auth/XboxTokenManager.hpp>
#include <bedrock/bedrock.hpp>
#include <bedrock/relay/EntityPositionTracker.hpp>
#include <bedrock/relay/ItemDurability.hpp>
#include <bedrock/world/BedrockBlockRegistry.hpp>

#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>

#if defined(BEDROCK_ANDROID_RELEASE_BUILD) && !defined(__OPTIMIZE__)
#error "The distributable Android relay must compile native code with optimization"
#endif

namespace {

constexpr char LogTag[] = "CpeRelayNative";
constexpr int MinimumRetainedRadiusChunks = 10;
constexpr int MaximumRetainedRadiusChunks = 64;
constexpr int MaximumClientPublisherRadiusChunks = 32;
constexpr std::size_t AndroidLevelChunkRetentionMaximumBytes =
    96u * 1024u * 1024u;

std::atomic<bool> configuredDetailedLogging {true};
std::atomic<bool> configuredChunkRetention {false};
std::atomic<int> configuredRetainedRadiusChunks {24};
std::atomic<bool> configuredAutoArmor {false};
std::atomic<bool> configuredAutoTotem {false};
std::atomic<bool> configuredMiniMap {false};
std::atomic<bool> configuredSchematic {false};

int clampRetainedRadiusChunks(int radius) {
    return std::max(
        MinimumRetainedRadiusChunks,
        std::min(MaximumRetainedRadiusChunks, radius)
    );
}

std::array<int, 3> parsedVersion(std::string_view version) noexcept {
    std::array<int, 3> result {};
    std::size_t offset = 0;
    for (std::size_t index = 0; index < result.size(); ++index) {
        int value = 0;
        bool foundDigit = false;
        while (offset < version.size() && version[offset] >= '0' &&
               version[offset] <= '9') {
            foundDigit = true;
            value = value * 10 + (version[offset] - '0');
            ++offset;
        }
        result[index] = foundDigit ? value : 0;
        if (offset < version.size() && version[offset] == '.') ++offset;
    }
    return result;
}

bool versionAtLeast(
    std::string_view version,
    int major,
    int minor,
    int patch
) noexcept {
    return parsedVersion(version) >= std::array<int, 3> {major, minor, patch};
}

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

uint64_t steadyMilliseconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

std::optional<int64_t> packetInteger(
    const bedrock::PacketValue* value
) noexcept {
    if (value == nullptr) return std::nullopt;
    switch (value->kind) {
        case bedrock::PacketValue::Kind::Int:
            return value->intValue;
        case bedrock::PacketValue::Kind::UInt:
            return static_cast<int64_t>(value->uintValue);
        case bedrock::PacketValue::Kind::Double:
            return static_cast<int64_t>(value->doubleValue);
        case bedrock::PacketValue::Kind::Bool:
            return value->boolValue ? 1 : 0;
        default:
            return std::nullopt;
    }
}

const bedrock::PacketValue* findNamedPacketValue(
    const bedrock::PacketValue& value,
    std::string_view name,
    int depth = 0
) noexcept {
    if (depth > 24) return nullptr;
    if (value.kind == bedrock::PacketValue::Kind::Object) {
        const auto direct = value.objectValue.find(std::string(name));
        if (direct != value.objectValue.end()) return &direct->second;
        for (const auto& [_, child] : value.objectValue) {
            if (const auto* found = findNamedPacketValue(
                    child,
                    name,
                    depth + 1
                )) {
                return found;
            }
        }
    } else if (value.kind == bedrock::PacketValue::Kind::Array) {
        for (const auto& child : value.arrayValue) {
            if (const auto* found = findNamedPacketValue(
                    child,
                    name,
                    depth + 1
                )) {
                return found;
            }
        }
    }
    return nullptr;
}

std::optional<int32_t> nbtInteger(
    const bedrock::PacketValue* item,
    std::string_view name
) noexcept {
    if (item == nullptr) return std::nullopt;
    const auto* node = findNamedPacketValue(*item, name);
    if (node == nullptr) return std::nullopt;
    if (const auto direct = packetInteger(node)) {
        return static_cast<int32_t>(*direct);
    }
    if (node->kind == bedrock::PacketValue::Kind::Object) {
        const auto payload = node->objectValue.find("value");
        if (payload != node->objectValue.end()) {
            if (const auto number = packetInteger(&payload->second)) {
                return static_cast<int32_t>(*number);
            }
        }
    }
    return std::nullopt;
}

std::optional<int32_t> itemNbtInteger(
    const bedrock::RelayPacketEvent& decoded,
    const std::string& prefix,
    std::string_view name
) noexcept {
    // Match prismarine-protocol's decoded item shape first. Restricting the
    // search to the item NBT root prevents an unrelated `metadata` or nested
    // display tag from being mistaken for durability.
    const std::array<std::string, 7> roots {
        prefix + ".extra.nbt.nbt.value",
        prefix + ".extra.nbt.value",
        prefix + ".extra.nbt",
        prefix + ".nbt.nbt.value",
        prefix + ".nbt.value",
        prefix + ".nbt",
        prefix
    };
    for (const auto& root : roots) {
        if (const auto value = nbtInteger(decoded.value(root), name)) {
            return value;
        }
    }
    return std::nullopt;
}

bool packetValueHasContent(const bedrock::PacketValue* value) noexcept {
    if (value == nullptr) return false;
    if (value->kind == bedrock::PacketValue::Kind::Array) {
        return !value->arrayValue.empty();
    }
    if (value->kind == bedrock::PacketValue::Kind::Object) {
        const auto payload = value->objectValue.find("value");
        if (payload != value->objectValue.end()) {
            return packetValueHasContent(&payload->second);
        }
        return !value->objectValue.empty();
    }
    return value->kind != bedrock::PacketValue::Kind::Null;
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

bool isResourcePackTransportPacket(std::string_view name) {
    return name == "resource_packs_info" ||
        name == "resource_pack_stack" ||
        name == "resource_pack_data_info" ||
        name == "resource_pack_chunk_data" ||
        name == "resource_pack_client_response" ||
        name == "resource_pack_chunk_request";
}

bool shouldPublishResourcePackSample(
    std::string_view name,
    uint64_t sampleIndex
) {
    if (name != "resource_pack_chunk_data" &&
        name != "resource_pack_chunk_request") {
        return true;
    }
    return sampleIndex <= 8 || sampleIndex % 32 == 0;
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
        isResourcePackTransportPacket(name) ||
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
    struct EquipmentItem {
        bool present = false;
        int64_t networkId = 0;
        int32_t count = 0;
        int32_t stackId = 0;
        std::string name;
        int32_t damage = 0;
        bool damageKnown = false;
        int32_t maximumDurability = 0;
        int32_t remainingDurability = 0;
        int32_t durabilityPercent = 0;
        bool enchanted = false;
    };

    struct FlightRecord {
        uint64_t sequence = 0;
        int64_t timestampMs = 0;
        std::string component;
        std::string message;
    };

    struct MiniMapKey {
        int32_t dimension = 0;
        int32_t x = 0;
        int32_t z = 0;

        bool operator==(const MiniMapKey&) const = default;
    };

    struct MiniMapKeyHash {
        std::size_t operator()(const MiniMapKey& key) const noexcept {
            std::size_t result = std::hash<int32_t> {}(key.dimension);
            result ^= std::hash<int32_t> {}(key.x) +
                0x9e3779b9u + (result << 6u) + (result >> 2u);
            result ^= std::hash<int32_t> {}(key.z) +
                0x9e3779b9u + (result << 6u) + (result >> 2u);
            return result;
        }
    };

    struct MiniMapTile {
        MiniMapKey key;
        std::array<int32_t, 256> pixels {};
        std::array<int16_t, 256> surfaceHeights {};
        std::array<float, 256> groundHeights {};
        uint64_t revision = 0;
    };

    struct MiniMapAppearance {
        bool air = false;
        bool solid = false;
        float collisionTop = 1.0f;
        int32_t color = 0xff777777;
    };

    struct MiniMapChunkJob {
        std::string version;
        std::vector<uint8_t> payload;
        uint64_t generation = 0;
        int32_t dimension = 0;
        int32_t x = 0;
        int32_t z = 0;
        int64_t cameraDistanceSquared = 0;
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
    std::atomic<uint64_t> resourcePackPacketsSeen {0};
    std::atomic<uint64_t> resourcePackPacketsForwarded {0};
    std::atomic<bool> detailedLogging {true};
    std::atomic<bool> chunkRetentionEnabled {false};
    std::atomic<bool> minecraftUiBlocked {false};
    std::atomic<bool> autoArmorEnabled {false};
    std::atomic<bool> autoTotemEnabled {false};
    std::atomic<bool> miniMapEnabled {false};
    std::atomic<bool> schematicEnabled {false};
    std::atomic<int> retainedRadiusChunks {24};
    std::atomic<uint64_t> chunkPublisherPacketsObserved {0};
    std::atomic<uint64_t> chunkPublisherPacketsRewritten {0};
    std::atomic<uint64_t> chunkPublisherDecodeFailures {0};
    std::atomic<uint32_t> lastServerPublisherRadiusBlocks {0};
    std::atomic<uint32_t> lastEffectivePublisherRadiusBlocks {0};
    std::atomic<uint64_t> retainedLevelChunkCount {0};
    std::atomic<uint64_t> retainedLevelChunkBytes {0};
    std::atomic<uint64_t> retainedLevelChunkMaximumBytes {
        AndroidLevelChunkRetentionMaximumBytes
    };
    std::atomic<uint64_t> retainedLevelChunksStored {0};
    std::atomic<uint64_t> retainedLevelChunksReplaced {0};
    std::atomic<uint64_t> retainedLevelChunksEvictedRadius {0};
    std::atomic<uint64_t> retainedLevelChunksEvictedMemory {0};
    std::atomic<uint64_t> retainedLevelChunkParseFailures {0};
    std::atomic<uint64_t> cameraOrientationUpdates {0};
    std::atomic<uint64_t> cameraOrientationDecodeFailures {0};
    bedrock::EntityPositionTracker entityPositions;
    // main hand, off hand, helmet, chestplate, leggings, boots
    std::array<EquipmentItem, 6> equipment;
    uint64_t equipmentRevision = 0;
    std::vector<EquipmentItem> playerInventory;
    bool playerInventoryReady = false;
    int32_t nextAutomationRequestId = 1'000'000;
    int32_t pendingAutomationRequestId = 0;
    uint64_t pendingAutomationStartedAtMs = 0;
    uint64_t lastAutomationAttemptAtMs = 0;
    uint64_t automationAccepted = 0;
    uint64_t automationRejected = 0;
    std::string automationStatus = "Ожидание инвентаря";
    double playerHealth = 20.0;
    double playerMaximumHealth = 20.0;
    double playerHunger = 20.0;
    double playerSaturation = 5.0;
    double playerAbsorption = 0.0;
    bool playerHealthKnown = false;
    bool playerHungerKnown = false;
    bool playerAbsorptionKnown = false;
    int32_t playerResistanceLevel = 0;
    std::unordered_map<int64_t, std::string> itemNames;
    bedrock::ProtoDefVariableStorePtr itemProtocolVariables =
        bedrock::makeProtoDefVariableStore();
    std::mutex itemDecodeMutex;
    std::atomic<int32_t> miniMapDimension {0};
    std::atomic<uint64_t> miniMapDecodedChunks {0};
    std::atomic<uint64_t> miniMapDecodeFailures {0};
    std::atomic<uint64_t> miniMapCachedChunksSkipped {0};
    mutable std::mutex miniMapMutex;
    std::condition_variable miniMapCondition;
    std::deque<MiniMapChunkJob> miniMapJobs;
    std::unordered_map<MiniMapKey, MiniMapTile, MiniMapKeyHash> miniMapTiles;
    std::thread miniMapWorker;
    bool miniMapStopping = false;
    uint64_t miniMapGeneration = 1;
    uint64_t miniMapRevision = 0;
    mutable std::mutex blockRegistryMutex;
    std::optional<bedrock::BedrockBlockRegistry> blockRegistry;
    std::unordered_map<int32_t, MiniMapAppearance> miniMapAppearances;
    bool blockRuntimeIdsAreHashes = true;

    RelayState() {
        miniMapWorker = std::thread([this]() { miniMapWorkerLoop(); });
    }

    ~RelayState() {
        {
            std::lock_guard lock(miniMapMutex);
            miniMapStopping = true;
            miniMapJobs.clear();
        }
        miniMapCondition.notify_all();
        if (miniMapWorker.joinable()) miniMapWorker.join();
    }

    static int32_t floatBits(float value) noexcept {
        static_assert(sizeof(float) == sizeof(int32_t));
        int32_t result = 0;
        std::memcpy(&result, &value, sizeof(result));
        return result;
    }

    static constexpr int16_t UnknownSurfaceHeight =
        std::numeric_limits<int16_t>::min();

    static bool blockNameContains(
        std::string_view name,
        std::string_view part
    ) noexcept {
        return name.find(part) != std::string_view::npos;
    }

    static bool blockNameHasToken(
        std::string_view name,
        std::string_view token
    ) noexcept {
        std::size_t offset = 0;
        while (offset <= name.size()) {
            const auto found = name.find(token, offset);
            if (found == std::string_view::npos) return false;
            const auto end = found + token.size();
            if ((found == 0 || name[found - 1] == '_') &&
                (end == name.size() || name[end] == '_')) {
                return true;
            }
            offset = found + 1;
        }
        return false;
    }

    static int32_t mapRgb(int red, int green, int blue) noexcept {
        return static_cast<int32_t>(
            0xff000000u |
            (static_cast<uint32_t>(std::clamp(red, 0, 255)) << 16u) |
            (static_cast<uint32_t>(std::clamp(green, 0, 255)) << 8u) |
            static_cast<uint32_t>(std::clamp(blue, 0, 255))
        );
    }

    static int32_t namedBlockColor(
        std::string_view name,
        int32_t dimension
    ) noexcept {
        struct DyeColor {
            std::string_view name;
            int red;
            int green;
            int blue;
        };
        static constexpr std::array<DyeColor, 16> Dyes {{
            {"light_blue", 85, 169, 214}, {"light_gray", 169, 170, 165},
            {"magenta", 184, 75, 183}, {"orange", 232, 137, 45},
            {"yellow", 231, 207, 59}, {"lime", 116, 186, 54},
            {"pink", 216, 137, 167}, {"gray", 85, 90, 94},
            {"cyan", 49, 138, 145}, {"purple", 127, 69, 162},
            {"blue", 63, 85, 163}, {"brown", 112, 70, 45},
            {"green", 82, 107, 47}, {"red", 168, 61, 61},
            {"black", 38, 39, 42}, {"white", 231, 231, 223}
        }};
        const bool dyedMaterial = blockNameContains(name, "wool") ||
            blockNameContains(name, "concrete") ||
            blockNameContains(name, "terracotta") ||
            blockNameContains(name, "stained_glass") ||
            blockNameContains(name, "glazed") ||
            blockNameContains(name, "carpet") ||
            blockNameContains(name, "candle") ||
            blockNameContains(name, "shulker_box");
        if (dyedMaterial) {
            for (const auto& dye : Dyes) {
                if (blockNameHasToken(name, dye.name)) {
                    return mapRgb(dye.red, dye.green, dye.blue);
                }
            }
        }

        if (blockNameContains(name, "water") ||
            blockNameContains(name, "bubble_column")) return mapRgb(49, 105, 196);
        if (blockNameContains(name, "lava")) return mapRgb(238, 83, 25);
        if (blockNameContains(name, "magma")) return mapRgb(151, 61, 34);
        if (blockNameContains(name, "snow") || name == "powder_snow") {
            return mapRgb(238, 244, 247);
        }
        if (blockNameContains(name, "ice")) return mapRgb(125, 184, 235);
        if (blockNameContains(name, "grass_block") ||
            blockNameContains(name, "moss") || name == "grass") {
            return mapRgb(91, 151, 70);
        }
        if (blockNameContains(name, "leaves") ||
            blockNameContains(name, "vine") ||
            blockNameContains(name, "azalea")) return mapRgb(55, 116, 54);
        if (blockNameContains(name, "kelp") ||
            blockNameContains(name, "seagrass")) return mapRgb(43, 111, 84);
        if (blockNameContains(name, "dirt") ||
            blockNameContains(name, "podzol") ||
            blockNameContains(name, "farmland")) return mapRgb(123, 83, 54);
        if (blockNameContains(name, "mud")) return mapRgb(78, 69, 71);
        if (blockNameContains(name, "mycelium")) return mapRgb(111, 89, 106);
        if (blockNameContains(name, "sand") ||
            blockNameContains(name, "end_stone")) return mapRgb(215, 199, 135);
        if (blockNameContains(name, "gravel")) return mapRgb(126, 120, 116);
        if (blockNameContains(name, "clay")) return mapRgb(152, 161, 178);
        // Ores and metal blocks must win before their deepslate/stone host.
        if (blockNameContains(name, "diamond")) return mapRgb(67, 199, 198);
        if (blockNameContains(name, "emerald")) return mapRgb(48, 182, 91);
        if (blockNameContains(name, "lapis")) return mapRgb(52, 81, 151);
        if (blockNameContains(name, "redstone")) return mapRgb(180, 43, 46);
        if (blockNameContains(name, "gold")) return mapRgb(222, 177, 45);
        if (blockNameContains(name, "iron")) return mapRgb(190, 181, 164);
        if (blockNameContains(name, "coal")) return mapRgb(48, 49, 50);
        if (blockNameContains(name, "oxidized_copper")) return mapRgb(61, 148, 124);
        if (blockNameContains(name, "weathered_copper")) return mapRgb(83, 145, 111);
        if (blockNameContains(name, "exposed_copper")) return mapRgb(171, 121, 83);
        if (blockNameContains(name, "copper")) return mapRgb(190, 105, 75);
        if (blockNameContains(name, "deepslate") ||
            blockNameContains(name, "blackstone")) return mapRgb(55, 59, 68);
        if (blockNameContains(name, "basalt")) return mapRgb(73, 72, 76);
        if (blockNameContains(name, "tuff")) return mapRgb(99, 109, 100);
        if (blockNameContains(name, "calcite") ||
            blockNameContains(name, "quartz")) return mapRgb(220, 215, 204);
        if (blockNameContains(name, "granite")) return mapRgb(151, 103, 82);
        if (blockNameContains(name, "diorite")) return mapRgb(188, 187, 183);
        if (blockNameContains(name, "andesite")) return mapRgb(116, 119, 120);
        if (blockNameContains(name, "stone") ||
            blockNameContains(name, "cobble")) return mapRgb(119, 123, 126);
        if (blockNameContains(name, "netherrack")) return mapRgb(105, 45, 47);
        if (blockNameContains(name, "nether_brick")) return mapRgb(55, 29, 36);
        if (blockNameContains(name, "soul_sand") ||
            blockNameContains(name, "soul_soil")) return mapRgb(82, 63, 51);
        if (blockNameContains(name, "warped")) return mapRgb(41, 116, 112);
        if (blockNameContains(name, "crimson")) return mapRgb(125, 51, 72);
        if (blockNameContains(name, "prismarine")) return mapRgb(83, 151, 136);
        if (blockNameContains(name, "purpur")) return mapRgb(164, 113, 166);
        if (blockNameContains(name, "brick")) return mapRgb(151, 77, 65);
        if (blockNameContains(name, "glass")) return mapRgb(157, 210, 216);
        if (blockNameContains(name, "slime")) return mapRgb(109, 190, 87);
        if (blockNameContains(name, "honey")) return mapRgb(222, 151, 45);
        if (blockNameContains(name, "hay")) return mapRgb(194, 168, 45);
        if (blockNameContains(name, "melon")) return mapRgb(109, 151, 47);
        if (blockNameContains(name, "pumpkin")) return mapRgb(196, 111, 35);
        if (blockNameContains(name, "flower") ||
            blockNameContains(name, "tulip") ||
            blockNameContains(name, "orchid") ||
            blockNameContains(name, "dandelion")) return mapRgb(183, 111, 123);

        if (blockNameContains(name, "planks") ||
            blockNameContains(name, "log") ||
            blockNameContains(name, "wood") ||
            blockNameContains(name, "stem") ||
            blockNameContains(name, "hyphae") ||
            blockNameContains(name, "bookshelf") ||
            blockNameContains(name, "chest") ||
            blockNameContains(name, "barrel")) {
            if (blockNameContains(name, "spruce") ||
                blockNameContains(name, "dark_oak")) return mapRgb(83, 58, 38);
            if (blockNameContains(name, "birch") ||
                blockNameContains(name, "bamboo")) return mapRgb(196, 174, 109);
            if (blockNameContains(name, "acacia")) return mapRgb(165, 89, 51);
            if (blockNameContains(name, "mangrove")) return mapRgb(113, 55, 48);
            if (blockNameContains(name, "cherry")) return mapRgb(210, 151, 151);
            return mapRgb(151, 108, 64);
        }

        uint32_t hash = 2166136261u;
        for (const auto byte : name) {
            hash ^= static_cast<uint8_t>(byte);
            hash *= 16777619u;
        }
        const int dimensionBias = dimension == 1 ? -18 : (dimension == 2 ? 14 : 0);
        return mapRgb(
            72 + static_cast<int>(hash & 0x5f) + dimensionBias,
            72 + static_cast<int>((hash >> 8u) & 0x5f),
            72 + static_cast<int>((hash >> 16u) & 0x5f)
        );
    }

    static int32_t shadeMapColor(
        int32_t color,
        int shade,
        uint32_t biomeId
    ) noexcept {
        const auto raw = static_cast<uint32_t>(color);
        const int biomeShade = static_cast<int>(biomeId % 7u) - 3;
        const int amount = std::clamp(shade + biomeShade, -52, 48);
        return mapRgb(
            static_cast<int>((raw >> 16u) & 0xffu) + amount,
            static_cast<int>((raw >> 8u) & 0xffu) + amount,
            static_cast<int>(raw & 0xffu) + amount
        );
    }

    MiniMapAppearance miniMapAppearanceLocked(
        int32_t runtimeId,
        int32_t dimension
    ) {
        const auto cached = miniMapAppearances.find(runtimeId);
        if (cached != miniMapAppearances.end()) return cached->second;
        MiniMapAppearance appearance;
        if (blockRegistry.has_value()) {
            const auto* block = blockRegistry->blockByRuntimeId(runtimeId);
            const auto* state = blockRegistry->stateByRuntimeId(runtimeId);
            if (block != nullptr) {
                appearance.air = block->name == "air" ||
                    block->name == "cave_air" || block->name == "void_air";
                if (!appearance.air && state != nullptr) {
                    float collisionTop = -std::numeric_limits<float>::infinity();
                    for (const auto& shape : state->shapes) {
                        if (shape[3] <= 0.0 || shape[4] <= 0.0 ||
                            shape[5] <= 0.0) {
                            continue;
                        }
                        const auto top = static_cast<float>(
                            shape[1] + shape[4] * 0.5
                        );
                        if (std::isfinite(top)) {
                            collisionTop = std::max(collisionTop, top);
                        }
                    }
                    if (std::isfinite(collisionTop)) {
                        appearance.solid = true;
                        appearance.collisionTop = collisionTop;
                    }
                }
                if (!appearance.air && !appearance.solid &&
                    block->boundingBox != "empty" &&
                    (state == nullptr || state->missingStateShape)) {
                    appearance.solid = true;
                    appearance.collisionTop = 1.0f;
                }
                appearance.color = namedBlockColor(block->name, dimension);
            } else {
                appearance.air = runtimeId == 0;
                appearance.solid = !appearance.air;
                appearance.color = namedBlockColor(
                    "unknown_" + std::to_string(runtimeId),
                    dimension
                );
            }
        } else {
            appearance.air = runtimeId == 0;
            appearance.solid = !appearance.air;
            appearance.color = namedBlockColor(
                "runtime_" + std::to_string(runtimeId),
                dimension
            );
        }
        miniMapAppearances.emplace(runtimeId, appearance);
        return appearance;
    }

    void loadBlockRegistry(const std::filesystem::path& directory) {
        if (directory.empty()) return;
        auto loaded = bedrock::BedrockBlockRegistryLoader::loadMinecraftData(
            directory / "blocks.json",
            directory / "blockStates.json",
            directory / "blockCollisionShapes.json",
            true
        );
        loaded.loadRuntimeIds(true);
        const auto blockCount = loaded.blockCount();
        const auto stateCount = loaded.stateCount();
        {
            std::lock_guard lock(blockRegistryMutex);
            blockRegistry = std::move(loaded);
            miniMapAppearances.clear();
            blockRuntimeIdsAreHashes = true;
        }
        push(
            "block_registry",
            "Loaded minecraft-data blocks=" + std::to_string(blockCount) +
                " states=" + std::to_string(stateCount) +
                " version=" + version,
            "INFO",
            "world"
        );
    }

    void configureBlockRuntimeIds(bool hashed) {
        std::lock_guard lock(blockRegistryMutex);
        if (!blockRegistry.has_value() || blockRuntimeIdsAreHashes == hashed) {
            return;
        }
        blockRegistry->loadRuntimeIds(hashed);
        blockRuntimeIdsAreHashes = hashed;
        miniMapAppearances.clear();
    }

    void resetMiniMapWorld(int32_t dimension) noexcept {
        miniMapDimension.store(dimension, std::memory_order_relaxed);
        {
            std::lock_guard lock(miniMapMutex);
            ++miniMapGeneration;
            ++miniMapRevision;
            miniMapJobs.clear();
            miniMapTiles.clear();
        }
        {
            std::lock_guard lock(blockRegistryMutex);
            miniMapAppearances.clear();
        }
        miniMapCondition.notify_all();
    }

    void enqueueMiniMapChunk(
        const std::string& version,
        const bedrock::VersionedGamePacket& packet
    ) noexcept {
        if ((!miniMapEnabled.load(std::memory_order_relaxed) &&
             !schematicEnabled.load(std::memory_order_relaxed)) ||
            packet.name != "level_chunk") {
            return;
        }
        try {
            bedrock::VersionedPayloadCursor cursor(packet.payload);
            const int32_t chunkX = cursor.readVarInt();
            const int32_t chunkZ = cursor.readVarInt();
            const int32_t dimension = cursor.readVarInt();
            const auto camera = entityPositions.cameraSnapshot();
            int64_t distanceSquared = 0;
            if (camera.known) {
                const int32_t cameraChunkX = static_cast<int32_t>(
                    std::floor(camera.x / 16.0f)
                );
                const int32_t cameraChunkZ = static_cast<int32_t>(
                    std::floor(camera.z / 16.0f)
                );
                const int64_t dx = static_cast<int64_t>(chunkX) -
                    cameraChunkX;
                const int64_t dz = static_cast<int64_t>(chunkZ) -
                    cameraChunkZ;
                distanceSquared = dx * dx + dz * dz;
                if (dimension != miniMapDimension.load(
                        std::memory_order_relaxed
                    )) {
                    distanceSquared += 1'000'000;
                }
            }
            MiniMapChunkJob incoming {
                version,
                packet.payload,
                0,
                dimension,
                chunkX,
                chunkZ,
                distanceSquared
            };
            std::lock_guard lock(miniMapMutex);
            if (miniMapStopping) return;
            incoming.generation = miniMapGeneration;
            for (auto& queued : miniMapJobs) {
                if (queued.dimension == dimension && queued.x == chunkX &&
                    queued.z == chunkZ) {
                    queued = std::move(incoming);
                    miniMapCondition.notify_one();
                    return;
                }
            }
            constexpr std::size_t MaximumQueuedChunks = 96;
            if (miniMapJobs.size() < MaximumQueuedChunks) {
                miniMapJobs.push_back(std::move(incoming));
            } else {
                auto farthest = std::max_element(
                    miniMapJobs.begin(),
                    miniMapJobs.end(),
                    [](const auto& left, const auto& right) {
                        return left.cameraDistanceSquared <
                            right.cameraDistanceSquared;
                    }
                );
                if (farthest == miniMapJobs.end() ||
                    incoming.cameraDistanceSquared >=
                        farthest->cameraDistanceSquared) {
                    return;
                }
                *farthest = std::move(incoming);
            }
            miniMapCondition.notify_one();
        } catch (...) {
        }
    }

    void miniMapWorkerLoop() noexcept {
        for (;;) {
            MiniMapChunkJob job;
            {
                std::unique_lock lock(miniMapMutex);
                miniMapCondition.wait(lock, [this]() {
                    return miniMapStopping || !miniMapJobs.empty();
                });
                if (miniMapStopping) return;
                const auto nearest = std::min_element(
                    miniMapJobs.begin(),
                    miniMapJobs.end(),
                    [](const auto& left, const auto& right) {
                        return left.cameraDistanceSquared <
                            right.cameraDistanceSquared;
                    }
                );
                job = std::move(*nearest);
                miniMapJobs.erase(nearest);
            }

            try {
                auto packet = bedrock::BedrockLevelChunkCodec::decodePacketPayload(
                    job.payload
                );
                if (packet.cacheEnabled) {
                    miniMapCachedChunksSkipped.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                    continue;
                }
                auto column = bedrock::BedrockLevelChunkCodec::decodeNoCacheColumn(
                    packet,
                    versionAtLeast(job.version, 1, 18, 0)
                );
                MiniMapTile tile;
                tile.key = MiniMapKey {packet.dimension, packet.x, packet.z};
                tile.surfaceHeights.fill(UnknownSurfaceHeight);
                tile.groundHeights.fill(
                    std::numeric_limits<float>::quiet_NaN()
                );
                std::array<int32_t, 256> baseColors {};
                std::array<uint32_t, 256> biomes {};
                {
                    // StartGame may switch between numeric and hashed runtime
                    // IDs. Keep the registry mode stable for the whole tile.
                    std::lock_guard registryLock(blockRegistryMutex);
                    for (int32_t z = 0; z < 16; ++z) {
                        for (int32_t x = 0; x < 16; ++x) {
                            const auto offset = static_cast<std::size_t>(
                                z * 16 + x
                            );
                            bool surfaceFound = false;
                            bool groundFound = false;
                            int32_t surfaceY = column.minY();
                            for (int32_t sectionY = column.maxCY() - 1;
                                 sectionY >= column.minCY() &&
                                    (!surfaceFound || !groundFound);
                                 --sectionY) {
                                const auto* section =
                                    column.getSectionAtIndex(sectionY);
                                if (section == nullptr) continue;
                                for (int32_t localY = 15;
                                     localY >= 0 &&
                                        (!surfaceFound || !groundFound);
                                     --localY) {
                                    const auto runtimeId =
                                        section->getBlockStateId(
                                            static_cast<uint8_t>(x),
                                            static_cast<uint8_t>(localY),
                                            static_cast<uint8_t>(z)
                                        );
                                    const auto appearance =
                                        miniMapAppearanceLocked(
                                            runtimeId,
                                            packet.dimension
                                        );
                                    if (appearance.air) continue;
                                    const int32_t blockY =
                                        sectionY * 16 + localY;
                                    if (!surfaceFound) {
                                        surfaceFound = true;
                                        surfaceY = blockY;
                                        tile.surfaceHeights[offset] =
                                            static_cast<int16_t>(blockY);
                                        baseColors[offset] = appearance.color;
                                    }
                                    if (!groundFound && appearance.solid) {
                                        groundFound = true;
                                        tile.groundHeights[offset] =
                                            static_cast<float>(blockY) +
                                                appearance.collisionTop;
                                    }
                                }
                            }
                            if (!surfaceFound) continue;
                            try {
                                biomes[offset] = column.getBiomeId({
                                    x,
                                    surfaceY,
                                    z,
                                    std::nullopt
                                });
                            } catch (...) {
                            }
                        }
                    }
                }

                auto heightAt = [&tile](int x, int z, int fallback) {
                    x = std::clamp(x, 0, 15);
                    z = std::clamp(z, 0, 15);
                    const auto value = tile.surfaceHeights[
                        static_cast<std::size_t>(z * 16 + x)
                    ];
                    return value == UnknownSurfaceHeight
                        ? fallback
                        : static_cast<int>(value);
                };
                for (int32_t z = 0; z < 16; ++z) {
                    for (int32_t x = 0; x < 16; ++x) {
                        const auto offset = static_cast<std::size_t>(
                            z * 16 + x
                        );
                        const int height = tile.surfaceHeights[offset];
                        if (height == UnknownSurfaceHeight) {
                            tile.pixels[offset] = 0x00000000;
                            continue;
                        }
                        const int west = heightAt(x - 1, z, height);
                        const int east = heightAt(x + 1, z, height);
                        const int north = heightAt(x, z - 1, height);
                        const int south = heightAt(x, z + 1, height);
                        int shade = std::clamp(
                            (west + north - east - south) * 5 +
                                (height - 64) / 20,
                            -42,
                            38
                        );
                        int contour = height % 8;
                        if (contour < 0) contour += 8;
                        if (contour == 0) shade -= 6;
                        tile.pixels[offset] = shadeMapColor(
                            baseColors[offset],
                            shade,
                            biomes[offset]
                        );
                    }
                }

                {
                    std::lock_guard lock(miniMapMutex);
                    if (job.generation != miniMapGeneration || miniMapStopping) {
                        continue;
                    }
                    tile.revision = ++miniMapRevision;
                    miniMapDimension.store(
                        packet.dimension,
                        std::memory_order_relaxed
                    );
                    miniMapTiles.insert_or_assign(tile.key, std::move(tile));
                    while (miniMapTiles.size() > 2048) {
                        auto oldest = miniMapTiles.begin();
                        for (auto current = miniMapTiles.begin();
                             current != miniMapTiles.end(); ++current) {
                            if (current->second.revision <
                                oldest->second.revision) {
                                oldest = current;
                            }
                        }
                        miniMapTiles.erase(oldest);
                    }
                }
                miniMapDecodedChunks.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                miniMapDecodeFailures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    std::vector<int32_t> miniMapSnapshotValues(
        uint64_t afterRevision,
        int radiusChunks
    ) const {
        const auto camera = entityPositions.cameraSnapshot();
        const int clampedRadius = std::clamp(radiusChunks, 1, 12);
        const int32_t cameraChunkX = static_cast<int32_t>(
            std::floor(camera.x / 16.0f)
        );
        const int32_t cameraChunkZ = static_cast<int32_t>(
            std::floor(camera.z / 16.0f)
        );
        const int32_t dimension = miniMapDimension.load(
            std::memory_order_relaxed
        );

        std::vector<const MiniMapTile*> selected;
        uint64_t revision = 0;
        uint64_t generation = 0;
        {
            std::lock_guard lock(miniMapMutex);
            revision = miniMapRevision;
            generation = miniMapGeneration;
            selected.reserve(miniMapTiles.size());
            for (const auto& [key, tile] : miniMapTiles) {
                if (key.dimension != dimension ||
                    std::abs(key.x - cameraChunkX) > clampedRadius ||
                    std::abs(key.z - cameraChunkZ) > clampedRadius ||
                    (afterRevision != 0 && tile.revision <= afterRevision)) {
                    continue;
                }
                selected.push_back(&tile);
            }

            std::vector<int32_t> result;
            result.reserve(12 + selected.size() * 258);
            result.push_back(0x4350454d); // CPEM
            result.push_back(1);
            result.push_back(camera.known ? 1 : 0);
            result.push_back(floatBits(camera.x));
            result.push_back(floatBits(camera.y));
            result.push_back(floatBits(camera.z));
            result.push_back(floatBits(camera.yaw));
            result.push_back(dimension);
            result.push_back(static_cast<int32_t>(revision & 0xffffffffu));
            result.push_back(static_cast<int32_t>(revision >> 32u));
            result.push_back(static_cast<int32_t>(generation & 0xffffffffu));
            result.push_back(static_cast<int32_t>(selected.size()));
            for (const auto* tile : selected) {
                result.push_back(tile->key.x);
                result.push_back(tile->key.z);
                result.insert(
                    result.end(),
                    tile->pixels.begin(),
                    tile->pixels.end()
                );
            }
            return result;
        }
    }

    float worldSurfaceY(int32_t worldX, int32_t worldZ) const noexcept {
        auto chunkCoordinate = [](int32_t value) {
            int32_t chunk = value / 16;
            if (value < 0 && value % 16 != 0) --chunk;
            return chunk;
        };
        const int32_t chunkX = chunkCoordinate(worldX);
        const int32_t chunkZ = chunkCoordinate(worldZ);
        const int32_t localX = worldX - chunkX * 16;
        const int32_t localZ = worldZ - chunkZ * 16;
        const int32_t dimension = miniMapDimension.load(
            std::memory_order_relaxed
        );
        std::lock_guard lock(miniMapMutex);
        const auto found = miniMapTiles.find(MiniMapKey {
            dimension,
            chunkX,
            chunkZ
        });
        if (found == miniMapTiles.end()) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        const auto height = found->second.groundHeights[
            static_cast<std::size_t>(localZ * 16 + localX)
        ];
        return std::isfinite(height)
            ? height
            : std::numeric_limits<float>::quiet_NaN();
    }

    static void refreshDurability(
        EquipmentItem& item,
        std::string_view version
    ) noexcept {
        item.maximumDurability = bedrock::maximumItemDurability(
            version,
            item.name
        ).value_or(0);
        item.remainingDurability = 0;
        item.durabilityPercent = 0;
        if (!item.damageKnown) return;
        const auto durability = bedrock::calculateItemDurability(
            version,
            item.name,
            item.damage
        );
        if (!durability.has_value()) return;
        item.damage = durability->damage;
        item.maximumDurability = durability->maximum;
        item.remainingDurability = durability->remaining;
        item.durabilityPercent = durability->percent;
    }

    EquipmentItem decodedItem(
        std::string_view version,
        const bedrock::RelayPacketEvent& decoded,
        const std::string& prefix
    ) {
        EquipmentItem item;
        std::string itemPrefix = prefix;
        item.networkId = decoded.getInt(prefix + ".network_id", 0);
        if (item.networkId == 0 &&
            decoded.has(prefix + ".item.network_id")) {
            itemPrefix += ".item";
            item.networkId = decoded.getInt(itemPrefix + ".network_id", 0);
        }
        item.present = item.networkId != 0;
        if (!item.present) return item;
        item.count = static_cast<int32_t>(std::max<int64_t>(
            1,
            decoded.getInt(
                itemPrefix + ".count",
                decoded.getInt(prefix + ".count", 1)
            )
        ));
        item.stackId = static_cast<int32_t>(decoded.getInt(
            prefix + ".stack_id",
            decoded.getInt(
                prefix + ".runtime_id",
                decoded.getInt(itemPrefix + ".stack_id", 0)
            )
        ));
        auto nbtDamage = itemNbtInteger(decoded, itemPrefix, "Damage");
        if (!nbtDamage.has_value()) {
            nbtDamage = itemNbtInteger(decoded, itemPrefix, "damage");
        }
        item.damageKnown = nbtDamage.has_value();
        item.damage = std::max(0, nbtDamage.value_or(0));
        item.enchanted = packetValueHasContent(
            decoded.value(itemPrefix + ".extra.nbt.nbt.value.ench")
        );
        const auto* itemValue = decoded.value(itemPrefix);
        if (!item.enchanted && itemValue != nullptr) {
            item.enchanted = packetValueHasContent(
                findNamedPacketValue(*itemValue, "ench")
            );
        }
        {
            std::lock_guard lock(mutex);
            const auto known = itemNames.find(item.networkId);
            item.name = known == itemNames.end()
                ? "minecraft:item_" + std::to_string(item.networkId)
                : known->second;
        }
        refreshDurability(item, version);
        return item;
    }

    void observeItemRegistry(
        const std::string& version,
        const bedrock::VersionedGamePacket& packet
    ) noexcept {
        try {
            std::vector<std::pair<int64_t, std::string>> palette;
            {
                std::lock_guard decodeLock(itemDecodeMutex);
                palette = bedrock::ProtoDefPacketDecoder(
                    version,
                    itemProtocolVariables
                ).decodeItemPaletteStrict(packet.name, packet.payload);
            }
            std::lock_guard lock(mutex);
            itemNames.clear();
            itemNames.reserve(palette.size());
            for (auto& [runtimeId, name] : palette) {
                itemNames.insert_or_assign(runtimeId, std::move(name));
            }
            bool equipmentChanged = false;
            for (auto& item : equipment) {
                if (!item.present) continue;
                const auto known = itemNames.find(item.networkId);
                if (known == itemNames.end()) continue;
                item.name = known->second;
                refreshDurability(item, version);
                equipmentChanged = true;
            }
            for (auto& item : playerInventory) {
                if (!item.present) continue;
                const auto known = itemNames.find(item.networkId);
                if (known == itemNames.end()) continue;
                item.name = known->second;
                refreshDurability(item, version);
            }
            if (equipmentChanged) ++equipmentRevision;
        } catch (...) {
        }
    }

    static std::string decodedWindowId(
        const bedrock::RelayPacketEvent& decoded
    ) {
        auto value = decoded.getString("window_id", "");
        if (value.empty()) value = decoded.getString("inventory_id", "");
        return value;
    }

    struct AutomationPlan {
        bool valid = false;
        bool totem = false;
        std::size_t inventorySlot = 0;
        std::size_t equipmentIndex = 0;
        uint8_t equipmentSlot = 0;
        int32_t requestId = 0;
        EquipmentItem source;
        EquipmentItem destination;
    };

    static int armorType(std::string_view name) noexcept {
        if (name.find("helmet") != std::string_view::npos) return 0;
        if (name.find("chestplate") != std::string_view::npos) return 1;
        if (name.find("leggings") != std::string_view::npos) return 2;
        if (name.find("boots") != std::string_view::npos) return 3;
        return -1;
    }

    static int armorScore(const EquipmentItem& item) noexcept {
        if (!item.present) return -1;
        int material = -1;
        if (item.name.find("netherite") != std::string::npos) material = 5;
        else if (item.name.find("diamond") != std::string::npos) material = 4;
        else if (item.name.find("turtle") != std::string::npos) material = 3;
        else if (item.name.find("iron") != std::string::npos) material = 3;
        else if (item.name.find("chainmail") != std::string::npos) material = 2;
        else if (item.name.find("gold") != std::string::npos) material = 1;
        else if (item.name.find("leather") != std::string::npos) material = 0;
        if (material < 0) return -1;
        const int durability = item.damageKnown
            ? item.durabilityPercent
            : 100;
        return material * 100 + std::clamp(durability, 0, 100);
    }

    AutomationPlan chooseAutomationPlanLocked() {
        AutomationPlan plan;
        if (!playerInventoryReady || playerInventory.empty()) return plan;

        if (autoTotemEnabled.load(std::memory_order_relaxed) &&
            equipment[1].name != "minecraft:totem_of_undying") {
            for (std::size_t slot = 0; slot < playerInventory.size(); ++slot) {
                const auto& item = playerInventory[slot];
                if (item.name != "minecraft:totem_of_undying" ||
                    item.stackId == 0) {
                    continue;
                }
                plan.valid = true;
                plan.totem = true;
                plan.inventorySlot = slot;
                plan.equipmentIndex = 1;
                plan.equipmentSlot = 0;
                plan.source = item;
                plan.destination = equipment[1];
                return plan;
            }
        }

        if (!autoArmorEnabled.load(std::memory_order_relaxed)) return plan;
        int bestGain = 0;
        int bestScore = -1;
        for (std::size_t slot = 0; slot < playerInventory.size(); ++slot) {
            const auto& item = playerInventory[slot];
            if (!item.present || item.stackId == 0) continue;
            const int type = armorType(item.name);
            if (type < 0) continue;
            const auto equipmentIndex = static_cast<std::size_t>(type + 2);
            const int candidateScore = armorScore(item);
            const int currentScore = armorScore(equipment[equipmentIndex]);
            const int gain = candidateScore - currentScore;
            if (candidateScore < 0 || gain <= 0) continue;
            if (!plan.valid || gain > bestGain ||
                (gain == bestGain && candidateScore > bestScore)) {
                plan.valid = true;
                plan.totem = false;
                plan.inventorySlot = slot;
                plan.equipmentIndex = equipmentIndex;
                plan.equipmentSlot = static_cast<uint8_t>(type);
                plan.source = item;
                plan.destination = equipment[equipmentIndex];
                bestGain = gain;
                bestScore = candidateScore;
            }
        }
        return plan;
    }

    static int legacyContainerSlotId(std::string_view type) noexcept {
        if (type == "armor") return 6;
        if (type == "offhand") return 34;
        return 12; // hotbar_and_inventory
    }

    static bedrock::ProtoDefValue stackRequestSlot(
        const std::string& version,
        std::string type,
        uint8_t slot,
        int32_t stackId
    ) {
        using Value = bedrock::ProtoDefValue;
        if (!versionAtLeast(version, 1, 16, 210)) {
            return Value::object({
                {"container_id", Value::integer(legacyContainerSlotId(type))},
                {"slot_id", Value::uinteger(slot)},
                {"stack_id", Value::integer(stackId)}
            });
        }
        if (versionAtLeast(version, 1, 21, 20)) {
            return Value::object({
                {"slot_type", Value::object({
                    {"container_id", Value::string(std::move(type))},
                    {"dynamic_container_id", Value::uinteger(0)}
                })},
                {"slot", Value::uinteger(slot)},
                {"stack_id", Value::integer(stackId)}
            });
        }
        return Value::object({
            {"slot_type", Value::string(std::move(type))},
            {"slot", Value::uinteger(slot)},
            {"stack_id", Value::integer(stackId)}
        });
    }

    bedrock::VersionedGamePacket makeAutomationPacket(
        const std::string& version,
        const AutomationPlan& plan
    ) {
        using Value = bedrock::ProtoDefValue;
        const std::string destinationType = plan.totem
            ? "offhand"
            : "armor";
        auto request = Value::object({
            {"request_id", Value::integer(plan.requestId)},
            {"actions", Value::array({Value::object({
                {"type_id", Value::string("swap")},
                {"source", stackRequestSlot(
                    version,
                    "hotbar_and_inventory",
                    static_cast<uint8_t>(plan.inventorySlot),
                    plan.source.stackId
                )},
                {"destination", stackRequestSlot(
                    version,
                    destinationType,
                    plan.equipmentSlot,
                    plan.destination.stackId
                )}
            })})},
            {"custom_names", Value::array({})},
            {"cause", Value::string("chat_public")}
        });
        auto payload = bedrock::ProtoDefPacketEncoder(
            version,
            itemProtocolVariables
        ).encodePacket("item_stack_request", Value::object({
            {"requests", Value::array({std::move(request)})}
        }));
        return bedrock::VersionedMcpeCodec::forVersion(version)
            .packetCodec()
            .makePacketByName("item_stack_request", payload);
    }

    void maybeInjectAutomation(
        const std::string& version,
        bedrock::BedrockRelayPacketEvent& event
    ) noexcept {
        if (event.packet.name != "player_auth_input" &&
            event.packet.name != "move_player") {
            return;
        }
        if ((!autoArmorEnabled.load(std::memory_order_relaxed) &&
             !autoTotemEnabled.load(std::memory_order_relaxed)) ||
            minecraftUiBlocked.load(std::memory_order_relaxed)) {
            return;
        }

        const uint64_t now = steadyMilliseconds();
        AutomationPlan plan;
        {
            std::lock_guard lock(mutex);
            if (pendingAutomationRequestId != 0) {
                if (now - pendingAutomationStartedAtMs < 1'500) return;
                pendingAutomationRequestId = 0;
                playerInventoryReady = false;
                automationStatus = "Нет ответа сервера — ожидается синхронизация";
                return;
            }
            if (now - lastAutomationAttemptAtMs < 500) return;
            plan = chooseAutomationPlanLocked();
            if (!plan.valid) return;
            plan.requestId = nextAutomationRequestId++;
        }

        bedrock::VersionedGamePacket injected;
        try {
            std::lock_guard decodeLock(itemDecodeMutex);
            injected = makeAutomationPacket(version, plan);
        } catch (const std::exception& error) {
            {
                std::lock_guard lock(mutex);
                lastAutomationAttemptAtMs = now;
                automationStatus =
                    "Пакет автоматизации не поддержан этой версией";
            }
            push(
                "automation_encode_failed",
                "version=" + version + " error=" + safeMessage(error.what()),
                "WARN",
                "automation"
            );
            return;
        }

        {
            std::lock_guard lock(mutex);
            if (plan.inventorySlot >= playerInventory.size() ||
                playerInventory[plan.inventorySlot].stackId !=
                    plan.source.stackId) {
                return;
            }
            playerInventory[plan.inventorySlot] = plan.destination;
            equipment[plan.equipmentIndex] = plan.source;
            ++equipmentRevision;
            pendingAutomationRequestId = plan.requestId;
            pendingAutomationStartedAtMs = now;
            lastAutomationAttemptAtMs = now;
            automationStatus = plan.totem
                ? "Тотем перемещается в левую руку"
                : "Надевается " + plan.source.name;
        }
        event.replace(std::vector<bedrock::VersionedGamePacket> {
            event.packet,
            std::move(injected)
        });
    }

    void observeDecodedGameplayPacket(
        const std::string& version,
        bedrock::BedrockRelayPacketEvent& event,
        bool serverbound
    ) noexcept {
        const auto& name = event.packet.name;
        if ((!serverbound && name == "container_open") ||
            (serverbound && name == "set_player_inventory_options")) {
            minecraftUiBlocked.store(true, std::memory_order_relaxed);
        } else if (name == "container_close") {
            minecraftUiBlocked.store(false, std::memory_order_relaxed);
        }
        if (!serverbound && name == "item_registry") {
            observeItemRegistry(version, event.packet);
            return;
        }
        if (!serverbound && name == "start_game") {
            minecraftUiBlocked.store(false, std::memory_order_relaxed);
            try {
                std::lock_guard decodeLock(itemDecodeMutex);
                bedrock::RelayPacketEvent decoded(version, event);
                configureBlockRuntimeIds(decoded.getBool(
                    "block_network_ids_are_hashes",
                    true
                ));
            } catch (...) {
            }
            resetMiniMapWorld(0);
            std::lock_guard lock(mutex);
            equipment = {};
            playerInventory.clear();
            playerInventoryReady = false;
            pendingAutomationRequestId = 0;
            automationStatus = "Ожидание инвентаря";
            playerHealth = 20.0;
            playerMaximumHealth = 20.0;
            playerHunger = 20.0;
            playerSaturation = 5.0;
            playerAbsorption = 0.0;
            playerHealthKnown = false;
            playerHungerKnown = false;
            playerAbsorptionKnown = false;
            playerResistanceLevel = 0;
            ++equipmentRevision;
            return;
        }
        if (!serverbound && name == "change_dimension") {
            try {
                bedrock::VersionedPayloadCursor cursor(event.packet.payload);
                resetMiniMapWorld(cursor.readVarInt());
            } catch (...) {
                resetMiniMapWorld(
                    miniMapDimension.load(std::memory_order_relaxed)
                );
            }
        }
        if (serverbound && name == "interact") {
            try {
                std::lock_guard decodeLock(itemDecodeMutex);
                bedrock::RelayPacketEvent decoded(
                    version,
                    event,
                    itemProtocolVariables,
                    true
                );
                if (decoded.getString("action_id", "") ==
                    "open_inventory") {
                    minecraftUiBlocked.store(true, std::memory_order_relaxed);
                }
            } catch (...) {
            }
            return;
        }
        const bool relevant = name == "add_item_entity" ||
            name == "mob_equipment" ||
            name == "mob_armor_equipment" ||
            name == "inventory_content" ||
            name == "inventory_slot" ||
            name == "item_stack_response" ||
            name == "player_armor_damage" ||
            name == "set_health" ||
            name == "update_attributes" ||
            name == "mob_effect";
        if (!relevant) return;

        try {
            std::lock_guard decodeLock(itemDecodeMutex);
            bedrock::RelayPacketEvent decoded(
                version,
                event,
                itemProtocolVariables,
                true
            );
            if (!serverbound && name == "set_health") {
                std::lock_guard lock(mutex);
                playerHealth = std::max(
                    0.0,
                    decoded.getDouble("health", playerHealth)
                );
                playerHealthKnown = true;
                return;
            }
            if (!serverbound && name == "update_attributes") {
                const auto camera = entityPositions.cameraSnapshot();
                const auto runtimeId = decoded.getUInt(
                    "runtime_entity_id",
                    0
                );
                if (camera.runtimeId != 0 && runtimeId != camera.runtimeId) {
                    return;
                }
                const auto* attributes = decoded.value("attributes");
                if (attributes == nullptr ||
                    attributes->kind != bedrock::PacketValue::Kind::Array) {
                    return;
                }
                std::lock_guard lock(mutex);
                for (std::size_t index = 0;
                     index < attributes->arrayValue.size(); ++index) {
                    const auto prefix = "attributes[" +
                        std::to_string(index) + "]";
                    auto attributeName = decoded.getString(
                        prefix + ".name",
                        ""
                    );
                    std::transform(
                        attributeName.begin(),
                        attributeName.end(),
                        attributeName.begin(),
                        [](unsigned char value) {
                            return static_cast<char>(std::tolower(value));
                        }
                    );
                    const auto current = decoded.getDouble(
                        prefix + ".current",
                        0.0
                    );
                    const auto maximum = decoded.getDouble(
                        prefix + ".max",
                        current
                    );
                    if (attributeName.find("health") != std::string::npos &&
                        attributeName.find("absorption") ==
                            std::string::npos) {
                        playerHealth = std::max(0.0, current);
                        playerMaximumHealth = std::max(1.0, maximum);
                        playerHealthKnown = true;
                    } else if (attributeName.find("hunger") !=
                        std::string::npos) {
                        playerHunger = std::clamp(current, 0.0, 20.0);
                        playerHungerKnown = true;
                    } else if (attributeName.find("saturation") !=
                        std::string::npos) {
                        playerSaturation = std::max(0.0, current);
                        playerHungerKnown = true;
                    } else if (attributeName.find("absorption") !=
                        std::string::npos) {
                        playerAbsorption = std::max(0.0, current);
                        playerAbsorptionKnown = true;
                    }
                }
                return;
            }
            if (!serverbound && name == "mob_effect") {
                const auto camera = entityPositions.cameraSnapshot();
                const auto runtimeId = decoded.getUInt(
                    "runtime_entity_id",
                    0
                );
                if (camera.runtimeId != 0 && runtimeId != camera.runtimeId) {
                    return;
                }
                // Bedrock effect id 11 is Resistance on every supported
                // protocol generation. Amplifier 0 means Resistance I.
                if (decoded.getInt("effect_id", -1) == 11) {
                    const auto action = decoded.getString("event_id", "");
                    std::lock_guard lock(mutex);
                    playerResistanceLevel = action == "remove" ||
                            decoded.getInt("event_id", 0) == 3
                        ? 0
                        : std::max(
                            1,
                            static_cast<int32_t>(
                                decoded.getInt("amplifier", 0) + 1
                            )
                        );
                }
                return;
            }
            if (!serverbound && name == "item_stack_response") {
                const auto* responses = decoded.value("responses");
                if (responses == nullptr ||
                    responses->kind != bedrock::PacketValue::Kind::Array) {
                    return;
                }
                std::lock_guard lock(mutex);
                for (std::size_t index = 0;
                     index < responses->arrayValue.size(); ++index) {
                    const std::string prefix =
                        "responses[" + std::to_string(index) + "]";
                    const auto requestId = static_cast<int32_t>(
                        decoded.getInt(prefix + ".request_id", 0)
                    );
                    if (requestId != pendingAutomationRequestId) continue;
                    const auto status = decoded.getString(
                        prefix + ".status",
                        decoded.getString(prefix + ".result", "")
                    );
                    const bool accepted = status == "ok" ||
                        decoded.getInt(prefix + ".status", 1) == 0 ||
                        decoded.getInt(prefix + ".result", 1) == 0;
                    pendingAutomationRequestId = 0;
                    if (accepted) {
                        ++automationAccepted;
                        automationStatus = "Последнее действие подтверждено";
                    } else {
                        ++automationRejected;
                        playerInventoryReady = false;
                        automationStatus =
                            "Сервер отклонил действие — ожидается синхронизация";
                    }
                    break;
                }
                return;
            }
            if (!serverbound && name == "add_item_entity") {
                const auto networkId = decoded.getInt("item.network_id", 0);
                const auto runtimeId = decoded.getUInt(
                    "runtime_entity_id",
                    0
                );
                if (networkId == 0 || runtimeId == 0) return;
                std::string label;
                {
                    std::lock_guard lock(mutex);
                    const auto known = itemNames.find(networkId);
                    label = known == itemNames.end()
                        ? "Предмет"
                        : known->second;
                }
                entityPositions.observeDecodedItemEntity(
                    decoded.getInt("entity_id_self", 0),
                    runtimeId,
                    std::move(label),
                    static_cast<float>(decoded.getDouble("position.x", 0.0)),
                    static_cast<float>(decoded.getDouble("position.y", 0.0)),
                    static_cast<float>(decoded.getDouble("position.z", 0.0))
                );
                return;
            }

            const auto camera = entityPositions.cameraSnapshot();
            if (name == "mob_equipment") {
                const auto runtimeId = decoded.getUInt(
                    "runtime_entity_id",
                    0
                );
                if (camera.runtimeId != 0 && runtimeId != camera.runtimeId) {
                    return;
                }
                auto hand = decodedItem(version, decoded, "item");
                const auto equipmentIndex =
                    decoded.getString("window_id", "") == "offhand"
                        ? std::size_t {1}
                        : std::size_t {0};
                std::lock_guard lock(mutex);
                equipment[equipmentIndex] = std::move(hand);
                ++equipmentRevision;
                return;
            }
            if (!serverbound && name == "mob_armor_equipment") {
                const auto runtimeId = decoded.getUInt(
                    "runtime_entity_id",
                    0
                );
                if (camera.runtimeId != 0 && runtimeId != camera.runtimeId) {
                    return;
                }
                std::array<EquipmentItem, 4> armor {
                    decodedItem(version, decoded, "helmet"),
                    decodedItem(version, decoded, "chestplate"),
                    decodedItem(version, decoded, "leggings"),
                    decodedItem(version, decoded, "boots")
                };
                std::lock_guard lock(mutex);
                for (std::size_t index = 0; index < armor.size(); ++index) {
                    equipment[index + 2] = std::move(armor[index]);
                }
                ++equipmentRevision;
                return;
            }
            const auto windowId = decodedWindowId(decoded);
            if (!serverbound && name == "inventory_content" &&
                windowId == "inventory") {
                const auto* items = decoded.value("input");
                if (items == nullptr ||
                    items->kind != bedrock::PacketValue::Kind::Array) {
                    return;
                }
                std::vector<EquipmentItem> inventory;
                inventory.reserve(items->arrayValue.size());
                for (std::size_t index = 0;
                     index < items->arrayValue.size(); ++index) {
                    inventory.push_back(decodedItem(
                        version,
                        decoded,
                        "input[" + std::to_string(index) + "]"
                    ));
                }
                std::lock_guard lock(mutex);
                playerInventory = std::move(inventory);
                playerInventoryReady = true;
                if (pendingAutomationRequestId == 0) {
                    automationStatus = "Инвентарь синхронизирован";
                }
                return;
            }
            if (!serverbound && name == "inventory_content" &&
                windowId == "offhand") {
                const auto* items = decoded.value("input");
                auto offhand = items != nullptr &&
                        items->kind == bedrock::PacketValue::Kind::Array &&
                        !items->arrayValue.empty()
                    ? decodedItem(version, decoded, "input[0]")
                    : EquipmentItem {};
                std::lock_guard lock(mutex);
                equipment[1] = std::move(offhand);
                ++equipmentRevision;
                return;
            }
            if (!serverbound && name == "inventory_content" &&
                windowId == "armor") {
                const auto* items = decoded.value("input");
                if (items == nullptr ||
                    items->kind != bedrock::PacketValue::Kind::Array) {
                    return;
                }
                std::array<EquipmentItem, 4> armor;
                for (std::size_t index = 0; index < armor.size(); ++index) {
                    armor[index] = index < items->arrayValue.size()
                        ? decodedItem(
                            version,
                            decoded,
                            "input[" + std::to_string(index) + "]"
                        )
                        : EquipmentItem {};
                }
                std::lock_guard lock(mutex);
                for (std::size_t index = 0; index < armor.size(); ++index) {
                    equipment[index + 2] = std::move(armor[index]);
                }
                ++equipmentRevision;
                return;
            }
            if (!serverbound && name == "inventory_slot" &&
                windowId == "inventory") {
                const auto slot = decoded.getUInt("slot", 999);
                auto item = decodedItem(version, decoded, "item");
                std::lock_guard lock(mutex);
                if (slot >= playerInventory.size()) {
                    playerInventory.resize(static_cast<std::size_t>(slot) + 1);
                }
                playerInventory[static_cast<std::size_t>(slot)] =
                    std::move(item);
                playerInventoryReady = true;
                return;
            }
            if (!serverbound && name == "inventory_slot" &&
                windowId == "offhand") {
                auto offhand = decodedItem(version, decoded, "item");
                std::lock_guard lock(mutex);
                equipment[1] = std::move(offhand);
                ++equipmentRevision;
                return;
            }
            if (!serverbound && name == "inventory_slot" &&
                windowId == "armor") {
                const auto slot = decoded.getUInt("slot", 99);
                if (slot < 4) {
                    auto armor = decodedItem(version, decoded, "item");
                    std::lock_guard lock(mutex);
                    equipment[static_cast<std::size_t>(slot) + 2] =
                        std::move(armor);
                    ++equipmentRevision;
                }
                return;
            }
            if (!serverbound && name == "player_armor_damage") {
                static constexpr std::array<const char*, 4> DamageFields {
                    "helmet_damage",
                    "chestplate_damage",
                    "leggings_damage",
                    "boots_damage"
                };
                std::lock_guard lock(mutex);
                for (std::size_t index = 0; index < DamageFields.size(); ++index) {
                    if (decoded.has(DamageFields[index]) &&
                        equipment[index + 2].present &&
                        equipment[index + 2].damageKnown) {
                        equipment[index + 2].damage = std::max(
                            0,
                            equipment[index + 2].damage + static_cast<int32_t>(
                                decoded.getInt(DamageFields[index], 0)
                            )
                        );
                        refreshDurability(equipment[index + 2], version);
                    }
                }
                ++equipmentRevision;
            }
        } catch (...) {
        }
    }

    void clearGameplayTelemetry() {
        minecraftUiBlocked.store(false, std::memory_order_relaxed);
        resetMiniMapWorld(0);
        std::lock_guard lock(mutex);
        equipment = {};
        playerInventory.clear();
        playerInventoryReady = false;
        pendingAutomationRequestId = 0;
        automationStatus = "Ожидание инвентаря";
        playerHealth = 20.0;
        playerMaximumHealth = 20.0;
        playerHunger = 20.0;
        playerSaturation = 5.0;
        playerAbsorption = 0.0;
        playerHealthKnown = false;
        playerHungerKnown = false;
        playerAbsorptionKnown = false;
        playerResistanceLevel = 0;
        itemNames.clear();
        ++equipmentRevision;
    }

    void configureGameplayFeatures(
        bool armor,
        bool totem,
        bool miniMap,
        bool schematic
    ) {
        autoArmorEnabled.store(armor, std::memory_order_relaxed);
        autoTotemEnabled.store(totem, std::memory_order_relaxed);
        miniMapEnabled.store(miniMap, std::memory_order_relaxed);
        schematicEnabled.store(schematic, std::memory_order_relaxed);
        if (!armor && !totem) {
            std::lock_guard lock(mutex);
            pendingAutomationRequestId = 0;
            automationStatus = "Автоматизация выключена";
        }
    }

    void configureRuntime(
        bool detailed,
        bool retainChunks,
        int radiusChunks
    ) {
        detailedLogging.store(detailed, std::memory_order_relaxed);
        const bool wasRetaining = chunkRetentionEnabled.exchange(
            retainChunks,
            std::memory_order_relaxed
        );
        const int clampedRadius = clampRetainedRadiusChunks(radiusChunks);
        const int previousRadius = retainedRadiusChunks.exchange(
            clampedRadius,
            std::memory_order_relaxed
        );
        if (retainChunks != wasRetaining ||
            (retainChunks && clampedRadius != previousRadius)) {
            chunkPublisherPacketsObserved.store(0, std::memory_order_relaxed);
            chunkPublisherPacketsRewritten.store(0, std::memory_order_relaxed);
            chunkPublisherDecodeFailures.store(0, std::memory_order_relaxed);
            lastServerPublisherRadiusBlocks.store(
                0,
                std::memory_order_relaxed
            );
            lastEffectivePublisherRadiusBlocks.store(
                0,
                std::memory_order_relaxed
            );
        }
        if (!retainChunks) {
            retainedLevelChunkCount.store(0, std::memory_order_relaxed);
            retainedLevelChunkBytes.store(0, std::memory_order_relaxed);
        }
        if (!detailed) {
            std::lock_guard lock(mutex);
            events.clear();
            flight.clear();
            lastFlushedFlightSequence = flightSequence;
        }
    }

    void updateLevelChunkRetentionStats(
        const bedrock::LevelChunkRetentionStats& stats
    ) noexcept {
        retainedLevelChunkCount.store(
            stats.residentChunks,
            std::memory_order_relaxed
        );
        retainedLevelChunkBytes.store(
            stats.residentBytes,
            std::memory_order_relaxed
        );
        retainedLevelChunkMaximumBytes.store(
            stats.maximumBytes,
            std::memory_order_relaxed
        );
        retainedLevelChunksStored.store(
            stats.storedLevelChunks,
            std::memory_order_relaxed
        );
        retainedLevelChunksReplaced.store(
            stats.replacedLevelChunks,
            std::memory_order_relaxed
        );
        retainedLevelChunksEvictedRadius.store(
            stats.evictedOutsideRadius,
            std::memory_order_relaxed
        );
        retainedLevelChunksEvictedMemory.store(
            stats.evictedForMemory,
            std::memory_order_relaxed
        );
        retainedLevelChunkParseFailures.store(
            stats.parseFailures,
            std::memory_order_relaxed
        );
    }

    void push(bedrock::JsRuntimeValue event) {
        if (!detailedLogging.load(std::memory_order_relaxed)) return;
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
        if (!detailedLogging.load(std::memory_order_relaxed)) return;
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

    void flushFlight(
        std::string reason,
        std::size_t maximumRecords = 768
    ) {
        if (!detailedLogging.load(std::memory_order_relaxed)) return;
        std::vector<FlightRecord> snapshot;
        uint64_t firstSequence = 0;
        uint64_t lastSequence = 0;
        std::size_t omittedRecords = 0;
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
            const auto availableRecords = static_cast<std::size_t>(
                flight.end() - firstUnflushed
            );
            const auto selectedRecords = std::min(
                availableRecords,
                std::max<std::size_t>(1, maximumRecords)
            );
            omittedRecords = availableRecords - selectedRecords;
            const auto firstSelected = firstUnflushed +
                static_cast<std::ptrdiff_t>(omittedRecords);
            snapshot.assign(firstSelected, flight.end());
            firstSequence = snapshot.front().sequence;
            lastSequence = snapshot.back().sequence;
            lastFlushedFlightSequence = flightSequence;
        }

        push(
            "flight_dump",
            "reason=" + reason + " records=" +
                std::to_string(snapshot.size()) + " sequence=" +
                std::to_string(firstSequence) + ".." +
                std::to_string(lastSequence) + " omittedOlder=" +
                std::to_string(omittedRecords),
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
    static constexpr std::array<std::string_view, 6> EquipmentSlots {
        "hand", "offhand", "helmet", "chestplate", "leggings", "boots"
    };
    std::vector<bedrock::JsRuntimeValue> equipment;
    equipment.reserve(state->equipment.size());
    for (std::size_t index = 0; index < state->equipment.size(); ++index) {
        const auto& item = state->equipment[index];
        equipment.push_back(bedrock::JsRuntimeValue::object({
            {"slot", bedrock::JsRuntimeValue::string(
                std::string(EquipmentSlots[index])
            )},
            {"present", bedrock::JsRuntimeValue::boolean(item.present)},
            {"networkId", bedrock::JsRuntimeValue::number(
                static_cast<double>(item.networkId)
            )},
            {"count", bedrock::JsRuntimeValue::number(item.count)},
            {"stackId", bedrock::JsRuntimeValue::number(item.stackId)},
            {"name", bedrock::JsRuntimeValue::string(item.name)},
            {"damage", bedrock::JsRuntimeValue::number(item.damage)},
            {"damageKnown", bedrock::JsRuntimeValue::boolean(
                item.damageKnown
            )},
            {"maximumDurability", bedrock::JsRuntimeValue::number(
                item.maximumDurability
            )},
            {"remainingDurability", bedrock::JsRuntimeValue::number(
                item.remainingDurability
            )},
            {"durabilityPercent", bedrock::JsRuntimeValue::number(
                item.durabilityPercent
            )},
            {"enchanted", bedrock::JsRuntimeValue::boolean(item.enchanted)}
        }));
    }
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
        {"detailedLogging", bedrock::JsRuntimeValue::boolean(
            state->detailedLogging.load(std::memory_order_relaxed)
        )},
        {"chunkRetentionEnabled", bedrock::JsRuntimeValue::boolean(
            state->chunkRetentionEnabled.load(std::memory_order_relaxed)
        )},
        {"minecraftUiBlocked", bedrock::JsRuntimeValue::boolean(
            state->minecraftUiBlocked.load(std::memory_order_relaxed)
        )},
        {"autoArmorEnabled", bedrock::JsRuntimeValue::boolean(
            state->autoArmorEnabled.load(std::memory_order_relaxed)
        )},
        {"autoTotemEnabled", bedrock::JsRuntimeValue::boolean(
            state->autoTotemEnabled.load(std::memory_order_relaxed)
        )},
        {"miniMapEnabled", bedrock::JsRuntimeValue::boolean(
            state->miniMapEnabled.load(std::memory_order_relaxed)
        )},
        {"miniMapDecodedChunks", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->miniMapDecodedChunks.load(
                std::memory_order_relaxed
            ))
        )},
        {"miniMapDecodeFailures", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->miniMapDecodeFailures.load(
                std::memory_order_relaxed
            ))
        )},
        {"miniMapCachedChunksSkipped", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->miniMapCachedChunksSkipped.load(
                std::memory_order_relaxed
            ))
        )},
        {"retainedRadiusChunks", bedrock::JsRuntimeValue::number(
            state->retainedRadiusChunks.load(std::memory_order_relaxed)
        )},
        {"chunkPublisherPacketsObserved", bedrock::JsRuntimeValue::number(
            static_cast<double>(
                state->chunkPublisherPacketsObserved.load(
                    std::memory_order_relaxed
                )
            )
        )},
        {"chunkPublisherPacketsRewritten", bedrock::JsRuntimeValue::number(
            static_cast<double>(
                state->chunkPublisherPacketsRewritten.load(
                    std::memory_order_relaxed
                )
            )
        )},
        {"chunkPublisherDecodeFailures", bedrock::JsRuntimeValue::number(
            static_cast<double>(
                state->chunkPublisherDecodeFailures.load(
                    std::memory_order_relaxed
                )
            )
        )},
        {"lastServerPublisherRadiusBlocks", bedrock::JsRuntimeValue::number(
            state->lastServerPublisherRadiusBlocks.load(
                std::memory_order_relaxed
            )
        )},
        {"lastEffectivePublisherRadiusBlocks", bedrock::JsRuntimeValue::number(
            state->lastEffectivePublisherRadiusBlocks.load(
                std::memory_order_relaxed
            )
        )},
        {"retainedLevelChunkCount", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->retainedLevelChunkCount.load(
                std::memory_order_relaxed
            ))
        )},
        {"retainedLevelChunkBytes", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->retainedLevelChunkBytes.load(
                std::memory_order_relaxed
            ))
        )},
        {"retainedLevelChunkMaximumBytes", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->retainedLevelChunkMaximumBytes.load(
                std::memory_order_relaxed
            ))
        )},
        {"retainedLevelChunksStored", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->retainedLevelChunksStored.load(
                std::memory_order_relaxed
            ))
        )},
        {"retainedLevelChunksReplaced", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->retainedLevelChunksReplaced.load(
                std::memory_order_relaxed
            ))
        )},
        {"retainedLevelChunksEvictedRadius", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->retainedLevelChunksEvictedRadius.load(
                std::memory_order_relaxed
            ))
        )},
        {"retainedLevelChunksEvictedMemory", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->retainedLevelChunksEvictedMemory.load(
                std::memory_order_relaxed
            ))
        )},
        {"retainedLevelChunkParseFailures", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->retainedLevelChunkParseFailures.load(
                std::memory_order_relaxed
            ))
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
        {"equipmentRevision", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->equipmentRevision)
        )},
        {"equipment", bedrock::JsRuntimeValue::array(std::move(equipment))},
        {"playerInventoryReady", bedrock::JsRuntimeValue::boolean(
            state->playerInventoryReady
        )},
        {"automationPending", bedrock::JsRuntimeValue::boolean(
            state->pendingAutomationRequestId != 0
        )},
        {"automationAccepted", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->automationAccepted)
        )},
        {"automationRejected", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->automationRejected)
        )},
        {"automationStatus", bedrock::JsRuntimeValue::string(
            state->automationStatus
        )},
        {"playerHealthKnown", bedrock::JsRuntimeValue::boolean(
            state->playerHealthKnown
        )},
        {"playerHealth", bedrock::JsRuntimeValue::number(
            state->playerHealth
        )},
        {"playerMaximumHealth", bedrock::JsRuntimeValue::number(
            state->playerMaximumHealth
        )},
        {"playerHungerKnown", bedrock::JsRuntimeValue::boolean(
            state->playerHungerKnown
        )},
        {"playerHunger", bedrock::JsRuntimeValue::number(
            state->playerHunger
        )},
        {"playerSaturation", bedrock::JsRuntimeValue::number(
            state->playerSaturation
        )},
        {"playerAbsorptionKnown", bedrock::JsRuntimeValue::boolean(
            state->playerAbsorptionKnown
        )},
        {"playerAbsorption", bedrock::JsRuntimeValue::number(
            state->playerAbsorption
        )},
        {"playerResistanceLevel", bedrock::JsRuntimeValue::number(
            state->playerResistanceLevel
        )},
        {"error", bedrock::JsRuntimeValue::string(state->lastError)}
    });
    if (ok.has_value()) {
        result.set("ok", bedrock::JsRuntimeValue::boolean(*ok));
    }
    return result;
}

bedrock::JsRuntimeValue entityOverlayCameraValue(
    const bedrock::TrackedCameraPosition& camera,
    uint64_t capturedSteadyAtMs
) {
    const auto cameraAgeMs = camera.updatedAtMs > 0 &&
        capturedSteadyAtMs >= camera.updatedAtMs
            ? capturedSteadyAtMs - camera.updatedAtMs
            : 0;
    return bedrock::JsRuntimeValue::object({
        {"known", bedrock::JsRuntimeValue::boolean(camera.known)},
        {"inputTickKnown", bedrock::JsRuntimeValue::boolean(
            camera.inputTickKnown
        )},
        {"inputTick", bedrock::JsRuntimeValue::number(
            static_cast<double>(camera.inputTick)
        )},
        {"x", bedrock::JsRuntimeValue::number(camera.x)},
        {"y", bedrock::JsRuntimeValue::number(camera.y)},
        {"z", bedrock::JsRuntimeValue::number(camera.z)},
        {"pitch", bedrock::JsRuntimeValue::number(camera.pitch)},
        {"yaw", bedrock::JsRuntimeValue::number(camera.yaw)},
        {"ageMs", bedrock::JsRuntimeValue::number(
            static_cast<double>(cameraAgeMs)
        )},
        {"updatedAtMs", bedrock::JsRuntimeValue::number(
            static_cast<double>(camera.updatedAtMs)
        )}
    });
}

bedrock::JsRuntimeValue entityCameraSnapshotValue(
    const std::shared_ptr<RelayState>& state
) {
    const auto camera = state
        ? state->entityPositions.cameraSnapshot()
        : bedrock::TrackedCameraPosition {};
    return entityOverlayCameraValue(camera, steadyMilliseconds());
}

bedrock::JsRuntimeValue entityOverlaySnapshotValue(
    const std::shared_ptr<RelayState>& state
) {
    const auto snapshot = state->entityPositions.snapshot(320.0f, 96);
    const auto capturedSteadyAtMs = steadyMilliseconds();
    std::vector<bedrock::JsRuntimeValue> entities;
    entities.reserve(snapshot.entities.size());
    for (const auto& entity : snapshot.entities) {
        const auto entityAgeMs = entity.updatedAtMs > 0 &&
            capturedSteadyAtMs >= entity.updatedAtMs
                ? capturedSteadyAtMs - entity.updatedAtMs
                : 0;
        entities.push_back(bedrock::JsRuntimeValue::object({
            {"id", bedrock::JsRuntimeValue::string(
                std::to_string(entity.runtimeId)
            )},
            {"type", bedrock::JsRuntimeValue::string(entity.type)},
            {"label", bedrock::JsRuntimeValue::string(entity.label)},
            {"player", bedrock::JsRuntimeValue::boolean(entity.player)},
            {"item", bedrock::JsRuntimeValue::boolean(entity.item)},
            {"x", bedrock::JsRuntimeValue::number(entity.x)},
            {"y", bedrock::JsRuntimeValue::number(entity.y)},
            {"z", bedrock::JsRuntimeValue::number(entity.z)},
            {"width", bedrock::JsRuntimeValue::number(entity.width)},
            {"height", bedrock::JsRuntimeValue::number(entity.height)},
            {"ageMs", bedrock::JsRuntimeValue::number(
                static_cast<double>(entityAgeMs)
            )},
            {"updatedAtMs", bedrock::JsRuntimeValue::number(
                static_cast<double>(entity.updatedAtMs)
            )}
        }));
    }

    return bedrock::JsRuntimeValue::object({
        {"capturedAtMs", bedrock::JsRuntimeValue::number(
            static_cast<double>(unixMilliseconds())
        )},
        {"camera", entityOverlayCameraValue(
            snapshot.camera,
            capturedSteadyAtMs
        )},
        {"entities", bedrock::JsRuntimeValue::array(std::move(entities))},
        {"totalTrackedEntities", bedrock::JsRuntimeValue::number(
            static_cast<double>(snapshot.totalTrackedEntities)
        )},
        {"recognizedPackets", bedrock::JsRuntimeValue::number(
            static_cast<double>(snapshot.recognizedPackets)
        )},
        {"decodedPackets", bedrock::JsRuntimeValue::number(
            static_cast<double>(snapshot.decodedPackets)
        )},
        {"cameraOrientationUpdates", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->cameraOrientationUpdates.load(
                std::memory_order_relaxed
            ))
        )},
        {"cameraOrientationDecodeFailures", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->cameraOrientationDecodeFailures.load(
                std::memory_order_relaxed
            ))
        )},
        {"parseFailures", bedrock::JsRuntimeValue::number(
            static_cast<double>(snapshot.parseFailures)
        )}
    });
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
        // The mobile relay has raw packet observers, not packet editors.
        // Preserve backend extensions byte-for-byte instead of disconnecting
        // when a server uses a newer optional packet field.
        options.parseErrorPolicy = bedrock::RelayParseErrorPolicy::ForwardRaw;
        options.enableChunkCaching = false;
        options.levelChunkRetentionMaximumBytes =
            AndroidLevelChunkRetentionMaximumBytes;
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
                " retainedChunkLimitMiB=" + std::to_string(
                    AndroidLevelChunkRetentionMaximumBytes /
                        (1024u * 1024u)
                ) +
                " nativeBuild=" + std::string(NativeBuildType) +
                " compilerOptimized=" +
                (NativeCompilerOptimized ? "true" : "false"),
            "INFO",
            "lifecycle"
        );

        auto relay = std::make_unique<bedrock::Relay>(std::move(options));
        relay->live().configureLevelChunkRetention(
            state->chunkRetentionEnabled.load(std::memory_order_relaxed),
            static_cast<uint32_t>(state->retainedRadiusChunks.load(
                std::memory_order_relaxed
            ))
        );
        auto* liveRelay = &relay->live();

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
            const bool publishEncryptionRecovery =
                event.kind == bedrock::BedrockServerTransportEventKind::Receive &&
                event.message.starts_with(
                    "downstream encryption recovery"
                );
            std::string breadcrumb;
            if (record || publishLoginStage || publishError ||
                publishEncryptionRecovery) {
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
            } else if (publishEncryptionRecovery) {
                state->push(
                    "encryption_recovery",
                    breadcrumb,
                    event.message.find("succeeded") != std::string::npos
                        ? "INFO"
                        : "WARN",
                    "transport"
                );
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
            state->entityPositions.clear();
            state->clientboundEquipmentPackets = 0;
            state->clientboundEquipmentTransportPackets = 0;
            state->clientboundEquipmentForwardedPackets = 0;
            state->chunkPublisherPacketsObserved = 0;
            state->chunkPublisherPacketsRewritten = 0;
            state->chunkPublisherDecodeFailures = 0;
            state->lastServerPublisherRadiusBlocks = 0;
            state->lastEffectivePublisherRadiusBlocks = 0;
            state->retainedLevelChunkCount = 0;
            state->retainedLevelChunkBytes = 0;
            state->retainedLevelChunksStored = 0;
            state->retainedLevelChunksReplaced = 0;
            state->retainedLevelChunksEvictedRadius = 0;
            state->retainedLevelChunksEvictedMemory = 0;
            state->retainedLevelChunkParseFailures = 0;
            state->cameraOrientationUpdates = 0;
            state->cameraOrientationDecodeFailures = 0;
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
            state->entityPositions.clear();
            state->clearGameplayTelemetry();
            state->push(
                "disconnect",
                "origin=downstream_transport_close downstream_session=" +
                    player.sessionId() +
                    "; matching upstream was notified and closed; "
                    "relay remains running and listening; "
                    "clientbound_equipment_packets=" +
                    std::to_string(
                        state->clientboundEquipmentPackets.load()
                    ),
                "INFO",
                "lifecycle"
            );
            // A normal Minecraft-side close is not a relay fault. Preserve a
            // compact tail without forcing the UI to format hundreds of
            // high-volume packet breadcrumbs at the moment of disconnect.
            state->flushFlight("downstream_disconnect", 96);
        });
        relay->onError([state](const std::string& message) {
            state->entityPositions.clear();
            state->clearGameplayTelemetry();
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
        relay->live().onServerbound([state, version](
            bedrock::BedrockRelayPacketEvent& event
        ) {
            bool positionObserved = false;
            if (event.packet.name == "player_auth_input") {
                try {
                    bedrock::RelayPacketEvent decoded(version, event);
                    constexpr double Missing =
                        std::numeric_limits<double>::quiet_NaN();
                    const double forwardX = decoded.getDouble(
                        "camera_orientation.x",
                        Missing
                    );
                    const double forwardY = decoded.getDouble(
                        "camera_orientation.y",
                        Missing
                    );
                    const double forwardZ = decoded.getDouble(
                        "camera_orientation.z",
                        Missing
                    );
                    const bool inputTickKnown = decoded.has("tick");
                    const uint64_t inputTick = inputTickKnown
                        ? decoded.getUInt("tick", 0)
                        : 0;
                    if (std::isfinite(forwardX) &&
                        std::isfinite(forwardY) &&
                        std::isfinite(forwardZ)) {
                        const bool orientationApplied =
                            state->entityPositions
                                .observeServerboundWithCameraForward(
                                    event.packet,
                                    static_cast<float>(forwardX),
                                    static_cast<float>(forwardY),
                                    static_cast<float>(forwardZ),
                                    inputTick,
                                    inputTickKnown
                                );
                        positionObserved = true;
                        if (orientationApplied) {
                            state->cameraOrientationUpdates.fetch_add(
                                1,
                                std::memory_order_relaxed
                            );
                        }
                    }
                } catch (const std::exception& error) {
                    const auto failures =
                        state->cameraOrientationDecodeFailures.fetch_add(
                            1,
                            std::memory_order_relaxed
                        ) + 1;
                    if (failures == 1) {
                        state->push(
                            "camera_orientation_fallback",
                            "PlayerAuthInput camera_orientation decode failed; "
                                "using pitch/yaw: " + safeMessage(error.what()),
                            "WARN",
                            "entities"
                        );
                    }
                }
            }
            if (!positionObserved) {
                state->entityPositions.observeServerbound(event.packet);
            }
            state->observeDecodedGameplayPacket(version, event, true);
            state->maybeInjectAutomation(version, event);
            if (isResourcePackTransportPacket(event.packet.name)) {
                const auto sampleIndex =
                    state->resourcePackPacketsSeen.fetch_add(
                        1,
                        std::memory_order_relaxed
                    ) + 1;
                if (shouldPublishResourcePackSample(
                        event.packet.name,
                        sampleIndex
                    )) {
                    state->push(
                        "resource_pack_flow",
                        "stage=received_from_minecraft sample=" +
                            std::to_string(sampleIndex) + " " +
                            packetBreadcrumb("serverbound", event.packet),
                        "INFO",
                        "resource_pack"
                    );
                }
            }
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
        relay->live().onClientbound([state, version](
            bedrock::BedrockRelayPacketEvent& event
        ) {
            state->entityPositions.observeClientbound(event.packet);
            state->observeDecodedGameplayPacket(version, event, false);
            state->enqueueMiniMapChunk(version, event.packet);
            if (isResourcePackTransportPacket(event.packet.name)) {
                const auto sampleIndex =
                    state->resourcePackPacketsSeen.fetch_add(
                        1,
                        std::memory_order_relaxed
                    ) + 1;
                if (shouldPublishResourcePackSample(
                        event.packet.name,
                        sampleIndex
                    )) {
                    state->push(
                        "resource_pack_flow",
                        "stage=received_from_server sample=" +
                            std::to_string(sampleIndex) + " " +
                            packetBreadcrumb("clientbound", event.packet),
                        "INFO",
                        "resource_pack"
                    );
                }
            }
            if (event.packet.name == "network_chunk_publisher_update" &&
                state->chunkRetentionEnabled.load(std::memory_order_relaxed)) {
                const auto requestedRadiusChunks =
                    state->retainedRadiusChunks.load(
                        std::memory_order_relaxed
                    );
                // Keep the full requested cache inside the relay, but do not
                // make Minecraft render/download a 1024-block publisher
                // radius on memory-constrained Android devices.
                const auto clientRadiusChunks = std::min(
                    requestedRadiusChunks,
                    MaximumClientPublisherRadiusChunks
                );
                const auto minimumRadiusBlocks = static_cast<uint32_t>(
                    clientRadiusChunks * 16
                );
                const auto retention = bedrock::retainPublishedChunks(
                    event.packet,
                    minimumRadiusBlocks
                );
                if (retention.decoded) {
                    const auto observed =
                        state->chunkPublisherPacketsObserved.fetch_add(
                            1,
                            std::memory_order_relaxed
                        ) + 1;
                    state->lastServerPublisherRadiusBlocks.store(
                        retention.originalRadiusBlocks,
                        std::memory_order_relaxed
                    );
                    state->lastEffectivePublisherRadiusBlocks.store(
                        retention.effectiveRadiusBlocks,
                        std::memory_order_relaxed
                    );
                    auto rewritten =
                        state->chunkPublisherPacketsRewritten.load(
                            std::memory_order_relaxed
                        );
                    if (retention.rewritten) {
                        rewritten =
                            state->chunkPublisherPacketsRewritten.fetch_add(
                            1,
                            std::memory_order_relaxed
                        ) + 1;
                    }
                    if (observed <= 3 || observed % 128 == 0) {
                        state->push(
                            "chunk_retention",
                            "serverRadiusBlocks=" + std::to_string(
                                retention.originalRadiusBlocks
                            ) + " requestedCacheRadiusChunks=" +
                                std::to_string(requestedRadiusChunks) +
                                " clientRadiusLimitChunks=" +
                                std::to_string(clientRadiusChunks) +
                                " effectiveRadiusBlocks=" + std::to_string(
                                retention.effectiveRadiusBlocks
                            ) + " publisherUpdates=" +
                                std::to_string(observed) +
                                " rewrittenPackets=" +
                                std::to_string(rewritten),
                            "DEBUG",
                            "chunks"
                        );
                    }
                } else if (retention.recognized) {
                    const auto failures =
                        state->chunkPublisherDecodeFailures.fetch_add(
                            1,
                            std::memory_order_relaxed
                        ) + 1;
                    if (failures <= 3 || failures % 128 == 0) {
                        state->push(
                            "chunk_retention_decode_failed",
                            "Could not decode network_chunk_publisher_update; "
                            "packet forwarded unchanged failures=" +
                                std::to_string(failures),
                            "WARN",
                            "chunks"
                        );
                    }
                }
            }
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
        relay->live().onForwarded([state, liveRelay](
            const bedrock::BedrockRelayPacketEvent& event
        ) {
            if (isResourcePackTransportPacket(event.packet.name)) {
                const auto sampleIndex =
                    state->resourcePackPacketsForwarded.fetch_add(
                        1,
                        std::memory_order_relaxed
                    ) + 1;
                if (shouldPublishResourcePackSample(
                        event.packet.name,
                        sampleIndex
                    )) {
                    state->push(
                        "resource_pack_flow",
                        "stage=forwarded sample=" +
                            std::to_string(sampleIndex) + " " +
                            packetBreadcrumb(
                                event.direction ==
                                        bedrock::BedrockRelayDirection::Clientbound
                                    ? "clientbound"
                                    : "serverbound",
                                event.packet
                            ),
                        "INFO",
                        "resource_pack"
                    );
                }
            }
            if (event.direction ==
                    bedrock::BedrockRelayDirection::Clientbound &&
                (event.packet.name == "level_chunk" ||
                 event.packet.name == "network_chunk_publisher_update" ||
                 event.packet.name == "change_dimension")) {
                const auto stats = liveRelay->levelChunkRetentionStats();
                state->updateLevelChunkRetentionStats(stats);
                if (stats.enabled && event.packet.name == "level_chunk" &&
                    (stats.storedLevelChunks <= 3 ||
                     stats.storedLevelChunks % 256 == 0 ||
                     (stats.evictedForMemory != 0 &&
                      stats.storedLevelChunks % 64 == 0))) {
                    state->push(
                        "level_chunk_cache",
                        "residentChunks=" + std::to_string(
                            stats.residentChunks
                        ) + " residentBytes=" + std::to_string(
                            stats.residentBytes
                        ) + " stored=" + std::to_string(
                            stats.storedLevelChunks
                        ) + " replaced=" + std::to_string(
                            stats.replacedLevelChunks
                        ) + " evictedRadius=" + std::to_string(
                            stats.evictedOutsideRadius
                        ) + " evictedMemory=" + std::to_string(
                            stats.evictedForMemory
                        ),
                        stats.evictedForMemory == 0 ? "DEBUG" : "WARN",
                        "chunks"
                    );
                }
            }
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
        state_->entityPositions.clear();
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

    void configureLevelChunkRetention(bool enabled, int radiusChunks) {
        std::lock_guard lock(relayMutex_);
        if (!relay_) return;
        relay_->live().configureLevelChunkRetention(
            enabled,
            static_cast<uint32_t>(clampRetainedRadiusChunks(radiusChunks))
        );
        state_->updateLevelChunkRetentionStats(
            relay_->live().levelChunkRetentionStats()
        );
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
    jstring cacheDirectoryValue,
    jstring minecraftDataDirectoryValue
) {
    auto state = std::make_shared<RelayState>();
    state->configureRuntime(
        configuredDetailedLogging.load(std::memory_order_relaxed),
        configuredChunkRetention.load(std::memory_order_relaxed),
        configuredRetainedRadiusChunks.load(std::memory_order_relaxed)
    );
    state->configureGameplayFeatures(
        configuredAutoArmor.load(std::memory_order_relaxed),
        configuredAutoTotem.load(std::memory_order_relaxed),
        configuredMiniMap.load(std::memory_order_relaxed),
        configuredSchematic.load(std::memory_order_relaxed)
    );
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
        const auto minecraftDataDirectory = fromJavaString(
            environment,
            minecraftDataDirectoryValue
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
        if (!minecraftDataDirectory.empty()) {
            try {
                state->loadBlockRegistry(
                    std::filesystem::path(minecraftDataDirectory)
                );
            } catch (const std::exception& error) {
                state->push(
                    "block_registry_failed",
                    "Could not load packaged minecraft-data: " +
                        safeMessage(error.what()),
                    "WARN",
                    "world"
                );
            }
        } else {
            state->push(
                "block_registry_unavailable",
                "No packaged minecraft-data directory for version=" + version,
                "WARN",
                "world"
            );
        }

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
        if (configuredDetailedLogging.load(std::memory_order_relaxed)) {
            __android_log_print(
                ANDROID_LOG_ERROR,
                LogTag,
                "%s",
                message.c_str()
            );
        }
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

extern "C" JNIEXPORT void JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_configureRuntime(
    JNIEnv*,
    jclass,
    jboolean detailedLogging,
    jboolean chunkRetentionEnabled,
    jint retainedRadiusChunks
) {
    const int clampedRadius = clampRetainedRadiusChunks(
        static_cast<int>(retainedRadiusChunks)
    );
    configuredDetailedLogging.store(
        detailedLogging == JNI_TRUE,
        std::memory_order_relaxed
    );
    configuredChunkRetention.store(
        chunkRetentionEnabled == JNI_TRUE,
        std::memory_order_relaxed
    );
    configuredRetainedRadiusChunks.store(
        clampedRadius,
        std::memory_order_relaxed
    );

    std::shared_ptr<RelayState> state;
    std::shared_ptr<RelayController> activeController;
    {
        std::lock_guard lock(controllerMutex);
        state = currentState;
        activeController = controller;
    }
    if (state) {
        state->configureRuntime(
            detailedLogging == JNI_TRUE,
            chunkRetentionEnabled == JNI_TRUE,
            clampedRadius
        );
    }
    if (activeController) {
        activeController->configureLevelChunkRetention(
            chunkRetentionEnabled == JNI_TRUE,
            clampedRadius
        );
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_configureGameplayFeatures(
    JNIEnv*,
    jclass,
    jboolean autoArmorEnabled,
    jboolean autoTotemEnabled,
    jboolean miniMapEnabled,
    jboolean schematicEnabled
) {
    const bool armor = autoArmorEnabled == JNI_TRUE;
    const bool totem = autoTotemEnabled == JNI_TRUE;
    const bool miniMap = miniMapEnabled == JNI_TRUE;
    const bool schematic = schematicEnabled == JNI_TRUE;
    configuredAutoArmor.store(armor, std::memory_order_relaxed);
    configuredAutoTotem.store(totem, std::memory_order_relaxed);
    configuredMiniMap.store(miniMap, std::memory_order_relaxed);
    configuredSchematic.store(schematic, std::memory_order_relaxed);

    std::shared_ptr<RelayState> state;
    {
        std::lock_guard lock(controllerMutex);
        state = currentState;
    }
    if (state) {
        state->configureGameplayFeatures(armor, totem, miniMap, schematic);
    }
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

extern "C" JNIEXPORT jboolean JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_minecraftUiBlocked(
    JNIEnv*,
    jclass
) {
    std::shared_ptr<RelayState> state;
    {
        std::lock_guard lock(controllerMutex);
        state = currentState;
    }
    return state && state->minecraftUiBlocked.load(
        std::memory_order_relaxed
    ) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_miniMapSnapshot(
    JNIEnv* environment,
    jclass,
    jlong afterRevision,
    jint radiusChunks
) {
    std::shared_ptr<RelayState> state;
    {
        std::lock_guard lock(controllerMutex);
        state = currentState;
    }
    auto values = state->miniMapSnapshotValues(
        static_cast<uint64_t>(afterRevision),
        static_cast<int>(radiusChunks)
    );
    auto result = environment->NewIntArray(
        static_cast<jsize>(values.size())
    );
    if (result == nullptr || values.empty()) return result;
    static_assert(sizeof(jint) == sizeof(int32_t));
    environment->SetIntArrayRegion(
        result,
        0,
        static_cast<jsize>(values.size()),
        reinterpret_cast<const jint*>(values.data())
    );
    return result;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_worldSurfaceY(
    JNIEnv*,
    jclass,
    jint worldX,
    jint worldZ
) {
    std::shared_ptr<RelayState> state;
    {
        std::lock_guard lock(controllerMutex);
        state = currentState;
    }
    return state
        ? static_cast<jfloat>(state->worldSurfaceY(worldX, worldZ))
        : std::numeric_limits<jfloat>::quiet_NaN();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_entityCameraSnapshot(
    JNIEnv* environment,
    jclass
) {
    std::shared_ptr<RelayState> state;
    {
        std::lock_guard lock(controllerMutex);
        state = currentState;
    }
    return toJavaString(
        environment,
        jsonString(entityCameraSnapshotValue(state))
    );
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_entityOverlaySnapshot(
    JNIEnv* environment,
    jclass
) {
    std::shared_ptr<RelayState> state;
    {
        std::lock_guard lock(controllerMutex);
        state = currentState;
    }
    return toJavaString(
        environment,
        jsonString(entityOverlaySnapshotValue(state))
    );
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
