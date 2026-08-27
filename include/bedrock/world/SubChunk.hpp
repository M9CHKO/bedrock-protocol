#pragma once

#include <cstddef>
#include <cstdint>
#include <bedrock/world/PalettedStorage.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

struct SubChunkSection {
    uint8_t version = 0;
    uint8_t storageCount = 0;
    int8_t yIndex = 0;
    std::size_t offset = 0;
    std::size_t headerBytes = 0;
    std::size_t totalBytes = 0;
    std::vector<PalettedStorageInfo> storages;
};

struct SubChunkScanResult {
    std::vector<SubChunkSection> sections;
};

class SubChunkParser {
public:
    static SubChunkSection readHeaderAt(
        const std::vector<uint8_t>& data,
        std::size_t offset
    ) {
        if (offset >= data.size()) {
            throw std::runtime_error("subchunk offset outside data");
        }

        SubChunkSection out;
        out.offset = offset;
        out.version = data[offset++];

        switch (out.version) {
            case 0:
            case 8:
                require(data, offset, 1, "subchunk storage count");
                out.storageCount = data[offset++];
                break;

            case 1:
                out.storageCount = 1;
                break;

            case 9:
                require(data, offset, 2, "subchunk v9 header");
                out.storageCount = data[offset++];
                out.yIndex = static_cast<int8_t>(data[offset++]);
                break;

            default:
                throw std::runtime_error("unsupported subchunk version: " + std::to_string(out.version));
        }

        out.headerBytes = offset - out.offset;
        return out;
    }

    static SubChunkScanResult scanHeadersOnly(
        const std::vector<uint8_t>& data,
        uint32_t expectedSubChunks
    ) {
        SubChunkScanResult out;

        std::size_t offset = 0;

        for (uint32_t i = 0; i < expectedSubChunks && offset < data.size(); ++i) {
            auto section = readHeaderAt(data, offset);
            out.sections.push_back(section);
            offset += section.headerBytes;
        }

        return out;
    }

    static SubChunkScanResult scanWithStorages(
        const std::vector<uint8_t>& data,
        uint32_t expectedSubChunks,
        BedrockNbtEncoding persistentEncoding = BedrockNbtEncoding::LittleVarInt
    ) {
        SubChunkScanResult out;

        std::size_t offset = 0;

        for (uint32_t i = 0; i < expectedSubChunks && offset < data.size(); ++i) {
            auto section = readHeaderAt(data, offset);
            offset += section.headerBytes;

            section.storages.reserve(section.storageCount);

            for (uint8_t storageIndex = 0; storageIndex < section.storageCount; ++storageIndex) {
                auto storage = PalettedStorageParser::scanAt(data, offset, persistentEncoding);
                section.storages.push_back(storage);
                offset += storage.totalBytes;
            }

            section.totalBytes = offset - section.offset;
            out.sections.push_back(section);
        }

        return out;
    }

private:
    static void require(
        const std::vector<uint8_t>& data,
        std::size_t offset,
        std::size_t size,
        const char* what
    ) {
        if (offset + size > data.size()) {
            throw std::runtime_error(std::string("not enough bytes for ") + what);
        }
    }
};

} // namespace bedrock
