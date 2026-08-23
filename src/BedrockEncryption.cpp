#include "bedrock/BedrockEncryption.hpp"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <cstddef>
#include <cstring>

namespace bedrock {

namespace {

std::string lowerHex(const std::vector<uint8_t>& bytes) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t byte : bytes) {
        out.push_back(HEX[(byte >> 4) & 0x0f]);
        out.push_back(HEX[byte & 0x0f]);
    }
    return out;
}

std::size_t nodeChecksumSliceBoundary(std::size_t size) noexcept {
    if (size >= 8) {
        return size - 8;
    }

    // Buffer#slice normalizes a negative index by adding buffer.length and
    // then clamps it to zero. Both JS slices use `chunk.length - 8`, so for
    // lengths 0..7 the normalized boundary is max(2 * length - 8, 0).
    return size > 4 ? (2 * size) - 8 : 0;
}

} // namespace

bool BedrockChecksumVerification::matches() const noexcept {
    return receivedChecksum == expectedChecksum;
}

std::string BedrockChecksumVerification::mismatchMessage() const {
    return "Checksum mismatch " + lowerHex(receivedChecksum) +
        " != " + lowerHex(expectedChecksum);
}

BedrockAesGcmStream::BedrockAesGcmStream(
    const std::vector<uint8_t>& secretKeyBytes,
    const std::vector<uint8_t>& iv16,
    Mode mode
) : mode_(mode) {
    if (secretKeyBytes.size() != 32) {
        throw BedrockEncryptionError("BedrockAesGcmStream secretKeyBytes must be 32 bytes");
    }

    if (iv16.size() < 12) {
        throw BedrockEncryptionError("BedrockAesGcmStream iv16 must contain at least 12 bytes");
    }

    std::vector<uint8_t> iv12(
        iv16.begin(),
        iv16.begin() + 12
    );

    ctx_ = EVP_CIPHER_CTX_new();

    if (!ctx_) {
        throw BedrockEncryptionError("EVP_CIPHER_CTX_new stream failed");
    }

    if (mode_ == Mode::Encrypt) {
        if (EVP_EncryptInit_ex(ctx_, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            EVP_CIPHER_CTX_free(ctx_);
            ctx_ = nullptr;
            throw BedrockEncryptionError("EVP_EncryptInit_ex stream cipher failed");
        }

        if (EVP_CIPHER_CTX_ctrl(ctx_, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv12.size()), nullptr) != 1) {
            EVP_CIPHER_CTX_free(ctx_);
            ctx_ = nullptr;
            throw BedrockEncryptionError("EVP_CTRL_GCM_SET_IVLEN encrypt stream failed");
        }

        if (EVP_EncryptInit_ex(ctx_, nullptr, nullptr, secretKeyBytes.data(), iv12.data()) != 1) {
            EVP_CIPHER_CTX_free(ctx_);
            ctx_ = nullptr;
            throw BedrockEncryptionError("EVP_EncryptInit_ex stream key/iv failed");
        }
    } else {
        if (EVP_DecryptInit_ex(ctx_, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            EVP_CIPHER_CTX_free(ctx_);
            ctx_ = nullptr;
            throw BedrockEncryptionError("EVP_DecryptInit_ex stream cipher failed");
        }

        if (EVP_CIPHER_CTX_ctrl(ctx_, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv12.size()), nullptr) != 1) {
            EVP_CIPHER_CTX_free(ctx_);
            ctx_ = nullptr;
            throw BedrockEncryptionError("EVP_CTRL_GCM_SET_IVLEN decrypt stream failed");
        }

        if (EVP_DecryptInit_ex(ctx_, nullptr, nullptr, secretKeyBytes.data(), iv12.data()) != 1) {
            EVP_CIPHER_CTX_free(ctx_);
            ctx_ = nullptr;
            throw BedrockEncryptionError("EVP_DecryptInit_ex decrypt stream key/iv failed");
        }
    }
}

