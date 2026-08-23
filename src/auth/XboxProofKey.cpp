#include <bedrock/auth/XboxProofKey.hpp>

#include <bedrock/auth/BedrockAuthJwt.hpp>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>

#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace bedrock {
namespace {

constexpr std::uint32_t kPolicyVersion = 1;
constexpr std::int64_t kWindowsEpochSeconds = 11644473600LL;
constexpr std::uint64_t kFiletimeTicksPerSecond = 10000000ULL;

struct BioDeleter {
    void operator()(BIO* bio) const noexcept { BIO_free(bio); }
};

struct EvpPkeyDeleter {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};

struct EvpPkeyCtxDeleter {
    void operator()(EVP_PKEY_CTX* ctx) const noexcept { EVP_PKEY_CTX_free(ctx); }
};

struct EvpMdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const noexcept { EVP_MD_CTX_free(ctx); }
};

struct EcKeyDeleter {
    void operator()(EC_KEY* key) const noexcept { EC_KEY_free(key); }
};

struct BnCtxDeleter {
    void operator()(BN_CTX* ctx) const noexcept { BN_CTX_free(ctx); }
};

struct BnDeleter {
    void operator()(BIGNUM* value) const noexcept { BN_free(value); }
};

struct EcdsaSigDeleter {
    void operator()(ECDSA_SIG* sig) const noexcept { ECDSA_SIG_free(sig); }
};

using UniqueBio = std::unique_ptr<BIO, BioDeleter>;
using UniqueEvpPkey = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using UniqueEvpPkeyCtx = std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter>;
using UniqueEvpMdCtx = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;
using UniqueEcKey = std::unique_ptr<EC_KEY, EcKeyDeleter>;
using UniqueBnCtx = std::unique_ptr<BN_CTX, BnCtxDeleter>;
using UniqueBn = std::unique_ptr<BIGNUM, BnDeleter>;
using UniqueEcdsaSig = std::unique_ptr<ECDSA_SIG, EcdsaSigDeleter>;

void appendU32BE(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

void appendU64BE(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
}

void appendNullTerminated(
    std::vector<std::uint8_t>& out,
    std::string_view value
) {
    out.insert(out.end(), value.begin(), value.end());
    out.push_back(0);
}

std::string pathnameFromXboxEndpoint(std::string_view url) {
    // XboxTokenManager passes absolute HTTPS endpoint constants to WHATWG URL
    // and then reads .pathname. This helper is deliberately endpoint-scoped,
    // not a claim to implement the complete WHATWG URL algorithm.
    constexpr std::string_view prefix = "https://";
    if (!url.starts_with(prefix)) {
        throw std::invalid_argument("Xbox proof signing URL must be absolute HTTPS");
    }

    const std::size_t authority = prefix.size();
    const std::size_t pathStart = url.find_first_of("/?#", authority);
    const std::size_t authorityEnd = pathStart == std::string_view::npos
        ? url.size()
        : pathStart;
    if (authorityEnd == authority) {
        throw std::invalid_argument("Xbox proof signing URL has no authority");
    }
    for (char c : url) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x21 || byte > 0x7e || c == '\\') {
            throw std::invalid_argument("Xbox proof signing URL must be ASCII HTTPS");
        }
    }

    const std::size_t suffix = url.find_first_of("?#");
    const std::string_view endpoint = url.substr(0, suffix);
    constexpr std::array<std::string_view, 5> endpoints {
        "https://device.auth.xboxlive.com/device/authenticate",
        "https://title.auth.xboxlive.com/title/authenticate",
        "https://user.auth.xboxlive.com/user/authenticate",
        "https://sisu.xboxlive.com/authorize",
        "https://xsts.auth.xboxlive.com/xsts/authorize"
    };
    bool knownEndpoint = false;
    for (const std::string_view candidate : endpoints) {
        if (endpoint == candidate) {
            knownEndpoint = true;
            break;
        }
    }
    if (!knownEndpoint) {
        throw std::invalid_argument("unsupported Xbox proof signing endpoint");
    }

    if (pathStart == std::string_view::npos || url[pathStart] != '/') {
        return "/";
    }

    const std::size_t pathEnd = url.find_first_of("?#", pathStart);
    if (pathEnd == std::string_view::npos) {
        return std::string(url.substr(pathStart));
    }
    return std::string(url.substr(pathStart, pathEnd - pathStart));
}

