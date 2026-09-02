#pragma once

// Easy public API. Include this in bots:
//   #include <bedrock/bedrock.hpp>

#include <bedrock/Options.hpp>
#include <bedrock/JsValue.hpp>
#include <bedrock/auth/LiveTokenManager.hpp>
#include <bedrock/auth/MicrosoftAuthFlow.hpp>
#include <bedrock/auth/MinecraftBedrockServicesManager.hpp>
#include <bedrock/auth/MinecraftBedrockTokenManager.hpp>
#include <bedrock/auth/NativeBedrockAuthflow.hpp>
#include <bedrock/auth/PlayfabTokenManager.hpp>
#include <bedrock/api/Client.hpp>
#include <bedrock/RakNetPing.hpp>
#include <bedrock/chat/BedrockChat.hpp>
#include <bedrock/client/BedrockNetworkClient.hpp>
#include <bedrock/item/BedrockItem.hpp>
#include <bedrock/registry/BedrockDefaultSkin.hpp>
#include <bedrock/registry/BedrockFeatureRegistry.hpp>
#include <bedrock/registry/BedrockGameplayRegistry.hpp>
#include <bedrock/registry/BedrockLootRegistry.hpp>
#include <bedrock/registry/BedrockRegistry.hpp>
#include <bedrock/nbt/BedrockNbt.hpp>
#include <bedrock/nbt/BedrockNbtFile.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protocol/SnappyCodec.hpp>
#include <bedrock/protodef/ProtoDefNbt.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>
#include <bedrock/protodef/ProtoDefVariables.hpp>
#include <bedrock/realms/BedrockRealms.hpp>
#include <bedrock/relay/BedrockLiveRelay.hpp>
#include <bedrock/relay/BedrockRelay.hpp>
#include <bedrock/relay/ChunkPublisherRetention.hpp>
#include <bedrock/relay/LevelChunkRetentionCache.hpp>
#include <bedrock/server/BedrockServer.hpp>
#include <bedrock/server/ServerAdvertisement.hpp>
#include <bedrock/util/XxHash64.hpp>
#include <bedrock/world/BedrockBlockRegistry.hpp>
#include <bedrock/world/BedrockChunk.hpp>
#include <bedrock/world/BedrockLegacyChunk.hpp>
#include <bedrock/world/BedrockSubChunkPacket.hpp>
#include <bedrock/world/MinecraftDataAssets.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <netdb.h>
#if !defined(_WIN32)
#include <netinet/in.h>
#endif

namespace bedrock {

using Packet = api::Packet;
using TextPacket = api::TextPacket;
using DebugMode = api::DebugMode;
using ClientStatus = BedrockNetworkClientStatus;
using Server = BedrockServer;
using PacketValue = ProtoDefValue;
using PacketObject = std::unordered_map<std::string, PacketValue>;
using PacketArray = std::vector<PacketValue>;

// JavaScript distinguishes an omitted `version` property from a present
// property whose value is the empty string. Keep std::string's source-level
// API while retaining that presence bit across Options copies. In particular,
// both designated `.version = ""` and later `options.version = ""` are
// explicit and must reach Options.validateOptions instead of discovery.
class RequestedVersion : public std::string {
public:
    RequestedVersion() = default;
    RequestedVersion(const RequestedVersion&) = default;
    RequestedVersion(RequestedVersion&&) noexcept = default;
    RequestedVersion& operator=(const RequestedVersion&) = default;
    RequestedVersion& operator=(RequestedVersion&&) noexcept = default;

    RequestedVersion(const char* value)
        : std::string(value ? value : ""), provided_(true) {}

    RequestedVersion(std::string value)
        : std::string(std::move(value)), provided_(true) {}

    RequestedVersion(std::string_view value)
        : std::string(value), provided_(true) {}

    RequestedVersion& operator=(const char* value) {
        std::string::operator=(value ? value : "");
        provided_ = true;
        return *this;
    }

    RequestedVersion& operator=(std::string value) {
        std::string::operator=(std::move(value));
        provided_ = true;
        return *this;
    }

