#include <bedrock/BedrockFramer.hpp>
#include <bedrock/bedrock.hpp>
#include <bedrock/auth/FileAuthCache.hpp>
#include <bedrock/auth/MsalCachePlugin.hpp>
#include <bedrock/auth/MsalError.hpp>
#include <bedrock/auth/MsalRequestParameterBuilder.hpp>
#include <bedrock/auth/MsalSerializableTokenCache.hpp>
#include <bedrock/auth/MsaTokenManager.hpp>
#include <bedrock/auth/UuidV3.hpp>
#include <bedrock/debug/PacketFieldDecoder.hpp>
#include <bedrock/relay/BedrockRelay.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/protocol/VersionedPacketCodec.hpp>
#include <bedrock/protodef/ProtoDefEncoder.hpp>
#include <bedrock/protodef/ProtoDefContext.hpp>
#include <bedrock/protodef/ProtoDefDecoder.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefReader.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>
#include <bedrock/protodef/ProtoDefWriter.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace bedrock {

struct BedrockNetworkClientTestAccess {
    static bool queueRunning(BedrockNetworkClient& client) {
        std::lock_guard<std::mutex> lock(client.queueMutex_);
        return !client.stopQueue_;
    }

    static bool hasTransport(BedrockNetworkClient& client) {
        std::lock_guard<std::mutex> lock(client.sendMutex_);
        return static_cast<bool>(client.raknet_);
    }

    static std::filesystem::path effectiveAuthenticationCacheRoot(
        BedrockNetworkClient& client
    ) {
        std::lock_guard<std::mutex> lock(client.optionsMutex_);
        return client.authenticationCacheRoot_;
    }

    static MsalConfigPtr effectiveMsalConfig(BedrockNetworkClient& client) {
        std::lock_guard<std::mutex> lock(client.optionsMutex_);
        return client.authenticationMsalConfig_;
    }

    static AuthCachePtr authenticationCache(
        BedrockNetworkClient& client,
        const std::string& name
    ) {
        std::lock_guard<std::mutex> lock(client.optionsMutex_);
        const auto it = client.authenticationCaches_.find(name);
        return it == client.authenticationCaches_.end()
            ? AuthCachePtr{}
            : it->second;
    }

    static bool hasAuthenticationXboxProofKey(
        BedrockNetworkClient& client
    ) {
        std::lock_guard<std::mutex> lock(client.optionsMutex_);
        return client.authenticationXboxProofKey_.has_value();
    }

    static bool connectLifecycleIdle(BedrockNetworkClient& client) {
        std::lock_guard<std::mutex> lock(client.connectLifecycleMutex_);
        return client.connectLifecyclePhase_ ==
            BedrockNetworkClient::ConnectLifecyclePhase::Idle;
    }

    static void setBeforeQueueStartHook(
        BedrockNetworkClient& client,
        std::function<void()> hook
    ) {
        std::lock_guard<std::mutex> lock(client.eventHandlersMutex_);
        client.beforeQueueStartTestHook_ = std::move(hook);
    }
};

struct ClientFactoryTestAccess {
    static void setAutoConnectStarted(Client& client) {
        std::lock_guard<std::mutex> lock(client.state_->mutex);
        client.state_->autoConnectStarted = true;
    }
};

} // namespace bedrock

namespace {

static_assert(std::is_same_v<bedrock::Server, bedrock::BedrockServer>);

std::vector<int> parseVersion(const std::string& version) {
    std::vector<int> out;
    std::string cur;

    for (char c : version) {
        if (c == '.') {
            out.push_back(cur.empty() ? 0 : std::stoi(cur));
            cur.clear();
        } else if (c >= '0' && c <= '9') {
            cur.push_back(c);
        }
    }

    out.push_back(cur.empty() ? 0 : std::stoi(cur));
    while (out.size() < 3) out.push_back(0);
    return out;
}

bool versionAtLeast(const std::string& version, const std::string& minimum) {
    return parseVersion(version) >= parseVersion(minimum);
}

bool versionEquals(const std::string& version, const std::string& other) {
    return parseVersion(version) == parseVersion(other);
}

bool sameBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool checkPacketRoundtrip(const std::string& version, const std::string& packetName) {
    auto codec = bedrock::VersionedPacketCodec::forVersion(version);

    uint32_t id = 0;
    try {
        id = codec.definition().packetId(packetName);
    } catch (const std::exception&) {
        return true;
    }

    const std::vector<uint8_t> payload = {
        static_cast<uint8_t>(id & 0xffu),
        static_cast<uint8_t>((id >> 8u) & 0xffu),
        0x42,
        0x7f
    };

    auto encoded = codec.encodeFullPacketByName(packetName, payload);
    auto decoded = codec.decodeFullPacket(encoded);

    if (decoded.packetId != id || decoded.name != packetName || !sameBytes(decoded.payload, payload)) {
        std::cerr << "[FAIL] " << version << " packet " << packetName << " id/name/payload mismatch\n";
        return false;
    }

    auto encodedById = codec.encodeFullPacketById(decoded.packetId, decoded.payload);
    if (!sameBytes(encoded, encodedById)) {
        std::cerr << "[FAIL] " << version << " packet " << packetName << " encode-by-id mismatch\n";
        return false;
    }

    return true;
}

bool checkBatchRoundtrip(const std::string& version, bool compressionReady) {
    auto codec = bedrock::VersionedPacketCodec::forVersion(version);

    if (
        !codec.definition().hasPacket("play_status") ||
        !codec.definition().hasPacket("resource_pack_client_response") ||
        !codec.definition().hasPacket("request_chunk_radius")
    ) {
        return true;
    }

    std::vector<std::vector<uint8_t>> packets;
    packets.push_back(codec.encodeFullPacketByName("play_status", { 0x00 }));
    packets.push_back(codec.encodeFullPacketByName("resource_pack_client_response", { 0x03, 0x00 }));
    packets.push_back(codec.encodeFullPacketByName("request_chunk_radius", { 0x10 }));

    bedrock::BedrockFramerSettings settings;
    settings.compressionReady = compressionReady;
    settings.compressorInHeader = versionAtLeast(version, "1.20.61");
    settings.compressionThreshold = 0;
    settings.compressionAlgorithm = 0;

    auto encoded = bedrock::BedrockFramer::encodeBatch(packets, settings);
    auto decoded = bedrock::BedrockFramer::decodeBatch(encoded, settings);

    if (decoded.size() != packets.size()) {
        std::cerr << "[FAIL] " << version << " batch size mismatch\n";
        return false;
    }

    for (std::size_t i = 0; i < packets.size(); ++i) {
        if (!sameBytes(decoded[i], packets[i])) {
            std::cerr << "[FAIL] " << version << " batch packet " << i << " mismatch\n";
            return false;
        }
    }

    return true;
}

bedrock::ProtoDefValue object(std::unordered_map<std::string, bedrock::ProtoDefValue> fields) {
    return bedrock::ProtoDefValue::object(std::move(fields));
}

bedrock::ProtoDefValue array(std::vector<bedrock::ProtoDefValue> values = {}) {
    return bedrock::ProtoDefValue::array(std::move(values));
}

bedrock::ProtoDefValue vec2f(double x, double z) {
    return object({
        {"x", bedrock::ProtoDefValue::floating(x)},
        {"z", bedrock::ProtoDefValue::floating(z)}
    });
}

bedrock::ProtoDefValue vec3f(double x, double y, double z) {
    return object({
        {"x", bedrock::ProtoDefValue::floating(x)},
        {"y", bedrock::ProtoDefValue::floating(y)},
        {"z", bedrock::ProtoDefValue::floating(z)}
    });
}

bedrock::ProtoDefValue inputFlags(std::initializer_list<std::string> enabled = {}) {
    auto flags = object({
        {"item_interact", bedrock::ProtoDefValue::boolean(false)},
        {"block_action", bedrock::ProtoDefValue::boolean(false)},
        {"item_stack_request", bedrock::ProtoDefValue::boolean(false)},
        {"client_predicted_vehicle", bedrock::ProtoDefValue::boolean(false)}
    });

    for (const auto& name : enabled) {
        flags.objectValue[name] = bedrock::ProtoDefValue::boolean(true);
    }

    return flags;
}

bool checkProtoDefNativeHelpers() {
    bool ok = true;
    bedrock::ProtoDefEncoder encoder;

    auto expectBytes = [&](const std::string& label, const std::string& typeJson, const bedrock::ProtoDefValue& value, std::vector<uint8_t> expected) {
        try {
            bedrock::ProtoDefWriter writer;
            encoder.encode(typeJson, value, writer);
            auto actual = writer.take();
            if (!sameBytes(actual, expected)) {
                std::cerr << "[FAIL] protodef helper " << label << " byte mismatch\n";
                ok = false;
            }
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] protodef helper " << label << ": " << e.what() << "\n";
            ok = false;
        }
    };

    auto expectDecodedField = [&](const std::string& label, const std::vector<bedrock::ProtoDefField>& fields, const std::string& path, const std::string& value) {
        const auto it = std::find_if(fields.begin(), fields.end(), [&](const bedrock::ProtoDefField& field) {
            return field.path == path;
        });
        if (it == fields.end() || it->value != value) {
            std::cerr << "[FAIL] protodef helper " << label << " missing " << path << "=" << value << "\n";
            ok = false;
        }
    };

    expectBytes(
        "fixed-buffer",
        "[\"buffer\",{\"count\":4}]",
        bedrock::ProtoDefValue::bytes({1, 2, 3, 4}),
        {1, 2, 3, 4}
    );

    expectBytes(
        "u16-big-endian",
        "\"u16\"",
        bedrock::ProtoDefValue::uinteger(0x1234),
        {0x12, 0x34}
    );

    expectBytes(
        "lu16-little-endian",
        "\"lu16\"",
        bedrock::ProtoDefValue::uinteger(0x1234),
        {0x34, 0x12}
    );

    expectBytes(
        "f32-big-endian",
        "\"f32\"",
        bedrock::ProtoDefValue::floating(1.0),
        {0x3f, 0x80, 0x00, 0x00}
    );

    expectBytes(
        "lf32-little-endian",
        "\"lf32\"",
        bedrock::ProtoDefValue::floating(1.0),
        {0x00, 0x00, 0x80, 0x3f}
    );

    expectBytes(
        "bitfield",
        "[\"bitfield\",[{\"name\":\"type\",\"size\":3,\"signed\":false},{\"name\":\"key\",\"size\":5,\"signed\":false}]]",
        object({
            {"type", bedrock::ProtoDefValue::uinteger(5)},
            {"key", bedrock::ProtoDefValue::uinteger(17)}
        }),
        {0xb1}
    );

    expectBytes(
        "ipAddress",
        "\"ipAddress\"",
        bedrock::ProtoDefValue::string("127.0.0.1"),
        {127, 0, 0, 1}
    );

    expectBytes(
        "endOfArray",
        "[\"endOfArray\",{\"type\":\"u8\"}]",
        array({bedrock::ProtoDefValue::uinteger(7), bedrock::ProtoDefValue::uinteger(8)}),
        {7, 8}
    );

    expectBytes(
        "entityMetadataLoop",
        "[\"entityMetadataLoop\",{\"type\":\"u8\",\"endVal\":127}]",
        array({bedrock::ProtoDefValue::uinteger(1), bedrock::ProtoDefValue::uinteger(2)}),
        {1, 2, 127}
    );

    expectBytes(
        "varint128-bitflags",
        "[\"bitflags\",{\"type\":\"varint128\",\"flags\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\",\"j\",\"k\",\"l\",\"m\",\"n\",\"o\",\"p\",\"q\",\"r\",\"s\",\"t\",\"u\",\"v\",\"w\",\"x\",\"y\",\"z\",\"aa\",\"ab\",\"ac\",\"ad\",\"ae\",\"af\",\"ag\",\"ah\",\"ai\",\"aj\",\"ak\",\"al\",\"am\",\"an\",\"ao\",\"ap\",\"aq\",\"ar\",\"as\",\"at\",\"au\",\"av\",\"aw\",\"ax\",\"ay\",\"az\",\"ba\",\"bb\",\"bc\",\"bd\",\"be\",\"bf\",\"bg\",\"bh\",\"bi\",\"bj\",\"bk\",\"bl\",\"bm\"]}]",
        array({bedrock::ProtoDefValue::string("bl")}),
        {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01}
    );

    try {
        bedrock::ProtoDefDecoder decoder;
        const std::vector<uint8_t> encodedFlags {0x05};
        bedrock::PacketFieldCursor cursor(encodedFlags);
        bedrock::ProtoDefReader reader(cursor);
        bedrock::ProtoDefContext context;
        std::vector<bedrock::ProtoDefField> fields;
        decoder.decode(
            "[\"bitflags\",{\"type\":\"varint\",\"flags\":[\"a\",\"b\",\"c\"]}]",
            reader,
            "flags",
            fields,
            context
        );
        expectDecodedField("bitflags-decode", fields, "flags", "5");
        expectDecodedField("bitflags-decode", fields, "flags.a", "true");
        expectDecodedField("bitflags-decode", fields, "flags.b", "false");
        expectDecodedField("bitflags-decode", fields, "flags.c", "true");
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] protodef helper bitflags-decode: " << e.what() << "\n";
        ok = false;
    }

    return ok;
}

bool checkSchemaEncode(
    const std::string& version,
    const std::string& packetName,
    const bedrock::ProtoDefValue& value
) {
    auto codec = bedrock::VersionedPacketCodec::forVersion(version);
    if (!codec.definition().hasPacket(packetName)) {
        return true;
    }

    try {
        bedrock::ProtoDefPacketEncoder encoder(version);
        auto payload = encoder.encodePacket(packetName, value);
        auto full = codec.encodeFullPacketByName(packetName, payload);
        auto decoded = codec.decodeFullPacket(full);

        if (decoded.name != packetName || decoded.payload != payload) {
            std::cerr << "[FAIL] " << version << " schema encode " << packetName << " mismatch\n";
            return false;
        }

        bedrock::ProtoDefPacketDecoder decoder(version);
        (void) decoder.decodePacket(packetName, payload);
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << version << " schema encode " << packetName << ": " << e.what() << "\n";
        return false;
    }

    return true;
}

