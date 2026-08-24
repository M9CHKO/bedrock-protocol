#include <bedrock/auth/JsRuntimeValue.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

namespace bedrock {
namespace {

constexpr double kMaximumJsDateMilliseconds = 8.64e15;

double jsTimeClip(double value) noexcept {
    if (!std::isfinite(value) ||
        std::fabs(value) > kMaximumJsDateMilliseconds) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::trunc(value);
}

struct CivilDate {
    std::int64_t year = 0;
    unsigned month = 1;
    unsigned day = 1;
};

CivilDate civilFromUnixDays(std::int64_t days) noexcept {
    // Proleptic Gregorian conversion used by ECMAScript Date. This variant
    // supports the complete TimeClip range (well beyond std::chrono::year).
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(
        days - era * 146097
    );
    const unsigned yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 -
         dayOfEra / 146096) / 365;
    std::int64_t year = static_cast<std::int64_t>(yearOfEra) + era * 400;
    const unsigned dayOfYear = dayOfEra -
        (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthPrime = (5 * dayOfYear + 2) / 153;
    const unsigned day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
    const unsigned month = monthPrime < 10
        ? monthPrime + 3
        : monthPrime - 9;
    year += month <= 2;
    return {year, month, day};
}

void appendPaddedUnsigned(
    std::string& output,
    std::uint64_t value,
    std::size_t width
) {
    std::string digits = std::to_string(value);
    if (digits.size() < width) {
        output.append(width - digits.size(), '0');
    }
    output += digits;
}

std::string jsDateToISOString(double milliseconds) {
    if (!std::isfinite(milliseconds)) {
        throw std::runtime_error("Invalid time value");
    }

    constexpr std::int64_t kMillisecondsPerDay = 86400000;
    const auto clipped = static_cast<std::int64_t>(milliseconds);
    std::int64_t days = clipped / kMillisecondsPerDay;
    std::int64_t withinDay = clipped % kMillisecondsPerDay;
    if (withinDay < 0) {
        withinDay += kMillisecondsPerDay;
        --days;
    }

    const auto civil = civilFromUnixDays(days);
    const unsigned hour = static_cast<unsigned>(withinDay / 3600000);
    withinDay %= 3600000;
    const unsigned minute = static_cast<unsigned>(withinDay / 60000);
    withinDay %= 60000;
    const unsigned second = static_cast<unsigned>(withinDay / 1000);
    const unsigned millisecond = static_cast<unsigned>(withinDay % 1000);

    std::string result;
    result.reserve(28);
    if (civil.year >= 0 && civil.year <= 9999) {
        appendPaddedUnsigned(
            result,
            static_cast<std::uint64_t>(civil.year),
            4
        );
    } else {
        result.push_back(civil.year < 0 ? '-' : '+');
        const auto magnitude = civil.year < 0
            ? static_cast<std::uint64_t>(-(civil.year + 1)) + 1U
            : static_cast<std::uint64_t>(civil.year);
        appendPaddedUnsigned(result, magnitude, 6);
    }
    result.push_back('-');
    appendPaddedUnsigned(result, civil.month, 2);
    result.push_back('-');
    appendPaddedUnsigned(result, civil.day, 2);
    result.push_back('T');
    appendPaddedUnsigned(result, hour, 2);
    result.push_back(':');
    appendPaddedUnsigned(result, minute, 2);
    result.push_back(':');
    appendPaddedUnsigned(result, second, 2);
    result.push_back('.');
    appendPaddedUnsigned(result, millisecond, 3);
    result.push_back('Z');
    return result;
}

struct DecodedUtf8 {
    std::uint32_t codePoint = 0;
    std::size_t width = 0;
};

std::optional<DecodedUtf8> decodeUtf8OrWtf8(
    std::string_view input,
    std::size_t offset
) {
    if (offset >= input.size()) return std::nullopt;
    const auto first = static_cast<std::uint8_t>(input[offset]);
    if (first < 0x80) return DecodedUtf8 { first, 1 };

    const auto continuation = [&](std::size_t index) {
        return index < input.size() &&
            (static_cast<std::uint8_t>(input[index]) & 0xc0U) == 0x80U;
    };

    if (first >= 0xc2 && first <= 0xdf && continuation(offset + 1)) {
        const auto second = static_cast<std::uint8_t>(input[offset + 1]);
        return DecodedUtf8 {
            ((first & 0x1fU) << 6) | (second & 0x3fU),
            2
        };
    }

    if (first >= 0xe0 && first <= 0xef &&
        continuation(offset + 1) && continuation(offset + 2)) {
        const auto second = static_cast<std::uint8_t>(input[offset + 1]);
        const auto third = static_cast<std::uint8_t>(input[offset + 2]);
        if ((first == 0xe0 && second < 0xa0)) return std::nullopt;
        // ED A0..BF encodes an unpaired UTF-16 surrogate in WTF-8. JavaScript
        // strings can contain it, and well-formed JSON.stringify escapes it.
        return DecodedUtf8 {
            ((first & 0x0fU) << 12) |
                ((second & 0x3fU) << 6) | (third & 0x3fU),
            3
        };
    }

    if (first >= 0xf0 && first <= 0xf4 &&
        continuation(offset + 1) && continuation(offset + 2) &&
        continuation(offset + 3)) {
        const auto second = static_cast<std::uint8_t>(input[offset + 1]);
        const auto third = static_cast<std::uint8_t>(input[offset + 2]);
        const auto fourth = static_cast<std::uint8_t>(input[offset + 3]);
        if ((first == 0xf0 && second < 0x90) ||
            (first == 0xf4 && second >= 0x90)) {
            return std::nullopt;
        }
        return DecodedUtf8 {
            ((first & 0x07U) << 18) |
                ((second & 0x3fU) << 12) |
                ((third & 0x3fU) << 6) | (fourth & 0x3fU),
            4
        };
    }

    return std::nullopt;
}

void appendUtf8OrWtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7fU) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else if (codePoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codePoint >> 12)));
        output.push_back(static_cast<char>(
            0x80U | ((codePoint >> 6) & 0x3fU)
        ));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else if (codePoint <= 0x10ffffU) {
        output.push_back(static_cast<char>(0xf0U | (codePoint >> 18)));
        output.push_back(static_cast<char>(
            0x80U | ((codePoint >> 12) & 0x3fU)
        ));
        output.push_back(static_cast<char>(
            0x80U | ((codePoint >> 6) & 0x3fU)
        ));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    } else {
        throw std::invalid_argument("JavaScript code point is out of range");
    }
}

