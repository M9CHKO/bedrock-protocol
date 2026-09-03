#include <bedrock/BinaryStream.hpp>
#include <bedrock/world/BedrockSubChunkPacket.hpp>

#include <array>
#include <cstddef>
#include <limits>

namespace bedrock {
namespace {

bool legacySingleEntrySchema(const std::string& version) {
    return version == "1.18.0";
}

std::array<int, 3> parseVersion(const std::string& version) {
    std::array<int, 3> parts {0, 0, 0};
    std::size_t offset = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto dot = version.find('.', offset);
        const auto end = dot == std::string::npos ? version.size() : dot;
        if (end == offset) {
            throw BedrockSubChunkPacketError("invalid Minecraft version: " + version);
        }
        try {
            parts[i] = std::stoi(version.substr(offset, end - offset));
        } catch (const std::exception&) {
            throw BedrockSubChunkPacketError("invalid Minecraft version: " + version);
        }
        if (dot == std::string::npos) {
            if (i != parts.size() - 1) {
                throw BedrockSubChunkPacketError("invalid Minecraft version: " + version);
            }
            break;
        }
        offset = dot + 1;
    }
    return parts;
}

bool hasRenderHeightMap(const std::string& version) {
    return parseVersion(version) >= std::array<int, 3> {1, 21, 90};
}

int32_t readZigZag32(BinaryStream& stream) {
    const uint32_t value = stream.readVarUInt();
    const uint32_t decoded = (value >> 1u) ^ (0u - (value & 1u));
    return static_cast<int32_t>(decoded);
}

void writeZigZag32(BinaryStream& stream, int32_t value) {
    const uint32_t encoded =
        (static_cast<uint32_t>(value) << 1u) ^
        static_cast<uint32_t>(value >> 31);
    stream.writeVarUInt(encoded);
}

std::vector<uint8_t> readByteArray(BinaryStream& stream) {
    const uint32_t size = stream.readVarUInt();
    if (size > stream.remaining()) {
        throw BedrockSubChunkPacketError("subchunk byte array exceeds packet payload");
    }
    return stream.readBytes(size);
}

void writeByteArray(BinaryStream& stream, const std::vector<uint8_t>& value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
        throw BedrockSubChunkPacketError("subchunk byte array is too large");
    }
    stream.writeVarUInt(static_cast<uint32_t>(value.size()));
    stream.writeBytes(value);
}

BedrockSubChunkResult readResultU8(BinaryStream& stream) {
    const uint8_t value = stream.readU8();
    if (value > static_cast<uint8_t>(BedrockSubChunkResult::SuccessAllAir)) {
        throw BedrockSubChunkPacketError("invalid subchunk result: " + std::to_string(value));
    }
    return static_cast<BedrockSubChunkResult>(value);
}

BedrockHeightMapType readHeightMapType(BinaryStream& stream) {
    const uint8_t value = stream.readU8();
    if (value > static_cast<uint8_t>(BedrockHeightMapType::AllCopied)) {
        throw BedrockSubChunkPacketError("invalid subchunk heightmap type: " + std::to_string(value));
    }
    return static_cast<BedrockHeightMapType>(value);
}

std::vector<int8_t> readHeightMap(
    BinaryStream& stream,
    BedrockHeightMapType type
) {
    if (type != BedrockHeightMapType::HasData) {
        return {};
    }
    if (stream.remaining() < 256) {
        throw BedrockSubChunkPacketError("truncated subchunk heightmap");
    }
    std::vector<int8_t> out;
    out.reserve(256);
    for (std::size_t i = 0; i < 256; ++i) {
        out.push_back(stream.readI8());
    }
    return out;
}

