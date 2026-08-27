#include "bedrock/BedrockFramer.hpp"

#include <bedrock/protocol/SnappyCodec.hpp>

#include <zlib.h>
#include <cstring>
#include <limits>

namespace bedrock {

namespace {

std::vector<uint8_t> compressSnappy(const std::vector<uint8_t>& input) {
    try {
        return SnappyCodec::compress(input);
    } catch (const SnappyCodecError& error) {
        throw BedrockFramerError(error.what());
    }
}

std::vector<uint8_t> decompressSnappy(const std::vector<uint8_t>& input) {
    try {
        return SnappyCodec::decompress(input);
    } catch (const SnappyCodecError& error) {
        throw BedrockFramerError(error.what());
    }
}

} // namespace

void BedrockFramer::writeVarUInt(std::vector<uint8_t>& out, uint32_t v) {
    while (true) {
        if ((v & ~0x7fu) == 0) {
            out.push_back(static_cast<uint8_t>(v));
            return;
        }

        out.push_back(static_cast<uint8_t>((v & 0x7f) | 0x80));
        v >>= 7;
    }
}

uint32_t BedrockFramer::readVarUInt(const std::vector<uint8_t>& data, size_t& off) {
    uint32_t result = 0;

    for (int shift = 0; shift <= 28; shift += 7) {
        if (off >= data.size()) {
            throw BedrockFramerError("readVarUInt out of range");
        }

        uint8_t byte = data[off++];

        result |= static_cast<uint32_t>(byte & 0x7f) << shift;

        if ((byte & 0x80) == 0) {
            return result;
        }
    }

    throw BedrockFramerError("VarUInt too big");
}

std::vector<uint8_t> BedrockFramer::framePackets(
    const std::vector<std::vector<uint8_t>>& packets
) {
    std::vector<uint8_t> out;

    for (const auto& packet : packets) {
        if (packet.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
            throw BedrockFramerError("packet too large");
        }

        writeVarUInt(out, static_cast<uint32_t>(packet.size()));
        out.insert(out.end(), packet.begin(), packet.end());
    }

    return out;
}

std::vector<std::vector<uint8_t>> BedrockFramer::unframePackets(
    const std::vector<uint8_t>& framed
) {
    std::vector<std::vector<uint8_t>> packets;

    size_t off = 0;

    while (off < framed.size()) {
        uint32_t len = readVarUInt(framed, off);

        if (off + len > framed.size()) {
            throw BedrockFramerError("framed packet length exceeds buffer");
        }

        packets.emplace_back(
            framed.begin() + static_cast<std::ptrdiff_t>(off),
            framed.begin() + static_cast<std::ptrdiff_t>(off + len)
        );

        off += len;
    }

    return packets;
}

std::vector<uint8_t> BedrockFramer::deflateRaw(
    const std::vector<uint8_t>& input
) {
    if (input.empty()) {
        return {};
    }

    z_stream zs {};
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());

    int ret = deflateInit2(
        &zs,
        Z_DEFAULT_COMPRESSION,
        Z_DEFLATED,
        -MAX_WBITS,
        8,
        Z_DEFAULT_STRATEGY
    );

    if (ret != Z_OK) {
        throw BedrockFramerError("deflateInit2 failed");
    }

    std::vector<uint8_t> out;
    uint8_t temp[32768];

    do {
        zs.next_out = temp;
        zs.avail_out = sizeof(temp);

        ret = deflate(&zs, Z_FINISH);

        if (ret != Z_OK && ret != Z_STREAM_END) {
            deflateEnd(&zs);
            throw BedrockFramerError("deflate failed");
        }

        size_t produced = sizeof(temp) - zs.avail_out;
        out.insert(out.end(), temp, temp + produced);
    } while (ret != Z_STREAM_END);

    deflateEnd(&zs);
    return out;
}

std::vector<uint8_t> BedrockFramer::inflateRaw(
    const std::vector<uint8_t>& input
) {
    if (input.empty()) {
        return {};
    }

    z_stream zs {};
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());

    int ret = inflateInit2(&zs, -MAX_WBITS);

    if (ret != Z_OK) {
        throw BedrockFramerError("inflateInit2 failed");
    }

    std::vector<uint8_t> out;
    uint8_t temp[32768];

    do {
        zs.next_out = temp;
        zs.avail_out = sizeof(temp);

        ret = inflate(&zs, Z_NO_FLUSH);

        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            throw BedrockFramerError("inflate failed");
        }

        size_t produced = sizeof(temp) - zs.avail_out;
        out.insert(out.end(), temp, temp + produced);
    } while (ret != Z_STREAM_END);

    inflateEnd(&zs);
    return out;
}

