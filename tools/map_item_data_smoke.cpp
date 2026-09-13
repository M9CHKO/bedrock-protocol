#include <bedrock/bedrock.hpp>
#include <bedrock/debug/ProtocolTypeTsvIndex.hpp>
#include <bedrock/generated/GeneratedProtocolTypes.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protocol/VersionedPacketCodec.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Value = bedrock::ProtoDefValue;

constexpr std::string_view kMapScaleExpression =
    "update_flags.initialisation || update_flags.decoration || "
    "update_flags.texture";

struct MapUpdateCase {
    bool texture = false;
    bool decoration = false;
    bool initialisation = false;
    std::size_t pixelCount = 4;
};

std::vector<std::string> sourceExpressionVersions() {
    std::vector<std::string> versions;
    const std::filesystem::path root("data/minecraft-data/bedrock");
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        const auto protocol = entry.path() / "protocol.json";
        std::ifstream input(protocol, std::ios::binary);
        if (!input) continue;
        const std::string contents {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
        if (contents.find(kMapScaleExpression) != std::string::npos) {
            versions.push_back(entry.path().filename().string());
        }
    }
    std::sort(versions.begin(), versions.end());
    return versions;
}

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[MAP-ITEM-DATA-SMOKE] " << message << '\n';
    }
    return condition;
}

