#include <bedrock/world/BedrockSubChunkPacket.hpp>

#include <openssl/sha.h>

#include <array>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string sha256Hex(const std::vector<uint8_t>& value) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest {};
    SHA256(value.data(), value.size(), digest.data());
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        out << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return out.str();
}

std::vector<int8_t> ascendingHeightMap() {
    std::vector<int8_t> out;
    out.reserve(256);
    for (int value = 0; value < 256; ++value) {
        out.push_back(static_cast<int8_t>(value));
    }
    return out;
}

std::vector<int8_t> descendingHeightMap() {
    std::vector<int8_t> out;
    out.reserve(256);
    for (int value = 255; value >= 0; --value) {
        out.push_back(static_cast<int8_t>(value));
    }
    return out;
}

void checkGolden(
    const bedrock::BedrockSubChunkPacket& packet,
    const std::string& version,
    std::size_t expectedSize,
    const std::string& expectedSha256
) {
    const auto encoded = bedrock::BedrockSubChunkPacketCodec::encodePacketPayload(
        packet,
        version
    );
    if (encoded.size() != expectedSize || sha256Hex(encoded) != expectedSha256) {
        throw std::runtime_error(version + " Node ProtoDef golden mismatch");
    }

    const auto header = bedrock::BedrockSubChunkPacketCodec::decodePacketHeader(
        encoded,
        version
    );
    if ((version != "1.18.0" &&
         header.cacheEnabled != packet.cacheEnabled) ||
        header.dimension != packet.dimension ||
        header.originX != packet.originX || header.originY != packet.originY ||
        header.originZ != packet.originZ || !header.entries.empty()) {
        throw std::runtime_error(version + " header-only decode mismatch");
    }

    const auto decoded = bedrock::BedrockSubChunkPacketCodec::decodePacketPayload(
        encoded,
        version
    );
    const auto reencoded = bedrock::BedrockSubChunkPacketCodec::encodePacketPayload(
        decoded,
        version
    );
    if (reencoded != encoded) {
        throw std::runtime_error(version + " decode/encode roundtrip mismatch");
    }
}

void expectThrows(const std::function<void()>& callback, const char* label) {
    try {
        callback();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(std::string(label) + " did not throw");
}

} // namespace

