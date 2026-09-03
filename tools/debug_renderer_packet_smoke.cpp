#include <bedrock/bedrock.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protocol/VersionedPacketCodec.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* kVersion = "1.21.100";
constexpr const char* kPacketName = "debug_renderer";
constexpr uint32_t kPacketId = 0xa4;
constexpr const char* kScriptDrawerPacketName = "server_script_debug_drawer";
constexpr uint32_t kScriptDrawerPacketId = 0x148;
constexpr uint64_t kScriptDrawerNetworkId = 0x4350450000000000ULL;
constexpr std::size_t kScriptDrawerBatchSize = 64;
constexpr std::size_t kLargeSchematicShapeCount = 1'290;

using Value = bedrock::ProtoDefValue;

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[DEBUG-RENDERER-PACKET-SMOKE] " << message << "\n";
    }
    return condition;
}

std::vector<uint8_t> unhex(const std::string& text) {
    auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };

    if ((text.size() & 1u) != 0u) {
        throw std::runtime_error("odd hex length");
    }

    std::vector<uint8_t> result;
    result.reserve(text.size() / 2u);
    for (std::size_t index = 0; index < text.size(); index += 2u) {
        const int high = digit(text[index]);
        const int low = digit(text[index + 1u]);
        if (high < 0 || low < 0) {
            throw std::runtime_error("invalid hex digit");
        }
        result.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return result;
}

std::string hex(const std::vector<uint8_t>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2u);
    for (const uint8_t byte : bytes) {
        result.push_back(digits[byte >> 4u]);
        result.push_back(digits[byte & 0x0fu]);
    }
    return result;
}

const bedrock::ProtoDefField* field(
    const std::vector<bedrock::ProtoDefField>& fields,
    const std::string& path
) {
    const auto found = std::find_if(
        fields.begin(),
        fields.end(),
        [&](const auto& item) { return item.path == path; }
    );
    return found == fields.end() ? nullptr : &*found;
}

bool checkField(
    const std::vector<bedrock::ProtoDefField>& fields,
    const std::string& path,
    const std::string& expectedValue
) {
    const auto* decoded = field(fields, path);
    return check(
        decoded && decoded->value == expectedValue,
        path + " decoded value mismatch"
    );
}

bool checkFloatField(
    const std::vector<bedrock::ProtoDefField>& fields,
    const std::string& path,
    double expectedValue
) {
    const auto* decoded = field(fields, path);
    if (!check(decoded != nullptr, path + " is missing from strict decode")) {
        return false;
    }

    try {
        return check(
            std::abs(std::stod(decoded->value) - expectedValue) < 0.000001,
            path + " decoded value mismatch"
        );
    } catch (const std::exception&) {
        return check(false, path + " is not a floating-point value");
    }
}

Value clearValue() {
    return Value::object({
        {"type", Value::string("clear")}
    });
}

Value addCubeValue() {
    return Value::object({
        {"type", Value::string("add_cube")},
        {"text", Value::string("cube")},
        {"position", Value::object({
            {"x", Value::floating(1.0)},
            {"y", Value::floating(2.0)},
            {"z", Value::floating(3.0)}
        })},
        {"red", Value::floating(0.0)},
        {"green", Value::floating(1.0)},
        {"blue", Value::floating(0.5)},
        {"alpha", Value::floating(0.25)},
        {"duration", Value::integer(5000)}
    });
}

Value scriptDrawerShapeValue(
    uint64_t networkId = kScriptDrawerNetworkId
) {
    return Value::object({
        {"network_id", Value::uinteger(networkId)},
        {"shape_type", Value::string("box")},
        {"location", Value::object({
            {"x", Value::floating(1.0)},
            {"y", Value::floating(2.0)},
            {"z", Value::floating(3.0)}
        })},
        {"scale", Value::floating(1.0)},
        {"rotation", Value::null()},
        {"time_left", Value::null()},
        // 0xAARRGGBB is serialised as B,G,R,A by the li32 field, matching
        // Bedrock's BEARGB representation.
        {"color", Value::integer(0x44332211)},
        {"text", Value::null()},
        {"box_bound", Value::object({
            {"x", Value::floating(0.5)},
            {"y", Value::floating(0.5)},
            {"z", Value::floating(0.5)}
        })},
        {"line_end_location", Value::null()},
        {"arrow_head_length", Value::null()},
        {"arrow_head_radius", Value::null()},
        {"segment_count", Value::null()}
    });
}

