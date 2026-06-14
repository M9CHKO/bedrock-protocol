#pragma once

// Easy public API. Include this in bots:
//   #include <bedrock/bedrock.hpp>

#include <bedrock/api/Client.hpp>
#include <bedrock/client/BedrockNetworkClient.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>
#include <bedrock/relay/BedrockLiveRelay.hpp>
#include <bedrock/relay/BedrockRelay.hpp>
#include <bedrock/server/BedrockServer.hpp>
#include <bedrock/world/BedrockChunk.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bedrock {

using Packet = api::Packet;
using TextPacket = api::TextPacket;
using DebugMode = api::DebugMode;
using PacketValue = ProtoDefValue;
using PacketObject = std::unordered_map<std::string, PacketValue>;
using PacketArray = std::vector<PacketValue>;

struct Options {
    std::string host = "localhost";
    uint16_t port = 19132;
    std::string username = "Bot";
    std::string version = "latest";
    bool offline = false;
    bool interactiveAuth = true;
    std::string xboxClientId;
    std::filesystem::path authCacheRoot;

    int mtu = 1400;
    int connectTimeoutMs = 9000;
    int batchingIntervalMs = 20;
    bool autoInitPlayer = true;
    bool autoResourcePackResponses = true;
    bool clientCacheEnabled = false;
    bool trackWorld = false;
    int32_t chunkRadius = 20;
    std::vector<uint8_t> loginPacket;
    std::string clientDataJson;

    DebugMode debug = DebugMode::Off;
    bool decodePackets = true;
    bool packetDump = false;
    bool quiet = true;
};

class Client {
public:
    using PacketHandler = std::function<void(const Packet&)>;
    using TextHandler = std::function<void(const TextPacket&)>;

    explicit Client(Options options = {})
        : options_(normalizeOptions(std::move(options))),
          decoder_(options_.version),
          network_(toNetworkOptions(options_)) {}

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool connect() {
        return network_.connect();
    }

    int run() {
        return network_.run();
    }

    void close(const std::string& reason = "closed") {
        network_.close(reason);
    }

    void on(const std::string& packetName, PacketHandler handler) {
        if (packetName == "packet") {
            onAny(std::move(handler));
            return;
        }
        subscribedPackets_.insert(packetName);
        network_.on(packetName, [this, handler = std::move(handler)](const BedrockNetworkClientPacketEvent& event) {
            handler(toApiPacket(event.packet));
        });
    }

    void onAny(PacketHandler handler) {
        decodeAnyPacket_ = true;
        network_.onAny([this, handler = std::move(handler)](const BedrockNetworkClientPacketEvent& event) {
            handler(toApiPacket(event.packet));
        });
    }

    void onText(TextHandler handler) {
        subscribedPackets_.insert("text");
        network_.on("text", [this, handler = std::move(handler)](const BedrockNetworkClientPacketEvent& event) {
            auto packet = toApiPacket(event.packet);
            TextPacket text;
            text.sourceName = packet.get("source_name");
            text.message = packet.get("message");
            text.xuid = packet.get("xuid");
            text.platformChatId = packet.get("platform_chat_id");
            handler(text);
        });
    }

    void onJoin(std::function<void()> handler) {
        network_.onJoin(std::move(handler));
    }

    void onClose(std::function<void(const std::string&)> handler) {
        network_.onClose(std::move(handler));
    }

    void onError(std::function<void(const std::string&)> handler) {
        network_.onError(std::move(handler));
    }

    void send(const std::string& packetName, ProtoDefValue value) {
        network_.send(packetName, value);
    }

    void write(const std::string& packetName, ProtoDefValue value) {
        network_.write(packetName, value);
    }

    void queue(const std::string& packetName, ProtoDefValue value) {
        queuedValues_.push_back({packetName, value});
        network_.queue(packetName, value);
    }

    void sendQueued() {
        network_.sendQueued();
    }

    const auto& queuedPacketValues() const {
        return queuedValues_;
    }