UniqueEcKey checkedP256EcKey(EVP_PKEY* key, bool requirePrivate) {
    if (!key || EVP_PKEY_base_id(key) != EVP_PKEY_EC) {
        throw std::invalid_argument("Xbox proof key must be an EC private key");
    }

    UniqueEcKey ec(EVP_PKEY_get1_EC_KEY(key));
    if (!ec) {
        throw std::runtime_error("EVP_PKEY_get1_EC_KEY failed");
    }
    const EC_GROUP* group = EC_KEY_get0_group(ec.get());
    if (!group || EC_GROUP_get_curve_name(group) != NID_X9_62_prime256v1) {
        throw std::invalid_argument("Xbox proof key curve must be P-256");
    }
    if (requirePrivate && !EC_KEY_get0_private_key(ec.get())) {
        throw std::invalid_argument("Xbox proof key must contain private key material");
    }
    return ec;
}

std::array<std::uint8_t, 32> affineCoordinate(
    const EC_GROUP* group,
    const EC_POINT* point,
    bool xCoordinate
) {
    UniqueBnCtx ctx(BN_CTX_new());
    UniqueBn x(BN_new());
    UniqueBn y(BN_new());
    if (!ctx || !x || !y) {
        throw std::runtime_error("P-256 coordinate allocation failed");
    }
    if (EC_POINT_get_affine_coordinates(group, point, x.get(), y.get(), ctx.get()) != 1) {
        throw std::runtime_error("P-256 public coordinate extraction failed");
    }

    std::array<std::uint8_t, 32> out {};
    const BIGNUM* value = xCoordinate ? x.get() : y.get();
    if (BN_bn2binpad(value, out.data(), static_cast<int>(out.size())) !=
        static_cast<int>(out.size())) {
        throw std::runtime_error("P-256 public coordinate has invalid width");
    }
    return out;
}

XboxProofKey::P1363Signature signP1363(
    EVP_PKEY* key,
    std::span<const std::uint8_t> input
) {
    UniqueEvpMdCtx context(EVP_MD_CTX_new());
    if (!context) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }
    if (EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key) != 1) {
        throw std::runtime_error("ES256 signing initialization failed");
    }

    std::size_t derSize = 0;
    if (EVP_DigestSign(
            context.get(),
            nullptr,
            &derSize,
            input.data(),
            input.size()
        ) != 1) {
        throw std::runtime_error("ES256 signature sizing failed");
    }

    std::vector<unsigned char> der(derSize);
    if (EVP_DigestSign(
            context.get(),
            der.data(),
            &derSize,
            input.data(),
            input.size()
        ) != 1) {
        throw std::runtime_error("ES256 signing failed");
    }
    der.resize(derSize);

    const unsigned char* cursor = der.data();
    UniqueEcdsaSig decoded(d2i_ECDSA_SIG(
        nullptr,
        &cursor,
        static_cast<long>(der.size())
    ));
    if (!decoded || cursor != der.data() + der.size()) {
        throw std::runtime_error("OpenSSL returned an invalid ECDSA signature");
    }

    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(decoded.get(), &r, &s);
    XboxProofKey::P1363Signature p1363 {};
    if (!r || !s ||
        BN_bn2binpad(r, p1363.data(), 32) != 32 ||
        BN_bn2binpad(s, p1363.data() + 32, 32) != 32) {
        throw std::runtime_error("ES256 signature components have invalid width");
    }
    return p1363;
}

} // namespace

struct XboxProofKey::Impl {
    explicit Impl(UniqueEvpPkey value) : key(std::move(value)) {}
    UniqueEvpPkey key;
};

std::string XboxProofKeyJwk::toJson() const {
    // All values originate from fixed identifiers or base64url and therefore do
    // not require JSON escaping. The property order is Node's JWK export order,
    // followed by XboxTokenManager's appended alg/use properties.
    return "{\"kty\":\"" + kty + "\",\"x\":\"" + x +
        "\",\"y\":\"" + y + "\",\"crv\":\"" + crv +
        "\",\"alg\":\"" + alg + "\",\"use\":\"" + use + "\"}";
}