Value scriptDrawerValue(bool withShape) {
    std::vector<Value> shapes;
    if (withShape) shapes.push_back(scriptDrawerShapeValue());
    return Value::object({
        {"shapes", Value::array(std::move(shapes))}
    });
}

Value scriptDrawerRemovalShapeValue(uint64_t networkId) {
    return Value::object({
        {"network_id", Value::uinteger(networkId)},
        {"shape_type", Value::null()},
        {"location", Value::null()},
        {"scale", Value::null()},
        {"rotation", Value::null()},
        {"time_left", Value::null()},
        {"color", Value::null()},
        {"text", Value::null()},
        {"box_bound", Value::null()},
        {"line_end_location", Value::null()},
        {"arrow_head_length", Value::null()},
        {"arrow_head_radius", Value::null()},
        {"segment_count", Value::null()}
    });
}

Value scriptDrawerRemovalValue() {
    return Value::object({
        {"shapes", Value::array({
            scriptDrawerRemovalShapeValue(kScriptDrawerNetworkId)
        })}
    });
}

Value scriptDrawerRangeValue(
    std::size_t offset,
    std::size_t count,
    bool removals
) {
    std::vector<Value> shapes;
    shapes.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const uint64_t networkId =
            kScriptDrawerNetworkId + offset + index;
        shapes.push_back(
            removals
                ? scriptDrawerRemovalShapeValue(networkId)
                : scriptDrawerShapeValue(networkId)
        );
    }
    return Value::object({
        {"shapes", Value::array(std::move(shapes))}
    });
}

bool checkStructuredReencode(
    const bedrock::VersionedPacketCodec& codec,
    const bedrock::ProtoDefPacketEncoder& encoder,
    const std::vector<uint8_t>& goldenFullPacket,
    const std::string& label
) {
    auto packet = codec.decodeFullPacket(goldenFullPacket);
    bedrock::BedrockRelayPacketEvent rawEvent;
    rawEvent.packet = packet;
    bedrock::RelayPacketEvent event(kVersion, rawEvent, {}, true);

    const auto payload = encoder.encodePacket(
        packet.name,
        Value::object(event.decodedParams())
    );
    const auto reencoded = codec.encodeFullPacketByName(packet.name, payload);
    return check(
        reencoded == goldenFullPacket,
        label + " strict decode/re-encode changed bytes"
    );
}

bool checkClearPacket(
    const bedrock::VersionedPacketCodec& codec,
    const bedrock::ProtoDefPacketEncoder& encoder,
    const bedrock::ProtoDefPacketDecoder& decoder
) {
    // varuint packet id 0xa4, li32 action 1 (clear), no action payload.
    const auto golden = unhex("a40101000000");
    const auto payload = encoder.encodePacket(kPacketName, clearValue());
    const auto encoded = codec.makePacketByName(kPacketName, payload);

    bool ok = true;
    ok &= check(encoded.fullPacket == golden, "clear golden bytes mismatch");
    ok &= check(encoded.packetId == kPacketId, "clear packet id mismatch");

    const auto packet = codec.decodeFullPacket(golden);
    ok &= check(packet.packetId == kPacketId, "decoded clear packet id mismatch");
    ok &= check(packet.name == kPacketName, "decoded clear packet name mismatch");
    ok &= check(
        packet.paramsType == "packet_debug_renderer",
        "decoded clear params type mismatch"
    );
    ok &= check(packet.payload == payload, "decoded clear payload mismatch");

    try {
        const auto fields = decoder.decodePacketStrict(packet.name, packet.payload);
        ok &= check(fields.size() == 2u, "clear strict decode field count mismatch");
        ok &= checkField(fields, "type", "1/clear");
        ok &= checkField(fields, "$value", "<void>");
    } catch (const std::exception& error) {
        ok = false;
        std::cerr << "[DEBUG-RENDERER-PACKET-SMOKE] clear strict decode failed: "
                  << error.what() << "\n";
    }

    try {
        ok &= checkStructuredReencode(codec, encoder, golden, "clear");
    } catch (const std::exception& error) {
        ok = false;
        std::cerr << "[DEBUG-RENDERER-PACKET-SMOKE] clear re-encode failed: "
                  << error.what() << "\n";
    }
    return ok;
}