int main() {
    try {
        bedrock::BedrockSubChunkPacket legacy;
        legacy.cacheEnabled = true;
        legacy.dimension = 2;
        legacy.originX = -3;
        legacy.originY = -4;
        legacy.originZ = 5;
        bedrock::BedrockSubChunkPacketEntry legacyEntry;
        legacyEntry.result = bedrock::BedrockSubChunkResult::Success;
        legacyEntry.payload = {9, 1, 2};
        legacyEntry.heightMapType = bedrock::BedrockHeightMapType::HasData;
        legacyEntry.heightMap = ascendingHeightMap();
        legacyEntry.blobId = 0x1122334455667788ull;
        legacy.entries.push_back(std::move(legacyEntry));
        checkGolden(
            legacy,
            "1.18.0",
            275,
            "2b33fceb0b9488e9ea7b0bad5ea59c6e47fb2e36b987e7eddafb528b4eabe528"
        );

        bedrock::BedrockSubChunkPacket batched;
        batched.cacheEnabled = false;
        batched.dimension = -1;
        batched.originX = 10;
        batched.originY = -4;
        batched.originZ = -8;
        bedrock::BedrockSubChunkPacketEntry first;
        first.dx = 1;
        first.dy = 2;
        first.dz = -3;
        first.result = bedrock::BedrockSubChunkResult::Success;
        first.payload = {9, 1, 2, 3};
        first.heightMapType = bedrock::BedrockHeightMapType::HasData;
        first.heightMap = ascendingHeightMap();
        batched.entries.push_back(std::move(first));
        bedrock::BedrockSubChunkPacketEntry second;
        second.dx = -4;
        second.dy = 5;
        second.dz = 6;
        second.result = bedrock::BedrockSubChunkResult::SuccessAllAir;
        second.heightMapType = bedrock::BedrockHeightMapType::NoData;
        batched.entries.push_back(std::move(second));
        checkGolden(
            batched,
            "1.18.11",
            281,
            "0fbd57d02b14eca2f68c087b09679085c100123ff368e3df6acc70a94b3cf09a"
        );

        bedrock::BedrockSubChunkPacket render;
        render.cacheEnabled = true;
        render.dimension = 0;
        render.originX = -20;
        render.originY = 3;
        render.originZ = 40;
        bedrock::BedrockSubChunkPacketEntry renderFirst;
        renderFirst.dx = -1;
        renderFirst.dy = -2;
        renderFirst.dz = 3;
        renderFirst.result = bedrock::BedrockSubChunkResult::Success;
        renderFirst.payload = {10, 20};
        renderFirst.heightMapType = bedrock::BedrockHeightMapType::TooLow;
        renderFirst.renderHeightMapType = bedrock::BedrockHeightMapType::HasData;
        renderFirst.renderHeightMap = descendingHeightMap();
        renderFirst.blobId = 0xfedcba9876543210ull;
        render.entries.push_back(std::move(renderFirst));
        bedrock::BedrockSubChunkPacketEntry renderSecond;
        renderSecond.dx = 4;
        renderSecond.dy = 5;
        renderSecond.dz = -6;
        renderSecond.result = bedrock::BedrockSubChunkResult::SuccessAllAir;
        renderSecond.heightMapType = bedrock::BedrockHeightMapType::NoData;
        renderSecond.renderHeightMapType = bedrock::BedrockHeightMapType::TooHigh;
        renderSecond.blobId = 1;
        render.entries.push_back(std::move(renderSecond));
        checkGolden(
            render,
            "1.21.100",
            296,
            "49eee102335fae3532db7e9abe7b68f007eb19775212768a44d5e1f022605237"
        );

        bedrock::BedrockSubChunkPacket allCopied;
        allCopied.cacheEnabled = false;
        allCopied.dimension = 0;
        allCopied.originX = 1;
        allCopied.originY = -4;
        allCopied.originZ = 2;
        bedrock::BedrockSubChunkPacketEntry allCopiedEntry;
        allCopiedEntry.result = bedrock::BedrockSubChunkResult::SuccessAllAir;
        allCopiedEntry.heightMapType = bedrock::BedrockHeightMapType::AllCopied;
        allCopiedEntry.renderHeightMapType =
            bedrock::BedrockHeightMapType::AllCopied;
        allCopied.entries.push_back(std::move(allCopiedEntry));
        checkGolden(
            allCopied,
            "1.21.100",
            16,
            "29ddc7ff207324e2d3751a79aaa318f498c413ae00c7aa35f4b33fce195f0010"
        );

        expectThrows([] {
            bedrock::BedrockSubChunkPacket invalid;
            invalid.entries.resize(2);
            (void) bedrock::BedrockSubChunkPacketCodec::encodePacketPayload(
                invalid,
                "1.18.0"
            );
        }, "legacy entry count");

        expectThrows([] {
            std::vector<uint8_t> invalid {0, 0, 0, 0, 0, 1, 0, 0, 0};
            (void) bedrock::BedrockSubChunkPacketCodec::decodePacketPayload(
                invalid,
                "1.18.11"
            );
        }, "truncated modern entry");

        expectThrows([] {
            bedrock::BedrockSubChunkPacket invalid;
            invalid.entries.resize(1);
            invalid.entries[0].result = bedrock::BedrockSubChunkResult::Success;
            invalid.entries[0].heightMapType = bedrock::BedrockHeightMapType::HasData;
            invalid.entries[0].heightMap.resize(255);
            (void) bedrock::BedrockSubChunkPacketCodec::encodePacketPayload(
                invalid,
                "1.18.11"
            );
        }, "short heightmap");

        std::cout << "[SUBCHUNK-PACKET-SMOKE] ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[SUBCHUNK-PACKET-SMOKE] " << error.what() << '\n';
        return 1;
    }
}
