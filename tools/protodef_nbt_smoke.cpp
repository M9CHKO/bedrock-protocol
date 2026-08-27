#include <bedrock/protocol/VersionedPacketCodec.hpp>
#include <bedrock/protodef/ProtoDefContext.hpp>
#include <bedrock/protodef/ProtoDefDecoder.hpp>
#include <bedrock/protodef/ProtoDefEncoder.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/protodef/ProtoDefReader.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>
#include <bedrock/protodef/ProtoDefWriter.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Value = bedrock::ProtoDefValue;

Value object(std::unordered_map<std::string, Value> fields) {
    return Value::object(std::move(fields));
}

Value array(std::vector<Value> values = {}) {
    return Value::array(std::move(values));
}

Value tag(std::string type, Value payload) {
    return object({
        {"type", Value::string(std::move(type))},
        {"value", std::move(payload)}
    });
}

Value document(std::string type, std::string name, Value payload) {
    auto out = tag(std::move(type), std::move(payload));
    out.objectValue["name"] = Value::string(std::move(name));
    return out;
}

std::vector<uint8_t> unhex(const std::string& text) {
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    if ((text.size() & 1u) != 0) {
        throw std::runtime_error("odd hex vector length");
    }
    std::vector<uint8_t> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = digit(text[i]);
        const int lo = digit(text[i + 1]);
        if (hi < 0 || lo < 0) throw std::runtime_error("invalid hex vector");
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
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

const Value* child(const Value& value, const std::string& name) {
    if (value.kind != Value::Kind::Object) return nullptr;
    return value.get(name);
}

const bedrock::ProtoDefField* field(
    const std::vector<bedrock::ProtoDefField>& fields,
    const std::string& path
) {
    const auto it = std::find_if(fields.begin(), fields.end(), [&](const auto& item) {
        return item.path == path;
    });
    return it == fields.end() ? nullptr : &*it;
}

Value richDocument() {
    return document("compound", "root", object({
        {"a", tag("int", Value::integer(-7))},
        {"b", tag("long", Value::integer(-9))},
        {"bytes", tag("byteArray", array({
            Value::integer(1),
            Value::integer(-1)
        }))},
        {"ia", tag("intArray", array({
            Value::integer(1),
            Value::integer(-2)
        }))},
        {"la", tag("longArray", array({
            array({Value::integer(0), Value::integer(3)}),
            array({Value::integer(-1), Value::integer(-4)})
        }))},
        {"list", tag("list", object({
            {"type", Value::string("short")},
            {"value", array({Value::integer(1), Value::integer(-2)})}
        }))}
    }));
}

std::vector<uint8_t> encodePrimitive(const std::string& type, const Value& value) {
    bedrock::ProtoDefEncoder encoder;
    bedrock::ProtoDefWriter writer;
    encoder.encode("\"" + type + "\"", value, writer);
    return writer.take();
}

std::vector<bedrock::ProtoDefField> decodePrimitive(
    const std::string& type,
    const std::vector<uint8_t>& bytes
) {
    bedrock::PacketFieldCursor cursor(bytes);
    bedrock::ProtoDefReader reader(cursor);
    bedrock::ProtoDefContext context;
    std::vector<bedrock::ProtoDefField> fields;
    bedrock::ProtoDefDecoder decoder;
    decoder.decode("\"" + type + "\"", reader, "value", fields, context);
    if (reader.remaining() != 0) {
        throw std::runtime_error(type + " decoder left trailing bytes");
    }
    return fields;
}

bool checkRichPrimitiveVectors() {
    bool ok = true;
    const auto value = richDocument();

    // Generated directly by node_modules/bedrock-protocol 3.53.0 with
    // prismarine-nbt's littleVarint and little serializers.
    const auto nbtGolden = unhex(
        "0a04726f6f740301610d04016211070562797465730401ff"
        "0b0269610401000000feffffff0c026c61040300000000000000"
        "fcffffffffffffff09046c69737402040100feff00"
    );
    const auto lnbtGolden = unhex(
        "0a0400726f6f7403010061f9ffffff04010062f7ffffffffffffff"
        "07050062797465730200000001ff0b020069610200000001000000"
        "feffffff0c02006c61020000000300000000000000fcffffffffffffff"
        "0904006c69737402020000000100feff00"
    );

    const auto encodedNbt = encodePrimitive("nbt", value);
    const auto encodedLnbt = encodePrimitive("lnbt", value);
    ok = sameBytes("littleVarint NBT golden", encodedNbt, nbtGolden) && ok;
    ok = sameBytes("little-endian NBT golden", encodedLnbt, lnbtGolden) && ok;

    for (const auto& [type, bytes] : std::vector<std::pair<std::string, std::vector<uint8_t>>>{
        {"nbt", nbtGolden},
        {"lnbt", lnbtGolden}
    }) {
        try {
            const auto fields = decodePrimitive(type, bytes);
            const auto* decoded = field(fields, "value");
            if (!decoded || !decoded->structuredValue.has_value()) {
                std::cerr << "[FAIL] " << type << " decoder omitted structured NBT\n";
                ok = false;
                continue;
            }
            const auto& root = *decoded->structuredValue;
            const auto* rootType = child(root, "type");
            const auto* rootName = child(root, "name");
            const auto* payload = child(root, "value");
            const auto* integer = payload ? child(*payload, "a") : nullptr;
            const auto* integerPayload = integer ? child(*integer, "value") : nullptr;
            if (!rootType || rootType->stringValue != "compound" ||
                !rootName || rootName->stringValue != "root" ||
                !integerPayload || integerPayload->kind != Value::Kind::Int ||
                integerPayload->intValue != -7 ||
                decoded->value.find("<nbt") != std::string::npos) {
                std::cerr << "[FAIL] " << type << " structured decode mismatch\n";
                ok = false;
            }
            ok = sameBytes(
                type + " decode/re-encode",
                encodePrimitive(type, root),
                bytes
            ) && ok;
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << type << " decode: " << e.what() << "\n";
            ok = false;
        }
    }

    auto legacy = object({
        {"tag", Value::string("list")},
        {"name", Value::string("legacy")},
        {"value", object({
            {"childTag", Value::string("int")},
            {"value", array({Value::integer(1), Value::integer(-2)})}
        })}
    });
    ok = sameBytes(
        "legacy C++ NBT shape",
        encodePrimitive("nbt", legacy),
        unhex("09066c656761637903040203")
    ) && ok;

    const auto end = document("end", "", Value::null());
    ok = sameBytes("nbt TAG_End", encodePrimitive("nbt", end), {0, 0}) && ok;
    ok = sameBytes("lnbt TAG_End", encodePrimitive("lnbt", end), {0}) && ok;
    try {
        const auto nbtEnd = decodePrimitive("nbt", {0, 0});
        const auto lnbtEnd = decodePrimitive("lnbt", {0});
        if (!field(nbtEnd, "value") || !field(lnbtEnd, "value")) {
            std::cerr << "[FAIL] TAG_End decode omitted field\n";
            ok = false;
        }
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] TAG_End decode: " << e.what() << "\n";
        ok = false;
    }

    return ok;
}