void validateJsString(std::string_view value) {
    for (std::size_t offset = 0; offset < value.size();) {
        const auto decoded = decodeUtf8OrWtf8(value, offset);
        if (!decoded) {
            throw std::invalid_argument(
                "JavaScript string must use UTF-8 or WTF-8 encoding"
            );
        }
        offset += decoded->width;
    }
}

bool canonicalArrayIndex(
    std::string_view key,
    std::uint32_t& result
) noexcept {
    if (key.empty() || key.size() > 10) return false;
    if (key.size() > 1 && key.front() == '0') return false;

    std::uint64_t value = 0;
    for (const char character : key) {
        if (character < '0' || character > '9') return false;
        value = value * 10U + static_cast<unsigned>(character - '0');
        if (value > 0xfffffffeULL) return false;
    }

    result = static_cast<std::uint32_t>(value);
    return true;
}

std::vector<std::size_t> ecmaPropertyOrder(
    const std::vector<JsRuntimeProperty>& properties
) {
    struct NumericProperty {
        std::uint32_t value;
        std::size_t insertionIndex;
    };

    std::vector<NumericProperty> numeric;
    std::vector<std::size_t> strings;
    numeric.reserve(properties.size());
    strings.reserve(properties.size());

    for (std::size_t i = 0; i < properties.size(); ++i) {
        std::uint32_t index = 0;
        if (canonicalArrayIndex(properties[i].key, index)) {
            numeric.push_back({ index, i });
        } else {
            strings.push_back(i);
        }
    }

    std::sort(
        numeric.begin(),
        numeric.end(),
        [](const NumericProperty& left, const NumericProperty& right) {
            return left.value < right.value;
        }
    );

    std::vector<std::size_t> result;
    result.reserve(properties.size());
    for (const auto& property : numeric) {
        result.push_back(property.insertionIndex);
    }
    result.insert(result.end(), strings.begin(), strings.end());
    return result;
}

std::string fourHex(std::uint32_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(4, '0');
    for (int i = 3; i >= 0; --i) {
        result[static_cast<std::size_t>(i)] = digits[value & 0x0fU];
        value >>= 4;
    }
    return result;
}

std::string quoteJsString(std::string_view value) {
    validateJsString(value);
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');

    for (std::size_t offset = 0; offset < value.size();) {
        const auto decoded = *decodeUtf8OrWtf8(value, offset);
        const std::uint32_t codePoint = decoded.codePoint;
        offset += decoded.width;

        switch (codePoint) {
            case '"': result += "\\\""; continue;
            case '\\': result += "\\\\"; continue;
            case '\b': result += "\\b"; continue;
            case '\f': result += "\\f"; continue;
            case '\n': result += "\\n"; continue;
            case '\r': result += "\\r"; continue;
            case '\t': result += "\\t"; continue;
            default: break;
        }

        if (codePoint < 0x20U ||
            (codePoint >= 0xd800U && codePoint <= 0xdfffU)) {
            result += "\\u";
            result += fourHex(codePoint);
            continue;
        }

        appendUtf8OrWtf8(result, codePoint);
    }

    result.push_back('"');
    return result;
}

