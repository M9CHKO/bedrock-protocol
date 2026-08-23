#include <bedrock/protocol/VersionedMcpeCodec.hpp>

#include <zlib.h>

#include <cctype>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bedrock {

VersionedMcpeCodec::VersionedMcpeCodec(VersionedBatchCodec batchCodec)
    : batchCodec_(std::move(batchCodec)),
      compressorInPacketHeader_(versionAtLeast(batchCodec_.definition().minecraftVersion(), 1, 20, 61)) {}

VersionedMcpeCodec VersionedMcpeCodec::forVersion(const std::string& minecraftVersion) {
    return VersionedMcpeCodec(VersionedBatchCodec::forVersion(minecraftVersion));
}

const VersionedBatchCodec& VersionedMcpeCodec::batchCodec() const {
    return batchCodec_;
}

const VersionedPacketCodec& VersionedMcpeCodec::packetCodec() const {
    return batchCodec_.packetCodec();
}

const ProtocolDefinition& VersionedMcpeCodec::definition() const {
    return batchCodec_.definition();
}

bool VersionedMcpeCodec::compressorInPacketHeader() const noexcept {
    return compressorInPacketHeader_;
}

VersionedMcpePayload VersionedMcpeCodec::decodeMcpePayload(const std::vector<uint8_t>& mcpePayload) const {
    if (mcpePayload.empty()) {
        throw std::runtime_error("empty mcpe payload");
    }

    if (mcpePayload[0] != 0xfe) {
        throw std::runtime_error("mcpe payload missing 0xfe prefix");
    }

    if (mcpePayload.size() < 2) {
        throw std::runtime_error("mcpe payload missing compression header");
    }

    std::vector<uint8_t> compressionPacket(mcpePayload.begin() + 1, mcpePayload.end());
    return decodeCompressionPacket(compressionPacket);
}

VersionedMcpePayload VersionedMcpeCodec::decodeCompressionPacket(
    const std::vector<uint8_t>& compressionPacket
) const {
    if (compressionPacket.empty()) {
        throw std::runtime_error("empty compression packet");
    }

    VersionedMcpePayload payload;
    payload.compressionPacket = compressionPacket;

    if (!compressorInPacketHeader_) {
        payload.compressionHeader = static_cast<uint8_t>(VersionedMcpeCompression::DeflateRaw);
        try {
            payload.framedBatch = inflateRaw(compressionPacket);
        } catch (const std::exception&) {
            payload.compressionHeader = static_cast<uint8_t>(VersionedMcpeCompression::Uncompressed);
            payload.framedBatch = compressionPacket;
        }
        payload.batch = batchCodec_.decodeFramedBatch(payload.framedBatch);
        return payload;
    }

    payload.compressionHeader = compressionPacket[0];

    std::vector<uint8_t> body(compressionPacket.begin() + 1, compressionPacket.end());

    if (payload.compressionHeader == static_cast<uint8_t>(VersionedMcpeCompression::Uncompressed)) {
        payload.framedBatch = std::move(body);
    } else if (payload.compressionHeader == static_cast<uint8_t>(VersionedMcpeCompression::DeflateRaw)) {
        payload.framedBatch = inflateRaw(body);
    } else if (payload.compressionHeader == static_cast<uint8_t>(VersionedMcpeCompression::Snappy)) {
        throw std::runtime_error("Snappy compression not implemented");
    } else {
        throw std::runtime_error(
            "Unknown compression type " + std::to_string(payload.compressionHeader)
        );
    }

    payload.batch = batchCodec_.decodeFramedBatch(payload.framedBatch);
    return payload;
}

VersionedMcpePayload VersionedMcpeCodec::decodeEncryptedCompressionPacket(
    const std::vector<uint8_t>& compressionPacket
) const {
    if (compressorInPacketHeader_) {
        return decodeCompressionPacket(compressionPacket);
    }
    if (compressionPacket.empty()) {
        throw std::runtime_error("empty encrypted compression packet");
    }

    VersionedMcpePayload payload;
    payload.compressionHeader =
        static_cast<uint8_t>(VersionedMcpeCompression::DeflateRaw);
    payload.compressionPacket = compressionPacket;
    payload.framedBatch = inflateRaw(compressionPacket);
    payload.batch = batchCodec_.decodeFramedBatch(payload.framedBatch);
    return payload;
}

VersionedMcpePayload VersionedMcpeCodec::decodeUncompressedMcpePayload(
    const std::vector<uint8_t>& mcpePayload
) const {
    if (mcpePayload.empty()) {
        throw std::runtime_error("empty mcpe payload");
    }
    if (mcpePayload[0] != 0xfe) {
        throw std::runtime_error("mcpe payload missing 0xfe prefix");
    }

    return decodeUncompressedCompressionPacket(std::vector<uint8_t>(
        mcpePayload.begin() + 1,
        mcpePayload.end()
    ));
}

VersionedMcpePayload VersionedMcpeCodec::decodeUncompressedCompressionPacket(
    const std::vector<uint8_t>& compressionPacket
) const {
    VersionedMcpePayload payload;
    payload.compressionHeader =
        static_cast<uint8_t>(VersionedMcpeCompression::Uncompressed);
    payload.compressionPacket = compressionPacket;
    payload.framedBatch = compressionPacket;
    payload.batch = batchCodec_.decodeFramedBatch(payload.framedBatch);
    return payload;
}