BedrockAesGcmStream::~BedrockAesGcmStream() {
    if (ctx_) {
        EVP_CIPHER_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

BedrockCipherAlgorithm BedrockAesGcmStream::algorithm() const noexcept {
    return BedrockCipherAlgorithm::Aes256GcmNoTag;
}

std::vector<uint8_t> BedrockAesGcmStream::process(
    const std::vector<uint8_t>& input
) {
    if (!ctx_) {
        throw BedrockEncryptionError("BedrockAesGcmStream context is null");
    }

    std::vector<uint8_t> out;
    out.resize(input.size() + 16);

    int len = 0;

    if (mode_ == Mode::Encrypt) {
        if (!input.empty()) {
            if (EVP_EncryptUpdate(
                ctx_,
                out.data(),
                &len,
                input.data(),
                static_cast<int>(input.size())
            ) != 1) {
                throw BedrockEncryptionError("EVP_EncryptUpdate stream failed");
            }
        }
    } else {
        if (!input.empty()) {
            if (EVP_DecryptUpdate(
                ctx_,
                out.data(),
                &len,
                input.data(),
                static_cast<int>(input.size())
            ) != 1) {
                throw BedrockEncryptionError("EVP_DecryptUpdate stream failed");
            }
        }
    }

    out.resize(static_cast<size_t>(len));
    return out;
}

BedrockAesCfb8Stream::BedrockAesCfb8Stream(
    const std::vector<uint8_t>& secretKeyBytes,
    const std::vector<uint8_t>& iv16,
    Mode mode
) : mode_(mode) {
    if (secretKeyBytes.size() != 32) {
        throw BedrockEncryptionError("BedrockAesCfb8Stream secretKeyBytes must be 32 bytes");
    }

    if (iv16.size() != 16) {
        throw BedrockEncryptionError("BedrockAesCfb8Stream iv16 must be 16 bytes");
    }

    ctx_ = EVP_CIPHER_CTX_new();
    if (!ctx_) {
        throw BedrockEncryptionError("EVP_CIPHER_CTX_new CFB8 stream failed");
    }

    const int initialized = mode_ == Mode::Encrypt
        ? EVP_EncryptInit_ex(
            ctx_,
            EVP_aes_256_cfb8(),
            nullptr,
            secretKeyBytes.data(),
            iv16.data()
        )
        : EVP_DecryptInit_ex(
            ctx_,
            EVP_aes_256_cfb8(),
            nullptr,
            secretKeyBytes.data(),
            iv16.data()
        );

    if (initialized != 1) {
        EVP_CIPHER_CTX_free(ctx_);
        ctx_ = nullptr;
        throw BedrockEncryptionError("EVP CFB8 stream initialization failed");
    }

    // CFB8 is a byte-stream mode. Keep the explicit no-padding setting to
    // match crypto.createCipheriv('aes-256-cfb8')/NoPadding semantics.
    if (EVP_CIPHER_CTX_set_padding(ctx_, 0) != 1) {
        EVP_CIPHER_CTX_free(ctx_);
        ctx_ = nullptr;
        throw BedrockEncryptionError("EVP CFB8 stream no-padding setup failed");
    }
}

BedrockAesCfb8Stream::~BedrockAesCfb8Stream() {
    if (ctx_) {
        EVP_CIPHER_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

BedrockCipherAlgorithm BedrockAesCfb8Stream::algorithm() const noexcept {
    return BedrockCipherAlgorithm::Aes256Cfb8;
}

std::vector<uint8_t> BedrockAesCfb8Stream::process(
    const std::vector<uint8_t>& input
) {
    if (!ctx_) {
        throw BedrockEncryptionError("BedrockAesCfb8Stream context is null");
    }

    if (input.empty()) {
        return {};
    }

    std::vector<uint8_t> out(input.size() + 16);
    int len = 0;
    const int processed = mode_ == Mode::Encrypt
        ? EVP_EncryptUpdate(
            ctx_,
            out.data(),
            &len,
            input.data(),
            static_cast<int>(input.size())
        )
        : EVP_DecryptUpdate(
            ctx_,
            out.data(),
            &len,
            input.data(),
            static_cast<int>(input.size())
        );

    if (processed != 1) {
        throw BedrockEncryptionError("EVP CFB8 stream update failed");
    }

    out.resize(static_cast<size_t>(len));
    return out;
}

BedrockCipherAlgorithm BedrockEncryption::cipherAlgorithmForProtocol(
    uint32_t protocolVersion
) noexcept {
    return protocolVersion < GCM_PROTOCOL_VERSION
        ? BedrockCipherAlgorithm::Aes256Cfb8
        : BedrockCipherAlgorithm::Aes256GcmNoTag;
}

std::unique_ptr<BedrockCipherStream> BedrockEncryption::createCipherStream(
    uint32_t protocolVersion,
    const std::vector<uint8_t>& secretKeyBytes,
    const std::vector<uint8_t>& iv16,
    BedrockCipherMode mode
) {
    if (cipherAlgorithmForProtocol(protocolVersion) ==
        BedrockCipherAlgorithm::Aes256Cfb8) {
        return std::make_unique<BedrockAesCfb8Stream>(
            secretKeyBytes,
            iv16,
            mode
        );
    }

    return std::make_unique<BedrockAesGcmStream>(
        secretKeyBytes,
        iv16,
        mode
    );
}


void BedrockEncryption::writeU64LE(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
    }
}

std::vector<uint8_t> BedrockEncryption::computeChecksum(
    const std::vector<uint8_t>& packetPlaintext,
    uint64_t sendCounter,
    const std::vector<uint8_t>& secretKeyBytes
) {
    if (secretKeyBytes.size() != 32) {
        throw BedrockEncryptionError("secretKeyBytes must be 32 bytes");
    }

    std::vector<uint8_t> counter;
    writeU64LE(counter, sendCounter);

    SHA256_CTX ctx;

    if (SHA256_Init(&ctx) != 1) {
        throw BedrockEncryptionError("SHA256_Init failed");
    }

    if (SHA256_Update(&ctx, counter.data(), counter.size()) != 1) {
        throw BedrockEncryptionError("SHA256_Update counter failed");
    }

    if (!packetPlaintext.empty()) {
        if (SHA256_Update(&ctx, packetPlaintext.data(), packetPlaintext.size()) != 1) {
            throw BedrockEncryptionError("SHA256_Update packet failed");
        }
    }

    if (SHA256_Update(&ctx, secretKeyBytes.data(), secretKeyBytes.size()) != 1) {
        throw BedrockEncryptionError("SHA256_Update secret failed");
    }

    uint8_t hash[SHA256_DIGEST_LENGTH];

    if (SHA256_Final(hash, &ctx) != 1) {
        throw BedrockEncryptionError("SHA256_Final failed");
    }

    return std::vector<uint8_t>(hash, hash + 8);
}

std::vector<uint8_t> BedrockEncryption::makeAesPlaintext(
    const std::vector<uint8_t>& packetPlaintext,
    uint64_t sendCounter,
    const std::vector<uint8_t>& secretKeyBytes
) {
    auto check = computeChecksum(
        packetPlaintext,
        sendCounter,
        secretKeyBytes
    );

    std::vector<uint8_t> out;
    out.reserve(packetPlaintext.size() + check.size());

    out.insert(out.end(), packetPlaintext.begin(), packetPlaintext.end());
    out.insert(out.end(), check.begin(), check.end());

    return out;
}

BedrockChecksumVerification BedrockEncryption::verifyAesPlaintext(
    const std::vector<uint8_t>& aesPlaintext,
    uint64_t& receiveCounter,
    const std::vector<uint8_t>& secretKeyBytes
) {
    const auto boundary = nodeChecksumSliceBoundary(aesPlaintext.size());

    BedrockChecksumVerification result;
    result.packetPlaintext.assign(
        aesPlaintext.begin(),
        aesPlaintext.begin() + static_cast<std::ptrdiff_t>(boundary)
    );
    result.receivedChecksum.assign(
        aesPlaintext.begin() + static_cast<std::ptrdiff_t>(boundary),
        aesPlaintext.end()
    );
    result.expectedChecksum = computeChecksum(
        result.packetPlaintext,
        receiveCounter,
        secretKeyBytes
    );
    ++receiveCounter;
    return result;
}

std::optional<BedrockChecksumVerification> BedrockEncryption::decryptAndVerify(
    BedrockCipherStream& decryptStream,
    const std::vector<uint8_t>& encrypted,
    uint64_t& receiveCounter,
    const std::vector<uint8_t>& secretKeyBytes
) {
    if (encrypted.empty()) {
        return std::nullopt;
    }

    return verifyAesPlaintext(
        decryptStream.process(encrypted),
        receiveCounter,
        secretKeyBytes
    );
}

std::vector<uint8_t> BedrockEncryption::aes256GcmEncryptNoTag(
    const std::vector<uint8_t>& secretKeyBytes,
    const std::vector<uint8_t>& iv12,
    const std::vector<uint8_t>& plaintext
) {
    if (secretKeyBytes.size() != 32) {
        throw BedrockEncryptionError("secretKeyBytes must be 32 bytes for AES-256-GCM");
    }

    if (iv12.size() != 12) {
        throw BedrockEncryptionError("iv12 must be 12 bytes for AES-GCM");
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();

    if (!ctx) {
        throw BedrockEncryptionError("EVP_CIPHER_CTX_new encrypt failed");
    }

    std::vector<uint8_t> out;
    out.resize(plaintext.size() + 16);

    int len = 0;
    int total = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw BedrockEncryptionError("EVP_EncryptInit_ex cipher failed");
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv12.size()), nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw BedrockEncryptionError("EVP_CTRL_GCM_SET_IVLEN failed");
    }

    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, secretKeyBytes.data(), iv12.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw BedrockEncryptionError("EVP_EncryptInit_ex key/iv failed");
    }

    if (!plaintext.empty()) {
        if (EVP_EncryptUpdate(
            ctx,
            out.data(),
            &len,
            plaintext.data(),
            static_cast<int>(plaintext.size())
        ) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw BedrockEncryptionError("EVP_EncryptUpdate failed");
        }

        total += len;
    }

    if (EVP_EncryptFinal_ex(ctx, out.data() + total, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw BedrockEncryptionError("EVP_EncryptFinal_ex failed");
    }

    total += len;

    out.resize(static_cast<size_t>(total));

    EVP_CIPHER_CTX_free(ctx);
    return out;
}

std::vector<uint8_t> BedrockEncryption::aes256GcmDecryptNoTag(
    const std::vector<uint8_t>& secretKeyBytes,
    const std::vector<uint8_t>& iv12,
    const std::vector<uint8_t>& encrypted
) {
    if (secretKeyBytes.size() != 32) {
        throw BedrockEncryptionError("secretKeyBytes must be 32 bytes for AES-256-GCM");
    }

    if (iv12.size() != 12) {
        throw BedrockEncryptionError("iv12 must be 12 bytes for AES-GCM");
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();

    if (!ctx) {
        throw BedrockEncryptionError("EVP_CIPHER_CTX_new decrypt failed");
    }

    std::vector<uint8_t> out;
    out.resize(encrypted.size() + 16);

    int len = 0;
    int total = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw BedrockEncryptionError("EVP_DecryptInit_ex cipher failed");
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv12.size()), nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw BedrockEncryptionError("EVP_CTRL_GCM_SET_IVLEN decrypt failed");
    }

    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, secretKeyBytes.data(), iv12.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw BedrockEncryptionError("EVP_DecryptInit_ex key/iv decrypt failed");
    }

    if (!encrypted.empty()) {
        if (EVP_DecryptUpdate(
            ctx,
            out.data(),
            &len,
            encrypted.data(),
            static_cast<int>(encrypted.size())
        ) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw BedrockEncryptionError("EVP_DecryptUpdate failed");
        }

        total += len;
    }

    // Bedrock-protocol uses streaming GCM here and does not append/check an auth tag.
    out.resize(static_cast<size_t>(total));

    EVP_CIPHER_CTX_free(ctx);
    return out;
}