std::string jsNumberToString(double value) {
    if (!std::isfinite(value)) return "null";
    if (value == 0.0) return "0";

    const bool negative = std::signbit(value);
    const double magnitude = negative ? -value : value;
    std::array<char, 128> buffer {};
    const auto converted = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        magnitude,
        std::chars_format::scientific
    );
    if (converted.ec != std::errc {}) {
        throw std::runtime_error("failed to format JavaScript number");
    }

    const std::string scientific(buffer.data(), converted.ptr);
    const std::size_t exponentMarker = scientific.find('e');
    if (exponentMarker == std::string::npos) {
        throw std::runtime_error("scientific number has no exponent");
    }

    std::string digits;
    digits.reserve(exponentMarker);
    for (std::size_t i = 0; i < exponentMarker; ++i) {
        if (scientific[i] != '.') digits.push_back(scientific[i]);
    }

    std::size_t exponentOffset = exponentMarker + 1;
    bool negativeExponent = false;
    if (exponentOffset < scientific.size() &&
        (scientific[exponentOffset] == '+' ||
         scientific[exponentOffset] == '-')) {
        negativeExponent = scientific[exponentOffset] == '-';
        ++exponentOffset;
    }
    int exponentMagnitude = 0;
    const auto exponentResult = std::from_chars(
        scientific.data() + exponentOffset,
        scientific.data() + scientific.size(),
        exponentMagnitude
    );
    if (exponentResult.ec != std::errc {} ||
        exponentResult.ptr != scientific.data() + scientific.size()) {
        throw std::runtime_error("failed to parse formatted number exponent");
    }
    const int exponent = negativeExponent
        ? -exponentMagnitude
        : exponentMagnitude;
    const int decimalPosition = exponent + 1;
    const int digitCount = static_cast<int>(digits.size());

    std::string result;
    if (negative) result.push_back('-');

    if (digitCount <= decimalPosition && decimalPosition <= 21) {
        result += digits;
        result.append(
            static_cast<std::size_t>(decimalPosition - digitCount),
            '0'
        );
    } else if (0 < decimalPosition && decimalPosition <= 21) {
        result.append(digits, 0, static_cast<std::size_t>(decimalPosition));
        result.push_back('.');
        result.append(digits, static_cast<std::size_t>(decimalPosition));
    } else if (-6 < decimalPosition && decimalPosition <= 0) {
        result += "0.";
        result.append(static_cast<std::size_t>(-decimalPosition), '0');
        result += digits;
    } else {
        result.push_back(digits.front());
        if (digits.size() > 1) {
            result.push_back('.');
            result.append(digits, 1);
        }
        result.push_back('e');
        if (exponent >= 0) result.push_back('+');
        result += std::to_string(exponent);
    }

    return result;
}

bool sharedOwnerAndPointerEqual(
    const std::shared_ptr<const void>& left,
    const std::shared_ptr<const void>& right
) noexcept {
    return left.get() == right.get() &&
        !left.owner_before(right) && !right.owner_before(left);
}

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    JsRuntimeValue parse() {
        skipWhitespace();
        if (done()) fail("unexpected end of input");
        JsRuntimeValue result = parseValue();
        skipWhitespace();
        if (!done()) fail("trailing characters");
        return result;
    }

