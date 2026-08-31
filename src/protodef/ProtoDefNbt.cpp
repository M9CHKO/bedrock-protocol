#include <bedrock/protodef/ProtoDefNbt.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bedrock {
namespace {

const ProtoDefValue& requiredField(
    const ProtoDefValue& object,
    const char* key
) {
    if (object.kind != ProtoDefValue::Kind::Object) {
        throw BedrockNbtError("NBT tag must be an object");
    }
    const auto* value = object.get(key);
    if (!value) {
        throw BedrockNbtError(std::string("NBT tag is missing field: ") + key);
    }
    return *value;
}

std::string stringValue(const ProtoDefValue& value, const char* what) {
    if (value.kind != ProtoDefValue::Kind::String) {
        throw BedrockNbtError(std::string(what) + " must be a string");
    }
    return value.stringValue;
}

int64_t integerValue(const ProtoDefValue& value, const char* what) {
    switch (value.kind) {
        case ProtoDefValue::Kind::Int:
            return value.intValue;
        case ProtoDefValue::Kind::UInt:
            return std::bit_cast<int64_t>(value.uintValue);
        case ProtoDefValue::Kind::Bool:
            return value.boolValue ? 1 : 0;
        case ProtoDefValue::Kind::Double:
            return static_cast<int64_t>(value.doubleValue);
        case ProtoDefValue::Kind::String:
            try {
                return std::stoll(value.stringValue);
            } catch (...) {
                throw BedrockNbtError(std::string(what) + " is not a signed integer");
            }
        default:
            throw BedrockNbtError(std::string(what) + " must be an integer");
    }
}

double floatingValue(const ProtoDefValue& value, const char* what) {
    switch (value.kind) {
        case ProtoDefValue::Kind::Double:
            return value.doubleValue;
        case ProtoDefValue::Kind::Int:
            return static_cast<double>(value.intValue);
        case ProtoDefValue::Kind::UInt:
            return static_cast<double>(value.uintValue);
        case ProtoDefValue::Kind::Bool:
            return value.boolValue ? 1.0 : 0.0;
        default:
            throw BedrockNbtError(std::string(what) + " must be numeric");
    }
}

template <typename Integer>
Integer checkedInteger(const ProtoDefValue& value, const char* what) {
    const int64_t number = integerValue(value, what);
    if (number < static_cast<int64_t>(std::numeric_limits<Integer>::min()) ||
        number > static_cast<int64_t>(std::numeric_limits<Integer>::max())) {
        throw BedrockNbtError(std::string(what) + " is out of range");
    }
    return static_cast<Integer>(number);
}

int64_t longValue(const ProtoDefValue& value, const char* what) {
    if (value.kind != ProtoDefValue::Kind::Array) {
        return integerValue(value, what);
    }
    if (value.arrayValue.size() != 2) {
        throw BedrockNbtError(std::string(what) + " word pair must contain [high, low]");
    }

    const auto high = checkedInteger<int32_t>(value.arrayValue[0], what);
    const auto low = checkedInteger<int32_t>(value.arrayValue[1], what);
    const uint64_t bits =
        (static_cast<uint64_t>(static_cast<uint32_t>(high)) << 32u) |
        static_cast<uint32_t>(low);
    return std::bit_cast<int64_t>(bits);
}

std::string tagNameFromNode(const ProtoDefValue& value) {
    if (value.kind != ProtoDefValue::Kind::Object) {
        throw BedrockNbtError("NBT tag must be an object");
    }
    if (const auto* type = value.get("type")) {
        return stringValue(*type, "NBT type");
    }
    // Compatibility with the first C++ packet encoder shape.
    if (const auto* tag = value.get("tag")) {
        return stringValue(*tag, "NBT tag");
    }
    throw BedrockNbtError("NBT tag is missing type");
}

const ProtoDefValue& listPayload(const ProtoDefValue& payload) {
    if (payload.kind != ProtoDefValue::Kind::Object) {
        throw BedrockNbtError("NBT list value must be an object");
    }
    return requiredField(payload, "value");
}

NbtTagType listElementType(const ProtoDefValue& payload) {
    if (const auto* type = payload.get("type")) {
        return protoDefNbtTagType(stringValue(*type, "NBT list type"));
    }
    if (const auto* type = payload.get("childTag")) {
        return protoDefNbtTagType(stringValue(*type, "NBT list childTag"));
    }
    throw BedrockNbtError("NBT list is missing element type");
}

std::vector<uint8_t> byteArrayValue(const ProtoDefValue& payload) {
    if (payload.kind == ProtoDefValue::Kind::Bytes) {
        return payload.bytesValue;
    }
    if (payload.kind != ProtoDefValue::Kind::Array) {
        throw BedrockNbtError("NBT byteArray value must be bytes or an array");
    }

    std::vector<uint8_t> out;
    out.reserve(payload.arrayValue.size());
    for (const auto& item : payload.arrayValue) {
        out.push_back(static_cast<uint8_t>(checkedInteger<int8_t>(item, "NBT byteArray item")));
    }
    return out;
}

ProtoDefValue tagNode(std::string type, ProtoDefValue payload) {
    return ProtoDefValue::object({
        {"type", ProtoDefValue::string(std::move(type))},
        {"value", std::move(payload)}
    });
}

} // namespace