    RequestedVersion& operator=(std::string_view value) {
        std::string::operator=(value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& operator=(char value) {
        std::string::operator=(value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& operator=(std::initializer_list<char> value) {
        std::string::operator=(value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& assign(const std::string& value) {
        std::string::assign(value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& assign(
        const std::string& value,
        size_type index,
        size_type count = npos
    ) {
        std::string::assign(value, index, count);
        provided_ = true;
        return *this;
    }

    RequestedVersion& assign(std::string&& value) {
        std::string::operator=(std::move(value));
        provided_ = true;
        return *this;
    }

    RequestedVersion& assign(const char* value) {
        std::string::assign(value ? value : "");
        provided_ = true;
        return *this;
    }

    RequestedVersion& assign(const char* value, size_type count) {
        std::string::assign(value ? value : "", count);
        provided_ = true;
        return *this;
    }

    RequestedVersion& assign(size_type count, char value) {
        std::string::assign(count, value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& assign(std::initializer_list<char> value) {
        std::string::assign(value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& assign(std::string_view value) {
        std::string::assign(value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& assign(
        std::string_view value,
        size_type index,
        size_type count = npos
    ) {
        std::string::assign(value.substr(index, count));
        provided_ = true;
        return *this;
    }

    template <
        typename InputIt,
        std::enable_if_t<!std::is_integral_v<InputIt>, int> = 0
    >
    RequestedVersion& assign(InputIt first, InputIt last) {
        std::string::assign(first, last);
        provided_ = true;
        return *this;
    }

    RequestedVersion& append(const std::string& value) {
        std::string::append(value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& append(
        const std::string& value,
        size_type index,
        size_type count = npos
    ) {
        std::string::append(value, index, count);
        provided_ = true;
        return *this;
    }

    RequestedVersion& append(const char* value) {
        std::string::append(value ? value : "");
        provided_ = true;
        return *this;
    }

    RequestedVersion& append(const char* value, size_type count) {
        std::string::append(value ? value : "", count);
        provided_ = true;
        return *this;
    }

    RequestedVersion& append(size_type count, char value) {
        std::string::append(count, value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& append(std::initializer_list<char> value) {
        std::string::append(value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& append(std::string_view value) {
        std::string::append(value.data(), value.size());
        provided_ = true;
        return *this;
    }

    RequestedVersion& append(
        std::string_view value,
        size_type index,
        size_type count = npos
    ) {
        const auto part = value.substr(index, count);
        std::string::append(part.data(), part.size());
        provided_ = true;
        return *this;
    }

    template <
        typename InputIt,
        std::enable_if_t<!std::is_integral_v<InputIt>, int> = 0
    >
    RequestedVersion& append(InputIt first, InputIt last) {
        std::string::append(first, last);
        provided_ = true;
        return *this;
    }

    RequestedVersion& operator+=(const std::string& value) {
        return append(value);
    }

    RequestedVersion& operator+=(const char* value) {
        return append(value);
    }

    RequestedVersion& operator+=(char value) {
        push_back(value);
        return *this;
    }

    RequestedVersion& operator+=(std::initializer_list<char> value) {
        return append(value);
    }

    RequestedVersion& operator+=(std::string_view value) {
        return append(value);
    }

    RequestedVersion& insert(size_type index, const std::string& value) {
        std::string::insert(index, value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& insert(
        size_type index,
        const std::string& value,
        size_type valueIndex,
        size_type count = npos
    ) {
        std::string::insert(index, value, valueIndex, count);
        provided_ = true;
        return *this;
    }

    RequestedVersion& insert(size_type index, const char* value) {
        std::string::insert(index, value ? value : "");
        provided_ = true;
        return *this;
    }

    RequestedVersion& insert(
        size_type index,
        const char* value,
        size_type count
    ) {
        std::string::insert(index, value ? value : "", count);
        provided_ = true;
        return *this;
    }

    RequestedVersion& insert(size_type index, size_type count, char value) {
        std::string::insert(index, count, value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& insert(size_type index, std::string_view value) {
        std::string::insert(index, value.data(), value.size());
        provided_ = true;
        return *this;
    }

    RequestedVersion& insert(
        size_type index,
        std::string_view value,
        size_type valueIndex,
        size_type count = npos
    ) {
        const auto part = value.substr(valueIndex, count);
        std::string::insert(index, part.data(), part.size());
        provided_ = true;
        return *this;
    }

    iterator insert(const_iterator position, char value) {
        auto result = std::string::insert(position, value);
        provided_ = true;
        return result;
    }

    iterator insert(const_iterator position, size_type count, char value) {
        auto result = std::string::insert(position, count, value);
        provided_ = true;
        return result;
    }

    template <
        typename InputIt,
        std::enable_if_t<!std::is_integral_v<InputIt>, int> = 0
    >
    iterator insert(const_iterator position, InputIt first, InputIt last) {
        auto result = std::string::insert(position, first, last);
        provided_ = true;
        return result;
    }

    iterator insert(
        const_iterator position,
        std::initializer_list<char> value
    ) {
        auto result = std::string::insert(position, value);
        provided_ = true;
        return result;
    }

    RequestedVersion& replace(
        size_type index,
        size_type count,
        const std::string& value
    ) {
        std::string::replace(index, count, value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& replace(
        size_type index,
        size_type count,
        const std::string& value,
        size_type valueIndex,
        size_type valueCount = npos
    ) {
        std::string::replace(index, count, value, valueIndex, valueCount);
        provided_ = true;
        return *this;
    }

    RequestedVersion& replace(
        size_type index,
        size_type count,
        const char* value
    ) {
        std::string::replace(index, count, value ? value : "");
        provided_ = true;
        return *this;
    }

    RequestedVersion& replace(
        size_type index,
        size_type count,
        const char* value,
        size_type valueCount
    ) {
        std::string::replace(index, count, value ? value : "", valueCount);
        provided_ = true;
        return *this;
    }

    RequestedVersion& replace(
        size_type index,
        size_type count,
        size_type valueCount,
        char value
    ) {
        std::string::replace(index, count, valueCount, value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& replace(
        size_type index,
        size_type count,
        std::string_view value
    ) {
        std::string::replace(index, count, value.data(), value.size());
        provided_ = true;
        return *this;
    }

    RequestedVersion& replace(
        const_iterator first,
        const_iterator last,
        const std::string& value
    ) {
        std::string::replace(first, last, value);
        provided_ = true;
        return *this;
    }

    RequestedVersion& replace(
        const_iterator first,
        const_iterator last,
        const char* value
    ) {
        std::string::replace(first, last, value ? value : "");
        provided_ = true;
        return *this;
    }

    RequestedVersion& replace(
        const_iterator first,
        const_iterator last,
        const char* value,
        size_type count
    ) {
        std::string::replace(first, last, value ? value : "", count);
        provided_ = true;
        return *this;
    }

    RequestedVersion& replace(
        const_iterator first,
        const_iterator last,
        size_type count,
        char value
    ) {
        std::string::replace(first, last, count, value);
        provided_ = true;
        return *this;
    }

    template <
        typename InputIt,
        std::enable_if_t<!std::is_integral_v<InputIt>, int> = 0
    >
    RequestedVersion& replace(
        const_iterator first,
        const_iterator last,
        InputIt valueFirst,
        InputIt valueLast
    ) {
        std::string::replace(first, last, valueFirst, valueLast);
        provided_ = true;
        return *this;
    }

    RequestedVersion& replace(
        const_iterator first,
        const_iterator last,
        std::initializer_list<char> value
    ) {
        std::string::replace(first, last, value);
        provided_ = true;
        return *this;
    }

    void clear() noexcept {
        std::string::clear();
        provided_ = true;
    }

    void resize(size_type count) {
        std::string::resize(count);
        provided_ = true;
    }

    void resize(size_type count, char value) {
        std::string::resize(count, value);
        provided_ = true;
    }

    void push_back(char value) {
        std::string::push_back(value);
        provided_ = true;
    }

    void pop_back() {
        std::string::pop_back();
        provided_ = true;
    }

    RequestedVersion& erase(size_type index = 0, size_type count = npos) {
        std::string::erase(index, count);
        provided_ = true;
        return *this;
    }

    iterator erase(const_iterator position) {
        auto result = std::string::erase(position);
        provided_ = true;
        return result;
    }

    iterator erase(const_iterator first, const_iterator last) {
        auto result = std::string::erase(first, last);
        provided_ = true;
        return result;
    }

    void swap(RequestedVersion& other) noexcept {
        std::string::swap(other);
        // Calling a member mutator explicitly touches both public Options
        // properties; presence describes that caller action, not provenance
        // of the bytes that moved between wrapper instances.
        provided_ = true;
        other.provided_ = true;
    }

    void swap(std::string& other) noexcept {
        std::string::swap(other);
        provided_ = true;
    }

    friend void swap(RequestedVersion& left, RequestedVersion& right) noexcept {
        left.swap(right);
    }

    bool provided() const noexcept {
        return provided_;
    }

    void setResolved(std::string value) {
        std::string::operator=(std::move(value));
    }

private:
    bool provided_ = false;
};

// JavaScript's Client defaultOptions deliberately has no `port` property,
// while createClient() supplies `{ port: 19132, ...options }` before it
// constructs Client.  A plain uint16_t default cannot distinguish an omitted
// property from an explicit `port: 0`, so retain that presence without making
// native callers give up scalar-style assignment and conversion.
class RequestedPort {
public:
    RequestedPort() = default;
    RequestedPort(const RequestedPort&) = default;
    RequestedPort(RequestedPort&&) noexcept = default;
    RequestedPort& operator=(const RequestedPort&) = default;
    RequestedPort& operator=(RequestedPort&&) noexcept = default;

    RequestedPort(uint16_t value) noexcept
        : value_(value), presence_(JsPropertyPresence::Value), provided_(true) {}

    template <
        typename Integer,
        std::enable_if_t<
            std::is_integral_v<std::remove_reference_t<Integer>> &&
            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Integer>>, bool> &&
            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Integer>>, uint16_t>,
            int
        > = 0
    >
    RequestedPort(Integer value) noexcept
        : RequestedPort(static_cast<uint16_t>(value)) {}

    RequestedPort(std::nullptr_t) noexcept
        : presence_(JsPropertyPresence::Null), provided_(true) {}

    RequestedPort(JsUndefined) noexcept
        : presence_(JsPropertyPresence::Undefined), provided_(true) {}

    RequestedPort& operator=(uint16_t value) noexcept {
        value_ = value;
        presence_ = JsPropertyPresence::Value;
        provided_ = true;
        return *this;
    }

    template <
        typename Integer,
        std::enable_if_t<
            std::is_integral_v<std::remove_reference_t<Integer>> &&
            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Integer>>, bool> &&
            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<Integer>>, uint16_t>,
            int
        > = 0
    >
    RequestedPort& operator=(Integer value) noexcept {
        return *this = static_cast<uint16_t>(value);
    }

    RequestedPort& operator=(std::nullptr_t) noexcept {
        presence_ = JsPropertyPresence::Null;
        provided_ = true;
        return *this;
    }

    RequestedPort& operator=(JsUndefined) noexcept {
        presence_ = JsPropertyPresence::Undefined;
        provided_ = true;
        return *this;
    }

    operator uint16_t() const noexcept {
        return value_;
    }

    bool has_value() const noexcept {
        return presence_ == JsPropertyPresence::Value;
    }

    bool hasOwn() const noexcept {
        return presence_ != JsPropertyPresence::Missing;
    }

    bool provided() const noexcept {
        return provided_;
    }

    bool isUndefined() const noexcept {
        return presence_ == JsPropertyPresence::Undefined;
    }

    bool isNull() const noexcept {
        return presence_ == JsPropertyPresence::Null;
    }

    JsPropertyPresence presence() const noexcept {
        return presence_;
    }

    uint16_t value() const {
        if (!has_value()) {
            throw std::bad_optional_access();
        }
        return value_;
    }

    uint16_t value_or(uint16_t fallback) const noexcept {
        return has_value() ? value_ : fallback;
    }

    void setResolved(uint16_t value) noexcept {
        value_ = value;
        presence_ = JsPropertyPresence::Value;
    }

    void setUndefined() noexcept {
        presence_ = JsPropertyPresence::Undefined;
        provided_ = true;
    }

    void reset() noexcept {
        presence_ = JsPropertyPresence::Missing;
        provided_ = false;
    }

private:
    uint16_t value_ = 0;
    JsPropertyPresence presence_ = JsPropertyPresence::Missing;
    bool provided_ = false;
};

struct Options {
    std::string host;
    RequestedPort port;
    std::string username;
    // Omitted means createClient() discovers the server version. A present
    // empty string remains explicit and is rejected like the JavaScript API.
    RequestedVersion version;
    uint32_t protocolVersion = 0;
    JsProperty<bool> followPort;
    // client.js skips init only for a truthy delayedInit property. createClient
    // always overwrites it with true as the final object-literal field.
    JsProperty<bool> delayedInit;
    bool skipPing = false;
    BedrockRealmSelection realms;
    std::function<void(const std::string&)> conLog = [](const std::string& message) {
        std::cout << message << "\n";
    };
    // C++ cannot reproduce Node's process-level unhandled `error` event on a
    // background std::thread. This preinstalled sink exposes that boundary
    // without throwing across the worker. Ordinary onError listeners win.
    std::function<void(const std::string&)> onUnhandledAsyncError;
    bool offline = false;
    bool interactiveAuth = true;
    JsProperty<std::string> authTitle;
    std::string deviceType;
    std::string flow;
    bool forceRefresh = false;
    MsalConfigPtr msalConfig;
    std::shared_ptr<Authflow> authflow;
    std::string password;
    // Deprecated compatibility alias. New code should use authTitle.
    std::string xboxClientId;
    std::filesystem::path authCacheRoot;
    std::function<void(const XboxDeviceCodeInfo&)> onMsaCode;

    int mtu = 1400;
    int connectTimeoutMs = 9000;
    int batchingIntervalMs = 20;
    int compressionLevel = 7;
    bool autoInitPlayer = true;
    bool autoResourcePackResponses = true;
    bool clientCacheEnabled = false;
    bool trackWorld = false;
    int32_t chunkRadius = 10;
    std::vector<uint8_t> loginPacket;
    std::string clientDataJson;

    // Canonical bedrock-protocol option names. The older C++ extension fields
    // above remain observable for source compatibility but cannot affect the
    // root Client: JavaScript ignores those unknown property names. Upstream
    // stores viewDistance in options but
    // createClient.js reads client.viewDistance instead, so this option does
    // not replace chunkRadius (the same upstream behavior is preserved).
    JsProperty<int> connectTimeout;
    JsProperty<int> batchingInterval;
    JsProperty<int32_t> viewDistance;
    ProfilesFolderOption profilesFolder;
    std::string raknetBackend = "raknet-native";
    bool useRaknetWorkers = true;
    std::optional<bool> useNativeRaknet;
    std::string compressionAlgorithm = "deflate";
    uint16_t compressionThreshold = 512;

    DebugMode debug = DebugMode::Off;
    bool decodePackets = true;
    bool packetDump = false;
    bool quiet = true;
};

// Diagnostics are useful to C++ applications, but they are not part of the
// JavaScript ClientOptions contract. Keep them under one explicit member so
// normal connection settings remain easy to scan and autocomplete.
struct ClientDiagnostics {
    DebugMode debug = DebugMode::Off;
    bool decodePackets = true;
    bool packetDump = false;
    bool quiet = true;
};

// The normal createClient({...}) surface mirrors bedrock-protocol's
// ClientOptions instead of exposing every native implementation seam. The
// larger Options type above this facade remains available as
// LegacyClientOptions for code that needs those extensions.
struct ClientOptions {
    std::string host = "localhost";
    uint16_t port = 19132;
    std::string username = "Bot";
    // nullopt performs the same ping-based version discovery as an omitted JS
    // property.  A supplied value must be an exact supported Bedrock version.
    std::optional<std::string> version;
    bool offline = false;
    // When false, play_status.player_spawn leaves status at Initializing and
    // does not emit spawn; callers can send the init packet and setStatus()
    // manually, matching client.js.
    bool autoInitPlayer = true;

    std::optional<int32_t> viewDistance;
    std::optional<std::string> authTitle;
    int connectTimeout = 9000;
    bool skipPing = false;
    std::optional<bool> followPort;
    std::function<void(const std::string&)> conLog = [](const std::string& message) {
        std::cout << message << "\n";
    };
    BedrockRealmSelection realms;
    ProfilesFolderOption profilesFolder;
    std::function<void(const XboxDeviceCodeInfo&)> onMsaCode;

    std::string raknetBackend = "raknet-native";
    // Keep the spelling exported by bedrock-protocol's ClientOptions type.
    bool useRaknetWorker = true;
    int compressionLevel = 7;
    int batchingInterval = 20;
    ClientDiagnostics diagnostics;
};

using BotOptions = ClientOptions;
using LegacyClientOptions = Options;

struct ClientFactoryTestAccess;

class Client {
public:
    using PacketHandler = std::function<void(const Packet&)>;
    using TextHandler = std::function<void(const TextPacket&)>;
    using SessionHandler = std::function<void(const BedrockClientProfile&)>;

private:
    friend struct ClientFactoryTestAccess;

    struct State {
        explicit State(Options value)
            : requestedVersion(
                  value.version.provided() || !value.version.empty()
                  ? std::optional<std::string>(value.version)
                  : std::nullopt),
              options(std::move(value)) {
            // Client's constructor spreads Options.defaultOptions before its
            // delayed init. Keep the caller's presence separately while the
            // public options object immediately exposes CURRENT_VERSION.
            if (!options.version.provided()) {
                options.version.setResolved(std::string(CURRENT_VERSION));
            }
            if (!options.connectTimeout.provided()) {
                options.connectTimeout.setResolved(9000);
            }
        }

        ~State() {
            // The owning Client normally joins both workers. This self-thread
            // fallback is needed when a user destroys the Client from one of
            // its own callbacks.
            if (preflightThread.joinable()) preflightThread.detach();
            if (connectThread.joinable()) connectThread.detach();
        }

        mutable std::mutex mutex;
        std::condition_variable preflightCv;
        std::mutex workersMutex;
        std::atomic<bool> ownerAlive {true};
        std::atomic<bool> callbacksSuppressed {false};
        std::optional<std::string> requestedVersion;
        Options options;
        std::unique_ptr<ProtoDefPacketDecoder> decoder;
        std::shared_ptr<BedrockNetworkClient> network;
        bool initialized = false;
        bool connectAllowedEmitted = false;
        bool externalConnectAllowedSeen = false;
        bool externalErrorSeen = false;
        bool factoryMode = false;
        bool autoConnectStarted = false;
        bool connectWorkerStarted = false;
        bool autoConnectFinished = false;
        bool connectWorkerExited = false;
        enum class ConnectWorkerFinalization {
            Undecided,
            NoClose,
            WillClose,
            Exited
        };
        ConnectWorkerFinalization connectWorkerFinalization =
            ConnectWorkerFinalization::Undecided;
        bool autoConnectCancelled = false;
        bool deferredClose = false;
        std::string deferredCloseReason = "closed";
        std::uint64_t closeGeneration = 0;
        std::size_t autoConnectInvocations = 0;
        std::optional<std::string> unhandledAsyncError;
        struct PendingUnhandledAsyncError {
            std::string message;
            std::function<void(const std::string&)> sink;
        };
        std::mutex unhandledDeliveryMutex;
        std::deque<PendingUnhandledAsyncError> pendingUnhandledAsyncErrors;
        bool unhandledDeliveryActive = false;
        std::function<void()> afterNetworkConnectTestHook;
        std::function<void()> afterWorkerDecisionTestHook;

        std::thread preflightThread;
        std::thread connectThread;

        std::vector<std::pair<std::string, PacketHandler>> packetHandlers;
        std::vector<PacketHandler> anyHandlers;
        std::vector<TextHandler> textHandlers;
        std::vector<SessionHandler> sessionHandlers;
        std::vector<std::function<void()>> loggingInHandlers;
        std::vector<std::function<void()>> joinHandlers;
        std::vector<std::function<void()>> spawnHandlers;
        std::vector<std::function<void(int64_t)>> heartbeatHandlers;
        std::vector<std::function<void(ClientStatus)>> statusHandlers;
        std::vector<std::function<void(const std::string&)>> closeHandlers;
        std::vector<std::function<void(const std::string&)>> errorHandlers;
        std::vector<std::function<void()>> connectAllowedHandlers;

        std::vector<std::pair<std::string, ProtoDefValue>> queuedValues;
        std::unordered_set<std::string> subscribedPackets;
        bool decodeAnyPacket = false;
    };

    struct FactoryTag {};
    std::shared_ptr<State> state_;

public:
    explicit Client(Options options)
        : state_(std::make_shared<State>(std::move(options))) {
        if (!state_->options.delayedInit.truthy()) initialize(state_);
    }

    ~Client() {
        shutdown();
    }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&& other) noexcept
        : state_(std::move(other.state_)) {}

    Client& operator=(Client&& other) noexcept {
        if (this == &other) return *this;
        shutdown();
        state_ = std::move(other.state_);
        return *this;
    }

    static Client createFactory(Options options) {
        return Client(std::move(options), FactoryTag{});
    }

    void init() {
        initialize(state_);
    }

    // A lock-aware mutation seam for the public options object. JavaScript
    // exposes client.options directly; native callers use this callback before
    // init/session boundaries without racing the factory's worker threads.
    template <typename Mutator>
    void updateOptions(Mutator&& mutator) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        std::forward<Mutator>(mutator)(state_->options);
    }

    bool connect() {
        auto state = state_;
        BedrockNetworkClient* network = nullptr;
        bool autoConnectStarted = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->network) {
                throw std::runtime_error("Connect not currently allowed");
            }
            autoConnectStarted = state->autoConnectStarted;
            network = state->network.get();
        }

        // createClient's internal listener has already marked auto-connect by
        // the time auth.js emits its synchronous constructor error. A user
        // error listener may nevertheless call client.connect() recursively;
        // JavaScript runs authentication and startQueue() again in that same
        // stack. Only calls outside the owning preparation stack retain the
        // native factory's already-started fast path.
        if (autoConnectStarted &&
            !network->connectPreparationOwnedByCurrentThread()) {
            return true;
        }
        const bool prepared = network->prepareConnectLifecycle(false);
        if (!prepared) return false;
        return network->connect();
    }

    int run() {
        return requireNetwork()->run();
    }

    std::string ping() {
        std::shared_ptr<BedrockNetworkClient> network;
        Options current;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            network = state_->network;
            current = state_->options;
        }

        const auto fail = [&](const std::string& message) -> std::string {
            if (current.conLog) {
                current.conLog(
                    "Unable to connect to [" + current.host + "]/" +
                    (current.port.has_value()
                        ? std::to_string(current.port.value())
                        : current.port.isNull() ? "null" : "undefined") +
                    ". Is the server running?"
                );
            }
            throw std::runtime_error(message);
        };

        if (!network) {
            return fail(
                "Cannot read properties of undefined (reading 'ping')"
            );
        }

        const auto target = network->options();
        // RakNativeClient.ping(timeout = 1000) applies its parameter default
        // only to explicit undefined. Null/zero remain zero; the ordinary
        // defaultOptions path has already materialized 9000 in client.options.
        int timeout = 1000;
        if (current.connectTimeout.isNull()) {
            timeout = 0;
        } else if (current.connectTimeout.has_value()) {
            timeout = *current.connectTimeout;
        }

        const auto pong = RakNetPinger::ping(
            target.host,
            target.port,
            timeout
        );
        if (!pong.ok) {
            if (pong.timedOut) {
                if (current.conLog) {
                    current.conLog(
                        "Unable to connect to [" + current.host + "]/" +
                        (current.port.has_value()
                            ? std::to_string(current.port.value())
                            : current.port.isNull() ? "null" : "undefined") +
                        ". Is the server running?"
                    );
                }
                throw RakTimeout("Ping timed out");
            }
            return fail(
                pong.error.empty() ? "Bedrock ping failed" : pong.error
            );
        }
        return pong.rawMotd;
    }

    void close(const std::string& reason = "closed") {
        auto state = state_;
        std::shared_ptr<BedrockNetworkClient> network;
        bool factoryMode = false;
        bool pendingConnectWorker = false;
        bool previousCancellation = false;
        std::uint64_t observedCloseGeneration = 0;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            network = state->network;
            factoryMode = state->factoryMode;
            previousCancellation = state->autoConnectCancelled;
            observedCloseGeneration = state->closeGeneration;
            if (factoryMode) {
                // Block a not-yet-launched worker while the synchronous close
                // emission is in progress. A throwing listener rolls this bit
                // back below, preserving the already-recorded connect intent.
                state->autoConnectCancelled = true;
                pendingConnectWorker = state->connectWorkerStarted &&
                    !state->autoConnectFinished;
            }
        }

        try {
            if (network && pendingConnectWorker) {
                // Emit close, stop the queue and publish terminal status now,
                // in caller order, but leave the transport object to the
                // tracked connect worker. This preserves JavaScript listener
                // timing without racing raknet_ destruction against connect().
                network->beginDeferredClose(reason);
            } else if (network) {
                network->close(reason);
            }
        } catch (...) {
            bool rollbackThisClose = false;
            if (factoryMode) {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    // A public recursive/concurrent close may have committed
                    // while this outer EventEmitter snapshot was running. Its
                    // one-way cancellation/deferred cleanup must win even if a
                    // later outer listener throws.
                    if (state->closeGeneration == observedCloseGeneration &&
                        (!network || network->status() !=
                            ClientStatus::Disconnected)) {
                        state->autoConnectCancelled = previousCancellation;
                        rollbackThisClose = true;
                    }
                }
                if (rollbackThisClose && !previousCancellation) {
                    launchAutoConnectWorker(state);
                }
            }
            throw;
        }

        bool deferTransportClose = false;
        bool finishTransportCloseHere = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (factoryMode && pendingConnectWorker) {
                if (state->connectWorkerStarted &&
                    !state->autoConnectFinished) {
                    state->deferredClose = true;
                    state->deferredCloseReason = reason;
                    state->callbacksSuppressed.store(true);
                    deferTransportClose = true;
                } else {
                    // The worker finished between the emission and this lock
                    // and therefore cannot consume a new deferred flag.
                    finishTransportCloseHere = true;
                }
            }
            // client.js close() removes every listener. Active emissions have
            // already taken their own snapshot in the facade/network layer,
            // so clear only after a successful close emission. If a listener
            // threw, the catch above left every store intact for a retry.
            state->packetHandlers.clear();
            state->anyHandlers.clear();
            state->textHandlers.clear();
            state->sessionHandlers.clear();
            state->loggingInHandlers.clear();
            state->joinHandlers.clear();
            state->spawnHandlers.clear();
            state->heartbeatHandlers.clear();
            state->statusHandlers.clear();
            state->closeHandlers.clear();
            state->errorHandlers.clear();
            state->connectAllowedHandlers.clear();
            ++state->closeGeneration;
        }
        if (network && deferTransportClose) {
            network->requestRakNetStop();
        } else if (network && finishTransportCloseHere) {
            network->close(reason);
        }
    }

    void disconnect(const std::string& reason = "Client leaving", bool hide = false) {
        auto state = state_;
        std::shared_ptr<BedrockNetworkClient> network;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->network) {
                // client.js checks the public Disconnected status before it
                // writes anything. A delayed createClient has no native
                // network yet and therefore takes that silent return without
                // cancelling discovery or clearing pending listeners.
                return;
            }
            network = state->network;
        }
        // client.js writes the disconnect packet and then calls this.close(),
        // so factory cancellation, listener removal, nested-close generation
        // and deferred transport ownership must all pass through the facade.
        if (network->sendDisconnectPacket(reason, hide)) {
            close(reason);
        }
    }

    ClientStatus status() const {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->network
            ? state->network->status()
            : ClientStatus::Disconnected;
    }

    void setStatus(ClientStatus status) {
        requireNetwork()->setStatus(status);
    }

    std::optional<uint64_t> entityId() const {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->network
            ? state->network->entityId()
            : std::nullopt;
    }

    // client.js retains the latest decoded start_game params. The C++ facade
    // returns its normal Packet view, force-decoded even when no start_game
    // listener was registered.
    std::optional<Packet> startGameData() const {
        auto state = state_;
        std::shared_ptr<BedrockNetworkClient> network;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            network = state->network;
        }
        if (!network) return std::nullopt;
        const auto packet = network->startGameData();
        return packet
            ? std::optional<Packet>(toApiPacket(state, *packet, true))
            : std::nullopt;
    }

    void updateItemPalette(const ProtoDefValue& palette) {
        requireNetwork()->updateItemPalette(palette);
    }

    std::optional<BedrockClientProfile> profile() const {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->network
            ? state->network->profile()
            : std::nullopt;
    }

    std::optional<std::string> username() const {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->network
            ? state->network->username()
            : std::nullopt;
    }

    std::vector<std::string> accessToken() const {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->network
            ? state->network->accessToken()
            : std::vector<std::string>{};
    }

    uint32_t protocolVersion() const {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->network
            ? state->network->protocolVersion()
            : state->options.protocolVersion;
    }

    bool versionLessThan(const std::string& version) const {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->network) return delayedStringComparison(version);
        return state->network->versionLessThan(version);
    }

    bool versionLessThan(uint32_t protocolVersion) const noexcept {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->network && state->network->versionLessThan(protocolVersion);
    }

    bool versionGreaterThan(const std::string& version) const {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->network) return delayedStringComparison(version);
        return state->network->versionGreaterThan(version);
    }

    bool versionGreaterThan(uint32_t protocolVersion) const noexcept {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->network && state->network->versionGreaterThan(protocolVersion);
    }

    bool versionGreaterThanOrEqualTo(const std::string& version) const {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->network) return delayedStringComparison(version);
        return state->network->versionGreaterThanOrEqualTo(version);
    }

    bool versionGreaterThanOrEqualTo(uint32_t protocolVersion) const noexcept {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->network && state->network->versionGreaterThanOrEqualTo(protocolVersion);
    }

    bool versionLessThanOrEqualTo(const std::string& version) const {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->network) return delayedStringComparison(version);
        return state->network->versionLessThanOrEqualTo(version);
    }

    bool versionLessThanOrEqualTo(uint32_t protocolVersion) const noexcept {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->network && state->network->versionLessThanOrEqualTo(protocolVersion);
    }

    Options& options() noexcept {
        return state_->options;
    }

    const Options& options() const noexcept {
        return state_->options;
    }

    Options optionsSnapshot() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->options;
    }

    bool initialized() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->initialized;
    }

    bool autoConnectStarted() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->autoConnectStarted;
    }

    bool connectWorkerStarted() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->connectWorkerStarted;
    }

    std::optional<std::string> takeUnhandledAsyncError() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto error = std::move(state_->unhandledAsyncError);
        state_->unhandledAsyncError.reset();
        return error;
    }

    void onConnectAllowed(std::function<void()> handler) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            // EventEmitter does not replay an event to listeners registered
            // after emit() returned.
            state_->connectAllowedHandlers.push_back(std::move(handler));
            state_->externalConnectAllowedSeen = true;
        }
        state_->preflightCv.notify_all();
    }

    void on(const std::string& packetName, PacketHandler handler) {
        if (packetName == "packet") {
            onAny(std::move(handler));
            return;
        }
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        state->subscribedPackets.insert(packetName == "kick" ? "disconnect" : packetName);
        if (state->network) {
            attachPacketHandlerLocked(state, packetName, std::move(handler));
        } else {
            state->packetHandlers.push_back({packetName, std::move(handler)});
        }
        armPreflightLocked(state);
    }

    void onAny(PacketHandler handler) {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        state->decodeAnyPacket = true;
        if (state->network) {
            attachAnyHandlerLocked(state, std::move(handler));
        } else {
            state->anyHandlers.push_back(std::move(handler));
        }
        armPreflightLocked(state);
    }

    void onText(TextHandler handler) {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        state->subscribedPackets.insert("text");
        if (state->network) {
            attachTextHandlerLocked(state, std::move(handler));
        } else {
            state->textHandlers.push_back(std::move(handler));
        }
        armPreflightLocked(state);
    }

    void onSession(SessionHandler handler) {
        addNetworkHandler(
            state_->sessionHandlers,
            std::move(handler),
            [](BedrockNetworkClient& network, SessionHandler value) {
                network.onSession(std::move(value));
            }
        );
    }

    void onLoggingIn(std::function<void()> handler) {
        addNetworkHandler(
            state_->loggingInHandlers,
            std::move(handler),
            [](BedrockNetworkClient& network, std::function<void()> value) {
                network.onLoggingIn(std::move(value));
            }
        );
    }

    void onJoin(std::function<void()> handler) {
        addNetworkHandler(
            state_->joinHandlers,
            std::move(handler),
            [](BedrockNetworkClient& network, std::function<void()> value) {
                network.onJoin(std::move(value));
            }
        );
    }

    void onSpawn(std::function<void()> handler) {
        addNetworkHandler(
            state_->spawnHandlers,
            std::move(handler),
            [](BedrockNetworkClient& network, std::function<void()> value) {
                network.onSpawn(std::move(value));
            }
        );
    }

    void onHeartbeat(std::function<void(int64_t)> handler) {
        addNetworkHandler(
            state_->heartbeatHandlers,
            std::move(handler),
            [](BedrockNetworkClient& network, std::function<void(int64_t)> value) {
                network.onHeartbeat(std::move(value));
            }
        );
    }

    void onStatus(std::function<void(ClientStatus)> handler) {
        addNetworkHandler(
            state_->statusHandlers,
            std::move(handler),
            [](BedrockNetworkClient& network, std::function<void(ClientStatus)> value) {
                network.onStatus(std::move(value));
            }
        );
    }

    void onClose(std::function<void(const std::string&)> handler) {
        addNetworkHandler(
            state_->closeHandlers,
            std::move(handler),
            [](BedrockNetworkClient& network, std::function<void(const std::string&)> value) {
                network.onClose(std::move(value));
            }
        );
    }

    // Node's Client `close` event carries no reason argument. The string
    // overload above is retained as a C++ transport-diagnostic extension.
    void onClose(std::function<void()> handler) {
        onClose([handler = std::move(handler)](const std::string&) {
            handler();
        });
    }

    void onError(std::function<void(const std::string&)> handler) {
        auto state = state_;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            // Keep this list even after init: preflight errors do not have a
            // BedrockNetworkClient yet, while later errors are installed below.
            state->errorHandlers.push_back(handler);
            state->externalErrorSeen = true;
            if (state->network) {
                state->network->onError(guardHandler(state, std::move(handler)));
            }
            armPreflightLocked(state);
        }
        state->preflightCv.notify_all();
    }

    void send(const std::string& packetName, ProtoDefValue value) {
        requireNetwork()->send(packetName, value);
    }

    void sendBuffer(const std::vector<uint8_t>& buffer, bool immediate = false) {
        requireNetwork()->sendBuffer(buffer, immediate);
    }

    void write(const std::string& packetName, ProtoDefValue value) {
        requireNetwork()->write(packetName, value);
    }

    void queue(const std::string& packetName, ProtoDefValue value) {
        auto state = state_;
        BedrockNetworkClient* network = nullptr;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->network) throw std::runtime_error("Client not initialized");
            state->queuedValues.push_back({packetName, value});
            network = state->network.get();
        }
        network->queue(packetName, value);
    }

    void sendQueued() {
        requireNetwork()->sendQueued();
    }

    const auto& queuedPacketValues() const {
        return state_->queuedValues;
    }

    BedrockNetworkClient& network() {
        return *requireNetwork();
    }

    const BedrockNetworkClient& network() const {
        return *requireNetwork();
    }

    BedrockWorld& world() {
        return requireNetwork()->world();
    }

    const BedrockWorld& world() const {
        return requireNetwork()->world();
    }

private:
    Client(Options options, FactoryTag)
        : state_(std::make_shared<State>(std::move(options))) {
        state_->options.delayedInit.setResolved(true);
        // createClient.js constructs Client with
        // `{ port: 19132, followPort: !options.realms, ...options }`.
        // Preserve an explicit zero while supplying the factory-only default.
        if (!state_->options.port.provided()) {
            state_->options.port.setResolved(19132);
        }
        if (!state_->options.followPort.provided()) {
            state_->options.followPort.setResolved(
                !static_cast<bool>(state_->options.realms)
            );
        }
        state_->factoryMode = true;

        // onServerInfo installs this listener before createClient returns.
        // It launches the blocking native C++ connect on a tracked worker so
        // later connect_allowed listeners run immediately, as in JavaScript.
        std::weak_ptr<State> weak = state_;
        state_->connectAllowedHandlers.push_back([weak]() {
            if (auto state = weak.lock()) markAutoConnect(state);
        });

        if (state_->options.skipPing &&
            !static_cast<bool>(state_->options.realms)) {
            initialize(state_);
            return;
        }

        auto state = state_;
        std::lock_guard<std::mutex> workersLock(state->workersMutex);
        state->preflightThread = std::thread([state]() {
            // createClient.js starts the asynchronous ping immediately. Only
            // its Promise continuation is deferred; do the same here.
            const auto deliverFailure = [state](const std::string& message) {
                // C++ has no current-stack microtask hook. Gate error delivery
                // on an immediately registered listener, with a bounded
                // fallback for callers that intentionally install none.
                {
                    std::unique_lock<std::mutex> lock(state->mutex);
                    state->preflightCv.wait_for(
                        lock,
                        std::chrono::milliseconds(25),
                        [&]() {
                            return state->externalErrorSeen ||
                                !state->ownerAlive.load();
                        }
                    );
                    if (!state->ownerAlive.load()) return;
                }
                emitFactoryError(state, message);
            };
            try {
                runPreflight(state);
            } catch (const std::exception& e) {
                deliverFailure(e.what());
            } catch (...) {
                // No exception may escape a std::thread entry point. Node
                // would surface a non-Error throw as an unhandled async
                // rejection; expose the native equivalent through the sink.
                deliverFailure("Unknown asynchronous error");
            }
        });
    }

    static Options normalizeOptions(Options options) {
        if (options.host.empty() || !options.port.has_value()) {
            throw std::runtime_error("Invalid host/port");
        }
        if (options.version.empty() && !options.version.provided()) {
            options.version.setResolved(std::string(CURRENT_VERSION));
        }
        if (isObjectPrototypeVersionName(options.version)) {
            // Options.validateOptions accepts inherited truthy members of its
            // ordinary Versions object. minecraft-data then supplies no schema
            // and ProtoDef throws this TypeError during createSerializer().
            throw std::runtime_error(
                "Cannot read properties of undefined (reading 'types')"
            );
        }
        options.protocolVersion = validateVersion(options.version);

        // options.js applies this deprecated override after version
        // validation, regardless of an explicitly supplied backend.
        if (options.useNativeRaknet.has_value()) {
            options.raknetBackend = *options.useNativeRaknet
                ? "raknet-native"
                : "jsp-raknet";
        }
        if (!options.raknetBackend.empty() &&
            options.raknetBackend != "raknet-native") {
            // The native port currently has one RakNet implementation.  Fail
            // explicitly instead of accepting jsp-raknet/raknet-node and then
            // silently using a different backend.
            throw std::runtime_error(
                "RakNet backend is not implemented in C++: " +
                options.raknetBackend
            );
        }
        return options;
    }

    static void validateConnectionAddress(const Options& options) {
        addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        addrinfo* result = nullptr;
        const std::string host = options.host == "::1"
            ? std::string("127.0.0.1")
            : options.host;
        const auto service = std::to_string(static_cast<uint16_t>(options.port));
        const int status = getaddrinfo(
            host.c_str(), service.c_str(), &hints, &result
        );
        if (result) freeaddrinfo(result);
        if (status != 0) {
            throw std::runtime_error(
                "Invalid connection address " + options.host + "/" +
                std::to_string(static_cast<uint16_t>(options.port))
            );
        }
    }

    static BedrockNetworkClientOptions toNetworkOptions(const Options& options) {
        BedrockNetworkClientOptions out;
        out.host = options.host;
        out.port = static_cast<uint16_t>(options.port);
        out.username = options.username;
        out.profile = options.username;
        out.version = options.version;
        out.protocolVersion = options.protocolVersion;
        out.offline = options.offline;
        out.interactiveAuth = options.interactiveAuth;
        out.authTitle = options.authTitle;
        out.deviceType = options.deviceType;
        out.flow = options.flow;
        out.forceRefresh = options.forceRefresh;
        out.msalConfig = options.msalConfig;
        out.authflow = options.authflow;
        out.password = options.password;
        out.xboxClientId = options.xboxClientId;
        out.authCacheRoot = options.authCacheRoot;
        out.onMsaCode = options.onMsaCode;
        out.mtu = options.mtu;
        const int effectiveConnectTimeout = options.connectTimeout.truthy()
            ? std::max(*options.connectTimeout, 1)
            : 9000;
        const int effectiveBatchingInterval = options.batchingInterval.truthy()
            ? std::max(*options.batchingInterval, 1)
            : 20;
        // client.js reads only the canonical properties at their use sites.
        // Keep unknown legacy fields observable in Client.options, but never
        // let them alter the canonical timer behavior.
        out.connectTimeoutMs = effectiveConnectTimeout;
        out.batchingIntervalMs = effectiveBatchingInterval;
        out.compressionLevel = options.compressionLevel;
        out.autoInitPlayer = options.autoInitPlayer;
        out.autoResourcePackResponses = options.autoResourcePackResponses;
        out.clientCacheEnabled = options.clientCacheEnabled;
        out.trackWorld = options.trackWorld;
        // createClient.js reads client.viewDistance rather than
        // client.options.viewDistance/chunkRadius. Neither root Options field
        // affects the request in the upstream implementation.
        out.chunkRadius = 10;
        out.loginPacket = options.loginPacket;
        out.clientDataJson = options.clientDataJson;
        out.connectTimeout = effectiveConnectTimeout;
        out.batchingInterval = effectiveBatchingInterval;
        out.viewDistance = options.viewDistance.optionalValue();
        out.profilesFolder = options.profilesFolder;
        out.raknetBackend = options.raknetBackend;
        out.useRaknetWorkers = options.useRaknetWorkers;
        out.useNativeRaknet = options.useNativeRaknet;
        out.compressionAlgorithm = options.compressionAlgorithm;
        out.compressionThreshold = options.compressionThreshold;
        return out;
    }

    static std::string firstThreeVersionUnits(const std::string& version) {
        std::size_t end = version.size();
        std::size_t search = 0;
        for (int unit = 0; unit < 3; ++unit) {
            const auto dot = version.find('.', search);
            if (dot == std::string::npos) return version;
            if (unit == 2) {
                end = dot;
                break;
            }
            search = dot + 1;
        }
        return version.substr(0, end);
    }

    static bool delayedStringComparison(const std::string& version) {
        if (findVersion(version)) {
            // JavaScript compares undefined protocolVersion with a Number
            // before Client.init(); every relational operator returns false.
            return false;
        }
        // Options.Versions is a normal object, so inherited Object.prototype
        // names pass its truthiness check and then coerce to NaN.
        if (isObjectPrototypeVersionName(version)) return false;
        throw std::runtime_error("Unknown version: " + version);
    }

    static bool isObjectPrototypeVersionName(std::string_view version) {
        for (const std::string_view inherited : {
                 "constructor",
                 "__defineGetter__",
                 "__defineSetter__",
                 "hasOwnProperty",
                 "__lookupGetter__",
                 "__lookupSetter__",
                 "isPrototypeOf",
                 "propertyIsEnumerable",
                 "toString",
                 "valueOf",
                 "__proto__",
                 "toLocaleString"
             }) {
            if (version == inherited) return true;
        }
        return false;
    }

    static ServerAdvertisement pingForFactory(const Options& options) {
        const auto pong = RakNetPinger::ping(options.host, options.port, 1000);
        if (!pong.ok) {
            if (pong.timedOut) throw RakTimeout("Ping timed out");
            throw std::runtime_error(
                pong.error.empty() ? "Bedrock ping failed" : pong.error
            );
        }
        return fromServerName(pong.rawMotd);
    }

    static void resolveRealmForFactory(const std::shared_ptr<State>& state) {
        Options options;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->ownerAlive.load()) return;
            options = state->options;
        }

        NativeBedrockAuthflowOptions nativeAuthOptions {
            .username = options.username,
            .profilesFolder = options.profilesFolder,
            .authTitle = options.authTitle,
            .deviceType = options.deviceType,
            .flow = options.flow,
            .forceRefresh = options.forceRefresh,
            .msalConfig = options.msalConfig,
            .password = options.password,
            .onMsaCode = options.onMsaCode
        };
        validateNativeBedrockAuthflowOptions(nativeAuthOptions);

        // realmAuthenticate() publishes validateOptions mutations before the
        // PrismarineAuth constructor can fail.
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->ownerAlive.load()) return;
            state->options.profilesFolder =
                nativeAuthOptions.profilesFolder;
            state->options.authTitle = nativeAuthOptions.authTitle;
            state->options.deviceType = nativeAuthOptions.deviceType;
            state->options.flow = nativeAuthOptions.flow;
        }