XboxProofKey::XboxProofKey(std::shared_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

XboxProofKey XboxProofKey::generate() {
    UniqueEvpPkeyCtx context(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(
            context.get(),
            NID_X9_62_prime256v1
        ) != 1) {
        throw std::runtime_error("P-256 key generation initialization failed");
    }

    EVP_PKEY* rawKey = nullptr;
    if (EVP_PKEY_keygen(context.get(), &rawKey) != 1 || !rawKey) {
        throw std::runtime_error("P-256 key generation failed");
    }
    UniqueEvpPkey key(rawKey);
    (void)checkedP256EcKey(key.get(), true);
    return XboxProofKey(std::make_shared<Impl>(std::move(key)));
}

XboxProofKey XboxProofKey::fromPrivateKeyPem(std::string_view privateKeyPem) {
    if (privateKeyPem.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("Xbox proof key PEM is too large");
    }
    UniqueBio bio(BIO_new_mem_buf(
        privateKeyPem.data(),
        static_cast<int>(privateKeyPem.size())
    ));
    if (!bio) {
        throw std::runtime_error("BIO_new_mem_buf failed");
    }

    UniqueEvpPkey key(PEM_read_bio_PrivateKey(
        bio.get(),
        nullptr,
        nullptr,
        nullptr
    ));
    if (!key) {
        throw std::invalid_argument("invalid Xbox proof key PEM");
    }
    (void)checkedP256EcKey(key.get(), true);
    return XboxProofKey(std::make_shared<Impl>(std::move(key)));
}

XboxProofKeyJwk XboxProofKey::jwk() const {
    if (!impl_ || !impl_->key) {
        throw std::logic_error("Xbox proof key is empty");
    }
    UniqueEcKey ec = checkedP256EcKey(impl_->key.get(), true);
    const EC_GROUP* group = EC_KEY_get0_group(ec.get());
    const EC_POINT* point = EC_KEY_get0_public_key(ec.get());
    if (!point) {
        throw std::runtime_error("Xbox proof key has no public key");
    }

    const auto xBytes = affineCoordinate(group, point, true);
    const auto yBytes = affineCoordinate(group, point, false);
    XboxProofKeyJwk result;
    result.x = BedrockAuthJwt::base64Url(
        std::vector<std::uint8_t>(xBytes.begin(), xBytes.end())
    );
    result.y = BedrockAuthJwt::base64Url(
        std::vector<std::uint8_t>(yBytes.begin(), yBytes.end())
    );
    return result;
}

std::string XboxProofKey::jwkJson() const {
    return jwk().toJson();
}

std::vector<std::uint8_t> XboxProofKey::sign(
    std::string_view url,
    std::string_view authorizationToken,
    std::string_view payload
) const {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    return signAt(url, authorizationToken, payload, now);
}

std::vector<std::uint8_t> XboxProofKey::signAt(
    std::string_view url,
    std::string_view authorizationToken,
    std::string_view payload,
    std::int64_t unixTimeMilliseconds
) const {
    if (!impl_ || !impl_->key) {
        throw std::logic_error("Xbox proof key is empty");
    }
    const auto input = signingInputAt(
        url,
        authorizationToken,
        payload,
        unixTimeMilliseconds
    );
    const auto signature = signP1363(impl_->key.get(), input);
    return frameSignature(
        windowsTimestampFromUnixMilliseconds(unixTimeMilliseconds),
        signature
    );
}

std::uint64_t XboxProofKey::windowsTimestampFromUnixMilliseconds(
    std::int64_t unixTimeMilliseconds
) {
    // Match (Date.now() / 1000) | 0, including JavaScript's signed Int32 wrap.
    const std::int64_t wholeSeconds = unixTimeMilliseconds / 1000;
    const std::uint32_t low32 = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(wholeSeconds) & 0xffffffffULL
    );
    const std::int64_t javascriptSeconds = low32 < 0x80000000U
        ? static_cast<std::int64_t>(low32)
        : static_cast<std::int64_t>(low32) - 0x100000000LL;
    return static_cast<std::uint64_t>(javascriptSeconds + kWindowsEpochSeconds) *
        kFiletimeTicksPerSecond;
}

std::vector<std::uint8_t> XboxProofKey::signingInputAt(
    std::string_view url,
    std::string_view authorizationToken,
    std::string_view payload,
    std::int64_t unixTimeMilliseconds
) {
    const std::string pathname = pathnameFromXboxEndpoint(url);
    std::vector<std::uint8_t> input;
    appendU32BE(input, kPolicyVersion);
    input.push_back(0);
    appendU64BE(
        input,
        windowsTimestampFromUnixMilliseconds(unixTimeMilliseconds)
    );
    input.push_back(0);
    appendNullTerminated(input, "POST");
    appendNullTerminated(input, pathname);
    appendNullTerminated(input, authorizationToken);
    appendNullTerminated(input, payload);
    return input;
}

std::vector<std::uint8_t> XboxProofKey::frameSignature(
    std::uint64_t windowsTimestamp,
    std::span<const std::uint8_t> signature
) {
    if (signature.size() != P1363Signature {}.size()) {
        throw std::invalid_argument("ES256 IEEE-P1363 signature must be 64 bytes");
    }
    std::vector<std::uint8_t> header;
    header.reserve(12 + signature.size());
    appendU32BE(header, kPolicyVersion);
    appendU64BE(header, windowsTimestamp);
    header.insert(header.end(), signature.begin(), signature.end());
    return header;
}

} // namespace bedrock
