#include <bedrock/BedrockFramer.hpp>
#include <bedrock/bedrock.hpp>
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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
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
