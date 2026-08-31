#include <bedrock/nbt/BedrockNbt.hpp>

#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace bedrock {
namespace {

struct ReadContext {
    BinaryStream& stream;
    BedrockNbtEncoding encoding;
    const BedrockNbtLimits& limits;
    std::size_t totalValues = 0;
};

struct WriteContext {
    BinaryStream& stream;
    BedrockNbtEncoding encoding;
    const BedrockNbtLimits& limits;
    std::size_t totalValues = 0;
};

NbtTagType checkedTagType(uint8_t raw) {
    if (raw > static_cast<uint8_t>(NbtTagType::LongArray)) {
        throw BedrockNbtError("unsupported NBT tag type: " + std::to_string(raw));
    }
    return static_cast<NbtTagType>(raw);
}

void accountValue(std::size_t& total, const BedrockNbtLimits& limits) {
    if (total == limits.maxTotalValues) {
        throw BedrockNbtError("NBT value count exceeds configured limit");
    }
    ++total;
}

void checkDepth(std::size_t depth, const BedrockNbtLimits& limits) {
    if (depth > limits.maxDepth) {
        throw BedrockNbtError("NBT nesting depth exceeds configured limit");
    }
}

int32_t decodeZigZag32(uint32_t encoded) {
    const uint32_t bits = (encoded >> 1u) ^ (0u - (encoded & 1u));
    return std::bit_cast<int32_t>(bits);
}

int64_t decodeZigZag64(uint64_t encoded) {
    const uint64_t bits = (encoded >> 1u) ^ (0ull - (encoded & 1ull));
    return std::bit_cast<int64_t>(bits);
}

uint32_t encodeZigZag32(int32_t value) {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    return (bits << 1u) ^ (0u - (bits >> 31u));
}

uint64_t encodeZigZag64(int64_t value) {
    const uint64_t bits = std::bit_cast<uint64_t>(value);
    return (bits << 1u) ^ (0ull - (bits >> 63u));
}

int32_t readInt32(ReadContext& context) {
    if (context.encoding == BedrockNbtEncoding::LittleVarInt) {
        return decodeZigZag32(context.stream.readVarUInt());
    }
    return context.stream.readI32LE();
}

int64_t readInt64(ReadContext& context) {
    if (context.encoding == BedrockNbtEncoding::LittleVarInt) {
        return decodeZigZag64(context.stream.readVarULong());
    }
    return context.stream.readI64LE();
}

void writeInt32(WriteContext& context, int32_t value) {
    if (context.encoding == BedrockNbtEncoding::LittleVarInt) {
        context.stream.writeVarUInt(encodeZigZag32(value));
    } else {
        context.stream.writeI32LE(value);
    }
}

void writeInt64(WriteContext& context, int64_t value) {
    if (context.encoding == BedrockNbtEncoding::LittleVarInt) {
        context.stream.writeVarULong(encodeZigZag64(value));
    } else {
        context.stream.writeI64LE(value);
    }
}

std::size_t checkedCollectionLength(int32_t value, const BedrockNbtLimits& limits) {
    if (value < 0) {
        throw BedrockNbtError("negative NBT collection length");
    }
    const auto length = static_cast<std::size_t>(value);
    if (length > limits.maxCollectionLength) {
        throw BedrockNbtError("NBT collection length exceeds configured limit");
    }
    return length;
}

int32_t checkedCollectionLength(std::size_t value, const BedrockNbtLimits& limits) {
    if (value > limits.maxCollectionLength ||
        value > static_cast<std::size_t>(std::numeric_limits<int32_t>::max())) {
        throw BedrockNbtError("NBT collection is too large to encode");
    }
    return static_cast<int32_t>(value);
}

std::string readString(ReadContext& context, bool tagName) {
    std::size_t length = 0;
    if (context.encoding == BedrockNbtEncoding::LittleVarInt) {
        length = context.stream.readVarUInt();
    } else {
        length = context.stream.readU16LE();
    }

    if (length > context.limits.maxStringBytes) {
        throw BedrockNbtError("NBT string length exceeds configured limit");
    }
    if (length > context.stream.remaining()) {
        throw BedrockNbtError("NBT string length exceeds remaining input");
    }

    const auto bytes = context.stream.readBytes(length);
    std::string value(bytes.begin(), bytes.end());
    if (tagName && value.find('\0') != std::string::npos) {
        throw BedrockNbtError("NBT tag name contains an embedded null byte");
    }
    return value;
}

void writeString(WriteContext& context, std::string_view value, bool tagName) {
    if (value.size() > context.limits.maxStringBytes) {
        throw BedrockNbtError("NBT string length exceeds configured limit");
    }
    if (tagName && value.find('\0') != std::string_view::npos) {
        throw BedrockNbtError("NBT tag name contains an embedded null byte");
    }

    if (context.encoding == BedrockNbtEncoding::LittleVarInt) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
            throw BedrockNbtError("NBT string is too large to encode");
        }
        context.stream.writeVarUInt(static_cast<uint32_t>(value.size()));
    } else {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<uint16_t>::max())) {
            throw BedrockNbtError("little-endian NBT string exceeds uint16 length");
        }
        context.stream.writeU16LE(static_cast<uint16_t>(value.size()));
    }
    context.stream.writeBytes(
        reinterpret_cast<const uint8_t*>(value.data()),
        value.size()
    );
}

