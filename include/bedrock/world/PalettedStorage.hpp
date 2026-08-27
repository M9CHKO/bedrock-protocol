#pragma once

#include <bedrock/BinaryStream.hpp>
#include <bedrock/nbt/BedrockNbt.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

struct PalettedStorageHeader {
    uint8_t raw = 0;
    uint8_t bitsPerBlock = 0;
    bool runtime = false;
};

struct PalettedStorageInfo {
    PalettedStorageHeader header;
    std::size_t offset = 0;
    std::size_t wordCount = 0;
    std::size_t paletteCount = 0;
    std::size_t totalBytes = 0;
    std::vector<uint32_t> words;
    std::vector<uint32_t> runtimePalette;
    std::vector<NbtDocument> persistentPalette;

    std::size_t getPaletteIndex(std::size_t blockIndex) const {
        if (blockIndex >= 4096) {
            throw std::runtime_error("block index outside paletted storage");
        }
        if (header.bitsPerBlock == 0) {
            return 0;
        }

        const std::size_t bits = header.bitsPerBlock;
        const std::size_t blocksPerWord = 32 / bits;
        if (blocksPerWord == 0) {
            throw std::runtime_error("invalid paletted storage bits per block");
        }
        const std::size_t wordIndex = blockIndex / blocksPerWord;
        const std::size_t bitOffset = (blockIndex % blocksPerWord) * bits;
        if (wordIndex >= words.size()) {
            throw std::runtime_error("paletted storage word index outside data");
        }

        const uint32_t mask = bits == 32
            ? 0xffffffffu
            : ((1u << bits) - 1u);
        return static_cast<std::size_t>((words[wordIndex] >> bitOffset) & mask);
    }

    uint32_t getBlockRuntimeId(std::size_t blockIndex) const {
        if (!header.runtime || runtimePalette.empty()) {
            throw std::runtime_error("runtime palette is empty");
        }
        const std::size_t paletteIndex = getPaletteIndex(blockIndex);
        if (paletteIndex >= runtimePalette.size()) {
            throw std::runtime_error("palette index outside runtime palette");
        }
        return runtimePalette[paletteIndex];
    }

    const NbtDocument& getBlockPersistentState(std::size_t blockIndex) const {
        if (header.runtime || persistentPalette.empty()) {
            throw std::runtime_error("persistent palette is empty");
        }
        const std::size_t paletteIndex = getPaletteIndex(blockIndex);
        if (paletteIndex >= persistentPalette.size()) {
            throw std::runtime_error("palette index outside persistent palette");
        }
        return persistentPalette[paletteIndex];
    }
};

class PalettedStorageParser {
public:
    static PalettedStorageHeader readHeader(uint8_t byte) {
        PalettedStorageHeader out;
        out.raw = byte;
        out.bitsPerBlock = byte >> 1;
        out.runtime = (byte & 0x01) != 0;
        return out;
    }

    static std::size_t wordsFor4096Blocks(uint8_t bitsPerBlock) {
        if (bitsPerBlock == 0) {
            return 0;
        }
        if (bitsPerBlock > 32) {
            throw std::runtime_error("paletted storage bits per block exceeds 32");
        }
        const std::size_t blocksPerWord = 32 / bitsPerBlock;
        return (4096 + blocksPerWord - 1) / blocksPerWord;
    }

