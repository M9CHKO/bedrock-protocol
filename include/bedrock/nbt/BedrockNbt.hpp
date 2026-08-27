#pragma once

#include <bedrock/BinaryStream.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bedrock {

enum class NbtTagType : uint8_t {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    Long = 4,
    Float = 5,
    Double = 6,
    ByteArray = 7,
    String = 8,
    List = 9,
    Compound = 10,
    IntArray = 11,
    LongArray = 12
};

struct NbtNamedValue;

struct NbtValue {
    NbtTagType type = NbtTagType::End;
    int64_t integerValue = 0;
    double floatingValue = 0.0;
    std::string stringValue;
    std::vector<uint8_t> byteArrayValue;
    NbtTagType listElementType = NbtTagType::End;
    std::vector<NbtValue> listValue;
    std::vector<NbtNamedValue> compoundValue;
    std::vector<int32_t> intArrayValue;
    std::vector<int64_t> longArrayValue;

    static NbtValue end();
    static NbtValue byte(int8_t value);
    static NbtValue shortInteger(int16_t value);
    static NbtValue integer(int32_t value);
    static NbtValue longInteger(int64_t value);
    static NbtValue floating(float value);
    static NbtValue doubleFloating(double value);
    static NbtValue byteArray(std::vector<uint8_t> value);
    static NbtValue string(std::string value);
    static NbtValue list(NbtTagType elementType, std::vector<NbtValue> value = {});
    static NbtValue compound(std::vector<NbtNamedValue> value = {});
    static NbtValue intArray(std::vector<int32_t> value);
    static NbtValue longArray(std::vector<int64_t> value);

    const NbtValue* find(std::string_view name) const;
    NbtValue* find(std::string_view name);
    void set(std::string name, NbtValue value);
};

struct NbtNamedValue {
    std::string name;
    NbtValue value;
};

struct NbtDocument {
    std::string name;
    NbtValue root;
};

bool operator==(const NbtValue& lhs, const NbtValue& rhs);
bool operator==(const NbtNamedValue& lhs, const NbtNamedValue& rhs);
bool operator==(const NbtDocument& lhs, const NbtDocument& rhs);

enum class BedrockNbtEncoding {
    LittleEndian,
    LittleVarInt
};

struct BedrockNbtLimits {
    std::size_t maxDepth = 64;
    std::size_t maxStringBytes = 16u * 1024u * 1024u;
    std::size_t maxCollectionLength = 16u * 1024u * 1024u;
    std::size_t maxTotalValues = 32u * 1024u * 1024u;
};

class BedrockNbtError : public std::runtime_error {
public:
    explicit BedrockNbtError(const std::string& message)
        : std::runtime_error(message) {}
};

class BedrockNbtCodec {
public:
    static NbtDocument read(
        BinaryStream& stream,
        BedrockNbtEncoding encoding,
        const BedrockNbtLimits& limits = {}
    );

    static void write(
        BinaryStream& stream,
        const NbtDocument& document,
        BedrockNbtEncoding encoding,
        const BedrockNbtLimits& limits = {}
    );

    // Unnamed NBT is used by several Bedrock packet fields. It starts with the
    // tag type and omits the root-name string.
    static NbtValue readUnnamed(
        BinaryStream& stream,
        BedrockNbtEncoding encoding,
        const BedrockNbtLimits& limits = {}
    );

    static void writeUnnamed(
        BinaryStream& stream,
        const NbtValue& value,
        BedrockNbtEncoding encoding,
        const BedrockNbtLimits& limits = {}
    );
};

} // namespace bedrock
