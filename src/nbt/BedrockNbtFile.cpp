#include <bedrock/nbt/BedrockNbtFile.hpp>

#include <bedrock/BinaryStream.hpp>

#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

namespace bedrock {
namespace {

BedrockLevelDatHeader readLevelHeader(const std::vector<uint8_t>& data) {
    if (data.size() < 8) {
        throw BedrockNbtError("Bedrock level.dat header is truncated");
    }

    const auto readU32Le = [&data](std::size_t offset) {
        return static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8u) |
            (static_cast<uint32_t>(data[offset + 2]) << 16u) |
            (static_cast<uint32_t>(data[offset + 3]) << 24u);
    };

    return {readU32Le(0), readU32Le(4)};
}

void verifyDetectedPayload(const BedrockNbtParseResult& result) {
    const auto& metadata = result.metadata;
    const std::size_t consumed = metadata.startOffset + metadata.size;
    if (consumed > metadata.buffer.size()) {
        throw BedrockNbtError("NBT parser reported an invalid consumed size");
    }
    if (consumed < metadata.buffer.size() &&
        metadata.buffer[consumed] != static_cast<uint8_t>(NbtTagType::Compound)) {
        throw BedrockNbtError(
            "unexpected trailing bytes after detected NBT root at byte " +
            std::to_string(metadata.size)
        );
    }
}

using CompoundIndex = std::unordered_map<std::string, const NbtValue*>;

CompoundIndex indexCompound(const NbtValue& value) {
    CompoundIndex result;
    result.reserve(value.compoundValue.size());
    for (const auto& child : value.compoundValue) {
        // A JavaScript object cannot retain duplicate keys. Use the same
        // last-write-wins view for simplify() and semantic equality.
        result.insert_or_assign(child.name, &child.value);
    }
    return result;
}

} // namespace

std::vector<uint8_t> BedrockNbt::writeUncompressed(
    const NbtDocument& document,
    BedrockNbtEncoding encoding,
    const BedrockNbtLimits& limits
) {
    BinaryStream stream;
    BedrockNbtCodec::write(stream, document, encoding, limits);
    return std::move(stream.buffer());
}

NbtDocument BedrockNbt::parseUncompressed(
    std::vector<uint8_t> data,
    BedrockNbtEncoding encoding,
    const BedrockNbtLimits& limits
) {
    return parseAs(std::move(data), encoding, 0, limits).parsed;
}

bool BedrockNbt::hasBedrockLevelHeader(
    const std::vector<uint8_t>& data
) noexcept {
    return data.size() >= 4 && data[1] == 0 && data[2] == 0 && data[3] == 0;
}

BedrockNbtParseResult BedrockNbt::parseAs(
    std::vector<uint8_t> data,
    BedrockNbtEncoding encoding,
    std::size_t startOffset,
    const BedrockNbtLimits& limits
) {
    if (startOffset > data.size()) {
        throw BedrockNbtError("NBT start offset exceeds input size");
    }

    BinaryStream stream(std::move(data));
    stream.seek(startOffset);

    NbtDocument parsed;
    try {
        parsed = BedrockNbtCodec::read(stream, encoding, limits);
    } catch (const BedrockNbtError&) {
        throw;
    } catch (const BinaryStreamError& error) {
        throw BedrockNbtError(std::string("failed to parse NBT: ") + error.what());
    }

    BedrockNbtParseResult result;
    result.parsed = std::move(parsed);
    result.encoding = encoding;
    result.metadata.startOffset = startOffset;
    result.metadata.size = stream.offset() - startOffset;
    result.metadata.buffer = std::move(stream.buffer());
    return result;
}

BedrockNbtParseResult BedrockNbt::parse(
    std::vector<uint8_t> data,
    std::optional<BedrockNbtEncoding> encoding,
    const BedrockNbtLimits& limits
) {
    if (encoding) {
        return parseAs(std::move(data), *encoding, 0, limits);
    }

    // Keep prismarine-nbt's Bedrock level.dat signature, but do not seek past
    // a truncated eight-byte header.
    if (data.size() >= 8 && hasBedrockLevelHeader(data)) {
        const auto header = readLevelHeader(data);
        auto result = parseAs(
            std::move(data),
            BedrockNbtEncoding::LittleEndian,
            8,
            limits
        );
        result.levelHeader = header;
        return result;
    }

    std::string littleError;
    try {
        auto result = parseAs(data, BedrockNbtEncoding::LittleEndian, 0, limits);
        verifyDetectedPayload(result);
        return result;
    } catch (const BedrockNbtError& error) {
        littleError = error.what();
    }

    try {
        auto result = parseAs(
            std::move(data),
            BedrockNbtEncoding::LittleVarInt,
            0,
            limits
        );
        verifyDetectedPayload(result);
        return result;
    } catch (const BedrockNbtError& varIntError) {
        throw BedrockNbtError(
            "unable to detect Bedrock NBT encoding (little-endian: " +
            littleError + "; little-varint: " + varIntError.what() + ")"
        );
    }
}