    BedrockNetworkClient& network() {
        return network_;
    }

    const BedrockNetworkClient& network() const {
        return network_;
    }

    BedrockWorld& world() {
        return network_.world();
    }

    const BedrockWorld& world() const {
        return network_.world();
    }

private:
    Options options_;
    ProtoDefPacketDecoder decoder_;
    BedrockNetworkClient network_;
    std::vector<std::pair<std::string, ProtoDefValue>> queuedValues_;
    std::unordered_set<std::string> subscribedPackets_;
    bool decodeAnyPacket_ = false;

    static Options normalizeOptions(Options options) {
        if (options.version.empty() || options.version == "auto" || options.version == "latest") {
            auto vs = ProtocolDefinition::versions();
            if (!vs.empty()) options.version = vs.back();
        }
        return options;
    }

    static BedrockNetworkClientOptions toNetworkOptions(const Options& options) {
        BedrockNetworkClientOptions out;
        out.host = options.host;
        out.port = options.port;
        out.username = options.username;
        out.profile = options.username.empty()
            ? std::string("Bot")
            : options.username;
        out.version = options.version;
        out.offline = options.offline;
        out.interactiveAuth = options.interactiveAuth;
        out.xboxClientId = options.xboxClientId;
        out.authCacheRoot = options.authCacheRoot;
        out.mtu = options.mtu;
        out.connectTimeoutMs = options.connectTimeoutMs;
        out.batchingIntervalMs = options.batchingIntervalMs;
        out.autoInitPlayer = options.autoInitPlayer;
        out.autoResourcePackResponses = options.autoResourcePackResponses;
        out.clientCacheEnabled = options.clientCacheEnabled;
        out.trackWorld = options.trackWorld;
        out.chunkRadius = options.chunkRadius;
        out.loginPacket = options.loginPacket;
        out.clientDataJson = options.clientDataJson;
        return out;
    }

    Packet toApiPacket(const VersionedGamePacket& packet) const {
        Packet out;
        out.id = packet.packetId;
        out.name = packet.name;
        out.ok = true;

        if (options_.decodePackets) {
            bool shouldDecode =
                decodeAnyPacket_ ||
                subscribedPackets_.find(packet.name) != subscribedPackets_.end();

            if (!shouldDecode) {
                return out;
            }

            try {
                auto fields = decoder_.decodePacket(packet.name, packet.payload);

                for (const auto& field : fields) {
                    out.fields[field.path] = field.value;

                    auto c1 = field.value.find(',');
                    auto c2 = c1 == std::string::npos
                        ? std::string::npos
                        : field.value.find(',', c1 + 1);

                    if (c1 != std::string::npos && c2 != std::string::npos) {
                        out.fields[field.path + ".x"] = field.value.substr(0, c1);
                        out.fields[field.path + ".y"] = field.value.substr(c1 + 1, c2 - c1 - 1);
                        out.fields[field.path + ".z"] = field.value.substr(c2 + 1);
                    }

                    auto dot = field.path.rfind('.');
                    if (dot != std::string::npos) {
                        out.fields[field.path.substr(dot + 1)] = field.value;
                    }
                }
            } catch (const std::exception& e) {
                out.ok = false;
                out.fields["decode_error"] = e.what();
            }
        }

        if (options_.debug != DebugMode::Off && (!options_.quiet || options_.packetDump)) {
            std::cout << "[packet] " << out.name << " id=" << out.id << "\n";
        }
        return out;
    }
};

// Direct in-process network client, if needed later.






inline Client createClient(Options options = {}) {
    return Client(std::move(options));
}

inline Client createNetworkClient(Options options = {}) {
    return Client(std::move(options));
}

struct RelayDestination {
    std::string host = "127.0.0.1";
    uint16_t port = 19132;
    bool offline = false;
};