        validateNativeBedrockAuthflowPresence(nativeAuthOptions);
        const auto effectiveCacheRoot =
            initializeNativeBedrockAuthCacheRoot(
                nativeAuthOptions.profilesFolder
            );
        auto authRuntime = createNativeBedrockAuthflow(
            nativeAuthOptions,
            effectiveCacheRoot
        );
        auto authflow = std::move(authRuntime.authflow);

        // realmAuthenticate assigns the newly constructed flow back to the
        // caller's options before its first Realms request. Later game login
        // therefore reuses this exact object.
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->ownerAlive.load()) return;
            state->options.authflow = authflow;
            options = state->options;
        }

        auto address = resolveBedrockRealmAddress(
            authflow,
            options.realms
        );

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->ownerAlive.load()) return;
            state->options.host = std::move(address.host);
            state->options.port.setResolved(address.port);
        }
    }

    static void runPreflight(const std::shared_ptr<State>& state) {
        Options pingOptions;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->ownerAlive.load()) return;
            pingOptions = state->options;
        }

        if (static_cast<bool>(pingOptions.realms)) {
            resolveRealmForFactory(state);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->ownerAlive.load()) return;
                pingOptions = state->options;
            }
        }

        if (pingOptions.skipPing) {
            initialize(state);
            return;
        }

        const auto advertisement = pingForFactory(pingOptions);
        std::function<void(const std::string&)> conLog;
        std::string logMessage;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (!state->ownerAlive.load()) return;

            // If ping was armed by onError, give the caller's immediately
            // following onConnectAllowed registration a deterministic gate
            // before an instant local pong can initialize and emit.
            state->preflightCv.wait_for(
                lock,
                std::chrono::milliseconds(25),
                [&]() {
                    return state->externalConnectAllowedSeen ||
                        !state->ownerAlive.load();
                }
            );
            if (!state->ownerAlive.load()) return;

            const auto advertisedVersion = advertisement.version.stringValue();
            const auto candidate = advertisedVersion
                ? firstThreeVersionUnits(*advertisedVersion)
                : std::string();
            state->options.version.setResolved(state->requestedVersion.value_or(
                (findVersion(candidate) || isObjectPrototypeVersionName(candidate))
                    ? candidate
                    : std::string(CURRENT_VERSION)
            ));

            if (state->options.followPort.value_or(false) &&
                advertisement.portV4.isTruthy()) {
                if (const auto advertisedPort = advertisement.portV4.toInteger()) {
                    state->options.port.setResolved(
                        static_cast<uint16_t>(*advertisedPort)
                    );
                }
            }

            conLog = state->options.conLog;
            if (conLog) {
                const auto adVersion = advertisement.version.toString();
                logMessage = "Connecting to " + state->options.host + ":" +
                    std::to_string(
                        static_cast<uint16_t>(state->options.port)
                    ) + " " +
                    advertisement.motd.toString() + " (" +
                    advertisement.levelName.toString() + "), version " +
                    adVersion + " " +
                    (state->options.version != adVersion
                        ? " (as " + state->options.version + ")"
                        : std::string());
            }
        }

        if (conLog) conLog(logMessage);
        initialize(state);
    }

    static void initialize(const std::shared_ptr<State>& state) {
        Options options;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->ownerAlive.load() || state->initialized) return;
            options = state->options;
        }

        options = normalizeOptions(std::move(options));
        validateConnectionAddress(options);
        auto network = std::make_shared<BedrockNetworkClient>(
            toNetworkOptions(options)
        );
        auto decoder = std::make_unique<ProtoDefPacketDecoder>(
            options.version,
            network->packetVariableStore()
        );
        std::weak_ptr<State> weakState = state;
        network->setAuthenticationOptionsResolvedHandler(
            [weakState](BedrockNetworkClientOptions resolved) {
                auto current = weakState.lock();
                if (!current) return;
                std::lock_guard<std::mutex> lock(current->mutex);
                if (!current->ownerAlive.load()) return;
                current->options.authTitle = resolved.authTitle;
                current->options.deviceType = resolved.deviceType;
                current->options.flow = resolved.flow;
                current->options.profilesFolder = resolved.profilesFolder;
            }
        );
        network->setAuthenticationUnhandledRejectionHandler(
            [weakState](std::string message) {
                auto current = weakState.lock();
                if (!current) return;
                recordUnhandledAsyncError(
                    current,
                    std::move(message),
                    true
                );
            }
        );
        network->setCallbackLifetimeProvider([weakState]() -> std::shared_ptr<void> {
            auto current = weakState.lock();
            if (!current || !current->ownerAlive.load() ||
                current->callbacksSuppressed.load()) {
                return {};
            }
            return current;
        });

        std::vector<std::function<void()>> connectAllowed;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->ownerAlive.load() || state->initialized) return;

            state->options = std::move(options);
            state->decoder = std::move(decoder);
            state->network = std::move(network);

            for (auto& [name, handler] : state->packetHandlers) {
                attachPacketHandlerLocked(state, name, std::move(handler));
            }
            state->packetHandlers.clear();
            for (auto& handler : state->anyHandlers) {
                attachAnyHandlerLocked(state, std::move(handler));
            }
            state->anyHandlers.clear();
            for (auto& handler : state->textHandlers) {
                attachTextHandlerLocked(state, std::move(handler));
            }
            state->textHandlers.clear();
            attachPendingNetworkHandlersLocked(state);

            state->initialized = true;
            if (!state->connectAllowedEmitted) {
                state->connectAllowedEmitted = true;
                connectAllowed = state->connectAllowedHandlers;
            }
        }

        // EventEmitter snapshots the listeners participating in this emit.
        // The factory's listener is first and records the synchronous connect
        // intent. The blocking native worker starts only after the complete
        // snapshot, so a later listener can close/cancel exactly in this turn.
        try {
            for (auto& handler : connectAllowed) handler();
        } catch (...) {
            // EventEmitter stops at the throwing listener, but the factory's
            // earlier internal connect() call has already happened. Preserve
            // that side effect before init's rejected continuation is routed
            // to the error event by runPreflight().
            launchAutoConnectWorker(state);
            throw;
        }
        launchAutoConnectWorker(state);
    }

    static void markAutoConnect(const std::shared_ptr<State>& state) {
        std::shared_ptr<BedrockNetworkClient> network;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->ownerAlive.load() || state->autoConnectCancelled ||
                state->autoConnectStarted || !state->network) {
                return;
            }
            state->autoConnectStarted = true;
            ++state->autoConnectInvocations;
            network = state->network;
        }

        // Client.connect() calls startQueue() synchronously before returning,
        // so a later listener in this same connect_allowed snapshot queues
        // after the reset. Only the blocking RakNet handshake is deferred to
        // launchAutoConnectWorker().
        try {
            const bool prepared = network->prepareConnectLifecycle(true);
            if (prepared) return;
            std::lock_guard<std::mutex> lock(state->mutex);
            state->autoConnectCancelled = true;
            state->autoConnectFinished = true;
        } catch (...) {
            // initialize() will route this exception to the factory error
            // boundary. Prevent its catch path from launching a second worker
            // that retries the failed synchronous connect preparation.
            std::lock_guard<std::mutex> lock(state->mutex);
            state->autoConnectCancelled = true;
            state->autoConnectFinished = true;
            throw;
        }
    }

    static void launchAutoConnectWorker(const std::shared_ptr<State>& state) {
        BedrockNetworkClient* network = nullptr;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->ownerAlive.load() || state->autoConnectCancelled ||
                !state->autoConnectStarted || state->connectWorkerStarted ||
                !state->network) {
                return;
            }
            state->connectWorkerStarted = true;
            network = state->network.get();
        }

        std::lock_guard<std::mutex> workersLock(state->workersMutex);
        state->connectThread = std::thread([state, network]() {
            if (!state->ownerAlive.load()) {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->autoConnectFinished = true;
                    state->connectWorkerFinalization =
                        State::ConnectWorkerFinalization::WillClose;
                }
                try { network->close("closed"); } catch (...) {}
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->connectWorkerFinalization =
                        State::ConnectWorkerFinalization::Exited;
                    state->connectWorkerExited = true;
                }
                return;
            }
            try {
                (void) network->connect();
            } catch (const std::exception& e) {
                // connect() already emitted its own error. Re-emitting here
                // would call a throwing listener twice; retain only the
                // uncaught boundary diagnostic for the native worker.
                recordUnhandledAsyncError(state, e.what(), true);
            } catch (...) {
                // A user callback can throw a non-std C++ value. Never let it
                // cross the worker entry point and terminate the process.
                recordUnhandledAsyncError(
                    state,
                    "Unknown asynchronous error",
                    true
                );
            }
            std::function<void()> afterConnectHook;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                afterConnectHook = state->afterNetworkConnectTestHook;
            }
            if (afterConnectHook) afterConnectHook();
            bool closeAfterConnect = false;
            std::string closeReason = "closed";
            std::function<void()> afterWorkerDecisionHook;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->autoConnectFinished = true;
                closeAfterConnect = state->deferredClose ||
                    !state->ownerAlive.load();
                if (state->deferredClose) {
                    closeReason = state->deferredCloseReason;
                }
                state->connectWorkerFinalization = closeAfterConnect
                    ? State::ConnectWorkerFinalization::WillClose
                    : State::ConnectWorkerFinalization::NoClose;
                afterWorkerDecisionHook =
                    state->afterWorkerDecisionTestHook;
            }
            if (afterWorkerDecisionHook) afterWorkerDecisionHook();
            // If close() or destruction happened on this worker, shutdown
            // deliberately did not touch RakNet reentrantly. connect() has
            // unwound now, so the deferred close is safe.
            if (closeAfterConnect) {
                try { network->close(closeReason); } catch (...) {}
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->connectWorkerFinalization =
                    State::ConnectWorkerFinalization::Exited;
                state->connectWorkerExited = true;
            }
        });
    }

    static void emitFactoryError(
        const std::shared_ptr<State>& state,
        const std::string& message
    ) {
        std::vector<std::function<void(const std::string&)>> handlers;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->ownerAlive.load()) return;
            handlers = state->errorHandlers;
        }
        if (handlers.empty()) {
            // Admission was fixed by the EventEmitter-style snapshot above;
            // a listener added after that point cannot retroactively handle
            // this error or suppress its unhandled boundary.
            recordUnhandledAsyncError(state, message, true);
            return;
        }
        try {
            for (auto& handler : handlers) handler(message);
        } catch (const std::exception& error) {
            // EventEmitter stops at the throwing listener. A std::thread
            // cannot carry that exception to the caller, so retain and expose
            // it through the preinstalled unhandled boundary instead.
            recordUnhandledAsyncError(state, error.what(), true);
        } catch (...) {
            recordUnhandledAsyncError(
                state,
                "Unknown asynchronous error",
                true
            );
        }
    }

    static void recordUnhandledAsyncError(
        const std::shared_ptr<State>& state,
        const std::string& message,
        bool invokeSink
    ) noexcept {
        std::function<void(const std::string&)> sink;
        try {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (invokeSink) {
                    sink = state->options.onUnhandledAsyncError;
                }
            }
            {
                std::lock_guard<std::mutex> lock(
                    state->unhandledDeliveryMutex
                );
                state->pendingUnhandledAsyncErrors.push_back({message, sink});
                if (state->unhandledDeliveryActive) return;
                state->unhandledDeliveryActive = true;
            }
        } catch (...) {
            return;
        }

        // One caller becomes the drainer. Concurrent and reentrant records
        // only append; user sinks run outside every mutex, in occurrence
        // order, and never concurrently on the preflight/connect workers.
        for (;;) {
            typename State::PendingUnhandledAsyncError pending;
            {
                std::lock_guard<std::mutex> lock(
                    state->unhandledDeliveryMutex
                );
                if (state->pendingUnhandledAsyncErrors.empty()) {
                    state->unhandledDeliveryActive = false;
                    return;
                }
                pending = std::move(
                    state->pendingUnhandledAsyncErrors.front()
                );
                state->pendingUnhandledAsyncErrors.pop_front();
            }
            try {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->unhandledAsyncError = pending.message;
                }
                state->preflightCv.notify_all();
                if (pending.sink) {
                    try { pending.sink(pending.message); } catch (...) {}
                }
            } catch (...) {
                // Never allow storage/allocation or a user sink to terminate
                // the preflight/connect std::thread boundary. Continue
                // draining later occurrences even if this record was lossy.
            }
        }
    }

    template <typename Handler, typename Attach>
    void addNetworkHandler(
        std::vector<Handler>& pending,
        Handler handler,
        Attach attach
    ) {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->network) {
            attach(*state->network, guardHandler(state, std::move(handler)));
        } else {
            pending.push_back(std::move(handler));
        }
        armPreflightLocked(state);
    }

    static void armPreflightLocked(const std::shared_ptr<State>& state) {
        state->preflightCv.notify_all();
    }

    static std::function<void()> guardHandler(
        const std::shared_ptr<State>& state,
        std::function<void()> handler
    ) {
        std::weak_ptr<State> weak = state;
        return [weak, handler = std::move(handler)]() {
            auto lifetimeLease = weak.lock();
            if (!lifetimeLease) return;
            handler();
        };
    }

    static SessionHandler guardHandler(
        const std::shared_ptr<State>& state,
        SessionHandler handler
    ) {
        std::weak_ptr<State> weak = state;
        return [weak, handler = std::move(handler)](
            const BedrockClientProfile& profile
        ) {
            auto lifetimeLease = weak.lock();
            if (!lifetimeLease) return;
            handler(profile);
        };
    }

    static std::function<void(int64_t)> guardHandler(
        const std::shared_ptr<State>& state,
        std::function<void(int64_t)> handler
    ) {
        std::weak_ptr<State> weak = state;
        return [weak, handler = std::move(handler)](int64_t value) {
            auto lifetimeLease = weak.lock();
            if (!lifetimeLease) return;
            handler(value);
        };
    }

    static std::function<void(ClientStatus)> guardHandler(
        const std::shared_ptr<State>& state,
        std::function<void(ClientStatus)> handler
    ) {
        std::weak_ptr<State> weak = state;
        return [weak, handler = std::move(handler)](ClientStatus value) {
            auto lifetimeLease = weak.lock();
            if (!lifetimeLease) return;
            handler(value);
        };
    }

    static std::function<void(const std::string&)> guardHandler(
        const std::shared_ptr<State>& state,
        std::function<void(const std::string&)> handler
    ) {
        std::weak_ptr<State> weak = state;
        return [weak, handler = std::move(handler)](const std::string& value) {
            auto lifetimeLease = weak.lock();
            if (!lifetimeLease) return;
            handler(value);
        };
    }

    static void attachPendingNetworkHandlersLocked(
        const std::shared_ptr<State>& state
    ) {
        for (auto& handler : state->sessionHandlers) {
            state->network->onSession(guardHandler(state, std::move(handler)));
        }
        state->sessionHandlers.clear();
        for (auto& handler : state->loggingInHandlers) {
            state->network->onLoggingIn(guardHandler(state, std::move(handler)));
        }
        state->loggingInHandlers.clear();
        for (auto& handler : state->joinHandlers) {
            state->network->onJoin(guardHandler(state, std::move(handler)));
        }
        state->joinHandlers.clear();
        for (auto& handler : state->spawnHandlers) {
            state->network->onSpawn(guardHandler(state, std::move(handler)));
        }
        state->spawnHandlers.clear();
        for (auto& handler : state->heartbeatHandlers) {
            state->network->onHeartbeat(guardHandler(state, std::move(handler)));
        }
        state->heartbeatHandlers.clear();
        for (auto& handler : state->statusHandlers) {
            state->network->onStatus(guardHandler(state, std::move(handler)));
        }
        state->statusHandlers.clear();
        for (auto& handler : state->closeHandlers) {
            state->network->onClose(guardHandler(state, std::move(handler)));
        }
        state->closeHandlers.clear();
        for (auto& handler : state->errorHandlers) {
            state->network->onError(guardHandler(state, handler));
        }
    }

    static void attachPacketHandlerLocked(
        const std::shared_ptr<State>& state,
        const std::string& packetName,
        PacketHandler handler
    ) {
        std::weak_ptr<State> weak = state;
        state->network->on(packetName, [weak, handler = std::move(handler)](
            const BedrockNetworkClientPacketEvent& event
        ) {
            if (auto current = weak.lock()) {
                handler(toApiPacket(current, event.packet));
            }
        });
    }

    static void attachAnyHandlerLocked(
        const std::shared_ptr<State>& state,
        PacketHandler handler
    ) {
        std::weak_ptr<State> weak = state;
        state->network->onAny([weak, handler = std::move(handler)](
            const BedrockNetworkClientPacketEvent& event
        ) {
            if (auto current = weak.lock()) {
                handler(toApiPacket(current, event.packet));
            }
        });
    }

    static void attachTextHandlerLocked(
        const std::shared_ptr<State>& state,
        TextHandler handler
    ) {
        std::weak_ptr<State> weak = state;
        state->network->on("text", [weak, handler = std::move(handler)](
            const BedrockNetworkClientPacketEvent& event
        ) {
            if (auto current = weak.lock()) {
                auto packet = toApiPacket(current, event.packet);
                auto text = api::textPacketFromPacket(packet);
                handler(text);
            }
        });
    }

    static Packet toApiPacket(
        const std::shared_ptr<State>& state,
        const VersionedGamePacket& packet,
        bool forceDecode = false
    ) {
        std::lock_guard<std::mutex> lock(state->mutex);
        Packet out;
        out.id = packet.packetId;
        out.name = packet.name;
        out.ok = true;

        if (forceDecode || state->options.decodePackets) {
            const bool shouldDecode =
                forceDecode || state->decodeAnyPacket ||
                state->subscribedPackets.find(packet.name) != state->subscribedPackets.end();
            if (!shouldDecode) return out;

            try {
                auto fields = state->decoder->decodePacket(packet.name, packet.payload);
                for (const auto& field : fields) {
                    // ProtoDef's diagnostic field form retains mapper values
                    // as `numeric/name`. JavaScript packet params expose only
                    // the mapped enum name, so normalize at the root Client
                    // facade while leaving low-level inspector fields intact.
                    auto value = field.value;
                    if (field.type.rfind("mapper<", 0) == 0) {
                        const auto separator = value.find('/');
                        if (separator != std::string::npos) {
                            value = value.substr(separator + 1);
                        }
                    }
                    out.fields[field.path] = value;
                    auto c1 = value.find(',');
                    auto c2 = c1 == std::string::npos
                        ? std::string::npos
                        : value.find(',', c1 + 1);
                    if (c1 != std::string::npos && c2 != std::string::npos) {
                        out.fields[field.path + ".x"] = value.substr(0, c1);
                        out.fields[field.path + ".y"] = value.substr(c1 + 1, c2 - c1 - 1);
                        out.fields[field.path + ".z"] = value.substr(c2 + 1);
                    }
                    auto dot = field.path.rfind('.');
                    if (dot != std::string::npos) {
                        out.fields[field.path.substr(dot + 1)] = value;
                    }
                }
            } catch (const std::exception& e) {
                out.ok = false;
                out.fields["decode_error"] = e.what();
            }
        }

        if (state->options.debug != DebugMode::Off &&
            (!state->options.quiet || state->options.packetDump)) {
            std::cout << "[packet] " << out.name << " id=" << out.id << "\n";
        }
        return out;
    }

    BedrockNetworkClient* requireNetwork() {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->network) throw std::runtime_error("Client not initialized");
        return state->network.get();
    }

    const BedrockNetworkClient* requireNetwork() const {
        auto state = state_;
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->network) throw std::runtime_error("Client not initialized");
        return state->network.get();
    }

    static bool joinWorker(
        const std::shared_ptr<State>& state,
        bool preflight
    ) {
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(state->workersMutex);
            auto& source = preflight ? state->preflightThread : state->connectThread;
            if (!source.joinable()) return false;
            if (source.get_id() == std::this_thread::get_id()) {
                source.detach();
                return true;
            }
            worker = std::move(source);
        }
        worker.join();
        return false;
    }

    void shutdown() noexcept {
        auto state = std::move(state_);
        if (!state) return;
        state->ownerAlive.store(false);

        std::shared_ptr<BedrockNetworkClient> network;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            network = state->network;
        }

        // Causal callback threads must escape before *either* worker join. A
        // preflight thread may already have published the network and be
        // blocked in a concurrent close/connect_allowed callback, so joining
        // it first can form the same latch cycle as joining connect below.
        //
        // A close listener can destroy the facade while this very thread owns
        // BedrockNetworkClient's EventEmitter-style close latch. Joining the
        // connect worker here creates a causal cycle: the worker observes the
        // dead owner, calls network->close(), and waits for the latch that this
        // listener can release only after shutdown returns. The active close
        // snapshot/worker both lease State, so publish deferred ownership,
        // stop transport I/O and let the outer emission unwind without a
        // recursive close or join.
        if (network && network->onCloseEmissionThread()) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->callbacksSuppressed.store(true);
                state->autoConnectCancelled = true;
                using Finalization = State::ConnectWorkerFinalization;
                if (state->connectWorkerStarted &&
                    state->connectWorkerFinalization ==
                        Finalization::Undecided) {
                    state->deferredClose = true;
                    state->deferredCloseReason = "closed";
                }
            }
            network->requestRakNetStop();
            return;
        }

        // A RakNet callback may destroy the public Client while the factory's
        // connect worker is still returning from BedrockNetworkClient::connect.
        // Joining it here would deadlock: that worker closes/joins this RakNet
        // callback thread. Mark the close for the connect worker and let the
        // callback unwind under its transport-held State lease.
        if (network && network->onRakNetCallbackThread()) {
            bool workerWillClose = false;
            bool closeHereWithoutJoin = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->callbacksSuppressed.store(true);
                state->autoConnectCancelled = true;
                using Finalization = State::ConnectWorkerFinalization;
                if (state->connectWorkerStarted &&
                    state->connectWorkerFinalization ==
                        Finalization::Undecided) {
                    // The worker has not made its final close decision yet and
                    // is guaranteed to observe this flag under the same mutex.
                    state->deferredClose = true;
                    state->deferredCloseReason = "closed";
                    workerWillClose = true;
                } else if (state->connectWorkerFinalization ==
                           Finalization::WillClose) {
                    workerWillClose = true;
                } else {
                    // No worker exists, or it already committed to NoClose /
                    // Exited. Close callback-safely here without joining a
                    // thread that may still be returning from its final hook.
                    closeHereWithoutJoin = true;
                }
            }
            network->requestRakNetStop();
            if (workerWillClose) return;
            if (closeHereWithoutJoin) {
                try { network->close(); } catch (...) {}
                return;
            }
        }

        // The ordinary, non-causal destruction path retains the strict join
        // order: preflight is the only thread that can create connectThread.
        // Re-snapshot network afterwards because initialization could already
        // have committed it before observing ownerAlive=false.
        (void) joinWorker(state, true);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            network = state->network;
        }

        if (network) network->requestRakNetStop();
        const bool onConnectWorker = joinWorker(state, false);
        if (network && !onConnectWorker) {
            try { network->close(); } catch (...) {}
        }
    }
};