std::vector<uint8_t> BedrockEncryption::encryptMcpePayloadGcm(
    const std::vector<uint8_t>& packetPlaintext,
    uint64_t sendCounter,
    const std::vector<uint8_t>& secretKeyBytes,
    const std::vector<uint8_t>& iv16
) {
    if (iv16.size() < 12) {
        throw BedrockEncryptionError("iv16 must contain at least 12 bytes");
    }

    auto aesPlaintext = makeAesPlaintext(
        packetPlaintext,
        sendCounter,
        secretKeyBytes
    );

    std::vector<uint8_t> iv12(
        iv16.begin(),
        iv16.begin() + 12
    );

    auto encrypted = aes256GcmEncryptNoTag(
        secretKeyBytes,
        iv12,
        aesPlaintext
    );

    std::vector<uint8_t> out;
    out.reserve(1 + encrypted.size());

    out.push_back(0xfe);
    out.insert(out.end(), encrypted.begin(), encrypted.end());

    return out;
}

std::vector<uint8_t> BedrockEncryption::decryptMcpePayloadGcm(
    const std::vector<uint8_t>& encryptedMcpePayload,
    uint64_t receiveCounter,
    const std::vector<uint8_t>& secretKeyBytes,
    const std::vector<uint8_t>& iv16
) {
    if (encryptedMcpePayload.empty()) {
        throw BedrockEncryptionError("empty encrypted MCPE payload");
    }

    if (encryptedMcpePayload[0] != 0xfe) {
        throw BedrockEncryptionError("encrypted MCPE payload missing 0xfe header");
    }

    if (iv16.size() < 12) {
        throw BedrockEncryptionError("iv16 must contain at least 12 bytes");
    }

    std::vector<uint8_t> encrypted(
        encryptedMcpePayload.begin() + 1,
        encryptedMcpePayload.end()
    );

    if (encrypted.empty()) {
        return {};
    }

    std::vector<uint8_t> iv12(
        iv16.begin(),
        iv16.begin() + 12
    );

    auto aesPlaintext = aes256GcmDecryptNoTag(
        secretKeyBytes,
        iv12,
        encrypted
    );

    auto verification = verifyAesPlaintext(
        aesPlaintext,
        receiveCounter,
        secretKeyBytes
    );
    if (!verification.matches()) {
        throw BedrockEncryptionError(verification.mismatchMessage());
    }

    return verification.packetPlaintext;
}

} // namespace bedrock