bool checkSchemaEncodes(const std::string& version) {
    bool ok = true;

    if (versionAtLeast(version, "1.16.0")) {
        ok = checkSchemaEncode(version, "request_chunk_radius", object({
            {"chunk_radius", bedrock::ProtoDefValue::integer(20)},
            {"max_radius", bedrock::ProtoDefValue::uinteger(0)}
        })) && ok;
    } else {
        ok = checkSchemaEncode(version, "request_chunk_radius", object({
            {"chunkRadius", bedrock::ProtoDefValue::integer(20)}
        })) && ok;
    }

    ok = checkSchemaEncode(version, "client_cache_status", object({
        {"enabled", bedrock::ProtoDefValue::boolean(false)}
    })) && ok;

    ok = checkSchemaEncode(version, "set_local_player_as_initialized", object({
        {"runtime_entity_id", bedrock::ProtoDefValue::uinteger(UINT64_MAX)}
    })) && ok;

    ok = checkSchemaEncode(version, "resource_pack_client_response", object({
        {"response_status", bedrock::ProtoDefValue::string("completed")},
        {"resourcepackids", array()}
    })) && ok;

    ok = checkSchemaEncode(version, "network_settings", object({
        {"compression_threshold", bedrock::ProtoDefValue::uinteger(256)},
        {"compression_algorithm", bedrock::ProtoDefValue::string("deflate")},
        {"client_throttle", bedrock::ProtoDefValue::boolean(false)},
        {"client_throttle_threshold", bedrock::ProtoDefValue::uinteger(0)},
        {"client_throttle_scalar", bedrock::ProtoDefValue::floating(0.0)}
    })) && ok;

    auto textPacket = versionAtLeast(version, "1.16.0")
        ? object({
            {"type", bedrock::ProtoDefValue::string("raw")},
            {"needs_translation", bedrock::ProtoDefValue::boolean(false)},
            {"message", bedrock::ProtoDefValue::string("hello from schema encoder")},
            {"xuid", bedrock::ProtoDefValue::string("")},
            {"platform_chat_id", bedrock::ProtoDefValue::string("")},
            {"filtered_message", bedrock::ProtoDefValue::string("")}
        })
        : object({
            {"type", bedrock::ProtoDefValue::integer(0)},
            {"message", bedrock::ProtoDefValue::string("hello from schema encoder")}
        });
    if (versionAtLeast(version, "1.21.130")) {
        textPacket.objectValue["category"] = bedrock::ProtoDefValue::string("message_only");
        textPacket.objectValue["raw"] = bedrock::ProtoDefValue::string("hello from schema encoder");
        textPacket.objectValue["tip"] = bedrock::ProtoDefValue::string("");
        textPacket.objectValue["system_message"] = bedrock::ProtoDefValue::string("");
        textPacket.objectValue["text_object_whisper"] = bedrock::ProtoDefValue::string("");
        textPacket.objectValue["text_object_announcement"] = bedrock::ProtoDefValue::string("");
        textPacket.objectValue["text_object"] = bedrock::ProtoDefValue::string("");
        textPacket.objectValue["has_filtered_message"] = bedrock::ProtoDefValue::boolean(false);
    }
    ok = checkSchemaEncode(version, "text", textPacket) && ok;

    auto movePlayer = versionAtLeast(version, "1.16.0")
        ? object({
            {"runtime_id", bedrock::ProtoDefValue::uinteger(1)},
            {"position", object({
                {"x", bedrock::ProtoDefValue::floating(0.0)},
                {"y", bedrock::ProtoDefValue::floating(64.0)},
                {"z", bedrock::ProtoDefValue::floating(0.0)}
            })},
            {"pitch", bedrock::ProtoDefValue::floating(0.0)},
            {"yaw", bedrock::ProtoDefValue::floating(0.0)},
            {"head_yaw", bedrock::ProtoDefValue::floating(0.0)},
            {"mode", bedrock::ProtoDefValue::string("normal")},
            {"on_ground", bedrock::ProtoDefValue::boolean(true)},
            {"ridden_runtime_id", bedrock::ProtoDefValue::uinteger(0)},
            {"tick", bedrock::ProtoDefValue::uinteger(1)}
        })
        : object({
            {"entityId", bedrock::ProtoDefValue::integer(1)},
            {"x", bedrock::ProtoDefValue::floating(0.0)},
            {"y", bedrock::ProtoDefValue::floating(64.0)},
            {"z", bedrock::ProtoDefValue::floating(0.0)},
            {"yaw", bedrock::ProtoDefValue::floating(0.0)},
            {"headYaw", bedrock::ProtoDefValue::floating(0.0)},
            {"pitch", bedrock::ProtoDefValue::floating(0.0)},
            {"mode", bedrock::ProtoDefValue::integer(0)},
            {"onGround", bedrock::ProtoDefValue::integer(1)}
        });
    if (versionEquals(version, "1.26.10")) {
        movePlayer.objectValue["mode"] = bedrock::ProtoDefValue::uinteger(0);
    }
    ok = checkSchemaEncode(version, "move_player", movePlayer) && ok;

    ok = checkSchemaEncode(version, "player_auth_input", object({
        {"pitch", bedrock::ProtoDefValue::floating(0.0)},
        {"yaw", bedrock::ProtoDefValue::floating(0.0)},
        {"position", vec3f(0.0, 64.0, 0.0)},
        {"move_vector", vec2f(0.0, 0.0)},
        {"head_yaw", bedrock::ProtoDefValue::floating(0.0)},
        {"input_data", inputFlags()},
        {"input_mode", bedrock::ProtoDefValue::string("mouse")},
        {"play_mode", bedrock::ProtoDefValue::string("normal")},
        {"interaction_model", bedrock::ProtoDefValue::string("classic")},
        {"interact_rotation", vec2f(0.0, 0.0)},
        {"tick", bedrock::ProtoDefValue::uinteger(1)},
        {"delta", vec3f(0.0, 0.0, 0.0)},
        {"analogue_move_vector", vec2f(0.0, 0.0)},
        {"camera_orientation", vec3f(0.0, 0.0, 0.0)},
        {"raw_move_vector", vec2f(0.0, 0.0)}
    })) && ok;

    if (versionAtLeast(version, "1.16.0")) {
        ok = checkSchemaEncode(version, "move_entity", object({
            {"runtime_entity_id", bedrock::ProtoDefValue::uinteger(1)},
            {"flags", bedrock::ProtoDefValue::uinteger(0)},
            {"position", vec3f(0.0, 64.0, 0.0)},
            {"rotation", object({
                {"yaw", bedrock::ProtoDefValue::floating(90.0)},
                {"pitch", bedrock::ProtoDefValue::floating(0.0)},
                {"head_yaw", bedrock::ProtoDefValue::floating(90.0)}
            })}
        })) && ok;
    } else {
        ok = checkSchemaEncode(version, "move_entity", object({
            {"entities", array({
                object({
                    {"eid", bedrock::ProtoDefValue::integer(1)},
                    {"x", bedrock::ProtoDefValue::floating(0.0)},
                    {"y", bedrock::ProtoDefValue::floating(64.0)},
                    {"z", bedrock::ProtoDefValue::floating(0.0)},
                    {"yaw", bedrock::ProtoDefValue::floating(90.0)},
                    {"headYaw", bedrock::ProtoDefValue::floating(90.0)},
                    {"pitch", bedrock::ProtoDefValue::floating(0.0)}
                })
            })}
        })) && ok;
    }

    if (!versionAtLeast(version, "1.16.0")) {
        auto setEntityData = object({
            {versionEquals(version, "0.14") ? "entityId" : "entity_id", bedrock::ProtoDefValue::integer(1)},
            {"metadata", array({
                object({
                    {"type", bedrock::ProtoDefValue::integer(4)},
                    {"key", bedrock::ProtoDefValue::integer(3)},
                    {"value", bedrock::ProtoDefValue::string("meta")}
                })
            })}
        });
        ok = checkSchemaEncode(version, "set_entity_data", setEntityData) && ok;
    }

    return ok;
}

bool checkRelayPipeline(const std::string& version) {
    bool ok = true;
    auto mcpeCodec = bedrock::VersionedMcpeCodec::forVersion(version);
    const auto& definition = mcpeCodec.definition();
    const auto& packetCodec = mcpeCodec.packetCodec();

    auto makeRelay = [&]() {
        bedrock::BedrockRelayOptions options;
        options.clientOptions.minecraftVersion = version;
        options.clientOptions.outgoingCompression = bedrock::VersionedMcpeCompression::Uncompressed;
        options.clientOptions.autoResourcePackResponses = false;
        options.clientOptions.autoStartGameInit = false;
        return bedrock::BedrockRelay(std::move(options));
    };

    if (definition.hasPacket("play_status")) {
        auto relay = makeRelay();
        relay.markDownstreamJoined();
        bool sawPlayStatus = false;
        relay.onClientbound([&](bedrock::BedrockRelayPacketEvent& event) {
            if (event.packet.name == "play_status") {
                sawPlayStatus = true;
                event.cancel();
            }
        });

        auto packet = packetCodec.makePacketByName("play_status", {0x01});
        auto mcpe = mcpeCodec.encodeMcpePayload({packet}, bedrock::VersionedMcpeCompression::Uncompressed);
        auto frame = relay.handleClientboundMcpe(mcpe);

        if (!sawPlayStatus || frame.forwardedPackets.size() != 0 || !frame.forwardedMcpe.empty()) {
            std::cerr << "[FAIL] " << version << " relay clientbound cancel mismatch\n";
            ok = false;
        }
    }

    if (definition.hasPacket("client_cache_status")) {
        auto relay = makeRelay();
        relay.markDownstreamJoined();
        relay.markUpstreamJoined();
        auto packet = packetCodec.makePacketByName("client_cache_status", {0x01});
        auto mcpe = mcpeCodec.encodeMcpePayload({packet}, bedrock::VersionedMcpeCompression::Uncompressed);
        auto frame = relay.handleServerboundMcpe(mcpe);

        if (
            frame.forwardedPackets.size() != 1 ||
            frame.forwardedPackets[0].name != "client_cache_status" ||
            frame.forwardedPackets[0].payload != std::vector<uint8_t>{0x00}
        ) {
            std::cerr << "[FAIL] " << version << " relay client_cache_status force mismatch\n";
            ok = false;
        } else {
            auto decoded = mcpeCodec.decodeMcpePayload(frame.forwardedMcpe);
            if (
                decoded.batch.packets.size() != 1 ||
                decoded.batch.packets[0].name != "client_cache_status" ||
                decoded.batch.packets[0].payload != std::vector<uint8_t>{0x00}
            ) {
                std::cerr << "[FAIL] " << version << " relay repacked mcpe mismatch\n";
                ok = false;
            }
        }

        auto replacingRelay = makeRelay();
        replacingRelay.markDownstreamJoined();
        replacingRelay.markUpstreamJoined();
        replacingRelay.onServerbound([&](bedrock::BedrockRelayPacketEvent& event) {
            if (event.packet.name == "client_cache_status") {
                event.replace(packetCodec.makePacketByName("client_cache_status", {0x01}));
            }
        });
        auto replaced = replacingRelay.handleServerboundMcpe(mcpe);
        if (
            replaced.forwardedPackets.size() != 1 ||
            replaced.forwardedPackets[0].payload != std::vector<uint8_t>{0x01}
        ) {
            std::cerr << "[FAIL] " << version << " relay serverbound replace mismatch\n";
            ok = false;
        }
    }

    if (definition.hasPacket("level_chunk") && definition.hasPacket("start_game")) {
        auto relay = makeRelay();
        relay.markDownstreamJoined();
        auto chunk = packetCodec.makePacketByName("level_chunk", {0x11, 0x22, 0x33});
        auto start = packetCodec.makePacketByName("start_game", {0x44, 0x55});

        auto chunkMcpe = mcpeCodec.encodeMcpePayload({chunk}, bedrock::VersionedMcpeCompression::Uncompressed);
        auto chunkFrame = relay.handleClientboundMcpe(chunkMcpe);
        if (!chunkFrame.forwardedPackets.empty() || relay.queuedClientboundPacketCount() != 1) {
            std::cerr << "[FAIL] " << version << " relay level_chunk queue mismatch\n";
            ok = false;
        }

        auto startMcpe = mcpeCodec.encodeMcpePayload({start}, bedrock::VersionedMcpeCompression::Uncompressed);
        auto startFrame = relay.handleClientboundMcpe(startMcpe);
        if (
            startFrame.forwardedPackets.size() != 2 ||
            startFrame.forwardedPackets[0].name != "start_game" ||
            startFrame.forwardedPackets[1].name != "level_chunk" ||
            relay.queuedClientboundPacketCount() != 0
        ) {
            std::cerr << "[FAIL] " << version << " relay start_game release mismatch\n";
            ok = false;
        }
    }

    if (definition.hasPacket("play_status")) {
        auto relay = makeRelay();
        auto packet = packetCodec.makePacketByName("play_status", {0x01});
        auto mcpe = mcpeCodec.encodeMcpePayload({packet}, bedrock::VersionedMcpeCompression::Uncompressed);
        auto queued = relay.handleClientboundMcpe(mcpe);

        if (!queued.queued || relay.downQueueSize() != 1) {
            std::cerr << "[FAIL] " << version << " relay down queue mismatch\n";
            ok = false;
        }

        auto flushed = relay.markDownstreamJoined();
        if (flushed.size() != 1 || flushed[0].forwardedPackets.size() != 1 || relay.downQueueSize() != 0) {
            std::cerr << "[FAIL] " << version << " relay down queue flush mismatch\n";
            ok = false;
        }
    }

    if (definition.hasPacket("client_cache_status")) {
        auto relay = makeRelay();
        relay.markDownstreamJoined();
        auto packet = packetCodec.makePacketByName("client_cache_status", {0x01});
        auto mcpe = mcpeCodec.encodeMcpePayload({packet}, bedrock::VersionedMcpeCompression::Uncompressed);
        auto queued = relay.handleServerboundMcpe(mcpe);

        if (!queued.queued || relay.upQueueSize() != 1) {
            std::cerr << "[FAIL] " << version << " relay up queue mismatch\n";
            ok = false;
        }

        auto flushed = relay.markUpstreamJoined();
        if (flushed.size() != 1 || flushed[0].forwardedPackets.size() != 1 || relay.upQueueSize() != 0) {
            std::cerr << "[FAIL] " << version << " relay up queue flush mismatch\n";
            ok = false;
        }
    }

    return ok;
}

bool checkRelayPacketApi() {
    constexpr const char* version = "1.21.100";
    bool ok = true;

    try {
        bedrock::ProtoDefPacketEncoder encoder(version);
        auto payload = encoder.encodePacket("player_auth_input", object({
            {"pitch", bedrock::ProtoDefValue::floating(0.0)},
            {"yaw", bedrock::ProtoDefValue::floating(90.0)},
            {"position", vec3f(10.0, 70.0, 20.0)},
            {"move_vector", vec2f(0.0, 1.0)},
            {"head_yaw", bedrock::ProtoDefValue::floating(90.0)},
            {"input_data", inputFlags({"start_gliding", "up"})},
            {"input_mode", bedrock::ProtoDefValue::string("mouse")},
            {"play_mode", bedrock::ProtoDefValue::string("normal")},
            {"interaction_model", bedrock::ProtoDefValue::string("classic")},
            {"interact_rotation", vec2f(0.0, 0.0)},
            {"tick", bedrock::ProtoDefValue::uinteger(1)},
            {"delta", vec3f(0.0, 0.0, 0.0)},
            {"analogue_move_vector", vec2f(0.0, 1.0)},
            {"camera_orientation", vec3f(0.0, 90.0, 0.0)},
            {"raw_move_vector", vec2f(0.0, 1.0)}
        }));

        auto codec = bedrock::VersionedPacketCodec::forVersion(version);
        bedrock::BedrockRelayPacketEvent event;
        event.direction = bedrock::BedrockRelayDirection::Serverbound;
        event.packet = codec.makePacketByName("player_auth_input", payload);

        bedrock::RelayPacketEvent wrapped(version, event);
        const bool startGliding = wrapped.getBool("input_data.start_gliding");
        const bool up = wrapped.getBool("input_data.up");
        const double x = wrapped.getDouble("position.x");
        const double y = wrapped.getDouble("position.y");
        const double z = wrapped.getDouble("position.z");
        if (!startGliding || !up || x != 10.0 || y != 70.0 || z != 20.0) {
            std::cerr << "[FAIL] relay api player_auth_input decoded params mismatch"
                      << " start_gliding=" << startGliding
                      << " up=" << up
                      << " pos=(" << x << "," << y << "," << z << ")"
                      << "\n";
            ok = false;
        }

        wrapped.set("delta.x", 0.25);
        wrapped.set("delta.y", 0.5);
        wrapped.set("delta.z", 0.75);
        (void) encoder.encodePacket("player_auth_input", bedrock::ProtoDefValue::object(wrapped.decodedParams()));
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] relay api player_auth_input: " << e.what() << "\n";
        ok = false;
    }

    return ok;
}

bool checkClientLifecycleGolden() {
    struct Fixture {
        const char* name;
        bedrock::ProtoDefValue value;
        std::vector<uint8_t> expectedFullPacket;
    };

    const std::vector<std::string> versions = {
        "1.20.40", "1.20.61", "1.20.80", "1.21.100"
    };
    bool ok = true;

    for (const auto& version : versions) {
        bedrock::ProtoDefPacketEncoder encoder(version);
        auto codec = bedrock::VersionedPacketCodec::forVersion(version);
        std::vector<Fixture> fixtures;
        fixtures.push_back({
            "resource_pack_client_response",
            object({
                {"response_status", bedrock::ProtoDefValue::string("completed")},
                {"resourcepackids", array()}
            }),
            {0x08, 0x04, 0x00, 0x00}
        });
        fixtures.push_back({
            "client_cache_status",
            object({{"enabled", bedrock::ProtoDefValue::boolean(false)}}),
            {0x81, 0x01, 0x00}
        });
        fixtures.push_back({
            "tick_sync",
            object({
                {"request_time", bedrock::ProtoDefValue::integer(123456789)},
                {"response_time", bedrock::ProtoDefValue::integer(0)}
            }),
            {
                0x17, 0x15, 0xcd, 0x5b, 0x07, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            }
        });
        fixtures.push_back({
            "request_chunk_radius",
            object({
                {"chunk_radius", bedrock::ProtoDefValue::integer(10)},
                {"max_radius", bedrock::ProtoDefValue::uinteger(0)}
            }),
            {0x45, 0x14, 0x00}
        });
        fixtures.push_back({
            "set_local_player_as_initialized",
            object({
                {"runtime_entity_id", bedrock::ProtoDefValue::uinteger(123456789)}
            }),
            {0x71, 0x95, 0x9a, 0xef, 0x3a}
        });

        for (const auto& fixture : fixtures) {
            try {
                auto payload = encoder.encodePacket(fixture.name, fixture.value);
                auto fullPacket = codec.encodeFullPacketByName(fixture.name, payload);
                if (!sameBytes(fullPacket, fixture.expectedFullPacket)) {
                    std::cerr << "[FAIL] " << version << " JS lifecycle golden "
                              << fixture.name << " byte mismatch\n";
                    ok = false;
                }
            } catch (const std::exception& e) {
                std::cerr << "[FAIL] " << version << " JS lifecycle golden "
                          << fixture.name << ": " << e.what() << "\n";
                ok = false;
            }
        }

        try {
            auto spawn = codec.makePacketByName("play_status", {0x00, 0x00, 0x00, 0x03});
            if (bedrock::VersionedPayloadReader::readPlayStatus(spawn).status != 3) {
                std::cerr << "[FAIL] " << version << " play_status endian mismatch\n";
                ok = false;
            }

            std::vector<uint8_t> startGamePayload = {
                0x02,                         // zigzag64 entity_id = 1
                0x95, 0x9a, 0xef, 0x3a,       // varint64 runtime_entity_id = 123456789
                0x00,                         // zigzag32 player_gamemode = 0
                0x00, 0x00, 0x00, 0x00,       // x
                0x00, 0x00, 0x00, 0x00,       // y
                0x00, 0x00, 0x00, 0x00        // z
            };
            auto startGame = codec.makePacketByName("start_game", startGamePayload);
            auto parsed = bedrock::VersionedPayloadReader::readStartGame(startGame);
            if (parsed.entityId != 1 || parsed.runtimeEntityId != 123456789) {
                std::cerr << "[FAIL] " << version << " start_game entity id mismatch\n";
                ok = false;
            }
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << version << " lifecycle payload reader: "
                      << e.what() << "\n";
            ok = false;
        }
    }

    return ok;
}