// Direct in-process network client, if needed later.






namespace detail {

inline Options expandClientOptions(ClientOptions options) {
    Options out;
    out.host = std::move(options.host);
    out.port = options.port;
    out.username = std::move(options.username);
    if (options.version.has_value()) {
        out.version = std::move(*options.version);
    }
    out.offline = options.offline;
    out.autoInitPlayer = options.autoInitPlayer;
    if (options.viewDistance.has_value()) {
        out.viewDistance = *options.viewDistance;
    }
    if (options.authTitle.has_value()) {
        out.authTitle = std::move(*options.authTitle);
    }
    out.connectTimeout = options.connectTimeout;
    out.skipPing = options.skipPing;
    if (options.followPort.has_value()) {
        out.followPort = *options.followPort;
    }
    out.conLog = std::move(options.conLog);
    out.realms = std::move(options.realms);
    out.profilesFolder = std::move(options.profilesFolder);
    out.onMsaCode = std::move(options.onMsaCode);
    out.raknetBackend = std::move(options.raknetBackend);
    out.useRaknetWorkers = options.useRaknetWorker;
    out.compressionLevel = options.compressionLevel;
    out.batchingInterval = options.batchingInterval;
    out.debug = options.diagnostics.debug;
    out.decodePackets = options.diagnostics.decodePackets;
    out.packetDump = options.diagnostics.packetDump;
    out.quiet = options.diagnostics.quiet;
    return out;
}

} // namespace detail

