#include <bedrock/bedrock.hpp>
#include <bedrock/debug/ProtocolTypeTsvIndex.hpp>
#include <bedrock/protocol/VersionedPacketCodec.hpp>
#include <bedrock/protodef/ProtoDefContext.hpp>
#include <bedrock/protodef/ProtoDefDecoder.hpp>
#include <bedrock/protodef/ProtoDefEncoder.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/protodef/ProtoDefReader.hpp>
#include <bedrock/protodef/ProtoDefWriter.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Value = bedrock::ProtoDefValue;

std::vector<uint8_t> unhex(const std::string& text) {
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if ((text.size() & 1u) != 0) throw std::runtime_error("odd hex length");
    std::vector<uint8_t> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = digit(text[i]);
        const int low = digit(text[i + 1]);
        if (high < 0 || low < 0) throw std::runtime_error("invalid hex");
        out.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return out;
}

bool sameBytes(
    const std::string& label,
    const std::vector<uint8_t>& actual,
    const std::vector<uint8_t>& expected
) {
    if (actual == expected) return true;
    std::cerr << "[FAIL] " << label << " byte mismatch: actual="
              << actual.size() << " expected=" << expected.size() << "\n";
    return false;
}

const bedrock::ProtoDefField* field(
    const std::vector<bedrock::ProtoDefField>& fields,
    const std::string& path
) {
    const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& item) {
        return item.path == path;
    });
    return found == fields.end() ? nullptr : &*found;
}

std::vector<bedrock::ProtoDefField> decodePrimitive(
    const std::string& typeJson,
    const std::vector<uint8_t>& bytes,
    std::size_t* remaining = nullptr
) {
    bedrock::PacketFieldCursor cursor(bytes);
    bedrock::ProtoDefReader reader(cursor);
    bedrock::ProtoDefContext context;
    std::vector<bedrock::ProtoDefField> fields;
    bedrock::ProtoDefDecoder decoder;
    decoder.decode(typeJson, reader, "value", fields, context);
    if (remaining) *remaining = reader.remaining();
    return fields;
}

std::string protocolType(const std::string& version, const std::string& name) {
    bedrock::ProtocolTypeTsvIndex index;
    auto type = index.findTypeJson(version, name);
    if (!type.has_value()) {
        throw std::runtime_error("missing generated protocol type: " + version + "/" + name);
    }
    return *type;
}

std::vector<uint8_t> encodePrimitive(
    const std::string& typeJson,
    const Value& value
) {
    bedrock::ProtoDefEncoder encoder;
    bedrock::ProtoDefWriter writer;
    encoder.encode(typeJson, value, writer);
    return writer.take();
}

