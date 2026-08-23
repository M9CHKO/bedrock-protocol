#include <bedrock/auth/XboxProofKey.hpp>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::uint8_t> fromHex(std::string_view hex) {
    if ((hex.size() & 1U) != 0) throw std::runtime_error("odd hex fixture");
    auto digit = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
        throw std::runtime_error("invalid hex fixture");
    };
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>((digit(hex[i]) << 4) | digit(hex[i + 1])));
    }
    return out;
}

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

bool verifyP1363(
    std::string_view privatePem,
    std::span<const std::uint8_t> input,
    std::span<const std::uint8_t> signature
) {
    if (signature.size() != 64) return false;
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(privatePem.data(), static_cast<int>(privatePem.size())),
        BIO_free
    );
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr),
        EVP_PKEY_free
    );
    if (!key) throw std::runtime_error("fixture PEM did not parse");

    std::unique_ptr<BIGNUM, decltype(&BN_free)> r(
        BN_bin2bn(signature.data(), 32, nullptr),
        BN_free
    );
    std::unique_ptr<BIGNUM, decltype(&BN_free)> s(
        BN_bin2bn(signature.data() + 32, 32, nullptr),
        BN_free
    );
    std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> ecSignature(
        ECDSA_SIG_new(),
        ECDSA_SIG_free
    );
    if (!r || !s || !ecSignature ||
        ECDSA_SIG_set0(ecSignature.get(), r.release(), s.release()) != 1) {
        throw std::runtime_error("could not make DER signature");
    }

    const int derLength = i2d_ECDSA_SIG(ecSignature.get(), nullptr);
    if (derLength <= 0) throw std::runtime_error("could not size DER signature");
    std::vector<unsigned char> der(static_cast<std::size_t>(derLength));
    unsigned char* derCursor = der.data();
    if (i2d_ECDSA_SIG(ecSignature.get(), &derCursor) != derLength) {
        throw std::runtime_error("could not encode DER signature");
    }

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
        EVP_MD_CTX_new(),
        EVP_MD_CTX_free
    );
    if (!context ||
        EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1) {
        throw std::runtime_error("could not initialize fixture verification");
    }
    return EVP_DigestVerify(
        context.get(),
        der.data(),
        der.size(),
        input.data(),
        input.size()
    ) == 1;
}

} // namespace

