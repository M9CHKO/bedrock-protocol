#include <bedrock/auth/BedrockAuthJwt.hpp>

#include <bedrock/BedrockKeyExchange.hpp>
#include <bedrock/protodef/ProtoDefJson.hpp>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <filesystem>
#include <fstream>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

namespace {

std::vector<std::string> splitJwtExact(const std::string& jwt) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= jwt.size(); ++i) {
        if (i == jwt.size() || jwt[i] == '.') {
            parts.push_back(jwt.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.size() != 3) {
        throw std::runtime_error("jwt malformed");
    }
    return parts;
}

double jsonNumber(const ProtoDefValue& value, const char* field) {
    switch (value.kind) {
        case ProtoDefValue::Kind::Int:
            return static_cast<double>(value.intValue);
        case ProtoDefValue::Kind::UInt:
            return static_cast<double>(value.uintValue);
        case ProtoDefValue::Kind::Double:
            return value.doubleValue;
        default:
            throw std::runtime_error(std::string("invalid ") + field + " value");
    }
}

void validateJwtTimes(const ProtoDefValue& payload, int64_t clockTimestamp) {
    if (payload.kind != ProtoDefValue::Kind::Object) {
        return;
    }
    const double now = static_cast<double>(
        clockTimestamp >= 0 ? clockTimestamp : static_cast<int64_t>(std::time(nullptr))
    );
    if (const auto* nbf = payload.get("nbf")) {
        if (jsonNumber(*nbf, "nbf") > now) {
            throw std::runtime_error("jwt not active");
        }
    }
    if (const auto* exp = payload.get("exp")) {
        if (now >= jsonNumber(*exp, "exp")) {
            throw std::runtime_error("jwt expired");
        }
    }
}

void verifyJoseEs384(
    const std::string& signingInput,
    const std::vector<uint8_t>& joseSignature,
    const std::string& publicKeyDerBase64
) {
    if (joseSignature.size() != 96) {
        throw std::runtime_error("invalid ES384 signature length");
    }

    const auto publicDer = BedrockKeyExchange::base64Decode(publicKeyDerBase64);
    const unsigned char* publicDerPtr = publicDer.data();
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> publicKey(
        d2i_PUBKEY(nullptr, &publicDerPtr, static_cast<long>(publicDer.size())),
        EVP_PKEY_free
    );
    if (!publicKey) {
        throw std::runtime_error("secretOrPublicKey is not valid key material");
    }
    if (EVP_PKEY_base_id(publicKey.get()) != EVP_PKEY_EC) {
        throw std::runtime_error("ES384 requires an EC public key");
    }

    const EC_KEY* ecKey = EVP_PKEY_get0_EC_KEY(publicKey.get());
    const EC_GROUP* group = ecKey ? EC_KEY_get0_group(ecKey) : nullptr;
    if (!group || EC_GROUP_get_curve_name(group) != NID_secp384r1) {
        throw std::runtime_error("ES384 requires curve secp384r1");
    }

    std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> signature(
        ECDSA_SIG_new(),
        ECDSA_SIG_free
    );
    if (!signature) {
        throw std::runtime_error("ECDSA_SIG_new failed");
    }
    BIGNUM* r = BN_bin2bn(joseSignature.data(), 48, nullptr);
    BIGNUM* s = BN_bin2bn(joseSignature.data() + 48, 48, nullptr);
    if (!r || !s || ECDSA_SIG_set0(signature.get(), r, s) != 1) {
        BN_free(r);
        BN_free(s);
        throw std::runtime_error("invalid ES384 signature");
    }

    const int derLength = i2d_ECDSA_SIG(signature.get(), nullptr);
    if (derLength <= 0) {
        throw std::runtime_error("i2d_ECDSA_SIG length failed");
    }
    std::vector<uint8_t> derSignature(static_cast<std::size_t>(derLength));
    unsigned char* derSignaturePtr = derSignature.data();
    if (i2d_ECDSA_SIG(signature.get(), &derSignaturePtr) != derLength) {
        throw std::runtime_error("i2d_ECDSA_SIG failed");
    }

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(
        EVP_MD_CTX_new(),
        EVP_MD_CTX_free
    );
    if (!digest ||
        EVP_DigestVerifyInit(digest.get(), nullptr, EVP_sha384(), nullptr, publicKey.get()) != 1 ||
        EVP_DigestVerifyUpdate(digest.get(), signingInput.data(), signingInput.size()) != 1 ||
        EVP_DigestVerifyFinal(
            digest.get(),
            derSignature.data(),
            derSignature.size()
        ) != 1) {
        throw std::runtime_error("invalid signature");
    }
}

} // namespace