bool checkVersionContractGolden() {
    bool ok = true;
    const std::vector<std::pair<std::string, uint32_t>> expected {
        {"1.26.0", 924u},
        {"1.21.130", 898u},
        {"1.21.124", 860u},
        {"1.21.120", 859u},
        {"1.21.111", 844u},
        {"1.21.100", 827u},
        {"1.21.93", 819u},
        {"1.21.90", 818u},
        {"1.21.80", 800u},
        {"1.21.70", 786u},
        {"1.21.60", 776u},
        {"1.21.50", 766u},
        {"1.21.42", 748u},
        {"1.21.30", 729u},
        {"1.21.20", 712u},
        {"1.21.2", 686u},
        {"1.21.0", 685u},
        {"1.20.80", 671u},
        {"1.20.71", 662u},
        {"1.20.61", 649u},
        {"1.20.50", 630u},
        {"1.20.40", 622u},
        {"1.20.30", 618u},
        {"1.20.15", 594u},
        {"1.20.10", 594u},
        {"1.20.0", 589u},
        {"1.19.80", 582u},
        {"1.19.70", 575u},
        {"1.19.63", 568u},
        {"1.19.62", 567u},
        {"1.19.60", 567u},
        {"1.19.50", 560u},
        {"1.19.40", 557u},
        {"1.19.30", 554u},
        {"1.19.21", 545u},
        {"1.19.20", 544u},
        {"1.19.10", 534u},
        {"1.19.1", 527u},
        {"1.18.30", 503u},
        {"1.18.11", 486u},
        {"1.18.0", 475u},
        {"1.17.40", 471u},
        {"1.17.30", 465u},
        {"1.17.10", 448u},
        {"1.17.0", 440u},
        {"1.16.220", 431u},
        {"1.16.210", 428u},
        {"1.16.201", 422u}
    };

    if (bedrock::CURRENT_VERSION != "1.26.0" ||
        bedrock::MIN_VERSION != "1.16.201" ||
        bedrock::Versions.size() != expected.size() ||
        bedrock::VERSION_ENTRIES.size() != expected.size()) {
        std::cerr << "[FAIL] JS options constants/version count mismatch\n";
        ok = false;
    }

    std::vector<std::string> expectedNames;
    expectedNames.reserve(expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& [name, protocol] = expected[i];
        expectedNames.push_back(name);
        const auto mapIt = bedrock::Versions.find(name);
        if (mapIt == bedrock::Versions.end() || mapIt->second != protocol ||
            bedrock::VERSION_ENTRIES[i].minecraftVersion != name ||
            bedrock::VERSION_ENTRIES[i].protocolVersion != protocol ||
            !bedrock::supportsVersion(name) ||
            bedrock::ProtocolDefinition::forVersion(name).protocolVersion() != protocol) {
            std::cerr << "[FAIL] JS Versions mismatch for " << name << "\n";
            ok = false;
        }
    }

    if (bedrock::versions() != expectedNames ||
        bedrock::ProtocolDefinition::versions() != expectedNames) {
        std::cerr << "[FAIL] JS Versions insertion order mismatch\n";
        ok = false;
    }

    const std::vector<std::string> rejected {
        "", "auto", "latest", "0.14", "0.15", "1.26.10", "1.26.20"
    };
    for (const auto& version : rejected) {
        if (bedrock::supportsVersion(version) ||
            bedrock::ProtocolDefinition::supportsVersion(version)) {
            std::cerr << "[FAIL] JS-unknown version publicly supported: " << version << "\n";
            ok = false;
        }

        bool networkRejected = false;
        try {
            bedrock::BedrockNetworkClientOptions options;
            options.version = version;
            bedrock::BedrockNetworkClient client(options);
        } catch (const std::exception&) {
            networkRejected = true;
        }
        if (!networkRejected) {
            std::cerr << "[FAIL] network client accepted JS-unknown version: " << version << "\n";
            ok = false;
        }

        // The high-level factory needs one C++ representation for an omitted
        // JS property. Options::version therefore uses the empty string as its
        // presence sentinel and a directly constructed facade normalizes that
        // one value to CURRENT_VERSION. Every non-empty JS-unknown spelling is
        // still rejected exactly as before; the low-level client above also
        // continues to reject an explicitly empty protocol version.
        if (!version.empty()) {
            bool rootRejected = false;
            try {
                bedrock::Options options;
                options.version = version;
                bedrock::Client client(options);
            } catch (const std::exception&) {
                rootRejected = true;
            }
            if (!rootRejected) {
                std::cerr << "[FAIL] createClient facade accepted JS-unknown version: " << version << "\n";
                ok = false;
            }
        }
    }

    for (const auto& [alias, schema, protocol] :
         std::vector<std::tuple<std::string, std::string, uint32_t>> {
             {"1.19.63", "1.19.62", 568u},
             {"1.20.15", "1.20.10", 594u}
         }) {
        try {
            const auto aliasDefinition = bedrock::ProtocolDefinition::forVersion(alias);
            const auto schemaDefinition = bedrock::ProtocolDefinition::forVersion(schema);
            if (std::string(aliasDefinition.minecraftVersion()) != alias ||
                aliasDefinition.protocolVersion() != protocol ||
                aliasDefinition.packetCount() != schemaDefinition.packetCount()) {
                std::cerr << "[FAIL] alias metadata mismatch for " << alias << "\n";
                ok = false;
            }

            const auto aliasCodec = bedrock::VersionedPacketCodec::forVersion(alias);
            const auto full = aliasCodec.encodeFullPacketByName("play_status", {0, 0, 0, 3});
            const auto decoded = aliasCodec.decodeFullPacket(full);
            if (decoded.name != "play_status" ||
                !sameBytes(decoded.payload, {0, 0, 0, 3})) {
                std::cerr << "[FAIL] alias codec mismatch for " << alias << "\n";
                ok = false;
            }

            bedrock::ProtoDefPacketEncoder encoder(alias);
            const auto payload = encoder.encodePacket(
                "client_cache_status",
                bedrock::ProtoDefValue::object({
                    {"enabled", bedrock::ProtoDefValue::boolean(false)}
                })
            );
            if (!sameBytes(payload, {0x00})) {
                std::cerr << "[FAIL] alias schema encode mismatch for " << alias << "\n";
                ok = false;
            }
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] alias support " << alias << ": " << e.what() << "\n";
            ok = false;
        }
    }

    try {
        bedrock::Options options;
        options.host = "127.0.0.1";
        options.port = 19132;
        options.version = "1.20.15";
        options.protocolVersion = 1u;
        bedrock::Client client(options);
        if (client.protocolVersion() != 594u ||
            client.options().protocolVersion != 594u ||
            client.versionLessThan("1.20.10") ||
            client.versionGreaterThan("1.20.10") ||
            !client.versionGreaterThanOrEqualTo("1.20.10") ||
            !client.versionLessThanOrEqualTo("1.20.10") ||
            !client.versionGreaterThan("1.19.63") ||
            !client.versionLessThan("1.21.0") ||
            !client.versionGreaterThanOrEqualTo(594u) ||
            !client.versionLessThanOrEqualTo(594u)) {
            std::cerr << "[FAIL] client protocolVersion/comparator mismatch\n";
            ok = false;
        }

        for (const auto& unknown : {
                 std::string("unknown"),
                 std::string("latest"),
                 std::string("")
             }) {
            const auto throwsExact = [&](auto&& compare) {
                try {
                    (void) compare();
                } catch (const std::exception& e) {
                    return std::string(e.what()) == "Unknown version: " + unknown;
                }
                return false;
            };
            if (!throwsExact([&]() { return client.versionLessThan(unknown); }) ||
                !throwsExact([&]() { return client.versionGreaterThan(unknown); }) ||
                !throwsExact([&]() { return client.versionGreaterThanOrEqualTo(unknown); }) ||
                !throwsExact([&]() { return client.versionLessThanOrEqualTo(unknown); })) {
                std::cerr << "[FAIL] comparator accepted unknown string " << unknown << "\n";
                ok = false;
            }
        }

        for (const auto& inherited : {
                 std::string("constructor"),
                 std::string("__defineGetter__"),
                 std::string("__defineSetter__"),
                 std::string("hasOwnProperty"),
                 std::string("__lookupGetter__"),
                 std::string("__lookupSetter__"),
                 std::string("isPrototypeOf"),
                 std::string("propertyIsEnumerable"),
                 std::string("toString"),
                 std::string("valueOf"),
                 std::string("__proto__"),
                 std::string("toLocaleString")
             }) {
            if (client.versionLessThan(inherited) ||
                client.versionGreaterThan(inherited) ||
                client.versionGreaterThanOrEqualTo(inherited) ||
                client.versionLessThanOrEqualTo(inherited)) {
                std::cerr << "[FAIL] client Object.prototype comparator did not coerce to NaN: "
                          << inherited << "\n";
                ok = false;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] public client version contract: " << e.what() << "\n";
        ok = false;
    }

    try {
        bedrock::BedrockServerOptions options;
        options.version = "1.20.15";
        bedrock::BedrockServer server(options);
        bedrock::BedrockServerConnection connection;

        if (server.versionLessThan("1.20.10") ||
            server.versionGreaterThan("1.20.10") ||
            !server.versionGreaterThanOrEqualTo("1.20.10") ||
            !server.versionGreaterThan("1.19.63") ||
            !server.versionLessThan("1.21.0") ||
            server.versionLessThan(594u) ||
            server.versionGreaterThan(594u) ||
            !server.versionGreaterThanOrEqualTo(594u) ||
            server.versionLessThan(connection, "1.20.10") ||
            server.versionGreaterThan(connection, "1.20.10") ||
            !server.versionGreaterThanOrEqualTo(connection, "1.20.10") ||
            !server.versionLessThanOrEqualTo(connection, "1.20.10") ||
            !server.versionGreaterThan(connection, "1.19.63") ||
            !server.versionLessThan(connection, "1.21.0") ||
            server.versionLessThan(connection, 594u) ||
            server.versionGreaterThan(connection, 594u) ||
            !server.versionGreaterThanOrEqualTo(connection, 594u) ||
            !server.versionLessThanOrEqualTo(connection, 594u)) {
            std::cerr << "[FAIL] server Player protocolVersion/comparator mismatch\n";
            ok = false;
        }

        for (const auto& unknown : {
                 std::string("unknown"),
                 std::string("latest"),
                 std::string(""),
                 std::string("toString"),
                 std::string("constructor")
             }) {
            if (server.versionLessThan(unknown) ||
                server.versionGreaterThan(unknown) ||
                server.versionGreaterThanOrEqualTo(unknown)) {
                std::cerr << "[FAIL] direct Server comparator did not coerce missing string to NaN: "
                          << unknown << "\n";
                ok = false;
            }
        }

        for (const auto& unknown : {
                 std::string("unknown"),
                 std::string("latest"),
                 std::string("")
             }) {
            const auto throwsExact = [&](auto&& compare) {
                try {
                    (void) compare();
                } catch (const std::exception& e) {
                    return std::string(e.what()) == "Unknown version: " + unknown;
                }
                return false;
            };
            if (!throwsExact([&]() { return server.versionLessThan(connection, unknown); }) ||
                !throwsExact([&]() { return server.versionGreaterThan(connection, unknown); }) ||
                !throwsExact([&]() {
                    return server.versionGreaterThanOrEqualTo(connection, unknown);
                }) ||
                !throwsExact([&]() {
                    return server.versionLessThanOrEqualTo(connection, unknown);
                })) {
                std::cerr << "[FAIL] server Player comparator accepted unknown string "
                          << unknown << "\n";
                ok = false;
            }
        }

        for (const auto& inherited : {
                 std::string("constructor"),
                 std::string("__defineGetter__"),
                 std::string("__defineSetter__"),
                 std::string("hasOwnProperty"),
                 std::string("__lookupGetter__"),
                 std::string("__lookupSetter__"),
                 std::string("isPrototypeOf"),
                 std::string("propertyIsEnumerable"),
                 std::string("toString"),
                 std::string("valueOf"),
                 std::string("__proto__"),
                 std::string("toLocaleString")
             }) {
            if (server.versionLessThan(connection, inherited) ||
                server.versionGreaterThan(connection, inherited) ||
                server.versionGreaterThanOrEqualTo(connection, inherited) ||
                server.versionLessThanOrEqualTo(connection, inherited)) {
                std::cerr << "[FAIL] server Player Object.prototype comparator did not coerce to NaN: "
                          << inherited << "\n";
                ok = false;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] server Player version contract: " << e.what() << "\n";
        ok = false;
    }

    try {
        bedrock::BedrockNetworkClient client;
        if (client.options().version != std::string(bedrock::CURRENT_VERSION) ||
            client.protocolVersion() != 924u ||
            client.options().protocolVersion != 924u) {
            std::cerr << "[FAIL] default client version is not CURRENT_VERSION\n";
            ok = false;
        }
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] default client version contract: " << e.what() << "\n";
        ok = false;
    }

    return ok;
}