NbtValue readPayload(ReadContext& context, NbtTagType type, std::size_t depth) {
    checkDepth(depth, context.limits);
    accountValue(context.totalValues, context.limits);

    NbtValue out;
    out.type = type;

    switch (type) {
        case NbtTagType::End:
            return out;
        case NbtTagType::Byte:
            out.integerValue = context.stream.readI8();
            return out;
        case NbtTagType::Short:
            out.integerValue = context.stream.readI16LE();
            return out;
        case NbtTagType::Int:
            out.integerValue = readInt32(context);
            return out;
        case NbtTagType::Long:
            out.integerValue = readInt64(context);
            return out;
        case NbtTagType::Float:
            out.floatingValue = context.stream.readFloatLE();
            return out;
        case NbtTagType::Double:
            out.floatingValue = context.stream.readDoubleLE();
            return out;
        case NbtTagType::ByteArray: {
            const auto length = checkedCollectionLength(readInt32(context), context.limits);
            if (length > context.stream.remaining()) {
                throw BedrockNbtError("NBT byte array exceeds remaining input");
            }
            out.byteArrayValue = context.stream.readBytes(length);
            return out;
        }
        case NbtTagType::String:
            out.stringValue = readString(context, false);
            return out;
        case NbtTagType::List: {
            out.listElementType = checkedTagType(context.stream.readU8());
            const auto length = checkedCollectionLength(readInt32(context), context.limits);
            if (out.listElementType == NbtTagType::End && length != 0) {
                throw BedrockNbtError("non-empty NBT list cannot use TAG_End elements");
            }
            out.listValue.reserve(length);
            for (std::size_t i = 0; i < length; ++i) {
                out.listValue.push_back(readPayload(context, out.listElementType, depth + 1));
            }
            return out;
        }
        case NbtTagType::Compound:
            while (true) {
                const auto childType = checkedTagType(context.stream.readU8());
                if (childType == NbtTagType::End) {
                    return out;
                }
                std::string childName = readString(context, true);
                out.compoundValue.push_back({
                    std::move(childName),
                    readPayload(context, childType, depth + 1)
                });
            }
        case NbtTagType::IntArray: {
            const auto length = checkedCollectionLength(readInt32(context), context.limits);
            if (length > context.stream.remaining() / sizeof(int32_t)) {
                throw BedrockNbtError("NBT int array exceeds remaining input");
            }
            out.intArrayValue.reserve(length);
            for (std::size_t i = 0; i < length; ++i) {
                // prismarine-nbt's littleVarint format changes the array
                // length to zigzag32, but keeps each i32 element fixed-width.
                out.intArrayValue.push_back(context.stream.readI32LE());
            }
            return out;
        }
        case NbtTagType::LongArray: {
            const auto length = checkedCollectionLength(readInt32(context), context.limits);
            if (length > context.stream.remaining() / sizeof(int64_t)) {
                throw BedrockNbtError("NBT long array exceeds remaining input");
            }
            out.longArrayValue.reserve(length);
            for (std::size_t i = 0; i < length; ++i) {
                out.longArrayValue.push_back(context.stream.readI64LE());
            }
            return out;
        }
    }

    throw BedrockNbtError("unreachable NBT tag type");
}

template <typename Integer>
Integer checkedInteger(const NbtValue& value, const char* tagName) {
    if (value.integerValue < static_cast<int64_t>(std::numeric_limits<Integer>::min()) ||
        value.integerValue > static_cast<int64_t>(std::numeric_limits<Integer>::max())) {
        throw BedrockNbtError(std::string("NBT ") + tagName + " value is out of range");
    }
    return static_cast<Integer>(value.integerValue);
}

