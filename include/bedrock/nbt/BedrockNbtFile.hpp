#pragma once

#include <bedrock/nbt/BedrockNbt.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bedrock {

// Metadata equivalent to prismarine-nbt's parse result. `size` is measured
// from `startOffset`, while `buffer` retains the complete original input.
struct BedrockNbtMetadata {
    std::vector<uint8_t> buffer;
    std::size_t startOffset = 0;
    std::size_t size = 0;
};

struct BedrockLevelDatHeader {
    uint32_t version = 0;
    uint32_t payloadLength = 0;
};

struct BedrockNbtParseResult {
    NbtDocument parsed;
    BedrockNbtEncoding encoding = BedrockNbtEncoding::LittleEndian;
    BedrockNbtMetadata metadata;
    std::optional<BedrockLevelDatHeader> levelHeader;
};

// Bedrock-only high-level prismarine-nbt API. Big-endian and gzip handling
// belong to Java Edition and are intentionally not part of this library.
class BedrockNbt {
public:
    static std::vector<uint8_t> writeUncompressed(
        const NbtDocument& document,
        BedrockNbtEncoding encoding = BedrockNbtEncoding::LittleEndian,
        const BedrockNbtLimits& limits = {}
    );

    static NbtDocument parseUncompressed(
        std::vector<uint8_t> data,
        BedrockNbtEncoding encoding = BedrockNbtEncoding::LittleEndian,
        const BedrockNbtLimits& limits = {}
    );

    static bool hasBedrockLevelHeader(const std::vector<uint8_t>& data) noexcept;

    static bool hasLevelHeader(const std::vector<uint8_t>& data) noexcept {
        return hasBedrockLevelHeader(data);
    }

    // Parses one root tag at `startOffset`. Explicit parsing intentionally
    // permits trailing data and reports the consumed byte count in metadata,
    // matching prismarine-nbt's parseAs behavior.
    static BedrockNbtParseResult parseAs(
        std::vector<uint8_t> data,
        BedrockNbtEncoding encoding,
        std::size_t startOffset = 0,
        const BedrockNbtLimits& limits = {}
    );

    // With an explicit encoding this behaves like parseAs at offset zero.
    // Without one it recognises the Bedrock level.dat header, then tries the
    // two Bedrock wire encodings and rejects unexplained trailing bytes.
    static BedrockNbtParseResult parse(
        std::vector<uint8_t> data,
        std::optional<BedrockNbtEncoding> encoding = std::nullopt,
        const BedrockNbtLimits& limits = {}
    );

    // Strict level.dat helpers validate both the eight-byte header and the
    // declared payload length. Generic parse() remains prismarine-compatible.
    static BedrockNbtParseResult parseLevelDat(
        std::vector<uint8_t> data,
        const BedrockNbtLimits& limits = {}
    );

    static std::vector<uint8_t> writeLevelDat(
        const NbtDocument& document,
        uint32_t version = 8,
        const BedrockNbtLimits& limits = {}
    );

    static ProtoDefValue simplify(const NbtValue& value);
    static ProtoDefValue simplify(const NbtDocument& document);

    // Compound field order and root names do not affect semantic equality.
    static bool equal(const NbtValue& left, const NbtValue& right);
    static bool equal(const NbtDocument& left, const NbtDocument& right);
};

} // namespace bedrock
