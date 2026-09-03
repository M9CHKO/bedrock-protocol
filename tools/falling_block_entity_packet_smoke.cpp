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
constexpr const char* kAddPacketName = "add_entity";
constexpr const char* kRemovePacketName = "remove_entity";
constexpr uint32_t kAddPacketId = 13;
constexpr uint32_t kRemovePacketId = 14;
constexpr int64_t kUniqueId = 0x4350451000000001LL;
constexpr uint64_t kRuntimeId = 0x4350451000000001ULL;
constexpr int32_t kBlockRuntimeId = 1234;

using Value = bedrock::ProtoDefValue;

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[FALLING-BLOCK-ENTITY-PACKET-SMOKE] " << message << "\n";
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

Value vec3(double x, double y, double z) {
    return Value::object({
        {"x", Value::floating(x)},
        {"y", Value::floating(y)},
        {"z", Value::floating(z)}
    });
}

Value metadataEntry(
    const char* key,
    const char* type,
    Value value
) {
    // MetadataDictionary.value is key-switched and ordinary keys then use a
    // nested type switch. Keep the nested switch context beside its leaf.
    Value switchValue = std::string(key) == "flags"
        ? std::move(value)
        : Value::object({
            {"type", Value::string(type)},
            {"$value", std::move(value)}
        });
    return Value::object({
        {"key", Value::string(key)},
        {"type", Value::string(type)},
        {"value", std::move(switchValue)}
    });
}

Value fallingBlockValue() {
    return Value::object({
        {"unique_id", Value::integer(kUniqueId)},
        {"runtime_id", Value::uinteger(kRuntimeId)},
        {"entity_type", Value::string("minecraft:falling_block")},
        {"position", vec3(10.5, 64.49, -20.5)},
        {"velocity", vec3(0.0, 0.0, 0.0)},
        {"pitch", Value::floating(0.0)},
        {"yaw", Value::floating(0.0)},
        {"head_yaw", Value::floating(0.0)},
        {"body_yaw", Value::floating(0.0)},
        {"attributes", Value::array({})},
        {"metadata", Value::array({
            metadataEntry(
                "flags",
                "long",
                Value::object({{"no_ai", Value::boolean(true)}})
            ),
            metadataEntry(
                "variant",
                "int",
                Value::integer(kBlockRuntimeId)
            ),
            metadataEntry("scale", "float", Value::floating(0.92)),
            metadataEntry(
                "boundingbox_width",
                "float",
                Value::floating(0.0)
            ),
            metadataEntry(
                "boundingbox_height",
                "float",
                Value::floating(0.0)
            )
        })},
        {"properties", Value::object({
            {"ints", Value::array({})},
            {"floats", Value::array({})}
        })},
        {"links", Value::array({})}
    });
}

Value removeValue() {
    return Value::object({
        {"entity_id_self", Value::integer(kUniqueId)}
    });
}

bool checkStructuredReencode(
    const bedrock::VersionedPacketCodec& codec,
    const bedrock::ProtoDefPacketEncoder& encoder,
    const std::vector<uint8_t>& fullPacket,
    Value structuredValue,
    const std::string& label
) {
    const auto packet = codec.decodeFullPacket(fullPacket);
    // This client-only producer owns the exact packet value and reuses it for
    // retries. RelayPacketEvent's generic reconstructed MetadataDictionary is
    // intentionally not part of this send path.
    const auto payload = encoder.encodePacket(
        packet.name,
        structuredValue
    );
    const auto reencoded = codec.encodeFullPacketByName(packet.name, payload);
    return check(
        reencoded == fullPacket,
        label + " strict structured decode/re-encode changed bytes"
    );
}

