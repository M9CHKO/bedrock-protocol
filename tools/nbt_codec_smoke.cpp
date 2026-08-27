#include <bedrock/nbt/BedrockNbt.hpp>

#include <cctype>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> fromHex(const std::string& value) {
    if ((value.size() & 1u) != 0) {
        throw std::runtime_error("odd hex input");
    }
    auto nibble = [](char ch) -> uint8_t {
        if (ch >= '0' && ch <= '9') return static_cast<uint8_t>(ch - '0');
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ch >= 'a' && ch <= 'f') return static_cast<uint8_t>(ch - 'a' + 10);
        throw std::runtime_error("invalid hex input");
    };

    std::vector<uint8_t> out;
    out.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((nibble(value[i]) << 4u) | nibble(value[i + 1])));
    }
    return out;
}

bedrock::NbtDocument makeDocument() {
    using bedrock::NbtTagType;
    using bedrock::NbtValue;

    return {
        "root",
        NbtValue::compound({
            {"byte", NbtValue::byte(-5)},
            {"short", NbtValue::shortInteger(-1234)},
            {"int", NbtValue::integer(-123456789)},
            {"long", NbtValue::longInteger(0x0123456789abcdefll)},
            {"float", NbtValue::floating(1.5f)},
            {"double", NbtValue::doubleFloating(-0.25)},
            {"bytes", NbtValue::byteArray({0, 255, 127, 128})},
            {"string", NbtValue::string("Bedrock\xe2\x98\x83")},
            {"list", NbtValue::list(NbtTagType::Int, {
                NbtValue::integer(-1),
                NbtValue::integer(0),
                NbtValue::integer(300)
            })},
            {"compound", NbtValue::compound({
                {"flag", NbtValue::byte(1)}
            })},
            {"ints", NbtValue::intArray({-1, 0, 300000})},
            {"longs", NbtValue::longArray({1, -1})}
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

void checkGolden(
    bedrock::BedrockNbtEncoding encoding,
    const std::string& goldenHex,
    const char* label
) {
    const auto expected = fromHex(goldenHex);
    const auto document = makeDocument();

    bedrock::BinaryStream encoded;
    bedrock::BedrockNbtCodec::write(encoded, document, encoding);
    if (encoded.buffer() != expected) {
        throw std::runtime_error(std::string(label) + " Node golden mismatch");
    }

    bedrock::BinaryStream input(expected);
    const auto decoded = bedrock::BedrockNbtCodec::read(input, encoding);
    if (!(decoded == document) || !input.eof()) {
        throw std::runtime_error(std::string(label) + " decode mismatch");
    }

    bedrock::BinaryStream sequential;
    bedrock::BedrockNbtCodec::write(sequential, document, encoding);
    bedrock::BedrockNbtCodec::write(sequential, document, encoding);
    bedrock::BinaryStream sequentialInput(sequential.buffer());
    if (!(bedrock::BedrockNbtCodec::read(sequentialInput, encoding) == document) ||
        !(bedrock::BedrockNbtCodec::read(sequentialInput, encoding) == document) ||
        !sequentialInput.eof()) {
        throw std::runtime_error(std::string(label) + " sequential decode mismatch");
    }
}

} // namespace

int main() {
    try {
        // Generated independently with prismarine-nbt 2.8.0 from node_modules.
        checkGolden(
            bedrock::BedrockNbtEncoding::LittleEndian,
            "0a0400726f6f7401040062797465fb02050073686f72742efb030300696e74eb32a4f8"
            "0404006c6f6e67efcdab8967452301050500666c6f61740000c03f060600646f75626c"
            "65000000000000d0bf07050062797465730400000000ff7f80080600737472696e670a"
            "00426564726f636be298830904006c6973740303000000ffffffff000000002c0100000a"
            "0800636f6d706f756e64010400666c616701000b0400696e747303000000ffffffff0000"
            "0000e09304000c05006c6f6e6773020000000100000000000000ffffffffffffffff00",
            "little-endian NBT"
        );

        checkGolden(
            bedrock::BedrockNbtEncoding::LittleVarInt,
            "0a04726f6f74010462797465fb020573686f72742efb0303696e74a9b4de7504046c6f"
            "6e67deb7de9af1d9a2a3020505666c6f61740000c03f0606646f75626c650000000000"
            "00d0bf070562797465730800ff7f800806737472696e670a426564726f636be2988309"
            "046c69737403060100d8040a08636f6d706f756e640104666c616701000b04696e7473"
            "06ffffffff00000000e09304000c056c6f6e6773040100000000000000ffffffffffff"
            "ffff00",
            "little-varint NBT"
        );

        const auto unnamed = bedrock::NbtValue::list(
            bedrock::NbtTagType::Long,
            {
                bedrock::NbtValue::longInteger(std::numeric_limits<int64_t>::min()),
                bedrock::NbtValue::longInteger(std::numeric_limits<int64_t>::max())
            }
        );
        bedrock::BinaryStream unnamedBytes;
        bedrock::BedrockNbtCodec::writeUnnamed(
            unnamedBytes,
            unnamed,
            bedrock::BedrockNbtEncoding::LittleVarInt
        );
        bedrock::BinaryStream unnamedInput(unnamedBytes.buffer());
        if (!(bedrock::BedrockNbtCodec::readUnnamed(
                unnamedInput,
                bedrock::BedrockNbtEncoding::LittleVarInt
            ) == unnamed) || !unnamedInput.eof()) {
            throw std::runtime_error("unnamed NBT roundtrip mismatch");
        }

        expectThrows([] {
            bedrock::BinaryStream input(std::vector<uint8_t>{13});
            (void) bedrock::BedrockNbtCodec::read(
                input,
                bedrock::BedrockNbtEncoding::LittleEndian
            );
        }, "unknown tag");

        expectThrows([] {
            bedrock::BinaryStream input(std::vector<uint8_t>{7, 0, 1});
            (void) bedrock::BedrockNbtCodec::read(
                input,
                bedrock::BedrockNbtEncoding::LittleVarInt
            );
        }, "negative byte array length");

        expectThrows([] {
            bedrock::BinaryStream input(std::vector<uint8_t>{8, 0, 5, 'a'});
            (void) bedrock::BedrockNbtCodec::read(
                input,
                bedrock::BedrockNbtEncoding::LittleVarInt
            );
        }, "truncated string");

        expectThrows([] {
            bedrock::NbtValue invalid = bedrock::NbtValue::list(
                bedrock::NbtTagType::End,
                {bedrock::NbtValue::end()}
            );
            bedrock::BinaryStream output;
            bedrock::BedrockNbtCodec::writeUnnamed(
                output,
                invalid,
                bedrock::BedrockNbtEncoding::LittleVarInt
            );
        }, "non-empty end list");

        std::cout << "[NBT-CODEC-SMOKE] ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[NBT-CODEC-SMOKE] " << error.what() << '\n';
        return 1;
    }
}
