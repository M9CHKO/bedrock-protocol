#include <bedrock/BedrockFramer.hpp>
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
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

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