inline Client createClient(ClientOptions options) {
    return Client::createFactory(detail::expandClientOptions(std::move(options)));
}

// Typed legacy Options calls keep working.  This is intentionally a template:
// a direct createClient({...}) cannot deduce it and therefore selects the
// compact ClientOptions aggregate above.
template <typename LegacyOptions>
requires std::is_same_v<std::remove_cvref_t<LegacyOptions>, Options>
inline Client createClient(LegacyOptions&& options) {
    return Client::createFactory(
        Options(std::forward<LegacyOptions>(options))
    );
}

inline Client createBot(BotOptions options) {
    return createClient(std::move(options));
}

// Manual root Client construction without createClient's automatic connect.
// The explicit name avoids colliding with the native BedrockNetworkClient
// factory exported by its own header.
inline Client createManualClient(Options options = {}) {
    return Client(std::move(options));
}

template <typename LegacyOptions>
requires std::is_same_v<std::remove_cvref_t<LegacyOptions>, Options>
inline Client createNetworkClient(LegacyOptions&& options) {
    return createManualClient(
        Options(std::forward<LegacyOptions>(options))
    );
}

struct PingOptions {
    std::string host;
    std::optional<uint16_t> port;
};

inline ServerAdvertisement ping(PingOptions options) {
    if (!options.port.has_value()) {
        throw std::invalid_argument("Wrong arguments");
    }

    auto pong = RakNetPinger::ping(options.host, *options.port, 1000);
    if (!pong.ok) {
        if (pong.timedOut) {
            throw RakTimeout("Ping timed out");
        }
        throw std::runtime_error(pong.error.empty() ? "Bedrock ping failed" : pong.error);
    }
    return fromServerName(pong.rawMotd);
}

struct RelayDestination {
    std::string host = "127.0.0.1";
    uint16_t port = 19132;
    BedrockRealmSelection realms;
    // JavaScript uses destination.offline ?? relay.offline, so omission must
    // remain distinguishable from an explicit false value.
    std::optional<bool> offline;
};

class RelayPlayer;

// Accept both the current JavaScript callback shape `(code, player)` and the
// former C++ one-argument shape `(code)`. This keeps existing source working
// while making the downstream session available to new code.
class RelayMsaCodeCallback {
public:
    using ContextHandler = std::function<void(
        const XboxDeviceCodeInfo&,
        RelayPlayer&
    )>;
    using LegacyHandler = std::function<void(const XboxDeviceCodeInfo&)>;

    RelayMsaCodeCallback() = default;
    RelayMsaCodeCallback(std::nullptr_t) noexcept {}

    RelayMsaCodeCallback(ContextHandler handler)
        : handler_(std::move(handler)) {}

    RelayMsaCodeCallback(LegacyHandler handler) {
        assignLegacy(std::move(handler));
    }

    template <typename Handler>
    requires (!std::is_same_v<std::remove_cvref_t<Handler>, RelayMsaCodeCallback>)
    RelayMsaCodeCallback(Handler&& handler) {
        assign(std::forward<Handler>(handler));
    }

    RelayMsaCodeCallback& operator=(std::nullptr_t) noexcept {
        handler_ = {};
        return *this;
    }

    RelayMsaCodeCallback& operator=(ContextHandler handler) {
        handler_ = std::move(handler);
        return *this;
    }

    RelayMsaCodeCallback& operator=(LegacyHandler handler) {
        assignLegacy(std::move(handler));
        return *this;
    }

    template <typename Handler>
    requires (!std::is_same_v<std::remove_cvref_t<Handler>, RelayMsaCodeCallback>)
    RelayMsaCodeCallback& operator=(Handler&& handler) {
        assign(std::forward<Handler>(handler));
        return *this;
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(handler_);
    }

    void operator()(
        const XboxDeviceCodeInfo& code,
        RelayPlayer& player
    ) const {
        if (handler_) handler_(code, player);
    }

private:
    ContextHandler handler_;

    void assignLegacy(LegacyHandler handler) {
        if (!handler) {
            handler_ = {};
            return;
        }
        handler_ = [handler = std::move(handler)](
            const XboxDeviceCodeInfo& code,
            RelayPlayer&
        ) {
            handler(code);
        };
    }

    template <typename Handler>
    void assign(Handler&& handler) {
        using Callback = std::decay_t<Handler>;
        if constexpr (std::is_invocable_v<
                          Callback&,
                          const XboxDeviceCodeInfo&,
                          RelayPlayer&
                      >) {
            handler_ = std::forward<Handler>(handler);
        } else {
            static_assert(
                std::is_invocable_v<Callback&, const XboxDeviceCodeInfo&>,
                "Relay onMsaCode must accept (code) or (code, RelayPlayer&)"
            );
            handler_ = [handler = Callback(std::forward<Handler>(handler))](
                const XboxDeviceCodeInfo& code,
                RelayPlayer&
            ) mutable {
                handler(code);
            };
        }
    }
};

// Non-standard authentication injection points stay available without
// cluttering the normal JavaScript RelayOptions surface.
struct RelayAdvancedOptions {
    std::string username = "RelayBot";
    bool forceRefresh = false;
    MsalConfigPtr msalConfig;
    std::shared_ptr<Authflow> authflow;
    std::string password;
    XboxTokenHttpClientFactory httpClientFactory;
};

enum class RelayParseErrorPolicy {
    Disconnect,
    Drop,
    ForwardRaw
};

inline const char* relayParseErrorPolicyName(
    RelayParseErrorPolicy policy
) noexcept {
    switch (policy) {
        case RelayParseErrorPolicy::Disconnect: return "disconnect";
        case RelayParseErrorPolicy::Drop: return "drop";
        case RelayParseErrorPolicy::ForwardRaw: return "forward_raw";
    }
    return "disconnect";
}

struct RelayParseError {
    BedrockRelayDirection direction = BedrockRelayDirection::Clientbound;
    std::string sessionId;
    std::string packetName;
    std::string message;
    RelayParseErrorPolicy policy = RelayParseErrorPolicy::Disconnect;
};

struct RelayOptions {
    std::string version = std::string(CURRENT_VERSION);
    std::string host = "0.0.0.0";
    uint16_t port = 19132;
    ServerMotd motd = ServerMotd("Bedrock Protocol C++ Relay");
    bool offline = false;
    int maxPlayers = 3;
    std::string compressionAlgorithm = "deflate";
    int compressionLevel = 7;
    uint16_t compressionThreshold = 512;
    std::string raknetBackend = "raknet-native";
    bool useRaknetWorker = true;
    std::optional<std::string> authTitle;
    std::string deviceType;
    std::string flow;
    ProfilesFolderOption profilesFolder;
    // relay.js calls onMsaCode(code, downstreamPlayer). One-argument C++
    // callbacks remain accepted by RelayMsaCodeCallback for compatibility.
    RelayMsaCodeCallback onMsaCode;
    int batchingInterval = 20;
    bool logging = false;
    // Secret-safe resource-pack/item palette diagnostics. Disabled by
    // default and independent from packet parse-error policy.
    bool itemResourceDiagnostics = false;
    bool enableChunkCaching = false;
    std::size_t levelChunkRetentionMaximumBytes =
        DefaultLevelChunkRetentionMaximumBytes;
    bool forceSingle = false;
    // With forceSingle, let a new downstream replace a stale existing
    // session instead of rejecting the new transport.
    bool replaceExisting = false;
    // Match RelayPlayer.readUpstream: malformed backend packets are always
    // dropped, and normally disconnect that downstream unless this is true.
    bool omitParseErrors = false;
    RelayDestination destination;
    RelayAdvancedOptions advanced;
    // When omitted, the historical API is preserved: omitParseErrors=false
    // disconnects and omitParseErrors=true drops only the malformed packet.
    // ForwardRaw keeps the original VersionedGamePacket byte-for-byte and
    // deliberately skips field handlers for that packet.
    std::optional<RelayParseErrorPolicy> parseErrorPolicy;
    // Appended extension fields preserve existing positional aggregate
    // initialization while allowing frontends to tune each transport side.
    int downstreamRaknetTimeoutMs = 30'000;
    int upstreamRaknetTimeoutMs = 30'000;

    // A single root offline value is the common case and applies to both
    // sides. destination.offline exists only as an explicit upstream override.
    bool listenerOffline() const noexcept {
        return offline;
    }

    bool destinationOffline() const noexcept {
        return destination.offline.value_or(offline);
    }
};

struct RelayPacketDestination {
    bool canceled = false;

    void cancel() {
        canceled = true;
    }
};