void writePayload(WriteContext& context, const NbtValue& value, std::size_t depth) {
    checkDepth(depth, context.limits);
    accountValue(context.totalValues, context.limits);

    switch (value.type) {
        case NbtTagType::End:
            return;
        case NbtTagType::Byte:
            context.stream.writeI8(checkedInteger<int8_t>(value, "byte"));
            return;
        case NbtTagType::Short:
            context.stream.writeI16LE(checkedInteger<int16_t>(value, "short"));
            return;
        case NbtTagType::Int:
            writeInt32(context, checkedInteger<int32_t>(value, "int"));
            return;
        case NbtTagType::Long:
            writeInt64(context, value.integerValue);
            return;
        case NbtTagType::Float:
            context.stream.writeFloatLE(static_cast<float>(value.floatingValue));
            return;
        case NbtTagType::Double:
            context.stream.writeDoubleLE(value.floatingValue);
            return;
        case NbtTagType::ByteArray:
            writeInt32(context, checkedCollectionLength(value.byteArrayValue.size(), context.limits));
            context.stream.writeBytes(value.byteArrayValue);
            return;
        case NbtTagType::String:
            writeString(context, value.stringValue, false);
            return;
        case NbtTagType::List: {
            if (static_cast<uint8_t>(value.listElementType) >
                static_cast<uint8_t>(NbtTagType::LongArray)) {
                throw BedrockNbtError("unsupported NBT list element type");
            }
            if (value.listElementType == NbtTagType::End && !value.listValue.empty()) {
                throw BedrockNbtError("non-empty NBT list cannot use TAG_End elements");
            }
            context.stream.writeU8(static_cast<uint8_t>(value.listElementType));
            writeInt32(context, checkedCollectionLength(value.listValue.size(), context.limits));
            for (const auto& item : value.listValue) {
                if (item.type != value.listElementType) {
                    throw BedrockNbtError("NBT list item type does not match its element type");
                }
                writePayload(context, item, depth + 1);
            }
            return;
        }
        case NbtTagType::Compound:
            if (value.compoundValue.size() > context.limits.maxCollectionLength) {
                throw BedrockNbtError("NBT compound length exceeds configured limit");
            }
            for (const auto& child : value.compoundValue) {
                if (child.value.type == NbtTagType::End) {
                    throw BedrockNbtError("NBT compound cannot contain a named TAG_End");
                }
                context.stream.writeU8(static_cast<uint8_t>(child.value.type));
                writeString(context, child.name, true);
                writePayload(context, child.value, depth + 1);
            }
            context.stream.writeU8(static_cast<uint8_t>(NbtTagType::End));
            return;
        case NbtTagType::IntArray:
            writeInt32(context, checkedCollectionLength(value.intArrayValue.size(), context.limits));
            for (const auto item : value.intArrayValue) {
                context.stream.writeI32LE(item);
            }
            return;
        case NbtTagType::LongArray:
            writeInt32(context, checkedCollectionLength(value.longArrayValue.size(), context.limits));
            for (const auto item : value.longArrayValue) {
                context.stream.writeI64LE(item);
            }
            return;
    }

    throw BedrockNbtError("unreachable NBT tag type");
}

} // namespace

NbtValue::NbtValue() = default;
NbtValue::~NbtValue() = default;
NbtValue::NbtValue(const NbtValue& other) = default;
NbtValue::NbtValue(NbtValue&& other) noexcept = default;
NbtValue& NbtValue::operator=(const NbtValue& other) = default;
NbtValue& NbtValue::operator=(NbtValue&& other) noexcept = default;

NbtValue NbtValue::end() {
    return {};
}

NbtValue NbtValue::byte(int8_t value) {
    NbtValue out;
    out.type = NbtTagType::Byte;
    out.integerValue = value;
    return out;
}

NbtValue NbtValue::shortInteger(int16_t value) {
    NbtValue out;
    out.type = NbtTagType::Short;
    out.integerValue = value;
    return out;
}

NbtValue NbtValue::integer(int32_t value) {
    NbtValue out;
    out.type = NbtTagType::Int;
    out.integerValue = value;
    return out;
}

NbtValue NbtValue::longInteger(int64_t value) {
    NbtValue out;
    out.type = NbtTagType::Long;
    out.integerValue = value;
    return out;
}

NbtValue NbtValue::floating(float value) {
    NbtValue out;
    out.type = NbtTagType::Float;
    out.floatingValue = value;
    return out;
}

NbtValue NbtValue::doubleFloating(double value) {
    NbtValue out;
    out.type = NbtTagType::Double;
    out.floatingValue = value;
    return out;
}

NbtValue NbtValue::byteArray(std::vector<uint8_t> value) {
    NbtValue out;
    out.type = NbtTagType::ByteArray;
    out.byteArrayValue = std::move(value);
    return out;
}