bool checkAuthTitleGolden() {
    bool ok = true;
    const std::vector<std::pair<std::string_view, std::string_view>> expected {
        {bedrock::Titles::MinecraftNintendoSwitch, "00000000441cc96b"},
        {bedrock::Titles::MinecraftPlaystation, "000000004827c78e"},
        {bedrock::Titles::MinecraftAndroid, "0000000048183522"},
        {bedrock::Titles::MinecraftJava, "00000000402b5328"},
        {bedrock::Titles::MinecraftIOS, "000000004c17c01a"},
        {bedrock::Titles::XboxAppIOS, "000000004c12ae6f"},
        {bedrock::Titles::XboxGamepassIOS, "000000004c20a908"}
    };
    for (const auto& [actual, value] : expected) {
        if (actual != value) {
            std::cerr << "[FAIL] prismarine-auth Titles constant mismatch\n";
            ok = false;
        }
    }
    if (bedrock::title.MinecraftNintendoSwitch !=
        bedrock::Titles::MinecraftNintendoSwitch) {
        std::cerr << "[FAIL] root title export mismatch\n";
        ok = false;
    }

    bedrock::XboxLiveAuthOptions defaults;
    const auto defaultFlow = bedrock::XboxLiveAuth::resolveFlowOptions(defaults);
    if (defaultFlow.authTitle != bedrock::Titles::MinecraftNintendoSwitch ||
        defaultFlow.deviceType != "Nintendo" || defaultFlow.flow != "live") {
        std::cerr << "[FAIL] authTitle/deviceType/flow default mismatch\n";
        ok = false;
    }

    bedrock::XboxLiveAuthOptions compatibilityAlias;
    compatibilityAlias.xboxClientId = std::string(bedrock::Titles::MinecraftJava);
    const auto aliasFlow = bedrock::XboxLiveAuth::resolveFlowOptions(compatibilityAlias);
    if (aliasFlow.authTitle != bedrock::Titles::MinecraftJava ||
        !aliasFlow.deviceType.empty() || !aliasFlow.flow.empty()) {
        std::cerr << "[FAIL] xboxClientId compatibility resolution mismatch\n";
        ok = false;
    }

    bedrock::XboxLiveAuthOptions explicitWithoutFlow;
    explicitWithoutFlow.authTitle = std::string(bedrock::Titles::MinecraftIOS);
    const auto explicitWithoutFlowResolved =
        bedrock::XboxLiveAuth::resolveFlowOptions(explicitWithoutFlow);
    if (explicitWithoutFlowResolved.authTitle != bedrock::Titles::MinecraftIOS ||
        !explicitWithoutFlowResolved.deviceType.empty() ||
        !explicitWithoutFlowResolved.flow.empty()) {
        std::cerr << "[FAIL] explicit authTitle unexpectedly received JS defaults\n";
        ok = false;
    }

    bedrock::XboxLiveAuthOptions explicitOptions;
    explicitOptions.authTitle = std::string(bedrock::Titles::MinecraftAndroid);
    explicitOptions.xboxClientId = std::string(bedrock::Titles::MinecraftJava);
    explicitOptions.deviceType = "Android";
    explicitOptions.flow = "sisu";
    const auto explicitFlow = bedrock::XboxLiveAuth::resolveFlowOptions(explicitOptions);
    if (explicitFlow.authTitle != bedrock::Titles::MinecraftAndroid ||
        explicitFlow.deviceType != "Android" || explicitFlow.flow != "sisu") {
        std::cerr << "[FAIL] explicit auth flow resolution mismatch\n";
        ok = false;
    }

    bedrock::XboxLiveAuthOptions explicitNull;
    explicitNull.authTitle = nullptr;
    explicitNull.deviceType = "NullDevice";
    explicitNull.flow = "msal";
    const auto nullFlow = bedrock::XboxLiveAuth::resolveFlowOptions(explicitNull);
    if (!nullFlow.authTitle.empty() || nullFlow.deviceType != "NullDevice" ||
        nullFlow.flow != "msal" || !explicitNull.authTitle.isNull()) {
        std::cerr << "[FAIL] null authTitle incorrectly received JS defaults\n";
        ok = false;
    }

    bedrock::XboxLiveAuthOptions explicitUndefined;
    explicitUndefined.authTitle = bedrock::jsUndefined;
    explicitUndefined.deviceType = "must-be-overwritten";
    explicitUndefined.flow = "must-be-overwritten";
    const auto undefinedFlow =
        bedrock::XboxLiveAuth::resolveFlowOptions(explicitUndefined);
    if (undefinedFlow.authTitle != bedrock::Titles::MinecraftNintendoSwitch ||
        undefinedFlow.deviceType != "Nintendo" ||
        undefinedFlow.flow != "live" ||
        !explicitUndefined.authTitle.isUndefined()) {
        std::cerr << "[FAIL] undefined authTitle did not receive JS defaults\n";
        ok = false;
    }

    bedrock::BedrockNetworkClient normalizedNetworkClient;
    const auto networkOptions = normalizedNetworkClient.options();
    if (networkOptions.authTitle.has_value() ||
        !networkOptions.deviceType.empty() || !networkOptions.flow.empty()) {
        std::cerr << "[FAIL] network client ran auth defaults at construction\n";
        ok = false;
    }

    bedrock::Options normalizedRootOptions;
    normalizedRootOptions.host = "127.0.0.1";
    normalizedRootOptions.port = 19132;
    bedrock::Client normalizedRootClient(std::move(normalizedRootOptions));
    const auto rootOptions = normalizedRootClient.options();
    if (rootOptions.authTitle.has_value() ||
        !rootOptions.deviceType.empty() || !rootOptions.flow.empty()) {
        std::cerr << "[FAIL] root client ran auth defaults at construction\n";
        ok = false;
    }

    return ok;
}

bool checkMsalConfigGolden() {
    bool ok = true;
    const bedrock::XboxLiveAuthFlowOptions msalEmptyTitle {
        .authTitle = "",
        .deviceType = "ExplicitDevice",
        .flow = "msal"
    };

    auto supplied = bedrock::makeMsalConfig(
        "caller-client-id",
        "https://login.example.test/tenant"
    );
    supplied->set(
        "cache",
        bedrock::MsalConfig::string("must-be-replaced")
    );
    const auto initialized = bedrock::XboxLiveAuth::
        initializePrismarineAuthFlow(msalEmptyTitle, supplied);
    const auto* suppliedAuth = supplied->get("auth");
    const auto* suppliedCache = supplied->get("cache");
    const auto* suppliedPlugin = suppliedCache && suppliedCache->isObject()
        ? suppliedCache->get("cachePlugin")
        : nullptr;
    if (initialized != supplied || !suppliedAuth || !suppliedCache ||
        !suppliedPlugin || !suppliedPlugin->isObject() ||
        suppliedCache->objectNode()->size() != 1 ||
        suppliedAuth->get("clientId")->stringValue() != "caller-client-id" ||
        suppliedAuth->get("authority")->stringValue() !=
            "https://login.example.test/tenant") {
        std::cerr << "[FAIL] supplied msalConfig identity/cache mutation mismatch\n";
        ok = false;
    }

    const bedrock::XboxLiveAuthFlowOptions fallbackFlow {
        .authTitle = "fallback-client-id",
        .deviceType = "",
        .flow = "msal"
    };
    const auto fallback = bedrock::XboxLiveAuth::
        initializePrismarineAuthFlow(fallbackFlow, {});
    const auto* fallbackAuth = fallback ? fallback->get("auth") : nullptr;
    if (!fallback || fallback == supplied || !fallbackAuth ||
        fallbackAuth->get("clientId")->stringValue() != "fallback-client-id" ||
        fallbackAuth->get("authority")->stringValue() !=
            "https://login.microsoftonline.com/consumers" ||
        !fallback->get("cache")) {
        std::cerr << "[FAIL] private default msalConfig clone mismatch\n";
        ok = false;
    }

    const auto checkMalformed = [&ok, &msalEmptyTitle](
        bedrock::MsalConfigPtr config,
        const std::string& expected
    ) {
        bool exact = false;
        try {
            (void) bedrock::XboxLiveAuth::initializePrismarineAuthFlow(
                msalEmptyTitle,
                config
            );
        } catch (const std::exception& error) {
            exact = error.what() == expected;
        }
        if (!exact || (config && config->isObject() && config->get("cache"))) {
            std::cerr << "[FAIL] malformed msalConfig constructor boundary mismatch\n";
            ok = false;
        }
    };
    checkMalformed(
        std::make_shared<bedrock::MsalConfig>(
            bedrock::MsalConfig::object({})
        ),
        "Cannot read properties of undefined (reading 'clientId')"
    );
    checkMalformed(
        std::make_shared<bedrock::MsalConfig>(
            bedrock::MsalConfig::object({
                {"auth", bedrock::MsalConfig::null()}
            })
        ),
        "Cannot read properties of null (reading 'clientId')"
    );

    auto falsyConfig = std::make_shared<bedrock::MsalConfig>(
        bedrock::MsalConfig::boolean(false)
    );
    bool exactMissingTitle = false;
    try {
        (void) bedrock::XboxLiveAuth::initializePrismarineAuthFlow(
            msalEmptyTitle,
            falsyConfig
        );
    } catch (const std::exception& error) {
        exactMissingTitle = std::string(error.what()) ==
            "Must specify an Azure client ID token inside the `authTitle` parameter "
            "when using Azure-based auth. See "
            "https://learn.microsoft.com/en-us/entra/identity-platform/"
            "quickstart-register-app#register-an-application for more information "
            "on obtaining an Azure token.";
    }
    if (!exactMissingTitle) {
        std::cerr << "[FAIL] falsy msalConfig did not take fallback title branch\n";
        ok = false;
    }

    auto ignoredByLive = bedrock::makeMsalConfig("ignored");
    ignoredByLive->set(
        "cache",
        bedrock::MsalConfig::string("unchanged")
    );
    const auto liveResult = bedrock::XboxLiveAuth::
        initializePrismarineAuthFlow(
            bedrock::XboxLiveAuthFlowOptions {
                .authTitle = "live-title",
                .deviceType = "Nintendo",
                .flow = "live"
            },
            ignoredByLive
        );
    if (liveResult || !ignoredByLive->get("cache")->isString()) {
        std::cerr << "[FAIL] live flow inspected or mutated msalConfig\n";
        ok = false;
    }

    return ok;
}

class RecordingSerializableTokenCache final
    : public bedrock::ISerializableTokenCache {
public:
    void deserialize(
        const std::optional<std::string>& serializedCache
    ) override {
        ++deserializeCalls;
        deserialized = serializedCache;
        if (onDeserialize) onDeserialize(serializedCache);
    }

    std::string serialize() override {
        ++serializeCalls;
        if (onSerialize) return onSerialize();
        return serialized;
    }

    int deserializeCalls = 0;
    int serializeCalls = 0;
    std::optional<std::string> deserialized;
    std::string serialized = "{}";
    std::function<void(const std::optional<std::string>&)> onDeserialize;
    std::function<std::string()> onSerialize;
};

class ScriptedMsalPublicClientApplication final
    : public bedrock::IMsalPublicClientApplication {
public:
    using Method = std::function<bedrock::JsPromise<bedrock::JsRuntimeValue>(
        bedrock::JsRuntimeValue request
    )>;

    bedrock::JsPromise<bedrock::JsRuntimeValue>
    acquireTokenByRefreshToken(bedrock::JsRuntimeValue request) override {
        if (!refresh) throw std::runtime_error("refresh script missing");
        return refresh(std::move(request));
    }

    bedrock::JsPromise<bedrock::JsRuntimeValue>
    acquireTokenByDeviceCode(bedrock::JsRuntimeValue request) override {
        if (!device) throw std::runtime_error("device script missing");
        return device(std::move(request));
    }

    Method refresh;
    Method device;
};

bool checkMsalRequestBuilderGolden() {
    bool ok = true;
    bedrock::MsalRequestBuilderOptions options;
    options.clientId = "CID";
    options.scopes = {"XboxLive.signin", "offline_access"};
    options.correlationId = "11111111-1111-4111-8111-111111111111";
    options.authority =
        "https://login.microsoftonline.com/consumers/";
    const bedrock::MsalRequestBuilder builder(std::move(options));

    const auto device = builder.deviceCodeRequest();
    const auto poll = builder.deviceCodeTokenRequest("D C/+?");
    const auto refresh = builder.refreshTokenRequest("R T/+?");
    const std::string contentType =
        "application/x-www-form-urlencoded;charset=utf-8";

    if (device.method != "POST" ||
        device.url !=
            "https://login.microsoftonline.com/consumers/oauth2/v2.0/"
            "devicecode" ||
        device.body !=
            "scope=XboxLive.signin%20offline_access%20openid%20profile&"
            "client_id=CID" ||
        !device.header("Content-Type") ||
        *device.header("Content-Type") != contentType ||
        device.headers.size() != 1) {
        std::cerr << "[FAIL] MSAL device-code request oracle mismatch\n";
        ok = false;
    }

    const std::string tokenUrl =
        "https://login.microsoftonline.com/consumers/oauth2/v2.0/token?"
        "client-request-id=11111111-1111-4111-8111-111111111111";
    const std::string platformFields =
        "&x-client-SKU=msal.js.node&x-client-VER=2.16.3&x-client-OS=" +
        builder.platform().os + "&x-client-CPU=" + builder.platform().cpu;
    const std::string commonTail = platformFields +
        "&x-ms-lib-capability=retry-after, h429";

    if (poll.method != "POST" || poll.url != tokenUrl ||
        poll.body !=
            "scope=XboxLive.signin%20offline_access%20openid%20profile&"
            "client_id=CID&grant_type=device_code&device_code=D%20C%2F%2B%3F&"
            "client-request-id=11111111-1111-4111-8111-111111111111&"
            "client_info=1" + commonTail +
            "&x-client-current-telemetry=5|671,0,,,|,&"
            "x-client-last-telemetry=5|0|||0,0" ||
        !poll.header("Content-Type") ||
        *poll.header("Content-Type") != contentType) {
        std::cerr << "[FAIL] MSAL device-code poll request oracle mismatch: "
                  << poll.body << "\n";
        ok = false;
    }

    if (refresh.method != "POST" || refresh.url != tokenUrl ||
        refresh.body !=
            "client_id=CID&"
            "scope=XboxLive.signin%20offline_access%20openid%20profile&"
            "grant_type=refresh_token&client_info=1" + commonTail +
            "&x-client-current-telemetry=5|872,0,,,|,&"
            "x-client-last-telemetry=5|0|||0,0&refresh_token=R%20T%2F%2B%3F" ||
        !refresh.header("Content-Type") ||
        *refresh.header("Content-Type") != contentType) {
        std::cerr << "[FAIL] MSAL refresh request oracle mismatch: "
                  << refresh.body << "\n";
        ok = false;
    }

    const auto scopes = bedrock::MsalRequestParameterBuilder::normalizeScopes({
        "  OpenID  ", "scope", "scope", "", "offline_access"
    });
    if (scopes != std::vector<std::string>({
            "OpenID", "scope", "offline_access", "openid", "profile"
        })) {
        std::cerr << "[FAIL] MSAL ScopeSet normalization/order mismatch\n";
        ok = false;
    }

    bool malformed = false;
    try {
        (void) bedrock::MsalRequestParameterBuilder::encodeURIComponent(
            std::string("\xed\xa0\x80", 3)
        );
    } catch (const std::exception& error) {
        malformed = std::string(error.what()) == "URI malformed";
    }
    if (!malformed) {
        std::cerr << "[FAIL] MSAL encodeURIComponent lone-surrogate mismatch\n";
        ok = false;
    }

    return ok;
}

bool checkMsalSerializableTokenCacheGolden() {
    bool ok = true;
    bedrock::MsalSerializableTokenCache cache;
    const std::string empty =
        "{\"Account\":{},\"IdToken\":{},\"AccessToken\":{},"
        "\"RefreshToken\":{},\"AppMetadata\":{}}";
    if (cache.hasChanged() || cache.serialize() != empty ||
        cache.hasChanged()) {
        std::cerr << "[FAIL] MSAL TokenCache fresh serialization mismatch\n";
        ok = false;
    }

    const std::string snapshot =
        "{\"unknown\":{\"kept\":true},\"Account\":{\"a\":{"
        "\"home_account_id\":\"h\",\"environment\":\"e\","
        "\"realm\":\"r\",\"local_account_id\":\"l\","
        "\"username\":\"u\",\"authority_type\":\"MSSTS\","
        "\"extra\":\"preserved\",\"tenantProfiles\":["
        "\"{ \\\"tenantId\\\" : \\\"r\\\" }\"]}},"
        "\"IdToken\":{},\"AccessToken\":{},\"RefreshToken\":{},"
        "\"AppMetadata\":{}}";
    cache.deserialize(snapshot);
    if (!cache.hasChanged()) {
        std::cerr << "[FAIL] MSAL TokenCache deserialize change flag mismatch\n";
        ok = false;
    }
    const auto serialized = cache.serialize();
    const auto materialized = bedrock::JsRuntimeJson::parse(serialized);
    const auto* unknown = materialized.get("unknown");
    const auto* accountMap = materialized.get("Account");
    const auto* account = accountMap ? accountMap->get("a") : nullptr;
    const auto* profiles = account ? account->get("tenantProfiles") : nullptr;
    if (cache.hasChanged() || !unknown || !unknown->get("kept") ||
        !unknown->get("kept")->boolValue() || !account ||
        !account->get("extra") ||
        account->get("extra")->stringValue() != "preserved" ||
        !profiles || !profiles->isArray() || profiles->length() != 1 ||
        !profiles->get(0) ||
        profiles->get(0)->stringValue() != "{\"tenantId\":\"r\"}") {
        std::cerr << "[FAIL] MSAL TokenCache snapshot merge/projection mismatch\n";
        ok = false;
    }

    // NodeStorage.setInMemoryCache overlays a second deserialize on its flat
    // store, so an entity absent from the new document remains live.
    cache.deserialize(empty);
    if (!cache.hasEntity(
            bedrock::MsalSerializableTokenCache::EntityMap::Account,
            "a"
        )) {
        std::cerr << "[FAIL] MSAL TokenCache repeated-deserialize overlay mismatch\n";
        ok = false;
    }
    cache.removeEntity(
        bedrock::MsalSerializableTokenCache::EntityMap::Account,
        "a"
    );
    const auto removed = bedrock::JsRuntimeJson::parse(cache.serialize());
    if (!removed.get("Account") ||
        removed.get("Account")->hasOwn("a")) {
        std::cerr << "[FAIL] MSAL TokenCache removal merge mismatch\n";
        ok = false;
    }

    // Falsy deserialize replaces cacheSnapshot but leaves live maps intact.
    cache.setEntity(
        bedrock::MsalSerializableTokenCache::EntityMap::AccessToken,
        "token",
        bedrock::JsRuntimeValue::object({
            {"home_account_id", bedrock::JsRuntimeValue::string("home")},
            {"environment", bedrock::JsRuntimeValue::string("env")},
            {"credential_type", bedrock::JsRuntimeValue::string("AccessToken")},
            {"client_id", bedrock::JsRuntimeValue::string("CID")},
            {"secret", bedrock::JsRuntimeValue::string("AT")},
            {"realm", bedrock::JsRuntimeValue::string("tenant")},
            {"target", bedrock::JsRuntimeValue::string("scope")}
        })
    );
    cache.deserialize(std::nullopt);
    if (!cache.snapshot().isUndefined() ||
        !cache.hasEntity(
            bedrock::MsalSerializableTokenCache::EntityMap::AccessToken,
            "token"
        )) {
        std::cerr << "[FAIL] MSAL TokenCache undefined deserialize mismatch\n";
        ok = false;
    }

    return ok;
}