void writeHeightMap(
    BinaryStream& stream,
    BedrockHeightMapType type,
    const std::vector<int8_t>& value
) {
    stream.writeU8(static_cast<uint8_t>(type));
    if (type != BedrockHeightMapType::HasData) {
        if (!value.empty()) {
            throw BedrockSubChunkPacketError("heightmap data supplied for a non-data type");
        }
        return;
    }
    if (value.size() != 256) {
        throw BedrockSubChunkPacketError("subchunk heightmap must contain 256 bytes");
    }
    for (const auto item : value) {
        stream.writeI8(item);
    }
}

BedrockSubChunkPacketEntry readModernEntry(
    BinaryStream& stream,
    bool cacheEnabled,
    bool renderHeightMap
) {
    BedrockSubChunkPacketEntry entry;
    entry.dx = stream.readI8();
    entry.dy = stream.readI8();
    entry.dz = stream.readI8();
    entry.result = readResultU8(stream);
    if (!cacheEnabled || entry.result != BedrockSubChunkResult::SuccessAllAir) {
        entry.payload = readByteArray(stream);
    }
    entry.heightMapType = readHeightMapType(stream);
    entry.heightMap = readHeightMap(stream, entry.heightMapType);
    if (renderHeightMap) {
        entry.renderHeightMapType = readHeightMapType(stream);
        entry.renderHeightMap = readHeightMap(stream, entry.renderHeightMapType);
    }
    if (cacheEnabled) {
        entry.blobId = stream.readU64LE();
    }
    return entry;
}

void writeModernEntry(
    BinaryStream& stream,
    const BedrockSubChunkPacketEntry& entry,
    bool cacheEnabled,
    bool renderHeightMap
) {
    stream.writeI8(entry.dx);
    stream.writeI8(entry.dy);
    stream.writeI8(entry.dz);
    stream.writeU8(static_cast<uint8_t>(entry.result));
    if (!cacheEnabled || entry.result != BedrockSubChunkResult::SuccessAllAir) {
        writeByteArray(stream, entry.payload);
    } else if (!entry.payload.empty()) {
        throw BedrockSubChunkPacketError("cached all-air subchunk cannot carry a payload");
    }
    writeHeightMap(stream, entry.heightMapType, entry.heightMap);
    if (renderHeightMap) {
        writeHeightMap(stream, entry.renderHeightMapType, entry.renderHeightMap);
    } else if (entry.renderHeightMapType != BedrockHeightMapType::NoData ||
        !entry.renderHeightMap.empty()) {
        throw BedrockSubChunkPacketError("render heightmap is unavailable in this protocol version");
    }
    if (cacheEnabled) {
        if (!entry.blobId.has_value()) {
            throw BedrockSubChunkPacketError("cached subchunk entry is missing blob id");
        }
        stream.writeU64LE(*entry.blobId);
    } else if (entry.blobId.has_value()) {
        throw BedrockSubChunkPacketError("uncached subchunk entry cannot carry a blob id");
    }
}

} // namespace

BedrockSubChunkPacket BedrockSubChunkPacketCodec::decodePacketHeader(
    const std::vector<uint8_t>& payload,
    const std::string& minecraftVersion
) {
    BinaryStream stream(payload);
    BedrockSubChunkPacket packet;
    if (legacySingleEntrySchema(minecraftVersion)) {
        packet.dimension = readZigZag32(stream);
        packet.originX = readZigZag32(stream);
        packet.originY = readZigZag32(stream);
        packet.originZ = readZigZag32(stream);
        return packet;
    }

    packet.cacheEnabled = stream.readU8() != 0;
    packet.dimension = readZigZag32(stream);
    packet.originX = readZigZag32(stream);
    packet.originY = readZigZag32(stream);
    packet.originZ = readZigZag32(stream);
    (void) stream.readU32LE();
    return packet;
}

