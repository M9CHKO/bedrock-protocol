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

        const auto codec = bedrock::VersionedPacketCodec::forVersion(kVersion);
        const bedrock::ProtoDefPacketEncoder encoder(kVersion);
        const bedrock::ProtoDefPacketDecoder decoder(kVersion);
        ok &= checkClearPacket(codec, encoder, decoder);
        ok &= checkAddCubePacket(codec, encoder, decoder);

        if (!ok) return 1;
        std::cout << "[DEBUG-RENDERER-PACKET-SMOKE] OK version=" << kVersion
                  << " protocol=827 packet_id=0xa4\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[DEBUG-RENDERER-PACKET-SMOKE] " << error.what() << "\n";
        return 1;
    }
}