bool checkJsRuntimeDateMapGolden() {
    bool ok = true;
    const auto epoch = bedrock::JsRuntimeValue::date(0.9);
    const auto beforeEpoch = bedrock::JsRuntimeValue::date(-1.9);
    const auto maximum = bedrock::JsRuntimeValue::date(8.64e15);
    const auto minimum = bedrock::JsRuntimeValue::date(-8.64e15);
    const auto invalid = bedrock::JsRuntimeValue::date(8.64e15 + 1.0);
    if (epoch.dateMilliseconds() != 0.0 ||
        epoch.dateIsoString() != "1970-01-01T00:00:00.000Z" ||
        beforeEpoch.dateMilliseconds() != -1.0 ||
        beforeEpoch.dateIsoString() != "1969-12-31T23:59:59.999Z" ||
        maximum.dateIsoString() != "+275760-09-13T00:00:00.000Z" ||
        minimum.dateIsoString() != "-271821-04-20T00:00:00.000Z" ||
        bedrock::JsRuntimeJson::stringify(invalid) !=
            std::optional<std::string>("null")) {
        std::cerr << "[FAIL] JsRuntime Date/TimeClip/toJSON mismatch\n";
        ok = false;
    }

    auto profiles = bedrock::JsRuntimeValue::map();
    auto profile = bedrock::JsRuntimeValue::object({
        {"tenantId", bedrock::JsRuntimeValue::string("tenant")}
    });
    profiles.mapSet(
        bedrock::JsRuntimeValue::string("tenant"),
        profile
    );
    auto copied = profiles;
    const auto* found = profiles.mapGet(
        bedrock::JsRuntimeValue::string("tenant")
    );
    if (profiles.mapSize() != 1 || !profiles.get("size") ||
        profiles.get("size")->numberValue() != 1 ||
        profiles.hasOwn("size") || !found ||
        !found->sharesIdentityWith(profile) ||
        !copied.sharesIdentityWith(profiles) ||
        bedrock::JsRuntimeJson::stringify(profiles) !=
            std::optional<std::string>("{}")) {
        std::cerr << "[FAIL] JsRuntime Map identity/JSON mismatch\n";
        ok = false;
    }
    return ok;
}

bool checkMsalErrorGolden() {
    bool ok = true;
    bedrock::MsalClientAuthError cancelled(
        "device_code_polling_cancelled",
        "Caller has cancelled token endpoint polling during device code flow "
        "by setting DeviceCodeRequest.cancel = true."
    );
    cancelled.setCorrelationId("correlation");
    const auto cancelledJson = cancelled.jsonStringify();
    if (std::string(cancelled.what()) !=
            "device_code_polling_cancelled: Caller has cancelled token "
            "endpoint polling during device code flow by setting "
            "DeviceCodeRequest.cancel = true." ||
        cancelledJson != std::optional<std::string>(
            "{\"errorCode\":\"device_code_polling_cancelled\","
            "\"errorMessage\":\"Caller has cancelled token endpoint polling "
            "during device code flow by setting DeviceCodeRequest.cancel = "
            "true.\",\"subError\":\"\",\"name\":\"ClientAuthError\","
            "\"correlationId\":\"correlation\"}"
        )) {
        std::cerr << "[FAIL] MSAL ClientAuthError shape/stringify mismatch\n";
        ok = false;
    }

    bedrock::MsalServerError server(
        "invalid_grant",
        "description",
        "bad_token",
        bedrock::JsRuntimeValue::number(70000),
        bedrock::JsRuntimeValue::undefined()
    );
    server.setCorrelationId("server-correlation");
    const auto serverJson = bedrock::stringifyMsalException(
        std::make_exception_ptr(server)
    );
    if (serverJson != std::optional<std::string>(
            "{\"errorCode\":\"invalid_grant\","
            "\"errorMessage\":\"description\","
            "\"subError\":\"bad_token\",\"name\":\"ServerError\","
            "\"errorNo\":70000,\"correlationId\":"
            "\"server-correlation\"}"
        ) ||
        bedrock::stringifyMsalException(
            std::make_exception_ptr(std::runtime_error("plain"))
        ) != std::optional<std::string>("{}")) {
        std::cerr << "[FAIL] MSAL ServerError/plain Error stringify mismatch\n";
        ok = false;
    }
    return ok;
}

bool checkMsalCachePluginGolden() {
    bool ok = true;
    const bedrock::XboxLiveAuthFlowOptions flow {
        .authTitle = "plugin-client-id",
        .deviceType = "",
        .flow = "msal"
    };

    auto oldCache = bedrock::MsalConfig::object({
        {"old", bedrock::MsalConfig::boolean(true)}
    });
    auto retained = bedrock::MsalConfig::object({
        {"identity", bedrock::MsalConfig::string("same")}
    });
    auto config = std::make_shared<bedrock::MsalConfig>(
        bedrock::MsalConfig::object({
            {"2", bedrock::MsalConfig::string("two")},
            {"alpha", bedrock::MsalConfig::boolean(true)},
            {"cache", oldCache},
            {"beta", retained},
            {"1", bedrock::MsalConfig::string("one")},
            {"auth", bedrock::MsalConfig::object({
                {"clientId", bedrock::MsalConfig::string("plugin-client-id")}
            })}
        })
    );

    std::vector<std::string> order;
    auto readPromise = std::make_shared<
        std::promise<bedrock::AuthCacheValue>
    >();
    auto integrationCache = std::make_shared<bedrock::AuthCache>();
    integrationCache->setGetCachedMethod([&order, readPromise] {
        order.push_back("getCached");
        return readPromise->get_future();
    });
    integrationCache->setSetCachedPartialMethod(
        [](bedrock::AuthCacheValue) {
            return bedrock::makeReadyAuthCacheFuture();
        }
    );

    const auto initialized = bedrock::XboxLiveAuth::
        initializePrismarineAuthFlow(flow, config, integrationCache);
    const auto* installedCache = config->get("cache");
    const auto* plugin = installedCache
        ? installedCache->get("cachePlugin")
        : nullptr;
    const auto* before = plugin ? plugin->get("beforeCacheAccess") : nullptr;
    const auto* after = plugin ? plugin->get("afterCacheAccess") : nullptr;

    std::vector<std::string> rootKeys;
    for (const auto& property : config->ownProperties()) {
        rootKeys.push_back(property.key);
    }
    std::vector<std::string> pluginKeys;
    if (plugin) {
        for (const auto& property : plugin->ownProperties()) {
            pluginKeys.push_back(property.key);
        }
    }
    if (initialized != config || !installedCache || !plugin || !before ||
        !after || !before->isFunctionOf<bedrock::MsalCacheHookSignature>() ||
        !after->isFunctionOf<bedrock::MsalCacheHookSignature>() ||
        !before->get("name") ||
        before->get("name")->stringValue() != "beforeCacheAccess" ||
        !before->get("length") ||
        before->get("length")->numberValue() != 1 ||
        !after->get("name") ||
        after->get("name")->stringValue() != "afterCacheAccess" ||
        !after->get("length") ||
        after->get("length")->numberValue() != 1 ||
        !before->ownProperties().empty() || !after->ownProperties().empty() ||
        rootKeys != std::vector<std::string>({
            "1", "2", "alpha", "cache", "beta", "auth"
        }) ||
        pluginKeys != std::vector<std::string>({
            "beforeCacheAccess", "afterCacheAccess"
        }) ||
        !config->get("beta")->sharesIdentityWith(retained) ||
        !oldCache.get("old") || installedCache->sharesIdentityWith(oldCache)) {
        std::cerr << "[FAIL] MSAL cache plugin topology/identity/order mismatch\n";
        ok = false;
    }

    if (before) {
        auto originalTokenCache =
            std::make_shared<RecordingSerializableTokenCache>();
        originalTokenCache->onDeserialize = [&order](const auto&) {
            order.push_back("deserialize");
        };
        auto replacementTokenCache =
            std::make_shared<RecordingSerializableTokenCache>();
        auto context = std::make_shared<bedrock::TokenCacheContext>();
        context->tokenCache = std::static_pointer_cast<
            bedrock::ISerializableTokenCache
        >(originalTokenCache);

        auto pending = before->call<bedrock::MsalCacheHookSignature>(context);
        order.push_back("returned");
        context->tokenCache = std::static_pointer_cast<
            bedrock::ISerializableTokenCache
        >(replacementTokenCache);
        readPromise->set_value(bedrock::JsRuntimeValue::object({
            {"x", bedrock::JsRuntimeValue::number(1)}
        }));
        try {
            pending.get();
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] beforeCacheAccess rejected: "
                      << error.what() << "\n";
            ok = false;
        }
        if (order != std::vector<std::string>({
                "getCached", "returned", "deserialize"
            }) ||
            originalTokenCache->deserialized !=
                std::optional<std::string>("{\"x\":1}") ||
            replacementTokenCache->deserializeCalls != 0) {
            std::cerr << "[FAIL] beforeCacheAccess await/callee capture mismatch\n";
            ok = false;
        }
    }

    auto cacheA = std::make_shared<bedrock::AuthCache>();
    auto cacheB = std::make_shared<bedrock::AuthCache>();
    std::vector<std::string> writeCalls;
    bedrock::JsRuntimeValue writtenA;
    bedrock::JsRuntimeValue writtenB;
    cacheA->setSetCachedPartialMethod(
        [&writeCalls, &writtenA](bedrock::AuthCacheValue value) {
            writeCalls.push_back("A");
            writtenA = std::move(value);
            return bedrock::makeReadyAuthCacheFuture();
        }
    );
    cacheB->setSetCachedPartialMethod(
        [&writeCalls, &writtenB](bedrock::AuthCacheValue value) {
            writeCalls.push_back("B");
            writtenB = std::move(value);
            return bedrock::makeReadyAuthCacheFuture();
        }
    );
    cacheB->setGetCachedMethod([] {
        return bedrock::makeReadyAuthCacheFuture(
            bedrock::JsRuntimeValue::object({
                {"late", bedrock::JsRuntimeValue::boolean(true)}
            })
        );
    });

    auto clientIdObject = bedrock::JsRuntimeValue::object({
        {"opaque-id", bedrock::JsRuntimeValue::boolean(true)}
    });
    auto runtime = bedrock::makeMsaTokenManagerCachePlugin(
        cacheA,
        clientIdObject
    );
    const auto* runtimeBefore = runtime.cachePlugin.get("beforeCacheAccess");
    const auto* runtimeAfter = runtime.cachePlugin.get("afterCacheAccess");
    auto tokenCache = std::make_shared<RecordingSerializableTokenCache>();
    auto changed = std::make_shared<bedrock::TokenCacheContext>();
    changed->tokenCache = std::static_pointer_cast<
        bedrock::ISerializableTokenCache
    >(tokenCache);

    // The false branch must not resolve this.cache or tokenCache at all.
    changed->cacheHasChanged = false;
    tokenCache->onSerialize = []() -> std::string {
        throw std::runtime_error("serialize must not run");
    };
    try {
        runtimeAfter->call<bedrock::MsalCacheHookSignature>(changed).get();
    } catch (...) {
        std::cerr << "[FAIL] afterCacheAccess false branch touched cache\n";
        ok = false;
    }

    // setCachedPartial is captured from A before serialize swaps this.cache.
    changed->cacheHasChanged = true;
    tokenCache->onSerialize = [&runtime, cacheB] {
        runtime.managerState->cache = cacheB;
        return std::string("{\"z\":3}");
    };
    try {
        runtimeAfter->call<bedrock::MsalCacheHookSignature>(changed).get();
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] afterCacheAccess rejected: "
                  << error.what() << "\n";
        ok = false;
    }
    tokenCache->onSerialize = [] {
        return std::string("{\"z\":4}");
    };
    try {
        runtimeAfter->call<bedrock::MsalCacheHookSignature>(changed).get();
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] second afterCacheAccess rejected: "
                  << error.what() << "\n";
        ok = false;
    }
    if (writeCalls != std::vector<std::string>({"A", "B"}) ||
        !writtenA.get("z") || writtenA.get("z")->numberValue() != 3 ||
        !writtenB.get("z") || writtenB.get("z")->numberValue() != 4 ||
        !runtime.managerState->msaClientId.sharesIdentityWith(clientIdObject)) {
        std::cerr << "[FAIL] afterCacheAccess capture/live manager state mismatch\n";
        ok = false;
    }

    // A later before invocation observes the manager's newly assigned cache.
    auto lateTokenCache = std::make_shared<RecordingSerializableTokenCache>();
    auto lateContext = std::make_shared<bedrock::TokenCacheContext>();
    lateContext->tokenCache = std::static_pointer_cast<
        bedrock::ISerializableTokenCache
    >(lateTokenCache);
    try {
        runtimeBefore->call<bedrock::MsalCacheHookSignature>(lateContext).get();
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] late beforeCacheAccess rejected: "
                  << error.what() << "\n";
        ok = false;
    }
    if (lateTokenCache->deserialized !=
            std::optional<std::string>("{\"late\":true}")) {
        std::cerr << "[FAIL] beforeCacheAccess did not use live cache slot\n";
        ok = false;
    }

    const auto expectRejectedWithoutSyncThrow = [&ok](
        const std::string& label,
        const std::function<bedrock::JsPromise<void>()>& invoke,
        const std::string& expected
    ) {
        bedrock::JsPromise<void> promise;
        bool synchronousThrow = false;
        try {
            promise = invoke();
        } catch (...) {
            synchronousThrow = true;
        }
        std::string rejection;
        if (!synchronousThrow) {
            try {
                promise.get();
            } catch (const std::exception& error) {
                rejection = error.what();
            }
        }
        if (synchronousThrow || rejection != expected) {
            std::cerr << "[FAIL] " << label
                      << " sync-throw/rejection mismatch: " << rejection
                      << "\n";
            ok = false;
        }
    };

    auto throwingRead = std::make_shared<bedrock::AuthCache>();
    throwingRead->setGetCachedMethod([]() -> bedrock::AuthCacheValueFuture {
        throw std::runtime_error("get boom");
    });
    auto throwingRuntime = bedrock::makeMsaTokenManagerCachePlugin(
        throwingRead
    );
    auto errorTokenCache =
        std::make_shared<RecordingSerializableTokenCache>();
    auto errorContext = std::make_shared<bedrock::TokenCacheContext>();
    errorContext->tokenCache = std::static_pointer_cast<
        bedrock::ISerializableTokenCache
    >(errorTokenCache);
    const auto* throwingBefore =
        throwingRuntime.cachePlugin.get("beforeCacheAccess");
    expectRejectedWithoutSyncThrow(
        "beforeCacheAccess sync getCached throw",
        [throwingBefore, errorContext] {
            return throwingBefore->call<bedrock::MsalCacheHookSignature>(
                errorContext
            );
        },
        "get boom"
    );
    expectRejectedWithoutSyncThrow(
        "beforeCacheAccess null context",
        [throwingBefore] {
            return throwingBefore->call<bedrock::MsalCacheHookSignature>(
                bedrock::TokenCacheContextPtr {}
            );
        },
        "Cannot read properties of null (reading 'tokenCache')"
    );

    auto missingMethods = std::make_shared<bedrock::AuthCache>();
    auto missingRuntime = bedrock::makeMsaTokenManagerCachePlugin(
        missingMethods
    );
    const auto* missingBefore =
        missingRuntime.cachePlugin.get("beforeCacheAccess");
    expectRejectedWithoutSyncThrow(
        "beforeCacheAccess missing getCached",
        [missingBefore, errorContext] {
            return missingBefore->call<bedrock::MsalCacheHookSignature>(
                errorContext
            );
        },
        "this.cache.getCached is not a function"
    );

    auto parseRuntime = bedrock::makeMsaTokenManagerCachePlugin(cacheB);
    const auto* parseAfter = parseRuntime.cachePlugin.get("afterCacheAccess");
    errorContext->cacheHasChanged = true;
    errorTokenCache->onSerialize = [] {
        return std::string("{bad json}");
    };
    bedrock::JsPromise<void> parsePromise;
    bool parseSyncThrow = false;
    try {
        parsePromise = parseAfter->call<bedrock::MsalCacheHookSignature>(
            errorContext
        );
    } catch (...) {
        parseSyncThrow = true;
    }
    bool parseRejected = false;
    if (!parseSyncThrow) {
        try {
            parsePromise.get();
        } catch (...) {
            parseRejected = true;
        }
    }
    if (parseSyncThrow || !parseRejected) {
        std::cerr << "[FAIL] afterCacheAccess JSON.parse error boundary mismatch\n";
        ok = false;
    }

    return ok;
}