private:
    std::string_view input_;
    std::size_t offset_ = 0;

    [[noreturn]] void fail(const char* message) const {
        throw std::runtime_error(
            "ECMAScript JSON parse error at offset " +
            std::to_string(offset_) + ": " + message
        );
    }

    bool done() const noexcept { return offset_ >= input_.size(); }

    void skipWhitespace() noexcept {
        while (!done()) {
            const char c = input_[offset_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return;
            ++offset_;
        }
    }

    bool consume(char expected) noexcept {
        if (!done() && input_[offset_] == expected) {
            ++offset_;
            return true;
        }
        return false;
    }

    void expect(char expected, const char* message) {
        if (!consume(expected)) fail(message);
    }

    JsRuntimeValue parseValue() {
        skipWhitespace();
        if (done()) fail("unexpected end of input");
        switch (input_[offset_]) {
            case 'n': return parseLiteral("null", JsRuntimeValue::null());
            case 't': return parseLiteral("true", JsRuntimeValue::boolean(true));
            case 'f': return parseLiteral("false", JsRuntimeValue::boolean(false));
            case '"': return JsRuntimeValue::string(parseString());
            case '{': return parseObject();
            case '[': return parseArray();
            default:
                if (input_[offset_] == '-' ||
                    (input_[offset_] >= '0' && input_[offset_] <= '9')) {
                    return parseNumber();
                }
                fail("unexpected token");
        }
    }

    JsRuntimeValue parseLiteral(
        std::string_view literal,
        JsRuntimeValue value
    ) {
        if (input_.substr(offset_, literal.size()) != literal) {
            fail("invalid literal");
        }
        offset_ += literal.size();
        return value;
    }

    std::uint16_t readHex4() {
        if (input_.size() - offset_ < 4) fail("incomplete Unicode escape");
        std::uint16_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[offset_++];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') {
                digit = 10U + static_cast<unsigned>(c - 'a');
            } else if (c >= 'A' && c <= 'F') {
                digit = 10U + static_cast<unsigned>(c - 'A');
            } else {
                fail("invalid Unicode escape");
            }
            value = static_cast<std::uint16_t>((value << 4) | digit);
        }
        return value;
    }

    std::optional<std::uint16_t> peekLowSurrogate() const noexcept {
        if (input_.size() - offset_ < 6 ||
            input_[offset_] != '\\' || input_[offset_ + 1] != 'u') {
            return std::nullopt;
        }
        std::uint16_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const char c = input_[offset_ + 2 + i];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') {
                digit = 10U + static_cast<unsigned>(c - 'a');
            } else if (c >= 'A' && c <= 'F') {
                digit = 10U + static_cast<unsigned>(c - 'A');
            } else {
                return std::nullopt;
            }
            value = static_cast<std::uint16_t>((value << 4) | digit);
        }
        if (value < 0xdc00U || value > 0xdfffU) return std::nullopt;
        return value;
    }

    std::string parseString() {
        expect('"', "expected string");
        std::string result;
        while (!done()) {
            const auto byte = static_cast<std::uint8_t>(input_[offset_]);
            if (byte == '"') {
                ++offset_;
                return result;
            }
            if (byte < 0x20U) fail("unescaped control character in string");

            if (byte == '\\') {
                ++offset_;
                if (done()) fail("incomplete escape sequence");
                const char escape = input_[offset_++];
                switch (escape) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    case 'u': {
                        const std::uint16_t first = readHex4();
                        if (first >= 0xd800U && first <= 0xdbffU) {
                            if (const auto second = peekLowSurrogate()) {
                                offset_ += 6;
                                const std::uint32_t codePoint = 0x10000U +
                                    ((static_cast<std::uint32_t>(first) - 0xd800U) << 10) +
                                    (static_cast<std::uint32_t>(*second) - 0xdc00U);
                                appendUtf8OrWtf8(result, codePoint);
                            } else {
                                appendUtf8OrWtf8(result, first);
                            }
                        } else {
                            appendUtf8OrWtf8(result, first);
                        }
                        break;
                    }
                    default: fail("invalid escape sequence");
                }
                continue;
            }

            if (byte < 0x80U) {
                result.push_back(static_cast<char>(byte));
                ++offset_;
                continue;
            }

            const auto decoded = decodeUtf8OrWtf8(input_, offset_);
            if (!decoded) fail("invalid UTF-8 in string");
            result.append(input_, offset_, decoded->width);
            offset_ += decoded->width;
        }
        fail("unterminated string");
    }

    JsRuntimeValue parseObject() {
        expect('{', "expected object");
        JsRuntimeValue result = JsRuntimeValue::object();
        skipWhitespace();
        if (consume('}')) return result;

        while (true) {
            skipWhitespace();
            if (done() || input_[offset_] != '"') {
                fail("object key must be a string");
            }
            std::string key = parseString();
            skipWhitespace();
            expect(':', "expected colon after object key");
            result.set(std::move(key), parseValue());
            skipWhitespace();
            if (consume('}')) return result;
            expect(',', "expected comma between object properties");
            skipWhitespace();
            if (!done() && input_[offset_] == '}') {
                fail("trailing comma in object");
            }
        }
    }

    JsRuntimeValue parseArray() {
        expect('[', "expected array");
        JsRuntimeValue result = JsRuntimeValue::array();
        skipWhitespace();
        if (consume(']')) return result;

        while (true) {
            result.push(parseValue());
            skipWhitespace();
            if (consume(']')) return result;
            expect(',', "expected comma between array elements");
            skipWhitespace();
            if (!done() && input_[offset_] == ']') {
                fail("trailing comma in array");
            }
        }
    }

    JsRuntimeValue parseNumber() {
        const std::size_t start = offset_;
        consume('-');
        if (done()) fail("incomplete number");

        if (consume('0')) {
            if (!done() && input_[offset_] >= '0' && input_[offset_] <= '9') {
                fail("leading zero in number");
            }
        } else {
            if (input_[offset_] < '1' || input_[offset_] > '9') {
                fail("invalid integer part");
            }
            while (!done() && input_[offset_] >= '0' && input_[offset_] <= '9') {
                ++offset_;
            }
        }

        if (consume('.')) {
            if (done() || input_[offset_] < '0' || input_[offset_] > '9') {
                fail("fraction requires a digit");
            }
            while (!done() && input_[offset_] >= '0' && input_[offset_] <= '9') {
                ++offset_;
            }
        }

        if (!done() && (input_[offset_] == 'e' || input_[offset_] == 'E')) {
            ++offset_;
            if (!done() && (input_[offset_] == '+' || input_[offset_] == '-')) {
                ++offset_;
            }
            if (done() || input_[offset_] < '0' || input_[offset_] > '9') {
                fail("exponent requires a digit");
            }
            while (!done() && input_[offset_] >= '0' && input_[offset_] <= '9') {
                ++offset_;
            }
        }

        const std::string raw(input_.substr(start, offset_ - start));
        char* end = nullptr;
        const double value = std::strtod(raw.c_str(), &end);
        if (end != raw.c_str() + raw.size()) fail("invalid number");
        return JsRuntimeValue::number(value);
    }
};

struct JsonStringifyState {
    std::unordered_set<const JsRuntimeObject*> activeObjects;
    std::unordered_set<const JsRuntimeArray*> activeArrays;
    std::unordered_set<const JsRuntimeMap*> activeMaps;
};

template <typename Set, typename Pointer>
class ActiveJsonNode {
public:
    ActiveJsonNode(Set& values, Pointer value)
        : values_(values), value_(value) {
        if (!values_.insert(value_).second) {
            throw std::runtime_error("Converting circular structure to JSON");
        }
    }

    ~ActiveJsonNode() { values_.erase(value_); }

private:
    Set& values_;
    Pointer value_;
};