bool checkPrimitiveStructure() {
    bool ok = true;
    try {
        const std::string arrayType =
            "[\"array\",{\"countType\":\"zigzag32\",\"type\":\"u8\"}]";
        bedrock::ProtoDefEncoder encoder;
        bedrock::ProtoDefWriter writer;
        encoder.encode(arrayType, Value::array({
            Value::uinteger(7),
            Value::uinteger(8)
        }), writer);
        const auto encoded = writer.take();
        ok = sameBytes("zigzag32 array count", encoded, {0x04, 0x07, 0x08}) && ok;

        std::size_t remaining = 0;
        const auto fields = decodePrimitive(arrayType, encoded, &remaining);
        if (remaining != 0 || !field(fields, "value[0]") ||
            field(fields, "value[0]")->value != "7" ||
            !field(fields, "value[1]") || field(fields, "value[1]")->value != "8") {
            std::cerr << "[FAIL] zigzag32 array count decode\n";
            ok = false;
        }

        const auto signedByte = decodePrimitive("\"li8\"", {0xff});
        const auto unsignedByte = decodePrimitive("\"lu8\"", {0xff});
        if (!field(signedByte, "value") || field(signedByte, "value")->value != "-1" ||
            !field(unsignedByte, "value") || field(unsignedByte, "value")->value != "255") {
            std::cerr << "[FAIL] li8/lu8 decode\n";
            ok = false;
        }

        const auto uuid = decodePrimitive(
            "\"uuid\"",
            unhex("00112233445566778899aabbccddeeff")
        );
        if (!field(uuid, "value") ||
            field(uuid, "value")->value != "00112233-4455-6677-8899-aabbccddeeff") {
            std::cerr << "[FAIL] canonical UUID decode\n";
            ok = false;
        }

        const std::string expressionType =
            R"(["container",[{"name":"a","type":"bool"},{"name":"b","type":"bool"},{"name":"c","type":"bool"},{"name":"selected","type":["switch",{"compareTo":"a || b || c","fields":{"true":"u8","false":"u16"}}]}]])";
        const auto trueExpression = Value::object({
            {"a", Value::boolean(false)},
            {"b", Value::boolean(true)},
            {"c", Value::boolean(false)},
            {"selected", Value::uinteger(0x7f)}
        });
        const auto falseExpression = Value::object({
            {"a", Value::boolean(false)},
            {"b", Value::boolean(false)},
            {"c", Value::boolean(false)},
            {"selected", Value::uinteger(0x1234)}
        });
        const auto multipleTrueExpression = Value::object({
            {"a", Value::boolean(true)},
            {"b", Value::boolean(true)},
            {"c", Value::boolean(false)},
            {"selected", Value::uinteger(0x55)}
        });
        const auto trueBytes = encodePrimitive(expressionType, trueExpression);
        const auto falseBytes = encodePrimitive(expressionType, falseExpression);
        const auto multipleTrueBytes = encodePrimitive(
            expressionType,
            multipleTrueExpression
        );
        ok = sameBytes(
            "compareTo boolean expression true branch",
            trueBytes,
            {0x00, 0x01, 0x00, 0x7f}
        ) && ok;
        ok = sameBytes(
            "compareTo boolean expression false branch",
            falseBytes,
            {0x00, 0x00, 0x00, 0x12, 0x34}
        ) && ok;
        ok = sameBytes(
            "compareTo boolean expression multiple-true branch",
            multipleTrueBytes,
            {0x01, 0x01, 0x00, 0x55}
        ) && ok;
        const auto trueFields = decodePrimitive(
            expressionType,
            trueBytes,
            &remaining
        );
        if (remaining != 0 || !field(trueFields, "value.selected") ||
            field(trueFields, "value.selected")->value != "127") {
            std::cerr << "[FAIL] compareTo boolean expression decode\n";
            ok = false;
        }

        const std::string scalarSwitchType =
            R"(["container",[{"name":"selected","type":"bool"},{"name":"value","type":["switch",{"compareTo":"selected","fields":{"true":"u8","false":"u16"}}]}]])";
        const auto scalarBytes = encodePrimitive(
            scalarSwitchType,
            Value::object({
                {"selected", Value::boolean(false)},
                {"value", Value::uinteger(0x4567)}
            })
        );
        ok = sameBytes(
            "ordinary scalar compareTo",
            scalarBytes,
            {0x00, 0x45, 0x67}
        ) && ok;
        const auto scalarFields = decodePrimitive(
            scalarSwitchType,
            scalarBytes,
            &remaining
        );
        if (remaining != 0 || !field(scalarFields, "value.value") ||
            field(scalarFields, "value.value")->value != "17767") {
            std::cerr << "[FAIL] ordinary scalar compareTo decode\n";
            ok = false;
        }

        // Array item switches resolve against a field on the containing
        // packet via "../". This must remain correct without embedding a
        // deep copy of the packet (and its array) into every item.
        const std::string parentSwitchType =
            R"(["container",[{"name":"action","type":"u8"},{"name":"entries","type":["array",{"countType":"u8","type":["container",[{"name":"value","type":["switch",{"compareTo":"../action","fields":{"1":"u8","2":"u16"}}]}]]}]}]])";
        const auto parentSwitchBytes = encodePrimitive(
            parentSwitchType,
            Value::object({
                {"action", Value::uinteger(2)},
                {"entries", Value::array({
                    Value::object({{"value", Value::uinteger(0x1234)}}),
                    Value::object({{"value", Value::uinteger(0x5678)}})
                })}
            })
        );
        ok = sameBytes(
            "array item parent compareTo",
            parentSwitchBytes,
            {0x02, 0x02, 0x12, 0x34, 0x56, 0x78}
        ) && ok;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] primitive structured decode: " << error.what() << "\n";
        ok = false;
    }
    return ok;
}