NbtValue NbtValue::string(std::string value) {
    NbtValue out;
    out.type = NbtTagType::String;
    out.stringValue = std::move(value);
    return out;
}

NbtValue NbtValue::list(NbtTagType elementType, std::vector<NbtValue> value) {
    NbtValue out;
    out.type = NbtTagType::List;
    out.listElementType = elementType;
    out.listValue = std::move(value);
    return out;
}

NbtValue NbtValue::compound() {
    return compound({});
}

NbtValue NbtValue::compound(std::vector<NbtNamedValue> value) {
    NbtValue out;
    out.type = NbtTagType::Compound;
    out.compoundValue = std::move(value);
    return out;
}

NbtValue NbtValue::intArray(std::vector<int32_t> value) {
    NbtValue out;
    out.type = NbtTagType::IntArray;
    out.intArrayValue = std::move(value);
    return out;
}

NbtValue NbtValue::longArray(std::vector<int64_t> value) {
    NbtValue out;
    out.type = NbtTagType::LongArray;
    out.longArrayValue = std::move(value);
    return out;
}

const NbtValue* NbtValue::find(std::string_view name) const {
    if (type != NbtTagType::Compound) {
        return nullptr;
    }
    for (const auto& child : compoundValue) {
        if (child.name == name) {
            return &child.value;
        }
    }
    return nullptr;
}

NbtValue* NbtValue::find(std::string_view name) {
    if (type != NbtTagType::Compound) {
        return nullptr;
    }
    for (auto& child : compoundValue) {
        if (child.name == name) {
            return &child.value;
        }
    }
    return nullptr;
}

void NbtValue::set(std::string name, NbtValue value) {
    if (type != NbtTagType::Compound) {
        throw BedrockNbtError("NbtValue::set requires a compound value");
    }
    for (auto& child : compoundValue) {
        if (child.name == name) {
            child.value = std::move(value);
            return;
        }
    }
    compoundValue.push_back({std::move(name), std::move(value)});
}

bool operator==(const NbtNamedValue& lhs, const NbtNamedValue& rhs) {
    return lhs.name == rhs.name && lhs.value == rhs.value;
}

bool operator==(const NbtValue& lhs, const NbtValue& rhs) {
    return lhs.type == rhs.type &&
        lhs.integerValue == rhs.integerValue &&
        lhs.floatingValue == rhs.floatingValue &&
        lhs.stringValue == rhs.stringValue &&
        lhs.byteArrayValue == rhs.byteArrayValue &&
        lhs.listElementType == rhs.listElementType &&
        lhs.listValue == rhs.listValue &&
        lhs.compoundValue == rhs.compoundValue &&
        lhs.intArrayValue == rhs.intArrayValue &&
        lhs.longArrayValue == rhs.longArrayValue;
}

bool operator==(const NbtDocument& lhs, const NbtDocument& rhs) {
    return lhs.name == rhs.name && lhs.root == rhs.root;
}

NbtDocument BedrockNbtCodec::read(
    BinaryStream& stream,
    BedrockNbtEncoding encoding,
    const BedrockNbtLimits& limits
) {
    ReadContext context {stream, encoding, limits};
    const auto type = checkedTagType(stream.readU8());
    if (type == NbtTagType::End) {
        return {{}, NbtValue::end()};
    }
    std::string name = readString(context, true);
    return {std::move(name), readPayload(context, type, 0)};
}

void BedrockNbtCodec::write(
    BinaryStream& stream,
    const NbtDocument& document,
    BedrockNbtEncoding encoding,
    const BedrockNbtLimits& limits
) {
    WriteContext context {stream, encoding, limits};
    stream.writeU8(static_cast<uint8_t>(document.root.type));
    if (document.root.type == NbtTagType::End) {
        return;
    }
    writeString(context, document.name, true);
    writePayload(context, document.root, 0);
}

NbtValue BedrockNbtCodec::readUnnamed(
    BinaryStream& stream,
    BedrockNbtEncoding encoding,
    const BedrockNbtLimits& limits
) {
    ReadContext context {stream, encoding, limits};
    const auto type = checkedTagType(stream.readU8());
    return readPayload(context, type, 0);
}

void BedrockNbtCodec::writeUnnamed(
    BinaryStream& stream,
    const NbtValue& value,
    BedrockNbtEncoding encoding,
    const BedrockNbtLimits& limits
) {
    WriteContext context {stream, encoding, limits};
    stream.writeU8(static_cast<uint8_t>(value.type));
    writePayload(context, value, 0);
}

} // namespace bedrock