std::optional<std::string> stringifyValue(
    const JsRuntimeValue& value,
    JsonStringifyState& state
) {
    switch (value.kind()) {
        case JsRuntimeValue::Kind::Undefined:
        case JsRuntimeValue::Kind::Function:
        case JsRuntimeValue::Kind::Opaque:
            return std::nullopt;
        case JsRuntimeValue::Kind::Null:
            return "null";
        case JsRuntimeValue::Kind::Bool:
            return value.boolValue() ? "true" : "false";
        case JsRuntimeValue::Kind::Number:
            return jsNumberToString(value.numberValue());
        case JsRuntimeValue::Kind::String:
            return quoteJsString(value.stringValue());
        case JsRuntimeValue::Kind::Date:
            return value.dateIsValid()
                ? std::optional<std::string>(
                    quoteJsString(value.dateIsoString())
                )
                : std::optional<std::string>("null");
        case JsRuntimeValue::Kind::Object: {
            const auto& object = value.objectNode();
            ActiveJsonNode guard(state.activeObjects, object.get());
            std::string result = "{";
            bool first = true;
            for (const auto& property : object->ownProperties()) {
                auto child = stringifyValue(property.value, state);
                if (!child) continue;
                if (!first) result.push_back(',');
                first = false;
                result += quoteJsString(property.key);
                result.push_back(':');
                result += *child;
            }
            result.push_back('}');
            return result;
        }
        case JsRuntimeValue::Kind::Array: {
            const auto& array = value.arrayNode();
            ActiveJsonNode guard(state.activeArrays, array.get());
            std::string result = "[";
            const auto& elements = array->elements();
            for (std::size_t i = 0; i < elements.size(); ++i) {
                if (i) result.push_back(',');
                if (!elements[i]) {
                    result += "null";
                    continue;
                }
                auto child = stringifyValue(*elements[i], state);
                result += child ? *child : "null";
            }
            result.push_back(']');
            return result;
        }
        case JsRuntimeValue::Kind::Map: {
            const auto& map = value.mapNode();
            ActiveJsonNode guard(state.activeMaps, map.get());
            std::string result = "{";
            bool first = true;
            // Map entries are internal slots, not enumerable properties.
            for (const auto& property : map->ownProperties()) {
                auto child = stringifyValue(property.value, state);
                if (!child) continue;
                if (!first) result.push_back(',');
                first = false;
                result += quoteJsString(property.key);
                result.push_back(':');
                result += *child;
            }
            result.push_back('}');
            return result;
        }
    }

    return std::nullopt;
}

} // namespace

JsRuntimeValue JsRuntimeValue::undefined() noexcept {
    return {};
}

JsRuntimeValue JsRuntimeValue::null() noexcept {
    JsRuntimeValue result;
    result.kind_ = Kind::Null;
    return result;
}

JsRuntimeValue JsRuntimeValue::boolean(bool value) noexcept {
    JsRuntimeValue result;
    result.kind_ = Kind::Bool;
    result.boolValue_ = value;
    return result;
}

JsRuntimeValue JsRuntimeValue::number(double value) noexcept {
    JsRuntimeValue result;
    result.kind_ = Kind::Number;
    result.numberValue_ = value;
    return result;
}

JsRuntimeValue JsRuntimeValue::string(std::string value) {
    validateJsString(value);
    JsRuntimeValue result;
    result.kind_ = Kind::String;
    result.stringValue_ = std::move(value);
    return result;
}

JsRuntimeValue JsRuntimeValue::string(std::string_view value) {
    return string(std::string(value));
}

JsRuntimeValue JsRuntimeValue::string(const char* value) {
    return string(std::string(value ? value : ""));
}

JsRuntimeValue JsRuntimeValue::object() {
    JsRuntimeValue result;
    result.kind_ = Kind::Object;
    result.objectValue_ = std::make_shared<JsRuntimeObject>();
    return result;
}

JsRuntimeValue JsRuntimeValue::object(
    std::initializer_list<JsRuntimeProperty> properties
) {
    JsRuntimeValue result = object();
    for (const auto& property : properties) {
        result.set(property.key, property.value);
    }
    return result;
}

JsRuntimeValue JsRuntimeValue::array() {
    JsRuntimeValue result;
    result.kind_ = Kind::Array;
    result.arrayValue_ = std::make_shared<JsRuntimeArray>();
    return result;
}

JsRuntimeValue JsRuntimeValue::array(
    std::initializer_list<JsRuntimeValue> elements
) {
    return array(std::vector<JsRuntimeValue>(elements));
}

JsRuntimeValue JsRuntimeValue::array(std::vector<JsRuntimeValue> elements) {
    JsRuntimeValue result = array();
    for (auto& element : elements) result.push(std::move(element));
    return result;
}

JsRuntimeValue JsRuntimeValue::date(double millisecondsSinceEpoch) {
    JsRuntimeValue result;
    result.kind_ = Kind::Date;
    result.dateValue_ = std::make_shared<JsRuntimeDate>(
        millisecondsSinceEpoch
    );
    return result;
}

JsRuntimeValue JsRuntimeValue::map() {
    JsRuntimeValue result;
    result.kind_ = Kind::Map;
    result.mapValue_ = std::make_shared<JsRuntimeMap>();
    return result;
}

bool JsRuntimeValue::boolValue() const {
    if (kind_ != Kind::Bool) throw std::logic_error("JS value is not Boolean");
    return boolValue_;
}

double JsRuntimeValue::numberValue() const {
    if (kind_ != Kind::Number) throw std::logic_error("JS value is not Number");
    return numberValue_;
}

const std::string& JsRuntimeValue::stringValue() const {
    if (kind_ != Kind::String) throw std::logic_error("JS value is not String");
    return stringValue_;
}

std::string& JsRuntimeValue::stringValue() {
    if (kind_ != Kind::String) throw std::logic_error("JS value is not String");
    return stringValue_;
}

const JsRuntimeValue::ObjectPtr& JsRuntimeValue::objectNode() const {
    if (kind_ != Kind::Object) throw std::logic_error("JS value is not Object");
    return objectValue_;
}

JsRuntimeValue::ObjectPtr& JsRuntimeValue::objectNode() {
    if (kind_ != Kind::Object) throw std::logic_error("JS value is not Object");
    return objectValue_;
}

const JsRuntimeValue::ArrayPtr& JsRuntimeValue::arrayNode() const {
    if (kind_ != Kind::Array) throw std::logic_error("JS value is not Array");
    return arrayValue_;
}

