#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bedrock {

// Public portion of the P-256 key exported by Node.js as a JWK and extended by
// prismarine-auth's XboxTokenManager with the ES256 algorithm and signing use.
struct XboxProofKeyJwk {
    std::string kty = "EC";
    std::string x;
    std::string y;
    std::string crv = "P-256";
    std::string alg = "ES256";
    std::string use = "sig";

    std::string toJson() const;
};

// The request proof key used by prismarine-auth's XboxTokenManager. A generated
// key lives for the lifetime of the object, matching the key pair generated once
// by each MicrosoftAuthFlow instance.
class XboxProofKey {
public:
    using P1363Signature = std::array<std::uint8_t, 64>;

    static XboxProofKey generate();
    static XboxProofKey fromPrivateKeyPem(std::string_view privateKeyPem);

    XboxProofKey(const XboxProofKey&) noexcept = default;
    XboxProofKey& operator=(const XboxProofKey&) noexcept = default;
    XboxProofKey(XboxProofKey&&) noexcept = default;
    XboxProofKey& operator=(XboxProofKey&&) noexcept = default;
    ~XboxProofKey() = default;

    XboxProofKeyJwk jwk() const;
    std::string jwkJson() const;

    // Equivalent to XboxTokenManager.sign(url, authorizationToken, payload) for
    // the five absolute HTTPS endpoints used by prismarine-auth's Endpoints.
    // As in the JS implementation, URL search and hash components are omitted.
    // The returned bytes are the 4-byte policy version, 8-byte Windows
    // timestamp, and the 64-byte IEEE-P1363 ES256 signature.
    std::vector<std::uint8_t> sign(
        std::string_view url,
        std::string_view authorizationToken,
        std::string_view payload
    ) const;

    // Deterministic clock seam corresponding to a fixed JavaScript Date.now().
    std::vector<std::uint8_t> signAt(
        std::string_view url,
        std::string_view authorizationToken,
        std::string_view payload,
        std::int64_t unixTimeMilliseconds
    ) const;

    // Exposed byte-level helpers keep the protocol framing independently
    // testable even though ECDSA itself intentionally uses a random nonce.
    static std::uint64_t windowsTimestampFromUnixMilliseconds(
        std::int64_t unixTimeMilliseconds
    );
    static std::vector<std::uint8_t> signingInputAt(
        std::string_view url,
        std::string_view authorizationToken,
        std::string_view payload,
        std::int64_t unixTimeMilliseconds
    );
    static std::vector<std::uint8_t> frameSignature(
        std::uint64_t windowsTimestamp,
        std::span<const std::uint8_t> signature
    );

private:
    struct Impl;
    explicit XboxProofKey(std::shared_ptr<Impl> impl) noexcept;

    std::shared_ptr<Impl> impl_;
};

} // namespace bedrock
