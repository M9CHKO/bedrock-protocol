#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

struct PacketDecodedField {
    std::string name;
    std::string type;
    std::string value;
    std::size_t offset = 0;
    std::size_t size = 0;
};

class PacketFieldCursor {
public:
    explicit PacketFieldCursor(const std::vector<uint8_t>& data)
        : data_(data) {}

    std::size_t offset() const { return offset_; }
    const std::vector<uint8_t>& data() const { return data_; }
    void skip(std::size_t size) {
        require(size, "skip");
        offset_ += size;
    }
    void rewindTo(std::size_t offset) {
        if (offset > data_.size()) {
            throw std::runtime_error("PacketFieldCursor::rewindTo out of bounds");
        }
        offset_ = offset;
    }
    std::size_t remaining() const { return offset_ <= data_.size() ? data_.size() - offset_ : 0; }

    uint8_t u8() {
        require(1, "u8");
        return data_[offset_++];
    }

    bool boolean() {
        return u8() != 0;
    }

    uint16_t u16le() {
        require(2, "u16");
        uint16_t value =
            static_cast<uint16_t>(data_[offset_]) |
            static_cast<uint16_t>(data_[offset_ + 1] << 8);
        offset_ += 2;
        return value;
    }

    uint32_t u32le() {
        require(4, "u32");
        uint32_t value =
            static_cast<uint32_t>(data_[offset_]) |
            (static_cast<uint32_t>(data_[offset_ + 1]) << 8) |
            (static_cast<uint32_t>(data_[offset_ + 2]) << 16) |
            (static_cast<uint32_t>(data_[offset_ + 3]) << 24);
        offset_ += 4;
        return value;
    }

    int32_t i32le() {
        return static_cast<int32_t>(u32le());
    }

    uint32_t varuint32() {
        uint32_t value = 0;

        for (int shift = 0; shift <= 28; shift += 7) {
            uint8_t byte = u8();
            value |= static_cast<uint32_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) return value;
        }

        throw std::runtime_error("varuint32 too long");
    }

    uint64_t varuint64() {
        uint64_t value = 0;

        for (int shift = 0; shift <= 63; shift += 7) {
            uint8_t byte = u8();
            value |= static_cast<uint64_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) return value;
        }

        throw std::runtime_error("varuint64 too long");
    }

    int32_t zigzag32() {
        uint32_t raw = varuint32();
        return static_cast<int32_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }

    int64_t zigzag64() {
        uint64_t raw = varuint64();
        return static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }

    std::string string() {
        uint32_t len = varuint32();
        require(len, "string bytes");

        std::string value(
            data_.begin() + static_cast<std::ptrdiff_t>(offset_),
            data_.begin() + static_cast<std::ptrdiff_t>(offset_ + len)
        );

        offset_ += len;
        return value;
    }

private:
    const std::vector<uint8_t>& data_;
    std::size_t offset_ = 0;

    void require(std::size_t size, const char* what) const {
        if (offset_ + size > data_.size()) {
            throw std::runtime_error(
                std::string("not enough bytes for ") + what +
                " at offset " + std::to_string(offset_) +
                " remaining=" + std::to_string(data_.size() - offset_) +
                " need=" + std::to_string(size)
            );
        }
    }
};