bool checkMsaTokenManagerGolden() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto cache = std::make_shared<bedrock::AuthCache>();
    auto app = std::make_shared<ScriptedMsalPublicClientApplication>();
    auto config = bedrock::makeMsalConfig("manager-client-id");
    auto scopes = bedrock::JsRuntimeValue::array({
        bedrock::JsRuntimeValue::string("XboxLive.signin"),
        bedrock::JsRuntimeValue::string("offline_access")
    });
    bool factorySawPlugin = false;
    bedrock::MsaTokenManagerObservers observers;
    observers.dateNowMilliseconds = [] { return 11000.0; };
    bedrock::MsaTokenManager manager(
        config,
        scopes,
        cache,
        [&](const std::shared_ptr<bedrock::JsRuntimeValue>& supplied) {
            const auto* cacheValue = supplied ? supplied->get("cache") : nullptr;
            factorySawPlugin = cacheValue &&
                cacheValue->get("cachePlugin") &&
                cacheValue->get("cachePlugin")->get("beforeCacheAccess");
            return app;
        },
        queue,
        observers
    );

    if (!factorySawPlugin || manager.msaClientId.stringValue() !=
            "manager-client-id" ||
        !manager.scopes.sharesIdentityWith(scopes) ||
        manager.msalConfig != config || manager.msalApp != app ||
        manager.forceRefresh.truthy() || !manager.forceRefresh.isUndefined()) {
        std::cerr << "[FAIL] MsaTokenManager constructor fields/order mismatch\n";
        ok = false;
    }

    bool exactGetUsersError = false;
    try {
        (void) manager.getUsers();
    } catch (const std::exception& error) {
        exactGetUsersError = std::string(error.what()) ==
            "Cannot read properties of undefined (reading 'Account')";
    }
    if (!exactGetUsersError) {
        std::cerr << "[FAIL] MsaTokenManager fresh getUsers bug mismatch\n";
        ok = false;
    }

    auto userTwo = bedrock::JsRuntimeValue::object({
        {"name", bedrock::JsRuntimeValue::string("two")}
    });
    auto userTen = bedrock::JsRuntimeValue::object({
        {"name", bedrock::JsRuntimeValue::string("ten")}
    });
    auto userZ = bedrock::JsRuntimeValue::object({
        {"name", bedrock::JsRuntimeValue::string("z")}
    });
    auto userA = bedrock::JsRuntimeValue::object({
        {"name", bedrock::JsRuntimeValue::string("a")}
    });
    manager.msaCache = bedrock::JsRuntimeValue::object({
        {"Account", bedrock::JsRuntimeValue::object({
            {"z", userZ},
            {"10", userTen},
            {"2", userTwo},
            {"a", userA}
        })}
    });
    const auto users = manager.getUsers();
    if (!users.isArray() || users.length() != 4 ||
        !users.get(0)->sharesIdentityWith(userTwo) ||
        !users.get(1)->sharesIdentityWith(userTen) ||
        !users.get(2)->sharesIdentityWith(userZ) ||
        !users.get(3)->sharesIdentityWith(userA)) {
        std::cerr << "[FAIL] MsaTokenManager getUsers Object.values order mismatch\n";
        ok = false;
    }
    manager.msaCache = bedrock::JsRuntimeValue::object({
        {"Account", bedrock::JsRuntimeValue::string("A\xF0\x9F\x98\x80")}
    });
    if (bedrock::JsRuntimeJson::stringify(manager.getUsers()) !=
        std::optional<std::string>("[\"A\",\"\\ud83d\",\"\\ude00\"]")) {
        std::cerr << "[FAIL] MsaTokenManager getUsers UTF-16 values mismatch\n";
        ok = false;
    }

    const auto account = [](std::string clientId, double expires,
                            std::string secret) {
        return bedrock::JsRuntimeValue::object({
            {"client_id", bedrock::JsRuntimeValue::string(std::move(clientId))},
            {"expires_on", bedrock::JsRuntimeValue::number(expires)},
            {"secret", bedrock::JsRuntimeValue::string(std::move(secret))}
        });
    };
    const auto refreshAccount = [](std::string clientId,
                                   std::string secret) {
        return bedrock::JsRuntimeValue::object({
            {"client_id", bedrock::JsRuntimeValue::string(std::move(clientId))},
            {"secret", bedrock::JsRuntimeValue::string(std::move(secret))}
        });
    };

    auto accessCache = bedrock::JsRuntimeValue::object({
        {"AccessToken", bedrock::JsRuntimeValue::object({
            {"z", account("manager-client-id", 99, "Z")},
            {"10", account("other", 99, "TEN")},
            {"2", account("manager-client-id", 12.3456, "TWO")},
            {"a", account("manager-client-id", 99, "A")}
        })}
    });
    cache->setGetCachedMethod([accessCache] {
        return bedrock::makeReadyAuthCacheFuture(accessCache);
    });
    try {
        const auto access = manager.getAccessToken().get();
        if (!access.isObject() ||
            !access.get("valid")->boolValue() ||
            access.get("until")->numberValue() != 1345.0 ||
            access.get("token")->stringValue() != "TWO") {
            std::cerr << "[FAIL] MsaTokenManager access selection/TimeClip mismatch\n";
            ok = false;
        }
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] MsaTokenManager getAccessToken rejected: "
                  << error.what() << "\n";
        ok = false;
    }

    auto fullFilterCache = bedrock::JsRuntimeValue::object({
        {"AccessToken", bedrock::JsRuntimeValue::object({
            {"0", account("manager-client-id", 99, "FIRST")},
            {"1", bedrock::JsRuntimeValue::null()}
        })}
    });
    cache->setGetCachedMethod([fullFilterCache] {
        return bedrock::makeReadyAuthCacheFuture(fullFilterCache);
    });
    bool fullFilterRejected = false;
    try {
        (void) manager.getAccessToken().get();
    } catch (const std::exception& error) {
        fullFilterRejected = std::string(error.what()) ==
            "Cannot read properties of null (reading 'client_id')";
    }
    if (!fullFilterRejected) {
        std::cerr << "[FAIL] MsaTokenManager filter stopped after first match\n";
        ok = false;
    }

    auto refreshCache = bedrock::JsRuntimeValue::object({
        {"RefreshToken", bedrock::JsRuntimeValue::object({
            {"b", refreshAccount("manager-client-id", "B")},
            {"3", refreshAccount("manager-client-id", "THREE")},
            {"1", refreshAccount("other", "ONE")}
        })}
    });
    cache->setGetCachedMethod([refreshCache] {
        return bedrock::makeReadyAuthCacheFuture(refreshCache);
    });
    try {
        const auto refresh = manager.getRefreshToken().get();
        if (!refresh.isObject() ||
            refresh.get("token")->stringValue() != "THREE") {
            std::cerr << "[FAIL] MsaTokenManager refresh selection order mismatch\n";
            ok = false;
        }
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] MsaTokenManager getRefreshToken rejected: "
                  << error.what() << "\n";
        ok = false;
    }

    std::vector<bedrock::JsRuntimeValue> verifyReads {
        refreshCache,
        bedrock::JsRuntimeValue::object({
            {"AccessToken", bedrock::JsRuntimeValue::object({
                {"token", account("manager-client-id", 99, "ACCESS")}
            })}
        }),
        refreshCache
    };
    std::size_t verifyReadIndex = 0;
    cache->setGetCachedMethod([&verifyReads, &verifyReadIndex] {
        if (verifyReadIndex >= verifyReads.size()) {
            throw std::runtime_error("unexpected verify cache read");
        }
        return bedrock::makeReadyAuthCacheFuture(
            verifyReads[verifyReadIndex++]
        );
    });
    int refreshCalls = 0;
    bedrock::JsRuntimeValue observedRefreshRequest;
    app->refresh = [queue, &refreshCalls, &observedRefreshRequest](
        bedrock::JsRuntimeValue request
    ) {
        ++refreshCalls;
        observedRefreshRequest = request;
        return bedrock::JsPromise<bedrock::JsRuntimeValue>::resolved(
            queue,
            bedrock::JsRuntimeValue::object({
                {"accessToken", bedrock::JsRuntimeValue::string("new")}
            })
        );
    };
    manager.forceRefresh = bedrock::JsRuntimeValue::string("false");
    try {
        const auto verified = manager.verifyTokens().get();
        if (!verified.isBool() || !verified.boolValue() ||
            refreshCalls != 1 || verifyReadIndex != 3 ||
            !observedRefreshRequest.get("refreshToken") ||
            observedRefreshRequest.get("refreshToken")->stringValue() !=
                "THREE" ||
            !observedRefreshRequest.get("scopes")->sharesIdentityWith(
                manager.scopes
            )) {
            std::cerr << "[FAIL] MsaTokenManager forceRefresh/request mismatch\n";
            ok = false;
        }
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] MsaTokenManager verifyTokens rejected: "
                  << error.what() << "\n";
        ok = false;
    }

    auto deviceCacheDocument = bedrock::JsRuntimeValue::object();
    bedrock::JsRuntimeValue deviceWrite;
    std::atomic<int> ignoredDeviceWriteRejections {0};
    cache->setGetCachedMethod([deviceCacheDocument] {
        return bedrock::makeReadyAuthCacheFuture(deviceCacheDocument);
    });
    cache->setSetCachedPartialMethod([&deviceWrite](
        bedrock::AuthCacheValue value
    ) {
        deviceWrite = value;
        return bedrock::makeRejectedAuthCacheFuture<void>(
            "ignored device write"
        );
    });
    bedrock::MsaTokenManagerObservers deviceObservers = observers;
    deviceObservers.unhandledRejection =
        [&ignoredDeviceWriteRejections](std::exception_ptr) {
            ++ignoredDeviceWriteRejections;
        };
    auto deviceConfig = bedrock::makeMsalConfig("manager-client-id");
    auto deviceApp = std::make_shared<ScriptedMsalPublicClientApplication>();
    bedrock::JsRuntimeValue observedDeviceRequest;
    auto deviceResponse = bedrock::JsRuntimeValue::object({
        {"accessToken", bedrock::JsRuntimeValue::string("device-access")},
        {"account", bedrock::JsRuntimeValue::object({
            {"username", bedrock::JsRuntimeValue::string("player")}
        })}
    });
    auto callbackResponse = bedrock::JsRuntimeValue::object({
        {"message", bedrock::JsRuntimeValue::string("visit")}
    });
    deviceApp->device = [
        queue,
        &observedDeviceRequest,
        callbackResponse,
        deviceResponse
    ](bedrock::JsRuntimeValue request) {
        observedDeviceRequest = request;
        auto* callback = request.get("deviceCodeCallback");
        callback->call<void(bedrock::JsRuntimeValue)>(callbackResponse);
        return bedrock::JsPromise<bedrock::JsRuntimeValue>::resolved(
            queue,
            deviceResponse
        );
    };
    bedrock::MsaTokenManager deviceManager(
        deviceConfig,
        scopes,
        cache,
        [deviceApp](const auto&) { return deviceApp; },
        queue,
        deviceObservers
    );
    bedrock::JsRuntimeValue callbackSeen;
    try {
        const auto result = deviceManager.authDeviceCode(
            [&callbackSeen](const bedrock::JsRuntimeValue& response) {
                callbackSeen = response;
            }
        ).get();
        const auto* callback = observedDeviceRequest.get("deviceCodeCallback");
        if (!result.sharesIdentityWith(deviceResponse) ||
            !callbackSeen.sharesIdentityWith(callbackResponse) ||
            !observedDeviceRequest.get("scopes")->sharesIdentityWith(scopes) ||
            !callback || callback->get("name")->stringValue() !=
                "deviceCodeCallback" ||
            callback->get("length")->numberValue() != 1 ||
            !deviceWrite.sharesIdentityWith(deviceCacheDocument) ||
            !deviceCacheDocument.get("Account") ||
            !deviceCacheDocument.get("Account")->get("") ||
            !deviceCacheDocument.get("Account")->get("")->sharesIdentityWith(
                *deviceResponse.get("account")
            )) {
            std::cerr << "[FAIL] MsaTokenManager authDeviceCode shape/write mismatch\n";
            ok = false;
        }
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] MsaTokenManager authDeviceCode rejected: "
                  << error.what() << "\n";
        ok = false;
    }
    for (int attempt = 0;
         attempt < 100 && ignoredDeviceWriteRejections.load() == 0;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (ignoredDeviceWriteRejections.load() != 1) {
        std::cerr << "[FAIL] authDeviceCode awaited/hidden rejected write\n";
        ok = false;
    }

    return ok;
}

bool checkOfflineUuidGolden() {
    bool ok = true;

    if (bedrock::uuidFrom("Notch") !=
        "ce229d4c-d328-3d41-95ad-bb356a551668") {
        std::cerr << "[FAIL] uuidFrom URL-namespace UUID v3 golden mismatch\n";
        ok = false;
    }
    if (bedrock::uuidFrom("") !=
        "14cdb9b4-de01-3faa-aff5-65bc2f771745") {
        std::cerr << "[FAIL] uuidFrom empty-name UUID v3 golden mismatch\n";
        ok = false;
    }

    return ok;
}

bool checkFileAuthCacheGolden() {
    bool ok = true;

    if (bedrock::FileAuthCache::usernameHash("") != "da39a3" ||
        bedrock::FileAuthCache::usernameHash("alice@example.com") !=
            "fc2398" ||
        bedrock::FileAuthCache::cacheFileName("msal", "alice@example.com") !=
            "fc2398_msal-cache.json") {
        std::cerr << "[FAIL] FileCache filename/hash golden mismatch\n";
        ok = false;
    }

    const auto nonce = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("bedrock-file-auth-cache-" + std::to_string(nonce));
    std::error_code filesystemError;
    std::filesystem::create_directories(root, filesystemError);
    if (filesystemError) {
        std::cerr << "[FAIL] could not create isolated FileCache test root\n";
        return false;
    }

    try {
        const bedrock::AuthCacheFactoryOptions options {
            .cacheName = "live",
            .username = "alice@example.com"
        };
        bedrock::FileAuthCache cache(root, options);
        const auto expectedLocation = root / "fc2398_live-cache.json";
        auto initial = cache.getCached().get();
        if (cache.cacheLocation() != expectedLocation ||
            !initial.isObject() || !initial.objectNode()->empty() ||
            !std::filesystem::exists(expectedLocation)) {
            std::cerr << "[FAIL] FileCache missing-file reset/lazy load mismatch\n";
            ok = false;
        }

        cache.setCached(bedrock::JsRuntimeValue::object({
            {"first", bedrock::JsRuntimeValue::number(1)}
        })).get();
        cache.setCachedPartial(bedrock::JsRuntimeValue::object({
            {"second", bedrock::JsRuntimeValue::string("two")}
        })).get();
        const auto merged = cache.getCached().get();
        const auto* first = merged.get("first");
        const auto* second = merged.get("second");
        if (!first || !first->isNumber() || first->numberValue() != 1 ||
            !second || !second->isString() ||
            second->stringValue() != "two") {
            std::cerr << "[FAIL] FileCache shallow partial merge mismatch\n";
            ok = false;
        }

        const auto resetResult = cache.reset().get();
        const auto staleAfterReset = cache.getCached().get();
        bedrock::FileAuthCache freshAfterReset(root, options);
        const auto diskAfterReset = freshAfterReset.getCached().get();
        if (!resetResult.isObject() || !resetResult.objectNode()->empty() ||
            !staleAfterReset.get("first") ||
            !diskAfterReset.isObject() ||
            !diskAfterReset.objectNode()->empty()) {
            std::cerr << "[FAIL] FileCache reset memory/disk behavior mismatch\n";
            ok = false;
        }

        freshAfterReset.setCached(bedrock::JsRuntimeValue::object({
            {"diskOnly", bedrock::JsRuntimeValue::boolean(true)}
        })).get();
        bedrock::FileAuthCache partialBeforeRead(root, options);
        partialBeforeRead.setCachedPartial(bedrock::JsRuntimeValue::object({
            {"newOnly", bedrock::JsRuntimeValue::boolean(true)}
        })).get();
        bedrock::FileAuthCache verifyPartialBeforeRead(root, options);
        const auto partialDisk = verifyPartialBeforeRead.getCached().get();
        if (partialDisk.get("diskOnly") || !partialDisk.get("newOnly")) {
            std::cerr << "[FAIL] FileCache partial-before-read loaded disk state\n";
            ok = false;
        }

        {
            std::ofstream invalid(expectedLocation, std::ios::trunc);
            invalid << "not json";
        }
        bedrock::FileAuthCache invalidJson(root, options);
        const auto recovered = invalidJson.getCached().get();
        if (!recovered.isObject() || !recovered.objectNode()->empty()) {
            std::cerr << "[FAIL] FileCache invalid JSON recovery mismatch\n";
            ok = false;
        }
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] FileCache golden threw: " << error.what() << "\n";
        ok = false;
    }

    std::filesystem::remove_all(root, filesystemError);
    if (filesystemError) {
        std::cerr << "[FAIL] could not remove isolated FileCache test root\n";
        ok = false;
    }
    return ok;
}

