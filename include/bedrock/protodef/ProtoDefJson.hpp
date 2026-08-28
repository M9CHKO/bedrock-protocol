#pragma once

#include <bedrock/protodef/ProtoDefValue.hpp>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace bedrock {

class ProtoDefJson {
public:
    static std::string stringify(const ProtoDefValue& value) {
        switch (value.kind) {
            case ProtoDefValue::Kind::Null:
                return "null";
            case ProtoDefValue::Kind::Bool:
                return value.boolValue ? "true" : "false";
            case ProtoDefValue::Kind::Int:
                return std::to_string(value.intValue);
            case ProtoDefValue::Kind::UInt:
                return std::to_string(value.uintValue);
            case ProtoDefValue::Kind::Double: {
                std::ostringstream ss;
                ss << std::setprecision(17) << value.doubleValue;
                return ss.str();
            }
            case ProtoDefValue::Kind::String:
                return quote(value.stringValue);
            case ProtoDefValue::Kind::Bytes:
                return "{\"$bytes\":" + quote(hex(value.bytesValue)) + "}";
            case ProtoDefValue::Kind::Array: {
                std::string out = "[";
                for (std::size_t i = 0; i < value.arrayValue.size(); ++i) {
                    if (i) out += ",";
                    out += stringify(value.arrayValue[i]);
                }
                out += "]";
                return out;
            }
            case ProtoDefValue::Kind::Object: {
                std::string out = "{";
                bool first = true;
                for (const auto& [key, child] : value.objectValue) {
                    if (!first) out += ",";
                    first = false;
                    out += quote(key);
                    out += ":";
                    out += stringify(child);
                }
                out += "}";
                return out;
            }
        }

        return "null";
    }

    static ProtoDefValue parse(const std::string& json) {
        Parser parser(json);
        auto value = parser.parseValue();
        parser.skipWhitespace();
        if (!parser.done()) {
            throw std::runtime_error("ProtoDefJson trailing characters");
        }
        return value;
    }

private:
    class Parser {
    public:
        explicit Parser(const std::string& input)
            : input_(input) {}

        ProtoDefValue parseValue() {
            skipWhitespace();
            if (done()) throw std::runtime_error("ProtoDefJson unexpected end");

            char c = input_[pos_];
            if (c == '"') return ProtoDefValue::string(parseString());
            if (c == '{') return parseObject();
            if (c == '[') return parseArray();
            if (c == 't') return parseLiteral("true", ProtoDefValue::boolean(true));
            if (c == 'f') return parseLiteral("false", ProtoDefValue::boolean(false));
            if (c == 'n') return parseLiteral("null", ProtoDefValue::null());
            if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parseNumber();

            throw std::runtime_error("ProtoDefJson unexpected character");
        }

        void skipWhitespace() {
            while (!done() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
                ++pos_;
            }
        }

        bool done() const {
            return pos_ >= input_.size();
        }

    private:
        const std::string& input_;
        std::size_t pos_ = 0;

        ProtoDefValue parseLiteral(const char* literal, ProtoDefValue value) {
            std::string s(literal);
            if (input_.compare(pos_, s.size(), s) != 0) {
                throw std::runtime_error("ProtoDefJson invalid literal");
            }
            pos_ += s.size();
            return value;
        }