struct RelayOptions {
    std::string version = "latest";
    std::string host = "0.0.0.0";
    uint16_t port = 19132;
    std::string motd = "Bedrock Protocol C++ Relay";
    std::string username = "RelayBot";
    bool offline = false;
    int maxPlayers = 3;
    RelayDestination destination;
};

struct RelayPacketDestination {
    bool canceled = false;

    void cancel() {
        canceled = true;
    }
};

struct RelayVec2 {
    double x = 0.0;
    double z = 0.0;
};

struct RelayVec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct RelayMovementSync {
    uint64_t runtimeEntityId = 0;
    RelayVec3 position;
    RelayVec3 velocity;
    RelayVec2 rotation;
    uint64_t tick = 0;
    bool onGround = false;
    bool interceptServerCorrections = true;
    bool sendCorrectPlayerMovePrediction = true;
    bool sendMotionPredictionHints = true;
    bool sendEntityMotion = false;
};

class RelayPacketEvent {
public:
    BedrockRelayDirection direction = BedrockRelayDirection::Clientbound;
    std::string name;
    PacketObject params;
    VersionedGamePacket packet;
    bool canceled = false;

    RelayPacketEvent(
        std::string version,
        BedrockRelayPacketEvent& event
    ) : version_(std::move(version)),
        direction(event.direction),
        name(event.packet.name),
        packet(event.packet) {}

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
        replacements_.push_back(std::move(replacement));
    }

    void replace(const std::string& packetName, PacketValue value) {
        ProtoDefPacketEncoder encoder(version_);
        auto payload = encoder.encodePacket(packetName, value);
        VersionedMcpeCodec codec = VersionedMcpeCodec::forVersion(version_);
        replace(codec.packetCodec().makePacketByName(packetName, payload));
    }

