#include <bedrock/protocol/SnappyCodec.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace bedrock {
namespace {

constexpr std::size_t SNAPPY_FRAGMENT_SIZE = 65536;
constexpr unsigned HASH_BITS = 15;
constexpr std::size_t HASH_SIZE = std::size_t {1} << HASH_BITS;
constexpr uint32_t HASH_MULTIPLIER = 0x1e35a7bdu;
constexpr std::size_t NO_POSITION = std::numeric_limits<std::size_t>::max();

uint32_t load32(const std::vector<uint8_t>& input, std::size_t offset) {
    return static_cast<uint32_t>(input[offset]) |
        (static_cast<uint32_t>(input[offset + 1]) << 8u) |
        (static_cast<uint32_t>(input[offset + 2]) << 16u) |
        (static_cast<uint32_t>(input[offset + 3]) << 24u);
}

std::size_t hashSequence(uint32_t sequence) {
    return static_cast<std::size_t>(
        (sequence * HASH_MULTIPLIER) >> (32u - HASH_BITS)
    );
}

void writeVarUInt32(std::vector<uint8_t>& output, uint32_t value) {
    while (value >= 0x80u) {
        output.push_back(static_cast<uint8_t>((value & 0x7fu) | 0x80u));
        value >>= 7u;
    }
    output.push_back(static_cast<uint8_t>(value));
}

uint32_t readVarUInt32(const std::vector<uint8_t>& input, std::size_t& offset) {
    if (input.empty()) {
        throw SnappyCodecError("snappy block is missing the uncompressed length");
    }

    uint32_t value = 0;
    for (unsigned shift = 0; shift <= 28; shift += 7) {
        if (offset >= input.size()) {
            throw SnappyCodecError("snappy uncompressed length is truncated");
        }

        const uint8_t byte = input[offset++];
        if (shift == 28 && (byte & 0xf0u) != 0) {
            throw SnappyCodecError("snappy uncompressed length is invalid");
        }

        value |= static_cast<uint32_t>(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) {
            return value;
        }
    }

    throw SnappyCodecError("snappy uncompressed length is invalid");
}

void emitLiteral(
    std::vector<uint8_t>& output,
    const std::vector<uint8_t>& input,
    std::size_t offset,
    std::size_t length
) {
    if (length == 0) {
        return;
    }

    const uint32_t lengthMinusOne = static_cast<uint32_t>(length - 1);
    if (length <= 60) {
        output.push_back(static_cast<uint8_t>(lengthMinusOne << 2u));
    } else {
        unsigned lengthBytes = 0;
        uint32_t remaining = lengthMinusOne;
        do {
            ++lengthBytes;
            remaining >>= 8u;
        } while (remaining != 0);

        output.push_back(static_cast<uint8_t>((59u + lengthBytes) << 2u));
        for (unsigned i = 0; i < lengthBytes; ++i) {
            output.push_back(static_cast<uint8_t>(lengthMinusOne >> (8u * i)));
        }
    }

    output.insert(
        output.end(),
        input.begin() + static_cast<std::ptrdiff_t>(offset),
        input.begin() + static_cast<std::ptrdiff_t>(offset + length)
    );
}

void emitCopyOne(std::vector<uint8_t>& output, uint32_t offset, uint32_t length) {
    output.push_back(static_cast<uint8_t>(
        0x01u |
        ((length - 4u) << 2u) |
        ((offset >> 8u) << 5u)
    ));
    output.push_back(static_cast<uint8_t>(offset));
}

void emitCopyTwo(std::vector<uint8_t>& output, uint32_t offset, uint32_t length) {
    output.push_back(static_cast<uint8_t>(((length - 1u) << 2u) | 0x02u));
    output.push_back(static_cast<uint8_t>(offset));
    output.push_back(static_cast<uint8_t>(offset >> 8u));
}

void emitCopy(std::vector<uint8_t>& output, uint32_t offset, std::size_t length) {
    while (length >= 68) {
        emitCopyTwo(output, offset, 64);
        length -= 64;
    }
    if (length > 64) {
        emitCopyTwo(output, offset, 60);
        length -= 60;
    }

    const auto finalLength = static_cast<uint32_t>(length);
    if (offset < 2048u && finalLength >= 4u && finalLength <= 11u) {
        emitCopyOne(output, offset, finalLength);
    } else {
        emitCopyTwo(output, offset, finalLength);
    }
}

void compressFragment(
    const std::vector<uint8_t>& input,
    std::size_t fragmentBegin,
    std::size_t fragmentEnd,
    std::vector<uint8_t>& output
) {
    const std::size_t fragmentLength = fragmentEnd - fragmentBegin;
    if (fragmentLength < 4) {
        emitLiteral(output, input, fragmentBegin, fragmentLength);
        return;
    }

    std::vector<std::size_t> table(HASH_SIZE, NO_POSITION);
    std::size_t nextLiteral = fragmentBegin;
    std::size_t position = fragmentBegin;
    const std::size_t lastSequence = fragmentEnd - 4;

    while (position <= lastSequence) {
        const auto hash = hashSequence(load32(input, position));
        const std::size_t candidate = table[hash];
        table[hash] = position;

        if (candidate == NO_POSITION ||
            candidate < fragmentBegin ||
            load32(input, candidate) != load32(input, position)) {
            ++position;
            continue;
        }

        emitLiteral(output, input, nextLiteral, position - nextLiteral);

        std::size_t matchLength = 4;
        while (position + matchLength < fragmentEnd &&
               input[candidate + matchLength] == input[position + matchLength]) {
            ++matchLength;
        }

        emitCopy(
            output,
            static_cast<uint32_t>(position - candidate),
            matchLength
        );
        position += matchLength;
        nextLiteral = position;

        if (position > lastSequence) {
            break;
        }

        // Seed the table with the sequence immediately before the next scan.
        // This retains short adjacent matches without walking every byte of a
        // long match that was already emitted.
        const std::size_t previous = position - 1;
        table[hashSequence(load32(input, previous))] = previous;
    }

    emitLiteral(output, input, nextLiteral, fragmentEnd - nextLiteral);
}

uint32_t readLittleEndian(
    const std::vector<uint8_t>& input,
    std::size_t& offset,
    unsigned byteCount,
    const char* truncatedMessage
) {
    if (byteCount > input.size() - offset) {
        throw SnappyCodecError(truncatedMessage);
    }

    uint32_t value = 0;
    for (unsigned i = 0; i < byteCount; ++i) {
        value |= static_cast<uint32_t>(input[offset++]) << (8u * i);
    }
    return value;
}

} // namespace