bool checkAddEntity(
    const bedrock::VersionedPacketCodec& codec,
    const bedrock::ProtoDefPacketEncoder& encoder,
    const bedrock::ProtoDefPacketDecoder& decoder
) {
    const auto payload = encoder.encodePacket(kAddPacketName, fallingBlockValue());
    const auto encoded = codec.makePacketByName(kAddPacketName, payload);
    const auto golden = unhex(
        "0d"
        "8280808080c4a2d08601"
        "8180808080a291a843"
        "176d696e6563726166743a66616c6c696e675f626c6f636b"
        "00002841e1fa80420000a4c1"
        "000000000000000000000000"
        "00000000000000000000000000000000"
        "00"
        "05"
        "0007808008"
        "0202a413"
        "26031f856b3f"
        "350300000000"
        "360300000000"
        "000000"
    );

    bool ok = true;
    ok &= check(
        encoded.fullPacket == golden,
        "add_entity golden bytes mismatch"
    );
    ok &= check(encoded.packetId == kAddPacketId, "add_entity packet id mismatch");

    const auto packet = codec.decodeFullPacket(golden);
    ok &= check(packet.name == kAddPacketName, "decoded add_entity name mismatch");
    ok &= check(
        packet.paramsType == "packet_add_entity",
        "decoded add_entity params type mismatch"
    );
    ok &= check(packet.payload == payload, "decoded add_entity payload mismatch");

    try {
        const auto fields = decoder.decodePacketStrict(packet.name, packet.payload);
        ok &= checkField(fields, "unique_id", std::to_string(kUniqueId));
        ok &= checkField(fields, "runtime_id", std::to_string(kRuntimeId));
        ok &= checkField(fields, "entity_type", "minecraft:falling_block");
        ok &= checkField(fields, "position", "10.500000,64.489998,-20.500000");
        ok &= checkField(fields, "velocity", "0.000000,0.000000,0.000000");
        ok &= checkFloatField(fields, "pitch", 0.0);
        ok &= checkFloatField(fields, "yaw", 0.0);
        ok &= checkFloatField(fields, "head_yaw", 0.0);
        ok &= checkFloatField(fields, "body_yaw", 0.0);
        ok &= checkField(fields, "attributes.$count", "0");
        ok &= checkField(fields, "metadata.$count", "5");

        ok &= checkField(fields, "metadata[0].key", "0/flags");
        ok &= checkField(fields, "metadata[0].type", "7/long");
        ok &= checkField(fields, "metadata[0].value", "65536");
        ok &= checkField(fields, "metadata[0].value.no_ai", "true");
        ok &= checkField(fields, "metadata[0].value.has_collision", "false");
        ok &= checkField(
            fields,
            "metadata[0].value.affected_by_gravity",
            "false"
        );

        ok &= checkField(fields, "metadata[1].key", "2/variant");
        ok &= checkField(fields, "metadata[1].type", "2/int");
        ok &= checkField(
            fields,
            "metadata[1].value",
            std::to_string(kBlockRuntimeId)
        );
        ok &= checkField(fields, "metadata[2].key", "38/scale");
        ok &= checkField(fields, "metadata[2].type", "3/float");
        ok &= checkFloatField(fields, "metadata[2].value", 0.92);
        ok &= checkField(fields, "metadata[3].key", "53/boundingbox_width");
        ok &= checkField(fields, "metadata[3].type", "3/float");
        ok &= checkFloatField(fields, "metadata[3].value", 0.0);
        ok &= checkField(fields, "metadata[4].key", "54/boundingbox_height");
        ok &= checkField(fields, "metadata[4].type", "3/float");
        ok &= checkFloatField(fields, "metadata[4].value", 0.0);

        ok &= checkField(fields, "properties.ints.$count", "0");
        ok &= checkField(fields, "properties.floats.$count", "0");
        ok &= checkField(fields, "links.$count", "0");
    } catch (const std::exception& error) {
        ok = false;
        std::cerr << "[FALLING-BLOCK-ENTITY-PACKET-SMOKE] add_entity strict decode failed: "
                  << error.what() << "\n";
    }

    try {
        ok &= checkStructuredReencode(
            codec,
            encoder,
            golden,
            fallingBlockValue(),
            "add_entity"
        );
    } catch (const std::exception& error) {
        ok = false;
        std::cerr << "[FALLING-BLOCK-ENTITY-PACKET-SMOKE] add_entity re-encode failed: "
                  << error.what() << "\n";
    }
    return ok;
}

bool checkRemoveEntity(
    const bedrock::VersionedPacketCodec& codec,
    const bedrock::ProtoDefPacketEncoder& encoder,
    const bedrock::ProtoDefPacketDecoder& decoder
) {
    const auto payload = encoder.encodePacket(kRemovePacketName, removeValue());
    const auto encoded = codec.makePacketByName(kRemovePacketName, payload);
    const auto golden = unhex("0e8280808080c4a2d08601");

    bool ok = true;
    ok &= check(
        encoded.fullPacket == golden,
        "remove_entity golden bytes mismatch"
    );
    ok &= check(
        encoded.packetId == kRemovePacketId,
        "remove_entity packet id mismatch"
    );

    const auto packet = codec.decodeFullPacket(golden);
    ok &= check(
        packet.name == kRemovePacketName,
        "decoded remove_entity name mismatch"
    );
    ok &= check(
        packet.paramsType == "packet_remove_entity",
        "decoded remove_entity params type mismatch"
    );
    ok &= check(packet.payload == payload, "decoded remove_entity payload mismatch");

    try {
        const auto fields = decoder.decodePacketStrict(packet.name, packet.payload);
        ok &= checkField(
            fields,
            "entity_id_self",
            std::to_string(kUniqueId)
        );
    } catch (const std::exception& error) {
        ok = false;
        std::cerr << "[FALLING-BLOCK-ENTITY-PACKET-SMOKE] remove_entity strict decode failed: "
                  << error.what() << "\n";
    }

    try {
        ok &= checkStructuredReencode(
            codec,
            encoder,
            golden,
            removeValue(),
            "remove_entity"
        );
    } catch (const std::exception& error) {
        ok = false;
        std::cerr << "[FALLING-BLOCK-ENTITY-PACKET-SMOKE] remove_entity re-encode failed: "
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
        ok &= check(
            definition.packetId(kAddPacketName) == kAddPacketId,
            "add_entity schema packet id is not 13"
        );
        ok &= check(
            definition.packetParamsType(kAddPacketName) == "packet_add_entity",
            "add_entity schema type mismatch"
        );
        ok &= check(
            definition.packetId(kRemovePacketName) == kRemovePacketId,
            "remove_entity schema packet id is not 14"
        );
        ok &= check(
            definition.packetParamsType(kRemovePacketName) ==
                "packet_remove_entity",
            "remove_entity schema type mismatch"
        );

        const auto codec = bedrock::VersionedPacketCodec::forVersion(kVersion);
        const bedrock::ProtoDefPacketEncoder encoder(kVersion);
        const bedrock::ProtoDefPacketDecoder decoder(kVersion);
        ok &= checkAddEntity(codec, encoder, decoder);
        ok &= checkRemoveEntity(codec, encoder, decoder);

        if (!ok) return 1;
        std::cout << "[FALLING-BLOCK-ENTITY-PACKET-SMOKE] OK version="
                  << kVersion
                  << " protocol=827 add_id=13 remove_id=14 unique_id="
                  << kUniqueId << " block_runtime_id=" << kBlockRuntimeId
                  << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FALLING-BLOCK-ENTITY-PACKET-SMOKE] "
                  << error.what() << "\n";
        return 1;
    }
}