bool checkBedrockBitflags() {
    bool ok = true;
    try {
        // Exact node_modules/bedrock-protocol 3.53.0 raw type vectors.
        const auto inputType = protocolType("1.18.0", "InputFlag");
        const auto inputGolden = unhex("808080808002");
        std::size_t remaining = 0;
        const auto input = decodePrimitive(inputType, inputGolden, &remaining);
        const auto* inputRaw = field(input, "value");
        const auto* itemStackRequest = field(input, "value.item_stack_request");
        const auto* blockAction = field(input, "value.block_action");
        const auto* structuredRaw = inputRaw && inputRaw->structuredValue.has_value()
            ? inputRaw->structuredValue->get("_value")
            : nullptr;
        if (remaining != 0 || !inputRaw || inputRaw->size != inputGolden.size() ||
            !structuredRaw || structuredRaw->kind != Value::Kind::UInt ||
            structuredRaw->uintValue != (uint64_t{1} << 36) ||
            !itemStackRequest || itemStackRequest->value != "true" ||
            !blockAction || blockAction->value != "false") {
            std::cerr << "[FAIL] varint64 bitflags decode\n";
            ok = false;
        }

        ok = sameBytes(
            "varint64 bitflags encode",
            encodePrimitive(inputType, Value::object({
                {"_value", Value::uinteger(uint64_t{1} << 36)},
                {"item_stack_request", Value::boolean(false)}
            })),
            inputGolden
        ) && ok;

        const auto metadataType = protocolType("1.18.0", "MetadataFlags1");
        const auto metadataGolden = unhex("ffffffffffffffffff01");
        const auto metadata = decodePrimitive(metadataType, metadataGolden, &remaining);
        const auto* metadataRaw = field(metadata, "value");
        const auto* layingDown = field(metadata, "value.laying_down");
        const auto* metadataStructuredRaw = metadataRaw && metadataRaw->structuredValue.has_value()
            ? metadataRaw->structuredValue->get("_value")
            : nullptr;
        if (remaining != 0 || !metadataStructuredRaw ||
            metadataStructuredRaw->kind != Value::Kind::Int ||
            metadataStructuredRaw->intValue != std::numeric_limits<int64_t>::min() ||
            !layingDown || layingDown->value != "true") {
            std::cerr << "[FAIL] zigzag64 bitflags decode\n";
            ok = false;
        }

        ok = sameBytes(
            "zigzag64 bitflags encode",
            encodePrimitive(metadataType, Value::object({
                {"_value", Value::integer(std::numeric_limits<int64_t>::min())},
                {"laying_down", Value::boolean(false)}
            })),
            metadataGolden
        ) && ok;

        const auto permissionsType = protocolType("1.18.0", "ActionPermissions");
        const auto permissionsGolden = unhex("818004");
        const auto permissions = decodePrimitive(permissionsType, permissionsGolden, &remaining);
        const auto* mine = field(permissions, "value.mine");
        const auto* doors = field(permissions, "value.doors_and_switches");
        if (remaining != 0 || !mine || mine->value != "true" ||
            !doors || doors->value != "false") {
            std::cerr << "[FAIL] composite bitflag mask decode\n";
            ok = false;
        }

        ok = sameBytes(
            "bitflags raw-mask preservation",
            encodePrimitive(permissionsType, Value::object({
                {"_value", Value::uinteger(65537)},
                {"mine", Value::boolean(false)},
                {"unrelated", Value::boolean(true)}
            })),
            permissionsGolden
        ) && ok;

        const auto input128Type = protocolType("1.21.100", "InputFlag");
        const auto input128Golden = unhex("80808080808080808002");
        const auto input128 = decodePrimitive(input128Type, input128Golden, &remaining);
        const auto* input128Raw = field(input128, "value");
        const auto* input128StructuredRaw = input128Raw && input128Raw->structuredValue.has_value()
            ? input128Raw->structuredValue->get("_value")
            : nullptr;
        if (remaining != 0 || !input128StructuredRaw ||
            input128StructuredRaw->kind != Value::Kind::String ||
            input128StructuredRaw->stringValue != "18446744073709551616") {
            std::cerr << "[FAIL] varint128 bitflags decode\n";
            ok = false;
        }
        ok = sameBytes(
            "varint128 bitflags encode",
            encodePrimitive(input128Type, Value::object({
                {"_value", Value::string("18446744073709551616")}
            })),
            input128Golden
        ) && ok;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] Bedrock bitflags: " << error.what() << "\n";
        ok = false;
    }
    return ok;
}