std::vector<std::uint8_t> unhex(const std::string& text) {
    auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    if ((text.size() & 1u) != 0) return {};
    std::vector<std::uint8_t> output;
    output.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const int high = digit(text[index]);
        const int low = digit(text[index + 1]);
        if (high < 0 || low < 0) return {};
        output.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return output;
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

Value flagsValue(const MapUpdateCase& update) {
    const std::uint64_t mask =
        (update.texture ? 0x02u : 0u) |
        (update.decoration ? 0x04u : 0u) |
        (update.initialisation ? 0x08u : 0u);
    return Value::object({
        {"_value", Value::uinteger(mask)},
        {"void", Value::boolean(false)},
        {"texture", Value::boolean(update.texture)},
        {"decoration", Value::boolean(update.decoration)},
        {"initialisation", Value::boolean(update.initialisation)}
    });
}

Value modernPacketValue(const MapUpdateCase& update) {
    std::unordered_map<std::string, Value> packet {
        {"map_id", Value::integer(0x100000001LL)},
        {"update_flags", flagsValue(update)},
        {"dimension", Value::uinteger(0)},
        {"locked", Value::boolean(false)},
        {"origin", Value::object({
            {"x", Value::integer(128)},
            {"y", Value::integer(70)},
            {"z", Value::integer(-128)}
        })}
    };

    if (update.initialisation) {
        packet["included_in"] = Value::array({
            Value::integer(0x100000001LL),
            Value::integer(0x200000002LL)
        });
    }
    if (update.texture || update.decoration || update.initialisation) {
        packet["scale"] = Value::uinteger(3);
    }
    if (update.decoration) {
        packet["tracked"] = Value::object({
            {"objects", Value::array({})},
            {"decorations", Value::array({})}
        });
    }
    if (update.texture) {
        std::vector<Value> pixels;
        pixels.reserve(update.pixelCount);
        for (std::size_t index = 0; index < update.pixelCount; ++index) {
            pixels.push_back(Value::uinteger(
                0xff336699u ^ static_cast<std::uint32_t>(index & 0xffu)
            ));
        }
        const auto side = update.pixelCount == 16'384 ? 128 : 2;
        packet["texture"] = Value::object({
            {"width", Value::integer(side)},
            {"height", Value::integer(side)},
            {"x_offset", Value::integer(0)},
            {"y_offset", Value::integer(0)},
            {"pixels", Value::array(std::move(pixels))}
        });
    }
    return Value::object(std::move(packet));
}

Value reportedFullMapValue() {
    std::vector<Value> pixels(
        16'384,
        Value::uinteger(std::numeric_limits<std::uint32_t>::max())
    );
    return Value::object({
        {"map_id", Value::integer(0)},
        {"update_flags", flagsValue({.texture = true})},
        {"dimension", Value::uinteger(0)},
        {"locked", Value::boolean(false)},
        {"origin", Value::object({
            {"x", Value::integer(0)},
            {"y", Value::integer(0)},
            {"z", Value::integer(0)}
        })},
        {"scale", Value::uinteger(0)},
        {"texture", Value::object({
            {"width", Value::integer(128)},
            {"height", Value::integer(128)},
            {"x_offset", Value::integer(0)},
            {"y_offset", Value::integer(0)},
            {"pixels", Value::array(std::move(pixels))}
        })}
    });
}

bool validateModernFields(
    const std::string& label,
    const std::vector<bedrock::ProtoDefField>& fields,
    const MapUpdateCase& update
) {
    bool ok = true;
    const auto* textureFlag = field(fields, "update_flags.texture");
    const auto* decorationFlag = field(fields, "update_flags.decoration");
    const auto* initialisationFlag = field(
        fields,
        "update_flags.initialisation"
    );
    ok &= check(
        textureFlag && textureFlag->value ==
            (update.texture ? "true" : "false"),
        label + ": texture flag mismatch"
    );
    ok &= check(
        decorationFlag && decorationFlag->value ==
            (update.decoration ? "true" : "false"),
        label + ": decoration flag mismatch"
    );
    ok &= check(
        initialisationFlag && initialisationFlag->value ==
            (update.initialisation ? "true" : "false"),
        label + ": initialisation flag mismatch"
    );

    const bool hasScale = update.texture || update.decoration ||
        update.initialisation;
    const auto* scale = field(fields, "scale");
    ok &= check(
        hasScale
            ? scale && scale->value == "3"
            : !scale || scale->value == "<no_branch:false>",
        label + ": logical-OR scale branch mismatch"
    );

    const auto* includedCount = field(fields, "included_in.$count");
    ok &= check(
        update.initialisation
            ? includedCount && includedCount->value == "2"
            : includedCount == nullptr,
        label + ": initialisation branch mismatch"
    );
    const auto* objectCount = field(fields, "tracked.objects.$count");
    const auto* decorationCount = field(
        fields,
        "tracked.decorations.$count"
    );
    ok &= check(
        update.decoration
            ? objectCount && objectCount->value == "0" &&
                decorationCount && decorationCount->value == "0"
            : objectCount == nullptr && decorationCount == nullptr,
        label + ": decoration branch mismatch"
    );

    const auto* pixelCount = field(fields, "texture.pixels.$count");
    ok &= check(
        update.texture
            ? pixelCount && pixelCount->value ==
                std::to_string(update.pixelCount)
            : pixelCount == nullptr,
        label + ": texture branch/count mismatch"
    );
    if (update.texture && update.pixelCount != 0) {
        const auto* lastPixel = field(
            fields,
            "texture.pixels[" + std::to_string(update.pixelCount - 1) + "]"
        );
        ok &= check(
            lastPixel != nullptr,
            label + ": final texture pixel was not decoded"
        );
    }
    return ok;
}

bool roundTripModern(
    const std::string& version,
    const bedrock::ProtocolTypeTsvIndex& typeIndex,
    const std::string& source,
    const MapUpdateCase& update
) {
    const std::string label = version + " " + source;
    try {
        const bedrock::ProtoDefPacketEncoder encoder(version, typeIndex);
        const bedrock::ProtoDefPacketDecoder decoder(version, typeIndex);
        const auto value = modernPacketValue(update);
        const auto payload = encoder.encodePacket(
            "clientbound_map_item_data",
            value
        );
        const auto fields = decoder.decodePacketStrict(
            "clientbound_map_item_data",
            payload
        );
        bool ok = validateModernFields(label, fields, update);
        ok &= check(
            encoder.encodePacket("clientbound_map_item_data", value) == payload,
            label + ": repeated encode changed payload"
        );
        return ok;
    } catch (const std::exception& error) {
        std::cerr << "[MAP-ITEM-DATA-SMOKE] " << label << ": "
                  << error.what() << '\n';
        return false;
    }
}

bool roundTripLegacy(
    const std::string& version,
    const bedrock::ProtocolTypeTsvIndex& typeIndex,
    const std::string& source
) {
    const std::string label = version + " " + source;
    try {
        const bedrock::ProtoDefPacketEncoder encoder(version, typeIndex);
        const bedrock::ProtoDefPacketDecoder decoder(version, typeIndex);
        const std::vector<std::uint8_t> mapInfo {
            0x02, 0x04, 0x08, 0x80, 0x80, 0x80, 0x80, 0x0f
        };
        const auto value = Value::object({
            {"mapinfo", Value::bytes(mapInfo)}
        });
        const auto payload = encoder.encodePacket(
            "clientbound_map_item_data",
            value
        );
        const auto fields = decoder.decodePacketStrict(
            "clientbound_map_item_data",
            payload
        );
        const auto* mapInfoField = field(fields, "mapinfo");
        return check(payload == mapInfo, label + ": MapInfo bytes changed") &&
            check(
                mapInfoField && mapInfoField->structuredValue.has_value() &&
                    mapInfoField->structuredValue->kind == Value::Kind::Bytes &&
                    mapInfoField->structuredValue->bytesValue == mapInfo,
                label + ": legacy MapInfo did not consume the full payload"
            );
    } catch (const std::exception& error) {
        std::cerr << "[MAP-ITEM-DATA-SMOKE] " << label << ": "
                  << error.what() << '\n';
        return false;
    }
}

bool relayRoundTrip(
    const std::string& version,
    const Value& value
) {
    try {
        const bedrock::ProtoDefPacketEncoder encoder(version);
        const auto payload = encoder.encodePacket(
            "clientbound_map_item_data",
            value
        );
        const auto codec = bedrock::VersionedPacketCodec::forVersion(version);
        bedrock::BedrockRelayPacketEvent rawEvent;
        rawEvent.packet = codec.makePacketByName(
            "clientbound_map_item_data",
            payload
        );
        bedrock::RelayPacketEvent event(version, rawEvent);
        const auto& params = event.decodedParams();
        const auto reconstructed = encoder.encodePacket(
            "clientbound_map_item_data",
            Value::object(params)
        );
        return check(
            reconstructed == payload,
            version + ": high-level Relay decode/encode changed map payload"
        );
    } catch (const std::exception& error) {
        std::cerr << "[MAP-ITEM-DATA-SMOKE] " << version
                  << " Relay: " << error.what() << '\n';
        return false;
    }
}

bool reportedFullMapRoundTrip(
    const bedrock::ProtocolTypeTsvIndex& typeIndex,
    const std::string& source
) {
    constexpr const char* version = "1.21.100";
    const std::string label = std::string(version) + " full map " + source;
    try {
        const bedrock::ProtoDefPacketEncoder encoder(version, typeIndex);
        const bedrock::ProtoDefPacketDecoder decoder(version, typeIndex);
        const auto payload = encoder.encodePacket(
            "clientbound_map_item_data",
            reportedFullMapValue()
        );
        bool ok = check(
            payload.size() == 81'937,
            label + ": payload size is " + std::to_string(payload.size()) +
                ", expected 81937"
        );
        const std::vector<std::uint8_t> expectedPixel {
            0xff, 0xff, 0xff, 0xff, 0x0f
        };
        ok &= check(
            payload.size() >= expectedPixel.size() && std::equal(
                expectedPixel.begin(),
                expectedPixel.end(),
                payload.end() - static_cast<std::ptrdiff_t>(
                    expectedPixel.size()
                )
            ),
            label + ": UINT32_MAX pixel is not a five-byte varuint"
        );

        const auto fields = decoder.decodePacketStrict(
            "clientbound_map_item_data",
            payload
        );
        const auto fieldEquals = [&](const std::string& path,
                                     const std::string& expected) {
            const auto* value = field(fields, path);
            return check(
                value && value->value == expected,
                label + ": " + path + " mismatch"
            );
        };
        ok &= fieldEquals("update_flags.texture", "true");
        ok &= fieldEquals("scale", "0");
        ok &= fieldEquals("texture.width", "128");
        ok &= fieldEquals("texture.height", "128");
        ok &= fieldEquals("texture.x_offset", "0");
        ok &= fieldEquals("texture.y_offset", "0");
        ok &= fieldEquals("texture.pixels.$count", "16384");
        ok &= fieldEquals("texture.pixels[16383]", "4294967295");

        const auto codec = bedrock::VersionedPacketCodec::forVersion(version);
        bedrock::BedrockRelayPacketEvent rawEvent;
        rawEvent.packet = codec.makePacketByName(
            "clientbound_map_item_data",
            payload
        );
        bedrock::RelayPacketEvent event(version, rawEvent, {}, true);
        const auto reconstructed = encoder.encodePacket(
            "clientbound_map_item_data",
            Value::object(event.decodedParams())
        );
        ok &= check(
            reconstructed == payload,
            label + ": strict decode/reconstruction changed payload bytes"
        );
        return ok;
    } catch (const std::exception& error) {
        std::cerr << "[MAP-ITEM-DATA-SMOKE] " << label << ": "
                  << error.what() << '\n';
        return false;
    }
}

bool nodeGoldenRoundTrip() {
    constexpr const char* version = "1.21.100";
    try {
        // Generated by node_modules/bedrock-protocol 3.53.0 using its
        // compiled ProtoDef serializer. Pixel colours require five-byte
        // varuints, matching the packet shape that exposed the relay reset.
        const auto golden = unhex(
            "43828080802002000080028c01ff01030404000004"
            "99cdcdf90f98cdcdf90f9bcdcdf90f9acdcdf90f"
        );
        const auto codec = bedrock::VersionedPacketCodec::forVersion(version);
        const auto packet = codec.decodeFullPacket(golden);
        const bedrock::ProtoDefPacketDecoder decoder(version);
        const auto fields = decoder.decodePacketStrict(
            packet.name,
            packet.payload
        );
        bool ok = validateModernFields(
            "1.21.100 Node golden",
            fields,
            {.texture = true}
        );

        const bedrock::ProtoDefPacketEncoder encoder(version);
        const auto encoded = codec.encodeFullPacketByName(
            packet.name,
            encoder.encodePacket(
                packet.name,
                modernPacketValue({.texture = true})
            )
        );
        ok &= check(
            encoded == golden,
            "1.21.100 C++ encode differs from node_modules golden"
        );

        bedrock::BedrockRelayPacketEvent rawEvent;
        rawEvent.packet = packet;
        bedrock::RelayPacketEvent event(version, rawEvent);
        const auto reconstructed = codec.encodeFullPacketByName(
            packet.name,
            encoder.encodePacket(
                packet.name,
                Value::object(event.decodedParams())
            )
        );
        ok &= check(
            reconstructed == golden,
            "1.21.100 Relay changed node_modules golden payload"
        );
        return ok;
    } catch (const std::exception& error) {
        std::cerr << "[MAP-ITEM-DATA-SMOKE] 1.21.100 Node golden: "
                  << error.what() << '\n';
        return false;
    }
}

} // namespace

