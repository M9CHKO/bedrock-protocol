#pragma once

#include <bedrock/protocol/VersionedBatchCodec.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bedrock {

enum class VersionedMcpeCompression : uint8_t {
    DeflateRaw = 0x00,
    Snappy = 0x01,
    // Server-side extension used to distinguish the JavaScript Framer's
    // threshold-driven default from an explicit low-level wire override.
    Automatic = 0xfe,
    Uncompressed = 0xff
};

struct VersionedMcpePayload {
    uint8_t compressionHeader = 0xff;
    std::vector<uint8_t> compressionPacket;
    std::vector<uint8_t> framedBatch;
    VersionedPacketBatch batch;
};

class VersionedMcpeCodec {
public:
    explicit VersionedMcpeCodec(VersionedBatchCodec batchCodec);

    static VersionedMcpeCodec forVersion(const std::string& minecraftVersion);

    const VersionedBatchCodec& batchCodec() const;
    const VersionedPacketCodec& packetCodec() const;
    const ProtocolDefinition& definition() const;
    bool compressorInPacketHeader() const noexcept;

    VersionedMcpePayload decodeMcpePayload(const std::vector<uint8_t>& mcpePayload) const;
    VersionedMcpePayload decodeCompressionPacket(const std::vector<uint8_t>& compressionPacket) const;

    // encryption.js always deflates encrypted batches. Legacy protocols have
    // no compressor byte and, unlike the ordinary unencrypted decoder, do not
    // fall back to interpreting failed deflate input as an uncompressed batch.
    VersionedMcpePayload decodeEncryptedCompressionPacket(
        const std::vector<uint8_t>& compressionPacket
    ) const;

    // Before network_settings has enabled compressor-in-header framing, modern
    // Bedrock uses the same raw framed batch as the JavaScript Framer's
    // `compressionReady === false` branch.  Keep that negotiation state in the
    // connection and use these helpers only for that initial packet window.
    VersionedMcpePayload decodeUncompressedMcpePayload(
        const std::vector<uint8_t>& mcpePayload
    ) const;
    VersionedMcpePayload decodeUncompressedCompressionPacket(
        const std::vector<uint8_t>& compressionPacket
    ) const;

    std::vector<uint8_t> encodeMcpePayload(
        const std::vector<VersionedGamePacket>& packets,
        VersionedMcpeCompression compression = VersionedMcpeCompression::DeflateRaw,
        int compressionLevel = 7
    ) const;

    std::vector<uint8_t> encodeMcpePayload(
        const std::vector<VersionedGamePacket>& packets,
        const std::string& compressionAlgorithm,
        int compressionLevel,
        std::size_t compressionThreshold
    ) const;

    std::vector<uint8_t> encodeMcpePayloadByNames(
        const std::vector<std::pair<std::string, std::vector<uint8_t>>>& packets,
        VersionedMcpeCompression compression = VersionedMcpeCompression::DeflateRaw,
        int compressionLevel = 7
    ) const;

    std::vector<uint8_t> encodeMcpePayloadByNames(
        const std::vector<std::pair<std::string, std::vector<uint8_t>>>& packets,
        const std::string& compressionAlgorithm,
        int compressionLevel,
        std::size_t compressionThreshold
    ) const;

    std::vector<uint8_t> encodeCompressionPacket(
        const std::vector<VersionedGamePacket>& packets,
        VersionedMcpeCompression compression = VersionedMcpeCompression::DeflateRaw,
        int compressionLevel = 7
    ) const;

    // Mirrors transforms/framer.js: the strict threshold comparison is made
    // against the complete varint-framed batch, before compression.
    std::vector<uint8_t> encodeCompressionPacket(
        const std::vector<VersionedGamePacket>& packets,
        const std::string& compressionAlgorithm,
        int compressionLevel,
        std::size_t compressionThreshold
    ) const;

    // transforms/encryption.js does not use the Framer's threshold or
    // configured algorithm: every encrypted batch is raw-deflated. Modern
    // protocols additionally prefix the compressed bytes with mode 0.
    std::vector<uint8_t> encodeEncryptedCompressionPacket(
        const std::vector<VersionedGamePacket>& packets,
        int compressionLevel
    ) const;

    std::vector<uint8_t> encodeCompressionPacketByNames(
        const std::vector<std::pair<std::string, std::vector<uint8_t>>>& packets,
        VersionedMcpeCompression compression = VersionedMcpeCompression::DeflateRaw,
        int compressionLevel = 7
    ) const;

    std::vector<uint8_t> encodeCompressionPacketByNames(
        const std::vector<std::pair<std::string, std::vector<uint8_t>>>& packets,
        const std::string& compressionAlgorithm,
        int compressionLevel,
        std::size_t compressionThreshold
    ) const;

private:
    VersionedBatchCodec batchCodec_;
    bool compressorInPacketHeader_ = true;

    std::vector<uint8_t> encodeFramedBatch(
        const std::vector<uint8_t>& framedBatch,
        VersionedMcpeCompression compression,
        int compressionLevel
    ) const;
    static VersionedMcpeCompression chooseCompression(
        const std::string& compressionAlgorithm,
        bool shouldCompress
    );
    static std::vector<uint8_t> deflateRaw(const std::vector<uint8_t>& input, int compressionLevel);
    static std::vector<uint8_t> inflateRaw(const std::vector<uint8_t>& input);
    static bool versionAtLeast(const std::string& version, int major, int minor, int patch);
};

} // namespace bedrock
