#include <bedrock/protocol/BatchCodec.hpp>

#include <bedrock/protocol/GamePacketCodec.hpp>
#include <bedrock/protocol/SnappyCodec.hpp>

#include <zlib.h>

#include <stdexcept>
#include <string>

namespace bedrock {

std::vector<uint8_t> BatchCodec::encodeFramedBatch(
    const std::vector<std::vector<uint8_t>>& fullPackets
) {
    return GamePacketCodec::encodeBatchFromFullPackets(fullPackets);
}

DecodedBatch BatchCodec::decodeFramedBatch(
    const std::vector<uint8_t>& framedPackets
) {
    return GamePacketCodec::decodeBatch(framedPackets);
}

std::vector<uint8_t> BatchCodec::encodeCompressionPacket(
    const std::vector<uint8_t>& framedPackets,
    CompressionMode mode,
    size_t threshold
) {
    std::vector<uint8_t> out;

    bool useDeflate = false;
    bool useSnappy = false;

    switch (mode) {
        case CompressionMode::AlwaysDeflate:
            useDeflate = true;
            break;

        case CompressionMode::Threshold:
            useDeflate = framedPackets.size() >= threshold;
            break;

        case CompressionMode::Never:
            useDeflate = false;
            break;

        case CompressionMode::AlwaysSnappy:
            useSnappy = true;
            break;

        case CompressionMode::SnappyThreshold:
            useSnappy = framedPackets.size() >= threshold;
            break;
    }

    if (useDeflate) {
        out.push_back(0x00);
        auto compressed = deflateRaw(framedPackets);
        out.insert(out.end(), compressed.begin(), compressed.end());
    } else if (useSnappy) {
        out.push_back(0x01);
        auto compressed = SnappyCodec::compress(framedPackets);
        out.insert(out.end(), compressed.begin(), compressed.end());
    } else {
        out.push_back(0xff);
        out.insert(out.end(), framedPackets.begin(), framedPackets.end());
    }

    return out;
}

std::vector<uint8_t> BatchCodec::decodeCompressionPacket(
    const std::vector<uint8_t>& compressionPacket,
    uint8_t* compressionHeaderOut
) {
    if (compressionPacket.empty()) {
        throw std::runtime_error("BatchCodec::decodeCompressionPacket: empty packet");
    }

    const uint8_t header = compressionPacket[0];

    if (compressionHeaderOut != nullptr) {
        *compressionHeaderOut = header;
    }

    std::vector<uint8_t> body(
        compressionPacket.begin() + 1,
        compressionPacket.end()
    );

    if (header == 0x00) {
        return inflateRaw(body);
    }

    if (header == 0xff) {
        return body;
    }

    if (header == 0x01) {
        return SnappyCodec::decompress(body);
    }

    throw std::runtime_error(
        "BatchCodec::decodeCompressionPacket: unknown compression header " +
        std::to_string(static_cast<int>(header))
    );
}

std::vector<uint8_t> BatchCodec::wrapMcpe(
    const std::vector<uint8_t>& compressionPacket
) {
    std::vector<uint8_t> out;
    out.reserve(compressionPacket.size() + 1);

    out.push_back(0xfe);
    out.insert(out.end(), compressionPacket.begin(), compressionPacket.end());

    return out;
}

std::vector<uint8_t> BatchCodec::unwrapMcpe(
    const std::vector<uint8_t>& mcpePayload
) {
    if (mcpePayload.empty()) {
        throw std::runtime_error("BatchCodec::unwrapMcpe: empty payload");
    }

    if (mcpePayload[0] != 0xfe) {
        throw std::runtime_error("BatchCodec::unwrapMcpe: missing 0xfe header");
    }

    return std::vector<uint8_t>(
        mcpePayload.begin() + 1,
        mcpePayload.end()
    );
}

std::vector<uint8_t> BatchCodec::deflateRaw(
    const std::vector<uint8_t>& input
) {
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());

    int ret = deflateInit2(
        &stream,
        Z_DEFAULT_COMPRESSION,
        Z_DEFLATED,
        -MAX_WBITS,
        8,
        Z_DEFAULT_STRATEGY
    );

    if (ret != Z_OK) {
        throw std::runtime_error("BatchCodec::deflateRaw: deflateInit2 failed");
    }

    std::vector<uint8_t> out;
    uint8_t buffer[16384];

    do {
        stream.next_out = buffer;
        stream.avail_out = sizeof(buffer);

        ret = deflate(&stream, Z_FINISH);

        if (ret != Z_OK && ret != Z_STREAM_END) {
            deflateEnd(&stream);
            throw std::runtime_error("BatchCodec::deflateRaw: deflate failed");
        }

        const size_t produced = sizeof(buffer) - stream.avail_out;
        out.insert(out.end(), buffer, buffer + produced);
    } while (ret != Z_STREAM_END);

    deflateEnd(&stream);
    return out;
}

std::vector<uint8_t> BatchCodec::inflateRaw(
    const std::vector<uint8_t>& input
) {
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());

    int ret = inflateInit2(&stream, -MAX_WBITS);

    if (ret != Z_OK) {
        throw std::runtime_error("BatchCodec::inflateRaw: inflateInit2 failed");
    }

    std::vector<uint8_t> out;
    uint8_t buffer[16384];

    do {
        stream.next_out = buffer;
        stream.avail_out = sizeof(buffer);

        ret = inflate(&stream, Z_NO_FLUSH);

        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&stream);
            throw std::runtime_error("BatchCodec::inflateRaw: inflate failed");
        }

        const size_t produced = sizeof(buffer) - stream.avail_out;
        out.insert(out.end(), buffer, buffer + produced);
    } while (ret != Z_STREAM_END);

    inflateEnd(&stream);
    return out;
}

} // namespace bedrock