std::vector<uint8_t> VersionedMcpeCodec::encodeMcpePayload(
    const std::vector<VersionedGamePacket>& packets,
    VersionedMcpeCompression compression,
    int compressionLevel
) const {
    auto compressionPacket = encodeCompressionPacket(packets, compression, compressionLevel);

    std::vector<uint8_t> out;
    out.reserve(1 + compressionPacket.size());
    out.push_back(0xfe);
    out.insert(out.end(), compressionPacket.begin(), compressionPacket.end());
    return out;
}

std::vector<uint8_t> VersionedMcpeCodec::encodeMcpePayload(
    const std::vector<VersionedGamePacket>& packets,
    const std::string& compressionAlgorithm,
    int compressionLevel,
    std::size_t compressionThreshold
) const {
    auto compressionPacket = encodeCompressionPacket(
        packets,
        compressionAlgorithm,
        compressionLevel,
        compressionThreshold
    );

    std::vector<uint8_t> out;
    out.reserve(1 + compressionPacket.size());
    out.push_back(0xfe);
    out.insert(out.end(), compressionPacket.begin(), compressionPacket.end());
    return out;
}

std::vector<uint8_t> VersionedMcpeCodec::encodeMcpePayloadByNames(
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& packets,
    VersionedMcpeCompression compression,
    int compressionLevel
) const {
    auto compressionPacket = encodeCompressionPacketByNames(packets, compression, compressionLevel);

    std::vector<uint8_t> out;
    out.reserve(1 + compressionPacket.size());
    out.push_back(0xfe);
    out.insert(out.end(), compressionPacket.begin(), compressionPacket.end());
    return out;
}

std::vector<uint8_t> VersionedMcpeCodec::encodeMcpePayloadByNames(
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& packets,
    const std::string& compressionAlgorithm,
    int compressionLevel,
    std::size_t compressionThreshold
) const {
    auto compressionPacket = encodeCompressionPacketByNames(
        packets,
        compressionAlgorithm,
        compressionLevel,
        compressionThreshold
    );

    std::vector<uint8_t> out;
    out.reserve(1 + compressionPacket.size());
    out.push_back(0xfe);
    out.insert(out.end(), compressionPacket.begin(), compressionPacket.end());
    return out;
}

std::vector<uint8_t> VersionedMcpeCodec::encodeCompressionPacket(
    const std::vector<VersionedGamePacket>& packets,
    VersionedMcpeCompression compression,
    int compressionLevel
) const {
    auto framedBatch = batchCodec_.encodeFramedBatch(packets);
    return encodeFramedBatch(framedBatch, compression, compressionLevel);
}

std::vector<uint8_t> VersionedMcpeCodec::encodeCompressionPacket(
    const std::vector<VersionedGamePacket>& packets,
    const std::string& compressionAlgorithm,
    int compressionLevel,
    std::size_t compressionThreshold
) const {
    auto framedBatch = batchCodec_.encodeFramedBatch(packets);
    const bool shouldCompress = framedBatch.size() > compressionThreshold;
    return encodeFramedBatch(
        framedBatch,
        chooseCompression(compressionAlgorithm, shouldCompress),
        compressionLevel
    );
}

std::vector<uint8_t> VersionedMcpeCodec::encodeEncryptedCompressionPacket(
    const std::vector<VersionedGamePacket>& packets,
    int compressionLevel
) const {
    auto compressed = deflateRaw(
        batchCodec_.encodeFramedBatch(packets),
        compressionLevel
    );
    if (!compressorInPacketHeader_) {
        return compressed;
    }

    std::vector<uint8_t> out;
    out.reserve(1 + compressed.size());
    out.push_back(static_cast<uint8_t>(VersionedMcpeCompression::DeflateRaw));
    out.insert(out.end(), compressed.begin(), compressed.end());
    return out;
}

std::vector<uint8_t> VersionedMcpeCodec::encodeFramedBatch(
    const std::vector<uint8_t>& framedBatch,
    VersionedMcpeCompression compression,
    int compressionLevel
) const {
    if (compression == VersionedMcpeCompression::Automatic) {
        throw std::runtime_error("unsupported compression mode");
    }

    std::vector<uint8_t> out;

    if (!compressorInPacketHeader_) {
        if (compression == VersionedMcpeCompression::Uncompressed) {
            return framedBatch;
        }

        if (compression == VersionedMcpeCompression::DeflateRaw) {
            return deflateRaw(framedBatch, compressionLevel);
        }

        if (compression == VersionedMcpeCompression::Snappy) {
            throw std::runtime_error("Snappy compression not implemented");
        }

        throw std::runtime_error("unsupported compression mode");
    }

    out.push_back(static_cast<uint8_t>(compression));

    if (compression == VersionedMcpeCompression::Uncompressed) {
        out.insert(out.end(), framedBatch.begin(), framedBatch.end());
        return out;
    }

    if (compression == VersionedMcpeCompression::DeflateRaw) {
        auto compressed = deflateRaw(framedBatch, compressionLevel);
        out.insert(out.end(), compressed.begin(), compressed.end());
        return out;
    }

    if (compression == VersionedMcpeCompression::Snappy) {
        throw std::runtime_error("Snappy compression not implemented");
    }

    throw std::runtime_error("unsupported compression mode");
}