NbtTagType protoDefNbtTagType(std::string_view name) {
    if (name == "end") return NbtTagType::End;
    if (name == "byte") return NbtTagType::Byte;
    if (name == "short") return NbtTagType::Short;
    if (name == "int") return NbtTagType::Int;
    if (name == "long") return NbtTagType::Long;
    if (name == "float") return NbtTagType::Float;
    if (name == "double") return NbtTagType::Double;
    if (name == "byteArray") return NbtTagType::ByteArray;
    if (name == "string") return NbtTagType::String;
    if (name == "list") return NbtTagType::List;
    if (name == "compound") return NbtTagType::Compound;
    if (name == "intArray") return NbtTagType::IntArray;
    if (name == "longArray") return NbtTagType::LongArray;
    throw BedrockNbtError("unknown NBT tag type: " + std::string(name));
}

std::string_view protoDefNbtTagName(NbtTagType type) {
    switch (type) {
        case NbtTagType::End: return "end";
        case NbtTagType::Byte: return "byte";
        case NbtTagType::Short: return "short";
        case NbtTagType::Int: return "int";
        case NbtTagType::Long: return "long";
        case NbtTagType::Float: return "float";
        case NbtTagType::Double: return "double";
        case NbtTagType::ByteArray: return "byteArray";
        case NbtTagType::String: return "string";
        case NbtTagType::List: return "list";
        case NbtTagType::Compound: return "compound";
        case NbtTagType::IntArray: return "intArray";
        case NbtTagType::LongArray: return "longArray";
    }
    throw BedrockNbtError("invalid NBT tag type");
}