bool checkAuthCacheFactoryGolden() {
    bool ok = true;

    {
        std::vector<std::string> calls;
        std::vector<bedrock::AuthCachePtr> returned;
        auto config = bedrock::makeMsalConfig("factory-client-id");
        auto factory = std::make_shared<bedrock::AuthCacheFactory>(
            [&](bedrock::AuthCacheFactoryOptions options) {
                calls.push_back(options.cacheName + ":" + options.username);
                if (options.cacheName == "msal" && config->get("cache")) {
                    ok = false;
                    std::cerr << "[FAIL] msalConfig mutated before msal cache factory\n";
                }
                if (options.cacheName == "xbl" && !config->get("cache")) {
                    ok = false;
                    std::cerr << "[FAIL] msalConfig mutation was late for xbl factory\n";
                }
                auto cache = std::make_shared<bedrock::AuthCache>();
                returned.push_back(cache);
                return cache;
            }
        );

        bedrock::BedrockNetworkClientOptions options;
        options.host = "127.0.0.1";
        options.port = 9;
        options.username = "CacheFactoryUser";
        options.profile = "CacheFactoryUser";
        options.offline = false;
        options.authTitle = "";
        options.flow = "msal";
        options.msalConfig = config;
        options.profilesFolder = factory;

        bedrock::BedrockNetworkClient client(std::move(options));
        bedrock::BedrockNetworkClientTestAccess::setBeforeQueueStartHook(
            client,
            []() { throw std::runtime_error("stop after cache factories"); }
        );
        bool exactHookError = false;
        try {
            (void) client.prepareConnectLifecycle(false);
        } catch (const std::exception& error) {
            exactHookError = std::string(error.what()) ==
                "stop after cache factories";
        }

        const std::vector<std::string> expected {
            "msal:CacheFactoryUser",
            "xbl:CacheFactoryUser",
            "bed:CacheFactoryUser",
            "mca:CacheFactoryUser",
            "mcs:CacheFactoryUser",
            "pfb:CacheFactoryUser"
        };
        if (!exactHookError || calls != expected || returned.size() != 6 ||
            !client.options().profilesFolder.isFactory() ||
            client.options().profilesFolder.factory() != factory ||
            !bedrock::BedrockNetworkClientTestAccess::
                effectiveAuthenticationCacheRoot(client).empty() ||
            !bedrock::BedrockNetworkClientTestAccess::
                hasAuthenticationXboxProofKey(client)) {
            std::cerr << "[FAIL] CacheFactory identity/order/path mismatch\n";
            ok = false;
        }
        const std::array<const char*, 6> cacheNames {
            "msal", "xbl", "bed", "mca", "mcs", "pfb"
        };
        for (std::size_t index = 0; index < returned.size(); ++index) {
            const auto* name = cacheNames[index];
            if (bedrock::BedrockNetworkClientTestAccess::authenticationCache(
                    client,
                    name
                ) != returned[index]) {
                std::cerr << "[FAIL] CacheFactory return identity was not retained\n";
                ok = false;
            }
        }
        client.close();
    }

    const auto runRejected = [&ok](
        bool factoryThrows,
        const std::string& expectedError
    ) {
        std::vector<std::string> calls;
        auto malformed = std::make_shared<bedrock::MsalConfig>(
            bedrock::MsalConfig::object({})
        );
        auto factory = std::make_shared<bedrock::AuthCacheFactory>(
            [&](bedrock::AuthCacheFactoryOptions options) {
                calls.push_back(options.cacheName);
                if (factoryThrows) {
                    throw std::runtime_error("cache factory boom");
                }
                return std::make_shared<bedrock::AuthCache>();
            }
        );

        bedrock::BedrockNetworkClientOptions options;
        options.host = "127.0.0.1";
        options.port = 9;
        options.username = "CacheFactoryFailure";
        options.profile = "CacheFactoryFailure";
        options.offline = false;
        options.authTitle = "";
        options.flow = "msal";
        options.msalConfig = malformed;
        options.profilesFolder = factory;

        bedrock::BedrockNetworkClient client(std::move(options));
        std::string observedError;
        client.onError([&](const std::string& error) {
            observedError = error;
        });
        const bool prepared = client.prepareConnectLifecycle(false);
        if (!prepared || observedError != expectedError ||
            calls != std::vector<std::string> {"msal"} ||
            malformed->get("cache") ||
            !bedrock::BedrockNetworkClientTestAccess::queueRunning(client)) {
            std::cerr << "[FAIL] CacheFactory/config error precedence mismatch\n";
            ok = false;
        }
        client.close();
    };

    runRejected(true, "cache factory boom");
    runRejected(
        false,
        "Cannot read properties of undefined (reading 'clientId')"
    );

    {
        std::size_t calls = 0;
        auto factory = std::make_shared<bedrock::AuthCacheFactory>(
            [&](bedrock::AuthCacheFactoryOptions) {
                ++calls;
                return std::make_shared<bedrock::AuthCache>();
            }
        );
        bedrock::BedrockNetworkClientOptions options;
        options.host = "127.0.0.1";
        options.port = 9;
        options.username = "InvalidBeforeFactory";
        options.profile = "InvalidBeforeFactory";
        options.offline = false;
        options.authTitle = "";
        options.flow = "live";
        options.profilesFolder = factory;
        bedrock::BedrockNetworkClient client(std::move(options));
        client.onError([](const std::string&) {});
        (void) client.prepareConnectLifecycle(false);
        if (calls != 0) {
            std::cerr << "[FAIL] invalid live flow invoked CacheFactory\n";
            ok = false;
        }
        client.close();
    }

    {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        const auto root = std::filesystem::temp_directory_path() /
            ("bedrock-built-in-auth-cache-" + std::to_string(nonce));
        std::error_code filesystemError;
        std::filesystem::create_directories(root, filesystemError);
        if (filesystemError) {
            std::cerr << "[FAIL] could not create built-in cache test root\n";
            ok = false;
        } else {
            const std::array<const char*, 6> names {
                "msal", "xbl", "bed", "mca", "mcs", "pfb"
            };
            for (const char* name : names) {
                bedrock::FileAuthCache seed(
                    root,
                    bedrock::AuthCacheFactoryOptions {
                        .cacheName = name,
                        .username = "BuiltInCacheUser"
                    }
                );
                seed.setCached(bedrock::JsRuntimeValue::object({
                    {"stale", bedrock::JsRuntimeValue::boolean(true)}
                })).get();
            }

            bedrock::BedrockNetworkClientOptions options;
            options.host = "127.0.0.1";
            options.port = 9;
            options.username = "BuiltInCacheUser";
            options.profile = "BuiltInCacheUser";
            options.offline = false;
            options.authTitle = "built-in-msal-client-id";
            options.flow = "msal";
            options.forceRefresh = true;
            options.profilesFolder = root;

            bedrock::BedrockNetworkClient client(std::move(options));
            bedrock::BedrockNetworkClientTestAccess::setBeforeQueueStartHook(
                client,
                []() { throw std::runtime_error("stop after built-in caches"); }
            );
            bool exactHookError = false;
            try {
                (void) client.prepareConnectLifecycle(false);
            } catch (const std::exception& error) {
                exactHookError = std::string(error.what()) ==
                    "stop after built-in caches";
            }

            if (!exactHookError ||
                bedrock::BedrockNetworkClientTestAccess::
                    effectiveAuthenticationCacheRoot(client) != root) {
                std::cerr << "[FAIL] built-in FileCache root/order boundary mismatch\n";
                ok = false;
            }
            for (const char* name : names) {
                const auto cache = bedrock::BedrockNetworkClientTestAccess::
                    authenticationCache(client, name);
                const auto fileCache =
                    std::dynamic_pointer_cast<bedrock::FileAuthCache>(cache);
                if (!fileCache || fileCache->cacheLocation() !=
                        bedrock::FileAuthCache::cacheLocationFor(
                            root,
                            name,
                            "BuiltInCacheUser"
                        )) {
                    std::cerr << "[FAIL] built-in FileCache identity/path mismatch\n";
                    ok = false;
                    continue;
                }
                bedrock::FileAuthCache fromDisk(
                    root,
                    bedrock::AuthCacheFactoryOptions {
                        .cacheName = name,
                        .username = "BuiltInCacheUser"
                    }
                );
                const auto value = fromDisk.getCached().get();
                if (!value.isObject() || !value.objectNode()->empty()) {
                    std::cerr << "[FAIL] forceRefresh did not reset FileCache\n";
                    ok = false;
                }
            }
            client.close();
            std::filesystem::remove_all(root, filesystemError);
            if (filesystemError) {
                std::cerr << "[FAIL] could not remove built-in cache test root\n";
                ok = false;
            }
        }
    }

    return ok;
}

bool checkOnlineAuthBoundaryGolden() {
    bool ok = true;

    {
        bedrock::Options options;
        options.host = "127.0.0.1";
        options.port = 9;
        options.username = "AuthBoundary";
        options.offline = false;
        options.authTitle = std::string(bedrock::Titles::MinecraftIOS);
        options.flow = "";
        options.profilesFolder = true;
        options.conLog = {};

        bedrock::Client client(std::move(options));
        std::vector<std::string> order;
        std::size_t errorCount = 0;
        bool nestedResult = true;

        bedrock::BedrockNetworkClientTestAccess::setBeforeQueueStartHook(
            client.network(),
            [&]() { order.push_back("queue"); }
        );
        client.onError([&](const std::string& message) {
            if (message !=
                "Missing 'flow' argument in options. See docs for more information.") {
                ok = false;
                std::cerr << "[FAIL] reentrant auth emitted the wrong error\n";
            }
            order.push_back("error");
            if (errorCount++ == 0) {
                // createClient's own connect_allowed listener sets this before
                // authentication. Reproduce that state at the exact callback
                // boundary so the facade must not take its normal no-op path.
                bedrock::ClientFactoryTestAccess::setAutoConnectStarted(client);
                nestedResult = client.connect();
                order.push_back("inner-return");
            }
        });

        const bool outerResult = client.connect();
        order.push_back("outer-return");

        // This was captured directly from node_modules/bedrock-protocol:
        // authenticate() emits its constructor error synchronously, the error
        // listener's nested connect performs auth + startQueue, and only then
        // does the outer connect reach its own startQueue.
        const std::vector<std::string> expectedOrder {
            "error",
            "error",
            "queue",
            "inner-return",
            "queue",
            "outer-return"
        };
        if (order != expectedOrder || errorCount != 2 || nestedResult ||
            outerResult ||
            !bedrock::BedrockNetworkClientTestAccess::queueRunning(
                client.network()
            ) ||
            bedrock::BedrockNetworkClientTestAccess::hasTransport(
                client.network()
            )) {
            std::cerr << "[FAIL] reentrant invalid-auth connect order mismatch\n";
            ok = false;
        }
        client.close();
    }

    const auto checkRejected = [&ok](
        const std::string& label,
        std::optional<std::string> authTitle,
        std::string flow,
        const std::string& expectedError,
        bool throwingErrorListener,
        bool cacheInitializationExpected
    ) {
        bedrock::BedrockNetworkClientOptions options;
        options.host = "127.0.0.1";
        options.port = 9;
        options.profile = "AuthBoundary";
        options.offline = false;
        options.authTitle = std::move(authTitle);
        options.deviceType = "ExplicitDevice";
        options.flow = std::move(flow);
        options.profilesFolder = true;
        options.authCacheRoot = "cache-before-constructor";

        bedrock::BedrockNetworkClient client(std::move(options));
        std::vector<std::string> order;
        std::optional<bedrock::BedrockNetworkClientOptions> resolved;
        std::string observedError;
        std::string unhandledRejection;
        std::atomic<bool> returnedPublished {false};
        std::atomic<bool> unhandledDelivered {false};
        std::atomic<bool> unhandledBeforeReturn {false};
        bool queueWasRunningInsideError = true;

        client.setAuthenticationOptionsResolvedHandler(
            [&](bedrock::BedrockNetworkClientOptions snapshot) {
                order.push_back("options");
                resolved = std::move(snapshot);
            }
        );
        client.setAuthenticationUnhandledRejectionHandler(
            [&](std::string message) {
                if (!returnedPublished.load()) unhandledBeforeReturn = true;
                unhandledRejection = std::move(message);
                unhandledDelivered = true;
            }
        );
        client.onError([&](const std::string& message) {
            order.push_back("error");
            observedError = message;
            queueWasRunningInsideError =
                bedrock::BedrockNetworkClientTestAccess::queueRunning(client);
            const auto visible = client.options();
            if (!visible.profilesFolder.isBoolean() ||
                !visible.profilesFolder.booleanValue()) {
                ok = false;
                std::cerr << "[FAIL] " << label
                          << " error listener did not observe profilesFolder=true\n";
            }
            if (throwingErrorListener) {
                throw std::runtime_error("auth error listener boom");
            }
        });
        bedrock::BedrockNetworkClientTestAccess::setBeforeQueueStartHook(
            client,
            [&]() { order.push_back("before-queue"); }
        );

        bool prepared = false;
        try {
            prepared = client.prepareConnectLifecycle(false);
            order.push_back("returned");
            returnedPublished = true;
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << label
                      << " auth rejection escaped connect(): "
                      << error.what() << "\n";
            ok = false;
        }

        const auto afterPrepare = client.options();
        const auto effectiveCacheRoot =
            bedrock::BedrockNetworkClientTestAccess::
                effectiveAuthenticationCacheRoot(client);
        const bool connected = client.connect();
        const bool queueRunningAfterConnect =
            bedrock::BedrockNetworkClientTestAccess::queueRunning(client);
        const bool hasTransport =
            bedrock::BedrockNetworkClientTestAccess::hasTransport(client);

        const std::vector<std::string> expectedOrder {
            "options", "error", "before-queue", "returned"
        };
        if (!prepared || observedError != expectedError ||
            queueWasRunningInsideError || order != expectedOrder ||
            !resolved.has_value() || connected || !queueRunningAfterConnect ||
            hasTransport || !afterPrepare.profilesFolder.isBoolean() ||
            !afterPrepare.profilesFolder.booleanValue() ||
            resolved->authCacheRoot !=
                std::filesystem::path("cache-before-constructor") ||
            afterPrepare.authCacheRoot !=
                std::filesystem::path("cache-before-constructor")) {
            std::cerr << "[FAIL] " << label
                      << " auth error/queue/transport ordering mismatch\n";
            ok = false;
        }

        if (throwingErrorListener) {
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(250);
            while (!unhandledDelivered.load() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!unhandledDelivered.load() || unhandledBeforeReturn.load() ||
                unhandledRejection != "auth error listener boom") {
                std::cerr << "[FAIL] " << label
                          << " ignored-Promise rejection timing mismatch\n";
                ok = false;
            }
        } else if (!unhandledRejection.empty()) {
            std::cerr << "[FAIL] " << label
                      << " reported a handled error as unhandled\n";
            ok = false;
        }

        if (cacheInitializationExpected) {
            if (effectiveCacheRoot.filename() != "src" ||
                effectiveCacheRoot.parent_path().filename() !=
                    "prismarine-auth") {
                std::cerr << "[FAIL] " << label
                          << " did not use prismarine-auth/src cache fallback\n";
                ok = false;
            }
        } else if (!effectiveCacheRoot.empty()) {
            std::cerr << "[FAIL] " << label
                      << " initialized cache before missing-flow validation\n";
            ok = false;
        }

        client.close();
    };

    checkRejected(
        "missing flow",
        std::string(bedrock::Titles::MinecraftIOS),
        "",
        "Missing 'flow' argument in options. See docs for more information.",
        true,
        false
    );

    {
        auto supplied = bedrock::makeMsalConfig(
            "supplied-client-id",
            "https://login.example.test/tenant"
        );
        bedrock::BedrockNetworkClientOptions options;
        options.host = "127.0.0.1";
        options.port = 9;
        options.profile = "MsalConfigBoundary";
        options.offline = false;
        options.authTitle = nullptr;
        options.deviceType = "NullTitleDevice";
        options.flow = "msal";
        options.msalConfig = supplied;
        options.profilesFolder = true;

        bedrock::BedrockNetworkClient client(std::move(options));
        bool exactHookError = false;
        bedrock::BedrockNetworkClientTestAccess::setBeforeQueueStartHook(
            client,
            []() { throw std::runtime_error("stop after msal constructor"); }
        );
        try {
            (void) client.prepareConnectLifecycle(false);
        } catch (const std::exception& error) {
            exactHookError = std::string(error.what()) ==
                "stop after msal constructor";
        }
        const auto visible = client.options();
        const auto effective = bedrock::BedrockNetworkClientTestAccess::
            effectiveMsalConfig(client);
        if (!exactHookError || !visible.authTitle.isNull() ||
            visible.flow != "msal" || visible.deviceType != "NullTitleDevice" ||
            visible.msalConfig != supplied || effective != supplied ||
            !supplied->get("cache") ||
            !supplied->get("cache")->get("cachePlugin") ||
            bedrock::BedrockNetworkClientTestAccess::queueRunning(client)) {
            std::cerr << "[FAIL] supplied msalConfig/null authTitle boundary mismatch\n";
            ok = false;
        }
        client.close();
    }
    checkRejected(
        "live empty authTitle",
        std::string(""),
        "live",
        "Please specify an \"authTitle\" in Authflow constructor when using live flow",
        false,
        true
    );
    checkRejected(
        "sisu empty authTitle",
        std::string(""),
        "sisu",
        "Please specify an \"authTitle\" in Authflow constructor when using sisu flow",
        false,
        true
    );
    checkRejected(
        "unknown flow",
        std::string(bedrock::Titles::MinecraftIOS),
        "unknown",
        "Unknown flow: unknown (expected \"live\", \"sisu\", or \"msal\")",
        false,
        true
    );
    checkRejected(
        "msal empty authTitle",
        std::string(""),
        "msal",
        "Must specify an Azure client ID token inside the `authTitle` parameter "
        "when using Azure-based auth. See "
        "https://learn.microsoft.com/en-us/entra/identity-platform/"
        "quickstart-register-app#register-an-application for more information "
        "on obtaining an Azure token.",
        false,
        true
    );

    {
        bedrock::BedrockNetworkClientOptions options;
        options.host = "127.0.0.1";
        options.port = 9;
        options.profile = "AuthBoundary";
        options.offline = false;
        options.authTitle.reset();
        options.deviceType = "must-be-overwritten";
        options.flow = "must-be-overwritten";
        options.profilesFolder = true;
        options.authCacheRoot = "must-be-overridden";

        bedrock::BedrockNetworkClient client(std::move(options));
        bool optionsPublished = false;
        std::optional<bedrock::BedrockNetworkClientOptions> published;
        client.setAuthenticationOptionsResolvedHandler(
            [&](bedrock::BedrockNetworkClientOptions snapshot) {
                optionsPublished = true;
                published = std::move(snapshot);
            }
        );
        bedrock::BedrockNetworkClientTestAccess::setBeforeQueueStartHook(
            client,
            [&]() {
                if (!optionsPublished) {
                    throw std::runtime_error("options callback was late");
                }
                throw std::runtime_error("auth boundary stop");
            }
        );

        bool exactHookError = false;
        try {
            (void) client.prepareConnectLifecycle(false);
        } catch (const std::exception& error) {
            exactHookError = std::string(error.what()) ==
                "auth boundary stop";
        }
        const auto visible = client.options();
        const auto effectiveCacheRoot =
            bedrock::BedrockNetworkClientTestAccess::
                effectiveAuthenticationCacheRoot(client);
        if (!exactHookError || !published.has_value() ||
            published->authCacheRoot !=
                std::filesystem::path("must-be-overridden") ||
            visible.authTitle != std::optional<std::string>(
                std::string(bedrock::Titles::MinecraftNintendoSwitch)) ||
            visible.deviceType != "Nintendo" || visible.flow != "live" ||
            !visible.profilesFolder.isBoolean() ||
            !visible.profilesFolder.booleanValue() ||
            visible.authCacheRoot !=
                std::filesystem::path("must-be-overridden") ||
            effectiveCacheRoot.filename() != "src" ||
            effectiveCacheRoot.parent_path().filename() !=
                "prismarine-auth" ||
            !bedrock::BedrockNetworkClientTestAccess::connectLifecycleIdle(client) ||
            bedrock::BedrockNetworkClientTestAccess::queueRunning(client)) {
            std::cerr << "[FAIL] valid auth option mutation/callback boundary mismatch\n";
            ok = false;
        }
        client.close();
    }

    return ok;
}

