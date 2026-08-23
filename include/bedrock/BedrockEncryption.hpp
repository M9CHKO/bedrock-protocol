#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

namespace bedrock {

class BedrockEncryptionError : public std::runtime_error {
public:
    explicit BedrockEncryptionError(const std::string& msg)
        : std::runtime_error(msg) {}
};

enum class BedrockCipherMode {
    Encrypt,
    Decrypt
};

enum class BedrockCipherAlgorithm {
    Aes256Cfb8,
    Aes256GcmNoTag
};

struct BedrockChecksumVerification {
    std::vector<uint8_t> packetPlaintext;
    std::vector<uint8_t> receivedChecksum;
    std::vector<uint8_t> expectedChecksum;

    bool matches() const noexcept;
    std::string mismatchMessage() const;
};

class BedrockCipherStream {
public:
    using Mode = BedrockCipherMode;

    virtual ~BedrockCipherStream() = default;

    BedrockCipherStream(const BedrockCipherStream&) = delete;
    BedrockCipherStream& operator=(const BedrockCipherStream&) = delete;

    virtual BedrockCipherAlgorithm algorithm() const noexcept = 0;
    virtual std::vector<uint8_t> process(
        const std::vector<uint8_t>& input
    ) = 0;

protected:
    BedrockCipherStream() = default;
};

class BedrockAesGcmStream final : public BedrockCipherStream {
public:
    using Mode = BedrockCipherMode;

    BedrockAesGcmStream(
        const std::vector<uint8_t>& secretKeyBytes,
        const std::vector<uint8_t>& iv16,
        Mode mode
    );

    ~BedrockAesGcmStream() override;

    BedrockAesGcmStream(const BedrockAesGcmStream&) = delete;
    BedrockAesGcmStream& operator=(const BedrockAesGcmStream&) = delete;

    BedrockCipherAlgorithm algorithm() const noexcept override;
    std::vector<uint8_t> process(
        const std::vector<uint8_t>& input
    ) override;

private:
    EVP_CIPHER_CTX* ctx_ = nullptr;
    Mode mode_;
};

class BedrockAesCfb8Stream final : public BedrockCipherStream {
public:
    using Mode = BedrockCipherMode;

    BedrockAesCfb8Stream(
        const std::vector<uint8_t>& secretKeyBytes,
        const std::vector<uint8_t>& iv16,
        Mode mode
    );

    ~BedrockAesCfb8Stream() override;

    BedrockAesCfb8Stream(const BedrockAesCfb8Stream&) = delete;
    BedrockAesCfb8Stream& operator=(const BedrockAesCfb8Stream&) = delete;

    BedrockCipherAlgorithm algorithm() const noexcept override;
    std::vector<uint8_t> process(
        const std::vector<uint8_t>& input
    ) override;

private:
    EVP_CIPHER_CTX* ctx_ = nullptr;
    Mode mode_;
};

class BedrockEncryption {
public:
    // transforms/encryption.js switches from AES-256-CFB8 to the tagless
    // streaming AES-256-GCM form at Bedrock protocol 431 (1.16.220).
    static constexpr uint32_t GCM_PROTOCOL_VERSION = 431;

    static BedrockCipherAlgorithm cipherAlgorithmForProtocol(
        uint32_t protocolVersion
    ) noexcept;

    static std::unique_ptr<BedrockCipherStream> createCipherStream(
        uint32_t protocolVersion,
        const std::vector<uint8_t>& secretKeyBytes,
        const std::vector<uint8_t>& iv16,
        BedrockCipherMode mode
    );

    static std::vector<uint8_t> computeChecksum(
        const std::vector<uint8_t>& packetPlaintext,
        uint64_t sendCounter,
        const std::vector<uint8_t>& secretKeyBytes
    );

    static std::vector<uint8_t> makeAesPlaintext(
        const std::vector<uint8_t>& packetPlaintext,
        uint64_t sendCounter,
        const std::vector<uint8_t>& secretKeyBytes
    );

    // Mirrors encryption.js's two Buffer#slice calls exactly, including their
    // negative-index behavior when the decrypted chunk is shorter than the
    // eight-byte checksum. The receive counter is consumed after checksum
    // computation, before the caller handles mismatch or decompression.
    static BedrockChecksumVerification verifyAesPlaintext(
        const std::vector<uint8_t>& aesPlaintext,
        uint64_t& receiveCounter,
        const std::vector<uint8_t>& secretKeyBytes
    );

    // An empty cipher write produces no Node `data` event, so it returns no
    // verification result and leaves both stream and counter untouched.
    static std::optional<BedrockChecksumVerification> decryptAndVerify(
        BedrockCipherStream& decryptStream,
        const std::vector<uint8_t>& encrypted,
        uint64_t& receiveCounter,
        const std::vector<uint8_t>& secretKeyBytes
    );

    static std::vector<uint8_t> aes256GcmEncryptNoTag(
        const std::vector<uint8_t>& secretKeyBytes,
        const std::vector<uint8_t>& iv12,
        const std::vector<uint8_t>& plaintext
    );

    static std::vector<uint8_t> aes256GcmDecryptNoTag(
        const std::vector<uint8_t>& secretKeyBytes,
        const std::vector<uint8_t>& iv12,
        const std::vector<uint8_t>& encrypted
    );

    static std::vector<uint8_t> encryptMcpePayloadGcm(
        const std::vector<uint8_t>& packetPlaintext,
        uint64_t sendCounter,
        const std::vector<uint8_t>& secretKeyBytes,
        const std::vector<uint8_t>& iv16
    );

    static std::vector<uint8_t> decryptMcpePayloadGcm(
        const std::vector<uint8_t>& encryptedMcpePayload,
        uint64_t receiveCounter,
        const std::vector<uint8_t>& secretKeyBytes,
        const std::vector<uint8_t>& iv16
    );

private:
    static void writeU64LE(std::vector<uint8_t>& out, uint64_t v);
};

} // namespace bedrock