static std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("failed to read: " + path.string());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void writeTextFile(const std::filesystem::path& path, const std::string& data) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    if (!out) throw std::runtime_error("failed to write: " + path.string());
    out << data;
}

std::vector<uint8_t> BedrockAuthJwt::utf8(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::string BedrockAuthJwt::base64(const std::vector<uint8_t>& data) {
    if (data.empty()) return "";

    std::string out;
    out.resize(((data.size() + 2) / 3) * 4);

    int len = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(out.data()),
        data.data(),
        static_cast<int>(data.size())
    );

    if (len < 0) throw std::runtime_error("EVP_EncodeBlock failed");

    out.resize(static_cast<std::size_t>(len));
    return out;
}

std::string BedrockAuthJwt::base64Url(const std::vector<uint8_t>& data) {
    std::string out = base64(data);

    while (!out.empty() && out.back() == '=') {
        out.pop_back();
    }

    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }

    return out;
}

BedrockClientKeyPair BedrockAuthJwt::generateP384KeyPair() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!ctx) throw std::runtime_error("EVP_PKEY_CTX_new_id failed");

    if (EVP_PKEY_keygen_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("EVP_PKEY_keygen_init failed");
    }

    if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_secp384r1) != 1) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("EVP_PKEY_CTX_set_ec_paramgen_curve_nid failed");
    }

    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen(ctx, &pkey) != 1 || !pkey) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("EVP_PKEY_keygen failed");
    }

    EVP_PKEY_CTX_free(ctx);

    BIO* privateBio = BIO_new(BIO_s_mem());
    if (!privateBio) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("BIO_new private failed");
    }

    if (PEM_write_bio_PrivateKey(privateBio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        BIO_free(privateBio);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("PEM_write_bio_PrivateKey failed");
    }

    BUF_MEM* privateMem = nullptr;
    BIO_get_mem_ptr(privateBio, &privateMem);

    std::string privatePem(privateMem->data, privateMem->length);
    BIO_free(privateBio);

    int derLen = i2d_PUBKEY(pkey, nullptr);
    if (derLen <= 0) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("i2d_PUBKEY length failed");
    }

    std::vector<uint8_t> der(static_cast<std::size_t>(derLen));
    unsigned char* derPtr = der.data();

    if (i2d_PUBKEY(pkey, &derPtr) != derLen) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("i2d_PUBKEY failed");
    }

    EVP_PKEY_free(pkey);

    BedrockClientKeyPair out;
    out.privateKeyPem = privatePem;
    out.publicKeyDerBase64 = base64(der);
    return out;
}

void BedrockAuthJwt::saveKeyPair(
    const std::filesystem::path& privatePemPath,
    const std::filesystem::path& publicDerB64Path,
    const BedrockClientKeyPair& keys
) {
    writeTextFile(privatePemPath, keys.privateKeyPem);
    writeTextFile(publicDerB64Path, keys.publicKeyDerBase64);
}

BedrockClientKeyPair BedrockAuthJwt::loadOrCreateKeyPair(
    const std::filesystem::path& privatePemPath,
    const std::filesystem::path& publicDerB64Path
) {
    if (std::filesystem::exists(privatePemPath) && std::filesystem::exists(publicDerB64Path)) {
        BedrockClientKeyPair out;
        out.privateKeyPem = readTextFile(privatePemPath);
        out.publicKeyDerBase64 = readTextFile(publicDerB64Path);
        while (!out.publicKeyDerBase64.empty() &&
               (out.publicKeyDerBase64.back() == '\n' || out.publicKeyDerBase64.back() == '\r')) {
            out.publicKeyDerBase64.pop_back();
        }
        return out;
    }

    auto keys = generateP384KeyPair();
    saveKeyPair(privatePemPath, publicDerB64Path, keys);
    return keys;
}

