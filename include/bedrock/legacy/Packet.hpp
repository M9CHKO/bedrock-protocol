#pragma once

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace bedrock::legacy {

// Legacy packet-dump object retained for the old session helper. The public
// bedrock::Packet name belongs to the canonical API exported by bedrock.hpp.
class Packet {
public:
    std::string name;
    std::map<std::string, std::string> params;
    std::size_t size = 0;
    std::vector<uint8_t> buffer;
    std::vector<uint8_t> fullBuffer;
    std::string decodeError;

    std::string& operator[](const std::string& key) { return params[key]; }

    const std::string& get(const std::string& key) const {
        static const std::string empty;
        auto it = params.find(key);
        return it == params.end() ? empty : it->second;
    }

    bool has(const std::string& key) const {
        return params.find(key) != params.end();
    }

private:
    static std::string quote(const std::string& value) {
        if (value == "true" || value == "false") return value;
        if (value == "<switch>" || value == "undefined") return "undefined";
        if (value.rfind("bytes:", 0) == 0) return "<Buffer " + value + ">";

        bool number = !value.empty();
        for (char c : value) {
            if (!(c == '-' || c == '+' || c == '.' ||
                  (c >= '0' && c <= '9'))) {
                number = false;
                break;
            }
        }
        if (number) return value;

        std::string out = "'";
        for (unsigned char uc : value) {
            char c = static_cast<char>(uc);
            if (c == '\\' || c == '\'') {
                out.push_back('\\');
                out.push_back(c);
            } else if (uc < 32 || uc == 127) {
                out += "\\x";
                const char* hex = "0123456789abcdef";
                out.push_back(hex[(uc >> 4) & 0x0f]);
                out.push_back(hex[uc & 0x0f]);
            } else {
                out.push_back(c);
            }
        }
        out.push_back('\'');
        return out;
    }

    static std::string bufferToString(
        const std::vector<uint8_t>& bytes,
        std::size_t max = 50
    ) {
        std::ostringstream out;
        out << "<Buffer ";
        std::size_t shown = bytes.size() < max ? bytes.size() : max;
        for (std::size_t i = 0; i < shown; ++i) {
            if (i) out << " ";
            out << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(bytes[i]);
        }
        if (bytes.size() > shown) {
            out << std::dec << " ... " << (bytes.size() - shown)
                << " more bytes";
        }
        out << ">";
        return out.str();
    }

    friend std::ostream& operator<<(std::ostream& os, const Packet& packet) {
        os << "{\n";
        os << "  data: {\n";
        os << "    name: '" << packet.name << "',\n";
        os << "    params: {\n";
        for (const auto& [key, value] : packet.params) {
            if (value == "undefined" || value == "<switch>") continue;
            os << "      " << key << ": " << quote(value) << ",\n";
        }
        os << "    }\n";
        os << "  },\n";
        os << "  metadata: { size: " << packet.size << " },\n";
        os << "  buffer: " << bufferToString(packet.buffer) << ",\n";
        os << "  fullBuffer: " << bufferToString(packet.fullBuffer);
        if (!packet.decodeError.empty()) {
            os << ",\n";
            os << "  decodeError: '" << packet.decodeError << "'";
        }
        os << "\n}";
        return os;
    }
};

} // namespace bedrock::legacy