class RelayPacketEvent {
public:
    BedrockRelayDirection direction = BedrockRelayDirection::Clientbound;
    std::string sessionId;
    std::string name;
    PacketObject params;
    VersionedGamePacket packet;
    bool canceled = false;

    RelayPacketEvent(
        std::string version,
        BedrockRelayPacketEvent& event,
        ProtoDefVariableStorePtr variables = {},
        bool strictDecode = false
    ) : direction(event.direction),
        sessionId(event.sessionId),
        name(event.packet.name),
        packet(event.packet),
        version_(std::move(version)),
        variables_(variables ? std::move(variables) : makeProtoDefVariableStore()),
        strictDecode_(strictDecode) {
        if (name == "start_game" || name == "item_registry") {
            ensureDecoded();
        }
    }

    void cancel() {
        canceled = true;
        replacements_.clear();
    }

    bool has(const std::string& key) const {
        ensureDecoded();
        return findNestedParam(params, key) != nullptr;
    }

    PacketObject& decodedParams() {
        ensureDecoded();
        return params;
    }

    const PacketObject& decodedParams() const {
        ensureDecoded();
        return params;
    }

    PacketObject& paramObject() {
        return decodedParams();
    }

    const PacketObject& paramObject() const {
        return decodedParams();
    }

    PacketValue& operator[](const std::string& key) {
        ensureDecoded();
        mutated_ = true;
        return params[key];
    }

    const PacketValue* value(const std::string& key) const {
        ensureDecoded();
        return findNestedParam(params, key);
    }

    PacketValue* value(const std::string& key) {
        ensureDecoded();
        return findNestedParam(params, key);
    }

    void set(const std::string& key, PacketValue value) {
        ensureDecoded();
        setNestedParam(params, key, std::move(value));
        mutated_ = true;
    }

    void set(const std::string& key, const std::string& value) {
        set(key, PacketValue::string(value));
    }

    void set(const std::string& key, const char* value) {
        set(key, PacketValue::string(value ? std::string(value) : std::string()));
    }

    void set(const std::string& key, bool value) {
        set(key, PacketValue::boolean(value));
    }

    void set(const std::string& key, int64_t value) {
        set(key, PacketValue::integer(value));
    }

    void set(const std::string& key, uint64_t value) {
        set(key, PacketValue::uinteger(value));
    }

    void set(const std::string& key, double value) {
        set(key, PacketValue::floating(value));
    }

    std::string get(const std::string& key) const {
        ensureDecoded();
        auto* found = findNestedParam(params, key);
        return found ? valueToString(*found) : std::string();
    }

    std::string getString(const std::string& key, std::string fallback = {}) const {
        auto* v = value(key);
        return v ? valueToString(*v) : fallback;
    }

    int64_t getInt(const std::string& key, int64_t fallback = 0) const {
        auto* v = value(key);
        if (!v) return fallback;
        if (v->kind == PacketValue::Kind::Int) return v->intValue;
        if (v->kind == PacketValue::Kind::UInt) return static_cast<int64_t>(v->uintValue);
        if (v->kind == PacketValue::Kind::Double) return static_cast<int64_t>(v->doubleValue);
        if (v->kind == PacketValue::Kind::Bool) return v->boolValue ? 1 : 0;
        if (v->kind == PacketValue::Kind::String && isIntegerText(v->stringValue)) {
            return static_cast<int64_t>(std::strtoll(v->stringValue.c_str(), nullptr, 10));
        }
        return fallback;
    }

    uint64_t getUInt(const std::string& key, uint64_t fallback = 0) const {
        auto* v = value(key);
        if (!v) return fallback;
        if (v->kind == PacketValue::Kind::UInt) return v->uintValue;
        if (v->kind == PacketValue::Kind::Int) return static_cast<uint64_t>(v->intValue);
        if (v->kind == PacketValue::Kind::Double) return static_cast<uint64_t>(v->doubleValue);
        if (v->kind == PacketValue::Kind::Bool) return v->boolValue ? 1u : 0u;
        if (v->kind == PacketValue::Kind::String && isIntegerText(v->stringValue)) {
            return static_cast<uint64_t>(std::strtoull(v->stringValue.c_str(), nullptr, 10));
        }
        return fallback;
    }

    double getDouble(const std::string& key, double fallback = 0.0) const {
        auto* v = value(key);
        if (!v) return fallback;
        if (v->kind == PacketValue::Kind::Double) return v->doubleValue;
        if (v->kind == PacketValue::Kind::Int) return static_cast<double>(v->intValue);
        if (v->kind == PacketValue::Kind::UInt) return static_cast<double>(v->uintValue);
        if (v->kind == PacketValue::Kind::Bool) return v->boolValue ? 1.0 : 0.0;
        if (v->kind == PacketValue::Kind::String) return std::strtod(v->stringValue.c_str(), nullptr);
        return fallback;
    }

    bool getBool(const std::string& key, bool fallback = false) const {
        auto* v = value(key);
        if (!v) return fallback;
        if (v->kind == PacketValue::Kind::Bool) return v->boolValue;
        if (v->kind == PacketValue::Kind::Int) return v->intValue != 0;
        if (v->kind == PacketValue::Kind::UInt) return v->uintValue != 0;
        if (v->kind == PacketValue::Kind::Double) return v->doubleValue != 0.0;
        if (v->kind == PacketValue::Kind::String) return v->stringValue == "true" || v->stringValue == "1";
        return fallback;
    }

    void replace(VersionedGamePacket replacement) {
        canceled = false;
        replacements_.clear();
        if (replacement.name == "start_game" || replacement.name == "item_registry") {
            ProtoDefPacketDecoder decoder(version_, variables_);
            decoder.updatePacketVariables(
                replacement.name,
                replacement.payload
            );
        }
        replacements_.push_back(std::move(replacement));
    }

    void replace(const std::string& packetName, PacketValue value) {
        ProtoDefPacketEncoder encoder(version_, variables_);
        auto payload = encoder.encodePacket(packetName, value);
        VersionedMcpeCodec codec = VersionedMcpeCodec::forVersion(version_);
        replace(codec.packetCodec().makePacketByName(packetName, payload));
    }

private:
    friend class Relay;
    std::string version_;
    ProtoDefVariableStorePtr variables_;
    bool strictDecode_ = false;
    mutable bool decoded_ = false;
    bool mutated_ = false;
    mutable PacketObject originalParams_;
    std::vector<VersionedGamePacket> replacements_;

    void ensureDecoded() const {
        if (decoded_) {
            return;
        }

        auto* self = const_cast<RelayPacketEvent*>(this);
        ProtoDefPacketDecoder decoder(version_, variables_);
        const auto fields = strictDecode_
            ? decoder.decodePacketStrict(packet.name, packet.payload)
            : decoder.decodePacket(packet.name, packet.payload);
        for (const auto& field : fields) {
            setDecodedParam(self->params, field);
        }
        self->originalParams_ = self->params;
        self->decoded_ = true;
    }

    struct PathToken {
        std::string key;
        std::vector<std::size_t> indexes;
    };

    static std::vector<PathToken> parsePath(const std::string& path) {
        std::vector<PathToken> out;
        PathToken token;

        for (std::size_t i = 0; i < path.size();) {
            const char ch = path[i];
            if (ch == '.') {
                if (!token.key.empty() || !token.indexes.empty()) {
                    out.push_back(std::move(token));
                    token = {};
                }
                ++i;
                continue;
            }

            if (ch == '[') {
                ++i;
                std::size_t index = 0;
                while (i < path.size() && path[i] >= '0' && path[i] <= '9') {
                    index = (index * 10u) + static_cast<std::size_t>(path[i] - '0');
                    ++i;
                }
                if (i < path.size() && path[i] == ']') {
                    ++i;
                }
                token.indexes.push_back(index);
                continue;
            }

            token.key.push_back(ch);
            ++i;
        }

        if (!token.key.empty() || !token.indexes.empty()) {
            out.push_back(std::move(token));
        }
        return out;
    }

    static PacketValue& asObjectSlot(PacketValue& value, const std::string& key) {
        if (value.kind != PacketValue::Kind::Object) {
            value = PacketValue::object({});
        }
        return value.objectValue[key];
    }

    static PacketValue& asArraySlot(PacketValue& value, std::size_t index) {
        if (value.kind != PacketValue::Kind::Array) {
            value = PacketValue::array({});
        }
        if (value.arrayValue.size() <= index) {
            value.arrayValue.resize(index + 1);
        }
        return value.arrayValue[index];
    }

    static void setNestedParam(PacketObject& root, const std::string& path, PacketValue value) {
        const auto tokens = parsePath(path);
        if (tokens.empty()) {
            return;
        }

        PacketValue* current = nullptr;
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            const auto& token = tokens[i];
            PacketValue* slot = nullptr;
            if (current) {
                slot = &asObjectSlot(*current, token.key);
            } else {
                slot = &root[token.key];
            }

            for (auto index : token.indexes) {
                slot = &asArraySlot(*slot, index);
            }

            if (i + 1 == tokens.size()) {
                *slot = std::move(value);
                return;
            }
            current = slot;
        }
    }

    static void setDecodedParam(PacketObject& root, const ProtoDefField& field) {
        if (field.path.empty()) {
            return;
        }

        if (field.type == "void") {
            return;
        }

        if (field.type == "switch" && field.value.rfind("<no_branch:", 0) == 0) {
            // ProtoDef exposes an inactive named switch as undefined. There is
            // no Undefined ProtoDefValue kind, so omit it rather than leaking
            // an internal diagnostic string into relay packet parameters.
            return;
        }

        const auto structuralParent = [&](const char* suffix) -> std::optional<std::string> {
            const std::string ending(suffix);
            if (field.path.size() < ending.size() ||
                field.path.compare(field.path.size() - ending.size(), ending.size(), ending) != 0) {
                return std::nullopt;
            }
            return field.path.substr(0, field.path.size() - ending.size());
        };

        if (field.type.rfind("array_count", 0) == 0) {
            if (const auto parent = structuralParent(".$count")) {
                setNestedParam(root, *parent, PacketValue::array({}));
            }
            return;
        }

        if (field.type == "option_present") {
            if (const auto parent = structuralParent(".$present")) {
                setNestedParam(
                    root,
                    *parent,
                    field.value == "true"
                        ? PacketValue::boolean(true)
                        : PacketValue::null()
                );
            }
            return;
        }

        if (field.type == "entityMetadataLoop_end") {
            if (const auto parent = structuralParent(".$end")) {
                if (!findNestedParam(static_cast<const PacketObject&>(root), *parent)) {
                    setNestedParam(root, *parent, PacketValue::array({}));
                }
            }
            return;
        }

        if (field.type == "vec3f" || field.type == "vec3i") {
            auto c1 = field.value.find(',');
            auto c2 = c1 == std::string::npos
                ? std::string::npos
                : field.value.find(',', c1 + 1);

            if (c1 != std::string::npos && c2 != std::string::npos) {
                setNestedParam(root, field.path + ".x", decodedVectorValue(field, 0, c1));
                setNestedParam(root, field.path + ".y", decodedVectorValue(field, c1 + 1, c2));
                setNestedParam(root, field.path + ".z", decodedVectorValue(field, c2 + 1, field.value.size()));
                return;
            }
        }

        setNestedParam(root, field.path, valueFromDecodedField(field));
    }

    static const PacketValue* findNestedParam(const PacketObject& root, const std::string& path) {
        const auto tokens = parsePath(path);
        if (tokens.empty()) {
            return nullptr;
        }

        const PacketValue* current = nullptr;
        for (const auto& token : tokens) {
            if (current) {
                if (current->kind != PacketValue::Kind::Object) {
                    return nullptr;
                }
                auto it = current->objectValue.find(token.key);
                if (it == current->objectValue.end()) {
                    return nullptr;
                }
                current = &it->second;
            } else {
                auto it = root.find(token.key);
                if (it == root.end()) {
                    return nullptr;
                }
                current = &it->second;
            }

            for (auto index : token.indexes) {
                if (!current || current->kind != PacketValue::Kind::Array ||
                    current->arrayValue.size() <= index) {
                    return nullptr;
                }
                current = &current->arrayValue[index];
            }
        }
        return current;
    }

    static PacketValue* findNestedParam(PacketObject& root, const std::string& path) {
        return const_cast<PacketValue*>(
            findNestedParam(static_cast<const PacketObject&>(root), path)
        );
    }

    static std::string valueToString(const PacketValue& value) {
        if (value.kind == PacketValue::Kind::String) return value.stringValue;
        if (value.kind == PacketValue::Kind::Bool) return value.boolValue ? "true" : "false";
        if (value.kind == PacketValue::Kind::Int) return std::to_string(value.intValue);
        if (value.kind == PacketValue::Kind::UInt) return std::to_string(value.uintValue);
        if (value.kind == PacketValue::Kind::Double) return std::to_string(value.doubleValue);
        return {};
    }

    static PacketValue valueFromDecodedField(const ProtoDefField& field) {
        if (field.structuredValue.has_value()) {
            return *field.structuredValue;
        }

        if (field.type == "bitflags") {
            PacketObject out;
            out["_value"] = decodedUnsignedValue(field.value);
            return PacketValue::object(std::move(out));
        }

        if (field.type.rfind("mapper<", 0) == 0) {
            auto slash = field.value.find('/');
            if (slash != std::string::npos && slash + 1 < field.value.size()) {
                return PacketValue::string(field.value.substr(slash + 1));
            }
            if (isIntegerText(field.value)) {
                return PacketValue::uinteger(static_cast<uint64_t>(std::strtoull(field.value.c_str(), nullptr, 10)));
            }
            return PacketValue::string(field.value);
        }

        if (field.type == "bool") {
            return PacketValue::boolean(field.value == "true" || field.value == "1");
        }

        if (field.type == "lf32" || field.type == "lf64" ||
            field.type == "bf32" || field.type == "bf64") {
            return PacketValue::floating(std::strtod(field.value.c_str(), nullptr));
        }

        if (isSignedType(field.type) && isIntegerText(field.value)) {
            return PacketValue::integer(static_cast<int64_t>(std::strtoll(field.value.c_str(), nullptr, 10)));
        }

        if (isUnsignedType(field.type) && isIntegerText(field.value)) {
            return PacketValue::uinteger(static_cast<uint64_t>(std::strtoull(field.value.c_str(), nullptr, 10)));
        }

        return PacketValue::string(field.value);
    }

    static PacketValue decodedVectorValue(const ProtoDefField& field, std::size_t begin, std::size_t end) {
        const auto part = field.value.substr(begin, end - begin);
        if (field.type == "vec3f") {
            return PacketValue::floating(std::strtod(part.c_str(), nullptr));
        }
        return PacketValue::integer(static_cast<int64_t>(std::strtoll(part.c_str(), nullptr, 10)));
    }

    static PacketValue decodedUnsignedValue(const std::string& text) {
        auto normalized = text;
        while (normalized.size() > 1 && normalized.front() == '0') {
            normalized.erase(normalized.begin());
        }

        static const std::string maxUInt64 = "18446744073709551615";
        const bool fitsUInt64 =
            normalized.size() < maxUInt64.size() ||
            (normalized.size() == maxUInt64.size() && normalized <= maxUInt64);

        if (fitsUInt64 && isIntegerText(normalized)) {
            return PacketValue::uinteger(static_cast<uint64_t>(
                std::strtoull(normalized.c_str(), nullptr, 10)
            ));
        }

        return PacketValue::string(text);
    }

    static bool isIntegerText(const std::string& value) {
        if (value.empty()) {
            return false;
        }
        std::size_t pos = value[0] == '-' ? 1 : 0;
        if (pos >= value.size()) {
            return false;
        }
        for (; pos < value.size(); ++pos) {
            if (value[pos] < '0' || value[pos] > '9') {
                return false;
            }
        }
        return true;
    }

    static bool isSignedType(const std::string& type) {
        return type == "i8" || type == "i16" || type == "li16" ||
            type == "i32" || type == "li32" ||
            type == "i64" || type == "li64" ||
            type == "zigzag32" || type == "zigzag64" ||
            type == "varint";
    }

    static bool isUnsignedType(const std::string& type) {
        return type == "u8" || type == "u16" || type == "lu16" ||
            type == "u32" || type == "lu32" ||
            type == "u64" || type == "lu64" ||
            type == "varuint" || type == "varuint32" ||
            type == "varuint64" || type == "varint64" ||
            type == "varint128";
    }

    static bool valueEqual(const PacketValue& a, const PacketValue& b) {
        if (a.kind != b.kind) return false;
        switch (a.kind) {
            case PacketValue::Kind::Null: return true;
            case PacketValue::Kind::Bool: return a.boolValue == b.boolValue;
            case PacketValue::Kind::Int: return a.intValue == b.intValue;
            case PacketValue::Kind::UInt: return a.uintValue == b.uintValue;
            case PacketValue::Kind::Double: return a.doubleValue == b.doubleValue;
            case PacketValue::Kind::String: return a.stringValue == b.stringValue;
            case PacketValue::Kind::Bytes: return a.bytesValue == b.bytesValue;
            case PacketValue::Kind::Array:
                if (a.arrayValue.size() != b.arrayValue.size()) return false;
                for (std::size_t i = 0; i < a.arrayValue.size(); ++i) {
                    if (!valueEqual(a.arrayValue[i], b.arrayValue[i])) return false;
                }
                return true;
            case PacketValue::Kind::Object:
                return objectEqual(a.objectValue, b.objectValue);
        }
        return false;
    }

    static bool objectEqual(const PacketObject& a, const PacketObject& b) {
        if (a.size() != b.size()) return false;
        for (const auto& [key, value] : a) {
            auto it = b.find(key);
            if (it == b.end()) return false;
            if (!valueEqual(value, it->second)) return false;
        }
        return true;
    }

    void apply(BedrockRelayPacketEvent& event) {
        if (canceled) {
            event.cancel();
            return;
        }
        if (!replacements_.empty()) {
            event.replace(std::move(replacements_));
            return;
        }
        if (!mutated_ && decoded_ && !objectEqual(params, originalParams_)) {
            mutated_ = true;
        }
        if (!mutated_) {
            return;
        }

        try {
            ProtoDefPacketEncoder encoder(version_, variables_);
            auto payload = encoder.encodePacket(name, PacketValue::object(params));
            VersionedMcpeCodec codec = VersionedMcpeCodec::forVersion(version_);
            event.replace(codec.packetCodec().makePacketByName(name, payload));
        } catch (const std::exception&) {
            // Keep the original packet flowing if user-level params cannot be
            // re-encoded yet. Relay traffic must stay byte-for-byte safe.
        }
    }
};