bool checkPlayerAuthInputBitflagsRoundTrip() {
    constexpr const char* version = "1.18.0";
    bool ok = true;
    try {
        // Exact node_modules/bedrock-protocol 3.53.0 full packet vector. The
        // block_action flag is bit 35, so a 32-bit bitflags reader cannot
        // reach the following fields or the empty block_action array.
        const auto golden = unhex(
            "90010000803f0000004000004040000080400000a0400000803e000000bf"
            "0000c04080808080800101000700000000000000000000000000"
        );
        const auto codec = bedrock::VersionedPacketCodec::forVersion(version);
        const auto packet = codec.decodeFullPacket(golden);
        bedrock::ProtoDefPacketDecoder decoder(version);
        const auto fields = decoder.decodePacket(packet.name, packet.payload);
        const auto* blockActionFlag = field(fields, "input_data.block_action");
        const auto* tick = field(fields, "tick");
        const auto* blockActionCount = field(fields, "block_action.$count");
        if (!blockActionFlag || blockActionFlag->value != "true" ||
            !tick || tick->value != "7" ||
            !blockActionCount || blockActionCount->value != "0") {
            std::cerr << "[FAIL] player_auth_input high bitflag packet decode\n";
            ok = false;
        }

        bedrock::BedrockRelayPacketEvent relayEvent;
        relayEvent.packet = packet;
        bedrock::RelayPacketEvent wrapped(version, relayEvent);
        bedrock::ProtoDefPacketEncoder encoder(version);
        const auto& decodedParams = wrapped.decodedParams();
        const auto decodedBlockAction = decodedParams.find("block_action");
        if (decodedBlockAction == decodedParams.end() ||
            decodedBlockAction->second.kind != Value::Kind::Array) {
            std::cerr << "[FAIL] reconstructed block_action is not an array\n";
            ok = false;
        }
        if (decodedParams.contains("gaze_direction") ||
            decodedParams.contains("transaction") ||
            decodedParams.contains("item_stack_request")) {
            std::cerr << "[FAIL] inactive switch branches became placeholder values\n";
            ok = false;
        }
        const auto reconstructed = codec.encodeFullPacketByName(
            packet.name,
            encoder.encodePacket(packet.name, Value::object(decodedParams))
        );
        ok = sameBytes("player_auth_input bitflags relay round-trip", reconstructed, golden) && ok;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] player_auth_input bitflags round-trip: "
                  << error.what() << "\n";
        ok = false;
    }
    return ok;
}

const Value* nested(const Value& root, std::initializer_list<const char*> path) {
    const Value* current = &root;
    for (const char* part : path) {
        if (!current || current->kind != Value::Kind::Object) return nullptr;
        current = current->get(part);
    }
    return current;
}