std::vector<uint8_t> VersionedMcpeCodec::encodeCompressionPacketByNames(
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& packets,
    VersionedMcpeCompression compression,
    int compressionLevel
) const {
    std::vector<VersionedGamePacket> made;
    made.reserve(packets.size());

    for (const auto& item : packets) {
        made.push_back(packetCodec().makePacketByName(item.first, item.second));
    }

    return encodeCompressionPacket(made, compression, compressionLevel);
}

std::vector<uint8_t> VersionedMcpeCodec::encodeCompressionPacketByNames(
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& packets,
    const std::string& compressionAlgorithm,
    int compressionLevel,
    std::size_t compressionThreshold
) const {
    std::vector<VersionedGamePacket> made;
    made.reserve(packets.size());

    for (const auto& item : packets) {
        made.push_back(packetCodec().makePacketByName(item.first, item.second));
    }

    return encodeCompressionPacket(
        made,
        compressionAlgorithm,
        compressionLevel,
        compressionThreshold
    );
}

VersionedMcpeCompression VersionedMcpeCodec::chooseCompression(
    const std::string& compressionAlgorithm,
    bool shouldCompress
) {
    if (compressionAlgorithm == "none") {
        return VersionedMcpeCompression::Uncompressed;
    }
    if (compressionAlgorithm == "deflate") {
        return shouldCompress
            ? VersionedMcpeCompression::DeflateRaw
            : VersionedMcpeCompression::Uncompressed;
    }
    if (compressionAlgorithm == "snappy") {
        return shouldCompress
            ? VersionedMcpeCompression::Snappy
            : VersionedMcpeCompression::Uncompressed;
    }

    throw std::runtime_error("Unknown compression algorithm: " + compressionAlgorithm);
}

std::vector<uint8_t> VersionedMcpeCodec::deflateRaw(
    const std::vector<uint8_t>& input,
    int compressionLevel
) {
    z_stream stream{};

    int init = deflateInit2(
        &stream,
        compressionLevel,
        Z_DEFLATED,
        -MAX_WBITS,
        8,
        Z_DEFAULT_STRATEGY
    );

    if (init != Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }

    uint8_t dummy = 0;
    stream.next_in = input.empty()
        ? reinterpret_cast<Bytef*>(&dummy)
        : const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());

    std::vector<uint8_t> output;
    uint8_t buffer[32768];

    while (true) {
        stream.next_out = buffer;
        stream.avail_out = sizeof(buffer);

        int ret = deflate(&stream, Z_FINISH);

        if (ret != Z_OK && ret != Z_STREAM_END) {
            deflateEnd(&stream);
            throw std::runtime_error("deflate failed");
        }

        std::size_t produced = sizeof(buffer) - stream.avail_out;
        output.insert(output.end(), buffer, buffer + produced);

        if (ret == Z_STREAM_END) {
            break;
        }
    }

    deflateEnd(&stream);
    return output;
}

std::vector<uint8_t> VersionedMcpeCodec::inflateRaw(const std::vector<uint8_t>& input) {
    z_stream stream{};

    int init = inflateInit2(&stream, -MAX_WBITS);
    if (init != Z_OK) {
        throw std::runtime_error("inflateInit2 failed");
    }

    uint8_t dummy = 0;
    stream.next_in = input.empty()
        ? reinterpret_cast<Bytef*>(&dummy)
        : const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());

    std::vector<uint8_t> output;
    uint8_t buffer[32768];

    while (true) {
        stream.next_out = buffer;
        stream.avail_out = sizeof(buffer);

        int ret = inflate(&stream, Z_NO_FLUSH);
        std::size_t produced = sizeof(buffer) - stream.avail_out;

        output.insert(output.end(), buffer, buffer + produced);

        if (ret == Z_STREAM_END) {
            break;
        }

        if (ret != Z_OK) {
            inflateEnd(&stream);
            throw std::runtime_error("inflate failed");
        }

        if (stream.avail_in == 0 && produced == 0) {
            inflateEnd(&stream);
            throw std::runtime_error("incomplete deflate stream");
        }
    }

    inflateEnd(&stream);
    return output;
}

bool VersionedMcpeCodec::versionAtLeast(const std::string& version, int major, int minor, int patch) {
    std::vector<int> parts;
    std::string current;

    for (char c : version) {
        if (c == '.') {
            parts.push_back(current.empty() ? 0 : std::stoi(current));
            current.clear();
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            current.push_back(c);
        }
    }

    parts.push_back(current.empty() ? 0 : std::stoi(current));
    while (parts.size() < 3) {
        parts.push_back(0);
    }

    if (parts[0] != major) return parts[0] > major;
    if (parts[1] != minor) return parts[1] > minor;
    return parts[2] >= patch;
}

} // namespace bedrock