std::vector<uint8_t> BedrockFramer::encodeBatch(
    const std::vector<std::vector<uint8_t>>& packets,
    const BedrockFramerSettings& settings
) {
    std::vector<uint8_t> framed = framePackets(packets);

    std::vector<uint8_t> out;
    out.push_back(BATCH_HEADER);

    if (!settings.compressionReady) {
        out.insert(out.end(), framed.begin(), framed.end());
        return out;
    }

    bool shouldCompress = framed.size() > settings.compressionThreshold;

    if (!settings.compressorInHeader) {
        if (shouldCompress) {
            if (settings.compressionAlgorithm == 0) {
                auto compressed = deflateRaw(framed);
                out.insert(out.end(), compressed.begin(), compressed.end());
            } else if (settings.compressionAlgorithm == 1) {
                auto compressed = compressSnappy(framed);
                out.insert(out.end(), compressed.begin(), compressed.end());
            } else {
                throw BedrockFramerError("unknown compression algorithm");
            }
        } else {
            out.insert(out.end(), framed.begin(), framed.end());
        }
        return out;
    }

    if (!shouldCompress) {
        out.push_back(COMPRESSION_NONE);
        out.insert(out.end(), framed.begin(), framed.end());
        return out;
    }

    if (settings.compressionAlgorithm == 0) {
        out.push_back(COMPRESSION_DEFLATE);

        auto compressed = deflateRaw(framed);
        out.insert(out.end(), compressed.begin(), compressed.end());

        return out;
    }

    if (settings.compressionAlgorithm == 1) {
        out.push_back(COMPRESSION_SNAPPY);

        auto compressed = compressSnappy(framed);
        out.insert(out.end(), compressed.begin(), compressed.end());

        return out;
    }

    throw BedrockFramerError("unknown compression algorithm");
}

std::vector<std::vector<uint8_t>> BedrockFramer::decodeBatch(
    const std::vector<uint8_t>& batch,
    const BedrockFramerSettings& settings
) {
    if (batch.empty()) {
        throw BedrockFramerError("empty batch");
    }

    if (batch[0] != BATCH_HEADER) {
        throw BedrockFramerError("bad batch header");
    }

    size_t off = 1;
    std::vector<uint8_t> framed;

    if (!settings.compressionReady) {
        framed.assign(batch.begin() + static_cast<std::ptrdiff_t>(off), batch.end());
        return unframePackets(framed);
    }

    if (!settings.compressorInHeader) {
        std::vector<uint8_t> body(batch.begin() + static_cast<std::ptrdiff_t>(off), batch.end());
        try {
            if (settings.compressionAlgorithm == 0) {
                framed = inflateRaw(body);
            } else if (settings.compressionAlgorithm == 1) {
                framed = decompressSnappy(body);
            } else {
                throw BedrockFramerError("unknown compression algorithm");
            }
        } catch (const std::exception&) {
            framed = std::move(body);
        }
        return unframePackets(framed);
    }

    if (off >= batch.size()) {
        throw BedrockFramerError("missing compression header");
    }

    uint8_t compressionHeader = batch[off++];

    if (compressionHeader == COMPRESSION_NONE) {
        framed.assign(batch.begin() + static_cast<std::ptrdiff_t>(off), batch.end());
        return unframePackets(framed);
    }

    if (compressionHeader == COMPRESSION_DEFLATE) {
        std::vector<uint8_t> compressed(
            batch.begin() + static_cast<std::ptrdiff_t>(off),
            batch.end()
        );

        framed = inflateRaw(compressed);
        return unframePackets(framed);
    }

    if (compressionHeader == COMPRESSION_SNAPPY) {
        std::vector<uint8_t> compressed(
            batch.begin() + static_cast<std::ptrdiff_t>(off),
            batch.end()
        );

        framed = decompressSnappy(compressed);
        return unframePackets(framed);
    }

    throw BedrockFramerError("unknown compression header");
}

} // namespace bedrock