std::vector<uint8_t> SnappyCodec::compress(const std::vector<uint8_t>& input) {
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
        throw SnappyCodecError("snappy input exceeds the 32-bit block limit");
    }

    std::vector<uint8_t> output;
    output.reserve(input.size() + input.size() / 6 + 32);
    writeVarUInt32(output, static_cast<uint32_t>(input.size()));

    std::size_t fragmentBegin = 0;
    while (fragmentBegin < input.size()) {
        const std::size_t fragmentLength = std::min(
            SNAPPY_FRAGMENT_SIZE,
            input.size() - fragmentBegin
        );
        const std::size_t fragmentEnd = fragmentBegin + fragmentLength;
        compressFragment(input, fragmentBegin, fragmentEnd, output);
        fragmentBegin = fragmentEnd;
    }

    return output;
}

std::vector<uint8_t> SnappyCodec::decompress(const std::vector<uint8_t>& input) {
    std::size_t inputOffset = 0;
    const uint32_t declaredLength = readVarUInt32(input, inputOffset);

    std::vector<uint8_t> output(static_cast<std::size_t>(declaredLength));
    std::size_t outputOffset = 0;

    while (inputOffset < input.size() && outputOffset < output.size()) {
        const uint8_t tag = input[inputOffset++];
        const uint8_t type = tag & 0x03u;

        if (type == 0) {
            const uint8_t encodedLength = tag >> 2u;
            uint64_t literalLength = 0;
            if (encodedLength < 60) {
                literalLength = static_cast<uint64_t>(encodedLength) + 1u;
            } else {
                const unsigned lengthBytes = encodedLength - 59u;
                const uint32_t lengthMinusOne = readLittleEndian(
                    input,
                    inputOffset,
                    lengthBytes,
                    "snappy literal length is truncated"
                );
                literalLength = static_cast<uint64_t>(lengthMinusOne) + 1u;
            }

            if (literalLength > input.size() - inputOffset) {
                throw SnappyCodecError("snappy literal is truncated");
            }
            if (literalLength > output.size() - outputOffset) {
                throw SnappyCodecError("snappy literal exceeds the declared size");
            }

            const auto count = static_cast<std::size_t>(literalLength);
            std::copy_n(
                input.begin() + static_cast<std::ptrdiff_t>(inputOffset),
                count,
                output.begin() + static_cast<std::ptrdiff_t>(outputOffset)
            );
            inputOffset += count;
            outputOffset += count;
            continue;
        }

        uint32_t copyLength = 0;
        uint32_t copyOffset = 0;
        if (type == 1) {
            copyLength = 4u + ((tag >> 2u) & 0x07u);
            const uint32_t offsetLow = readLittleEndian(
                input,
                inputOffset,
                1,
                "snappy copy-1 offset is truncated"
            );
            copyOffset = ((static_cast<uint32_t>(tag) & 0xe0u) << 3u) | offsetLow;
        } else if (type == 2) {
            copyLength = 1u + (tag >> 2u);
            copyOffset = readLittleEndian(
                input,
                inputOffset,
                2,
                "snappy copy-2 offset is truncated"
            );
        } else {
            copyLength = 1u + (tag >> 2u);
            copyOffset = readLittleEndian(
                input,
                inputOffset,
                4,
                "snappy copy-4 offset is truncated"
            );
        }

        if (copyOffset == 0 || copyOffset > outputOffset) {
            throw SnappyCodecError("snappy copy offset is invalid");
        }
        if (copyLength > output.size() - outputOffset) {
            throw SnappyCodecError("snappy copy exceeds the declared size");
        }

        for (uint32_t i = 0; i < copyLength; ++i) {
            output[outputOffset] = output[outputOffset - copyOffset];
            ++outputOffset;
        }
    }

    if (outputOffset != output.size()) {
        throw SnappyCodecError("snappy block ended before the declared size");
    }
    if (inputOffset != input.size()) {
        throw SnappyCodecError("snappy block has trailing data");
    }

    return output;
}

} // namespace bedrock