JsRuntimeValue::ArrayPtr& JsRuntimeValue::arrayNode() {
    if (kind_ != Kind::Array) throw std::logic_error("JS value is not Array");
    return arrayValue_;
}

const JsRuntimeValue::DatePtr& JsRuntimeValue::dateNode() const {
    if (kind_ != Kind::Date) throw std::logic_error("JS value is not Date");
    return dateValue_;
}

JsRuntimeValue::DatePtr& JsRuntimeValue::dateNode() {
    if (kind_ != Kind::Date) throw std::logic_error("JS value is not Date");
    return dateValue_;
}

const JsRuntimeValue::MapPtr& JsRuntimeValue::mapNode() const {
    if (kind_ != Kind::Map) throw std::logic_error("JS value is not Map");
    return mapValue_;
}

JsRuntimeValue::MapPtr& JsRuntimeValue::mapNode() {
    if (kind_ != Kind::Map) throw std::logic_error("JS value is not Map");
    return mapValue_;
}

double JsRuntimeValue::dateMilliseconds() const {
    return dateNode()->milliseconds();
}

bool JsRuntimeValue::dateIsValid() const {
    return dateNode()->valid();
}

std::string JsRuntimeValue::dateIsoString() const {
    return dateNode()->toISOString();
}

const JsRuntimeValue* JsRuntimeValue::get(std::string_view key) const {
    if (kind_ == Kind::Object) return objectValue_->get(key);
    if (kind_ == Kind::Array) return arrayValue_->get(key);
    if (kind_ == Kind::Function && functionProperties_) {
        return functionProperties_->get(key);
    }
    if (kind_ == Kind::Map) return mapValue_->getProperty(key);
    return nullptr;
}

JsRuntimeValue* JsRuntimeValue::get(std::string_view key) {
    if (kind_ == Kind::Object) return objectValue_->get(key);
    if (kind_ == Kind::Array) return arrayValue_->get(key);
    if (kind_ == Kind::Function && functionProperties_) {
        return functionProperties_->get(key);
    }
    if (kind_ == Kind::Map) return mapValue_->getProperty(key);
    return nullptr;
}

bool JsRuntimeValue::hasOwn(std::string_view key) const {
    if (kind_ == Kind::Object) return objectValue_->hasOwn(key);
    if (kind_ == Kind::Array) return arrayValue_->hasOwn(key);
    if (kind_ == Kind::Function && functionProperties_) {
        return functionProperties_->hasOwn(key);
    }
    if (kind_ == Kind::Map) return mapValue_->hasOwnProperty(key);
    return false;
}

void JsRuntimeValue::set(std::string key, JsRuntimeValue value) {
    if (kind_ == Kind::Object) {
        objectValue_->set(std::move(key), std::move(value));
        return;
    }
    if (kind_ == Kind::Array) {
        arrayValue_->set(std::move(key), std::move(value));
        return;
    }
    if (kind_ == Kind::Map) {
        mapValue_->setProperty(std::move(key), std::move(value));
        return;
    }
    throw std::logic_error("cannot set a property on this JS value kind");
}

std::vector<JsRuntimeProperty> JsRuntimeValue::ownProperties() const {
    if (kind_ == Kind::Object) return objectValue_->ownProperties();
    if (kind_ == Kind::Array) return arrayValue_->ownProperties();
    if (kind_ == Kind::Map) return mapValue_->ownProperties();
    return {};
}

const JsRuntimeValue* JsRuntimeValue::get(std::size_t index) const {
    return kind_ == Kind::Array ? arrayValue_->get(index) : nullptr;
}

JsRuntimeValue* JsRuntimeValue::get(std::size_t index) {
    return kind_ == Kind::Array ? arrayValue_->get(index) : nullptr;
}

void JsRuntimeValue::set(std::size_t index, JsRuntimeValue value) {
    if (kind_ != Kind::Array) {
        throw std::logic_error("cannot set an array index on this JS value kind");
    }
    arrayValue_->set(index, std::move(value));
}

void JsRuntimeValue::push(JsRuntimeValue value) {
    if (kind_ != Kind::Array) {
        throw std::logic_error("cannot push to this JS value kind");
    }
    arrayValue_->push(std::move(value));
}

std::size_t JsRuntimeValue::length() const {
    if (kind_ != Kind::Array) throw std::logic_error("JS value is not Array");
    return arrayValue_->length();
}

JsRuntimeValue& JsRuntimeValue::mapSet(
    JsRuntimeValue key,
    JsRuntimeValue value
) {
    if (kind_ != Kind::Map) {
        throw std::logic_error("JS value is not Map");
    }
    mapValue_->set(std::move(key), std::move(value));
    return *this;
}

const JsRuntimeValue* JsRuntimeValue::mapGet(
    const JsRuntimeValue& key
) const {
    return kind_ == Kind::Map ? mapValue_->get(key) : nullptr;
}

JsRuntimeValue* JsRuntimeValue::mapGet(const JsRuntimeValue& key) {
    return kind_ == Kind::Map ? mapValue_->get(key) : nullptr;
}

bool JsRuntimeValue::mapErase(const JsRuntimeValue& key) {
    if (kind_ != Kind::Map) {
        throw std::logic_error("JS value is not Map");
    }
    return mapValue_->erase(key);
}

std::size_t JsRuntimeValue::mapSize() const {
    if (kind_ != Kind::Map) {
        throw std::logic_error("JS value is not Map");
    }
    return mapValue_->size();
}

