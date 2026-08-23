#include <bedrock/server/BedrockLoginVerifier.hpp>

#include <bedrock/BedrockKeyExchange.hpp>
#include <bedrock/auth/BedrockAuthJwt.hpp>
#include <bedrock/protodef/ProtoDefJson.hpp>

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bedrock {

namespace {

bool jsTruthy(const ProtoDefValue* value) {
    if (!value) {
        return false;
    }
    switch (value->kind) {
        case ProtoDefValue::Kind::Null:
            return false;
        case ProtoDefValue::Kind::Bool:
            return value->boolValue;
        case ProtoDefValue::Kind::Int:
            return value->intValue != 0;
        case ProtoDefValue::Kind::UInt:
            return value->uintValue != 0;
        case ProtoDefValue::Kind::Double:
            return value->doubleValue != 0.0 && !std::isnan(value->doubleValue);
        case ProtoDefValue::Kind::String:
            return !value->stringValue.empty();
        case ProtoDefValue::Kind::Bytes:
        case ProtoDefValue::Kind::Object:
        case ProtoDefValue::Kind::Array:
            return true;
    }
    return false;
}

const ProtoDefValue& requireObject(const ProtoDefValue& value, const char* message) {
    if (value.kind != ProtoDefValue::Kind::Object) {
        throw std::runtime_error(message);
    }
    return value;
}

std::string requireString(const ProtoDefValue* value, const char* message) {
    if (!value || value->kind != ProtoDefValue::Kind::String) {
        throw std::runtime_error(message);
    }
    return value->stringValue;
}

std::string optionalString(const ProtoDefValue* value) {
    return value && value->kind == ProtoDefValue::Kind::String
        ? value->stringValue
        : std::string{};
}

std::string jwtX5u(const std::string& token) {
    const auto header = ProtoDefJson::parse(BedrockKeyExchange::extractJwtHeaderJson(token));
    requireObject(header, "Invalid login JWT header");
    return requireString(header.get("x5u"), "Invalid login JWT x5u");
}

std::vector<std::string> chainFromValue(const ProtoDefValue& value) {
    if (value.kind != ProtoDefValue::Kind::Array) {
        throw std::runtime_error("Invalid login packet chain");
    }
    std::vector<std::string> chain;
    chain.reserve(value.arrayValue.size());
    for (const auto& token : value.arrayValue) {
        chain.push_back(requireString(&token, "Invalid login packet chain token"));
    }
    return chain;
}

std::vector<std::string> extractChain(const std::string& identityJson) {
    const auto authChain = ProtoDefJson::parse(identityJson);
    requireObject(authChain, "Invalid login packet auth chain");

    if (const auto* certificate = authChain.get("Certificate"); jsTruthy(certificate)) {
        const auto certificateJson = requireString(
            certificate,
            "Invalid login packet Certificate"
        );
        const auto certificateChain = ProtoDefJson::parse(certificateJson);
        requireObject(certificateChain, "Invalid login packet Certificate");
        const auto* chain = certificateChain.get("chain");
        if (!chain) {
            throw std::runtime_error("Invalid login packet Certificate chain");
        }
        return chainFromValue(*chain);
    }

    if (const auto* chain = authChain.get("chain"); jsTruthy(chain)) {
        return chainFromValue(*chain);
    }

    throw std::runtime_error("Invalid login packet: missing chain or Certificate");
}

void shallowMerge(ProtoDefValue& target, const ProtoDefValue& source) {
    if (source.kind == ProtoDefValue::Kind::Object) {
        for (const auto& [key, value] : source.objectValue) {
            target.objectValue[key] = value;
        }
        return;
    }

    if (source.kind == ProtoDefValue::Kind::Array) {
        for (std::size_t i = 0; i < source.arrayValue.size(); ++i) {
            target.objectValue[std::to_string(i)] = source.arrayValue[i];
        }
        return;
    }

    if (source.kind == ProtoDefValue::Kind::String) {
        for (std::size_t i = 0; i < source.stringValue.size(); ++i) {
            target.objectValue[std::to_string(i)] = ProtoDefValue::string(
                source.stringValue.substr(i, 1)
            );
        }
    }
}

} // namespace

const std::string& BedrockLoginVerifier::mojangPublicKey() {
    static const std::string key =
        "MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAECRXueJeTDqNRRgJi/vlRufByu/2G0i2Ebt6YMar5QX/"
        "R0DIIyrJMcUpruK4QveTfJSTp3Shlq4Gk34cD/4GUWwkv0DVuzeuB+tXija7HBxii03NHDbPAD0A"
        "KnLr2wdAp";
    return key;
}

BedrockLoginVerificationResult BedrockLoginVerifier::verify(
    const LoginPacketData& login,
    bool offline
) {
    const auto chain = extractChain(login.identity);
    if (chain.empty()) {
        throw std::runtime_error("Invalid login packet: empty chain");
    }

    ProtoDefValue data = ProtoDefValue::object({});
    bool didVerify = false;
    std::string publicKey = jwtX5u(chain.front());
    std::string finalKey;

    for (const auto& token : chain) {
        const auto verified = BedrockAuthJwt::verifyEs384Jwt(token, publicKey);
        const auto header = ProtoDefJson::parse(verified.headerJson);
        const auto decoded = ProtoDefJson::parse(verified.payloadJson);
        requireObject(header, "Invalid login JWT header");

        const std::string x5u = requireString(
            header.get("x5u"),
            "Invalid login JWT x5u"
        );
        const auto* accumulatedExtraData = data.get("extraData");
        const auto* accumulatedXuid = accumulatedExtraData &&
                accumulatedExtraData->kind == ProtoDefValue::Kind::Object
            ? accumulatedExtraData->get("XUID")
            : nullptr;
        if (x5u == mojangPublicKey() && !jsTruthy(accumulatedXuid)) {
            didVerify = true;
        }

        const auto* identityPublicKey = decoded.kind == ProtoDefValue::Kind::Object
            ? decoded.get("identityPublicKey")
            : nullptr;
        if (jsTruthy(identityPublicKey)) {
            publicKey = requireString(
                identityPublicKey,
                "Invalid identityPublicKey"
            );
            finalKey = publicKey;
        } else {
            publicKey = x5u;
        }

        shallowMerge(data, decoded);
    }

    const auto verifiedSkin = BedrockAuthJwt::verifyEs384Jwt(login.client, finalKey);
    const auto skinData = ProtoDefJson::parse(verifiedSkin.payloadJson);

    const auto* extraData = data.get("extraData");
    ProtoDefValue userData = extraData ? *extraData : ProtoDefValue::null();
    const ProtoDefValue* profileData = userData.kind == ProtoDefValue::Kind::Object
        ? &userData
        : nullptr;

    BedrockLoginProfile profile;
    if (profileData) {
        profile.name = optionalString(profileData->get("displayName"));
        profile.uuid = optionalString(profileData->get("identity"));
        const auto* lowerXuid = profileData->get("xuid");
        profile.xuid = jsTruthy(lowerXuid)
            ? requireString(lowerXuid, "Invalid profile xuid")
            : optionalString(profileData->get("XUID"));
    }

    BedrockLoginVerificationResult result;
    result.key = std::move(finalKey);
    result.data = std::move(data);
    result.userData = std::move(userData);
    result.skinData = std::move(skinData);
    result.profile = std::move(profile);
    result.version = login.protocolVersion;
    result.didVerify = didVerify;
    result.disconnectNotAuthenticated = !offline && !didVerify;
    return result;
}

} // namespace bedrock