    static PalettedStorageInfo scanAt(
        const std::vector<uint8_t>& data,
        std::size_t offset,
        BedrockNbtEncoding persistentEncoding = BedrockNbtEncoding::LittleVarInt
    ) {
        require(data, offset, 1, "paletted storage header");

        PalettedStorageInfo out;
        out.offset = offset;
        out.header = readHeader(data[offset++]);

        out.wordCount = wordsFor4096Blocks(out.header.bitsPerBlock);

        const std::size_t wordsBytes = out.wordCount * 4;
        require(data, offset, wordsBytes, "paletted storage words");

        out.words.reserve(out.wordCount);
        for (std::size_t i = 0; i < out.wordCount; ++i) {
            uint32_t word =
                static_cast<uint32_t>(data[offset]) |
                (static_cast<uint32_t>(data[offset + 1]) << 8) |
                (static_cast<uint32_t>(data[offset + 2]) << 16) |
                (static_cast<uint32_t>(data[offset + 3]) << 24);

            out.words.push_back(word);
            offset += 4;
        }

        if (out.header.runtime && out.header.bitsPerBlock == 0) {
            const int32_t runtimeId = readZigZagVarInt(data, offset);
            if (runtimeId < 0) {
                throw std::runtime_error("negative runtime id in paletted storage");
            }
            out.paletteCount = 1;
            out.runtimePalette.push_back(static_cast<uint32_t>(runtimeId));
            out.totalBytes = offset - out.offset;
            return out;
        }
        if (!out.header.runtime && out.header.bitsPerBlock == 0) {
            throw std::runtime_error("persistent paletted storage cannot use zero bits per block");
        }

        int64_t paletteCount = 0;
        if (!out.header.runtime && persistentEncoding == BedrockNbtEncoding::LittleEndian) {
            paletteCount = readU32LE(data, offset);
        } else {
            paletteCount = readZigZagVarInt(data, offset);
        }
        if (paletteCount < 1 || paletteCount > 4096) {
            throw std::runtime_error("invalid paletted storage palette count");
        }
        out.paletteCount = static_cast<std::size_t>(paletteCount);

        const uint64_t capacity = out.header.bitsPerBlock == 32
            ? (uint64_t{1} << 32u)
            : (uint64_t{1} << out.header.bitsPerBlock);
        if (out.paletteCount > capacity) {
            throw std::runtime_error("paletted storage palette does not fit bits per block");
        }

        if (out.header.runtime) {
            out.runtimePalette.reserve(out.paletteCount);
            for (std::size_t i = 0; i < out.paletteCount; ++i) {
                const int32_t runtimeId = readZigZagVarInt(data, offset);
                if (runtimeId < 0) {
                    throw std::runtime_error("negative runtime id in paletted storage");
                }
                out.runtimePalette.push_back(static_cast<uint32_t>(runtimeId));
            }
        } else {
            BinaryStream stream(data);
            stream.seek(offset);
            out.persistentPalette.reserve(out.paletteCount);
            for (std::size_t i = 0; i < out.paletteCount; ++i) {
                out.persistentPalette.push_back(BedrockNbtCodec::read(stream, persistentEncoding));
            }
            offset = stream.offset();
        }

        out.totalBytes = offset - out.offset;
        return out;
    }

private:
    static void require(
        const std::vector<uint8_t>& data,
        std::size_t offset,
        std::size_t size,
        const char* what
    ) {
        if (offset > data.size() || size > data.size() - offset) {
            throw std::runtime_error(std::string("not enough bytes for ") + what);
        }
    }

    static uint32_t readU32LE(
        const std::vector<uint8_t>& data,
        std::size_t& offset
    ) {
        require(data, offset, 4, "u32le");
        const uint32_t value =
            static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8u) |
            (static_cast<uint32_t>(data[offset + 2]) << 16u) |
            (static_cast<uint32_t>(data[offset + 3]) << 24u);
        offset += 4;
        return value;
    }

    static uint32_t readUVarInt(
        const std::vector<uint8_t>& data,
        std::size_t& offset
    ) {
        uint32_t value = 0;

        for (int shift = 0; shift <= 28; shift += 7) {
            require(data, offset, 1, "uvarint");
            uint8_t byte = data[offset++];

            value |= static_cast<uint32_t>(byte & 0x7f) << shift;

            if ((byte & 0x80) == 0) {
                return value;
            }
        }

        throw std::runtime_error("uvarint too long");
    }

    static int32_t readZigZagVarInt(
        const std::vector<uint8_t>& data,
        std::size_t& offset
    ) {
        const uint32_t value = readUVarInt(data, offset);
        const uint32_t decoded = (value >> 1u) ^ (0u - (value & 1u));
        return std::bit_cast<int32_t>(decoded);
    }
};

inline void writePalettedUVarInt(std::vector<uint8_t>& out, uint32_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
    }

    out.push_back(static_cast<uint8_t>(value));
}

inline void writePalettedU32LE(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

} // namespace bedrock