class PacketFieldDecoder {
public:
    static PacketDecodedField decodeOne(
        PacketFieldCursor& cursor,
        const std::string& name,
        const std::string& rawType
    ) {
        PacketDecodedField out;
        out.name = name;
        out.type = normalizeType(rawType);
        out.offset = cursor.offset();

        if (out.type == "u8") {
            out.value = std::to_string(cursor.u8());
        } else if (out.type == "bool") {
            out.value = cursor.boolean() ? "true" : "false";
        } else if (out.type == "u16" || out.type == "li16") {
            out.value = std::to_string(cursor.u16le());
        } else if (out.type == "u32" || out.type == "li32") {
            out.value = std::to_string(cursor.u32le());
        } else if (out.type == "i32") {
            out.value = std::to_string(cursor.i32le());
        } else if (out.type == "varint" || out.type == "varuint" || out.type == "varuint32") {
            out.value = std::to_string(cursor.varuint32());
        } else if (out.type == "zigzag32") {
            out.value = std::to_string(cursor.zigzag32());
        } else if (out.type == "zigzag64") {
            out.value = std::to_string(cursor.zigzag64());
        } else if (out.type == "string") {
            out.value = cursor.string();
        } else if (isMapperType(rawType)) {
            decodeMapper(cursor, rawType, out);
        } else {
            throw std::runtime_error("unsupported packet field type: " + rawType);
        }

        out.size = cursor.offset() - out.offset;
        return out;
    }

    static std::string normalizeType(std::string type) {
        if (type.size() >= 2 && type.front() == '"' && type.back() == '"') {
            type = type.substr(1, type.size() - 2);
        }

        return type;
    }

private:
    static bool isMapperType(const std::string& rawType) {
        return rawType.find("\"mapper\"") != std::string::npos ||
               rawType.find("mapper") != std::string::npos;
    }

    static std::string extractMapperBaseType(const std::string& rawType) {
        const std::string marker = "\"type\":";
        auto pos = rawType.find(marker);
        if (pos == std::string::npos) {
            return "u8";
        }

        pos = rawType.find('"', pos + marker.size());
        if (pos == std::string::npos) {
            return "u8";
        }

        auto end = rawType.find('"', pos + 1);
        if (end == std::string::npos) {
            return "u8";
        }

        return rawType.substr(pos + 1, end - pos - 1);
    }

    static std::string extractMapperName(
        const std::string& rawType,
        const std::string& numericValue
    ) {
        const std::string marker = "\"" + numericValue + "\":";
        auto pos = rawType.find(marker);
        if (pos == std::string::npos) {
            return "";
        }

        pos = rawType.find('"', pos + marker.size());
        if (pos == std::string::npos) {
            return "";
        }

        auto end = rawType.find('"', pos + 1);
        if (end == std::string::npos) {
            return "";
        }

        return rawType.substr(pos + 1, end - pos - 1);
    }

    static void decodeMapper(
        PacketFieldCursor& cursor,
        const std::string& rawType,
        PacketDecodedField& out
    ) {
        const std::string base = extractMapperBaseType(rawType);

        std::string numeric;

        if (base == "u8") {
            numeric = std::to_string(cursor.u8());
        } else if (base == "u16" || base == "li16") {
            numeric = std::to_string(cursor.u16le());
        } else if (base == "u32" || base == "li32") {
            numeric = std::to_string(cursor.u32le());
        } else if (base == "i32") {
            numeric = std::to_string(cursor.i32le());
        } else if (base == "varint" || base == "varuint" || base == "varuint32") {
            numeric = std::to_string(cursor.varuint32());
        } else if (base == "zigzag32") {
            numeric = std::to_string(cursor.zigzag32());
        } else {
            throw std::runtime_error("unsupported mapper base type: " + base);
        }

        auto mapped = extractMapperName(rawType, numeric);
        out.type = "mapper<" + base + ">";

        if (mapped.empty()) {
            out.value = numeric;
        } else {
            out.value = numeric + "/" + mapped;
        }
    }
};

inline void writeDebugVarUint32(std::vector<uint8_t>& out, uint32_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

inline void writeDebugZigZag32(std::vector<uint8_t>& out, int32_t value) {
    uint32_t raw =
        (static_cast<uint32_t>(value) << 1) ^
        static_cast<uint32_t>(value >> 31);
    writeDebugVarUint32(out, raw);
}

inline void writeDebugString(std::vector<uint8_t>& out, const std::string& value) {
    writeDebugVarUint32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

} // namespace bedrock