BedrockNbtParseResult BedrockNbt::parseLevelDat(
    std::vector<uint8_t> data,
    const BedrockNbtLimits& limits
) {
    if (!hasBedrockLevelHeader(data)) {
        throw BedrockNbtError("input does not have a Bedrock level.dat header");
    }

    const auto header = readLevelHeader(data);
    const std::size_t actualPayloadLength = data.size() - 8;
    if (header.payloadLength != actualPayloadLength) {
        throw BedrockNbtError(
            "Bedrock level.dat payload length mismatch: header declares " +
            std::to_string(header.payloadLength) + ", input contains " +
            std::to_string(actualPayloadLength)
        );
    }

    auto result = parseAs(
        std::move(data),
        BedrockNbtEncoding::LittleEndian,
        8,
        limits
    );
    if (result.metadata.size != actualPayloadLength) {
        throw BedrockNbtError(
            "Bedrock level.dat contains trailing bytes after its NBT root"
        );
    }
    result.levelHeader = header;
    return result;
}

std::vector<uint8_t> BedrockNbt::writeLevelDat(
    const NbtDocument& document,
    uint32_t version,
    const BedrockNbtLimits& limits
) {
    auto payload = writeUncompressed(
        document,
        BedrockNbtEncoding::LittleEndian,
        limits
    );
    if (payload.size() > std::numeric_limits<uint32_t>::max()) {
        throw BedrockNbtError("Bedrock level.dat NBT payload exceeds uint32 length");
    }

    BinaryStream stream;
    stream.writeU32LE(version);
    stream.writeU32LE(static_cast<uint32_t>(payload.size()));
    stream.writeBytes(payload);
    return std::move(stream.buffer());
}

ProtoDefValue BedrockNbt::simplify(const NbtValue& value) {
    switch (value.type) {
        case NbtTagType::End:
            return ProtoDefValue::null();
        case NbtTagType::Byte:
        case NbtTagType::Short:
        case NbtTagType::Int:
        case NbtTagType::Long:
            return ProtoDefValue::integer(value.integerValue);
        case NbtTagType::Float:
        case NbtTagType::Double:
            return ProtoDefValue::floating(value.floatingValue);
        case NbtTagType::ByteArray:
            return ProtoDefValue::bytes(value.byteArrayValue);
        case NbtTagType::String:
            return ProtoDefValue::string(value.stringValue);
        case NbtTagType::List: {
            std::vector<ProtoDefValue> result;
            result.reserve(value.listValue.size());
            for (const auto& item : value.listValue) {
                result.push_back(simplify(item));
            }
            return ProtoDefValue::array(std::move(result));
        }
        case NbtTagType::Compound: {
            std::unordered_map<std::string, ProtoDefValue> result;
            result.reserve(value.compoundValue.size());
            for (const auto& child : value.compoundValue) {
                result.insert_or_assign(child.name, simplify(child.value));
            }
            return ProtoDefValue::object(std::move(result));
        }
        case NbtTagType::IntArray: {
            std::vector<ProtoDefValue> result;
            result.reserve(value.intArrayValue.size());
            for (const auto item : value.intArrayValue) {
                result.push_back(ProtoDefValue::integer(item));
            }
            return ProtoDefValue::array(std::move(result));
        }
        case NbtTagType::LongArray: {
            std::vector<ProtoDefValue> result;
            result.reserve(value.longArrayValue.size());
            for (const auto item : value.longArrayValue) {
                result.push_back(ProtoDefValue::integer(item));
            }
            return ProtoDefValue::array(std::move(result));
        }
    }

    throw BedrockNbtError("unreachable NBT tag type during simplify");
}

ProtoDefValue BedrockNbt::simplify(const NbtDocument& document) {
    return simplify(document.root);
}

bool BedrockNbt::equal(const NbtValue& left, const NbtValue& right) {
    if (left.type != right.type) {
        return false;
    }

    switch (left.type) {
        case NbtTagType::End:
            return true;
        case NbtTagType::Byte:
        case NbtTagType::Short:
        case NbtTagType::Int:
        case NbtTagType::Long:
            return left.integerValue == right.integerValue;
        case NbtTagType::Float:
        case NbtTagType::Double:
            return left.floatingValue == right.floatingValue;
        case NbtTagType::ByteArray:
            return left.byteArrayValue == right.byteArrayValue;
        case NbtTagType::String:
            return left.stringValue == right.stringValue;
        case NbtTagType::List:
            if (left.listElementType != right.listElementType ||
                left.listValue.size() != right.listValue.size()) {
                return false;
            }
            for (std::size_t i = 0; i < left.listValue.size(); ++i) {
                if (!equal(left.listValue[i], right.listValue[i])) {
                    return false;
                }
            }
            return true;
        case NbtTagType::Compound: {
            const auto leftFields = indexCompound(left);
            const auto rightFields = indexCompound(right);
            if (leftFields.size() != rightFields.size()) {
                return false;
            }
            for (const auto& [name, value] : leftFields) {
                const auto other = rightFields.find(name);
                if (other == rightFields.end() || !equal(*value, *other->second)) {
                    return false;
                }
            }
            return true;
        }
        case NbtTagType::IntArray:
            return left.intArrayValue == right.intArrayValue;
        case NbtTagType::LongArray:
            return left.longArrayValue == right.longArrayValue;
    }

    return false;
}

bool BedrockNbt::equal(const NbtDocument& left, const NbtDocument& right) {
    // prismarine-nbt's equal() deliberately compares type/value and ignores
    // the optional root name.
    return equal(left.root, right.root);
}

} // namespace bedrock