BedrockSubChunkPacket BedrockSubChunkPacketCodec::decodePacketPayload(
    const std::vector<uint8_t>& payload,
    const std::string& minecraftVersion
) {
    BinaryStream stream(payload);
    BedrockSubChunkPacket packet;

    if (legacySingleEntrySchema(minecraftVersion)) {
        packet.dimension = readZigZag32(stream);
        packet.originX = readZigZag32(stream);
        packet.originY = readZigZag32(stream);
        packet.originZ = readZigZag32(stream);

        BedrockSubChunkPacketEntry entry;
        entry.payload = readByteArray(stream);
        const int32_t result = readZigZag32(stream);
        if (result < 0 || result > static_cast<int32_t>(BedrockSubChunkResult::YIndexOutOfBounds)) {
            throw BedrockSubChunkPacketError("invalid legacy subchunk result");
        }
        entry.result = static_cast<BedrockSubChunkResult>(result);
        entry.heightMapType = readHeightMapType(stream);
        entry.heightMap = readHeightMap(stream, entry.heightMapType);
        packet.cacheEnabled = stream.readU8() != 0;
        if (packet.cacheEnabled) {
            entry.blobId = stream.readU64LE();
        }
        packet.entries.push_back(std::move(entry));
    } else {
        packet.cacheEnabled = stream.readU8() != 0;
        packet.dimension = readZigZag32(stream);
        packet.originX = readZigZag32(stream);
        packet.originY = readZigZag32(stream);
        packet.originZ = readZigZag32(stream);
        const uint32_t count = stream.readU32LE();
        if (count > 4096) {
            throw BedrockSubChunkPacketError("subchunk entry count exceeds safety limit");
        }
        packet.entries.reserve(count);
        const bool renderHeightMap = hasRenderHeightMap(minecraftVersion);
        for (uint32_t i = 0; i < count; ++i) {
            packet.entries.push_back(readModernEntry(
                stream,
                packet.cacheEnabled,
                renderHeightMap
            ));
        }
    }

    if (!stream.eof()) {
        throw BedrockSubChunkPacketError("subchunk packet has trailing bytes");
    }
    return packet;
}

std::vector<uint8_t> BedrockSubChunkPacketCodec::encodePacketPayload(
    const BedrockSubChunkPacket& packet,
    const std::string& minecraftVersion
) {
    BinaryStream stream;

    if (legacySingleEntrySchema(minecraftVersion)) {
        if (packet.entries.size() != 1) {
            throw BedrockSubChunkPacketError("Minecraft 1.18.0 requires one subchunk entry");
        }
        const auto& entry = packet.entries[0];
        if (entry.result == BedrockSubChunkResult::SuccessAllAir) {
            throw BedrockSubChunkPacketError("all-air result is unavailable in Minecraft 1.18.0");
        }
        writeZigZag32(stream, packet.dimension);
        writeZigZag32(stream, packet.originX);
        writeZigZag32(stream, packet.originY);
        writeZigZag32(stream, packet.originZ);
        writeByteArray(stream, entry.payload);
        writeZigZag32(stream, static_cast<int32_t>(entry.result));
        writeHeightMap(stream, entry.heightMapType, entry.heightMap);
        stream.writeU8(packet.cacheEnabled ? 1 : 0);
        if (packet.cacheEnabled) {
            if (!entry.blobId.has_value()) {
                throw BedrockSubChunkPacketError("cached subchunk entry is missing blob id");
            }
            stream.writeU64LE(*entry.blobId);
        }
        return stream.buffer();
    }

    if (packet.entries.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
        throw BedrockSubChunkPacketError("too many subchunk packet entries");
    }
    stream.writeU8(packet.cacheEnabled ? 1 : 0);
    writeZigZag32(stream, packet.dimension);
    writeZigZag32(stream, packet.originX);
    writeZigZag32(stream, packet.originY);
    writeZigZag32(stream, packet.originZ);
    stream.writeU32LE(static_cast<uint32_t>(packet.entries.size()));
    const bool renderHeightMap = hasRenderHeightMap(minecraftVersion);
    for (const auto& entry : packet.entries) {
        writeModernEntry(stream, entry, packet.cacheEnabled, renderHeightMap);
    }
    return stream.buffer();
}

} // namespace bedrock
