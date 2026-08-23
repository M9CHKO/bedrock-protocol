#pragma once

#include <bedrock/LoginPacket.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>

#include <cstdint>
#include <string>

namespace bedrock {

struct BedrockLoginProfile {
    std::string name;
    std::string uuid;
    std::string xuid;
};

struct BedrockLoginVerificationResult {
    std::string key;
    ProtoDefValue data;
    ProtoDefValue userData;
    ProtoDefValue skinData;
    BedrockLoginProfile profile;
    uint32_t version = 0;
    bool didVerify = false;
    bool disconnectNotAuthenticated = false;
};

class BedrockLoginVerifier {
public:
    static const std::string& mojangPublicKey();

    static BedrockLoginVerificationResult verify(
        const LoginPacketData& login,
        bool offline
    );
};

} // namespace bedrock
