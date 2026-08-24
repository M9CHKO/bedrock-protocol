#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/evp.h>

namespace bedrock {

// bedrock-protocol's util.uuidFrom() labels this UUID "random", but actually
// calls uuid-1345 v3 with the hard-coded RFC URL namespace (6ba7b811), not the
// DNS namespace (6ba7b810). Hash its bytes plus the raw UTF-8 name exactly.
inline std::string uuidFrom(std::string_view name) {
    static constexpr std::array<std::uint8_t, 16> urlNamespace = {
        0x6b, 0xa7, 0xb8, 0x11,
        0x9d, 0xad,
        0x11, 0xd1,
        0x80, 0xb4,
        0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8
    };

    struct EvpMdContextDeleter {
        void operator()(EVP_MD_CTX* context) const noexcept {
            EVP_MD_CTX_free(context);
        }
    };

    using EvpMdContext = std::unique_ptr<EVP_MD_CTX, EvpMdContextDeleter>;

    EvpMdContext context(EVP_MD_CTX_new());
    if (!context) {
        throw std::runtime_error("EVP_MD_CTX_new failed while creating UUID v3");
    }

    const EVP_MD* md5 = EVP_md5();
    if (md5 == nullptr) {
        throw std::runtime_error("EVP_md5 failed while creating UUID v3");
    }
    if (EVP_DigestInit_ex(context.get(), md5, nullptr) != 1) {
        throw std::runtime_error("EVP_DigestInit_ex failed while creating UUID v3");
    }
    if (EVP_DigestUpdate(
            context.get(),
            urlNamespace.data(),
            urlNamespace.size()
        ) != 1) {
        throw std::runtime_error(
            "EVP_DigestUpdate failed for the UUID v3 URL namespace"
        );
    }
    if (!name.empty() &&
        EVP_DigestUpdate(context.get(), name.data(), name.size()) != 1) {
        throw std::runtime_error(
            "EVP_DigestUpdate failed for the UUID v3 name"
        );
    }

    std::array<std::uint8_t, 16> uuidBytes{};
    unsigned int digestLength = 0;
    if (EVP_DigestFinal_ex(
            context.get(),
            uuidBytes.data(),
            &digestLength
        ) != 1) {
        throw std::runtime_error("EVP_DigestFinal_ex failed while creating UUID v3");
    }
    if (digestLength != uuidBytes.size()) {
        throw std::runtime_error("OpenSSL returned an invalid UUID v3 MD5 length");
    }

    uuidBytes[6] = static_cast<std::uint8_t>(
        (uuidBytes[6] & 0x0fU) | 0x30U
    );
    uuidBytes[8] = static_cast<std::uint8_t>(
        (uuidBytes[8] & 0x3fU) | 0x80U
    );

    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < uuidBytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result.push_back('-');
        }
        const std::uint8_t byte = uuidBytes[index];
        result.push_back(hex[(byte >> 4U) & 0x0fU]);
        result.push_back(hex[byte & 0x0fU]);
    }

    return result;
}

} // namespace bedrock