int main() {
    bool ok = true;
    const auto versions = bedrock::ProtocolDefinition::versions();
    const auto sourceVersions = sourceExpressionVersions();
    const bedrock::ProtocolTypeTsvIndex tsvTypes;
    const bedrock::ProtocolTypeTsvIndex generatedTypes(
        std::filesystem::path("data/generated/does-not-exist")
    );
    const std::vector<MapUpdateCase> cases {
        {},
        {.texture = true},
        {.decoration = true},
        {.initialisation = true},
        {
            .texture = true,
            .decoration = true,
            .initialisation = true
        }
    };

    std::size_t checkedVersions = 0;
    std::size_t expressionVersions = 0;
    for (const auto& version : versions) {
        const auto definition = bedrock::ProtocolDefinition::forVersion(version);
        if (!definition.hasPacket("clientbound_map_item_data")) continue;
        ++checkedVersions;

        const auto type = bedrock::generatedProtocolTypeJson(
            version,
            "packet_clientbound_map_item_data"
        );
        ok &= check(type.has_value(), version + ": generated packet type missing");
        if (!type) continue;

        const bool hasLogicalOr = type->find(kMapScaleExpression) !=
            std::string::npos;
        if (!hasLogicalOr) {
            ok &= roundTripLegacy(version, tsvTypes, "TSV");
            ok &= roundTripLegacy(version, generatedTypes, "generated C++");
            continue;
        }

        ++expressionVersions;
        for (const auto& update : cases) {
            ok &= roundTripModern(version, tsvTypes, "TSV", update);
            ok &= roundTripModern(
                version,
                generatedTypes,
                "generated C++",
                update
            );
        }
        ok &= relayRoundTrip(version, modernPacketValue({.texture = true}));
    }

    ok &= check(!versions.empty(), "registered protocol version list is empty");
    ok &= check(
        checkedVersions == versions.size(),
        "clientbound_map_item_data is absent from a registered version"
    );
    ok &= check(
        expressionVersions != 0,
        "no logical-expression map schemas were exercised"
    );
    ok &= check(
        !sourceVersions.empty(),
        "no source protocol.json contains the logical-OR map schema"
    );
    for (const auto& version : sourceVersions) {
        const auto registered = std::find(
            versions.begin(),
            versions.end(),
            version
        ) != versions.end();
        const auto generated = bedrock::generatedProtocolTypeJson(
            version,
            "packet_clientbound_map_item_data"
        );
        ok &= check(registered, version + ": source schema is not registered");
        ok &= check(
            generated && generated->find(kMapScaleExpression) !=
                std::string::npos,
            version + ": generated C++ lost the source compareTo expression"
        );
    }

    // Mirror the reported 128x128, five-byte-per-colour packet on 1.21.100.
    if (std::find(versions.begin(), versions.end(), "1.21.100") !=
        versions.end()) {
        ok &= nodeGoldenRoundTrip();
        ok &= reportedFullMapRoundTrip(tsvTypes, "TSV");
        ok &= reportedFullMapRoundTrip(generatedTypes, "generated C++");
    }

    if (!ok) return 1;
    std::cout << "[MAP-ITEM-DATA-SMOKE] OK versions=" << checkedVersions
              << " source_expression_schemas=" << sourceVersions.size()
              << " registered_expression_versions=" << expressionVersions
              << '\n';
    return 0;
}