bool checkBufferAndRelayReconstruction() {
    constexpr const char* version = "1.21.100";
    const auto codec = bedrock::VersionedPacketCodec::forVersion(version);
    bedrock::ProtoDefPacketEncoder encoder(version);
    bedrock::ProtoDefPacketDecoder decoder(version);
    bool ok = true;

    try {
        // Generated by node_modules/bedrock-protocol 3.53.0.
        const auto golden = unhex(
            "5303616263020000000500000000000000040001feff"
        );
        const auto packet = codec.decodeFullPacket(golden);
        const auto fields = decoder.decodePacket(packet.name, packet.payload);
        const auto* payload = field(fields, "payload");
        if (!payload || !payload->structuredValue.has_value() ||
            payload->structuredValue->kind != Value::Kind::Bytes ||
            payload->structuredValue->bytesValue != std::vector<uint8_t>({0, 1, 0xfe, 0xff})) {
            std::cerr << "[FAIL] ByteArray structured decode\n";
            ok = false;
        }

        bedrock::BedrockRelayPacketEvent relayEvent;
        relayEvent.packet = packet;
        bedrock::RelayPacketEvent wrapped(version, relayEvent);
        const auto reconstructed = codec.encodeFullPacketByName(
            packet.name,
            encoder.encodePacket(packet.name, Value::object(wrapped.decodedParams()))
        );
        ok = sameBytes("ByteArray relay reconstruction", reconstructed, golden) && ok;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] ByteArray relay reconstruction: " << error.what() << "\n";
        ok = false;
    }

    return ok;
}

bool checkItemStructuralMarkers() {
    constexpr const char* version = "1.21.100";
    const auto codec = bedrock::VersionedPacketCodec::forVersion(version);
    const auto variables = bedrock::makeProtoDefVariableStore();
    variables->setVariable("ShieldItemID", 513);
    bedrock::ProtoDefPacketEncoder encoder(version, variables);
    bool ok = true;

    try {
        const auto golden = unhex(
            "3200001d00008208010000000012000000000000000000007b00000000000000"
        );
        bedrock::BedrockRelayPacketEvent relayEvent;
        relayEvent.packet = codec.decodeFullPacket(golden);
        bedrock::RelayPacketEvent wrapped(version, relayEvent, variables);
        const auto& params = wrapped.decodedParams();
        const Value paramsValue = Value::object(params);

        const auto* option = nested(paramsValue, {"container", "dynamic_container_id"});
        const auto* canPlace = nested(paramsValue, {"item", "extra", "can_place_on"});
        const auto* canDestroy = nested(paramsValue, {"item", "extra", "can_destroy"});
        const auto* airNetworkId = nested(paramsValue, {"storage_item", "network_id"});
        if (!option || option->kind != Value::Kind::Null ||
            !canPlace || canPlace->kind != Value::Kind::Array || !canPlace->arrayValue.empty() ||
            !canDestroy || canDestroy->kind != Value::Kind::Array || !canDestroy->arrayValue.empty() ||
            !airNetworkId || airNetworkId->kind != Value::Kind::Int || airNetworkId->intValue != 0) {
            std::cerr << "[FAIL] relay option/empty-array/void reconstruction\n";
            ok = false;
        }

        const auto reconstructed = codec.encodeFullPacketByName(
            "inventory_slot",
            encoder.encodePacket("inventory_slot", paramsValue)
        );
        ok = sameBytes("inventory_slot relay reconstruction", reconstructed, golden) && ok;

        wrapped.set("slot", static_cast<uint64_t>(1));
        const auto modified = codec.encodeFullPacketByName(
            "inventory_slot",
            encoder.encodePacket("inventory_slot", Value::object(wrapped.decodedParams()))
        );
        auto expected = golden;
        expected[2] = 0x01;
        ok = sameBytes("inventory_slot adjacent-field mutation", modified, expected) && ok;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] inventory_slot structural reconstruction: "
                  << error.what() << "\n";
        ok = false;
    }

    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = checkPrimitiveStructure() && ok;
    ok = checkBedrockBitflags() && ok;
    ok = checkPlayerAuthInputBitflagsRoundTrip() && ok;
    ok = checkBufferAndRelayReconstruction() && ok;
    ok = checkItemStructuralMarkers() && ok;
    if (!ok) return 1;
    std::cout << "[PROTODEF-STRUCTURED] counts, parent paths, bitflags, binary fields, options, arrays, and relay reconstruction ok\n";
    return 0;
}
