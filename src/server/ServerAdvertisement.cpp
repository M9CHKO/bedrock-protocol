#include <bedrock/server/ServerAdvertisement.hpp>

#include <bedrock/protocol/ProtocolDefinition.hpp>

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace bedrock {
namespace {

std::string currentUnixTimeMillis() {
    using namespace std::chrono;
    const auto now = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()
    ).count();
    return std::to_string(now);
}

std::vector<std::string> splitSemicolon(std::string_view value) {
    std::vector<std::string> parts;
    std::size_t begin = 0;

    while (true) {
        const auto delimiter = value.find(';', begin);
        if (delimiter == std::string_view::npos) {
            parts.emplace_back(value.substr(begin));
            return parts;
        }

        parts.emplace_back(value.substr(begin, delimiter - begin));
        begin = delimiter + 1;
    }
}

int digitValue(unsigned char c) noexcept {
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
    if (c >= 'a' && c <= 'z') return static_cast<int>(c - 'a') + 10;
    if (c >= 'A' && c <= 'Z') return static_cast<int>(c - 'A') + 10;
    return -1;
}

bool isAsciiWhitespace(unsigned char c) noexcept {
    switch (c) {
        case '\t':
        case '\n':
        case '\v':
        case '\f':
        case '\r':
        case ' ':
            return true;
        default:
            return false;
    }
}

std::size_t javascriptWhitespaceLength(std::string_view value,
                                       std::size_t offset) noexcept {
    if (offset >= value.size()) return 0;
    const auto byte = [&value, offset](std::size_t index) {
        return static_cast<unsigned char>(value[offset + index]);
    };

    if (isAsciiWhitespace(byte(0))) return 1;

    // Remaining ECMAScript WhiteSpace and LineTerminator code points in UTF-8.
    if (offset + 2 <= value.size() && byte(0) == 0xc2 && byte(1) == 0xa0) {
        return 2; // U+00A0
    }
    if (offset + 3 > value.size()) return 0;

    if (byte(0) == 0xe1 && byte(1) == 0x9a && byte(2) == 0x80) {
        return 3; // U+1680
    }
    if (byte(0) == 0xe2 && byte(1) == 0x80 &&
        ((byte(2) >= 0x80 && byte(2) <= 0x8a) ||
         byte(2) == 0xa8 || byte(2) == 0xa9 || byte(2) == 0xaf)) {
        return 3; // U+2000..U+200A, U+2028, U+2029, U+202F
    }
    if (byte(0) == 0xe2 && byte(1) == 0x81 && byte(2) == 0x9f) {
        return 3; // U+205F
    }
    if (byte(0) == 0xe3 && byte(1) == 0x80 && byte(2) == 0x80) {
        return 3; // U+3000
    }
    if (byte(0) == 0xef && byte(1) == 0xbb && byte(2) == 0xbf) {
        return 3; // U+FEFF
    }
    return 0;
}

// parseInt(value) with no radix, matching the behavior used by advertisement.js
// for normal Bedrock text fields: leading whitespace/sign, optional 0x prefix,
// and parsing up to the first invalid digit.
ServerAdvertisementScalar parseIntLikeJavaScript(std::string_view value) {
    std::size_t offset = 0;
    while (const auto whitespace = javascriptWhitespaceLength(value, offset)) {
        offset += whitespace;
    }

    bool negative = false;
    if (offset < value.size() &&
        (value[offset] == '+' || value[offset] == '-')) {
        negative = value[offset] == '-';
        ++offset;
    }

    int radix = 10;
    if (offset + 1 < value.size() && value[offset] == '0' &&
        (value[offset + 1] == 'x' || value[offset + 1] == 'X')) {
        radix = 16;
        offset += 2;
    }

    const std::size_t digitsBegin = offset;
    while (offset < value.size()) {
        const int digit = digitValue(static_cast<unsigned char>(value[offset]));
        if (digit < 0 || digit >= radix) break;
        ++offset;
    }

    if (offset == digitsBegin) return ServerAdvertisementScalar::nan();

    // strtod supplies the same correctly-rounded IEEE-754 result as the
    // ECMAScript integer-to-Number conversion, including overflow to Infinity.
    std::string numeric;
    if (negative) numeric.push_back('-');
    if (radix == 16) numeric += "0x";
    numeric.append(value.data() + digitsBegin, offset - digitsBegin);
    if (radix == 16) numeric += "p0";

    char* end = nullptr;
    const double result = std::strtod(numeric.c_str(), &end);
    if (!end || end == numeric.c_str()) return ServerAdvertisementScalar::nan();
    return ServerAdvertisementScalar(result);
}

std::string normalizeExponent(std::string value) {
    auto exponentPosition = value.find_first_of("eE");
    if (exponentPosition == std::string::npos) return value;

    const std::string mantissa = value.substr(0, exponentPosition);
    const char* exponentBegin = value.c_str() + exponentPosition + 1;
    char* exponentEnd = nullptr;
    const long exponentLong = std::strtol(exponentBegin, &exponentEnd, 10);
    if (exponentEnd == exponentBegin) return value;
    const int exponent = static_cast<int>(exponentLong);

    bool negative = false;
    std::string unsignedMantissa = mantissa;
    if (!unsignedMantissa.empty() && unsignedMantissa.front() == '-') {
        negative = true;
        unsignedMantissa.erase(unsignedMantissa.begin());
    }

    std::string digits;
    digits.reserve(unsignedMantissa.size());
    for (char c : unsignedMantissa) {
        if (c != '.') digits.push_back(c);
    }

    // ECMAScript uses fixed notation for exponents in [-6, 20].
    if (exponent >= -6 && exponent < 21) {
        const int decimalPosition = exponent + 1;
        std::string fixed;
        if (negative) fixed.push_back('-');

        if (decimalPosition <= 0) {
            fixed += "0.";
            fixed.append(static_cast<std::size_t>(-decimalPosition), '0');
            fixed += digits;
        } else if (static_cast<std::size_t>(decimalPosition) >= digits.size()) {
            fixed += digits;
            fixed.append(
                static_cast<std::size_t>(decimalPosition) - digits.size(),
                '0'
            );
        } else {
            fixed.append(digits.data(), static_cast<std::size_t>(decimalPosition));
            fixed.push_back('.');
            fixed.append(
                digits.data() + decimalPosition,
                digits.size() - static_cast<std::size_t>(decimalPosition)
            );
        }
        return fixed;
    }

    std::string scientific = mantissa;
    scientific.push_back('e');
    scientific.push_back(exponent < 0 ? '-' : '+');
    scientific += std::to_string(exponent < 0 ? -exponent : exponent);
    return scientific;
}

std::string numberToJavaScriptString(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return std::signbit(value) ? "-Infinity" : "Infinity";
    if (value == 0.0) return "0";

    char buffer[128] {};
    const auto result = std::to_chars(
        buffer,
        buffer + sizeof(buffer),
        value,
        std::chars_format::general
    );
    if (result.ec != std::errc()) {
        throw std::runtime_error("failed to format advertisement number");
    }

    return normalizeExponent(std::string(buffer, result.ptr));
}

std::optional<std::int64_t> stringToInteger(std::string_view value) noexcept {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end &&
           isAsciiWhitespace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    while (end > begin &&
           isAsciiWhitespace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    if (begin == end) return std::nullopt;

    std::int64_t parsed = 0;
    const auto result = std::from_chars(
        value.data() + begin,
        value.data() + end,
        parsed,
        10
    );
    if (result.ec != std::errc() || result.ptr != value.data() + end) {
        return std::nullopt;
    }
    return parsed;
}

} // namespace

ServerAdvertisementScalar::ServerAdvertisementScalar(std::nullopt_t) noexcept
    : value_(Undefined{}) {}

ServerAdvertisementScalar::ServerAdvertisementScalar(std::nullptr_t) noexcept
    : value_(Null{}) {}

ServerAdvertisementScalar::ServerAdvertisementScalar(const char* value)
    : value_(value ? Storage(std::string(value)) : Storage(Null{})) {}

ServerAdvertisementScalar::ServerAdvertisementScalar(std::string value)
    : value_(std::move(value)) {}

ServerAdvertisementScalar::ServerAdvertisementScalar(std::string_view value)
    : value_(std::string(value)) {}

ServerAdvertisementScalar::ServerAdvertisementScalar(bool value) noexcept
    : value_(value) {}

ServerAdvertisementScalar::ServerAdvertisementScalar(float value) noexcept
    : value_(static_cast<double>(value)) {}

ServerAdvertisementScalar::ServerAdvertisementScalar(double value) noexcept
    : value_(value) {}

ServerAdvertisementScalar& ServerAdvertisementScalar::operator=(std::nullopt_t) noexcept {
    value_ = Undefined{};
    return *this;
}

ServerAdvertisementScalar& ServerAdvertisementScalar::operator=(std::nullptr_t) noexcept {
    value_ = Null{};
    return *this;
}

ServerAdvertisementScalar& ServerAdvertisementScalar::operator=(const char* value) {
    value_ = value ? Storage(std::string(value)) : Storage(Null{});
    return *this;
}

ServerAdvertisementScalar& ServerAdvertisementScalar::operator=(std::string value) {
    value_ = std::move(value);
    return *this;
}

ServerAdvertisementScalar& ServerAdvertisementScalar::operator=(std::string_view value) {
    value_ = std::string(value);
    return *this;
}

ServerAdvertisementScalar& ServerAdvertisementScalar::operator=(bool value) noexcept {
    value_ = value;
    return *this;
}

ServerAdvertisementScalar& ServerAdvertisementScalar::operator=(float value) noexcept {
    value_ = static_cast<double>(value);
    return *this;
}

ServerAdvertisementScalar& ServerAdvertisementScalar::operator=(double value) noexcept {
    value_ = value;
    return *this;
}

ServerAdvertisementScalar ServerAdvertisementScalar::undefined() noexcept {
    return ServerAdvertisementScalar();
}

ServerAdvertisementScalar ServerAdvertisementScalar::null() noexcept {
    return ServerAdvertisementScalar(nullptr);
}

ServerAdvertisementScalar ServerAdvertisementScalar::nan() noexcept {
    return ServerAdvertisementScalar(std::numeric_limits<double>::quiet_NaN());
}

ServerAdvertisementScalar::Type ServerAdvertisementScalar::type() const noexcept {
    switch (value_.index()) {
        case 0: return Type::Undefined;
        case 1: return Type::Null;
        case 2: return Type::String;
        case 3: return Type::Number;
        case 4: return Type::Boolean;
        default: return Type::Undefined;
    }
}

bool ServerAdvertisementScalar::isUndefined() const noexcept {
    return std::holds_alternative<Undefined>(value_);
}

bool ServerAdvertisementScalar::isNull() const noexcept {
    return std::holds_alternative<Null>(value_);
}

bool ServerAdvertisementScalar::isString() const noexcept {
    return std::holds_alternative<std::string>(value_);
}

bool ServerAdvertisementScalar::isNumber() const noexcept {
    return std::holds_alternative<double>(value_);
}

bool ServerAdvertisementScalar::isBoolean() const noexcept {
    return std::holds_alternative<bool>(value_);
}

bool ServerAdvertisementScalar::isNaN() const noexcept {
    const auto* value = std::get_if<double>(&value_);
    return value && std::isnan(*value);
}

bool ServerAdvertisementScalar::isTruthy() const noexcept {
    if (const auto* value = std::get_if<std::string>(&value_)) {
        return !value->empty();
    }
    if (const auto* value = std::get_if<double>(&value_)) {
        return *value != 0.0 && !std::isnan(*value);
    }
    if (const auto* value = std::get_if<bool>(&value_)) return *value;
    return false;
}

const std::string* ServerAdvertisementScalar::stringValue() const noexcept {
    return std::get_if<std::string>(&value_);
}

std::optional<double> ServerAdvertisementScalar::numberValue() const noexcept {
    if (const auto* value = std::get_if<double>(&value_)) return *value;
    return std::nullopt;
}

std::optional<std::int64_t> ServerAdvertisementScalar::toInteger() const noexcept {
    if (const auto* value = std::get_if<double>(&value_)) {
        if (!std::isfinite(*value) ||
            *value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
            *value >= -static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(*value);
    }
    if (const auto* value = std::get_if<std::string>(&value_)) {
        return stringToInteger(*value);
    }
    if (const auto* value = std::get_if<bool>(&value_)) {
        return *value ? 1 : 0;
    }
    return std::nullopt;
}

std::string ServerAdvertisementScalar::toString() const {
    if (isUndefined()) return "undefined";
    if (isNull()) return "null";
    return toJoinString();
}

std::string ServerAdvertisementScalar::toJoinString() const {
    if (isUndefined() || isNull()) return {};
    if (const auto* value = std::get_if<std::string>(&value_)) return *value;
    if (const auto* value = std::get_if<double>(&value_)) {
        return numberToJavaScriptString(*value);
    }
    return std::get<bool>(value_) ? "true" : "false";
}

bool operator==(const ServerAdvertisementScalar& lhs,
                const ServerAdvertisementScalar& rhs) noexcept {
    return lhs.value_ == rhs.value_;
}

bool operator==(const ServerAdvertisementScalar& lhs,
                std::string_view rhs) noexcept {
    const auto* value = std::get_if<std::string>(&lhs.value_);
    if (!value || value->size() != rhs.size()) return false;
    return rhs.empty() ||
        std::char_traits<char>::compare(
            value->data(),
            rhs.data(),
            rhs.size()
        ) == 0;
}

bool operator==(const ServerAdvertisementScalar& lhs,
                const char* rhs) noexcept {
    if (!rhs) return lhs.isNull();
    const auto* value = std::get_if<std::string>(&lhs.value_);
    return value && value->compare(rhs) == 0;
}

bool operator==(const ServerAdvertisementScalar& lhs, double rhs) noexcept {
    const auto* value = std::get_if<double>(&lhs.value_);
    return value && *value == rhs;
}

std::ostream& operator<<(std::ostream& out,
                         const ServerAdvertisementScalar& value) {
    return out << value.toString();
}

ServerAdvertisement::ServerAdvertisement(
    ServerAdvertisementObject obj,
    ServerAdvertisementScalar port,
    std::string minecraftVersion
) : serverId(currentUnixTimeMillis()) {
    if (ProtocolDefinition::supportsVersion(minecraftVersion)) {
        protocol = ProtocolDefinition::forVersion(minecraftVersion).protocolVersion();
    } else {
        // Versions[unknown] is undefined in the JavaScript implementation.
        protocol = ServerAdvertisementScalar::undefined();
    }
    version = std::move(minecraftVersion);
    portV4 = port;
    portV6 = std::move(port);

    const auto nameIt = obj.find("name");
    if (nameIt != obj.end() && nameIt->second.isTruthy()) {
        obj["motd"] = nameIt->second;
    }
    assignObject(obj);
}

void ServerAdvertisement::assignObject(const ServerAdvertisementObject& obj) {
    const auto assign = [&obj](std::string_view key,
                               ServerAdvertisementScalar& target) {
        const auto it = obj.find(std::string(key));
        if (it != obj.end()) target = it->second;
    };

    assign("header", header);
    assign("motd", motd);
    assign("name", name);
    assign("protocol", protocol);
    assign("version", version);
    assign("playersOnline", playersOnline);
    assign("playersMax", playersMax);
    assign("serverId", serverId);
    assign("levelName", levelName);
    assign("gamemode", gamemode);
    assign("gamemodeId", gamemodeId);
    assign("portV4", portV4);
    assign("portV6", portV6);
}

ServerAdvertisement& ServerAdvertisement::fromString(std::string_view str) {
    const auto parts = splitSemicolon(str);
    const auto part = [&parts](std::size_t index) {
        return index < parts.size()
            ? ServerAdvertisementScalar(parts[index])
            : ServerAdvertisementScalar::undefined();
    };

    header = part(0);
    motd = part(1);
    protocol = part(2);
    version = part(3);
    playersOnline = part(4);
    playersMax = part(5);
    serverId = part(6);
    levelName = part(7);
    gamemode = part(8);
    gamemodeId = part(9);
    portV4 = part(10);
    portV6 = part(11);

    ServerAdvertisementScalar* numeric[] = {
        &playersOnline,
        &playersMax,
        &gamemodeId,
        &portV4,
        &portV6
    };
    for (auto* value : numeric) {
        if (value->isUndefined()) continue;
        if (!value->isTruthy()) {
            *value = ServerAdvertisementScalar::null();
            continue;
        }
        *value = parseIntLikeJavaScript(value->toString());
    }

    return *this;
}

std::string ServerAdvertisement::toString(
    const ServerAdvertisementScalar& ignoredVersion
) const {
    (void) ignoredVersion;

    const ServerAdvertisementScalar* fields[] = {
        &motd,
        &protocol,
        &version,
        &playersOnline,
        &playersMax,
        &serverId,
        &levelName,
        &gamemode,
        &gamemodeId,
        &portV4,
        &portV6
    };

    std::string result = "MCPE";
    for (const auto* field : fields) {
        result.push_back(';');
        result += field->toJoinString();
    }
    result += ";0;";
    return result;
}

std::vector<std::uint8_t> ServerAdvertisement::toBuffer(
    const ServerAdvertisementScalar& versionArgument
) const {
    const std::string string = toString(versionArgument);
    if (string.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::length_error(
            "server advertisement exceeds the UInt16BE length prefix"
        );
    }

    const auto length = static_cast<std::uint16_t>(string.size());
    std::vector<std::uint8_t> buffer;
    buffer.reserve(2 + string.size());
    buffer.push_back(static_cast<std::uint8_t>((length >> 8u) & 0xffu));
    buffer.push_back(static_cast<std::uint8_t>(length & 0xffu));
    buffer.insert(buffer.end(), string.begin(), string.end());
    return buffer;
}

std::vector<std::uint8_t> getServerName() {
    return ServerAdvertisement().toBuffer();
}

ServerAdvertisement fromServerName(std::string_view string) {
    ServerAdvertisement advertisement;
    advertisement.fromString(string);
    return advertisement;
}

} // namespace bedrock