std::type_index JsRuntimeValue::functionSignature() const noexcept {
    return functionValue_ ? functionValue_->signatureType() : typeid(void);
}

std::type_index JsRuntimeValue::opaqueType() const noexcept {
    return opaqueValue_ ? opaqueValue_->valueType() : typeid(void);
}

bool JsRuntimeValue::truthy() const noexcept {
    switch (kind_) {
        case Kind::Undefined:
        case Kind::Null:
            return false;
        case Kind::Bool:
            return boolValue_;
        case Kind::Number:
            return numberValue_ != 0.0 && !std::isnan(numberValue_);
        case Kind::String:
            return !stringValue_.empty();
        case Kind::Object:
        case Kind::Array:
        case Kind::Date:
        case Kind::Map:
        case Kind::Function:
        case Kind::Opaque:
            return true;
    }
    return false;
}

bool JsRuntimeValue::sharesIdentityWith(
    const JsRuntimeValue& other
) const noexcept {
    if (kind_ != other.kind_) return false;
    switch (kind_) {
        case Kind::Object:
            return objectValue_ == other.objectValue_;
        case Kind::Array:
            return arrayValue_ == other.arrayValue_;
        case Kind::Date:
            return dateValue_ == other.dateValue_;
        case Kind::Map:
            return mapValue_ == other.mapValue_;
        case Kind::Function:
            return functionValue_ == other.functionValue_;
        case Kind::Opaque: {
            if (opaqueValue_ == other.opaqueValue_) return true;
            if (!opaqueValue_ || !other.opaqueValue_) return false;
            const auto left = opaqueValue_->identity();
            const auto right = other.opaqueValue_->identity();
            if (!left || !right) return false;
            return sharedOwnerAndPointerEqual(left, right);
        }
        case Kind::Undefined:
        case Kind::Null:
        case Kind::Bool:
        case Kind::Number:
        case Kind::String:
            return false;
    }
    return false;
}

bool JsRuntimeValue::strictlyEquals(
    const JsRuntimeValue& other
) const noexcept {
    if (kind_ != other.kind_) return false;
    switch (kind_) {
        case Kind::Undefined:
        case Kind::Null:
            return true;
        case Kind::Bool:
            return boolValue_ == other.boolValue_;
        case Kind::Number:
            return numberValue_ == other.numberValue_;
        case Kind::String:
            return stringValue_ == other.stringValue_;
        case Kind::Object:
        case Kind::Array:
        case Kind::Date:
        case Kind::Map:
        case Kind::Function:
        case Kind::Opaque:
            return sharesIdentityWith(other);
    }
    return false;
}

const JsRuntimeValue* JsRuntimeObject::get(std::string_view key) const {
    const auto found = indices_.find(std::string(key));
    return found == indices_.end() ? nullptr : &properties_[found->second].value;
}

JsRuntimeValue* JsRuntimeObject::get(std::string_view key) {
    const auto found = indices_.find(std::string(key));
    return found == indices_.end() ? nullptr : &properties_[found->second].value;
}

bool JsRuntimeObject::hasOwn(std::string_view key) const {
    return indices_.find(std::string(key)) != indices_.end();
}

void JsRuntimeObject::set(std::string key, JsRuntimeValue value) {
    validateJsString(key);
    const auto found = indices_.find(key);
    if (found != indices_.end()) {
        properties_[found->second].value = std::move(value);
        return;
    }
    const std::size_t index = properties_.size();
    properties_.emplace_back(std::move(key), std::move(value));
    indices_[properties_.back().key] = index;
}

bool JsRuntimeObject::erase(std::string_view key) {
    const auto found = indices_.find(std::string(key));
    if (found == indices_.end()) return false;
    const std::size_t removed = found->second;
    indices_.erase(found);
    properties_.erase(properties_.begin() + static_cast<std::ptrdiff_t>(removed));
    for (std::size_t i = removed; i < properties_.size(); ++i) {
        indices_[properties_[i].key] = i;
    }
    return true;
}

std::vector<JsRuntimeProperty> JsRuntimeObject::ownProperties() const {
    std::vector<JsRuntimeProperty> result;
    result.reserve(properties_.size());
    for (const std::size_t index : ecmaPropertyOrder(properties_)) {
        result.push_back(properties_[index]);
    }
    return result;
}

const JsRuntimeValue* JsRuntimeArray::get(std::size_t index) const {
    if (index >= elements_.size() || !elements_[index]) return nullptr;
    return &*elements_[index];
}

JsRuntimeValue* JsRuntimeArray::get(std::size_t index) {
    if (index >= elements_.size() || !elements_[index]) return nullptr;
    return &*elements_[index];
}

void JsRuntimeArray::set(std::size_t index, JsRuntimeValue value) {
    if (index >= elements_.max_size()) {
        throw std::length_error("JavaScript array index exceeds native capacity");
    }
    if (index >= elements_.size()) elements_.resize(index + 1);
    elements_[index] = std::move(value);
    lengthValue_ = JsRuntimeValue::number(static_cast<double>(elements_.size()));
}

bool JsRuntimeArray::erase(std::size_t index) {
    if (index >= elements_.size() || !elements_[index]) return false;
    elements_[index].reset();
    return true;
}

void JsRuntimeArray::push(JsRuntimeValue value) {
    elements_.emplace_back(std::move(value));
    lengthValue_ = JsRuntimeValue::number(static_cast<double>(elements_.size()));
}