bool checkRelayOptionsGolden() {
    bool ok = true;

    {
        bedrock::BedrockLiveRelayOptions liveOptions;
        liveOptions.upstream.username = "RelayBot";
        liveOptions.upstream.profile = "RelayBot";
        liveOptions.useDownstreamDisplayNameForUpstreamUsername = false;
        bedrock::detail::applyRelayDownstreamIdentity(
            liveOptions,
            bedrock::BedrockRelayDownstreamProfile{"DisplayName", "XUID", "identity"}
        );
        if (liveOptions.upstream.username != "XUID" ||
            liveOptions.upstream.profile != "XUID") {
            std::cerr << "[FAIL] relay XUID auth identity mapping mismatch\n";
            ok = false;
        }

        liveOptions.useDownstreamDisplayNameForUpstreamUsername = true;
        bedrock::detail::applyRelayDownstreamIdentity(
            liveOptions,
            bedrock::BedrockRelayDownstreamProfile{}
        );
        if (!liveOptions.upstream.username.empty() ||
            !liveOptions.upstream.profile.empty()) {
            std::cerr << "[FAIL] relay empty displayName did not replace prior auth identity\n";
            ok = false;
        }
    }

    {
        bedrock::Relay relay(bedrock::RelayOptions{});
        const auto& relayOptions = relay.options();
        const auto& liveOptions = relay.live().options();
        if (relayOptions.destination.offline.has_value() ||
            liveOptions.server.compressionAlgorithm != "deflate" ||
            liveOptions.server.compressionLevel != 7 ||
            liveOptions.server.compressionThreshold != 512 ||
            liveOptions.upstream.offline ||
            liveOptions.upstream.chunkRadius != 10 ||
            liveOptions.upstream.batchingIntervalMs != 20 ||
            liveOptions.clientboundCompression !=
                bedrock::VersionedMcpeCompression::Automatic) {
            std::cerr << "[FAIL] relay inherited defaults mismatch\n";
            ok = false;
        }

        bedrock::BedrockNetworkClient normalized(liveOptions.upstream);
        const auto upstream = normalized.options();
        if (upstream.authTitle.has_value() ||
            !upstream.deviceType.empty() || !upstream.flow.empty()) {
            std::cerr << "[FAIL] relay auth defaults ran at construction\n";
            ok = false;
        }
    }

    {
        bedrock::RelayOptions options;
        options.offline = true;
        options.batchingInterval = 37;
        options.authTitle = std::string(bedrock::Titles::MinecraftAndroid);
        options.deviceType = "Android";
        options.flow = "sisu";
        bedrock::Relay relay(std::move(options));
        const auto& liveOptions = relay.live().options();
        if (!liveOptions.server.offline ||
            !liveOptions.upstream.offline ||
            !liveOptions.useDownstreamDisplayNameForUpstreamUsername ||
            liveOptions.upstream.batchingIntervalMs != 37 ||
            liveOptions.upstream.authTitle !=
                std::optional<std::string>(bedrock::Titles::MinecraftAndroid) ||
            liveOptions.upstream.deviceType != "Android" ||
            liveOptions.upstream.flow != "sisu") {
            std::cerr << "[FAIL] relay omitted destination offline/auth propagation mismatch\n";
            ok = false;
        }

        bedrock::BedrockNetworkClient normalized(liveOptions.upstream);
        const auto upstream = normalized.options();
        if (!upstream.authTitle.has_value() ||
            *upstream.authTitle != bedrock::Titles::MinecraftAndroid ||
            upstream.deviceType != "Android" || upstream.flow != "sisu") {
            std::cerr << "[FAIL] relay explicit auth flow normalization mismatch\n";
            ok = false;
        }
    }

    {
        bedrock::RelayOptions options;
        options.offline = true;
        options.destination.offline = false;
        bedrock::Relay relay(std::move(options));
        const auto& liveOptions = relay.live().options();
        if (liveOptions.upstream.offline ||
            !liveOptions.useDownstreamDisplayNameForUpstreamUsername) {
            std::cerr << "[FAIL] relay auth offline=false/displayName selection mismatch\n";
            ok = false;
        }
    }

    {
        bedrock::RelayOptions options;
        options.offline = false;
        options.destination.offline = true;
        options.authTitle = std::string(bedrock::Titles::MinecraftIOS);
        bedrock::Relay relay(std::move(options));
        const auto& liveOptions = relay.live().options();
        if (liveOptions.server.offline ||
            !liveOptions.upstream.offline ||
            liveOptions.useDownstreamDisplayNameForUpstreamUsername) {
            std::cerr << "[FAIL] relay auth offline=true/XUID selection mismatch\n";
            ok = false;
        }

        bedrock::BedrockNetworkClient normalized(liveOptions.upstream);
        const auto upstream = normalized.options();
        if (!upstream.authTitle.has_value() ||
            *upstream.authTitle != bedrock::Titles::MinecraftIOS ||
            !upstream.deviceType.empty() || !upstream.flow.empty()) {
            std::cerr << "[FAIL] relay explicit authTitle received default flow values\n";
            ok = false;
        }
    }

    {
        bedrock::RelayOptions options;
        options.deviceType = "Android";
        options.flow = "sisu";
        bedrock::Relay relay(std::move(options));
        bedrock::BedrockNetworkClient normalized(relay.live().options().upstream);
        const auto upstream = normalized.options();
        if (upstream.authTitle.has_value() ||
            upstream.deviceType != "Android" || upstream.flow != "sisu") {
            std::cerr << "[FAIL] relay omitted authTitle mutated before auth boundary\n";
            ok = false;
        }
    }

    return ok;
}

bool checkServerAdvertisementGolden() {
    bool ok = true;
    const std::string expected =
        "MCPE;N;622;1.20.40;2;10;42;L;Creative;1;19132;19132;0;";

    bedrock::ServerAdvertisement advertisement({
        {"motd", "N"},
        {"playersOnline", 2},
        {"playersMax", 10},
        {"serverId", "42"},
        {"levelName", "L"},
        {"gamemode", "Creative"},
        {"gamemodeId", 1}
    }, 19132, "1.20.40");

    if (advertisement.toString() != expected) {
        std::cerr << "[FAIL] ServerAdvertisement JS string mismatch\n";
        ok = false;
    }

    auto buffer = advertisement.toBuffer();
    std::vector<uint8_t> expectedBuffer {0x00, 0x36};
    expectedBuffer.insert(expectedBuffer.end(), expected.begin(), expected.end());
    if (!sameBytes(buffer, expectedBuffer)) {
        std::cerr << "[FAIL] ServerAdvertisement JS buffer mismatch\n";
        ok = false;
    }

    auto parsed = bedrock::fromServerName(expected);
    const bool parsedHeader = parsed.header == "MCPE";
    const bool parsedProtocol = parsed.protocol == "622";
    const bool parsedOnline = parsed.playersOnline == 2;
    const bool parsedMax = parsed.playersMax == 10;
    const bool parsedV4 = parsed.portV4 == 19132;
    const bool parsedV6 = parsed.portV6 == 19132;
    const bool parsedString = parsed.toString() == expected;
    if (!parsedHeader || !parsedProtocol || !parsedOnline || !parsedMax ||
        !parsedV4 || !parsedV6 || !parsedString) {
        std::cerr << "[FAIL] ServerAdvertisement JS parse mismatch\n";
        ok = false;
    }

    auto emptyNumeric = bedrock::fromServerName(
        "MCPE;;924;1.26.0;;;id;;;;;;"
    );
    if (!emptyNumeric.playersOnline.isNull() ||
        !emptyNumeric.playersMax.isNull() ||
        !emptyNumeric.gamemodeId.isNull() ||
        !emptyNumeric.portV4.isNull() ||
        !emptyNumeric.portV6.isNull() ||
        emptyNumeric.toString() != "MCPE;;924;1.26.0;;;id;;;;;;0;") {
        std::cerr << "[FAIL] ServerAdvertisement JS null/join mismatch\n";
        ok = false;
    }

    bedrock::ServerAdvertisement alias({{"name", "Alias"}}, 19132, "1.26.0");
    if (alias.motd != "Alias" || alias.protocol != 924 ||
        alias.version != "1.26.0") {
        std::cerr << "[FAIL] ServerAdvertisement name/default mismatch\n";
        ok = false;
    }

    return ok;
}

bool checkCompressionHeaderErrorsGolden() {
    const auto codec = bedrock::VersionedMcpeCodec::forVersion("1.20.61");
    bool ok = true;

    try {
        (void) codec.decodeCompressionPacket({0x01});
        std::cerr << "[FAIL] snappy compression header did not throw\n";
        ok = false;
    } catch (const std::exception& error) {
        if (std::string(error.what()) != "Snappy compression not implemented") {
            std::cerr << "[FAIL] snappy compression header error mismatch\n";
            ok = false;
        }
    }

    try {
        (void) codec.decodeCompressionPacket({0x02});
        std::cerr << "[FAIL] unknown compression header did not throw\n";
        ok = false;
    } catch (const std::exception& error) {
        if (std::string(error.what()) != "Unknown compression type 2") {
            std::cerr << "[FAIL] unknown compression header error mismatch\n";
            ok = false;
        }
    }

    return ok;
}

} // namespace

int main() {
    const std::vector<std::string> packetNames = {
        "play_status",
        "resource_pack_client_response",
        "client_cache_status",
        "request_chunk_radius",
        "set_local_player_as_initialized"
    };

    int checkedVersions = 0;
    int failures = 0;

    if (!checkProtoDefNativeHelpers()) {
        ++failures;
    }
    if (!checkRelayPacketApi()) {
        ++failures;
    }
    if (!checkClientLifecycleGolden()) {
        ++failures;
    }
    if (!checkVersionContractGolden()) {
        ++failures;
    }
    if (!checkAuthTitleGolden()) {
        ++failures;
    }
    if (!checkMsalConfigGolden()) {
        ++failures;
    }
    if (!checkMsalRequestBuilderGolden()) {
        ++failures;
    }
    if (!checkMsalSerializableTokenCacheGolden()) {
        ++failures;
    }
    if (!checkJsRuntimeDateMapGolden()) {
        ++failures;
    }
    if (!checkMsalErrorGolden()) {
        ++failures;
    }
    if (!checkMsalCachePluginGolden()) {
        ++failures;
    }
    if (!checkMsaTokenManagerGolden()) {
        ++failures;
    }
    if (!checkOfflineUuidGolden()) {
        ++failures;
    }
    if (!checkFileAuthCacheGolden()) {
        ++failures;
    }
    if (!checkAuthCacheFactoryGolden()) {
        ++failures;
    }
    if (!checkOnlineAuthBoundaryGolden()) {
        ++failures;
    }
    if (!checkRelayOptionsGolden()) {
        ++failures;
    }
    if (!checkServerAdvertisementGolden()) {
        ++failures;
    }
    if (!checkCompressionHeaderErrorsGolden()) {
        ++failures;
    }

    for (const auto& version : bedrock::ProtocolDefinition::versions()) {
        ++checkedVersions;
        bool ok = true;

        for (const auto& packetName : packetNames) {
            ok = checkPacketRoundtrip(version, packetName) && ok;
        }

        ok = checkBatchRoundtrip(version, false) && ok;
        ok = checkBatchRoundtrip(version, true) && ok;
        ok = checkSchemaEncodes(version) && ok;
        ok = checkRelayPipeline(version) && ok;

        if (!ok) {
            ++failures;
        }

        std::cout << "[ROUNDTRIP] " << version << " " << (ok ? "ok" : "fail") << "\n";
    }

    std::cout << "[ROUNDTRIP] checkedVersions=" << checkedVersions << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}