NbtValue protoDefValueToNbtValue(const ProtoDefValue& value) {
    if (value.kind == ProtoDefValue::Kind::Null) {
        return NbtValue::end();
    }

    const auto type = protoDefNbtTagType(tagNameFromNode(value));
    if (type == NbtTagType::End) {
        return NbtValue::end();
    }
    const auto& payload = requiredField(value, "value");

    switch (type) {
        case NbtTagType::End:
            return NbtValue::end();
        case NbtTagType::Byte:
            return NbtValue::byte(checkedInteger<int8_t>(payload, "NBT byte value"));
        case NbtTagType::Short:
            return NbtValue::shortInteger(checkedInteger<int16_t>(payload, "NBT short value"));
        case NbtTagType::Int:
            return NbtValue::integer(checkedInteger<int32_t>(payload, "NBT int value"));
        case NbtTagType::Long:
            return NbtValue::longInteger(longValue(payload, "NBT long value"));
        case NbtTagType::Float:
            return NbtValue::floating(static_cast<float>(floatingValue(payload, "NBT float value")));
        case NbtTagType::Double:
            return NbtValue::doubleFloating(floatingValue(payload, "NBT double value"));
        case NbtTagType::ByteArray:
            return NbtValue::byteArray(byteArrayValue(payload));
        case NbtTagType::String:
            return NbtValue::string(stringValue(payload, "NBT string value"));
        case NbtTagType::List: {
            const auto elementType = listElementType(payload);
            const auto& values = listPayload(payload);
            if (values.kind != ProtoDefValue::Kind::Array) {
                throw BedrockNbtError("NBT list items must be an array");
            }
            std::vector<NbtValue> out;
            out.reserve(values.arrayValue.size());
            for (const auto& item : values.arrayValue) {
                ProtoDefValue child = tagNode(std::string(protoDefNbtTagName(elementType)), item);
                out.push_back(protoDefValueToNbtValue(child));
            }
            return NbtValue::list(elementType, std::move(out));
        }
        case NbtTagType::Compound: {
            if (payload.kind != ProtoDefValue::Kind::Object) {
                throw BedrockNbtError("NBT compound value must be an object");
            }
            std::vector<std::string> names;
            names.reserve(payload.objectValue.size());
            for (const auto& [name, child] : payload.objectValue) {
                (void) child;
                names.push_back(name);
            }
            // ProtoDefValue stores objects in an unordered_map. Sorting gives
            // deterministic bytes while preserving NBT compound semantics.
            std::sort(names.begin(), names.end());

            std::vector<NbtNamedValue> out;
            out.reserve(names.size());
            for (const auto& name : names) {
                const auto& child = payload.objectValue.at(name);
                auto converted = protoDefValueToNbtValue(child);
                if (converted.type == NbtTagType::End) {
                    throw BedrockNbtError("NBT compound cannot contain a named TAG_End");
                }
                out.push_back({name, std::move(converted)});
            }
            return NbtValue::compound(std::move(out));
        }
        case NbtTagType::IntArray: {
            if (payload.kind != ProtoDefValue::Kind::Array) {
                throw BedrockNbtError("NBT intArray value must be an array");
            }
            std::vector<int32_t> out;
            out.reserve(payload.arrayValue.size());
            for (const auto& item : payload.arrayValue) {
                out.push_back(checkedInteger<int32_t>(item, "NBT intArray item"));
            }
            return NbtValue::intArray(std::move(out));
        }
        case NbtTagType::LongArray: {
            if (payload.kind != ProtoDefValue::Kind::Array) {
                throw BedrockNbtError("NBT longArray value must be an array");
            }
            std::vector<int64_t> out;
            out.reserve(payload.arrayValue.size());
            for (const auto& item : payload.arrayValue) {
                out.push_back(longValue(item, "NBT longArray item"));
            }
            return NbtValue::longArray(std::move(out));
        }
    }

    throw BedrockNbtError("unreachable NBT conversion");
}

ProtoDefValue nbtValueToProtoDefValue(const NbtValue& value) {
    switch (value.type) {
        case NbtTagType::End:
            return tagNode("end", ProtoDefValue::null());
        case NbtTagType::Byte:
        case NbtTagType::Short:
        case NbtTagType::Int:
        case NbtTagType::Long:
            return tagNode(
                std::string(protoDefNbtTagName(value.type)),
                ProtoDefValue::integer(value.integerValue)
            );
        case NbtTagType::Float:
        case NbtTagType::Double:
            return tagNode(
                std::string(protoDefNbtTagName(value.type)),
                ProtoDefValue::floating(value.floatingValue)
            );
        case NbtTagType::ByteArray: {
            std::vector<ProtoDefValue> values;
            values.reserve(value.byteArrayValue.size());
            for (const uint8_t item : value.byteArrayValue) {
                values.push_back(ProtoDefValue::integer(static_cast<int8_t>(item)));
            }
            return tagNode("byteArray", ProtoDefValue::array(std::move(values)));
        }
        case NbtTagType::String:
            return tagNode("string", ProtoDefValue::string(value.stringValue));
        case NbtTagType::List: {
            std::vector<ProtoDefValue> values;
            values.reserve(value.listValue.size());
            for (const auto& item : value.listValue) {
                auto child = nbtValueToProtoDefValue(item);
                values.push_back(requiredField(child, "value"));
            }
            return tagNode("list", ProtoDefValue::object({
                {"type", ProtoDefValue::string(std::string(protoDefNbtTagName(value.listElementType)))},
                {"value", ProtoDefValue::array(std::move(values))}
            }));
        }
        case NbtTagType::Compound: {
            std::unordered_map<std::string, ProtoDefValue> values;
            values.reserve(value.compoundValue.size());
            for (const auto& child : value.compoundValue) {
                values[child.name] = nbtValueToProtoDefValue(child.value);
            }
            return tagNode("compound", ProtoDefValue::object(std::move(values)));
        }
        case NbtTagType::IntArray: {
            std::vector<ProtoDefValue> values;
            values.reserve(value.intArrayValue.size());
            for (const int32_t item : value.intArrayValue) {
                values.push_back(ProtoDefValue::integer(item));
            }
            return tagNode("intArray", ProtoDefValue::array(std::move(values)));
        }
        case NbtTagType::LongArray: {
            std::vector<ProtoDefValue> values;
            values.reserve(value.longArrayValue.size());
            for (const int64_t item : value.longArrayValue) {
                values.push_back(ProtoDefValue::integer(item));
            }
            return tagNode("longArray", ProtoDefValue::array(std::move(values)));
        }
    }

    throw BedrockNbtError("unreachable NBT conversion");
}

