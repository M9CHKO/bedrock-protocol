#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bedrock {

struct BedrockClientKeyPair {
    std::string privateKeyPem;
    std::string publicKeyDerBase64;
};

struct BedrockVerifiedJwt {
    std::string headerJson;
    std::string payloadJson;
};

class BedrockAuthJwt {
public:
    static BedrockClientKeyPair generateP384KeyPair();

    static void saveKeyPair(
        const std::filesystem::path& privatePemPath,
        const std::filesystem::path& publicDerB64Path,
        const BedrockClientKeyPair& keys
    );

    static BedrockClientKeyPair loadOrCreateKeyPair(
        const std::filesystem::path& privatePemPath,
        const std::filesystem::path& publicDerB64Path
    );

    static std::string signEs384Jwt(
        const std::string& privateKeyPem,
        const std::string& publicKeyDerBase64,
        const std::string& payloadJson
    );

    static BedrockVerifiedJwt verifyEs384Jwt(
        const std::string& jwt,
        const std::string& publicKeyDerBase64,
        int64_t clockTimestamp = -1
    );

    static std::string base64(const std::vector<uint8_t>& data);
    static std::string base64Url(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> utf8(const std::string& s);
};

} // namespace bedrock