private:
    friend class Relay;
    friend class RelayPlayer;
    std::string version_;
    mutable bool decoded_ = false;
    bool mutated_ = false;
    mutable PacketObject originalParams_;
    std::vector<VersionedGamePacket> replacements_;

    bool changed() const {
        if (canceled || !replacements_.empty()) {
            return true;
        }
        if (!decoded_) {
            return false;
        }
        return mutated_ || !objectEqual(params, originalParams_);
    }

    void ensureDecoded() const {
        if (decoded_) {
            return;
        }

        auto* self = const_cast<RelayPacketEvent*>(this);
        ProtoDefPacketDecoder decoder(version_);
        for (const auto& field : decoder.decodePacket(packet.name, packet.payload)) {
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
            type == "varint" || type == "varint64";
    }

    static bool isUnsignedType(const std::string& type) {
        return type == "u8" || type == "u16" || type == "lu16" ||
            type == "u32" || type == "lu32" ||
            type == "u64" || type == "lu64" ||
            type == "varuint" || type == "varuint32" ||
            type == "varuint64" || type == "varint128";
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
            ProtoDefPacketEncoder encoder(version_);
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
            if (!relay_ || !relay_->upstream()) return;
            relay_->upstream()->queue(packetName, value);
        }

        void queue(const std::string& packetName, PacketObject value) {
            queue(packetName, PacketValue::object(std::move(value)));
        }

        void queue(const std::string& packetName, std::initializer_list<std::pair<const std::string, PacketValue>> value) {
            queue(packetName, PacketValue::object(PacketObject(value)));
        }

        void write(const std::string& packetName, PacketValue value) {
            if (!relay_ || !relay_->upstream()) return;
            relay_->upstream()->write(packetName, value);
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
        BedrockLiveRelay* relay_ = nullptr;
    };

    using PacketHandler = std::function<void(RelayPacketEvent&)>;
    using PacketWithDestinationHandler = std::function<void(RelayPacketEvent&, RelayPacketDestination&)>;

    BedrockServerConnection connection;
    Upstream upstream;

    RelayPlayer() = default;
    explicit RelayPlayer(BedrockLiveRelay* relay)
        : upstream(relay),
          relay_(relay) {}

    void on(const std::string& direction, PacketHandler handler) {
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
        relay_->server().send(connection, packetName, value);
    }

    void queue(const std::string& packetName, PacketObject value) {
        queue(packetName, PacketValue::object(std::move(value)));
    }

    void queue(const std::string& packetName, std::initializer_list<std::pair<const std::string, PacketValue>> value) {
        queue(packetName, PacketValue::object(PacketObject(value)));
    }

    void write(const std::string& packetName, PacketValue value) {
        queue(packetName, std::move(value));
    }

    void write(const std::string& packetName, PacketObject value) {
        queue(packetName, std::move(value));
    }

    void write(const std::string& packetName, std::initializer_list<std::pair<const std::string, PacketValue>> value) {
        queue(packetName, value);
    }

    void send(const std::string& packetName, PacketValue value) {
        queue(packetName, std::move(value));
    }

    void send(const std::string& packetName, PacketObject value) {
        queue(packetName, std::move(value));
    }

    void send(const std::string& packetName, std::initializer_list<std::pair<const std::string, PacketValue>> value) {
        queue(packetName, value);
    }

    void syncMovement(RelayMovementSync sync) {
        if (sync.interceptServerCorrections) {
            movementOverride_ = sync;
        }

        if (sync.sendCorrectPlayerMovePrediction) {
            queue("correct_player_move_prediction", {
                {"prediction_type", PacketValue::string("player")},
                {"position", vec3(sync.position)},
                {"delta", vec3(sync.velocity)},
                {"rotation", vec2(sync.rotation)},
                {"angular_velocity", PacketValue::null()},
                {"on_ground", PacketValue::boolean(sync.onGround)},
                {"tick", PacketValue::uinteger(sync.tick)}
            });
        }

        if (sync.runtimeEntityId != 0 && sync.sendMotionPredictionHints) {
            queue("motion_prediction_hints", {
                {"entity_runtime_id", PacketValue::uinteger(sync.runtimeEntityId)},
                {"velocity", vec3(sync.velocity)},
                {"on_ground", PacketValue::boolean(sync.onGround)}
            });
        }

        if (sync.runtimeEntityId != 0 && sync.sendEntityMotion) {
            queue("set_entity_motion", {
                {"runtime_entity_id", PacketValue::uinteger(sync.runtimeEntityId)},
                {"velocity", vec3(sync.velocity)},
                {"tick", PacketValue::uinteger(sync.tick)}
            });
        }
    }

    void setMovementSync(RelayMovementSync sync) {
        movementOverride_ = std::move(sync);
    }

    void clearMovementSync() {
        movementOverride_.reset();
    }

private:
    friend class Relay;
    BedrockLiveRelay* relay_ = nullptr;
    std::optional<RelayMovementSync> movementOverride_;
    std::vector<PacketHandler> clientboundHandlers_;
    std::vector<PacketHandler> serverboundHandlers_;
    std::vector<PacketWithDestinationHandler> clientboundDestinationHandlers_;
    std::vector<PacketWithDestinationHandler> serverboundDestinationHandlers_;

    static PacketValue vec2(const RelayVec2& value) {
        return PacketValue::object(PacketObject{
            {"x", PacketValue::floating(value.x)},
            {"z", PacketValue::floating(value.z)}
        });
    }

    static PacketValue vec3(const RelayVec3& value) {
        return PacketValue::object(PacketObject{
            {"x", PacketValue::floating(value.x)},
            {"y", PacketValue::floating(value.y)},
            {"z", PacketValue::floating(value.z)}
        });
    }

    static bool packetTargetsRuntime(const RelayPacketEvent& event, uint64_t runtimeEntityId) {
        if (runtimeEntityId == 0) {
            return true;
        }

        for (const auto* key : {
            "runtime_id",
            "runtime_entity_id",
            "entity_runtime_id",
            "entity_id_self"
        }) {
            if (event.has(key)) {
                const auto found = event.getUInt(key);
                return found == 0 || found == runtimeEntityId;
            }
        }
        return true;
    }

    void applyMovementSync(RelayPacketEvent& event) {
        if (!movementOverride_ ||
            !movementOverride_->interceptServerCorrections ||
            event.direction != BedrockRelayDirection::Clientbound) {
            return;
        }

        const auto& sync = *movementOverride_;
        if (event.name == "correct_player_move_prediction") {
            event.set("position.x", sync.position.x);
            event.set("position.y", sync.position.y);
            event.set("position.z", sync.position.z);
            event.set("delta.x", sync.velocity.x);
            event.set("delta.y", sync.velocity.y);
            event.set("delta.z", sync.velocity.z);
            event.set("rotation.x", sync.rotation.x);
            event.set("rotation.z", sync.rotation.z);
            event.set("angular_velocity", PacketValue::null());
            event.set("on_ground", sync.onGround);
            return;
        }

        if ((event.name == "motion_prediction_hints" ||
             event.name == "set_entity_motion") &&
            packetTargetsRuntime(event, sync.runtimeEntityId)) {
            event.set("velocity.x", sync.velocity.x);
            event.set("velocity.y", sync.velocity.y);
            event.set("velocity.z", sync.velocity.z);
            if (event.name == "motion_prediction_hints") {
                event.set("on_ground", sync.onGround);
            }
            return;
        }

        if (event.name == "move_player" &&
            packetTargetsRuntime(event, sync.runtimeEntityId)) {
            event.set("position.x", sync.position.x);
            event.set("position.y", sync.position.y);
            event.set("position.z", sync.position.z);
            event.set("pitch", sync.rotation.x);
            event.set("yaw", sync.rotation.z);
            event.set("head_yaw", sync.rotation.z);
            event.set("mode", "normal");
            event.set("on_ground", sync.onGround);
        }
    }

    bool movementSyncFromMutation(const RelayPacketEvent& event, RelayMovementSync& sync) const {
        if (!event.changed() || event.canceled ||
            event.direction != BedrockRelayDirection::Serverbound) {
            return false;
        }

        if (event.name != "player_auth_input" && event.name != "move_player") {
            return false;
        }

        if (!event.has("position.x") ||
            !event.has("position.y") ||
            !event.has("position.z")) {
            return false;
        }

        sync.runtimeEntityId = 0;
        for (const auto* key : {
            "runtime_id",
            "runtime_entity_id",
            "entity_runtime_id",
            "entity_id_self"
        }) {
            if (event.has(key)) {
                sync.runtimeEntityId = event.getUInt(key);
                break;
            }
        }

        sync.position = {
            event.getDouble("position.x"),
            event.getDouble("position.y"),
            event.getDouble("position.z")
        };

        sync.velocity = {
            event.getDouble("delta.x", 0.0),
            event.getDouble("delta.y", 0.0),
            event.getDouble("delta.z", 0.0)
        };

        sync.rotation = {
            event.getDouble("pitch", 0.0),
            event.getDouble("yaw", 0.0)
        };
        sync.tick = event.getUInt("tick", 0);
        sync.onGround = event.getBool("on_ground", false);
        sync.interceptServerCorrections = false;
        sync.sendCorrectPlayerMovePrediction = true;
        sync.sendMotionPredictionHints = true;
        sync.sendEntityMotion = false;
        return true;
    }

    void syncChangedMovementToClient(const RelayPacketEvent& event) {
        RelayMovementSync sync;
        if (movementSyncFromMutation(event, sync)) {
            syncMovement(sync);
        }
    }

    void dispatch(RelayPacketEvent& event) {
        applyMovementSync(event);

        if (!clientboundHandlers_.empty() ||
            !serverboundHandlers_.empty() ||
            !clientboundDestinationHandlers_.empty() ||
            !serverboundDestinationHandlers_.empty()) {
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
    using ConnectHandler = std::function<void(RelayPlayer&)>;
    using DisconnectHandler = std::function<void(RelayPlayer&)>;
    using PacketHandler = std::function<void(RelayPacketEvent&)>;
    using PacketWithDestinationHandler = std::function<void(RelayPacketEvent&, RelayPacketDestination&)>;
    using ErrorHandler = std::function<void(const std::string&)>;
    using StatusHandler = std::function<void(const BedrockLiveRelayStatus&)>;

    explicit Relay(RelayOptions options)
        : options_(normalizeOptions(std::move(options))),
          live_(toLiveOptions(options_)),
          player_(&live_) {}

    void listen() {
        live_.onConnect([this](const BedrockServerConnection& connection) {
            player_.connection = connection;
            for (auto& handler : connectHandlers_) {
                handler(player_);
            }
        });

        live_.onDisconnect([this](const BedrockServerConnection& connection) {
            player_.connection = connection;
            for (auto& handler : disconnectHandlers_) {
                handler(player_);
            }
        });

        live_.on("clientbound", [this](BedrockRelayPacketEvent& event) {
            RelayPacketEvent wrapped(options_.version, event);
            player_.dispatch(wrapped);
            if (!clientboundHandlers_.empty() || !clientboundDestinationHandlers_.empty()) {
                (void) wrapped.decodedParams();
            }
            for (auto& handler : clientboundHandlers_) {
                handler(wrapped);
            }
            RelayPacketDestination des;
            for (auto& handler : clientboundDestinationHandlers_) {
                handler(wrapped, des);
                if (des.canceled) {
                    wrapped.cancel();
                }
            }
            wrapped.apply(event);
        });

        live_.on("serverbound", [this](BedrockRelayPacketEvent& event) {
            RelayPacketEvent wrapped(options_.version, event);
            player_.dispatch(wrapped);
            if (!serverboundHandlers_.empty() || !serverboundDestinationHandlers_.empty()) {
                (void) wrapped.decodedParams();
            }
            for (auto& handler : serverboundHandlers_) {
                handler(wrapped);
            }
            RelayPacketDestination des;
            for (auto& handler : serverboundDestinationHandlers_) {
                handler(wrapped, des);
                if (des.canceled) {
                    wrapped.cancel();
                }
            }
            player_.syncChangedMovementToClient(wrapped);
            wrapped.apply(event);
        });

        live_.listen();
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
    RelayPlayer& player() { return player_; }

private:
    RelayOptions options_;
    BedrockLiveRelay live_;
    RelayPlayer player_;
    std::vector<ConnectHandler> connectHandlers_;
    std::vector<DisconnectHandler> disconnectHandlers_;
    std::vector<PacketHandler> clientboundHandlers_;
    std::vector<PacketHandler> serverboundHandlers_;
    std::vector<PacketWithDestinationHandler> clientboundDestinationHandlers_;
    std::vector<PacketWithDestinationHandler> serverboundDestinationHandlers_;

    static RelayOptions normalizeOptions(RelayOptions options) {
        if (options.version.empty() || options.version == "auto" || options.version == "latest") {
            auto vs = ProtocolDefinition::versions();
        if (!vs.empty()) options.version = vs.back();
        }
        return options;
    }

    static BedrockLiveRelayOptions toLiveOptions(const RelayOptions& options) {
        BedrockLiveRelayOptions out;
        out.server.host = options.host;
        out.server.port = options.port;
        out.server.version = options.version;
        out.server.motd = options.motd;
        out.server.maxPlayers = options.maxPlayers;
        out.server.compressionThreshold = 256;
        out.server.compressionAlgorithm = "deflate";

        out.upstream.host = options.destination.host;
        out.upstream.port = options.destination.port;
        out.upstream.version = options.version;
        out.upstream.username = options.username;
        out.upstream.profile = options.username;
        out.upstream.offline = options.destination.offline || options.offline;
        out.upstream.interactiveAuth = true;
        out.upstream.clientCacheEnabled = false;
        out.upstream.trackWorld = false;
        out.upstream.chunkRadius = 20;

        out.enableChunkCaching = false;
        out.logging = false;
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
