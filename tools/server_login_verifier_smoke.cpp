#include <bedrock/LoginPacket.hpp>
#include <bedrock/auth/BedrockAuthJwt.hpp>
#include <bedrock/protodef/ProtoDefJson.hpp>
#include <bedrock/server/BedrockLoginVerifier.hpp>

#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using bedrock::ProtoDefValue;

std::string json(const ProtoDefValue& value) {
    return bedrock::ProtoDefJson::stringify(value);
}

std::string chainIdentity(const std::vector<std::string>& chain) {
    std::vector<ProtoDefValue> tokens;
    tokens.reserve(chain.size());
    for (const auto& token : chain) {
        tokens.push_back(ProtoDefValue::string(token));
    }
    return json(ProtoDefValue::object({
        {"chain", ProtoDefValue::array(std::move(tokens))}
    }));
}

std::string certificateIdentity(const std::vector<std::string>& chain) {
    return json(ProtoDefValue::object({
        {"Certificate", ProtoDefValue::string(chainIdentity(chain))}
    }));
}

std::string tamperSignature(std::string token) {
    const auto dot = token.rfind('.');
    if (dot == std::string::npos || dot + 1 >= token.size()) {
        throw std::runtime_error("test token has no signature");
    }
    token[dot + 1] = token[dot + 1] == 'A' ? 'B' : 'A';
    return token;
}

bool throws(const std::function<void()>& action) {
    try {
        action();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

const ProtoDefValue* member(const ProtoDefValue& value, const std::string& key) {
    return value.kind == ProtoDefValue::Kind::Object ? value.get(key) : nullptr;
}

bool stringEquals(
    const ProtoDefValue& value,
    const std::string& key,
    const std::string& expected
) {
    const auto* child = member(value, key);
    return child &&
        child->kind == ProtoDefValue::Kind::String &&
        child->stringValue == expected;
}

} // namespace

int main() {
    const auto firstKeys = bedrock::BedrockAuthJwt::generateP384KeyPair();
    const auto finalKeys = bedrock::BedrockAuthJwt::generateP384KeyPair();

    const auto firstToken = bedrock::BedrockAuthJwt::signEs384Jwt(
        firstKeys.privateKeyPem,
        firstKeys.publicKeyDerBase64,
        json(ProtoDefValue::object({
            {"identityPublicKey", ProtoDefValue::string(finalKeys.publicKeyDerBase64)},
            {"retained", ProtoDefValue::string("from-first")},
            {"overwritten", ProtoDefValue::string("first")}
        }))
    );
    const auto finalToken = bedrock::BedrockAuthJwt::signEs384Jwt(
        finalKeys.privateKeyPem,
        finalKeys.publicKeyDerBase64,
        json(ProtoDefValue::object({
            {"identityPublicKey", ProtoDefValue::string(finalKeys.publicKeyDerBase64)},
            {"overwritten", ProtoDefValue::string("second")},
            {"extraData", ProtoDefValue::object({
                {"displayName", ProtoDefValue::string("Verifier Smoke")},
                {"identity", ProtoDefValue::string("11111111-2222-3333-4444-555555555555")},
                {"xuid", ProtoDefValue::string("lower-xuid")},
                {"XUID", ProtoDefValue::string("upper-xuid")}
            })}
        }))
    );
    const auto skinToken = bedrock::BedrockAuthJwt::signEs384Jwt(
        finalKeys.privateKeyPem,
        finalKeys.publicKeyDerBase64,
        json(ProtoDefValue::object({
            {"GameVersion", ProtoDefValue::string("1.20.40")},
            {"ThirdPartyName", ProtoDefValue::string("Verifier Smoke")}
        }))
    );

    const std::vector<std::string> chain {firstToken, finalToken};
    bedrock::LoginPacketData login;
    login.protocolVersion = 622;
    login.identity = chainIdentity(chain);
    login.client = skinToken;

    const auto offline = bedrock::BedrockLoginVerifier::verify(login, true);
    if (offline.key != finalKeys.publicKeyDerBase64 ||
        offline.didVerify ||
        offline.disconnectNotAuthenticated ||
        offline.version != 622 ||
        !stringEquals(offline.data, "retained", "from-first") ||
        !stringEquals(offline.data, "overwritten", "second") ||
        !stringEquals(offline.userData, "displayName", "Verifier Smoke") ||
        !stringEquals(offline.skinData, "GameVersion", "1.20.40") ||
        offline.profile.name != "Verifier Smoke" ||
        offline.profile.uuid != "11111111-2222-3333-4444-555555555555" ||
        offline.profile.xuid != "lower-xuid") {
        std::cerr << "[SERVER-LOGIN-VERIFY] offline profile/merge mismatch\n";
        return 1;
    }

    auto badChainLogin = login;
    badChainLogin.identity = chainIdentity({firstToken, tamperSignature(finalToken)});
    if (!throws([&] { (void) bedrock::BedrockLoginVerifier::verify(badChainLogin, true); })) {
        std::cerr << "[SERVER-LOGIN-VERIFY] tampered auth chain accepted\n";
        return 1;
    }

    auto badSkinLogin = login;
    badSkinLogin.client = tamperSignature(skinToken);
    if (!throws([&] { (void) bedrock::BedrockLoginVerifier::verify(badSkinLogin, true); })) {
        std::cerr << "[SERVER-LOGIN-VERIFY] tampered skin token accepted\n";
        return 1;
    }

    const auto onlineSelfSigned = bedrock::BedrockLoginVerifier::verify(login, false);
    if (onlineSelfSigned.didVerify ||
        !onlineSelfSigned.disconnectNotAuthenticated ||
        onlineSelfSigned.profile.name != "Verifier Smoke") {
        std::cerr << "[SERVER-LOGIN-VERIFY] online self-signed control-flow mismatch\n";
        return 1;
    }

    auto certificateLogin = login;
    certificateLogin.identity = certificateIdentity(chain);
    const auto certificate = bedrock::BedrockLoginVerifier::verify(certificateLogin, true);
    if (certificate.key != finalKeys.publicKeyDerBase64 ||
        certificate.profile.xuid != "lower-xuid") {
        std::cerr << "[SERVER-LOGIN-VERIFY] Certificate chain shape mismatch\n";
        return 1;
    }

    const auto expired = bedrock::BedrockAuthJwt::signEs384Jwt(
        firstKeys.privateKeyPem,
        firstKeys.publicKeyDerBase64,
        json(ProtoDefValue::object({{"exp", ProtoDefValue::uinteger(100)}}))
    );
    if (!throws([&] {
            (void) bedrock::BedrockAuthJwt::verifyEs384Jwt(
                expired,
                firstKeys.publicKeyDerBase64,
                100
            );
        })) {
        std::cerr << "[SERVER-LOGIN-VERIFY] expired JWT accepted\n";
        return 1;
    }

    const auto notYetActive = bedrock::BedrockAuthJwt::signEs384Jwt(
        firstKeys.privateKeyPem,
        firstKeys.publicKeyDerBase64,
        json(ProtoDefValue::object({{"nbf", ProtoDefValue::uinteger(101)}}))
    );
    if (!throws([&] {
            (void) bedrock::BedrockAuthJwt::verifyEs384Jwt(
                notYetActive,
                firstKeys.publicKeyDerBase64,
                100
            );
        })) {
        std::cerr << "[SERVER-LOGIN-VERIFY] future nbf JWT accepted\n";
        return 1;
    }

    std::cout << "[SERVER-LOGIN-VERIFY] OK\n";
    return 0;
}