bool checkPacketVectors() {
    bool ok = true;
    const std::string version = "1.21.100";
    bedrock::ProtoDefPacketEncoder encoder(version);
    bedrock::ProtoDefPacketDecoder decoder(version);
    const auto codec = bedrock::VersionedPacketCodec::forVersion(version);

    auto blockNbt = document("compound", "", object({
        {"CustomName", tag("string", Value::string("Bedrock"))},
        {"Items", tag("list", object({
            {"type", Value::string("int")},
            {"value", array({Value::integer(1), Value::integer(-2)})}
        }))},
        {"x", tag("int", Value::integer(1))}
    }));
    auto blockPacket = object({
        {"position", object({
            {"x", Value::integer(1)},
            {"y", Value::integer(-2)},
            {"z", Value::integer(3)}
        })},
        {"nbt", blockNbt}
    });

    try {
        const auto payload = encoder.encodePacket("block_entity_data", blockPacket);
        const auto full = codec.encodeFullPacketByName("block_entity_data", payload);
        const auto golden = unhex(
            "3802feffffff0f060a00080a437573746f6d4e616d6507426564726f636b"
            "09054974656d73030402030301780200"
        );
        ok = sameBytes("block_entity_data Node packet", full, golden) && ok;

        const auto fields = decoder.decodePacket("block_entity_data", payload);
        const auto* nbt = field(fields, "nbt");
        const auto* rootName = nbt && nbt->structuredValue.has_value()
            ? child(*nbt->structuredValue, "name")
            : nullptr;
        if (!rootName || rootName->kind != Value::Kind::String ||
            !rootName->stringValue.empty()) {
            std::cerr << "[FAIL] block_entity_data structured NBT decode\n";
            ok = false;
        }
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] block_entity_data packet: " << e.what() << "\n";
        ok = false;
    }

    auto loop = array({
        document("int", "alpha", Value::integer(-7)),
        document("string", "beta", Value::string("ok"))
    });
    auto eventPacket = object({
        {"event_id", Value::uinteger(42)},
        {"nbt", loop}
    });

    try {
        const auto payload = encoder.encodePacket("level_event_generic", eventPacket);
        const auto full = codec.encodeFullPacketByName("level_event_generic", payload);
        const auto golden = unhex("7c2a0305616c7068610d080462657461026f6b00");
        ok = sameBytes("level_event_generic Node packet", full, golden) && ok;

        const auto fields = decoder.decodePacket("level_event_generic", payload);
        const auto* nbt = field(fields, "nbt");
        if (!nbt || !nbt->structuredValue.has_value() ||
            nbt->structuredValue->kind != Value::Kind::Array ||
            nbt->structuredValue->arrayValue.size() != 2 ||
            nbt->value.find("<nbtLoop>") != std::string::npos) {
            std::cerr << "[FAIL] level_event_generic NBT loop decode\n";
            ok = false;
        }
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] level_event_generic packet: " << e.what() << "\n";
        ok = false;
    }

    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = checkRichPrimitiveVectors() && ok;
    ok = checkPacketVectors() && ok;

    if (!ok) return 1;
    std::cout << "[PROTODEF-NBT] Node golden vectors, structured decode, and packet integration ok\n";
    return 0;
}