class RelayPlayer {
public:
    class Upstream {
    public:
        explicit Upstream(BedrockLiveRelay* relay = nullptr) : relay_(relay) {}

        void queue(const std::string& packetName, PacketValue value) {
            auto target = client();
            if (!target) return;
            target->queue(packetName, value);
        }

        void queue(const std::string& packetName, PacketObject value) {
            queue(packetName, PacketValue::object(std::move(value)));
        }

        void queue(const std::string& packetName, std::initializer_list<std::pair<const std::string, PacketValue>> value) {
            queue(packetName, PacketValue::object(PacketObject(value)));
        }

        void write(const std::string& packetName, PacketValue value) {
            auto target = client();
            if (!target) return;
            target->write(packetName, value);
        }

        void write(const std::string& packetName, PacketObject value) {
            write(packetName, PacketValue::object(std::move(value)));
        }

        void write(const std::string& packetName, std::initializer_list<std::pair<const std::string, PacketValue>> value) {
            write(packetName, PacketValue::object(PacketObject(value)));
        }

        void send(const std::string& packetName, PacketValue value) {
            write(packetName, std::move(value));
        }

        void send(const std::string& packetName, PacketObject value) {
            write(packetName, std::move(value));
        }

        void send(const std::string& packetName, std::initializer_list<std::pair<const std::string, PacketValue>> value) {
            write(packetName, value);
        }

    private:
        friend class RelayPlayer;
        BedrockLiveRelay* relay_ = nullptr;
        BedrockServerConnection connection_;

        std::shared_ptr<BedrockNetworkClient> client() const {
            return relay_ ? relay_->upstreamShared(connection_) : nullptr;
        }

        void bind(const BedrockServerConnection& connection) {
            connection_ = connection;
        }
    };

    using PacketHandler = std::function<void(RelayPacketEvent&)>;
    using PacketWithDestinationHandler = std::function<void(RelayPacketEvent&, RelayPacketDestination&)>;
    using PlayerPacketHandler = BedrockServerConnection::PacketHandler;
    using PlayerVoidHandler = BedrockServerConnection::VoidHandler;
    using LoggingInHandler = BedrockServerConnection::LoggingInHandler;
    using ClientHandshakeHandler =
        BedrockServerConnection::ClientHandshakeHandler;
    using PlayerStatusHandler = BedrockServerConnection::StatusHandler;
    using PlayerErrorHandler = BedrockServerConnection::ErrorHandler;

    BedrockServerConnection connection;
    Upstream upstream;

    RelayPlayer() = default;
    explicit RelayPlayer(BedrockLiveRelay* relay)
        : upstream(relay),
          relay_(relay) {}

    std::string sessionId() const {
        return BedrockLiveRelay::sessionId(connection);
    }

    BedrockServerClientStatus status() const {
        return connection.status();
    }

    void setStatus(BedrockServerClientStatus status) {
        connection.setStatus(status);
    }

    void updateItemPalette(const ProtoDefValue& palette) {
        connection.updateItemPalette(palette);
    }

    std::optional<ProtoDefValue> getUserData() const {
        return connection.getUserData();
    }

    std::optional<ProtoDefValue> skinData() const {
        return connection.skinData();
    }

    std::optional<BedrockLoginProfile> profile() const {
        return connection.profile();
    }

    std::optional<uint32_t> version() const {
        return connection.version();
    }

    bool versionLessThan(const std::string& version) const {
        return connection.versionLessThan(version);
    }

    bool versionLessThan(uint32_t protocolVersion) const {
        return connection.versionLessThan(protocolVersion);
    }

    bool versionGreaterThan(const std::string& version) const {
        return connection.versionGreaterThan(version);
    }

    bool versionGreaterThan(uint32_t protocolVersion) const {
        return connection.versionGreaterThan(protocolVersion);
    }

    bool versionGreaterThanOrEqualTo(const std::string& version) const {
        return connection.versionGreaterThanOrEqualTo(version);
    }

    bool versionGreaterThanOrEqualTo(uint32_t protocolVersion) const {
        return connection.versionGreaterThanOrEqualTo(protocolVersion);
    }

    bool versionLessThanOrEqualTo(const std::string& version) const {
        return connection.versionLessThanOrEqualTo(version);
    }

    bool versionLessThanOrEqualTo(uint32_t protocolVersion) const {
        return connection.versionLessThanOrEqualTo(protocolVersion);
    }

    void onLoggingIn(LoggingInHandler handler) {
        connection.onLoggingIn(std::move(handler));
    }

    void onServerClientHandshake(ClientHandshakeHandler handler) {
        connection.onServerClientHandshake(std::move(handler));
    }

    void onLogin(PlayerPacketHandler handler) {
        connection.onLogin(std::move(handler));
    }

    void onLogin(PlayerVoidHandler handler) {
        connection.onLogin(std::move(handler));
    }

    void onJoin(PlayerVoidHandler handler) {
        connection.onJoin(std::move(handler));
    }

    void onSpawn(PlayerVoidHandler handler) {
        connection.onSpawn(std::move(handler));
    }

    void onSpawn(PlayerErrorHandler handler) {
        connection.onSpawn(std::move(handler));
    }

    void onClose(PlayerVoidHandler handler) {
        connection.onClose(std::move(handler));
    }

    void onClose(PlayerErrorHandler handler) {
        connection.onClose(std::move(handler));
    }

    void onStatus(PlayerStatusHandler handler) {
        connection.onStatus(std::move(handler));
    }

    void onError(PlayerErrorHandler handler) {
        connection.onError(std::move(handler));
    }

    void onAny(PlayerPacketHandler handler) {
        connection.onAny(std::move(handler));
    }

    void on(const std::string& eventName, PlayerVoidHandler handler) {
        connection.on(eventName, std::move(handler));
    }

    void on(const std::string& eventName, PlayerPacketHandler handler) {
        connection.on(eventName, std::move(handler));
    }

    void on(const std::string& eventName, LoggingInHandler handler) {
        connection.on(eventName, std::move(handler));
    }

    void on(const std::string& eventName, ClientHandshakeHandler handler) {
        connection.on(eventName, std::move(handler));
    }

    void on(const std::string& eventName, PlayerStatusHandler handler) {
        connection.on(eventName, std::move(handler));
    }

    void on(const std::string& eventName, PlayerErrorHandler handler) {
        connection.on(eventName, std::move(handler));
    }

    void on(const std::string& direction, PacketHandler handler) {
        std::lock_guard<std::recursive_mutex> lock(handlerMutex_);
        if (direction == "clientbound") {
            clientboundHandlers_.push_back(std::move(handler));
            return;
        }
        if (direction == "serverbound") {
            serverboundHandlers_.push_back(std::move(handler));
            return;
        }
        throw std::runtime_error("unknown relay player direction: " + direction);
    }

    void on(const std::string& direction, PacketWithDestinationHandler handler) {
        std::lock_guard<std::recursive_mutex> lock(handlerMutex_);
        if (direction == "clientbound") {
            clientboundDestinationHandlers_.push_back(std::move(handler));
            return;
        }
        if (direction == "serverbound") {
            serverboundDestinationHandlers_.push_back(std::move(handler));
            return;
        }
        throw std::runtime_error("unknown relay player direction: " + direction);
    }

    void onClientbound(PacketHandler handler) {
        on("clientbound", std::move(handler));
    }

    void onClientbound(PacketWithDestinationHandler handler) {
        on("clientbound", std::move(handler));
    }

    void onServerbound(PacketHandler handler) {
        on("serverbound", std::move(handler));
    }

    void onServerbound(PacketWithDestinationHandler handler) {
        on("serverbound", std::move(handler));
    }

    void queue(const std::string& packetName, PacketValue value) {
        if (!relay_) return;
        relay_->server().queue(connection, packetName, value);
    }

    void queue(const std::string& packetName, PacketObject value) {
        queue(packetName, PacketValue::object(std::move(value)));
    }

    void queue(const std::string& packetName, std::initializer_list<std::pair<const std::string, PacketValue>> value) {
        queue(packetName, PacketValue::object(PacketObject(value)));
    }

    void write(const std::string& packetName, PacketValue value) {
        if (!relay_) return;
        relay_->server().write(connection, packetName, value);
    }

    void write(const std::string& packetName, PacketObject value) {
        write(packetName, PacketValue::object(std::move(value)));
    }

    void write(const std::string& packetName, std::initializer_list<std::pair<const std::string, PacketValue>> value) {
        write(packetName, PacketValue::object(PacketObject(value)));
    }

    void send(const std::string& packetName, PacketValue value) {
        if (!relay_) return;
        relay_->server().send(connection, packetName, value);
    }

    void send(const std::string& packetName, PacketObject value) {
        send(packetName, PacketValue::object(std::move(value)));
    }

    void send(const std::string& packetName, std::initializer_list<std::pair<const std::string, PacketValue>> value) {
        send(packetName, PacketValue::object(PacketObject(value)));
    }

    void disconnect(
        const std::string& reason = "Server closed",
        bool hide = false
    ) {
        connection.disconnect(reason, hide);
    }

    void sendDisconnectStatus(const std::string& playStatus) {
        connection.sendDisconnectStatus(playStatus);
    }

    void close() {
        connection.close();
    }

private:
    friend class Relay;
    BedrockLiveRelay* relay_ = nullptr;
    mutable std::recursive_mutex handlerMutex_;
    std::vector<PacketHandler> clientboundHandlers_;
    std::vector<PacketHandler> serverboundHandlers_;
    std::vector<PacketWithDestinationHandler> clientboundDestinationHandlers_;
    std::vector<PacketWithDestinationHandler> serverboundDestinationHandlers_;

    void setConnection(const BedrockServerConnection& value) {
        connection = value;
        upstream.bind(value);
    }

    void resetSessionHandlers() {
        std::lock_guard<std::recursive_mutex> lock(handlerMutex_);
        clientboundHandlers_.clear();
        serverboundHandlers_.clear();
        clientboundDestinationHandlers_.clear();
        serverboundDestinationHandlers_.clear();
    }

    bool hasPacketHandlers(BedrockRelayDirection direction) const {
        std::lock_guard<std::recursive_mutex> lock(handlerMutex_);
        if (direction == BedrockRelayDirection::Clientbound) {
            return !clientboundHandlers_.empty() ||
                !clientboundDestinationHandlers_.empty();
        }
        return !serverboundHandlers_.empty() ||
            !serverboundDestinationHandlers_.empty();
    }

    void dispatch(RelayPacketEvent& event) {
        std::lock_guard<std::recursive_mutex> lock(handlerMutex_);
        const bool hasDirectionHandlers =
            event.direction == BedrockRelayDirection::Clientbound
                ? !clientboundHandlers_.empty() ||
                    !clientboundDestinationHandlers_.empty()
                : !serverboundHandlers_.empty() ||
                    !serverboundDestinationHandlers_.empty();
        if (hasDirectionHandlers) {
            (void) event.decodedParams();
        }

        auto& handlers = event.direction == BedrockRelayDirection::Clientbound
            ? clientboundHandlers_
            : serverboundHandlers_;
        for (auto& handler : handlers) {
            handler(event);
        }

        auto& destinationHandlers = event.direction == BedrockRelayDirection::Clientbound
            ? clientboundDestinationHandlers_
            : serverboundDestinationHandlers_;
        RelayPacketDestination des;
        for (auto& handler : destinationHandlers) {
            handler(event, des);
            if (des.canceled) {
                event.cancel();
            }
        }
    }
};

class Relay {
public:
    using ClientMap = std::unordered_map<
        std::string,
        std::shared_ptr<RelayPlayer>
    >;
    using ConnectHandler = std::function<void(RelayPlayer&)>;
    using JoinHandler = std::function<void(
        RelayPlayer&,
        BedrockNetworkClient&
    )>;
    using DisconnectHandler = std::function<void(RelayPlayer&)>;
    using PacketHandler = std::function<void(RelayPacketEvent&)>;
    using PacketWithDestinationHandler = std::function<void(RelayPacketEvent&, RelayPacketDestination&)>;
    using ErrorHandler = std::function<void(const std::string&)>;
    using StatusHandler = std::function<void(const BedrockLiveRelayStatus&)>;
    using ParseErrorHandler = std::function<void(const RelayParseError&)>;
    using DiagnosticHandler = BedrockLiveRelay::DiagnosticHandler;

    explicit Relay(RelayOptions options)
        : options_(normalizeOptions(std::move(options))),
          live_(toLiveOptions(options_)),
          fallbackPlayer_(std::make_shared<RelayPlayer>(&live_)),
          currentPlayer_(fallbackPlayer_) {
        live_.onUpstreamJoin([this](
            const BedrockServerConnection& connection,
            const std::shared_ptr<BedrockNetworkClient>& upstream
        ) {
            const auto player = playerForSession(
                BedrockLiveRelay::sessionId(connection)
            );
            if (!player || !upstream) return;
            const auto handlers = joinHandlers_;
            for (auto& handler : handlers) {
                handler(*player, *upstream);
            }
        });
        if (options_.onMsaCode) {
            live_.onMsaCode([this](
                const XboxDeviceCodeInfo& code,
                const BedrockServerConnection& connection
            ) {
                const auto player = playerForSession(
                    BedrockLiveRelay::sessionId(connection)
                );
                if (player) {
                    options_.onMsaCode(code, *player);
                    return;
                }
                // The low-level session normally publishes RelayPlayer during
                // its connect callback before authentication can begin. Keep
                // the Node fallback if a callback races an external teardown.
                live_.server().disconnect(
                    connection,
                    "It's your first time joining. Please sign in and "
                    "reconnect to join this server:\n\n" + code.message
                );
            });
        }
    }

