#include <bedrock/nbt/BedrockNbtFile.hpp>

#include <cctype>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> fromHex(const std::string& value) {
    if ((value.size() & 1u) != 0) {
        throw std::runtime_error("odd hex input");
    }
    const auto nibble = [](char ch) -> uint8_t {
        if (ch >= '0' && ch <= '9') return static_cast<uint8_t>(ch - '0');
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ch >= 'a' && ch <= 'f') {
            return static_cast<uint8_t>(ch - 'a' + 10);
        }
        throw std::runtime_error("invalid hex input");
    };

    std::vector<uint8_t> result;
    result.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        result.push_back(static_cast<uint8_t>(
            (nibble(value[i]) << 4u) | nibble(value[i + 1])
        ));
    }
    return result;
}

bedrock::NbtDocument makeDocument() {
    using bedrock::NbtTagType;
    using bedrock::NbtValue;
    return {
        "",
        NbtValue::compound({
            {"x", NbtValue::integer(42)},
            {"name", NbtValue::string("bedrock")},
            {"list", NbtValue::list(NbtTagType::Int, {
                NbtValue::integer(1),
                NbtValue::integer(-2),
                NbtValue::integer(3)
            })}
        })
    };
}

void expectThrows(const std::function<void()>& callback, const char* label) {
    try {
        callback();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(std::string(label) + " did not throw");
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    try {
        const auto document = makeDocument();

        // Generated independently by prismarine-nbt 2.8.0 from node_modules.
        const auto little = fromHex(
            "0a0000030100782a0000000804006e616d650700626564726f636b"
            "0904006c697374030300000001000000feffffff0300000000"
        );
        const auto littleVarInt = fromHex(
            "0a000301785408046e616d6507626564726f636b"
            "09046c697374030602030600"
        );
        const auto levelDat = fromHex(
            "0800000034000000"
            "0a0000030100782a0000000804006e616d650700626564726f636b"
            "0904006c697374030300000001000000feffffff0300000000"
        );

        require(
            bedrock::BedrockNbt::writeUncompressed(
                document,
                bedrock::BedrockNbtEncoding::LittleEndian
            ) == little,
            "little-endian writeUncompressed Node golden mismatch"
        );
        require(
            bedrock::BedrockNbt::writeUncompressed(
                document,
                bedrock::BedrockNbtEncoding::LittleVarInt
            ) == littleVarInt,
            "little-varint writeUncompressed Node golden mismatch"
        );
        require(
            bedrock::BedrockNbt::parseUncompressed(
                little,
                bedrock::BedrockNbtEncoding::LittleEndian
            ) == document,
            "parseUncompressed little-endian mismatch"
        );
        require(
            bedrock::BedrockNbt::parseUncompressed(
                littleVarInt,
                bedrock::BedrockNbtEncoding::LittleVarInt
            ) == document,
            "parseUncompressed little-varint mismatch"
        );

        const auto detectedLittle = bedrock::BedrockNbt::parse(little);
        require(
            detectedLittle.encoding == bedrock::BedrockNbtEncoding::LittleEndian &&
                detectedLittle.parsed == document &&
                detectedLittle.metadata.startOffset == 0 &&
                detectedLittle.metadata.size == little.size() &&
                detectedLittle.metadata.buffer == little &&
                !detectedLittle.levelHeader,
            "automatic little-endian detection metadata mismatch"
        );

        const auto detectedVarInt = bedrock::BedrockNbt::parse(littleVarInt);
        require(
            detectedVarInt.encoding == bedrock::BedrockNbtEncoding::LittleVarInt &&
                detectedVarInt.parsed == document &&
                detectedVarInt.metadata.size == littleVarInt.size(),
            "automatic little-varint detection mismatch"
        );

        auto explicitTrailing = little;
        explicitTrailing.push_back(0x7f);
        const auto explicitResult = bedrock::BedrockNbt::parseAs(
            explicitTrailing,
            bedrock::BedrockNbtEncoding::LittleEndian
        );
        require(
            explicitResult.parsed == document &&
                explicitResult.metadata.size == little.size() &&
                explicitResult.metadata.buffer == explicitTrailing,
            "parseAs must retain trailing data and report consumed bytes"
        );
        const auto explicitViaParse = bedrock::BedrockNbt::parse(
            explicitTrailing,
            bedrock::BedrockNbtEncoding::LittleEndian
        );
        require(
            explicitViaParse.metadata.size == little.size(),
            "parse with explicit format must preserve parseAs behavior"
        );
        expectThrows([&] {
            (void) bedrock::BedrockNbt::parse(explicitTrailing);
        }, "automatic detection with unexplained trailing bytes");

        auto concatenated = little;
        concatenated.insert(concatenated.end(), little.begin(), little.end());
        const auto firstRoot = bedrock::BedrockNbt::parse(concatenated);
        require(
            firstRoot.parsed == document &&
                firstRoot.metadata.size == little.size() &&
                firstRoot.metadata.buffer == concatenated,
            "automatic detection must permit a following compound root"
        );

        require(
            bedrock::BedrockNbt::hasBedrockLevelHeader(levelDat) &&
                bedrock::BedrockNbt::hasLevelHeader(levelDat),
            "level.dat header was not recognised"
        );
        require(
            bedrock::BedrockNbt::writeLevelDat(document, 8) == levelDat,
            "writeLevelDat Node payload golden mismatch"
        );

        const auto genericLevel = bedrock::BedrockNbt::parse(levelDat);
        require(
            genericLevel.parsed == document &&
                genericLevel.encoding == bedrock::BedrockNbtEncoding::LittleEndian &&
                genericLevel.metadata.startOffset == 8 &&
                genericLevel.metadata.size == little.size() &&
                genericLevel.metadata.buffer == levelDat &&
                genericLevel.levelHeader &&
                genericLevel.levelHeader->version == 8 &&
                genericLevel.levelHeader->payloadLength == little.size(),
            "generic level.dat parse mismatch"
        );

        const auto strictLevel = bedrock::BedrockNbt::parseLevelDat(levelDat);
        require(
            strictLevel.parsed == document && strictLevel.levelHeader &&
                strictLevel.levelHeader->version == 8,
            "strict level.dat parse mismatch"
        );

        auto badLength = levelDat;
        badLength[4] = static_cast<uint8_t>(little.size() - 1);
        expectThrows([&] {
            (void) bedrock::BedrockNbt::parseLevelDat(badLength);
        }, "mismatched level.dat payload length");
        expectThrows([] {
            (void) bedrock::BedrockNbt::parseLevelDat({8, 0, 0, 0});
        }, "truncated level.dat header");

        const auto simplified = bedrock::BedrockNbt::simplify(document);
        const auto* x = simplified.get("x");
        const auto* name = simplified.get("name");
        const auto* list = simplified.get("list");
        require(
            simplified.kind == bedrock::ProtoDefValue::Kind::Object &&
                x && x->kind == bedrock::ProtoDefValue::Kind::Int &&
                x->intValue == 42 &&
                name && name->kind == bedrock::ProtoDefValue::Kind::String &&
                name->stringValue == "bedrock" &&
                list && list->kind == bedrock::ProtoDefValue::Kind::Array &&
                list->arrayValue.size() == 3 &&
                list->arrayValue[0].intValue == 1 &&
                list->arrayValue[1].intValue == -2 &&
                list->arrayValue[2].intValue == 3,
            "simplify mismatch"
        );

        const bedrock::NbtDocument reordered {
            "ignored-root-name",
            bedrock::NbtValue::compound({
                {"list", document.root.compoundValue[2].value},
                {"name", bedrock::NbtValue::string("bedrock")},
                {"x", bedrock::NbtValue::integer(42)}
            })
        };
        require(
            !(reordered == document) && bedrock::BedrockNbt::equal(reordered, document),
            "semantic compound equality must ignore field order and root name"
        );

        bedrock::BedrockNbtLimits shortListLimit;
        shortListLimit.maxCollectionLength = 2;
        expectThrows([&] {
            (void) bedrock::BedrockNbt::parseAs(
                little,
                bedrock::BedrockNbtEncoding::LittleEndian,
                0,
                shortListLimit
            );
        }, "high-level parser collection limit");

        std::cout << "[BEDROCK-NBT-FILE-SMOKE] ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[BEDROCK-NBT-FILE-SMOKE] " << error.what() << '\n';
        return 1;
    }
}