bool checkAddCubePacket(
    const bedrock::VersionedPacketCodec& codec,
    const bedrock::ProtoDefPacketEncoder& encoder,
    const bedrock::ProtoDefPacketDecoder& decoder
) {
    // 1.21.100 uses one cube per packet: li32 action, string, vec3f,
    // four lf32 colour components, then a millisecond li64 duration.
    const auto golden = unhex(
        "a401"
        "02000000"
        "0463756265"
        "0000803f0000004000004040"
        "000000000000803f0000003f0000803e"
        "8813000000000000"
    );
    const auto payload = encoder.encodePacket(kPacketName, addCubeValue());
    const auto encoded = codec.makePacketByName(kPacketName, payload);

    bool ok = true;
    ok &= check(encoded.fullPacket == golden, "add_cube golden bytes mismatch");
    ok &= check(encoded.packetId == kPacketId, "add_cube packet id mismatch");

    const auto packet = codec.decodeFullPacket(golden);
    ok &= check(packet.packetId == kPacketId, "decoded add_cube packet id mismatch");
    ok &= check(packet.name == kPacketName, "decoded add_cube packet name mismatch");
    ok &= check(
        packet.paramsType == "packet_debug_renderer",
        "decoded add_cube params type mismatch"
    );
    ok &= check(packet.payload == payload, "decoded add_cube payload mismatch");

    try {
        const auto fields = decoder.decodePacketStrict(packet.name, packet.payload);
        ok &= check(fields.size() == 8u, "add_cube strict decode field count mismatch");
        ok &= checkField(fields, "type", "2/add_cube");
        ok &= checkField(fields, "text", "cube");
        ok &= checkField(fields, "position", "1.000000,2.000000,3.000000");
        ok &= checkFloatField(fields, "red", 0.0);
        ok &= checkFloatField(fields, "green", 1.0);
        ok &= checkFloatField(fields, "blue", 0.5);
        ok &= checkFloatField(fields, "alpha", 0.25);
        ok &= checkField(fields, "duration", "5000");
    } catch (const std::exception& error) {
        ok = false;
        std::cerr << "[DEBUG-RENDERER-PACKET-SMOKE] add_cube strict decode failed: "
                  << error.what() << "\n";
    }

    try {
        ok &= checkStructuredReencode(codec, encoder, golden, "add_cube");
    } catch (const std::exception& error) {
        ok = false;
        std::cerr << "[DEBUG-RENDERER-PACKET-SMOKE] add_cube re-encode failed: "
                  << error.what() << "\n";
    }
    return ok;
}