    ServerListenResult listen() {
        live_.onConnect([this](const BedrockServerConnection& connection) {
            auto player = std::make_shared<RelayPlayer>(&live_);
            player->setConnection(connection);
            const auto id = player->sessionId();
            {
                std::lock_guard<std::mutex> lock(playersMutex_);
                players_[id] = player;
                packetVariables_[id] = makeProtoDefVariableStore();
                currentPlayer_ = player;
            }
            for (auto& handler : connectHandlers_) {
                handler(*player);
            }
        });

        live_.onDisconnect([this](const BedrockServerConnection& connection) {
            const auto id = BedrockLiveRelay::sessionId(connection);
            std::shared_ptr<RelayPlayer> player;
            {
                std::lock_guard<std::mutex> lock(playersMutex_);
                const auto found = players_.find(id);
                if (found != players_.end()) player = found->second;
            }
            if (!player) return;
            player->setConnection(connection);
            for (auto& handler : disconnectHandlers_) {
                handler(*player);
            }
            player->resetSessionHandlers();
            {
                std::lock_guard<std::mutex> lock(playersMutex_);
                players_.erase(id);
                packetVariables_.erase(id);
                retiredPlayers_.push_back(player);
                if (currentPlayer_ == player) {
                    currentPlayer_ = players_.empty()
                        ? player
                        : players_.begin()->second;
                }
            }
        });

        live_.on("clientbound", [this](BedrockRelayPacketEvent& event) {
            handleLivePacket(event);
        });

        live_.on("serverbound", [this](BedrockRelayPacketEvent& event) {
            handleLivePacket(event);
        });

        return live_.listen();
    }

    int run() {
        listen();
        return live_.run();
    }

    void close(const std::string& reason = "closed") {
        live_.close(reason);
    }

    void on(const std::string& eventName, ConnectHandler handler) {
        if (eventName == "connect") {
            connectHandlers_.push_back(std::move(handler));
            return;
        }
        if (eventName == "disconnect") {
            disconnectHandlers_.push_back(std::move(handler));
            return;
        }
        throw std::runtime_error("unknown relay event: " + eventName);
    }

    void on(const std::string& eventName, JoinHandler handler) {
        if (eventName != "join") {
            throw std::runtime_error("unknown relay join event: " + eventName);
        }
        joinHandlers_.push_back(std::move(handler));
    }

    void on(const std::string& direction, PacketHandler handler) {
        if (direction == "clientbound") {
            clientboundHandlers_.push_back(std::move(handler));
            return;
        }
        if (direction == "serverbound") {
            serverboundHandlers_.push_back(std::move(handler));
            return;
        }
        throw std::runtime_error("unknown relay packet direction: " + direction);
    }

    void on(const std::string& direction, PacketWithDestinationHandler handler) {
        if (direction == "clientbound") {
            clientboundDestinationHandlers_.push_back(std::move(handler));
            return;
        }
        if (direction == "serverbound") {
            serverboundDestinationHandlers_.push_back(std::move(handler));
            return;
        }
        throw std::runtime_error("unknown relay packet direction: " + direction);
    }

    void onConnect(ConnectHandler handler) {
        on("connect", std::move(handler));
    }

    void onJoin(JoinHandler handler) {
        on("join", std::move(handler));
    }

    void onDisconnect(DisconnectHandler handler) {
        disconnectHandlers_.push_back(std::move(handler));
    }

    void onClientbound(PacketHandler handler) {
        on("clientbound", std::move(handler));
    }

    void onClientbound(PacketWithDestinationHandler handler) {
        on("clientbound", std::move(handler));
    }

    void onServerbound(PacketHandler handler) {
        on("serverbound", std::move(handler));
    }

    void onServerbound(PacketWithDestinationHandler handler) {
        on("serverbound", std::move(handler));
    }

    void onError(ErrorHandler handler) {
        live_.onError(std::move(handler));
    }

    void onStatus(StatusHandler handler) {
        live_.onStatus(std::move(handler));
    }

    void onDiagnostic(DiagnosticHandler handler) {
        live_.onDiagnostic(std::move(handler));
    }

    void onParseError(ParseErrorHandler handler) {
        parseErrorHandlers_.push_back(std::move(handler));
    }

    void on(const std::string& eventName, ErrorHandler handler) {
        if (eventName != "error") {
            throw std::runtime_error("unknown relay error event: " + eventName);
        }
        onError(std::move(handler));
    }

    void on(const std::string& eventName, StatusHandler handler) {
        if (eventName != "status") {
            throw std::runtime_error("unknown relay status event: " + eventName);
        }
        onStatus(std::move(handler));
    }

    BedrockLiveRelay& live() { return live_; }

    // Compatibility accessor matching the former single-player relay API.
    // While multiple downstreams are connected this is the most recently
    // accepted player; use player(connection) or players() for exact routing.
    RelayPlayer& player() {
        std::lock_guard<std::mutex> lock(playersMutex_);
        return *(currentPlayer_ ? currentPlayer_ : fallbackPlayer_);
    }

    const RelayPlayer& player() const {
        std::lock_guard<std::mutex> lock(playersMutex_);
        return *(currentPlayer_ ? currentPlayer_ : fallbackPlayer_);
    }

    RelayPlayer* player(const BedrockServerConnection& connection) {
        const auto found = playerForSession(BedrockLiveRelay::sessionId(connection));
        return found.get();
    }

    const RelayPlayer* player(const BedrockServerConnection& connection) const {
        const auto found = playerForSession(BedrockLiveRelay::sessionId(connection));
        return found.get();
    }

    std::vector<std::reference_wrapper<RelayPlayer>> players() {
        std::vector<std::reference_wrapper<RelayPlayer>> out;
        std::lock_guard<std::mutex> lock(playersMutex_);
        out.reserve(players_.size());
        for (auto& [id, value] : players_) {
            (void) id;
            out.emplace_back(*value);
        }
        return out;
    }

    std::vector<std::reference_wrapper<const RelayPlayer>> players() const {
        std::vector<std::reference_wrapper<const RelayPlayer>> out;
        std::lock_guard<std::mutex> lock(playersMutex_);
        out.reserve(players_.size());
        for (const auto& [id, value] : players_) {
            (void) id;
            out.emplace_back(*value);
        }
        return out;
    }

    std::size_t playerCount() const {
        std::lock_guard<std::mutex> lock(playersMutex_);
        return players_.size();
    }

    // Relay extends Server in JavaScript and therefore publishes clients by
    // downstream endpoint. Owning snapshots keep RelayPlayer views valid if a
    // disconnect races the caller's enumeration.
    ClientMap clients() const {
        ClientMap out;
        std::lock_guard<std::mutex> lock(playersMutex_);
        out.reserve(players_.size());
        for (const auto& [sessionId, player] : players_) {
            (void) sessionId;
            out[player->connection.key()] = player;
        }
        return out;
    }

    std::shared_ptr<RelayPlayer> client(const std::string& key) const {
        std::lock_guard<std::mutex> lock(playersMutex_);
        for (const auto& [sessionId, player] : players_) {
            (void) sessionId;
            if (player->connection.key() == key) {
                return player;
            }
        }
        return {};
    }

    const RelayOptions& options() const { return options_; }

private:
    RelayOptions options_;
    BedrockLiveRelay live_;
    mutable std::mutex playersMutex_;
    std::unordered_map<std::string, std::shared_ptr<RelayPlayer>> players_;
    std::unordered_map<std::string, ProtoDefVariableStorePtr> packetVariables_;
    // Keep disconnected player objects alive so references handed to user
    // callbacks cannot become dangling during the Relay lifetime.
    std::vector<std::shared_ptr<RelayPlayer>> retiredPlayers_;
    std::shared_ptr<RelayPlayer> fallbackPlayer_;
    std::shared_ptr<RelayPlayer> currentPlayer_;
    std::vector<ConnectHandler> connectHandlers_;
    std::vector<JoinHandler> joinHandlers_;
    std::vector<DisconnectHandler> disconnectHandlers_;
    std::vector<PacketHandler> clientboundHandlers_;
    std::vector<PacketHandler> serverboundHandlers_;
    std::vector<PacketWithDestinationHandler> clientboundDestinationHandlers_;
    std::vector<PacketWithDestinationHandler> serverboundDestinationHandlers_;
    std::vector<ParseErrorHandler> parseErrorHandlers_;

    std::shared_ptr<RelayPlayer> playerForSession(const std::string& id) const {
        std::lock_guard<std::mutex> lock(playersMutex_);
        const auto found = players_.find(id);
        return found == players_.end() ? nullptr : found->second;
    }

    ProtoDefVariableStorePtr variablesForSession(const std::string& id) {
        std::lock_guard<std::mutex> lock(playersMutex_);
        auto& variables = packetVariables_[id];
        if (!variables) variables = makeProtoDefVariableStore();
        return variables;
    }

    static RelayOptions normalizeOptions(RelayOptions options) {
        (void) validateVersion(options.version);
        return options;
    }

    RelayParseErrorPolicy effectiveParseErrorPolicy() const noexcept {
        if (options_.parseErrorPolicy.has_value()) {
            return *options_.parseErrorPolicy;
        }
        return options_.omitParseErrors
            ? RelayParseErrorPolicy::Drop
            : RelayParseErrorPolicy::Disconnect;
    }

    static bool isOpaqueResourcePackTransportPacket(
        const std::string& name
    ) noexcept {
        return name == "resource_packs_info" ||
            name == "resource_pack_stack" ||
            name == "resource_pack_data_info" ||
            name == "resource_pack_chunk_data" ||
            name == "resource_pack_client_response" ||
            name == "resource_pack_chunk_request";
    }

    void handleLivePacket(BedrockRelayPacketEvent& event) {
        const auto player = playerForSession(event.sessionId);
        auto packetVariables = variablesForSession(event.sessionId);
        std::unique_ptr<RelayPacketEvent> wrapped;
        try {
            const bool hasGlobalHandlers =
                event.direction == BedrockRelayDirection::Clientbound
                    ? !clientboundHandlers_.empty() ||
                        !clientboundDestinationHandlers_.empty()
                    : !serverboundHandlers_.empty() ||
                        !serverboundDestinationHandlers_.empty();
            const bool hasPlayerHandlers =
                player && player->hasPacketHandlers(event.direction);

            if (!hasGlobalHandlers && !hasPlayerHandlers) {
                // Resource-pack negotiation is a transport exchange between
                // the real server and Minecraft.  Do not make transparent
                // forwarding depend on this library knowing every optional
                // or newly appended resource-pack field.  The packet ID and
                // original bytes have already been framed by the MCPE codec;
                // with no structured consumer there is nothing to decode.
                if (isOpaqueResourcePackTransportPacket(event.packet.name)) {
                    return;
                }
                ProtoDefPacketDecoder decoder(
                    options_.version,
                    packetVariables
                );
                // Validation streams the small connection-scoped state needed
                // from start_game/item_registry without materializing their
                // complete field trees. Raw packet bytes remain untouched.
                decoder.validatePacketStrict(
                    event.packet.name,
                    event.packet.payload
                );
                return;
            }

            wrapped = std::make_unique<RelayPacketEvent>(
                options_.version,
                event,
                packetVariables,
                true
            );
            // Structured handlers receive fields only after a complete strict
            // decode. The low-level event still owns the untouched raw packet.
            (void) wrapped->decodedParams();
        } catch (const std::exception& error) {
            const auto policy = effectiveParseErrorPolicy();
            RelayParseError parseError {
                .direction = event.direction,
                .sessionId = event.sessionId,
                .packetName = event.packet.name,
                .message = error.what(),
                .policy = policy
            };
            std::cerr << "[relay] "
                      << (event.direction == BedrockRelayDirection::Clientbound
                              ? "clientbound"
                              : "serverbound")
                      << " parse error session=" << event.sessionId
                      << " packet=" << event.packet.name
                      << " policy=" << relayParseErrorPolicyName(policy)
                      << ": " << error.what() << "\n";
            const auto handlers = parseErrorHandlers_;
            for (auto& handler : handlers) {
                handler(parseError);
            }

            if (policy == RelayParseErrorPolicy::ForwardRaw) {
                // Do not expose partial params and do not call apply(): the
                // original packet continues through BedrockLiveRelay unchanged.
                return;
            }

            event.cancel();
            if (policy == RelayParseErrorPolicy::Disconnect && player) {
                live_.disconnectDownstream(
                    player->connection,
                    "Server packet parse error"
                );
            }
            return;
        }

        if (player) player->dispatch(*wrapped);
        auto& handlers = event.direction == BedrockRelayDirection::Clientbound
            ? clientboundHandlers_
            : serverboundHandlers_;
        for (auto& handler : handlers) {
            handler(*wrapped);
        }

        auto& destinationHandlers =
            event.direction == BedrockRelayDirection::Clientbound
                ? clientboundDestinationHandlers_
                : serverboundDestinationHandlers_;
        RelayPacketDestination destination;
        for (auto& handler : destinationHandlers) {
            handler(*wrapped, destination);
            if (destination.canceled) {
                wrapped->cancel();
            }
        }
        wrapped->apply(event);
    }

    static BedrockLiveRelayOptions toLiveOptions(const RelayOptions& options) {
        BedrockLiveRelayOptions out;
        out.server.host = options.host;
        out.server.port = options.port;
        out.server.version = options.version;
        out.server.motd = options.motd;
        out.server.maxPlayers = options.maxPlayers;
        out.server.offline = options.offline;
        out.server.raknetBackend = options.raknetBackend;
        out.server.raknetTimeoutMs = options.downstreamRaknetTimeoutMs;
        out.server.compressionAlgorithm = options.compressionAlgorithm;
        out.server.compressionLevel = options.compressionLevel;
        out.server.compressionThreshold = options.compressionThreshold;
        out.server.batchingInterval = options.batchingInterval;

        out.upstream.host = options.destination.host;
        out.upstream.port = options.destination.port;
        out.upstream.version = options.version;
        out.upstream.username = options.advanced.username;
        out.upstream.profile = options.advanced.username;
        out.upstream.offline = options.destinationOffline();
        out.upstream.interactiveAuth = true;
        out.upstream.authTitle = options.authTitle;
        out.upstream.deviceType = options.deviceType;
        out.upstream.flow = options.flow;
        out.upstream.forceRefresh = options.advanced.forceRefresh;
        out.upstream.msalConfig = options.advanced.msalConfig;
        out.upstream.authflow = options.advanced.authflow;
        out.upstream.password = options.advanced.password;
        out.upstream.httpClientFactory =
            options.advanced.httpClientFactory;
        out.upstream.profilesFolder = options.profilesFolder;
        out.upstream.raknetBackend = options.raknetBackend;
        out.upstream.raknetTimeoutMs = options.upstreamRaknetTimeoutMs;
        out.upstream.useRaknetWorkers = options.useRaknetWorker;
        out.upstream.batchingIntervalMs = options.batchingInterval;
        out.upstream.compressionLevel = options.compressionLevel;
        out.upstream.clientCacheEnabled = false;
        out.upstream.trackWorld = false;
        out.upstream.chunkRadius = 10;
        out.realms = options.destination.realms;

        out.enableChunkCaching = options.enableChunkCaching;
        out.levelChunkRetentionMaximumBytes =
            options.levelChunkRetentionMaximumBytes;
        out.logging = options.logging;
        out.itemResourceDiagnostics = options.itemResourceDiagnostics;
        out.forceSingle = options.forceSingle;
        out.replaceExisting = options.replaceExisting;
        out.useDownstreamDisplayNameForUpstreamUsername = options.offline;
        return out;
    }
};

inline Relay createRelay(RelayOptions options) {
    return Relay(std::move(options));
}

inline api::Client createExternalClient(api::ClientOptions options = {}) {
    return api::createClient(std::move(options));
}

inline std::vector<std::string> versions() {
    return ProtocolDefinition::versions();
}

inline bool supportsVersion(const std::string& version) {
    return ProtocolDefinition::supportsVersion(version);
}

inline PacketValue nil() {
    return PacketValue::null();
}

inline PacketValue boolean(bool value) {
    return PacketValue::boolean(value);
}

inline PacketValue i64(int64_t value) {
    return PacketValue::integer(value);
}

inline PacketValue i32(int32_t value) {
    return PacketValue::integer(value);
}

inline PacketValue u64(uint64_t value) {
    return PacketValue::uinteger(value);
}

inline PacketValue u32(uint32_t value) {
    return PacketValue::uinteger(value);
}

inline PacketValue f64(double value) {
    return PacketValue::floating(value);
}

inline PacketValue f32(float value) {
    return PacketValue::floating(value);
}

inline PacketValue str(std::string value) {
    return PacketValue::string(std::move(value));
}

inline PacketValue bytes(std::vector<uint8_t> value) {
    return PacketValue::bytes(std::move(value));
}

inline PacketValue object(PacketObject fields) {
    return PacketValue::object(std::move(fields));
}

inline PacketValue object(std::initializer_list<std::pair<const std::string, PacketValue>> fields) {
    return PacketValue::object(PacketObject(fields));
}

inline PacketValue array(PacketArray values) {
    return PacketValue::array(std::move(values));
}

inline PacketValue array(std::initializer_list<PacketValue> values) {
    return PacketValue::array(PacketArray(values));
}

} // namespace bedrock