std::string BedrockAuthJwt::signEs384Jwt(
    const std::string& privateKeyPem,
    const std::string& publicKeyDerBase64,
    const std::string& payloadJson
) {
    std::string headerJson =
        "{\"alg\":\"ES384\",\"x5u\":\"" + publicKeyDerBase64 + "\"}";

    std::string signingInput =
        base64Url(utf8(headerJson)) + "." + base64Url(utf8(payloadJson));

    BIO* bio = BIO_new_mem_buf(privateKeyPem.data(), static_cast<int>(privateKeyPem.size()));
    if (!bio) throw std::runtime_error("BIO_new_mem_buf failed");

    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    if (!pkey) throw std::runtime_error("PEM_read_bio_PrivateKey failed");

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }

    if (EVP_DigestSignInit(mdctx, nullptr, EVP_sha384(), nullptr, pkey) != 1) {
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("EVP_DigestSignInit failed");
    }

    if (EVP_DigestSignUpdate(mdctx, signingInput.data(), signingInput.size()) != 1) {
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("EVP_DigestSignUpdate failed");
    }

    std::size_t sigLen = 0;
    if (EVP_DigestSignFinal(mdctx, nullptr, &sigLen) != 1) {
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("EVP_DigestSignFinal length failed");
    }

    std::vector<uint8_t> sig(sigLen);
    if (EVP_DigestSignFinal(mdctx, sig.data(), &sigLen) != 1) {
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("EVP_DigestSignFinal failed");
    }

    sig.resize(sigLen);

    const unsigned char* sigPtr = sig.data();
    ECDSA_SIG* ecdsaSig = d2i_ECDSA_SIG(
        nullptr,
        &sigPtr,
        static_cast<long>(sig.size())
    );

    if (!ecdsaSig) {
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("d2i_ECDSA_SIG failed");
    }

    const BIGNUM* r = nullptr;
    const BIGNUM* ss = nullptr;
    ECDSA_SIG_get0(ecdsaSig, &r, &ss);

    std::vector<uint8_t> joseSig(96);

    if (BN_bn2binpad(r, joseSig.data(), 48) != 48) {
        ECDSA_SIG_free(ecdsaSig);
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("BN_bn2binpad r failed");
    }

    if (BN_bn2binpad(ss, joseSig.data() + 48, 48) != 48) {
        ECDSA_SIG_free(ecdsaSig);
        EVP_MD_CTX_free(mdctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("BN_bn2binpad s failed");
    }

    ECDSA_SIG_free(ecdsaSig);
    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);

    return signingInput + "." + base64Url(joseSig);
}

BedrockVerifiedJwt BedrockAuthJwt::verifyEs384Jwt(
    const std::string& jwt,
    const std::string& publicKeyDerBase64,
    int64_t clockTimestamp
) {
    const auto parts = splitJwtExact(jwt);
    if (parts[2].find_first_not_of(" \t\r\n") == std::string::npos) {
        throw std::runtime_error("jwt signature is required");
    }

    const auto headerBytes = BedrockKeyExchange::base64UrlDecode(parts[0]);
    const auto payloadBytes = BedrockKeyExchange::base64UrlDecode(parts[1]);
    const std::string headerJson(headerBytes.begin(), headerBytes.end());
    const std::string payloadJson(payloadBytes.begin(), payloadBytes.end());

    const auto header = ProtoDefJson::parse(headerJson);
    if (header.kind != ProtoDefValue::Kind::Object) {
        throw std::runtime_error("invalid token");
    }
    const auto* algorithm = header.get("alg");
    if (!algorithm ||
        algorithm->kind != ProtoDefValue::Kind::String ||
        algorithm->stringValue != "ES384") {
        throw std::runtime_error("invalid algorithm");
    }

    const auto payload = ProtoDefJson::parse(payloadJson);
    const auto joseSignature = BedrockKeyExchange::base64UrlDecode(parts[2]);
    verifyJoseEs384(parts[0] + "." + parts[1], joseSignature, publicKeyDerBase64);
    validateJwtTimes(payload, clockTimestamp);

    return BedrockVerifiedJwt{headerJson, payloadJson};
}

} // namespace bedrock
