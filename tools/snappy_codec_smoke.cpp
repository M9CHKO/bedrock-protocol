#include <bedrock/protocol/SnappyCodec.hpp>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        output << std::setw(2) << static_cast<unsigned>(byte);
    }
    return output.str();
}

bool checkDecode(
    const std::string& name,
    const std::vector<uint8_t>& encoded,
    const std::vector<uint8_t>& expected
) {
    try {
        const auto decoded = bedrock::SnappyCodec::decompress(encoded);
        if (decoded == expected) {
            return true;
        }
        std::cerr << "[SMOKE] " << name << " decoded bytes mismatch\n";
    } catch (const std::exception& error) {
        std::cerr << "[SMOKE] " << name << " unexpectedly failed: "
                  << error.what() << "\n";
    }
    return false;
}

bool checkRoundtrip(const std::string& name, const std::vector<uint8_t>& input) {
    try {
        const auto encoded = bedrock::SnappyCodec::compress(input);
        const auto decoded = bedrock::SnappyCodec::decompress(encoded);
        if (decoded == input) {
            return true;
        }
        std::cerr << "[SMOKE] " << name << " roundtrip mismatch\n";
    } catch (const std::exception& error) {
        std::cerr << "[SMOKE] " << name << " roundtrip failed: "
                  << error.what() << "\n";
    }
    return false;
}

bool checkRejects(const std::string& name, const std::vector<uint8_t>& encoded) {
    try {
        (void) bedrock::SnappyCodec::decompress(encoded);
        std::cerr << "[SMOKE] malformed " << name << " block was accepted\n";
        return false;
    } catch (const bedrock::SnappyCodecError&) {
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[SMOKE] malformed " << name << " threw wrong type: "
                  << error.what() << "\n";
        return false;
    }
}

std::vector<uint8_t> sequence(std::size_t size, uint32_t modulus) {
    std::vector<uint8_t> output(size);
    for (std::size_t i = 0; i < size; ++i) {
        output[i] = static_cast<uint8_t>(i % modulus);
    }
    return output;
}

std::vector<uint8_t> pseudoRandom(std::size_t size) {
    std::vector<uint8_t> output(size);
    uint32_t state = 0x12345678u;
    for (auto& byte : output) {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        byte = static_cast<uint8_t>(state);
    }
    return output;
}

void emitOracleFixtures() {
    const std::vector<std::pair<std::string, std::vector<uint8_t>>> fixtures {
        {"hello", {'h', 'e', 'l', 'l', 'o'}},
        {"run512", std::vector<uint8_t>(512, 0x5a)},
        {"pattern4096", sequence(4096, 251)}
    };

    for (const auto& [name, input] : fixtures) {
        std::cout << name << '=' << hex(bedrock::SnappyCodec::compress(input)) << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--emit-oracle") {
        emitOracleFixtures();
        return 0;
    }

    bool ok = true;

    // Fixed one-shot raw-block fixtures produced by the independent Node
    // `snappy` package, whose native implementation uses Google Snappy.
    ok &= checkDecode("empty oracle", {0x00}, {});
    ok &= checkDecode(
        "short literal oracle",
        {0x05, 0x10, 'h', 'e', 'l', 'l', 'o'},
        {'h', 'e', 'l', 'l', 'o'}
    );
    ok &= checkDecode(
        "copy-2 oracle",
        {0x20, 0x00, 'a', 0x7a, 0x01, 0x00},
        std::vector<uint8_t>(32, 'a')
    );
    ok &= checkDecode(
        "overlapping copy oracle",
        {0x18, 0x08, 'a', 'b', 'c', 0x52, 0x03, 0x00},
        {'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c',
         'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c'}
    );

    auto literal61 = sequence(61, 256);
    std::vector<uint8_t> literal61Oracle {0x3d, 0xf0, 0x3c};
    literal61Oracle.insert(literal61Oracle.end(), literal61.begin(), literal61.end());
    ok &= checkDecode("long literal oracle", literal61Oracle, literal61);

    auto literal256 = sequence(256, 256);
    std::vector<uint8_t> literal256Oracle {0x80, 0x02, 0xf0, 0xff};
    literal256Oracle.insert(literal256Oracle.end(), literal256.begin(), literal256.end());
    ok &= checkDecode("multibyte length oracle", literal256Oracle, literal256);

    ok &= checkDecode(
        "long run oracle",
        {
            0x80, 0x04, 0x00, 0x5a,
            0xfe, 0x01, 0x00, 0xfe, 0x01, 0x00,
            0xfe, 0x01, 0x00, 0xfe, 0x01, 0x00,
            0xfe, 0x01, 0x00, 0xfe, 0x01, 0x00,
            0xfe, 0x01, 0x00, 0xfa, 0x01, 0x00
        },
        std::vector<uint8_t>(512, 0x5a)
    );

    ok &= checkDecode(
        "copy-1 tag",
        {0x08, 0x0c, 'a', 'b', 'c', 'd', 0x01, 0x04},
        {'a', 'b', 'c', 'd', 'a', 'b', 'c', 'd'}
    );
    ok &= checkDecode(
        "copy-4 tag",
        {0x08, 0x0c, 'a', 'b', 'c', 'd', 0x0f, 0x04, 0x00, 0x00, 0x00},
        {'a', 'b', 'c', 'd', 'a', 'b', 'c', 'd'}
    );

    const auto run = std::vector<uint8_t>(512, 0x5a);
    ok &= checkRoundtrip("empty", {});
    ok &= checkRoundtrip("short", {'b', 'e', 'd', 'r', 'o', 'c', 'k'});
    ok &= checkRoundtrip("run", run);
    ok &= checkRoundtrip("fragment boundary", sequence(65536, 251));
    ok &= checkRoundtrip("multiple fragments", sequence(200000, 251));
    ok &= checkRoundtrip("incompressible", pseudoRandom(200000));

    if (bedrock::SnappyCodec::compress(run).size() >= run.size()) {
        std::cerr << "[SMOKE] repeated input was not actually compressed\n";
        ok = false;
    }

    ok &= checkRejects("missing length", {});
    ok &= checkRejects("truncated length", {0x80});
    ok &= checkRejects("oversized length", {0xff, 0xff, 0xff, 0xff, 0x10});
    ok &= checkRejects("truncated literal", {0x01, 0x00});
    ok &= checkRejects("literal exceeds size", {0x01, 0x04, 'a', 'b'});
    ok &= checkRejects("zero copy offset", {0x01, 0x02, 0x00, 0x00});
    ok &= checkRejects("copy before output", {0x04, 0x01, 0x02});
    ok &= checkRejects("short output", {0x02, 0x00, 'a'});
    ok &= checkRejects("trailing tag", {0x01, 0x00, 'a', 0x00, 'b'});

    if (!ok) {
        return 1;
    }

    std::cout << "[SMOKE] snappy codec ok\n";
    return 0;
}
