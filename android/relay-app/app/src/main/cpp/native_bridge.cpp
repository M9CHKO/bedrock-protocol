#include <bedrock/RakNetPing.hpp>
#include <bedrock/auth/JsRuntimeValue.hpp>
#include <bedrock/auth/XboxTokenManager.hpp>
#include <bedrock/bedrock.hpp>
#include <bedrock/relay/EntityPositionTracker.hpp>
#include <bedrock/relay/ItemDurability.hpp>
#include <bedrock/generated/GeneratedProtocolTypes.hpp>
#include <bedrock/protodef/ProtoDefEncoder.hpp>
#include <bedrock/protodef/ProtoDefWriter.hpp>
#include <bedrock/world/BedrockBlockRegistry.hpp>
#include <bedrock/world/BedrockSubChunkPacket.hpp>

#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <compare>
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
#include <set>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
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

std::string_view rakNetCloseSignal(uint8_t packetId) noexcept {
    // RakNet DefaultMessageIDTypes: graceful remote close, reliability
    // timeout, and protocol mismatch respectively.
    switch (packetId) {
        case 21: return "remote_disconnect_notification";
        case 22: return "connection_lost_or_timeout";
        case 25: return "incompatible_raknet_protocol";
        default: return {};
    }
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
        bedrock::ProtoDefValue transactionItem;
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
        bool removed = false;
    };

    struct MiniMapAppearance {
        bool air = false;
        bool solid = false;
        float collisionTop = 1.0f;
        int32_t color = 0xff777777;
    };

    struct MiniMapChunkJob {
        std::string version;
        std::string packetName;
        std::vector<uint8_t> payload;
        uint64_t generation = 0;
        uint64_t packetSequence = 0;
        int32_t dimension = 0;
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
        int64_t cameraDistanceSquared = 0;
    };

    struct SchematicColumnEntry {
        std::shared_ptr<bedrock::BedrockChunkColumn> column;
        std::set<int32_t> knownSections;
        bool completeBlockColumn = false;
        uint64_t revision = 0;
        uint64_t packetSequence = 0;
    };

    struct SchematicBlockKey {
        int32_t dimension = 0;
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;

        bool operator==(const SchematicBlockKey&) const = default;
    };

    struct SchematicBlockKeyHash {
        std::size_t operator()(const SchematicBlockKey& key) const noexcept {
            std::size_t result = std::hash<int32_t> {}(key.dimension);
            for (const auto value : {key.x, key.y, key.z}) {
                result ^= std::hash<int32_t> {}(value) +
                    0x9e3779b9u + (result << 6u) + (result >> 2u);
            }
            return result;
        }
    };

    struct SchematicBlockOverride {
        int32_t runtimeId = 0;
        uint64_t revision = 0;
        uint64_t packetSequence = 0;
    };

    struct SchematicBlockChange {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
        int32_t runtimeId = 0;
    };

    struct SchematicBlockAppearance {
        int32_t presence = 0;
        int32_t nameHash = 0;
        int32_t stateHash = 0;
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
    std::atomic<bool> schematicWorldTrackingActive {false};
    std::atomic<uint64_t> schematicTotalBlocks {0};
    std::atomic<uint64_t> schematicCorrectBlocks {0};
    std::atomic<uint64_t> schematicMissingBlocks {0};
    std::atomic<uint64_t> schematicWrongBlocks {0};
    std::atomic<uint64_t> schematicUnknownBlocks {0};
    std::atomic<uint64_t> schematicDisplayedMarkers {0};
    std::atomic<uint64_t> schematicMarkerRebuilds {0};
    std::atomic<uint64_t> schematicMarkerPackets {0};
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
    int64_t pendingAutomationNetworkId = 0;
    int32_t pendingAutomationStackId = 0;
    std::size_t pendingAutomationEquipmentIndex = 0;
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
    std::atomic<uint64_t> miniMapTileBuildFailures {0};
    std::atomic<uint64_t> miniMapLevelChunkSectionFallbacks {0};
    std::atomic<uint64_t> miniMapSubChunkDecodeFailures {0};
    std::atomic<uint64_t> miniMapCachedChunksSkipped {0};
    mutable std::mutex miniMapMutex;
    std::condition_variable miniMapCondition;
    std::deque<MiniMapChunkJob> miniMapJobs;
    std::unordered_set<MiniMapKey, MiniMapKeyHash> miniMapDirtyTiles;
    std::unordered_map<MiniMapKey, MiniMapTile, MiniMapKeyHash> miniMapTiles;
    std::thread miniMapWorker;
    bool miniMapStopping = false;
    uint64_t miniMapGeneration = 1;
    uint64_t miniMapRevision = 0;
    uint64_t schematicRevision = 0;
    bool schematicPublisherKnown = false;
    int32_t schematicPublisherChunkX = 0;
    int32_t schematicPublisherChunkZ = 0;
    int32_t schematicPublisherRadiusChunks = 0;
    std::atomic<uint64_t> schematicPacketSequence {0};
    std::unordered_map<
        MiniMapKey,
        SchematicColumnEntry,
        MiniMapKeyHash
    > schematicColumns;
    std::unordered_map<MiniMapKey, uint64_t, MiniMapKeyHash>
        schematicColumnHighWatermarks;
    std::unordered_map<
        SchematicBlockKey,
        SchematicBlockOverride,
        SchematicBlockKeyHash
    > schematicBlockOverrides;
    std::deque<std::pair<SchematicBlockKey, uint64_t>>
        schematicBlockOverrideOrder;
    mutable std::mutex blockRegistryMutex;
    std::optional<bedrock::BedrockBlockRegistry> blockRegistry;
    std::unordered_map<int32_t, MiniMapAppearance> miniMapAppearances;
    mutable std::unordered_map<int32_t, SchematicBlockAppearance>
        schematicBlockAppearances;
    mutable std::unordered_map<std::string, std::vector<bedrock::BlockShape>>
        schematicCollisionShapeCache;
    bool blockRuntimeIdsAreHashes = true;

    RelayState() {
        miniMapWorker = std::thread([this]() { miniMapWorkerLoop(); });
    }

    ~RelayState() {
        {
            std::lock_guard lock(miniMapMutex);
            miniMapStopping = true;
            miniMapJobs.clear();
            miniMapDirtyTiles.clear();
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

    static int32_t blockToChunkCoordinate(int32_t value) noexcept {
        int32_t chunk = value / 16;
        if (value < 0 && value % 16 != 0) --chunk;
        return chunk;
    }

    // IGN/Nukkit may expose a legacy 0..255 internal column while world
    // coordinates still use the modern Overworld -64 origin. This matches
    // the proven weathertop compatibility rule without shifting normal
    // modern (-64..319) columns or Nether/End columns.
    static int32_t storedBlockY(
        const bedrock::BedrockChunkColumn& column,
        int32_t dimension,
        int32_t worldY
    ) noexcept {
        return column.minY() < 0 || dimension != 0
            ? worldY
            : worldY + 64;
    }

    static int32_t worldBlockY(
        const bedrock::BedrockChunkColumn& column,
        int32_t dimension,
        int32_t storedY
    ) noexcept {
        return column.minY() < 0 || dimension != 0
            ? storedY
            : storedY - 64;
    }

    static std::string normalizedBaseBlockName(std::string_view name) {
        while (!name.empty() && std::isspace(
                static_cast<unsigned char>(name.front()))) {
            name.remove_prefix(1);
        }
        while (!name.empty() && std::isspace(
                static_cast<unsigned char>(name.back()))) {
            name.remove_suffix(1);
        }
        if (const auto properties = name.find('[');
            properties != std::string_view::npos) {
            name = name.substr(0, properties);
        }
        if (const auto nameSpace = name.find(':');
            nameSpace != std::string_view::npos) {
            name.remove_prefix(nameSpace + 1);
        }

        std::string normalized;
        normalized.reserve(name.size());
        bool replacingInvalidRun = false;
        for (const auto raw : name) {
            const auto value = static_cast<unsigned char>(raw);
            const auto lower = static_cast<char>(std::tolower(value));
            if ((lower >= 'a' && lower <= 'z') ||
                (lower >= '0' && lower <= '9') || lower == '_') {
                normalized.push_back(lower);
                replacingInvalidRun = false;
            } else if (!replacingInvalidRun) {
                normalized.push_back('_');
                replacingInvalidRun = true;
            }
        }
        const auto first = normalized.find_first_not_of('_');
        if (first == std::string::npos) return {};
        const auto last = normalized.find_last_not_of('_');
        return normalized.substr(first, last - first + 1);
    }

    static std::string normalizedBlockStateSignature(
        std::string_view name,
        const bedrock::BedrockBlockProperties& properties
    ) {
        std::string result = normalizedBaseBlockName(name);
        if (result.empty() || properties.empty()) return result;
        result.push_back('[');
        bool first = true;
        for (const auto& [propertyName, property] : properties) {
            std::string normalizedName;
            normalizedName.reserve(propertyName.size());
            for (const auto raw : propertyName) {
                normalizedName.push_back(static_cast<char>(std::tolower(
                    static_cast<unsigned char>(raw)
                )));
            }
            auto propertyValue = property.toString();
            std::transform(
                propertyValue.begin(),
                propertyValue.end(),
                propertyValue.begin(),
                [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                }
            );
            if (propertyValue == "true") propertyValue = "1";
            else if (propertyValue == "false") propertyValue = "0";
            if (!first) result.push_back(',');
            first = false;
            result += normalizedName;
            result.push_back('=');
            result += propertyValue;
        }
        result.push_back(']');
        return result;
    }

    static int32_t schematicBlockNameHash(std::string_view name) noexcept {
        uint32_t hash = 2166136261u;
        for (const auto raw : name) {
            hash ^= static_cast<uint8_t>(raw);
            hash *= 16777619u;
        }
        int32_t result = 0;
        static_assert(sizeof(result) == sizeof(hash));
        std::memcpy(&result, &hash, sizeof(result));
        return result;
    }

    static bool isAirBlockName(std::string_view name) noexcept {
        return name == "air" || name == "cave_air" || name == "void_air" ||
            name == "structure_void";
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
            schematicBlockAppearances.clear();
            schematicCollisionShapeCache.clear();
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
        schematicBlockAppearances.clear();
    }

    int32_t actualAirRuntimeId() const {
        std::lock_guard lock(blockRegistryMutex);
        if (!blockRegistry.has_value()) return 0;
        const auto* air = blockRegistry->blockByName("air");
        if (air == nullptr) return 0;
        if (!blockRuntimeIdsAreHashes) return air->defaultState;
        const auto* state = blockRegistry->stateById(air->defaultState);
        return state == nullptr
            ? 0
            : bedrock::BedrockBlockRegistry::computeRuntimeHash(
                state->name,
                state->properties
            );
    }

    std::optional<int32_t> schematicRuntimeId(
        std::string_view blockState
    ) const {
        std::lock_guard lock(blockRegistryMutex);
        if (!blockRegistry.has_value() || blockState.empty()) {
            return std::nullopt;
        }
        const auto block = blockRegistry->fromString(blockState);
        if (!block.has_value() || isAirBlockName(block->name)) {
            return std::nullopt;
        }
        if (!blockRuntimeIdsAreHashes) return block->stateId;
        return bedrock::BedrockBlockRegistry::computeRuntimeHash(
            block->name,
            block->properties
        );
    }

    std::vector<bedrock::BlockShape> schematicCollisionShapes(
        std::string_view blockState
    ) const {
        constexpr std::size_t MaximumPartsPerBlock = 3;
        const auto fullBlock = [] {
            return std::vector<bedrock::BlockShape> {
                bedrock::FullBlockShape
            };
        };
        if (blockState.empty()) return fullBlock();

        std::lock_guard lock(blockRegistryMutex);
        const std::string cacheKey(blockState);
        if (const auto cached = schematicCollisionShapeCache.find(cacheKey);
            cached != schematicCollisionShapeCache.end()) {
            return cached->second;
        }

        std::vector<bedrock::BlockShape> result;
        const auto block = blockRegistry.has_value()
            ? blockRegistry->fromString(blockState)
            : std::nullopt;
        if (block.has_value() && !isAirBlockName(block->name)) {
            const bool stairs = block->name.size() >= 7 &&
                block->name.compare(
                    block->name.size() - 7,
                    7,
                    "_stairs"
                ) == 0;
            const auto* upsideDownProperty = block->property(
                "upside_down_bit"
            );
            const auto* directionProperty = block->property(
                "weirdo_direction"
            );
            const auto upsideDown = upsideDownProperty == nullptr
                ? std::nullopt
                : upsideDownProperty->asInteger();
            const auto direction = directionProperty == nullptr
                ? std::nullopt
                : directionProperty->asInteger();
            if (stairs && upsideDown.has_value() && direction.has_value() &&
                *direction >= 0 && *direction <= 3) {
                // The 1.21.100 minecraft-data bridge assigns all Bedrock
                // stair states one arbitrary Java corner shape. Bedrock only
                // stores the straight facing here; connected inner/outer
                // corners are chosen by neighbours. A two-box straight stair
                // therefore gives a stable and directionally correct outline.
                const bool upper = *upsideDown != 0;
                result.push_back(upper
                    ? bedrock::BlockShape {0.0, 0.5, 0.0, 1.0, 1.0, 1.0}
                    : bedrock::BlockShape {0.0, 0.0, 0.0, 1.0, 0.5, 1.0});
                bedrock::BlockShape step = upper
                    ? bedrock::BlockShape {0.0, 0.0, 0.0, 1.0, 0.5, 1.0}
                    : bedrock::BlockShape {0.0, 0.5, 0.0, 1.0, 1.0, 1.0};
                // weirdo_direction: 0=east, 1=west, 2=south, 3=north.
                switch (*direction) {
                    case 0: step[0] = 0.5; break;
                    case 1: step[3] = 0.5; break;
                    case 2: step[2] = 0.5; break;
                    case 3: step[5] = 0.5; break;
                    default: break;
                }
                result.push_back(step);
            } else if (!block->missingStateShape) {
                result = block->raycastShapes();
            }
        }

        result.erase(
            std::remove_if(
                result.begin(),
                result.end(),
                [](const auto& shape) {
                    return !std::all_of(
                            shape.begin(),
                            shape.end(),
                            [](double value) { return std::isfinite(value); }
                        ) ||
                        shape[3] <= shape[0] ||
                        shape[4] <= shape[1] ||
                        shape[5] <= shape[2];
                }
            ),
            result.end()
        );
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        if (result.empty()) result = fullBlock();

        // Highly fragmented collision shapes are expensive in the debug
        // drawer. Preserve stairs and other common multipart blocks exactly,
        // but collapse pathological definitions to their union AABB.
        if (result.size() > MaximumPartsPerBlock) {
            auto united = result.front();
            for (std::size_t index = 1; index < result.size(); ++index) {
                united[0] = std::min(united[0], result[index][0]);
                united[1] = std::min(united[1], result[index][1]);
                united[2] = std::min(united[2], result[index][2]);
                united[3] = std::max(united[3], result[index][3]);
                united[4] = std::max(united[4], result[index][4]);
                united[5] = std::max(united[5], result[index][5]);
            }
            result.assign(1, united);
        }
        schematicCollisionShapeCache.emplace(cacheKey, result);
        return result;
    }

    void resetMiniMapWorld(int32_t dimension) noexcept {
        miniMapDimension.store(dimension, std::memory_order_relaxed);
        {
            std::lock_guard lock(miniMapMutex);
            ++miniMapGeneration;
            ++miniMapRevision;
            ++schematicRevision;
            miniMapJobs.clear();
            miniMapDirtyTiles.clear();
            miniMapTiles.clear();
            schematicColumns.clear();
            schematicColumnHighWatermarks.clear();
            schematicBlockOverrides.clear();
            schematicBlockOverrideOrder.clear();
            schematicPublisherKnown = false;
            schematicPublisherChunkX = 0;
            schematicPublisherChunkZ = 0;
            schematicPublisherRadiusChunks = 0;
        }
        {
            std::lock_guard lock(blockRegistryMutex);
            miniMapAppearances.clear();
            schematicBlockAppearances.clear();
        }
        miniMapCondition.notify_all();
    }

    void recordSchematicColumnSequenceLocked(
        const MiniMapKey& key,
        uint64_t packetSequence
    ) {
        schematicColumnHighWatermarks.insert_or_assign(key, packetSequence);
        constexpr std::size_t MaximumSchematicColumnWatermarks = 4096;
        if (schematicColumnHighWatermarks.size() <=
            MaximumSchematicColumnWatermarks) {
            return;
        }
        auto oldest = schematicColumnHighWatermarks.begin();
        for (auto current = schematicColumnHighWatermarks.begin();
             current != schematicColumnHighWatermarks.end();
             ++current) {
            if (current->second < oldest->second) oldest = current;
        }
        schematicColumnHighWatermarks.erase(oldest);
    }

    void markMiniMapTileRemovedLocked(const MiniMapKey& key) {
        const auto existing = miniMapTiles.find(key);
        if (existing == miniMapTiles.end() || existing->second.removed) return;
        MiniMapTile removed;
        removed.key = key;
        removed.pixels.fill(0x00000000);
        removed.surfaceHeights.fill(UnknownSurfaceHeight);
        removed.groundHeights.fill(std::numeric_limits<float>::quiet_NaN());
        removed.revision = ++miniMapRevision;
        removed.removed = true;
        existing->second = std::move(removed);
    }

    void invalidateMiniMapColumnLocked(
        const MiniMapKey& key,
        uint64_t invalidationSequence
    ) {
        schematicColumns.erase(key);
        recordSchematicColumnSequenceLocked(key, invalidationSequence);
        for (auto current = schematicBlockOverrides.begin();
             current != schematicBlockOverrides.end();) {
            if (current->first.dimension == key.dimension &&
                blockToChunkCoordinate(current->first.x) == key.x &&
                blockToChunkCoordinate(current->first.z) == key.z &&
                current->second.packetSequence <= invalidationSequence) {
                current = schematicBlockOverrides.erase(current);
            } else {
                ++current;
            }
        }
        markMiniMapTileRemovedLocked(key);
        ++schematicRevision;
    }

    void invalidateSchematicSubChunkEntryLocked(
        const MiniMapKey& key,
        int32_t sectionY,
        bedrock::BedrockSubChunkResult result,
        uint64_t invalidationSequence
    ) {
        const auto watermark = schematicColumnHighWatermarks.find(key);
        if (watermark != schematicColumnHighWatermarks.end() &&
            watermark->second > invalidationSequence) {
            return;
        }
        const auto invalidationRevision = ++schematicRevision;
        if (result == bedrock::BedrockSubChunkResult::ChunkNotFound) {
            schematicColumns.erase(key);
            for (auto current = schematicBlockOverrides.begin();
                 current != schematicBlockOverrides.end();) {
                if (current->first.dimension == key.dimension &&
                    blockToChunkCoordinate(current->first.x) == key.x &&
                    blockToChunkCoordinate(current->first.z) == key.z &&
                    current->second.packetSequence <= invalidationSequence) {
                    current = schematicBlockOverrides.erase(current);
                } else {
                    ++current;
                }
            }
        } else {
            const auto existing = schematicColumns.find(key);
            if (existing != schematicColumns.end()) {
                existing->second.knownSections.erase(sectionY);
                existing->second.completeBlockColumn = false;
                existing->second.packetSequence = invalidationSequence;
                existing->second.revision = invalidationRevision;
            }
            // A dropped/newer SubChunk supersedes every older UpdateBlock in
            // its column. Retaining one would make the snapshot report a
            // falsely known cell even though this section is now UNKNOWN.
            for (auto current = schematicBlockOverrides.begin();
                 current != schematicBlockOverrides.end();) {
                if (current->first.dimension == key.dimension &&
                    blockToChunkCoordinate(current->first.x) == key.x &&
                    blockToChunkCoordinate(current->first.z) == key.z &&
                    current->second.packetSequence <= invalidationSequence) {
                    current = schematicBlockOverrides.erase(current);
                } else {
                    ++current;
                }
            }
        }
        recordSchematicColumnSequenceLocked(key, invalidationSequence);
        markMiniMapTileRemovedLocked(key);
    }

    void invalidateMiniMapQueueGapLocked(uint64_t invalidationSequence) {
        ++miniMapGeneration;
        ++miniMapRevision;
        ++schematicRevision;
        miniMapJobs.clear();
        miniMapDirtyTiles.clear();
        miniMapTiles.clear();
        schematicColumns.clear();
        schematicColumnHighWatermarks.clear();
        if (invalidationSequence == std::numeric_limits<uint64_t>::max()) {
            schematicBlockOverrides.clear();
            schematicBlockOverrideOrder.clear();
        } else {
            for (auto current = schematicBlockOverrides.begin();
                 current != schematicBlockOverrides.end();) {
                if (current->second.packetSequence <= invalidationSequence) {
                    current = schematicBlockOverrides.erase(current);
                } else {
                    ++current;
                }
            }
        }
    }

    void cacheSchematicColumnLocked(
        const MiniMapKey& key,
        std::shared_ptr<bedrock::BedrockChunkColumn> column,
        std::set<int32_t> knownSections,
        bool completeBlockColumn,
        bool cameraKnown,
        int32_t cameraChunkX,
        int32_t cameraChunkZ,
        uint64_t packetSequence
    ) {
        if (schematicPublisherKnown &&
            (key.dimension !=
                miniMapDimension.load(std::memory_order_relaxed) ||
             std::abs(key.x - schematicPublisherChunkX) >
                    schematicPublisherRadiusChunks ||
             std::abs(key.z - schematicPublisherChunkZ) >
                    schematicPublisherRadiusChunks)) {
            return;
        }
        const auto watermark = schematicColumnHighWatermarks.find(key);
        if (watermark != schematicColumnHighWatermarks.end() &&
            watermark->second > packetSequence) {
            return;
        }
        const auto existing = schematicColumns.find(key);
        if (existing != schematicColumns.end() &&
            existing->second.packetSequence > packetSequence) {
            return;
        }
        for (auto current = schematicBlockOverrides.begin();
             current != schematicBlockOverrides.end();) {
            const auto& position = current->first;
            if (position.dimension != key.dimension ||
                blockToChunkCoordinate(position.x) != key.x ||
                blockToChunkCoordinate(position.z) != key.z) {
                ++current;
                continue;
            }
            const int32_t storedY = storedBlockY(
                *column,
                key.dimension,
                position.y
            );
            if (storedY < column->minY() || storedY >= column->maxY()) {
                ++current;
                continue;
            }
            const int32_t storedSectionY = blockToChunkCoordinate(storedY);
            const bool sectionKnown = completeBlockColumn ||
                knownSections.find(storedSectionY) != knownSections.end();
            if (!sectionKnown) {
                // A packet for another section cannot confirm or supersede
                // this exact block, regardless of packet ordering.
                ++current;
                continue;
            }
            if (current->second.packetSequence > packetSequence) {
                bedrock::BlockPosition blockPosition;
                blockPosition.x = position.x;
                blockPosition.y = storedY;
                blockPosition.z = position.z;
                blockPosition.layer = 0;
                column->setBlockStateId(
                    blockPosition,
                    current->second.runtimeId
                );
            }
            current = schematicBlockOverrides.erase(current);
        }

        const auto revision = ++schematicRevision;
        schematicColumns.insert_or_assign(
            key,
            SchematicColumnEntry {
                std::move(column),
                std::move(knownSections),
                completeBlockColumn,
                revision,
                packetSequence
            }
        );
        recordSchematicColumnSequenceLocked(key, packetSequence);

        // A 192-block schematic radius covers at most about 625 chunk columns;
        // retain the nearest 640 while still bounding decoded mobile memory.
        constexpr std::size_t MaximumSchematicColumns = 640;
        while (schematicColumns.size() > MaximumSchematicColumns) {
            auto farthest = schematicColumns.begin();
            int64_t farthestDistance = -1;
            uint64_t oldestRevision = std::numeric_limits<uint64_t>::max();
            for (auto current = schematicColumns.begin();
                 current != schematicColumns.end(); ++current) {
                int64_t distance = 0;
                if (cameraKnown) {
                    const int64_t dx = static_cast<int64_t>(current->first.x) -
                        cameraChunkX;
                    const int64_t dz = static_cast<int64_t>(current->first.z) -
                        cameraChunkZ;
                    distance = dx * dx + dz * dz;
                    if (current->first.dimension != key.dimension) {
                        distance += int64_t {1} << 60;
                    }
                }
                if (distance > farthestDistance ||
                    (distance == farthestDistance &&
                     current->second.revision < oldestRevision)) {
                    farthest = current;
                    farthestDistance = distance;
                    oldestRevision = current->second.revision;
                }
            }
            schematicColumns.erase(farthest);
        }
    }

    void observeSchematicPublisherWindow(
        int32_t centerXBlocks,
        int32_t centerZBlocks,
        uint32_t radiusBlocks
    ) noexcept {
        try {
            const int32_t centerChunkX = blockToChunkCoordinate(centerXBlocks);
            const int32_t centerChunkZ = blockToChunkCoordinate(centerZBlocks);
            // Keep one guard chunk because publisher coordinates may move
            // before the matching LevelChunk burst reaches the worker.
            const int32_t radiusChunks = std::max<int32_t>(
                1,
                static_cast<int32_t>((radiusBlocks + 15u) / 16u) + 1
            );
            const int32_t dimension = miniMapDimension.load(
                std::memory_order_relaxed
            );
            std::lock_guard lock(miniMapMutex);
            schematicPublisherKnown = true;
            schematicPublisherChunkX = centerChunkX;
            schematicPublisherChunkZ = centerChunkZ;
            schematicPublisherRadiusChunks = radiusChunks;
            const auto outsideWindow = [&](const MiniMapKey& key) {
                return key.dimension != dimension ||
                    std::abs(key.x - centerChunkX) > radiusChunks ||
                    std::abs(key.z - centerChunkZ) > radiusChunks;
            };

            bool changed = false;
            for (auto current = schematicColumns.begin();
                 current != schematicColumns.end();) {
                if (outsideWindow(current->first)) {
                    schematicColumnHighWatermarks.erase(current->first);
                    current = schematicColumns.erase(current);
                    changed = true;
                } else {
                    ++current;
                }
            }
            for (auto current = miniMapJobs.begin();
                 current != miniMapJobs.end();) {
                const MiniMapKey queuedKey {
                    current->dimension,
                    current->x,
                    current->z
                };
                if (current->packetName == "level_chunk" &&
                    outsideWindow(queuedKey)) {
                    current = miniMapJobs.erase(current);
                } else {
                    ++current;
                }
            }
            for (auto current = miniMapDirtyTiles.begin();
                 current != miniMapDirtyTiles.end();) {
                if (outsideWindow(*current)) {
                    current = miniMapDirtyTiles.erase(current);
                } else {
                    ++current;
                }
            }
            for (auto current = schematicBlockOverrides.begin();
                 current != schematicBlockOverrides.end();) {
                const auto chunkX = blockToChunkCoordinate(current->first.x);
                const auto chunkZ = blockToChunkCoordinate(current->first.z);
                if (current->first.dimension != dimension ||
                    std::abs(chunkX - centerChunkX) > radiusChunks ||
                    std::abs(chunkZ - centerChunkZ) > radiusChunks) {
                    current = schematicBlockOverrides.erase(current);
                    changed = true;
                } else {
                    ++current;
                }
            }
            for (auto current = miniMapTiles.begin();
                 current != miniMapTiles.end();) {
                if (current->first.dimension != dimension ||
                    std::abs(current->first.x - centerChunkX) > radiusChunks ||
                    std::abs(current->first.z - centerChunkZ) > radiusChunks) {
                    if (!current->second.removed) {
                        current->second.pixels.fill(0x00000000);
                        current->second.surfaceHeights.fill(
                            UnknownSurfaceHeight
                        );
                        current->second.groundHeights.fill(
                            std::numeric_limits<float>::quiet_NaN()
                        );
                        current->second.revision = ++miniMapRevision;
                        current->second.removed = true;
                    }
                    ++current;
                } else {
                    ++current;
                }
            }
            if (changed) ++schematicRevision;
        } catch (...) {
        }
    }

    void observeSchematicBlockUpdates(
        const std::vector<SchematicBlockChange>& changes
    ) noexcept {
        if (changes.empty()) return;
        const int32_t dimension = miniMapDimension.load(
            std::memory_order_relaxed
        );
        try {
            const auto packetSequence = schematicPacketSequence.fetch_add(
                1,
                std::memory_order_relaxed
            ) + 1;
            std::vector<MiniMapKey> changedColumns;
            {
                std::lock_guard lock(miniMapMutex);
                const auto revision = ++schematicRevision;
                changedColumns.reserve(changes.size());
                for (const auto& change : changes) {
                    const SchematicBlockKey blockKey {
                        dimension,
                        change.x,
                        change.y,
                        change.z
                    };
                    const auto columnKey = MiniMapKey {
                        dimension,
                        blockToChunkCoordinate(change.x),
                        blockToChunkCoordinate(change.z)
                    };
                    changedColumns.push_back(columnKey);
                    const auto cached = schematicColumns.find(columnKey);
                    if (cached != schematicColumns.end() &&
                        cached->second.column) {
                        const int32_t storedY = storedBlockY(
                            *cached->second.column,
                            dimension,
                            change.y
                        );
                        if (storedY >= cached->second.column->minY() &&
                            storedY < cached->second.column->maxY()) {
                            const int32_t sectionY =
                                blockToChunkCoordinate(storedY);
                            const bool sectionKnown =
                                cached->second.completeBlockColumn ||
                                cached->second.knownSections.find(sectionY) !=
                                    cached->second.knownSections.end();
                            if (sectionKnown) {
                                bedrock::BlockPosition position;
                                position.x = change.x;
                                position.y = storedY;
                                position.z = change.z;
                                position.layer = 0;
                                cached->second.column->setBlockStateId(
                                    position,
                                    change.runtimeId
                                );
                                cached->second.revision = revision;
                            }
                        }
                    }

                    // Keep a bounded update journal even when the column is
                    // cached. A background decode that started earlier must
                    // have every newer change overlaid before publication.
                    schematicBlockOverrides.insert_or_assign(
                        blockKey,
                        SchematicBlockOverride {
                            change.runtimeId,
                            revision,
                            packetSequence
                        }
                    );
                    schematicBlockOverrideOrder.emplace_back(
                        blockKey,
                        revision
                    );
                }
                constexpr std::size_t MaximumSchematicBlockOverrides = 8192;
                constexpr std::size_t MaximumSchematicOverrideOrder =
                    MaximumSchematicBlockOverrides * 2;
                while ((schematicBlockOverrides.size() >
                            MaximumSchematicBlockOverrides ||
                        schematicBlockOverrideOrder.size() >
                            MaximumSchematicOverrideOrder) &&
                       !schematicBlockOverrideOrder.empty()) {
                    const auto oldest = schematicBlockOverrideOrder.front();
                    schematicBlockOverrideOrder.pop_front();
                    const auto current = schematicBlockOverrides.find(
                        oldest.first
                    );
                    if (current != schematicBlockOverrides.end() &&
                        current->second.revision == oldest.second) {
                        schematicBlockOverrides.erase(current);
                    }
                }
            }
            enqueueMiniMapTileRebuilds(changedColumns);
        } catch (...) {
        }
    }

    void observeSchematicBlockUpdate(
        int32_t x,
        int32_t y,
        int32_t z,
        int32_t runtimeId
    ) noexcept {
        observeSchematicBlockUpdates({{x, y, z, runtimeId}});
    }

    std::vector<int32_t> schematicBlockSnapshotValues(
        uint64_t afterRevision,
        const std::vector<int32_t>& worldPositions
    ) const {
        struct SchematicBlockSample {
            bool known = false;
            bool semanticAir = false;
            int32_t runtimeId = 0;
        };

        const auto positionCount = worldPositions.size() / 3;
        uint64_t revision = 0;
        int32_t dimension = 0;
        std::vector<SchematicBlockSample> samples;
        {
            std::lock_guard lock(miniMapMutex);
            revision = schematicRevision;
            dimension = miniMapDimension.load(std::memory_order_relaxed);
            if (afterRevision == revision) {
                return {
                    0x43504553,
                    2,
                    static_cast<int32_t>(revision & 0xffffffffu),
                    static_cast<int32_t>(revision >> 32u),
                    dimension,
                    0
                };
            }

            samples.reserve(positionCount);
            for (std::size_t index = 0; index < positionCount; ++index) {
                const int32_t x = worldPositions[index * 3];
                const int32_t y = worldPositions[index * 3 + 1];
                const int32_t z = worldPositions[index * 3 + 2];
                const SchematicBlockKey blockKey {dimension, x, y, z};
                const auto overridden = schematicBlockOverrides.find(blockKey);
                if (overridden != schematicBlockOverrides.end()) {
                    // UpdateBlock carries a real runtime ID. Even ID zero must
                    // be classified through the active runtime registry.
                    samples.push_back({true, false, overridden->second.runtimeId});
                    continue;
                }
                const auto cached = schematicColumns.find(MiniMapKey {
                    dimension,
                    blockToChunkCoordinate(x),
                    blockToChunkCoordinate(z)
                });
                if (cached == schematicColumns.end() ||
                    !cached->second.column) {
                    samples.emplace_back();
                    continue;
                }
                const auto& entry = cached->second;
                const int32_t storedY = storedBlockY(
                    *entry.column,
                    dimension,
                    y
                );
                const int32_t sectionY = blockToChunkCoordinate(storedY);
                if (storedY < entry.column->minY() ||
                    storedY >= entry.column->maxY() ||
                    (!entry.completeBlockColumn &&
                     entry.knownSections.find(sectionY) ==
                        entry.knownSections.end())) {
                    samples.emplace_back();
                    continue;
                }
                bedrock::BlockPosition position;
                position.x = x;
                position.y = storedY;
                position.z = z;
                position.layer = 0;
                const auto* section = entry.column->getSection(storedY);
                // A fixed LevelChunk makes omitted in-range sections known
                // air, while an explicit zero-storage section has no layer.
                // Preserve that semantic fact instead of treating the
                // BedrockChunk fallback value 0 as a real runtime ID.
                const bool semanticAir = section == nullptr ||
                    section->layerCount() == 0;
                samples.push_back({
                    true,
                    semanticAir,
                    semanticAir ? 0 : entry.column->getBlockStateId(position)
                });
            }
        }

        std::vector<int32_t> result;
        result.reserve(6 + samples.size() * 3);
        result.push_back(0x43504553); // CPES
        result.push_back(2);
        result.push_back(static_cast<int32_t>(revision & 0xffffffffu));
        result.push_back(static_cast<int32_t>(revision >> 32u));
        result.push_back(dimension);
        result.push_back(static_cast<int32_t>(samples.size()));
        std::lock_guard registryLock(blockRegistryMutex);
        for (const auto& sample : samples) {
            if (!sample.known) {
                result.insert(result.end(), {0, 0, 0});
                continue;
            }
            if (sample.semanticAir) {
                result.insert(result.end(), {1, 0, 0});
                continue;
            }
            const int32_t runtimeId = sample.runtimeId;
            const auto cachedAppearance = schematicBlockAppearances.find(
                runtimeId
            );
            if (cachedAppearance != schematicBlockAppearances.end()) {
                result.push_back(cachedAppearance->second.presence);
                result.push_back(cachedAppearance->second.nameHash);
                result.push_back(cachedAppearance->second.stateHash);
                continue;
            }
            SchematicBlockAppearance appearance;
            const auto* state = blockRegistry.has_value()
                ? blockRegistry->stateByRuntimeId(runtimeId)
                : nullptr;
            if (state == nullptr) {
                appearance.presence = runtimeId == 0 ? 1 : 0;
            } else {
                const auto name = normalizedBaseBlockName(state->name);
                if (name.empty()) {
                    appearance.presence = 0;
                } else if (isAirBlockName(name)) {
                    appearance.presence = 1;
                } else {
                    appearance.presence = 2;
                    appearance.nameHash = schematicBlockNameHash(name);
                    appearance.stateHash = schematicBlockNameHash(
                        normalizedBlockStateSignature(
                            state->name,
                            state->properties
                        )
                    );
                }
            }
            schematicBlockAppearances.emplace(runtimeId, appearance);
            result.push_back(appearance.presence);
            result.push_back(appearance.nameHash);
            result.push_back(appearance.stateHash);
        }
        return result;
    }

    bool schematicSnapshotMatches(
        uint64_t expectedRevision,
        int32_t expectedDimension
    ) const noexcept {
        try {
            std::lock_guard lock(miniMapMutex);
            return schematicRevision == expectedRevision &&
                miniMapDimension.load(std::memory_order_relaxed) ==
                    expectedDimension;
        } catch (...) {
            return false;
        }
    }

    bool schematicDimensionMatches(int32_t expectedDimension) const noexcept {
        return miniMapDimension.load(std::memory_order_relaxed) ==
            expectedDimension;
    }

    void enqueueMiniMapChunk(
        const std::string& version,
        const bedrock::VersionedGamePacket& packet
    ) noexcept {
        if ((!miniMapEnabled.load(std::memory_order_relaxed) &&
             !schematicWorldTrackingActive.load(std::memory_order_relaxed)) ||
            (packet.name != "level_chunk" && packet.name != "subchunk")) {
            return;
        }
        try {
            uint64_t observedGeneration = 0;
            {
                std::lock_guard lock(miniMapMutex);
                if (miniMapStopping) return;
                observedGeneration = miniMapGeneration;
            }
            const auto camera = entityPositions.cameraSnapshot();
            int32_t chunkX = camera.known
                ? static_cast<int32_t>(std::floor(camera.x / 16.0f))
                : 0;
            int32_t chunkZ = camera.known
                ? static_cast<int32_t>(std::floor(camera.z / 16.0f))
                : 0;
            int32_t chunkY = 0;
            int32_t dimension = miniMapDimension.load(
                std::memory_order_relaxed
            );
            if (packet.name == "level_chunk") {
                bedrock::VersionedPayloadCursor cursor(packet.payload);
                chunkX = cursor.readVarInt();
                chunkZ = cursor.readVarInt();
                dimension = cursor.readVarInt();
            } else {
                const auto header =
                    bedrock::BedrockSubChunkPacketCodec::decodePacketHeader(
                        packet.payload,
                        version
                    );
                chunkX = header.originX;
                chunkY = header.originY;
                chunkZ = header.originZ;
                dimension = header.dimension;
            }
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
                packet.name,
                packet.payload,
                observedGeneration,
                schematicPacketSequence.fetch_add(
                    1,
                    std::memory_order_relaxed
                ) + 1,
                dimension,
                chunkX,
                chunkY,
                chunkZ,
                distanceSquared
            };
            std::optional<MiniMapChunkJob> droppedJob;
            {
                std::lock_guard lock(miniMapMutex);
                if (miniMapStopping || observedGeneration != miniMapGeneration) {
                    return;
                }
                for (auto& queued : miniMapJobs) {
                    const bool sameLevelColumn =
                        packet.name == "level_chunk" &&
                        queued.packetName == "level_chunk" &&
                        queued.dimension == dimension &&
                        queued.x == chunkX && queued.z == chunkZ;
                    if (sameLevelColumn) {
                        queued = std::move(incoming);
                        miniMapCondition.notify_one();
                        return;
                    }
                }

                // Initial SubChunk bursts can exceed the old 96-entry queue on
                // IGN-style servers. Keep a bounded raw-payload queue, but thin
                // it by camera distance without ever erasing unrelated world
                // authority.
                constexpr std::size_t MaximumQueuedChunks = 256;
                if (miniMapJobs.size() < MaximumQueuedChunks) {
                    miniMapJobs.push_back(std::move(incoming));
                } else {
                    auto farthest = std::max_element(
                        miniMapJobs.begin(),
                        miniMapJobs.end(),
                        [](const auto& left, const auto& right) {
                            if (left.cameraDistanceSquared !=
                                right.cameraDistanceSquared) {
                                return left.cameraDistanceSquared <
                                    right.cameraDistanceSquared;
                            }
                            return left.packetSequence > right.packetSequence;
                        }
                    );
                    if (farthest == miniMapJobs.end() ||
                        incoming.cameraDistanceSquared >=
                            farthest->cameraDistanceSquared) {
                        droppedJob = std::move(incoming);
                    } else {
                        droppedJob = std::move(*farthest);
                        *farthest = std::move(incoming);
                    }
                }
            }
            if (droppedJob.has_value()) {
                invalidateDroppedMiniMapJob(*droppedJob);
            }
            miniMapCondition.notify_one();
        } catch (...) {
        }
    }

    MiniMapTile buildMiniMapTileFromColumn(
        const MiniMapKey& key,
        const bedrock::BedrockChunkColumn& column,
        const std::set<int32_t>& knownSections,
        bool completeBlockColumn
    ) {
        MiniMapTile tile;
        tile.key = key;
        tile.surfaceHeights.fill(UnknownSurfaceHeight);
        tile.groundHeights.fill(std::numeric_limits<float>::quiet_NaN());
        std::array<int32_t, 256> baseColors {};
        std::array<uint32_t, 256> biomes {};
        {
            std::lock_guard registryLock(blockRegistryMutex);
            for (int32_t z = 0; z < 16; ++z) {
                for (int32_t x = 0; x < 16; ++x) {
                    const auto offset = static_cast<std::size_t>(z * 16 + x);
                    bool surfaceFound = false;
                    bool groundFound = false;
                    bool blockedByUnknownSection = false;
                    int32_t surfaceStoredY = column.minY();
                    for (int32_t sectionY = column.maxCY() - 1;
                         sectionY >= column.minCY() &&
                            (!surfaceFound || !groundFound);
                         --sectionY) {
                        if (!completeBlockColumn &&
                            knownSections.find(sectionY) == knownSections.end()) {
                            blockedByUnknownSection = true;
                            break;
                        }
                        const auto* section = column.getSectionAtIndex(sectionY);
                        if (section == nullptr || section->layerCount() == 0) {
                            continue;
                        }
                        for (int32_t localY = 15;
                             localY >= 0 && (!surfaceFound || !groundFound);
                             --localY) {
                            const auto runtimeId = section->getBlockStateId(
                                static_cast<uint8_t>(x),
                                static_cast<uint8_t>(localY),
                                static_cast<uint8_t>(z)
                            );
                            const auto appearance = miniMapAppearanceLocked(
                                runtimeId,
                                key.dimension
                            );
                            if (appearance.air) continue;
                            const int32_t storedY = sectionY * 16 + localY;
                            const int32_t worldY = worldBlockY(
                                column,
                                key.dimension,
                                storedY
                            );
                            if (!surfaceFound) {
                                surfaceFound = true;
                                surfaceStoredY = storedY;
                                tile.surfaceHeights[offset] =
                                    static_cast<int16_t>(worldY);
                                baseColors[offset] = appearance.color;
                            }
                            if (!groundFound && appearance.solid) {
                                groundFound = true;
                                tile.groundHeights[offset] =
                                    static_cast<float>(worldY) +
                                    appearance.collisionTop;
                            }
                        }
                    }
                    if (blockedByUnknownSection || !surfaceFound) {
                        tile.surfaceHeights[offset] = UnknownSurfaceHeight;
                        tile.groundHeights[offset] =
                            std::numeric_limits<float>::quiet_NaN();
                        continue;
                    }
                    try {
                        biomes[offset] = column.getBiomeId({
                            x,
                            surfaceStoredY,
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
                const auto offset = static_cast<std::size_t>(z * 16 + x);
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
                    (west + north - east - south) * 5 + (height - 64) / 20,
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
        return tile;
    }

    void rebuildMiniMapTilesFromColumns(
        const std::vector<MiniMapKey>& keys,
        uint64_t requiredGeneration
    ) {
        if (!miniMapEnabled.load(std::memory_order_relaxed) || keys.empty()) {
            return;
        }
        std::unordered_set<MiniMapKey, MiniMapKeyHash> uniqueKeys;
        uniqueKeys.reserve(keys.size());
        for (const auto& key : keys) uniqueKeys.insert(key);

        for (const auto& key : uniqueKeys) {
            std::shared_ptr<bedrock::BedrockChunkColumn> column;
            std::set<int32_t> knownSections;
            bool completeBlockColumn = false;
            uint64_t sourceRevision = 0;
            uint64_t generation = 0;
            {
                std::lock_guard lock(miniMapMutex);
                generation = miniMapGeneration;
                if (miniMapStopping ||
                    (requiredGeneration != 0 &&
                     requiredGeneration != generation)) {
                    return;
                }
                const auto cached = schematicColumns.find(key);
                if (cached == schematicColumns.end() ||
                    !cached->second.column) {
                    markMiniMapTileRemovedLocked(key);
                    continue;
                }
                column = std::make_shared<bedrock::BedrockChunkColumn>(
                    *cached->second.column
                );
                knownSections = cached->second.knownSections;
                completeBlockColumn = cached->second.completeBlockColumn;
                sourceRevision = cached->second.revision;
            }

            auto tile = buildMiniMapTileFromColumn(
                key,
                *column,
                knownSections,
                completeBlockColumn
            );
            {
                std::lock_guard lock(miniMapMutex);
                if (miniMapStopping || generation != miniMapGeneration) return;
                const auto cached = schematicColumns.find(key);
                if (cached == schematicColumns.end() ||
                    !cached->second.column ||
                    cached->second.revision != sourceRevision) {
                    continue;
                }
                tile.revision = ++miniMapRevision;
                miniMapTiles.insert_or_assign(key, std::move(tile));
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
        }
    }

    void recordMiniMapTileBuildFailure(
        const MiniMapKey& key,
        std::string message
    ) noexcept {
        const auto total = miniMapTileBuildFailures.fetch_add(
            1,
            std::memory_order_relaxed
        ) + 1;
        // A bad color/height render must not be reported as a malformed
        // LevelChunk. Keep the diagnostic useful without flooding the log
        // during a large initial chunk burst.
        if (total <= 4 || total % 32 == 0) {
            push(
                "minimap_tile_build_failed",
                "chunk=" + std::to_string(key.x) + "," +
                    std::to_string(key.z) + " dimension=" +
                    std::to_string(key.dimension) + " error=" +
                    safeMessage(std::move(message)) + " failures=" +
                    std::to_string(total),
                "WARN",
                "chunks"
            );
        }
    }

    void rebuildMiniMapTilesSafely(
        const std::vector<MiniMapKey>& keys,
        uint64_t requiredGeneration
    ) noexcept {
        if (!miniMapEnabled.load(std::memory_order_relaxed) || keys.empty()) {
            return;
        }
        try {
            std::unordered_set<MiniMapKey, MiniMapKeyHash> uniqueKeys;
            uniqueKeys.reserve(keys.size());
            for (const auto& key : keys) uniqueKeys.insert(key);
            for (const auto& key : uniqueKeys) {
                try {
                    rebuildMiniMapTilesFromColumns({key}, requiredGeneration);
                } catch (const std::exception& error) {
                    recordMiniMapTileBuildFailure(key, error.what());
                } catch (...) {
                    recordMiniMapTileBuildFailure(
                        key,
                        "unknown native exception"
                    );
                }
            }
        } catch (const std::exception& error) {
            recordMiniMapTileBuildFailure(keys.front(), error.what());
        } catch (...) {
            recordMiniMapTileBuildFailure(
                keys.front(),
                "unknown native exception"
            );
        }
    }

    void enqueueMiniMapTileRebuilds(
        const std::vector<MiniMapKey>& keys
    ) noexcept {
        if (!miniMapEnabled.load(std::memory_order_relaxed) || keys.empty()) {
            return;
        }
        try {
            {
                std::lock_guard lock(miniMapMutex);
                if (miniMapStopping) return;
                const int32_t dimension = miniMapDimension.load(
                    std::memory_order_relaxed
                );
                for (const auto& key : keys) {
                    if (key.dimension == dimension) {
                        miniMapDirtyTiles.insert(key);
                    }
                }
            }
            miniMapCondition.notify_one();
        } catch (...) {
        }
    }

    void cacheSchematicSubChunkJob(const MiniMapChunkJob& job) {
        if (!schematicWorldTrackingActive.load(std::memory_order_relaxed) &&
            !miniMapEnabled.load(std::memory_order_relaxed)) {
            return;
        }
        const auto packet =
            bedrock::BedrockSubChunkPacketCodec::decodePacketPayload(
                job.payload,
                job.version
            );
        const int32_t airRuntimeId = actualAirRuntimeId();
        const auto camera = entityPositions.cameraSnapshot();
        const int32_t cameraChunkX = camera.known
            ? static_cast<int32_t>(std::floor(camera.x / 16.0f))
            : 0;
        const int32_t cameraChunkZ = camera.known
            ? static_cast<int32_t>(std::floor(camera.z / 16.0f))
            : 0;
        std::vector<MiniMapKey> changedColumns;
        changedColumns.reserve(packet.entries.size());

        for (const auto& entry : packet.entries) {
            const int32_t chunkX = packet.originX + entry.dx;
            const int32_t sectionY = packet.originY + entry.dy;
            const int32_t chunkZ = packet.originZ + entry.dz;
            std::optional<bedrock::BedrockSubChunk> decodedSection;
            std::string entryDecodeError;
            try {
                if (sectionY >= std::numeric_limits<int8_t>::min() &&
                    sectionY <= std::numeric_limits<int8_t>::max() &&
                    entry.result ==
                        bedrock::BedrockSubChunkResult::SuccessAllAir) {
                    decodedSection = bedrock::BedrockSubChunk::createAir(
                        static_cast<int8_t>(sectionY),
                        airRuntimeId
                    );
                } else if (sectionY >= std::numeric_limits<int8_t>::min() &&
                           sectionY <= std::numeric_limits<int8_t>::max() &&
                           entry.result ==
                               bedrock::BedrockSubChunkResult::Success &&
                           !entry.payload.empty()) {
                    decodedSection = bedrock::BedrockSubChunk::decode(
                        bedrock::ChunkStorageType::Runtime,
                        entry.payload,
                        airRuntimeId
                    );
                    decodedSection->setY(static_cast<int8_t>(sectionY));
                }
            } catch (const std::exception& error) {
                entryDecodeError = safeMessage(error.what());
            } catch (...) {
                entryDecodeError = "unknown native exception";
            }
            if (!entryDecodeError.empty()) {
                recordMiniMapDecodeFailure(
                    job,
                    "entry=" + std::to_string(chunkX) + "," +
                        std::to_string(sectionY) + "," +
                        std::to_string(chunkZ) + " " + entryDecodeError
                );
            }

            const MiniMapKey key {packet.dimension, chunkX, chunkZ};
            std::lock_guard lock(miniMapMutex);
            if (job.generation != miniMapGeneration || miniMapStopping) {
                return;
            }
            if (schematicPublisherKnown &&
                (key.dimension !=
                    miniMapDimension.load(std::memory_order_relaxed) ||
                 std::abs(key.x - schematicPublisherChunkX) >
                    schematicPublisherRadiusChunks ||
                 std::abs(key.z - schematicPublisherChunkZ) >
                    schematicPublisherRadiusChunks)) {
                continue;
            }
            const auto watermark = schematicColumnHighWatermarks.find(key);
            if (watermark != schematicColumnHighWatermarks.end() &&
                watermark->second > job.packetSequence) {
                continue;
            }

            if (!decodedSection.has_value()) {
                invalidateSchematicSubChunkEntryLocked(
                    key,
                    sectionY,
                    entry.result,
                    job.packetSequence
                );
                continue;
            }

            std::shared_ptr<bedrock::BedrockChunkColumn> column;
            std::set<int32_t> knownSections;
            bool completeBlockColumn = false;
            const auto existing = schematicColumns.find(key);
            if (existing != schematicColumns.end() &&
                existing->second.column) {
                column = std::make_shared<bedrock::BedrockChunkColumn>(
                    *existing->second.column
                );
                knownSections = existing->second.knownSections;
                completeBlockColumn = existing->second.completeBlockColumn;
            } else {
                column = std::make_shared<bedrock::BedrockChunkColumn>(
                    chunkX,
                    chunkZ,
                    airRuntimeId
                );
                column->setBounds(-4, 20);
            }
            if (sectionY < column->minCY() || sectionY >= column->maxCY()) {
                recordSchematicColumnSequenceLocked(key, job.packetSequence);
                markMiniMapTileRemovedLocked(key);
                continue;
            }
            column->setSection(sectionY, std::move(*decodedSection));
            knownSections.insert(sectionY);
            cacheSchematicColumnLocked(
                key,
                std::move(column),
                std::move(knownSections),
                completeBlockColumn,
                camera.known,
                cameraChunkX,
                cameraChunkZ,
                job.packetSequence
            );
            changedColumns.push_back(key);
        }
        rebuildMiniMapTilesSafely(changedColumns, job.generation);
        miniMapDecodedChunks.fetch_add(1, std::memory_order_relaxed);
    }

    void recordMiniMapDecodeFailure(
        const MiniMapChunkJob& job,
        std::string message
    ) noexcept {
        const auto total = miniMapDecodeFailures.fetch_add(
            1,
            std::memory_order_relaxed
        ) + 1;
        if (job.packetName == "subchunk") {
            miniMapSubChunkDecodeFailures.fetch_add(
                1,
                std::memory_order_relaxed
            );
        }
        // Keep diagnostics useful without writing one log line per chunk in
        // the initial server burst.
        if (total <= 4 || total % 32 == 0) {
            push(
                "chunk_decode_failed",
                "packet=" + job.packetName + " chunk=" +
                    std::to_string(job.x) + "," + std::to_string(job.z) +
                    " dimension=" + std::to_string(job.dimension) +
                    " payloadBytes=" + std::to_string(job.payload.size()) +
                    " error=" + safeMessage(std::move(message)) +
                    " failures=" + std::to_string(total),
                "WARN",
                "chunks"
            );
        }
    }

    void invalidateDroppedMiniMapJob(const MiniMapChunkJob& job) noexcept {
        try {
            if (job.packetName == "level_chunk") {
                std::lock_guard lock(miniMapMutex);
                if (job.generation != miniMapGeneration || miniMapStopping) {
                    return;
                }
                invalidateMiniMapColumnLocked({
                    job.dimension,
                    job.x,
                    job.z
                }, job.packetSequence);
                return;
            }
            if (job.packetName != "subchunk") return;

            // Decoding the envelope exposes every relative entry without
            // decoding its block storage. Invalidate only those exact sections
            // when a queued raw packet has to be dropped.
            const auto packet =
                bedrock::BedrockSubChunkPacketCodec::decodePacketPayload(
                    job.payload,
                    job.version
                );
            std::lock_guard lock(miniMapMutex);
            if (job.generation != miniMapGeneration || miniMapStopping) return;
            for (const auto& entry : packet.entries) {
                invalidateSchematicSubChunkEntryLocked(
                    {
                        packet.dimension,
                        packet.originX + entry.dx,
                        packet.originZ + entry.dz
                    },
                    packet.originY + entry.dy,
                    entry.result,
                    job.packetSequence
                );
            }
        } catch (...) {
            // A malformed envelope cannot identify reliable offsets. Retain
            // the last-known cache just as the retail client does; never turn
            // one bad packet into an all-world schematic reset.
        }
    }

    void invalidateFailedMiniMapJob(const MiniMapChunkJob& job) noexcept {
        try {
            std::lock_guard lock(miniMapMutex);
            if (job.generation != miniMapGeneration || miniMapStopping) return;
            if (job.packetName == "level_chunk") {
                invalidateMiniMapColumnLocked({
                    job.dimension,
                    job.x,
                    job.z
                }, job.packetSequence);
            }
        } catch (...) {
        }
        miniMapCondition.notify_one();
    }

    void miniMapWorkerLoop() noexcept {
        constexpr std::size_t MaximumStructuralJobsBeforeDirtyTile = 8;
        std::size_t structuralJobsSinceDirtyTile = 0;
        for (;;) {
            MiniMapChunkJob job;
            std::optional<MiniMapKey> dirtyTile;
            {
                std::unique_lock lock(miniMapMutex);
                miniMapCondition.wait(lock, [this]() {
                    return miniMapStopping || !miniMapJobs.empty() ||
                        !miniMapDirtyTiles.empty();
                });
                if (miniMapStopping) return;
                const bool serviceDirtyTile =
                    !miniMapDirtyTiles.empty() &&
                    (miniMapJobs.empty() ||
                     structuralJobsSinceDirtyTile >=
                        MaximumStructuralJobsBeforeDirtyTile);
                if (serviceDirtyTile) {
                    const auto current = miniMapDirtyTiles.begin();
                    dirtyTile = *current;
                    miniMapDirtyTiles.erase(current);
                    structuralJobsSinceDirtyTile = 0;
                } else {
                    // Structural world packets must be decoded in their
                    // observed order. A newer partial SubChunk must never
                    // make an older full LevelChunk look stale column-wide.
                    const auto oldest = std::min_element(
                        miniMapJobs.begin(),
                        miniMapJobs.end(),
                        [](const auto& left, const auto& right) {
                            return left.packetSequence < right.packetSequence;
                        }
                    );
                    job = std::move(*oldest);
                    miniMapJobs.erase(oldest);
                    structuralJobsSinceDirtyTile = std::min(
                        structuralJobsSinceDirtyTile + 1,
                        MaximumStructuralJobsBeforeDirtyTile
                    );
                }
            }

            try {
                if (dirtyTile.has_value()) {
                    rebuildMiniMapTilesSafely({*dirtyTile}, 0);
                    continue;
                }
                {
                    std::lock_guard lock(miniMapMutex);
                    if (job.generation != miniMapGeneration) continue;
                }
                if (!miniMapEnabled.load(std::memory_order_relaxed) &&
                    !schematicWorldTrackingActive.load(
                        std::memory_order_relaxed
                    )) {
                    continue;
                }
                if (job.packetName == "subchunk") {
                    cacheSchematicSubChunkJob(job);
                    continue;
                }
                auto packet = bedrock::BedrockLevelChunkCodec::decodePacketPayload(
                    job.payload
                );
                if (packet.cacheEnabled) {
                    miniMapCachedChunksSkipped.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                    invalidateFailedMiniMapJob(job);
                    continue;
                }
                const int32_t airRuntimeId = actualAirRuntimeId();
                std::shared_ptr<bedrock::BedrockChunkColumn> column;
                try {
                    column = std::make_shared<bedrock::BedrockChunkColumn>(
                        bedrock::BedrockLevelChunkCodec::decodeNoCacheColumn(
                            packet,
                            versionAtLeast(job.version, 1, 18, 0),
                            airRuntimeId
                        )
                    );
                } catch (const std::exception& strictError) {
                    if (!versionAtLeast(job.version, 1, 18, 0) ||
                        packet.subChunkCount <= 0) {
                        throw;
                    }
                    try {
                        column = std::make_shared<bedrock::BedrockChunkColumn>(
                            bedrock::BedrockLevelChunkCodec::
                                decodeNoCacheBlockSectionsFallback(
                                    packet,
                                    airRuntimeId
                                )
                        );
                    } catch (const std::exception& fallbackError) {
                        throw bedrock::BedrockChunkError(
                            "strict LevelChunk decode failed: " +
                            std::string(strictError.what()) +
                            "; section fallback failed: " +
                            fallbackError.what()
                        );
                    }
                    const auto fallbacks =
                        miniMapLevelChunkSectionFallbacks.fetch_add(
                            1,
                            std::memory_order_relaxed
                        ) + 1;
                    if (fallbacks <= 4 || fallbacks % 64 == 0) {
                        push(
                            "chunk_decode_section_fallback",
                            "packet=level_chunk chunk=" +
                                std::to_string(packet.x) + "," +
                                std::to_string(packet.z) + " dimension=" +
                                std::to_string(packet.dimension) +
                                " sections=" +
                                std::to_string(packet.subChunkCount) +
                                " strictError=" +
                                safeMessage(strictError.what()) +
                                " salvaged=" + std::to_string(fallbacks),
                            "WARN",
                            "chunks"
                        );
                    }
                }
                const MiniMapKey columnKey {
                    packet.dimension,
                    packet.x,
                    packet.z
                };

                const auto camera = entityPositions.cameraSnapshot();
                const int32_t cameraChunkX = static_cast<int32_t>(
                    std::floor(camera.x / 16.0f)
                );
                const int32_t cameraChunkZ = static_cast<int32_t>(
                    std::floor(camera.z / 16.0f)
                );
                {
                    std::lock_guard lock(miniMapMutex);
                    if (job.generation != miniMapGeneration || miniMapStopping) {
                        continue;
                    }
                    miniMapDimension.store(
                        packet.dimension,
                        std::memory_order_relaxed
                    );
                    cacheSchematicColumnLocked(
                        columnKey,
                        column,
                        {},
                        packet.subChunkCount != -1 &&
                            packet.subChunkCount != -2,
                        camera.known,
                        cameraChunkX,
                        cameraChunkZ,
                        job.packetSequence
                    );
                }
                // The heavy surface scan runs after the authoritative column
                // (plus any newer UpdateBlock overrides) has been published.
                // It is skipped entirely for schematic-only operation.
                rebuildMiniMapTilesSafely(
                    {columnKey},
                    job.generation
                );
                miniMapDecodedChunks.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception& error) {
                invalidateFailedMiniMapJob(job);
                recordMiniMapDecodeFailure(job, error.what());
            } catch (...) {
                invalidateFailedMiniMapJob(job);
                recordMiniMapDecodeFailure(job, "unknown native exception");
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

    float worldSurfaceY(int32_t worldX, int32_t worldZ) noexcept {
        float fallback = std::numeric_limits<float>::quiet_NaN();
        try {
            const int32_t chunkX = blockToChunkCoordinate(worldX);
            const int32_t chunkZ = blockToChunkCoordinate(worldZ);
            const int32_t localX = worldX - chunkX * 16;
            const int32_t localZ = worldZ - chunkZ * 16;
            const int32_t dimension = miniMapDimension.load(
                std::memory_order_relaxed
            );
            std::lock_guard lock(miniMapMutex);
            const MiniMapKey key {
                dimension,
                chunkX,
                chunkZ
            };
            const auto tile = miniMapTiles.find(key);
            if (tile != miniMapTiles.end()) {
                fallback = tile->second.groundHeights[
                    static_cast<std::size_t>(localZ * 16 + localX)
                ];
            }
            const auto cached = schematicColumns.find(key);
            if (cached == schematicColumns.end() || !cached->second.column) {
                return fallback;
            }

            // Only one X/Z column is needed for placement. This keeps
            // collision placement exact without paying the full minimap
            // surface scan while the minimap feature is disabled.
            // miniMapMutex protects the cached column from concurrent
            // UpdateBlock mutation during this short scan.
            std::lock_guard registryLock(blockRegistryMutex);
            const auto& entry = cached->second;
            const auto& column = *entry.column;
            for (int32_t sectionY = column.maxCY() - 1;
                 sectionY >= column.minCY();
                 --sectionY) {
                if (!entry.completeBlockColumn &&
                    entry.knownSections.find(sectionY) ==
                        entry.knownSections.end()) {
                    // A lower streamed section cannot prove the surface while
                    // any section above it is still unknown.
                    return fallback;
                }
                const auto* section = column.getSectionAtIndex(sectionY);
                if (section == nullptr || section->layerCount() == 0) {
                    continue;
                }
                for (int32_t localY = 15; localY >= 0; --localY) {
                    const auto runtimeId = section->getBlockStateId(
                        static_cast<uint8_t>(localX),
                        static_cast<uint8_t>(localY),
                        static_cast<uint8_t>(localZ)
                    );
                    const auto appearance = miniMapAppearanceLocked(
                        runtimeId,
                        dimension
                    );
                    if (!appearance.solid) continue;
                    const int32_t storedY = sectionY * 16 + localY;
                    return static_cast<float>(worldBlockY(
                        column,
                        dimension,
                        storedY
                    )) +
                        appearance.collisionTop;
                }
            }
        } catch (...) {
        }
        return fallback;
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
        if (itemValue != nullptr &&
            itemValue->kind == bedrock::ProtoDefValue::Kind::Object) {
            item.transactionItem = *itemValue;
        }
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
                    item.transactionItem.kind !=
                        bedrock::ProtoDefValue::Kind::Object) {
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
            if (!item.present || item.transactionItem.kind !=
                bedrock::ProtoDefValue::Kind::Object) {
                continue;
            }
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

    static bedrock::ProtoDefValue legacyTransactionItem(
        const EquipmentItem& item
    ) {
        using Value = bedrock::ProtoDefValue;
        if (!item.present) {
            return Value::object({{"network_id", Value::integer(0)}});
        }
        if (item.transactionItem.kind != Value::Kind::Object) {
            throw std::runtime_error(
                "server-confirmed item payload is unavailable"
            );
        }

        // IGN accepts the legacy InventoryTransaction path used by
        // weathertop_bot. Its working client trace intentionally omits the
        // stack network id even though InventorySlot carried one.
        auto value = item.transactionItem;
        value.objectValue["has_stack_id"] = Value::uinteger(0);
        value.objectValue.erase("stack_id");
        return value;
    }

    static bedrock::ProtoDefValue legacyAutomationAction(
        std::string inventoryId,
        uint8_t slot,
        const EquipmentItem& oldItem,
        const EquipmentItem& newItem
    ) {
        using Value = bedrock::ProtoDefValue;
        return Value::object({
            {"source_type", Value::string("container")},
            {"inventory_id", Value::string(std::move(inventoryId))},
            {"slot", Value::uinteger(slot)},
            {"old_item", legacyTransactionItem(oldItem)},
            {"new_item", legacyTransactionItem(newItem)}
        });
    }

    bedrock::VersionedGamePacket makeAutomationPacket(
        const std::string& version,
        const AutomationPlan& plan
    ) {
        using Value = bedrock::ProtoDefValue;
        const std::string destinationInventory = plan.totem
            ? "offhand"
            : "armor";
        auto transaction = Value::object({
            {"legacy", Value::object({
                {"legacy_request_id", Value::integer(0)}
            })},
            {"transaction_type", Value::string("normal")},
            {"actions", Value::array({
                legacyAutomationAction(
                    "inventory",
                    static_cast<uint8_t>(plan.inventorySlot),
                    plan.source,
                    plan.destination
                ),
                legacyAutomationAction(
                    destinationInventory,
                    plan.equipmentSlot,
                    plan.destination,
                    plan.source
                )
            })}
        });
        // The generated C++ 1.21.100 table names this field
        // WindowIDZigzag32, but the working weathertop/node codec and the IGN
        // server use WindowIDVarint for legacy TransactionActions. Override
        // only this nested type while leaving every Item codec versioned.
        const auto packetSchema = bedrock::generatedProtocolTypeJson(
            version,
            "packet_inventory_transaction"
        );
        if (!packetSchema.has_value()) {
            throw std::runtime_error(
                "inventory_transaction schema is unavailable"
            );
        }
        bedrock::ProtoDefEncoder encoder(
            [&version](const std::string& typeName)
                -> std::optional<std::string> {
                if (typeName == "WindowIDZigzag32") {
                    if (auto legacyWindow = bedrock::generatedProtocolTypeJson(
                            version,
                            "WindowIDVarint"
                        ); legacyWindow.has_value()) {
                        return legacyWindow;
                    }
                }
                return bedrock::generatedProtocolTypeJson(version, typeName);
            }
        );
        encoder.setVariables(itemProtocolVariables->snapshot());
        bedrock::ProtoDefWriter writer;
        encoder.encode(
            *packetSchema,
            Value::object({{"transaction", std::move(transaction)}}),
            writer
        );
        auto payload = writer.take();
        return bedrock::VersionedMcpeCodec::forVersion(version)
            .packetCodec()
            .makePacketByName("inventory_transaction", payload);
    }

    void resolveLegacyAutomationLocked(std::size_t equipmentIndex) {
        if (pendingAutomationRequestId == 0 ||
            equipmentIndex != pendingAutomationEquipmentIndex) {
            return;
        }
        const bool accepted = equipmentIndex < equipment.size() &&
            equipment[equipmentIndex].present &&
            equipment[equipmentIndex].networkId == pendingAutomationNetworkId &&
            (pendingAutomationStackId == 0 ||
             equipment[equipmentIndex].stackId == pendingAutomationStackId);
        pendingAutomationRequestId = 0;
        pendingAutomationNetworkId = 0;
        pendingAutomationStackId = 0;
        pendingAutomationEquipmentIndex = 0;
        if (accepted) {
            ++automationAccepted;
            automationStatus = "Legacy-транзакция подтверждена сервером";
        } else {
            ++automationRejected;
            playerInventoryReady = false;
            automationStatus =
                "Сервер отклонил legacy-транзакцию — ожидается синхронизация";
        }
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
                pendingAutomationNetworkId = 0;
                pendingAutomationStackId = 0;
                pendingAutomationEquipmentIndex = 0;
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
            if (plan.inventorySlot >= playerInventory.size()) {
                return;
            }
            const auto& currentSource = playerInventory[plan.inventorySlot];
            if (currentSource.networkId != plan.source.networkId ||
                currentSource.count != plan.source.count ||
                (plan.source.stackId != 0 &&
                 currentSource.stackId != plan.source.stackId) ||
                (plan.source.damageKnown && currentSource.damageKnown &&
                 currentSource.damage != plan.source.damage)) {
                return;
            }
            playerInventory[plan.inventorySlot] = plan.destination;
            equipment[plan.equipmentIndex] = plan.source;
            ++equipmentRevision;
            pendingAutomationRequestId = plan.requestId;
            pendingAutomationNetworkId = plan.source.networkId;
            pendingAutomationStackId = plan.source.stackId;
            pendingAutomationEquipmentIndex = plan.equipmentIndex;
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
            int32_t initialDimension = 0;
            try {
                std::lock_guard decodeLock(itemDecodeMutex);
                bedrock::RelayPacketEvent decoded(version, event);
                configureBlockRuntimeIds(decoded.getBool(
                    "block_network_ids_are_hashes",
                    true
                ));
                initialDimension = static_cast<int32_t>(decoded.getInt(
                    "dimension",
                    0
                ));
            } catch (...) {
            }
            resetMiniMapWorld(initialDimension);
            std::lock_guard lock(mutex);
            equipment = {};
            playerInventory.clear();
            playerInventoryReady = false;
            pendingAutomationRequestId = 0;
            pendingAutomationNetworkId = 0;
            pendingAutomationStackId = 0;
            pendingAutomationEquipmentIndex = 0;
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
        if (!serverbound &&
            (schematicWorldTrackingActive.load(std::memory_order_relaxed) ||
             miniMapEnabled.load(std::memory_order_relaxed)) &&
            (name == "update_block" || name == "update_block_synced")) {
            try {
                std::lock_guard decodeLock(itemDecodeMutex);
                bedrock::RelayPacketEvent decoded(
                    version,
                    event,
                    itemProtocolVariables,
                    true
                );
                const auto layer = decoded.getInt(
                    "layer",
                    decoded.getInt("data_layer_id", 0)
                );
                if (layer != 0 || !decoded.has("block_runtime_id")) return;
                const auto rawRuntimeId = static_cast<uint32_t>(
                    decoded.getUInt("block_runtime_id", 0)
                );
                int32_t runtimeId = 0;
                static_assert(sizeof(runtimeId) == sizeof(rawRuntimeId));
                std::memcpy(&runtimeId, &rawRuntimeId, sizeof(runtimeId));
                observeSchematicBlockUpdate(
                    static_cast<int32_t>(decoded.getInt("position.x", 0)),
                    static_cast<int32_t>(decoded.getInt("position.y", 0)),
                    static_cast<int32_t>(decoded.getInt("position.z", 0)),
                    runtimeId
                );
            } catch (...) {
            }
            // Keep both the exact block cache and a visible minimap tile in
            // sync with the server-confirmed primary-layer update.
            return;
        }
        if (!serverbound && name == "update_subchunk_blocks" &&
            (schematicWorldTrackingActive.load(std::memory_order_relaxed) ||
             miniMapEnabled.load(std::memory_order_relaxed))) {
            try {
                std::vector<SchematicBlockChange> changes;
                {
                    std::lock_guard decodeLock(itemDecodeMutex);
                    bedrock::RelayPacketEvent decoded(
                        version,
                        event,
                        itemProtocolVariables,
                        true
                    );
                    const auto* blocks = decoded.value("blocks");
                    if (blocks == nullptr ||
                        blocks->kind != bedrock::PacketValue::Kind::Array) {
                        return;
                    }
                    // A subchunk contains at most 16^3 primary-layer cells.
                    // The protocol's `extra` list is the secondary/waterlogged
                    // layer and must not replace the primary block used by the
                    // schematic matcher.
                    const auto count = std::min<std::size_t>(
                        blocks->arrayValue.size(),
                        4096
                    );
                    changes.reserve(count);
                    for (std::size_t index = 0; index < count; ++index) {
                        const auto& entry = blocks->arrayValue[index];
                        if (entry.kind != bedrock::PacketValue::Kind::Object) {
                            continue;
                        }
                        const auto* position = entry.get("position");
                        if (position == nullptr ||
                            position->kind !=
                                bedrock::PacketValue::Kind::Object) {
                            continue;
                        }
                        const auto x = packetInteger(position->get("x"));
                        const auto y = packetInteger(position->get("y"));
                        const auto z = packetInteger(position->get("z"));
                        auto rawRuntime = packetInteger(entry.get("runtime_id"));
                        if (!rawRuntime.has_value()) {
                            rawRuntime = packetInteger(
                                entry.get("block_runtime_id")
                            );
                        }
                        if (!x.has_value() || !y.has_value() ||
                            !z.has_value() || !rawRuntime.has_value()) {
                            continue;
                        }
                        const auto rawRuntimeId = static_cast<uint32_t>(
                            *rawRuntime
                        );
                        int32_t runtimeId = 0;
                        static_assert(sizeof(runtimeId) == sizeof(rawRuntimeId));
                        std::memcpy(
                            &runtimeId,
                            &rawRuntimeId,
                            sizeof(runtimeId)
                        );
                        changes.push_back({
                            static_cast<int32_t>(*x),
                            static_cast<int32_t>(*y),
                            static_cast<int32_t>(*z),
                            runtimeId
                        });
                    }
                }
                // Apply the whole packet under one world-cache lock and one
                // revision instead of up to 4096 lock/journal operations on
                // the relay callback thread.
                observeSchematicBlockUpdates(changes);
            } catch (...) {
            }
            return;
        }
        const bool relevant = name == "add_item_entity" ||
            name == "mob_equipment" ||
            name == "mob_armor_equipment" ||
            name == "inventory_content" ||
            name == "inventory_slot" ||
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
                if (!serverbound) {
                    resolveLegacyAutomationLocked(equipmentIndex);
                }
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
                if (pendingAutomationEquipmentIndex >= 2 &&
                    pendingAutomationEquipmentIndex < equipment.size()) {
                    resolveLegacyAutomationLocked(
                        pendingAutomationEquipmentIndex
                    );
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
                resolveLegacyAutomationLocked(1);
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
                if (pendingAutomationEquipmentIndex >= 2 &&
                    pendingAutomationEquipmentIndex < equipment.size()) {
                    resolveLegacyAutomationLocked(
                        pendingAutomationEquipmentIndex
                    );
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
                resolveLegacyAutomationLocked(1);
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
                    resolveLegacyAutomationLocked(
                        static_cast<std::size_t>(slot) + 2
                    );
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
        pendingAutomationNetworkId = 0;
        pendingAutomationStackId = 0;
        pendingAutomationEquipmentIndex = 0;
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
        const bool wasMiniMap = miniMapEnabled.exchange(
            miniMap,
            std::memory_order_relaxed
        );
        const bool wasTracking = schematicWorldTrackingActive.exchange(
            schematic,
            std::memory_order_relaxed
        );
        const bool wasSchematic = schematicEnabled.exchange(
            schematic,
            std::memory_order_relaxed
        );
        if (wasSchematic != schematic || wasTracking != schematic ||
            wasMiniMap != miniMap) {
            std::lock_guard worldLock(miniMapMutex);
            if (!schematic && !miniMap) {
                invalidateMiniMapQueueGapLocked(
                    std::numeric_limits<uint64_t>::max()
                );
            } else {
                ++schematicRevision;
            }
        }
        if (!armor && !totem) {
            std::lock_guard lock(mutex);
            pendingAutomationRequestId = 0;
            pendingAutomationNetworkId = 0;
            pendingAutomationStackId = 0;
            pendingAutomationEquipmentIndex = 0;
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
        {"miniMapLevelChunkSectionFallbacks", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->miniMapLevelChunkSectionFallbacks.load(
                std::memory_order_relaxed
            ))
        )},
        {"miniMapSubChunkDecodeFailures", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->miniMapSubChunkDecodeFailures.load(
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
        {"schematicTotalBlocks", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->schematicTotalBlocks.load(
                std::memory_order_relaxed
            ))
        )},
        {"schematicCorrectBlocks", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->schematicCorrectBlocks.load(
                std::memory_order_relaxed
            ))
        )},
        {"schematicMissingBlocks", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->schematicMissingBlocks.load(
                std::memory_order_relaxed
            ))
        )},
        {"schematicWrongBlocks", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->schematicWrongBlocks.load(
                std::memory_order_relaxed
            ))
        )},
        {"schematicUnknownBlocks", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->schematicUnknownBlocks.load(
                std::memory_order_relaxed
            ))
        )},
        {"schematicDisplayedMarkers", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->schematicDisplayedMarkers.load(
                std::memory_order_relaxed
            ))
        )},
        {"schematicMarkerRebuilds", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->schematicMarkerRebuilds.load(
                std::memory_order_relaxed
            ))
        )},
        {"schematicMarkerPackets", bedrock::JsRuntimeValue::number(
            static_cast<double>(state->schematicMarkerPackets.load(
                std::memory_order_relaxed
            ))
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

struct SchematicDebugMarker {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int32_t status = 0;
    std::string expectedBlockState;

    auto operator<=>(const SchematicDebugMarker&) const = default;
};

struct SchematicDebugShapeKey {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint16_t part = 0;

    auto operator<=>(const SchematicDebugShapeKey&) const = default;
};

struct SchematicDebugShape {
    SchematicDebugShapeKey key;
    int32_t status = 0;
    bedrock::BlockShape bounds = bedrock::FullBlockShape;

    bool operator==(const SchematicDebugShape&) const = default;
};

struct ActiveSchematicDebugShape {
    SchematicDebugShape shape;
    uint64_t networkId = 0;
};

constexpr std::size_t MaximumSchematicDebugShapes = 3'600;
constexpr std::size_t SchematicOutlineLayers = 2;
constexpr std::size_t MaximumSchematicBaseShapes =
    MaximumSchematicDebugShapes / SchematicOutlineLayers;
constexpr double SchematicOutlineExpansion = 0.025;

bedrock::BlockShape unionSchematicDebugBounds(
    const std::vector<bedrock::BlockShape>& bounds
) {
    if (bounds.empty()) return bedrock::FullBlockShape;
    auto result = bounds.front();
    for (std::size_t index = 1; index < bounds.size(); ++index) {
        result[0] = std::min(result[0], bounds[index][0]);
        result[1] = std::min(result[1], bounds[index][1]);
        result[2] = std::min(result[2], bounds[index][2]);
        result[3] = std::max(result[3], bounds[index][3]);
        result[4] = std::max(result[4], bounds[index][4]);
        result[5] = std::max(result[5], bounds[index][5]);
    }
    return result;
}

std::vector<SchematicDebugShape> planSchematicDebugShapes(
    const RelayState& state,
    const std::vector<SchematicDebugMarker>& markers
) {
    struct ShapeGroup {
        SchematicDebugMarker marker;
        std::vector<bedrock::BlockShape> bounds;
    };
    std::vector<ShapeGroup> groups;
    groups.reserve(markers.size());
    std::size_t totalParts = 0;
    for (const auto& marker : markers) {
        if (marker.status != 1 && marker.status != 2 && marker.status != 3) {
            continue;
        }
        auto bounds = state.schematicCollisionShapes(
            marker.expectedBlockState
        );
        totalParts += bounds.size();
        groups.push_back({marker, std::move(bounds)});
    }

    // Never drop a whole nearby block merely because several earlier blocks
    // had multipart collision. Collapse farthest multipart groups to one
    // union box until the hard shape budget fits; each selected marker keeps
    // at least one useful outline.
    for (auto group = groups.rbegin();
         totalParts > MaximumSchematicBaseShapes && group != groups.rend();
         ++group) {
        if (group->bounds.size() <= 1) continue;
        totalParts -= group->bounds.size() - 1;
        group->bounds.assign(1, unionSchematicDebugBounds(group->bounds));
    }
    if (groups.size() > MaximumSchematicBaseShapes) {
        groups.resize(MaximumSchematicBaseShapes);
        totalParts = groups.size();
    }

    std::vector<SchematicDebugShape> result;
    result.reserve(std::min(
        totalParts * SchematicOutlineLayers,
        MaximumSchematicDebugShapes
    ));
    for (const auto& group : groups) {
        for (std::size_t part = 0; part < group.bounds.size(); ++part) {
            for (std::size_t layer = 0;
                 layer < SchematicOutlineLayers;
                 ++layer) {
                if (result.size() >= MaximumSchematicDebugShapes) break;
                auto bounds = group.bounds[part];
                const double expansion =
                    SchematicOutlineExpansion * static_cast<double>(layer);
                bounds[0] -= expansion;
                bounds[1] -= expansion;
                bounds[2] -= expansion;
                bounds[3] += expansion;
                bounds[4] += expansion;
                bounds[5] += expansion;
                result.push_back({
                    {
                        group.marker.x,
                        group.marker.y,
                        group.marker.z,
                        static_cast<uint16_t>(
                            part * SchematicOutlineLayers + layer
                        )
                    },
                    group.marker.status,
                    bounds
                });
            }
        }
    }
    std::sort(
        result.begin(),
        result.end(),
        [](const auto& left, const auto& right) {
            return left.key < right.key;
        }
    );
    return result;
}

// Keep relay-owned shapes away from the small sequential IDs commonly used
// by server scripts. The ASCII prefix "CPE" makes accidental overlap with a
// destination server's debug drawer registry practically impossible.
constexpr uint64_t SchematicDebugNetworkIdBase = 0x4350450000000000ULL;
// Filled previews are relay-owned stained-glass falling-block actors. They
// never enter the authoritative chunk and use a separate ID range from debug
// shapes. A neutral full-cell fill avoids presenting a misleading orientation
// when Bedrock ignores directional runtime state on a falling-block entity.
constexpr uint64_t SchematicTextureActorIdBase = 0x4350451000000000ULL;
constexpr std::size_t MaximumSchematicTextureActors = 1'800;
// Keep each clientbound operation comfortably bounded for RakNet queueing and
// as a defence-in-depth limit against oversized schematic allocations, while
// retaining stable keyed IDs across every batch.
constexpr std::size_t SchematicDebugShapesPerPacket = 64;

std::size_t schematicDebugBatchCount(std::size_t shapeCount) noexcept {
    return shapeCount / SchematicDebugShapesPerPacket +
        (shapeCount % SchematicDebugShapesPerPacket != 0 ? 1u : 0u);
}

bool supportsSchematicScriptDebugDrawer(std::string_view version) noexcept {
    // ServerScriptDebugDrawer was added in protocol 818 (Bedrock 1.21.90).
    // Unlike the older DebugRenderer packet, it is the packet used by the
    // current in-world debug-shape API and is rendered by retail 1.21.100.
    return versionAtLeast(version, 1, 21, 90);
}

bedrock::ProtoDefValue schematicDebugVec3(double x, double y, double z) {
    return bedrock::ProtoDefValue::object({
        {"x", bedrock::ProtoDefValue::floating(x)},
        {"y", bedrock::ProtoDefValue::floating(y)},
        {"z", bedrock::ProtoDefValue::floating(z)}
    });
}

int32_t schematicDebugArgb(
    int status,
    int opacityPercent,
    int32_t correctColor,
    int32_t wrongColor,
    int32_t missingColor
) {
    const uint32_t selectedColor = static_cast<uint32_t>(
        status == 3 ? wrongColor : status == 2 ? correctColor : missingColor
    );
    const int effectiveOpacity = std::clamp(opacityPercent, 10, 100);
    const uint32_t alpha = static_cast<uint32_t>(std::lround(
        effectiveOpacity * 255.0 / 100.0
    ));
    const uint32_t red = (selectedColor >> 16u) & 0xffu;
    const uint32_t green = (selectedColor >> 8u) & 0xffu;
    const uint32_t blue = selectedColor & 0xffu;
    // The schema calls this field li32 while Bedrock interprets its bytes as
    // BEARGB. Packing conventional 0xAARRGGBB and writing it little-endian
    // produces the required B,G,R,A byte order.
    const uint32_t argb =
        (alpha << 24u) | (red << 16u) | (green << 8u) | blue;
    return static_cast<int32_t>(argb);
}

std::string_view schematicNearestDyeName(int32_t argb) noexcept {
    struct DyeColor {
        std::string_view name;
        uint32_t rgb;
    };
    static constexpr std::array<DyeColor, 16> Colors {{
        {"white", 0xf9ffffu},
        {"orange", 0xf9801du},
        {"magenta", 0xc74ebdu},
        {"light_blue", 0x3ab3dau},
        {"yellow", 0xfed83du},
        {"lime", 0x80c71fu},
        {"pink", 0xf38baau},
        {"gray", 0x474f52u},
        {"light_gray", 0x9d9d97u},
        {"cyan", 0x169c9cu},
        {"purple", 0x8932b8u},
        {"blue", 0x3c44aau},
        {"brown", 0x835432u},
        {"green", 0x5e7c16u},
        {"red", 0xb02e26u},
        {"black", 0x1d1d21u}
    }};
    const uint32_t rgb = static_cast<uint32_t>(argb) & 0x00ffffffu;
    const int red = static_cast<int>((rgb >> 16u) & 0xffu);
    const int green = static_cast<int>((rgb >> 8u) & 0xffu);
    const int blue = static_cast<int>(rgb & 0xffu);
    const DyeColor* nearest = &Colors.front();
    uint32_t nearestDistance = std::numeric_limits<uint32_t>::max();
    for (const auto& candidate : Colors) {
        const int candidateRed = static_cast<int>(
            (candidate.rgb >> 16u) & 0xffu
        );
        const int candidateGreen = static_cast<int>(
            (candidate.rgb >> 8u) & 0xffu
        );
        const int candidateBlue = static_cast<int>(candidate.rgb & 0xffu);
        const int deltaRed = red - candidateRed;
        const int deltaGreen = green - candidateGreen;
        const int deltaBlue = blue - candidateBlue;
        const uint32_t distance = static_cast<uint32_t>(
            deltaRed * deltaRed + deltaGreen * deltaGreen +
            deltaBlue * deltaBlue
        );
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = &candidate;
        }
    }
    return nearest->name;
}

bedrock::VersionedGamePacket makeLegacySchematicDebugClearPacket(
    const bedrock::ProtoDefPacketEncoder& encoder,
    const bedrock::VersionedPacketCodec& packetCodec
) {
    const auto payload = encoder.encodePacket(
        "debug_renderer",
        bedrock::ProtoDefValue::object({
            {"type", bedrock::ProtoDefValue::string("clear")}
        })
    );
    return packetCodec.makePacketByName("debug_renderer", payload);
}

bedrock::VersionedGamePacket makeLegacySchematicDebugCubePacket(
    const bedrock::ProtoDefPacketEncoder& encoder,
    const bedrock::VersionedPacketCodec& packetCodec,
    const SchematicDebugMarker& marker,
    int opacityPercent,
    int32_t correctColor,
    int32_t wrongColor,
    int32_t missingColor
) {
    const double alpha = std::clamp(opacityPercent, 10, 100) / 100.0;
    const uint32_t color = static_cast<uint32_t>(
        marker.status == 3
            ? wrongColor
            : marker.status == 2 ? correctColor : missingColor
    );
    const auto payload = encoder.encodePacket(
        "debug_renderer",
        bedrock::ProtoDefValue::object({
            {"type", bedrock::ProtoDefValue::string("add_cube")},
            {"text", bedrock::ProtoDefValue::string("")},
            {"position", bedrock::ProtoDefValue::object({
                // Debug marker cubes are centred on their Vec3. Block
                // coordinates name a corner, so use the exact block centre.
                {"x", bedrock::ProtoDefValue::floating(marker.x + 0.5)},
                {"y", bedrock::ProtoDefValue::floating(marker.y + 0.5)},
                {"z", bedrock::ProtoDefValue::floating(marker.z + 0.5)}
            })},
            {"red", bedrock::ProtoDefValue::floating(
                ((color >> 16u) & 0xffu) / 255.0
            )},
            {"green", bedrock::ProtoDefValue::floating(
                ((color >> 8u) & 0xffu) / 255.0
            )},
            {"blue", bedrock::ProtoDefValue::floating(
                (color & 0xffu) / 255.0
            )},
            {"alpha", bedrock::ProtoDefValue::floating(alpha)},
            // Rebuilds are event-driven; keep unchanged markers alive for a
            // full play session without periodic packet spam.
            {"duration", bedrock::ProtoDefValue::integer(86'400'000)}
        })
    );
    return packetCodec.makePacketByName("debug_renderer", payload);
}

bedrock::VersionedGamePacket makeSchematicScriptDebugDrawerPacket(
    const bedrock::ProtoDefPacketEncoder& encoder,
    const bedrock::VersionedPacketCodec& packetCodec,
    std::string_view version,
    const std::vector<ActiveSchematicDebugShape>& activeShapes,
    int opacityPercent,
    int32_t correctColor,
    int32_t wrongColor,
    int32_t missingColor,
    std::size_t shapeOffset,
    std::size_t shapeCount
) {
    if (shapeOffset > activeShapes.size() ||
        shapeCount > activeShapes.size() - shapeOffset ||
        shapeCount > SchematicDebugShapesPerPacket) {
        throw std::out_of_range("invalid schematic debug shape batch");
    }
    std::vector<bedrock::ProtoDefValue> shapes;
    shapes.reserve(shapeCount);
    const bool locationIsCenter = versionAtLeast(version, 1, 26, 10);
    for (std::size_t index = shapeOffset;
         index < shapeOffset + shapeCount;
         ++index) {
        const auto& active = activeShapes[index];
        const auto& shape = active.shape;
        const auto& bounds = shape.bounds;
        const double minimumX = shape.key.x + bounds[0];
        const double minimumY = shape.key.y + bounds[1];
        const double minimumZ = shape.key.z + bounds[2];
        const double sizeX = bounds[3] - bounds[0];
        const double sizeY = bounds[4] - bounds[1];
        const double sizeZ = bounds[5] - bounds[2];
        // Before 1.26.10 DebugBox Location is its minimum corner. Starting in
        // 1.26.10 the client fixed it to be the centre. box_bound is the full
        // dimension on both versions, so collision AABBs can be used without
        // per-version scaling. All unused option fields remain explicit nulls
        // so the generated protocol codec emits their presence flags.
        shapes.push_back(bedrock::ProtoDefValue::object({
            {"network_id", bedrock::ProtoDefValue::uinteger(
                active.networkId
            )},
            {"shape_type", bedrock::ProtoDefValue::string("box")},
            {"location", schematicDebugVec3(
                locationIsCenter ? minimumX + sizeX * 0.5 : minimumX,
                locationIsCenter ? minimumY + sizeY * 0.5 : minimumY,
                locationIsCenter ? minimumZ + sizeZ * 0.5 : minimumZ
            )},
            {"scale", bedrock::ProtoDefValue::floating(1.0)},
            {"rotation", bedrock::ProtoDefValue::null()},
            {"time_left", bedrock::ProtoDefValue::null()},
            {"color", bedrock::ProtoDefValue::integer(
                schematicDebugArgb(
                    shape.status,
                    opacityPercent,
                    correctColor,
                    wrongColor,
                    missingColor
                )
            )},
            {"text", bedrock::ProtoDefValue::null()},
            {"box_bound", schematicDebugVec3(sizeX, sizeY, sizeZ)},
            {"line_end_location", bedrock::ProtoDefValue::null()},
            {"arrow_head_length", bedrock::ProtoDefValue::null()},
            {"arrow_head_radius", bedrock::ProtoDefValue::null()},
            {"segment_count", bedrock::ProtoDefValue::null()}
        }));
    }

    const auto payload = encoder.encodePacket(
        "server_script_debug_drawer",
        bedrock::ProtoDefValue::object({
            {"shapes", bedrock::ProtoDefValue::array(std::move(shapes))}
        })
    );
    return packetCodec.makePacketByName(
        "server_script_debug_drawer",
        payload
    );
}

bedrock::VersionedGamePacket makeSchematicScriptDebugRemovalPacket(
    const bedrock::ProtoDefPacketEncoder& encoder,
    const bedrock::VersionedPacketCodec& packetCodec,
    const std::vector<uint64_t>& networkIds,
    std::size_t shapeOffset,
    std::size_t shapeCount
) {
    if (shapeOffset > networkIds.size() ||
        shapeCount > networkIds.size() - shapeOffset ||
        shapeCount > SchematicDebugShapesPerPacket) {
        throw std::out_of_range("invalid schematic debug removal batch");
    }
    std::vector<bedrock::ProtoDefValue> removals;
    removals.reserve(shapeCount);
    for (std::size_t index = shapeOffset;
         index < shapeOffset + shapeCount;
         ++index) {
        // A shape record containing only its key is the protocol removal
        // operation. Do not rely on an empty packet: that is a no-op on the
        // retail client rather than a registry-wide clear.
        removals.push_back(bedrock::ProtoDefValue::object({
            {"network_id", bedrock::ProtoDefValue::uinteger(
                networkIds[index]
            )},
            {"shape_type", bedrock::ProtoDefValue::null()},
            {"location", bedrock::ProtoDefValue::null()},
            {"scale", bedrock::ProtoDefValue::null()},
            {"rotation", bedrock::ProtoDefValue::null()},
            {"time_left", bedrock::ProtoDefValue::null()},
            {"color", bedrock::ProtoDefValue::null()},
            {"text", bedrock::ProtoDefValue::null()},
            {"box_bound", bedrock::ProtoDefValue::null()},
            {"line_end_location", bedrock::ProtoDefValue::null()},
            {"arrow_head_length", bedrock::ProtoDefValue::null()},
            {"arrow_head_radius", bedrock::ProtoDefValue::null()},
            {"segment_count", bedrock::ProtoDefValue::null()}
        }));
    }
    const auto payload = encoder.encodePacket(
        "server_script_debug_drawer",
        bedrock::ProtoDefValue::object({
            {"shapes", bedrock::ProtoDefValue::array(std::move(removals))}
        })
    );
    return packetCodec.makePacketByName(
        "server_script_debug_drawer",
        payload
    );
}

std::vector<bedrock::VersionedGamePacket> makeSchematicDebugClearPackets(
    const std::string& version,
    const std::vector<ActiveSchematicDebugShape>& activeShapes
) {
    bedrock::ProtoDefPacketEncoder encoder(version);
    const auto codec = bedrock::VersionedMcpeCodec::forVersion(version);
    if (supportsSchematicScriptDebugDrawer(version)) {
        std::vector<bedrock::VersionedGamePacket> packets;
        std::vector<uint64_t> networkIds;
        networkIds.reserve(activeShapes.size());
        for (const auto& active : activeShapes) {
            networkIds.push_back(active.networkId);
        }
        std::sort(networkIds.begin(), networkIds.end());
        networkIds.erase(
            std::unique(networkIds.begin(), networkIds.end()),
            networkIds.end()
        );
        packets.reserve(schematicDebugBatchCount(networkIds.size()));
        for (std::size_t offset = 0; offset < networkIds.size();
             offset += SchematicDebugShapesPerPacket) {
            const auto count = std::min(
                SchematicDebugShapesPerPacket,
                networkIds.size() - offset
            );
            packets.push_back(makeSchematicScriptDebugRemovalPacket(
                encoder,
                codec.packetCodec(),
                networkIds,
                offset,
                count
            ));
        }
        return packets;
    }
    return {makeLegacySchematicDebugClearPacket(encoder, codec.packetCodec())};
}

struct SchematicTextureActor {
    SchematicDebugMarker marker;
    int32_t blockRuntimeId = 0;
    int textureOpacityPercent = 100;

    auto operator<=>(const SchematicTextureActor&) const = default;
};

struct ActiveSchematicTextureActor {
    SchematicTextureActor actor;
    uint64_t entityId = 0;
};

bedrock::ProtoDefValue schematicEntityMetadataEntry(
    std::string key,
    std::string type,
    bedrock::ProtoDefValue value
) {
    // MetadataDictionary first switches on the key and ordinary keys then
    // enter a second switch selected by their metadata type. Keep both switch
    // contexts explicit; passing the scalar directly only compiles and then
    // fails when ProtoDef tries to encode add_entity at runtime.
    auto switchValue = key == "flags"
        ? std::move(value)
        : bedrock::ProtoDefValue::object({
            {"type", bedrock::ProtoDefValue::string(type)},
            {"$value", std::move(value)}
        });
    return bedrock::ProtoDefValue::object({
        {"key", bedrock::ProtoDefValue::string(std::move(key))},
        {"type", bedrock::ProtoDefValue::string(std::move(type))},
        {"value", std::move(switchValue)}
    });
}

bedrock::VersionedGamePacket makeSchematicTextureActorPacket(
    const bedrock::ProtoDefPacketEncoder& encoder,
    const bedrock::VersionedPacketCodec& packetCodec,
    const SchematicTextureActor& actor,
    uint64_t entityId
) {
    std::vector<bedrock::ProtoDefValue> metadata;
    metadata.reserve(5);
    metadata.push_back(schematicEntityMetadataEntry(
        "flags",
        "long",
        bedrock::ProtoDefValue::object({
            {"no_ai", bedrock::ProtoDefValue::boolean(true)}
        })
    ));
    metadata.push_back(schematicEntityMetadataEntry(
        "variant",
        "int",
        bedrock::ProtoDefValue::integer(actor.blockRuntimeId)
    ));
    metadata.push_back(schematicEntityMetadataEntry(
        "scale",
        "float",
        // Keep the translucent fill inside the authoritative one-block grid.
        // Bedrock exposes no alpha metadata for a falling-block actor, so the
        // density setting is represented by a small inset.
        bedrock::ProtoDefValue::floating(
            0.94 + std::clamp(actor.textureOpacityPercent, 10, 100) * 0.0004
        )
    ));
    for (const auto* key : {"boundingbox_width", "boundingbox_height"}) {
        metadata.push_back(schematicEntityMetadataEntry(
            key,
            "float",
            bedrock::ProtoDefValue::floating(0.0)
        ));
    }

    const auto emptyArray = [] {
        return bedrock::ProtoDefValue::array(
            std::vector<bedrock::ProtoDefValue> {}
        );
    };
    const auto payload = encoder.encodePacket(
        "add_entity",
        bedrock::ProtoDefValue::object({
            {"unique_id", bedrock::ProtoDefValue::integer(
                static_cast<int64_t>(entityId)
            )},
            {"runtime_id", bedrock::ProtoDefValue::uinteger(entityId)},
            {"entity_type", bedrock::ProtoDefValue::string(
                "minecraft:falling_block"
            )},
            {"position", schematicDebugVec3(
                actor.marker.x + 0.5,
                actor.marker.y + 0.49,
                actor.marker.z + 0.5
            )},
            {"velocity", schematicDebugVec3(0.0, 0.0, 0.0)},
            {"pitch", bedrock::ProtoDefValue::floating(0.0)},
            {"yaw", bedrock::ProtoDefValue::floating(0.0)},
            {"head_yaw", bedrock::ProtoDefValue::floating(0.0)},
            {"body_yaw", bedrock::ProtoDefValue::floating(0.0)},
            {"attributes", emptyArray()},
            {"metadata", bedrock::ProtoDefValue::array(std::move(metadata))},
            {"properties", bedrock::ProtoDefValue::object({
                {"ints", emptyArray()},
                {"floats", emptyArray()}
            })},
            {"links", emptyArray()}
        })
    );
    return packetCodec.makePacketByName("add_entity", payload);
}

bedrock::VersionedGamePacket makeSchematicTextureActorRemovalPacket(
    const bedrock::ProtoDefPacketEncoder& encoder,
    const bedrock::VersionedPacketCodec& packetCodec,
    uint64_t entityId
) {
    const auto payload = encoder.encodePacket(
        "remove_entity",
        bedrock::ProtoDefValue::object({
            {"entity_id_self", bedrock::ProtoDefValue::integer(
                static_cast<int64_t>(entityId)
            )}
        })
    );
    return packetCodec.makePacketByName("remove_entity", payload);
}

void appendSchematicTextureActorRemovals(
    std::vector<bedrock::VersionedGamePacket>& packets,
    const bedrock::ProtoDefPacketEncoder& encoder,
    const bedrock::VersionedPacketCodec& packetCodec,
    const std::vector<ActiveSchematicTextureActor>& actors
) {
    for (const auto& actor : actors) {
        packets.push_back(makeSchematicTextureActorRemovalPacket(
            encoder,
            packetCodec,
            actor.entityId
        ));
    }
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
        // Minecraft on Android may stop its loopback RakNet worker for tens
        // of seconds while loading resources, switching UI or under GC/GPU
        // pressure. A fresh connection still replaces this one immediately.
        options.downstreamRaknetTimeoutMs = 120'000;
        options.upstreamRaknetTimeoutMs = 120'000;
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
                " downstreamRaknetTimeoutMs=" + std::to_string(
                    options.downstreamRaknetTimeoutMs
                ) +
                " upstreamRaknetTimeoutMs=" + std::to_string(
                    options.upstreamRaknetTimeoutMs
                ) +
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
                    !event.message.empty() ||
                    !rakNetCloseSignal(event.raknetPacketId).empty();
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
            const auto closeSignal = event.kind ==
                    bedrock::BedrockServerTransportEventKind::Receive
                ? rakNetCloseSignal(event.raknetPacketId)
                : std::string_view {};
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
            } else if (!closeSignal.empty()) {
                state->push(
                    "raknet_close_signal",
                    breadcrumb + " reason=" + std::string(closeSignal),
                    closeSignal == "remote_disconnect_notification"
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
            attachSchematicDownstream(player.connection, player.sessionId());
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
            detachSchematicDownstream(player.sessionId());
            state->entityPositions.clear();
            state->clearGameplayTelemetry();
            state->push(
                "disconnect",
                "origin=session_terminated downstream_session=" +
                    player.sessionId() +
                    "; inspect the preceding raknet_close_signal or relay "
                    "error for the initiating side; matching upstream was "
                    "notified and closed; "
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
        relay->live().onClientbound([this, state, version](
            bedrock::BedrockRelayPacketEvent& event
        ) {
            state->entityPositions.observeClientbound(event.packet);
            state->observeDecodedGameplayPacket(version, event, false);
            state->enqueueMiniMapChunk(version, event.packet);
            if (event.packet.name == "start_game" ||
                event.packet.name == "change_dimension") {
                if (auto clearPackets = takeSchematicLifecycleClear(version);
                    clearPackets.has_value()) {
                    std::vector<bedrock::VersionedGamePacket> replacement;
                    replacement.reserve(clearPackets->size() + 1u);
                    replacement.push_back(event.packet);
                    for (auto& clearPacket : *clearPackets) {
                        replacement.push_back(std::move(clearPacket));
                    }
                    event.replace(std::move(replacement));
                }
            }
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
            if (event.packet.name == "network_chunk_publisher_update") {
                const bool retentionEnabled =
                    state->chunkRetentionEnabled.load(
                        std::memory_order_relaxed
                    );
                const auto requestedRadiusChunks =
                    state->retainedRadiusChunks.load(
                        std::memory_order_relaxed
                    );
                // Inspect a copy for diagnostics. The publisher update itself
                // must be forwarded byte-for-byte as sent by the server.
                auto inspectedPacket = event.packet;
                const auto retention = bedrock::retainPublishedChunks(
                    inspectedPacket,
                    0
                );
                if (retention.decoded) {
                    state->observeSchematicPublisherWindow(
                        retention.centerXBlocks,
                        retention.centerZBlocks,
                        retention.originalRadiusBlocks
                    );
                    // Schematic cache eviction still follows the actual
                    // client's publisher window even when optional chunk
                    // retention is disabled.
                    if (retentionEnabled) {
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
                                    " effectiveRadiusBlocks=" + std::to_string(
                                    retention.effectiveRadiusBlocks
                                ) + " publisherUpdates=" +
                                    std::to_string(observed) +
                                    " rewrittenPackets=" +
                                    std::to_string(rewritten) +
                                    " forwardedUnchanged=true",
                                "DEBUG",
                                "chunks"
                            );
                        }
                    }
                } else if (retention.recognized && retentionEnabled) {
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

    bool replaceSchematicDebugMarkers(
        std::vector<SchematicDebugMarker> markers,
        bool texturesEnabled,
        int textureOpacityPercent,
        bool outlinesEnabled,
        int outlineOpacityPercent,
        int32_t correctOutlineColor,
        int32_t wrongOutlineColor,
        int32_t missingOutlineColor,
        uint64_t total,
        uint64_t correct,
        uint64_t missing,
        uint64_t wrong,
        uint64_t unknown,
        uint64_t expectedWorldRevision,
        int32_t expectedDimension
    ) noexcept {
        try {
            std::lock_guard sendLock(schematicSendMutex_);
            if (!state_->schematicSnapshotMatches(
                    expectedWorldRevision,
                    expectedDimension
                )) {
                return false;
            }
            constexpr std::size_t MaximumDebugMarkers = 1'800;
            markers.erase(
                std::remove_if(
                    markers.begin(),
                    markers.end(),
                    [](const auto& marker) {
                        return marker.status != 0 && marker.status != 1 &&
                            marker.status != 2 && marker.status != 3;
                    }
                ),
                markers.end()
            );
            // Java supplies nearest-first markers. One world cell can own only
            // one preview, regardless of whether a duplicated record carries
            // a different transient world status.
            std::set<std::array<int32_t, 3>> seenMarkerCells;
            markers.erase(
                std::remove_if(
                    markers.begin(),
                    markers.end(),
                    [&](const auto& marker) {
                        return !seenMarkerCells.insert({
                            marker.x,
                            marker.y,
                            marker.z
                        }).second;
                    }
                ),
                markers.end()
            );
            if (markers.size() > MaximumDebugMarkers) {
                markers.resize(MaximumDebugMarkers);
            }
            textureOpacityPercent = std::clamp(
                textureOpacityPercent,
                10,
                100
            );
            outlineOpacityPercent = std::clamp(
                outlineOpacityPercent,
                10,
                100
            );

            const bool scriptDebug = supportsSchematicScriptDebugDrawer(
                state_->version
            );
            std::vector<SchematicTextureActor> textureActors;
            std::vector<SchematicDebugMarker> legacyWrongMarkers;
            std::vector<SchematicDebugShape> debugShapes;
            if (scriptDebug && texturesEnabled) {
                textureActors.reserve(MaximumSchematicTextureActors);
                const std::string dyeName(schematicNearestDyeName(
                    missingOutlineColor
                ));
                auto fillRuntimeId = state_->schematicRuntimeId(
                    "minecraft:" + dyeName + "_stained_glass[]"
                );
                if (!fillRuntimeId.has_value()) {
                    fillRuntimeId = state_->schematicRuntimeId(
                        "minecraft:stained_glass[color=" + dyeName + "]"
                    );
                }
                for (auto marker : markers) {
                    if ((marker.status != 0 && marker.status != 1) ||
                        textureActors.size() >= MaximumSchematicTextureActors) {
                        continue;
                    }
                    if (!fillRuntimeId.has_value()) continue;
                    // UNKNOWN and MISSING render identically. The fill is a
                    // colored cell marker, while the double collision outline
                    // carries the exact state geometry and orientation.
                    marker.status = 1;
                    marker.expectedBlockState.clear();
                    textureActors.push_back({
                        std::move(marker),
                        *fillRuntimeId,
                        textureOpacityPercent
                    });
                }
            }
            if (scriptDebug && outlinesEnabled) {
                debugShapes = planSchematicDebugShapes(*state_, markers);
            } else if (!scriptDebug && outlinesEnabled) {
                for (auto marker : markers) {
                    if (marker.status != 3) continue;
                    marker.expectedBlockState.clear();
                    legacyWrongMarkers.push_back(std::move(marker));
                }
                debugShapes = planSchematicDebugShapes(
                    *state_,
                    legacyWrongMarkers
                );
            }
            std::sort(textureActors.begin(), textureActors.end());
            std::sort(legacyWrongMarkers.begin(), legacyWrongMarkers.end());
            const std::size_t displayedTextureActors = textureActors.size();
            std::set<std::array<int32_t, 3>> displayedCells;
            for (const auto& actor : textureActors) {
                displayedCells.insert({
                    actor.marker.x,
                    actor.marker.y,
                    actor.marker.z
                });
            }
            for (const auto& shape : debugShapes) {
                displayedCells.insert({
                    shape.key.x,
                    shape.key.y,
                    shape.key.z
                });
            }
            const std::size_t displayedMarkers = displayedCells.size();

            bedrock::BedrockServerConnection downstream;
            std::string downstreamSessionId;
            uint64_t markerGeneration = 0;
            uint64_t nextDebugShapeId = 0;
            uint64_t nextTextureActorId = 0;
            std::vector<ActiveSchematicDebugShape> previousDebugShapes;
            std::vector<ActiveSchematicTextureActor> previousTextureActors;
            int previousOpacity = 0;
            int32_t previousCorrectColor = 0;
            int32_t previousWrongColor = 0;
            int32_t previousMissingColor = 0;
            {
                std::lock_guard markerLock(schematicMarkerMutex_);
                if (!state_->schematicSnapshotMatches(
                        expectedWorldRevision,
                        expectedDimension
                    )) {
                    return false;
                }
                state_->schematicTotalBlocks.store(total, std::memory_order_relaxed);
                state_->schematicCorrectBlocks.store(correct, std::memory_order_relaxed);
                state_->schematicMissingBlocks.store(missing, std::memory_order_relaxed);
                state_->schematicWrongBlocks.store(wrong, std::memory_order_relaxed);
                state_->schematicUnknownBlocks.store(unknown, std::memory_order_relaxed);
                state_->schematicDisplayedMarkers.store(
                    displayedMarkers,
                    std::memory_order_relaxed
                );
                const bool textureActorsUnchanged =
                    textureActors.size() == activeSchematicTextureActors_.size() &&
                    std::equal(
                        textureActors.begin(),
                        textureActors.end(),
                        activeSchematicTextureActors_.begin(),
                        [](const auto& desired, const auto& active) {
                            return desired == active.actor;
                        }
                    );
                const bool debugShapesUnchanged =
                    debugShapes.size() == activeSchematicDebugShapes_.size() &&
                    std::equal(
                        debugShapes.begin(),
                        debugShapes.end(),
                        activeSchematicDebugShapes_.begin(),
                        [](const auto& desired, const auto& active) {
                            return desired == active.shape;
                        }
                    );
                if (debugShapesUnchanged &&
                    textureActorsUnchanged &&
                    (debugShapes.empty() ||
                     (outlineOpacityPercent == activeSchematicOpacity_ &&
                      correctOutlineColor == activeSchematicCorrectColor_ &&
                      wrongOutlineColor == activeSchematicWrongColor_ &&
                      missingOutlineColor == activeSchematicMissingColor_))) {
                    return true;
                }
                if (!schematicDownstream_.has_value()) {
                    activeSchematicDebugShapes_.clear();
                    activeSchematicTextureActors_.clear();
                    activeSchematicOpacity_ = outlineOpacityPercent;
                    activeSchematicCorrectColor_ = correctOutlineColor;
                    activeSchematicWrongColor_ = wrongOutlineColor;
                    activeSchematicMissingColor_ = missingOutlineColor;
                    state_->schematicDisplayedMarkers.store(
                        0,
                        std::memory_order_relaxed
                    );
                    return false;
                }
                downstream = *schematicDownstream_;
                downstreamSessionId = schematicDownstreamSessionId_;
                markerGeneration = schematicMarkerGeneration_;
                nextDebugShapeId = nextSchematicDebugShapeId_;
                nextTextureActorId = nextSchematicTextureActorId_;
                previousDebugShapes = activeSchematicDebugShapes_;
                previousTextureActors = activeSchematicTextureActors_;
                previousOpacity = activeSchematicOpacity_;
                previousCorrectColor = activeSchematicCorrectColor_;
                previousWrongColor = activeSchematicWrongColor_;
                previousMissingColor = activeSchematicMissingColor_;
            }

            std::vector<ActiveSchematicTextureActor> nextTextureActors;
            std::vector<ActiveSchematicTextureActor> addedTextureActors;
            std::vector<ActiveSchematicTextureActor> removedTextureActors;
            nextTextureActors.reserve(textureActors.size());
            addedTextureActors.reserve(textureActors.size());
            removedTextureActors.reserve(previousTextureActors.size());
            std::size_t previousIndex = 0;
            std::size_t desiredIndex = 0;
            while (previousIndex < previousTextureActors.size() ||
                   desiredIndex < textureActors.size()) {
                if (desiredIndex >= textureActors.size() ||
                    (previousIndex < previousTextureActors.size() &&
                     previousTextureActors[previousIndex].actor <
                        textureActors[desiredIndex])) {
                    removedTextureActors.push_back(
                        previousTextureActors[previousIndex++]
                    );
                    continue;
                }
                if (previousIndex >= previousTextureActors.size() ||
                    textureActors[desiredIndex] <
                        previousTextureActors[previousIndex].actor) {
                    if (nextTextureActorId ==
                        std::numeric_limits<uint64_t>::max()) {
                        throw std::overflow_error(
                            "schematic texture actor id space exhausted"
                        );
                    }
                    ActiveSchematicTextureActor added {
                        textureActors[desiredIndex++],
                        nextTextureActorId++
                    };
                    nextTextureActors.push_back(added);
                    addedTextureActors.push_back(std::move(added));
                    continue;
                }
                nextTextureActors.push_back(
                    previousTextureActors[previousIndex]
                );
                ++previousIndex;
                ++desiredIndex;
            }

            std::vector<ActiveSchematicDebugShape> nextDebugShapes;
            std::vector<ActiveSchematicDebugShape> changedDebugShapes;
            std::vector<uint64_t> removedDebugShapeIds;
            nextDebugShapes.reserve(debugShapes.size());
            changedDebugShapes.reserve(debugShapes.size());
            removedDebugShapeIds.reserve(previousDebugShapes.size());
            previousIndex = 0;
            desiredIndex = 0;
            while (previousIndex < previousDebugShapes.size() ||
                   desiredIndex < debugShapes.size()) {
                if (desiredIndex >= debugShapes.size() ||
                    (previousIndex < previousDebugShapes.size() &&
                     previousDebugShapes[previousIndex].shape.key <
                        debugShapes[desiredIndex].key)) {
                    removedDebugShapeIds.push_back(
                        previousDebugShapes[previousIndex++].networkId
                    );
                    continue;
                }
                if (previousIndex >= previousDebugShapes.size() ||
                    debugShapes[desiredIndex].key <
                        previousDebugShapes[previousIndex].shape.key) {
                    if (nextDebugShapeId >= SchematicTextureActorIdBase) {
                        throw std::overflow_error(
                            "schematic debug shape id space exhausted"
                        );
                    }
                    ActiveSchematicDebugShape added {
                        debugShapes[desiredIndex++],
                        nextDebugShapeId++
                    };
                    nextDebugShapes.push_back(added);
                    changedDebugShapes.push_back(std::move(added));
                    continue;
                }
                ActiveSchematicDebugShape retained {
                    debugShapes[desiredIndex],
                    previousDebugShapes[previousIndex].networkId
                };
                if (retained.shape !=
                        previousDebugShapes[previousIndex].shape ||
                    outlineOpacityPercent != previousOpacity ||
                    correctOutlineColor != previousCorrectColor ||
                    wrongOutlineColor != previousWrongColor ||
                    missingOutlineColor != previousMissingColor) {
                    changedDebugShapes.push_back(retained);
                }
                nextDebugShapes.push_back(std::move(retained));
                ++previousIndex;
                ++desiredIndex;
            }

            bedrock::ProtoDefPacketEncoder debugEncoder(state_->version);
            const auto debugCodec = bedrock::VersionedMcpeCodec::forVersion(
                state_->version
            );
            std::vector<bedrock::VersionedGamePacket> packets;
            std::string_view backend;
            if (scriptDebug) {
                packets.reserve(
                    addedTextureActors.size() + removedTextureActors.size() +
                    schematicDebugBatchCount(changedDebugShapes.size()) +
                    schematicDebugBatchCount(removedDebugShapeIds.size())
                );
                // Alternate bounded additions and removals. On an anchor move
                // the next ghosts become visible before the old ones disappear,
                // while the temporary entity count stays close to the cap.
                std::size_t addedIndex = 0;
                std::size_t removedIndex = 0;
                while (addedIndex < addedTextureActors.size() ||
                       removedIndex < removedTextureActors.size()) {
                    const auto addedEnd = std::min(
                        addedIndex + SchematicDebugShapesPerPacket,
                        addedTextureActors.size()
                    );
                    for (; addedIndex < addedEnd; ++addedIndex) {
                        const auto& actor = addedTextureActors[addedIndex];
                        packets.push_back(makeSchematicTextureActorPacket(
                            debugEncoder,
                            debugCodec.packetCodec(),
                            actor.actor,
                            actor.entityId
                        ));
                    }
                    const auto removedEnd = std::min(
                        removedIndex + SchematicDebugShapesPerPacket,
                        removedTextureActors.size()
                    );
                    for (; removedIndex < removedEnd; ++removedIndex) {
                        packets.push_back(makeSchematicTextureActorRemovalPacket(
                            debugEncoder,
                            debugCodec.packetCodec(),
                            removedTextureActors[removedIndex].entityId
                        ));
                    }
                }

                // Stable keys retain the same network ID when status or
                // geometry changes. Publish replacements first, then remove
                // only the exact IDs whose block/part disappeared.
                for (std::size_t offset = 0;
                     offset < changedDebugShapes.size();
                     offset += SchematicDebugShapesPerPacket) {
                    const auto count = std::min(
                        SchematicDebugShapesPerPacket,
                        changedDebugShapes.size() - offset
                    );
                    packets.push_back(makeSchematicScriptDebugDrawerPacket(
                        debugEncoder,
                        debugCodec.packetCodec(),
                        state_->version,
                        changedDebugShapes,
                        outlineOpacityPercent,
                        correctOutlineColor,
                        wrongOutlineColor,
                        missingOutlineColor,
                        offset,
                        count
                    ));
                }
                for (std::size_t offset = 0;
                     offset < removedDebugShapeIds.size();
                     offset += SchematicDebugShapesPerPacket) {
                    const auto count = std::min(
                        SchematicDebugShapesPerPacket,
                        removedDebugShapeIds.size() - offset
                    );
                    packets.push_back(makeSchematicScriptDebugRemovalPacket(
                        debugEncoder,
                        debugCodec.packetCodec(),
                        removedDebugShapeIds,
                        offset,
                        count
                    ));
                }
                backend = texturesEnabled && outlinesEnabled
                    ? "server_script_debug_drawer+stained_glass_fill"
                    : texturesEnabled
                        ? "stained_glass_fill"
                        : "server_script_debug_drawer";
            } else {
                packets.reserve(legacyWrongMarkers.size() + 1);
                packets.push_back(makeLegacySchematicDebugClearPacket(
                    debugEncoder,
                    debugCodec.packetCodec()
                ));
                for (const auto& marker : legacyWrongMarkers) {
                    packets.push_back(makeLegacySchematicDebugCubePacket(
                        debugEncoder,
                        debugCodec.packetCodec(),
                        marker,
                        outlineOpacityPercent,
                        correctOutlineColor,
                        wrongOutlineColor,
                        missingOutlineColor
                    ));
                }
                backend = "legacy_debug_renderer";
            }

            const bool queued = queueSchematicPacketsBatched(
                downstream,
                packets
            );
            bool staleLifecycle = false;
            bool staleDimension = false;
            {
                std::lock_guard markerLock(schematicMarkerMutex_);
                staleLifecycle = schematicMarkerGeneration_ != markerGeneration ||
                    schematicDownstreamSessionId_ != downstreamSessionId;
                staleDimension = !state_->schematicDimensionMatches(
                    expectedDimension
                );
                if (queued && !staleLifecycle && !staleDimension) {
                    activeSchematicDebugShapes_ = std::move(nextDebugShapes);
                    activeSchematicTextureActors_ =
                        std::move(nextTextureActors);
                    activeSchematicOpacity_ = outlineOpacityPercent;
                    activeSchematicCorrectColor_ = correctOutlineColor;
                    activeSchematicWrongColor_ = wrongOutlineColor;
                    activeSchematicMissingColor_ = missingOutlineColor;
                    nextSchematicDebugShapeId_ = nextDebugShapeId;
                    nextSchematicTextureActorId_ = nextTextureActorId;
                }
            }
            if (!queued || staleLifecycle || staleDimension) {
                // A dimension/lifecycle transition can interleave between the
                // bounded outer batches. Remove every ID that belonged to
                // either side of this delta; extra removals are harmless and
                // prevent a partially delivered plan from leaking forward.
                std::vector<ActiveSchematicDebugShape> rollbackShapes =
                    previousDebugShapes;
                rollbackShapes.insert(
                    rollbackShapes.end(),
                    nextDebugShapes.begin(),
                    nextDebugShapes.end()
                );
                auto rollbackPackets = makeSchematicDebugClearPackets(
                    state_->version,
                    rollbackShapes
                );
                appendSchematicTextureActorRemovals(
                    rollbackPackets,
                    debugEncoder,
                    debugCodec.packetCodec(),
                    nextTextureActors
                );
                appendSchematicTextureActorRemovals(
                    rollbackPackets,
                    debugEncoder,
                    debugCodec.packetCodec(),
                    removedTextureActors
                );
                queueSchematicPacketsBatched(downstream, rollbackPackets);
                if (!staleLifecycle) {
                    std::lock_guard markerLock(schematicMarkerMutex_);
                    activeSchematicDebugShapes_.clear();
                    activeSchematicTextureActors_.clear();
                    activeSchematicOpacity_ = 0;
                    ++schematicMarkerGeneration_;
                }
                resetSchematicCounters();
                return false;
            }
            const auto rebuild = state_->schematicMarkerRebuilds.fetch_add(
                1,
                std::memory_order_relaxed
            ) + 1;
            state_->schematicMarkerPackets.fetch_add(
                packets.size(),
                std::memory_order_relaxed
            );
            state_->push(
                "schematic_markers",
                "backend=" + std::string(backend) + " total=" +
                    std::to_string(total) + " correct=" +
                    std::to_string(correct) + " missing=" +
                    std::to_string(missing) + " wrong=" +
                    std::to_string(wrong) + " unknown=" +
                    std::to_string(unknown) + " displayed=" +
                    std::to_string(displayedMarkers) +
                    " textured=" +
                    std::to_string(displayedTextureActors) +
                    " shapeFrames=" +
                    std::to_string(previousDebugShapes.size()) + "->" +
                    std::to_string(debugShapes.size()) +
                    " shapeDelta=+~" +
                    std::to_string(changedDebugShapes.size()) + "/-" +
                    std::to_string(removedDebugShapeIds.size()) +
                    " actorDelta=+" +
                    std::to_string(addedTextureActors.size()) + "/-" +
                    std::to_string(removedTextureActors.size()) +
                    " packets=" + std::to_string(packets.size()) +
                    " rebuild=" + std::to_string(rebuild),
                "DEBUG",
                "schematics"
            );
            return true;
        } catch (const std::exception& error) {
            state_->push(
                "schematic_debug_renderer_failed",
                safeMessage(error.what()),
                "ERROR",
                "schematics"
            );
            return false;
        } catch (...) {
            state_->push(
                "schematic_debug_renderer_failed",
                "Unknown debug renderer failure",
                "ERROR",
                "schematics"
            );
            return false;
        }
    }

    void clearSchematicDebugMarkers(bool resetCounters = true) noexcept {
        try {
            std::lock_guard sendLock(schematicSendMutex_);
            if (resetCounters) resetSchematicCounters();
            std::optional<bedrock::BedrockServerConnection> downstream;
            std::vector<ActiveSchematicDebugShape> debugShapes;
            std::vector<ActiveSchematicTextureActor> textureActors;
            uint64_t clearGeneration = 0;
            {
                std::lock_guard markerLock(schematicMarkerMutex_);
                clearGeneration = ++schematicMarkerGeneration_;
                if (activeSchematicDebugShapes_.empty() &&
                    activeSchematicTextureActors_.empty()) return;
                downstream = schematicDownstream_;
                debugShapes = activeSchematicDebugShapes_;
                textureActors = activeSchematicTextureActors_;
                if (!downstream.has_value()) {
                    activeSchematicDebugShapes_.clear();
                    activeSchematicTextureActors_.clear();
                    activeSchematicOpacity_ = 0;
                    return;
                }
            }
            auto clearPackets = makeSchematicDebugClearPackets(
                state_->version,
                debugShapes
            );
            bedrock::ProtoDefPacketEncoder encoder(state_->version);
            const auto codec = bedrock::VersionedMcpeCodec::forVersion(
                state_->version
            );
            appendSchematicTextureActorRemovals(
                clearPackets,
                encoder,
                codec.packetCodec(),
                textureActors
            );
            const bool queued = clearPackets.empty() ||
                queueSchematicPacketsBatched(*downstream, clearPackets);
            if (queued) {
                // Keep the active IDs until their exact removals have entered
                // the clientbound queue. If queueing fails (including an
                // allocation exception), the next clear can retry instead of
                // losing the only handles capable of removing stale ghosts.
                {
                    std::lock_guard markerLock(schematicMarkerMutex_);
                    if (schematicMarkerGeneration_ == clearGeneration) {
                        activeSchematicDebugShapes_.clear();
                        activeSchematicTextureActors_.clear();
                        activeSchematicOpacity_ = 0;
                    }
                }
                state_->schematicMarkerPackets.fetch_add(
                    clearPackets.size(),
                    std::memory_order_relaxed
                );
            } else {
                state_->push(
                    "schematic_debug_clear_deferred",
                    "Clientbound queue rejected the clear; retained active IDs for retry",
                    "WARN",
                    "schematics"
                );
            }
        } catch (const std::exception& error) {
            state_->push(
                "schematic_debug_clear_failed",
                safeMessage(error.what()),
                "WARN",
                "schematics"
            );
        } catch (...) {
            state_->push(
                "schematic_debug_clear_failed",
                "Unknown clear failure; retained active IDs for retry",
                "WARN",
                "schematics"
            );
        }
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
    std::mutex schematicSendMutex_;
    std::mutex schematicMarkerMutex_;
    std::optional<bedrock::BedrockServerConnection> schematicDownstream_;
    std::string schematicDownstreamSessionId_;
    std::vector<ActiveSchematicDebugShape> activeSchematicDebugShapes_;
    std::vector<ActiveSchematicTextureActor> activeSchematicTextureActors_;
    uint64_t nextSchematicDebugShapeId_ = SchematicDebugNetworkIdBase;
    uint64_t nextSchematicTextureActorId_ = SchematicTextureActorIdBase;
    int activeSchematicOpacity_ = 0;
    int32_t activeSchematicCorrectColor_ = 0;
    int32_t activeSchematicWrongColor_ = 0;
    int32_t activeSchematicMissingColor_ = 0;
    uint64_t schematicMarkerGeneration_ = 0;
    std::atomic<bool> stopped_ {false};
    std::mutex loginWatchdogMutex_;
    std::condition_variable loginWatchdogCv_;
    std::optional<PendingLogin> pendingLogin_;
    uint64_t loginWatchdogGeneration_ = 0;
    bool loginWatchdogStopping_ = false;
    std::thread loginWatchdogThread_;

    bool queueSchematicPacketsBatched(
        const bedrock::BedrockServerConnection& downstream,
        const std::vector<bedrock::VersionedGamePacket>& packets
    ) {
        if (packets.empty()) return true;
        // BedrockLiveRelay compresses every supplied vector into one outer
        // MCPE batch. Bound both packet count and uncompressed bytes for all
        // schematic add, delta, rollback and explicit-clear paths.
        constexpr std::size_t MaximumPacketsPerBatch = 64;
        constexpr std::size_t MaximumBytesPerBatch = 48 * 1024;
        std::lock_guard relayLock(relayMutex_);
        if (!relay_) return false;
        std::size_t packetIndex = 0;
        while (packetIndex < packets.size()) {
            std::vector<bedrock::VersionedGamePacket> batch;
            batch.reserve(MaximumPacketsPerBatch);
            std::size_t batchBytes = 0;
            while (packetIndex < packets.size() &&
                   batch.size() < MaximumPacketsPerBatch) {
                const auto packetBytes =
                    packets[packetIndex].payload.size() + 16;
                if (!batch.empty() &&
                    batchBytes + packetBytes > MaximumBytesPerBatch) {
                    break;
                }
                batchBytes += packetBytes;
                batch.push_back(packets[packetIndex++]);
            }
            if (!relay_->live().queueClientboundPackets(downstream, batch)) {
                return false;
            }
        }
        return true;
    }

    void resetSchematicCounters() noexcept {
        state_->schematicTotalBlocks.store(0, std::memory_order_relaxed);
        state_->schematicCorrectBlocks.store(0, std::memory_order_relaxed);
        state_->schematicMissingBlocks.store(0, std::memory_order_relaxed);
        state_->schematicWrongBlocks.store(0, std::memory_order_relaxed);
        state_->schematicUnknownBlocks.store(0, std::memory_order_relaxed);
        state_->schematicDisplayedMarkers.store(0, std::memory_order_relaxed);
    }

    void attachSchematicDownstream(
        const bedrock::BedrockServerConnection& connection,
        std::string sessionId
    ) noexcept {
        std::lock_guard lock(schematicMarkerMutex_);
        ++schematicMarkerGeneration_;
        schematicDownstream_ = connection;
        schematicDownstreamSessionId_ = std::move(sessionId);
        activeSchematicDebugShapes_.clear();
        activeSchematicTextureActors_.clear();
        nextSchematicDebugShapeId_ = SchematicDebugNetworkIdBase;
        nextSchematicTextureActorId_ = SchematicTextureActorIdBase;
        activeSchematicOpacity_ = 0;
        resetSchematicCounters();
    }

    void detachSchematicDownstream(std::string_view sessionId) noexcept {
        std::lock_guard lock(schematicMarkerMutex_);
        if (schematicDownstreamSessionId_ != sessionId) return;
        ++schematicMarkerGeneration_;
        schematicDownstream_.reset();
        schematicDownstreamSessionId_.clear();
        activeSchematicDebugShapes_.clear();
        activeSchematicTextureActors_.clear();
        nextSchematicDebugShapeId_ = SchematicDebugNetworkIdBase;
        nextSchematicTextureActorId_ = SchematicTextureActorIdBase;
        activeSchematicOpacity_ = 0;
        resetSchematicCounters();
    }

    std::optional<std::vector<bedrock::VersionedGamePacket>>
    takeSchematicLifecycleClear(const std::string& version) noexcept {
        try {
            std::lock_guard lock(schematicMarkerMutex_);
            // Always invalidate a render that may still be queued outside this
            // lock. A dimension/start-game transition with no committed actors
            // can still race an in-flight first publication.
            ++schematicMarkerGeneration_;
            if (activeSchematicDebugShapes_.empty() &&
                activeSchematicTextureActors_.empty()) return std::nullopt;
            const auto debugShapes = activeSchematicDebugShapes_;
            const auto textureActors = activeSchematicTextureActors_;
            activeSchematicDebugShapes_.clear();
            activeSchematicTextureActors_.clear();
            activeSchematicOpacity_ = 0;
            state_->schematicDisplayedMarkers.store(0, std::memory_order_relaxed);
            state_->schematicCorrectBlocks.store(0, std::memory_order_relaxed);
            state_->schematicMissingBlocks.store(0, std::memory_order_relaxed);
            state_->schematicWrongBlocks.store(0, std::memory_order_relaxed);
            state_->schematicUnknownBlocks.store(0, std::memory_order_relaxed);
            auto packets = makeSchematicDebugClearPackets(
                version,
                debugShapes
            );
            bedrock::ProtoDefPacketEncoder encoder(version);
            const auto codec = bedrock::VersionedMcpeCodec::forVersion(
                version
            );
            appendSchematicTextureActorRemovals(
                packets,
                encoder,
                codec.packetCodec(),
                textureActors
            );
            return packets;
        } catch (...) {
            return std::nullopt;
        }
    }

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
                    std::chrono::seconds(60),
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
        pendingLogin_->deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(60);
        pendingLogin_->generation = ++loginWatchdogGeneration_;
        loginWatchdogCv_.notify_all();
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
                "local_login_slow",
                "downstream_session=" + timedOut.sessionId +
                    " last_stage=" + timedOut.stage +
                    " timeoutMs=60000 raknet={" + rakNetDetail +
                    "}; preserving the active transport; a new Minecraft "
                    "attempt can replace it",
                "WARN",
                "watchdog"
            );
            state_->flushFlight("local_login_slow", 32);

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
    std::shared_ptr<RelayController> activeController;
    {
        std::lock_guard lock(controllerMutex);
        state = currentState;
        activeController = controller;
    }
    if (state) {
        state->configureGameplayFeatures(armor, totem, miniMap, schematic);
    }
    if (activeController && !schematic) {
        activeController->clearSchematicDebugMarkers();
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

extern "C" JNIEXPORT jintArray JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_schematicBlockSnapshot(
    JNIEnv* environment,
    jclass,
    jlong afterRevision,
    jintArray worldCoordinates
) {
    try {
        std::vector<int32_t> positions;
        if (worldCoordinates != nullptr) {
            const auto length = environment->GetArrayLength(worldCoordinates);
            if (length < 0 || length % 3 != 0) {
                throw std::invalid_argument(
                    "schematic coordinates must contain XYZ triples"
                );
            }
            positions.resize(static_cast<std::size_t>(length));
            if (length > 0) {
                static_assert(sizeof(jint) == sizeof(int32_t));
                environment->GetIntArrayRegion(
                    worldCoordinates,
                    0,
                    length,
                    reinterpret_cast<jint*>(positions.data())
                );
                if (environment->ExceptionCheck()) return nullptr;
            }
        }

        std::shared_ptr<RelayState> state;
        {
            std::lock_guard lock(controllerMutex);
            state = currentState;
        }
        auto values = state
            ? state->schematicBlockSnapshotValues(
                static_cast<uint64_t>(afterRevision),
                positions
            )
            : std::vector<int32_t> {0x43504553, 2, 0, 0, 0, 0};
        if (values.size() > static_cast<std::size_t>(
                std::numeric_limits<jsize>::max())) {
            throw std::length_error("schematic snapshot is too large");
        }
        auto result = environment->NewIntArray(
            static_cast<jsize>(values.size())
        );
        if (result == nullptr || values.empty()) return result;
        environment->SetIntArrayRegion(
            result,
            0,
            static_cast<jsize>(values.size()),
            reinterpret_cast<const jint*>(values.data())
        );
        return result;
    } catch (const std::exception& error) {
        const auto exception = environment->FindClass(
            "java/lang/RuntimeException"
        );
        if (exception != nullptr) {
            environment->ThrowNew(exception, error.what());
            environment->DeleteLocalRef(exception);
        }
        return nullptr;
    } catch (...) {
        const auto exception = environment->FindClass(
            "java/lang/RuntimeException"
        );
        if (exception != nullptr) {
            environment->ThrowNew(
                exception,
                "Unknown native schematic snapshot failure"
            );
            environment->DeleteLocalRef(exception);
        }
        return nullptr;
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_replaceSchematicDebugMarkers(
    JNIEnv* environment,
    jclass,
    jintArray markerRecords,
    jobjectArray expectedBlockStates,
    jboolean texturesEnabled,
    jint textureOpacityPercent,
    jboolean outlinesEnabled,
    jint outlineOpacityPercent,
    jint correctOutlineColor,
    jint wrongOutlineColor,
    jint missingOutlineColor,
    jint total,
    jint correct,
    jint missing,
    jint wrong,
    jint unknown,
    jlong expectedWorldRevision,
    jint expectedDimension
) {
    try {
        constexpr jsize MaximumRecordValues = 1'800 * 4;
        std::vector<int32_t> records;
        if (markerRecords != nullptr) {
            const auto length = environment->GetArrayLength(markerRecords);
            if (length < 0 || length % 4 != 0 ||
                length > MaximumRecordValues) {
                throw std::invalid_argument(
                    "schematic markers must contain at most 1800 XYZS tuples"
                );
            }
            records.resize(static_cast<std::size_t>(length));
            if (length > 0) {
                static_assert(sizeof(jint) == sizeof(int32_t));
                environment->GetIntArrayRegion(
                    markerRecords,
                    0,
                    length,
                    reinterpret_cast<jint*>(records.data())
                );
                if (environment->ExceptionCheck()) return JNI_FALSE;
            }
        }

        const auto markerCount = static_cast<jsize>(records.size() / 4);
        const auto stateCount = expectedBlockStates == nullptr
            ? 0
            : environment->GetArrayLength(expectedBlockStates);
        if (stateCount != markerCount) {
            throw std::invalid_argument(
                "schematic marker states must match XYZS tuple count"
            );
        }
        std::vector<SchematicDebugMarker> markers;
        markers.reserve(static_cast<std::size_t>(markerCount));
        for (jsize index = 0; index < markerCount; ++index) {
            const auto offset = static_cast<std::size_t>(index) * 4;
            auto blockState = static_cast<jstring>(
                environment->GetObjectArrayElement(expectedBlockStates, index)
            );
            if (environment->ExceptionCheck()) return JNI_FALSE;
            std::string expectedState;
            if (blockState != nullptr) {
                expectedState = fromJavaString(environment, blockState);
                environment->DeleteLocalRef(blockState);
            }
            if (expectedState.size() > 1'024) {
                throw std::invalid_argument(
                    "schematic marker block state is invalid"
                );
            }
            markers.push_back({
                records[offset],
                records[offset + 1],
                records[offset + 2],
                records[offset + 3],
                std::move(expectedState)
            });
        }

        std::shared_ptr<RelayController> activeController;
        {
            std::lock_guard lock(controllerMutex);
            activeController = controller;
        }
        if (activeController) {
            return activeController->replaceSchematicDebugMarkers(
                std::move(markers),
                texturesEnabled == JNI_TRUE,
                static_cast<int>(textureOpacityPercent),
                outlinesEnabled == JNI_TRUE,
                static_cast<int>(outlineOpacityPercent),
                static_cast<int32_t>(correctOutlineColor),
                static_cast<int32_t>(wrongOutlineColor),
                static_cast<int32_t>(missingOutlineColor),
                static_cast<uint64_t>(std::max<jint>(0, total)),
                static_cast<uint64_t>(std::max<jint>(0, correct)),
                static_cast<uint64_t>(std::max<jint>(0, missing)),
                static_cast<uint64_t>(std::max<jint>(0, wrong)),
                static_cast<uint64_t>(std::max<jint>(0, unknown)),
                static_cast<uint64_t>(expectedWorldRevision),
                static_cast<int32_t>(expectedDimension)
            ) ? JNI_TRUE : JNI_FALSE;
        }
        return JNI_FALSE;
    } catch (const std::exception& error) {
        const auto exception = environment->FindClass(
            "java/lang/RuntimeException"
        );
        if (exception != nullptr) {
            environment->ThrowNew(exception, error.what());
            environment->DeleteLocalRef(exception);
        }
        return JNI_FALSE;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_m9chko_bedrockrelay_NativeBridge_clearSchematicDebugMarkers(
    JNIEnv*,
    jclass
) {
    std::shared_ptr<RelayController> activeController;
    {
        std::lock_guard lock(controllerMutex);
        activeController = controller;
    }
    if (activeController) activeController->clearSchematicDebugMarkers();
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