const JsRuntimeValue* JsRuntimeArray::get(std::string_view key) const {
    if (key == "length") return &lengthValue_;
    std::uint32_t index = 0;
    if (canonicalArrayIndex(key, index)) return get(index);
    return namedProperties_.get(key);
}

JsRuntimeValue* JsRuntimeArray::get(std::string_view key) {
    if (key == "length") return &lengthValue_;
    std::uint32_t index = 0;
    if (canonicalArrayIndex(key, index)) return get(index);
    return namedProperties_.get(key);
}

bool JsRuntimeArray::hasOwn(std::string_view key) const {
    if (key == "length") return true;
    std::uint32_t index = 0;
    if (canonicalArrayIndex(key, index)) return get(index) != nullptr;
    return namedProperties_.hasOwn(key);
}

void JsRuntimeArray::set(std::string key, JsRuntimeValue value) {
    if (key == "length") {
        if (!value.isNumber()) {
            throw std::range_error("Invalid array length");
        }
        const double requested = value.numberValue();
        if (!std::isfinite(requested) || requested < 0.0 ||
            requested > 4294967295.0 || std::floor(requested) != requested) {
            throw std::range_error("Invalid array length");
        }
        const auto length = static_cast<std::size_t>(requested);
        if (length > elements_.max_size()) {
            throw std::length_error("JavaScript array length exceeds native capacity");
        }
        elements_.resize(length);
        lengthValue_ = JsRuntimeValue::number(requested);
        return;
    }

    std::uint32_t index = 0;
    if (canonicalArrayIndex(key, index)) {
        set(static_cast<std::size_t>(index), std::move(value));
        return;
    }
    namedProperties_.set(std::move(key), std::move(value));
}

bool JsRuntimeArray::erase(std::string_view key) {
    if (key == "length") return false;
    std::uint32_t index = 0;
    if (canonicalArrayIndex(key, index)) return erase(index);
    return namedProperties_.erase(key);
}

std::vector<JsRuntimeProperty> JsRuntimeArray::ownProperties() const {
    std::vector<JsRuntimeProperty> result;
    for (std::size_t i = 0; i < elements_.size(); ++i) {
        if (elements_[i]) {
            result.emplace_back(std::to_string(i), *elements_[i]);
        }
    }
    auto named = namedProperties_.ownProperties();
    result.insert(
        result.end(),
        std::make_move_iterator(named.begin()),
        std::make_move_iterator(named.end())
    );
    return result;
}

JsRuntimeDate::JsRuntimeDate(double millisecondsSinceEpoch) noexcept
    : milliseconds_(jsTimeClip(millisecondsSinceEpoch)) {}

bool JsRuntimeDate::valid() const noexcept {
    return std::isfinite(milliseconds_);
}

std::string JsRuntimeDate::toISOString() const {
    return jsDateToISOString(milliseconds_);
}

void JsRuntimeMap::updateSize() noexcept {
    sizeValue_ = JsRuntimeValue::number(static_cast<double>(entries_.size()));
}

JsRuntimeValue& JsRuntimeMap::set(
    JsRuntimeValue key,
    JsRuntimeValue value
) {
    for (auto& entry : entries_) {
        if (entry.key.strictlyEquals(key)) {
            entry.value = std::move(value);
            return entry.value;
        }
    }
    entries_.emplace_back(std::move(key), std::move(value));
    updateSize();
    return entries_.back().value;
}

const JsRuntimeValue* JsRuntimeMap::get(const JsRuntimeValue& key) const {
    for (const auto& entry : entries_) {
        if (entry.key.strictlyEquals(key)) return &entry.value;
    }
    return nullptr;
}

JsRuntimeValue* JsRuntimeMap::get(const JsRuntimeValue& key) {
    for (auto& entry : entries_) {
        if (entry.key.strictlyEquals(key)) return &entry.value;
    }
    return nullptr;
}

bool JsRuntimeMap::erase(const JsRuntimeValue& key) {
    const auto found = std::find_if(
        entries_.begin(),
        entries_.end(),
        [&](const JsRuntimeMapEntry& entry) {
            return entry.key.strictlyEquals(key);
        }
    );
    if (found == entries_.end()) return false;
    entries_.erase(found);
    updateSize();
    return true;
}

void JsRuntimeMap::clear() {
    entries_.clear();
    updateSize();
}

const JsRuntimeValue* JsRuntimeMap::getProperty(
    std::string_view key
) const {
    if (const auto* own = properties_.get(key)) return own;
    return key == "size" ? &sizeValue_ : nullptr;
}

JsRuntimeValue* JsRuntimeMap::getProperty(std::string_view key) {
    if (auto* own = properties_.get(key)) return own;
    return key == "size" ? &sizeValue_ : nullptr;
}

bool JsRuntimeMap::hasOwnProperty(std::string_view key) const {
    return properties_.hasOwn(key);
}

void JsRuntimeMap::setProperty(
    std::string key,
    JsRuntimeValue value
) {
    properties_.set(std::move(key), std::move(value));
}

std::vector<JsRuntimeProperty> JsRuntimeMap::ownProperties() const {
    return properties_.ownProperties();
}

JsRuntimeValue JsRuntimeJson::parse(std::string_view json) {
    return JsonParser(json).parse();
}

std::optional<std::string> JsRuntimeJson::stringify(
    const JsRuntimeValue& value
) {
    JsonStringifyState state;
    return stringifyValue(value, state);
}

} // namespace bedrock