NbtDocument protoDefValueToNbtDocument(const ProtoDefValue& value) {
    if (value.kind == ProtoDefValue::Kind::Null) {
        return {{}, NbtValue::end()};
    }
    std::string name;
    if (value.kind == ProtoDefValue::Kind::Object) {
        if (const auto* nodeName = value.get("name")) {
            name = stringValue(*nodeName, "NBT root name");
        }
    }
    return {std::move(name), protoDefValueToNbtValue(value)};
}

ProtoDefValue nbtDocumentToProtoDefValue(const NbtDocument& document) {
    auto out = nbtValueToProtoDefValue(document.root);
    out.objectValue["name"] = ProtoDefValue::string(document.name);
    return out;
}

void writeProtoDefNbt(
    ProtoDefWriter& writer,
    const ProtoDefValue& value,
    BedrockNbtEncoding encoding
) {
    if (value.kind == ProtoDefValue::Kind::Bytes) {
        writer.bytes(value.bytesValue);
        return;
    }

    const auto document = protoDefValueToNbtDocument(value);
    if (document.root.type == NbtTagType::End) {
        writer.u8(0);
        // prismarine-nbt's littleVarint nbt type still writes the root name
        // for TAG_End. bedrock-protocol's fixed little-endian wrapper emits
        // only the discriminator byte.
        if (encoding == BedrockNbtEncoding::LittleVarInt) {
            writer.string(document.name);
        }
        return;
    }

    BinaryStream stream;
    BedrockNbtCodec::write(stream, document, encoding);
    writer.bytes(stream.buffer());
}

ProtoDefValue readProtoDefNbt(
    ProtoDefReader& reader,
    BedrockNbtEncoding encoding
) {
    if (reader.remaining() == 0) {
        throw BedrockNbtError("not enough bytes for NBT root tag");
    }

    const auto start = reader.offset();
    BinaryStream stream = BinaryStream::view(reader.data(), start);
    NbtDocument document;

    if (reader.data()[start] == static_cast<uint8_t>(NbtTagType::End)) {
        stream.readU8();
        if (encoding == BedrockNbtEncoding::LittleVarInt) {
            document.name = stream.readString();
        }
        document.root = NbtValue::end();
    } else {
        document = BedrockNbtCodec::read(stream, encoding);
    }

    reader.skip(stream.offset() - start);
    return nbtDocumentToProtoDefValue(document);
}

void skipProtoDefNbt(
    ProtoDefReader& reader,
    BedrockNbtEncoding encoding
) {
    if (reader.remaining() == 0) {
        throw BedrockNbtError("not enough bytes for NBT root tag");
    }

    const auto start = reader.offset();
    BinaryStream stream = BinaryStream::view(reader.data(), start);

    if (reader.data()[start] == static_cast<uint8_t>(NbtTagType::End)) {
        stream.readU8();
        if (encoding == BedrockNbtEncoding::LittleVarInt) {
            (void) stream.readString();
        }
    } else {
        BedrockNbtCodec::skip(stream, encoding);
    }

    reader.skip(stream.offset() - start);
}

} // namespace bedrock