bool checkServerScriptDebugDrawerPacket(
    const bedrock::VersionedPacketCodec& codec,
    const bedrock::ProtoDefPacketEncoder& encoder,
    const bedrock::ProtoDefPacketDecoder& decoder
) {
    // This single-box golden exercises every field used by the schematic
    // backend and all unused option-presence bytes. Retail 1.21.100 uses
    // Location as the lower block corner and renders a 0.5 bound one block
    // wide on each axis.
    const auto golden = unhex(
        "c802"
        "01"
        "8080808080a091a843"
        "0101"
        "010000803f0000004000004040"
        "010000803f"
        "00"
        "00"
        "0111223344"
        "00"
        "010000003f0000003f0000003f"
        "00000000"
    );
    const auto payload = encoder.encodePacket(
        kScriptDrawerPacketName,
        scriptDrawerValue(true)
    );
    const auto encoded = codec.makePacketByName(
        kScriptDrawerPacketName,
        payload
    );

    bool ok = true;
    if (!check(encoded.fullPacket == golden, "script drawer golden bytes mismatch")) {
        std::cerr << "[DEBUG-RENDERER-PACKET-SMOKE] actual="
                  << hex(encoded.fullPacket) << "\n"
                  << "[DEBUG-RENDERER-PACKET-SMOKE] expected="
                  << hex(golden) << "\n";
        ok = false;
    }
    ok &= check(
        encoded.packetId == kScriptDrawerPacketId,
        "script drawer packet id mismatch"
    );

    const auto packet = codec.decodeFullPacket(golden);
    ok &= check(
        packet.name == kScriptDrawerPacketName,
        "decoded script drawer packet name mismatch"
    );
    ok &= check(
        packet.paramsType == "packet_server_script_debug_drawer",
        "decoded script drawer params type mismatch"
    );
    ok &= check(packet.payload == payload, "decoded script drawer payload mismatch");

    try {
        const auto fields = decoder.decodePacketStrict(packet.name, packet.payload);
        ok &= checkField(fields, "shapes.$count", "1");
        ok &= checkField(
            fields,
            "shapes[0].network_id",
            "4850452664980340736"
        );
        ok &= checkField(fields, "shapes[0].shape_type", "1/box");
        ok &= checkField(fields, "shapes[0].location.$present", "true");
        ok &= checkField(fields, "shapes[0].location", "1.000000,2.000000,3.000000");
        ok &= checkField(fields, "shapes[0].color.$present", "true");
        ok &= checkField(fields, "shapes[0].color", "1144201745");
        ok &= checkField(fields, "shapes[0].box_bound.$present", "true");
        ok &= checkField(fields, "shapes[0].box_bound", "0.500000,0.500000,0.500000");
    } catch (const std::exception& error) {
        ok = false;
        std::cerr << "[DEBUG-RENDERER-PACKET-SMOKE] script drawer strict decode failed: "
                  << error.what() << "\n";
    }

    try {
        ok &= checkStructuredReencode(codec, encoder, golden, "script drawer");
    } catch (const std::exception& error) {
        ok = false;
        std::cerr << "[DEBUG-RENDERER-PACKET-SMOKE] script drawer re-encode failed: "
                  << error.what() << "\n";
    }

    // A keyed record with every optional field absent is the removal
    // operation. The array lets the relay group all removals into one packet.
    const auto removalPayload = encoder.encodePacket(
        kScriptDrawerPacketName,
        scriptDrawerRemovalValue()
    );
    const auto removal = codec.makePacketByName(
        kScriptDrawerPacketName,
        removalPayload
    );
    ok &= check(
        removal.fullPacket == unhex(
            "c802"
            "01"
            "8080808080a091a843"
            "000000000000000000000000"
        ),
        "script drawer keyed-removal bytes mismatch"
    );
    return ok;
}

bool checkScriptDrawerRange(
    const bedrock::VersionedPacketCodec& codec,
    const bedrock::ProtoDefPacketEncoder& encoder,
    const bedrock::ProtoDefPacketDecoder& decoder,
    std::size_t offset,
    std::size_t count,
    bool removals,
    const std::string& label
) {
    const auto payload = encoder.encodePacket(
        kScriptDrawerPacketName,
        scriptDrawerRangeValue(offset, count, removals)
    );
    const auto encoded = codec.makePacketByName(
        kScriptDrawerPacketName,
        payload
    );

    bool ok = true;
    ok &= check(
        encoded.packetId == kScriptDrawerPacketId,
        label + " packet id mismatch"
    );
    try {
        const auto fields = decoder.decodePacketStrict(
            kScriptDrawerPacketName,
            encoded.payload
        );
        ok &= checkField(
            fields,
            "shapes.$count",
            std::to_string(count)
        );
        if (count != 0) {
            ok &= checkField(
                fields,
                "shapes[0].network_id",
                std::to_string(kScriptDrawerNetworkId + offset)
            );
            ok &= checkField(
                fields,
                "shapes[" + std::to_string(count - 1u) + "].network_id",
                std::to_string(
                    kScriptDrawerNetworkId + offset + count - 1u
                )
            );
        }
    } catch (const std::exception& error) {
        ok = false;
        std::cerr << "[DEBUG-RENDERER-PACKET-SMOKE] " << label
                  << " strict decode failed: " << error.what() << "\n";
    }
    return ok;
}