        ProtoDefValue parseNumber() {
            std::size_t start = pos_;
            if (input_[pos_] == '-') ++pos_;
            while (!done() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;

            bool floating = false;
            if (!done() && input_[pos_] == '.') {
                floating = true;
                ++pos_;
                while (!done() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
            }

            if (!done() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
                floating = true;
                ++pos_;
                if (!done() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
                while (!done() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
            }

            std::string raw = input_.substr(start, pos_ - start);
            if (floating) {
                return ProtoDefValue::floating(std::strtod(raw.c_str(), nullptr));
            }
            if (!raw.empty() && raw[0] == '-') {
                return ProtoDefValue::integer(std::stoll(raw));
            }
            return ProtoDefValue::uinteger(std::stoull(raw));
        }

        std::string parseString() {
            if (input_[pos_] != '"') throw std::runtime_error("ProtoDefJson expected string");
            ++pos_;

            std::string out;
            while (!done()) {
                char c = input_[pos_++];
                if (c == '"') return out;
                if (c != '\\') {
                    out.push_back(c);
                    continue;
                }
                if (done()) throw std::runtime_error("ProtoDefJson invalid escape");
                char e = input_[pos_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': appendUtf8(out, parseUnicodeEscape()); break;
                    default:
                        throw std::runtime_error("ProtoDefJson unsupported escape");
                }
            }

            throw std::runtime_error("ProtoDefJson unterminated string");
        }

        static int hexDigit(char value) {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
        }

        uint32_t parseHexCodeUnit() {
            if (pos_ + 4 > input_.size()) {
                throw std::runtime_error("ProtoDefJson incomplete unicode escape");
            }
            uint32_t value = 0;
            for (int i = 0; i < 4; ++i) {
                const auto digit = hexDigit(input_[pos_++]);
                if (digit < 0) {
                    throw std::runtime_error("ProtoDefJson invalid unicode escape");
                }
                value = (value << 4u) | static_cast<uint32_t>(digit);
            }
            return value;
        }

        uint32_t parseUnicodeEscape() {
            const auto first = parseHexCodeUnit();
            if (first >= 0xd800 && first <= 0xdbff) {
                if (pos_ + 2 > input_.size() || input_[pos_] != '\\' ||
                    input_[pos_ + 1] != 'u') {
                    throw std::runtime_error("ProtoDefJson missing low surrogate");
                }
                pos_ += 2;
                const auto second = parseHexCodeUnit();
                if (second < 0xdc00 || second > 0xdfff) {
                    throw std::runtime_error("ProtoDefJson invalid low surrogate");
                }
                return 0x10000u + ((first - 0xd800u) << 10u) + (second - 0xdc00u);
            }
            if (first >= 0xdc00 && first <= 0xdfff) {
                throw std::runtime_error("ProtoDefJson unexpected low surrogate");
            }
            return first;
        }

        static void appendUtf8(std::string& out, uint32_t value) {
            if (value <= 0x7f) {
                out.push_back(static_cast<char>(value));
            } else if (value <= 0x7ff) {
                out.push_back(static_cast<char>(0xc0u | (value >> 6u)));
                out.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
            } else if (value <= 0xffff) {
                out.push_back(static_cast<char>(0xe0u | (value >> 12u)));
                out.push_back(static_cast<char>(0x80u | ((value >> 6u) & 0x3fu)));
                out.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
            } else if (value <= 0x10ffff) {
                out.push_back(static_cast<char>(0xf0u | (value >> 18u)));
                out.push_back(static_cast<char>(0x80u | ((value >> 12u) & 0x3fu)));
                out.push_back(static_cast<char>(0x80u | ((value >> 6u) & 0x3fu)));
                out.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
            } else {
                throw std::runtime_error("ProtoDefJson unicode code point is out of range");
            }
        }

        ProtoDefValue parseArray() {
            ++pos_;
            std::vector<ProtoDefValue> values;
            skipWhitespace();
            if (!done() && input_[pos_] == ']') {
                ++pos_;
                return ProtoDefValue::array(std::move(values));
            }

            while (true) {
                values.push_back(parseValue());
                skipWhitespace();
                if (done()) throw std::runtime_error("ProtoDefJson unterminated array");
                if (input_[pos_] == ']') {
                    ++pos_;
                    return ProtoDefValue::array(std::move(values));
                }
                if (input_[pos_] != ',') throw std::runtime_error("ProtoDefJson expected comma");
                ++pos_;
            }
        }

        ProtoDefValue parseObject() {
            ++pos_;
            std::unordered_map<std::string, ProtoDefValue> object;
            skipWhitespace();
            if (!done() && input_[pos_] == '}') {
                ++pos_;
                return ProtoDefValue::object(std::move(object));
            }

            while (true) {
                skipWhitespace();
                std::string key = parseString();
                skipWhitespace();
                if (done() || input_[pos_] != ':') throw std::runtime_error("ProtoDefJson expected colon");
                ++pos_;
                object[key] = parseValue();
                skipWhitespace();
                if (done()) throw std::runtime_error("ProtoDefJson unterminated object");
                if (input_[pos_] == '}') {
                    ++pos_;
                    if (object.size() == 1) {
                        auto it = object.find("$bytes");
                        if (it != object.end() && it->second.kind == ProtoDefValue::Kind::String) {
                            return ProtoDefValue::bytes(unhex(it->second.stringValue));
                        }
                    }
                    return ProtoDefValue::object(std::move(object));
                }
                if (input_[pos_] != ',') throw std::runtime_error("ProtoDefJson expected comma");
                ++pos_;
            }
        }
    };

    static std::string quote(const std::string& input) {
        std::string out = "\"";
        for (unsigned char c : input) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        std::ostringstream ss;
                        ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                        out += ss.str();
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
            }
        }
        out += "\"";
        return out;
    }

    static std::string hex(const std::vector<uint8_t>& bytes) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (uint8_t b : bytes) {
            out.push_back(digits[(b >> 4) & 0x0f]);
            out.push_back(digits[b & 0x0f]);
        }
        return out;
    }

    static std::vector<uint8_t> unhex(const std::string& text) {
        auto hexValue = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };

        std::vector<uint8_t> out;
        if (text.size() % 2 != 0) {
            throw std::runtime_error("ProtoDefJson bytes hex length must be even");
        }
        out.reserve(text.size() / 2);
        for (std::size_t i = 0; i < text.size(); i += 2) {
            int hi = hexValue(text[i]);
            int lo = hexValue(text[i + 1]);
            if (hi < 0 || lo < 0) {
                throw std::runtime_error("ProtoDefJson invalid bytes hex");
            }
            out.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
        return out;
    }
};

inline std::string protoDefValueToJson(const ProtoDefValue& value) {
    return ProtoDefJson::stringify(value);
}

inline ProtoDefValue protoDefValueFromJson(const std::string& json) {
    return ProtoDefJson::parse(json);
}

} // namespace bedrock