int main() {
    try {
        // d=1 P-256 PKCS#8 key imported by Node through createPrivateKey(JWK).
        // The PEM, JWK, preimage, and signatures below were emitted directly by
        // prismarine-auth@2.7.0 XboxTokenManager.sign.
        constexpr std::string_view privatePem =
            "-----BEGIN PRIVATE KEY-----\n"
            "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgAAAAAAAAAAAAAAAA\n"
            "AAAAAAAAAAAAAAAAAAAAAAAAAAGhRANCAARrF9Hy4SxCR/i85uVjpEDydwN9gS3r\n"
            "M6D0oTlF2JjClk/jQuL+Gn+bjufrSnwPnhYrzjNXazFezsu2QGg3v1H1\n"
            "-----END PRIVATE KEY-----\n";
        constexpr std::int64_t nowMs = 1700000000123LL;
        constexpr std::uint64_t windowsTimestamp = 133444736000000000ULL;
        constexpr std::string_view url =
            "https://user.auth.xboxlive.com/user/authenticate?included=no#frag";
        constexpr std::string_view authorization = "XBL3.0 x=abc;token";
        constexpr std::string_view payload = "{\"snowman\":\"\xE2\x98\x83\",\"n\":1}";

        const auto proofKey = bedrock::XboxProofKey::fromPrivateKeyPem(privatePem);
        const auto jwk = proofKey.jwk();
        require(jwk.kty == "EC", "JWK kty mismatch");
        require(jwk.x == "axfR8uEsQkf4vOblY6RA8ncDfYEt6zOg9KE5RdiYwpY", "JWK x mismatch");
        require(jwk.y == "T-NC4v4af5uO5-tKfA-eFivOM1drMV7Oy7ZAaDe_UfU", "JWK y mismatch");
        require(jwk.crv == "P-256", "JWK crv mismatch");
        require(jwk.alg == "ES256", "JWK alg mismatch");
        require(jwk.use == "sig", "JWK use mismatch");
        require(
            proofKey.jwkJson() ==
                "{\"kty\":\"EC\",\"x\":\"axfR8uEsQkf4vOblY6RA8ncDfYEt6zOg9KE5RdiYwpY\","
                "\"y\":\"T-NC4v4af5uO5-tKfA-eFivOM1drMV7Oy7ZAaDe_UfU\","
                "\"crv\":\"P-256\",\"alg\":\"ES256\",\"use\":\"sig\"}",
            "JWK JSON mismatch"
        );

        require(
            bedrock::XboxProofKey::windowsTimestampFromUnixMilliseconds(nowMs) ==
                windowsTimestamp,
            "Windows FILETIME mismatch"
        );
        require(
            bedrock::XboxProofKey::windowsTimestampFromUnixMilliseconds(-1) ==
                116444736000000000ULL,
            "JavaScript negative fractional second truncation mismatch"
        );
        require(
            bedrock::XboxProofKey::windowsTimestampFromUnixMilliseconds(2147483648000LL) ==
                94969899520000000ULL,
            "JavaScript signed Int32 timestamp wrap mismatch"
        );
        require(
            bedrock::XboxProofKey::windowsTimestampFromUnixMilliseconds(4294967296000LL) ==
                116444736000000000ULL,
            "JavaScript Uint32-period timestamp wrap mismatch"
        );

        const auto expectedPreimage = fromHex(
            "000000010001da1747c66d000000504f535400"
            "2f757365722f61757468656e74696361746500"
            "58424c332e3020783d6162633b746f6b656e00"
            "7b22736e6f776d616e223a22e29883222c226e223a317d00"
        );
        const auto preimage = bedrock::XboxProofKey::signingInputAt(
            url,
            authorization,
            payload,
            nowMs
        );
        require(preimage == expectedPreimage, "Xbox signing preimage mismatch");

        struct EndpointFixture {
            std::string_view url;
            std::string_view preimageHex;
        };
        constexpr std::array endpointFixtures {
            EndpointFixture {
                "https://user.auth.xboxlive.com/user/authenticate",
                "000000010001da1747c66d000000504f5354002f757365722f61757468656e746963617465000000"
            },
            EndpointFixture {
                "https://sisu.xboxlive.com/authorize",
                "000000010001da1747c66d000000504f5354002f617574686f72697a65000000"
            },
            EndpointFixture {
                "https://xsts.auth.xboxlive.com/xsts/authorize",
                "000000010001da1747c66d000000504f5354002f787374732f617574686f72697a65000000"
            },
            EndpointFixture {
                "https://device.auth.xboxlive.com/device/authenticate",
                "000000010001da1747c66d000000504f5354002f6465766963652f61757468656e746963617465000000"
            },
            EndpointFixture {
                "https://title.auth.xboxlive.com/title/authenticate",
                "000000010001da1747c66d000000504f5354002f7469746c652f61757468656e746963617465000000"
            }
        };
        for (const auto& fixture : endpointFixtures) {
            require(
                bedrock::XboxProofKey::signingInputAt(
                    fixture.url,
                    "",
                    "",
                    nowMs
                ) == fromHex(fixture.preimageHex),
                "authoritative Xbox endpoint preimage mismatch"
            );
        }

        // Oracle call replaced only crypto.sign with bytes 00..3f. Thus this
        // checks XboxTokenManager's complete returned Buffer byte-for-byte while
        // avoiding the random ECDSA nonce.
        std::array<std::uint8_t, 64> deterministicSignature {};
        for (std::size_t i = 0; i < deterministicSignature.size(); ++i) {
            deterministicSignature[i] = static_cast<std::uint8_t>(i);
        }
        const auto expectedFramed = fromHex(
            "0000000101da1747c66d0000"
            "000102030405060708090a0b0c0d0e0f"
            "101112131415161718191a1b1c1d1e1f"
            "202122232425262728292a2b2c2d2e2f"
            "303132333435363738393a3b3c3d3e3f"
        );
        require(
            bedrock::XboxProofKey::frameSignature(
                windowsTimestamp,
                deterministicSignature
            ) == expectedFramed,
            "Xbox signature header framing mismatch"
        );

        // This non-deterministic signature was emitted once by the unmodified
        // Node crypto.sign path. Verify its exact P1363 bytes against the exact
        // JS preimage as a cross-runtime fixture.
        const auto jsSignature = fromHex(
            "0310d5ba77f5f1081a02aa2b34f8312b39bb9e25cd3e14a4b83cfb6e786e7f93"
            "64cfdf4518d5db76df39cc172957d0565cc01ee52639abf2cc55649292c194cb"
        );
        require(
            verifyP1363(privatePem, expectedPreimage, jsSignature),
            "authoritative JS P1363 signature did not verify"
        );

        const auto signedHeader = proofKey.signAt(url, authorization, payload, nowMs);
        require(signedHeader.size() == 76, "signed header must be 76 bytes");
        require(
            std::equal(
                signedHeader.begin(),
                signedHeader.begin() + 12,
                expectedFramed.begin()
            ),
            "signed header prefix mismatch"
        );
        require(
            verifyP1363(
                privatePem,
                expectedPreimage,
                std::span<const std::uint8_t>(signedHeader).subspan(12)
            ),
            "C++ P1363 signature did not verify"
        );

        const auto generated = bedrock::XboxProofKey::generate();
        const auto generatedJwk = generated.jwk();
        require(generatedJwk.x.size() == 43, "generated JWK x width mismatch");
        require(generatedJwk.y.size() == 43, "generated JWK y width mismatch");
        require(generatedJwk.x.find('=') == std::string::npos, "generated JWK x padding");
        require(generatedJwk.y.find('=') == std::string::npos, "generated JWK y padding");

        bool rejectedShortSignature = false;
        try {
            const std::array<std::uint8_t, 63> shortSignature {};
            (void)bedrock::XboxProofKey::frameSignature(
                windowsTimestamp,
                shortSignature
            );
        } catch (const std::invalid_argument&) {
            rejectedShortSignature = true;
        }
        require(rejectedShortSignature, "short P1363 signature was accepted");

        bool rejectedUnknownEndpoint = false;
        try {
            (void)bedrock::XboxProofKey::signingInputAt(
                "https://example.test/a/../b",
                "",
                "",
                nowMs
            );
        } catch (const std::invalid_argument&) {
            rejectedUnknownEndpoint = true;
        }
        require(rejectedUnknownEndpoint, "unknown non-WHATWG endpoint was accepted");

        std::cout << "xbox proof key smoke: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "xbox proof key smoke: " << error.what() << '\n';
        return 1;
    }
}