bool checkLargeAndBatchedServerScriptDebugDrawerPackets(
    const bedrock::VersionedPacketCodec& codec,
    const bedrock::ProtoDefPacketEncoder& encoder,
    const bedrock::ProtoDefPacketDecoder& decoder
) {
    // The Android crash was triggered while ProtoDef constructed a single
    // 1,290-entry container. Keep this direct regression so the structured
    // value ownership path cannot silently return to quadratic growth.
    bool ok = checkScriptDrawerRange(
        codec,
        encoder,
        decoder,
        0,
        kLargeSchematicShapeCount,
        false,
        "large script drawer"
    );

    // Production deliberately caps each keyed operation array. Verify every
    // ID is covered exactly once for both additions and removals and that the
    // final partial batch keeps its global network-ID offset.
    for (const bool removals : {false, true}) {
        std::size_t offset = 0;
        std::size_t packetCount = 0;
        while (offset < kLargeSchematicShapeCount) {
            const auto count = std::min(
                kScriptDrawerBatchSize,
                kLargeSchematicShapeCount - offset
            );
            ok &= check(count <= kScriptDrawerBatchSize,
                        "script drawer batch exceeded shape cap");
            ok &= checkScriptDrawerRange(
                codec,
                encoder,
                decoder,
                offset,
                count,
                removals,
                removals
                    ? "script drawer removal batch"
                    : "script drawer addition batch"
            );
            offset += count;
            ++packetCount;
        }
        const auto expectedPackets =
            kLargeSchematicShapeCount / kScriptDrawerBatchSize +
            (kLargeSchematicShapeCount % kScriptDrawerBatchSize != 0 ? 1u : 0u);
        ok &= check(
            offset == kLargeSchematicShapeCount,
            "script drawer batches did not cover every shape"
        );
        ok &= check(
            packetCount == expectedPackets,
            "script drawer batch packet count mismatch"
        );
    }
    return ok;
}

} // namespace

int main() {
    try {
        const auto definition = bedrock::ProtocolDefinition::forVersion(kVersion);
        bool ok = true;
        ok &= check(definition.protocolVersion() == 827u, "protocol id is not 827");
        ok &= check(definition.packetId(kPacketName) == kPacketId, "packet id is not 0xa4");
        ok &= check(
            definition.packetParamsType(kPacketName) == "packet_debug_renderer",
            "packet schema type mismatch"
        );
        ok &= check(
            definition.packetId(kScriptDrawerPacketName) ==
                kScriptDrawerPacketId,
            "script drawer packet id is not 0x148"
        );
        ok &= check(
            definition.packetParamsType(kScriptDrawerPacketName) ==
                "packet_server_script_debug_drawer",
            "script drawer packet schema type mismatch"
        );

        const auto codec = bedrock::VersionedPacketCodec::forVersion(kVersion);
        const bedrock::ProtoDefPacketEncoder encoder(kVersion);
        const bedrock::ProtoDefPacketDecoder decoder(kVersion);
        ok &= checkClearPacket(codec, encoder, decoder);
        ok &= checkAddCubePacket(codec, encoder, decoder);
        ok &= checkServerScriptDebugDrawerPacket(codec, encoder, decoder);
        ok &= checkLargeAndBatchedServerScriptDebugDrawerPackets(
            codec,
            encoder,
            decoder
        );

        if (!ok) return 1;
        std::cout << "[DEBUG-RENDERER-PACKET-SMOKE] OK version=" << kVersion
                  << " protocol=827 packet_ids=0xa4,0x148 large_shapes="
                  << kLargeSchematicShapeCount << " batch="
                  << kScriptDrawerBatchSize << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[DEBUG-RENDERER-PACKET-SMOKE] " << error.what() << "\n";
        return 1;
    }
}
