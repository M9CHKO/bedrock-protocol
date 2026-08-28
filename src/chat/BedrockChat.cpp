#include <bedrock/chat/BedrockChat.hpp>

#include <bedrock/protodef/ProtoDefJson.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace bedrock {

struct BedrockLanguageData {
    std::unordered_map<std::string, std::string> entries;
};

namespace {

constexpr std::string_view Section = "\xc2\xa7";

const ProtoDefValue* field(const ProtoDefValue& value, std::string_view name) {
    if (value.kind != ProtoDefValue::Kind::Object) return nullptr;
    return value.get(std::string(name));
}

bool isNumber(const ProtoDefValue& value) {
    return value.kind == ProtoDefValue::Kind::Int ||
        value.kind == ProtoDefValue::Kind::UInt ||
        value.kind == ProtoDefValue::Kind::Double;
}

bool isTextScalar(const ProtoDefValue& value) {
    return value.kind == ProtoDefValue::Kind::String || isNumber(value);
}

std::string numberString(const ProtoDefValue& value) {
    if (value.kind == ProtoDefValue::Kind::Int) return std::to_string(value.intValue);
    if (value.kind == ProtoDefValue::Kind::UInt) return std::to_string(value.uintValue);
    if (value.kind != ProtoDefValue::Kind::Double) return {};

    std::array<char, 128> buffer {};
    const auto converted = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value.doubleValue,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10
    );
    if (converted.ec == std::errc()) {
        return std::string(buffer.data(), converted.ptr);
    }
    return std::to_string(value.doubleValue);
}

std::string scalarString(const ProtoDefValue& value) {
    if (value.kind == ProtoDefValue::Kind::String) return value.stringValue;
    return numberString(value);
}

bool truthy(const ProtoDefValue* value) {
    if (value == nullptr) return false;
    switch (value->kind) {
        case ProtoDefValue::Kind::Null: return false;
        case ProtoDefValue::Kind::Bool: return value->boolValue;
        case ProtoDefValue::Kind::Int: return value->intValue != 0;
        case ProtoDefValue::Kind::UInt: return value->uintValue != 0;
        case ProtoDefValue::Kind::Double:
            return value->doubleValue != 0.0 && !std::isnan(value->doubleValue);
        case ProtoDefValue::Kind::String: return !value->stringValue.empty();
        case ProtoDefValue::Kind::Bytes:
        case ProtoDefValue::Kind::Object:
        case ProtoDefValue::Kind::Array:
            return true;
    }
    return false;
}

const ProtoDefValue* textValue(const ProtoDefValue& value) {
    if (isTextScalar(value)) return &value;
    if (value.kind != ProtoDefValue::Kind::Object) return nullptr;
    if (const auto* text = field(value, "text"); text != nullptr && isTextScalar(*text)) {
        return text;
    }
    if (const auto* text = field(value, ""); text != nullptr && isTextScalar(*text)) {
        return text;
    }
    return nullptr;
}

const ProtoDefValue* arrayField(const ProtoDefValue& value, std::string_view name) {
    const auto* out = field(value, name);
    return out != nullptr && out->kind == ProtoDefValue::Kind::Array ? out : nullptr;
}

const ProtoDefValue* extras(const ProtoDefValue& value) {
    if (value.kind == ProtoDefValue::Kind::Array) return &value;
    return arrayField(value, "extra");
}

std::string translationFormat(
    const ProtoDefValue& value,
    const BedrockLanguage& language
) {
    const auto* translate = field(value, "translate");
    if (translate == nullptr || translate->kind != ProtoDefValue::Kind::String) return {};

    if (const auto* found = language.find(translate->stringValue)) return *found;
    if (const auto* fallback = field(value, "fallback");
        fallback != nullptr && fallback->kind == ProtoDefValue::Kind::String) {
        return fallback->stringValue;
    }
    return translate->stringValue;
}

std::string formatTranslation(
    std::string_view format,
    const std::vector<std::string>& values
) {
    std::string out;
    out.reserve(format.size());
    std::size_t sequential = 0;

    for (std::size_t i = 0; i < format.size();) {
        if (format[i] != '%') {
            out.push_back(format[i++]);
            continue;
        }

        const auto start = i++;
        if (i < format.size() && format[i] == '%') {
            out.push_back('%');
            ++i;
            continue;
        }

        std::size_t digitsStart = i;
        while (i < format.size() && format[i] >= '0' && format[i] <= '9') ++i;

        std::optional<std::size_t> positional;
        if (i > digitsStart && i < format.size() && format[i] == '$') {
            std::size_t parsed = 0;
            const auto result = std::from_chars(
                format.data() + digitsStart,
                format.data() + i,
                parsed
            );
            if (result.ec == std::errc() && parsed > 0) positional = parsed - 1;
            ++i;
        } else {
            i = digitsStart;
        }

        if (i < format.size() && (format[i] == 's' ||
            (format[i] == '%' && positional.has_value()))) {
            const auto index = positional.value_or(sequential++);
            if (index < values.size()) out += values[index];
            ++i;
            continue;
        }

        out.append(format.substr(start, i - start));
    }

    return out;
}

std::size_t utf8SequenceLength(unsigned char value) {
    if ((value & 0x80u) == 0) return 1;
    if ((value & 0xe0u) == 0xc0u) return 2;
    if ((value & 0xf0u) == 0xe0u) return 3;
    if ((value & 0xf8u) == 0xf0u) return 4;
    return 1;
}

std::string truncateLikeJavascript(std::string_view value, std::size_t maxUnits) {
    std::size_t bytes = 0;
    std::size_t units = 0;
    while (bytes < value.size()) {
        auto length = utf8SequenceLength(static_cast<unsigned char>(value[bytes]));
        if (bytes + length > value.size()) length = 1;
        const auto addedUnits = length == 4 ? 2u : 1u;
        if (units + addedUnits > maxUnits) break;
        units += addedUnits;
        bytes += length;
    }
    return std::string(value.substr(0, bytes));
}

std::size_t javascriptLength(std::string_view value) {
    std::size_t bytes = 0;
    std::size_t units = 0;
    while (bytes < value.size()) {
        auto length = utf8SequenceLength(static_cast<unsigned char>(value[bytes]));
        if (bytes + length > value.size()) length = 1;
        units += length == 4 ? 2u : 1u;
        bytes += length;
    }
    return units;
}

bool isLegacyFormattingCode(char value) {
    return (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f') ||
        value == 'l' || value == 'n' || value == 'm' || value == 'o' ||
        value == 'k' || value == 'r';
}

std::string stripLegacyFormatting(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size();) {
        if (i + 2 < value.size() &&
            value.compare(i, Section.size(), Section) == 0 &&
            isLegacyFormattingCode(value[i + Section.size()])) {
            i += Section.size() + 1;
            continue;
        }
        out.push_back(value[i++]);
    }
    return out;
}

BedrockMessageBuilder fromSectionCodeString(std::string_view value) {
    char separator = 1;
    while (separator < 0x20 && value.find(separator) != std::string_view::npos) {
        ++separator;
    }
    if (separator >= 0x20) {
        throw std::runtime_error("chat text contains every available control separator");
    }

    std::string normalized;
    normalized.reserve(value.size());
    for (std::size_t i = 0; i < value.size();) {
        if (value.compare(i, Section.size(), Section) == 0) {
            normalized.push_back(separator);
            i += Section.size();
        } else {
            normalized.push_back(value[i++]);
        }
    }
    return BedrockMessageBuilder::fromString(normalized, separator);
}

void replaceAll(std::string& value, std::string_view from, std::string_view to) {
    if (from.empty()) return;
    std::size_t offset = 0;
    while ((offset = value.find(from, offset)) != std::string::npos) {
        value.replace(offset, from.size(), to);
        offset += to.size();
    }
}

bool isHexDigit(char value) {
    return (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F');
}

int hexByte(char first, char second) {
    const auto nibble = [](char value) {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return value - 'A' + 10;
    };
    return nibble(first) * 16 + nibble(second);
}

bool isHexColor(std::string_view value) {
    if (value.size() != 7 || value.front() != '#') return false;
    return std::all_of(value.begin() + 1, value.end(), isHexDigit);
}

std::string replaceHexWithAnsi(std::string value) {
    std::size_t offset = 0;
    while ((offset = value.find(Section, offset)) != std::string::npos) {
        std::size_t digits = offset + Section.size();
        if (digits < value.size() && value[digits] == '#') ++digits;
        if (digits + 6 > value.size()) {
            offset += Section.size();
            continue;
        }
        bool valid = true;
        for (std::size_t i = 0; i < 6; ++i) valid &= isHexDigit(value[digits + i]);
        if (!valid) {
            offset += Section.size();
            continue;
        }

        const auto red = hexByte(value[digits], value[digits + 1]);
        const auto green = hexByte(value[digits + 2], value[digits + 3]);
        const auto blue = hexByte(value[digits + 4], value[digits + 5]);
        const auto consumed = digits + 6 - offset;
        const auto replacement = "\x1b[38;2;" + std::to_string(red) + ";" +
            std::to_string(green) + ";" + std::to_string(blue) + "m";
        value.replace(offset, consumed, replacement);
        offset += replacement.size();
    }
    return value;
}

std::string escapeHtml(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const auto character : value) {
        switch (character) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#039;"; break;
            default: out.push_back(character); break;
        }
    }
    return out;
}

enum class FlagState {
    Falsy,
    Enabled,
    DisabledString
};

FlagState ownFlag(const ProtoDefValue* value) {
    if (value != nullptr && value->kind == ProtoDefValue::Kind::String &&
        value->stringValue == "false") {
        return FlagState::DisabledString;
    }
    return truthy(value) ? FlagState::Enabled : FlagState::Falsy;
}

struct ChatStyle {
    std::optional<std::string> color;
    FlagState bold = FlagState::Falsy;
    FlagState italic = FlagState::Falsy;
    FlagState underlined = FlagState::Falsy;
    FlagState strikethrough = FlagState::Falsy;
    FlagState obfuscated = FlagState::Falsy;
};

bool isNamedColor(std::string_view value) {
    static constexpr std::array<std::string_view, 16> colors = {
        "black", "dark_blue", "dark_green", "dark_aqua", "dark_red",
        "dark_purple", "gold", "gray", "dark_gray", "blue", "green",
        "aqua", "red", "light_purple", "yellow", "white"
    };
    return std::find(colors.begin(), colors.end(), value) != colors.end();
}

ChatStyle ownStyle(const ProtoDefValue& value) {
    ChatStyle style;
    style.bold = ownFlag(field(value, "bold"));
    style.italic = ownFlag(field(value, "italic"));
    style.underlined = ownFlag(field(value, "underlined"));
    style.strikethrough = ownFlag(field(value, "strikethrough"));
    style.obfuscated = ownFlag(field(value, "obfuscated"));

    const auto* color = field(value, "color");
    if (color == nullptr || color->kind != ProtoDefValue::Kind::String) return style;
    const auto& name = color->stringValue;
    if (name == "obfuscated") style.obfuscated = FlagState::Enabled;
    else if (name == "bold") style.bold = FlagState::Enabled;
    else if (name == "strikethrough") style.strikethrough = FlagState::Enabled;
    else if (name == "underlined") style.underlined = FlagState::Enabled;
    else if (name == "italic") style.italic = FlagState::Enabled;
    else if (name != "reset" && (isNamedColor(name) || isHexColor(name))) {
        style.color = name;
    }
    return style;
}

FlagState inheritFlag(FlagState own, FlagState parent) {
    return own == FlagState::Falsy ? parent : own;
}

ChatStyle inheritedStyle(const ProtoDefValue& value, const ChatStyle& parent) {
    auto style = ownStyle(value);
    if (!style.color.has_value()) style.color = parent.color;
    style.bold = inheritFlag(style.bold, parent.bold);
    style.italic = inheritFlag(style.italic, parent.italic);
    style.underlined = inheritFlag(style.underlined, parent.underlined);
    style.strikethrough = inheritFlag(style.strikethrough, parent.strikethrough);
    style.obfuscated = inheritFlag(style.obfuscated, parent.obfuscated);
    return style;
}

std::string colorCode(std::string_view color) {
    static const std::unordered_map<std::string, std::string> codes = {
        {"black", "\xc2\xa7" "0"}, {"dark_blue", "\xc2\xa7" "1"},
        {"dark_green", "\xc2\xa7" "2"}, {"dark_aqua", "\xc2\xa7" "3"},
        {"dark_red", "\xc2\xa7" "4"}, {"dark_purple", "\xc2\xa7" "5"},
        {"gold", "\xc2\xa7" "6"}, {"gray", "\xc2\xa7" "7"},
        {"dark_gray", "\xc2\xa7" "8"}, {"blue", "\xc2\xa7" "9"},
        {"green", "\xc2\xa7" "a"}, {"aqua", "\xc2\xa7" "b"},
        {"red", "\xc2\xa7" "c"}, {"light_purple", "\xc2\xa7" "d"},
        {"yellow", "\xc2\xa7" "e"}, {"white", "\xc2\xa7" "f"}
    };
    if (isHexColor(color)) return std::string(Section) + std::string(color);
    const auto found = codes.find(std::string(color));
    return found == codes.end() ? std::string() : found->second;
}

std::string motdPrefix(const ChatStyle& style) {
    std::string out;
    if (style.color.has_value()) out += colorCode(*style.color);
    if (style.bold == FlagState::Enabled) out += "\xc2\xa7l";
    if (style.italic == FlagState::Enabled) out += "\xc2\xa7o";
    if (style.underlined == FlagState::Enabled) out += "\xc2\xa7n";
    if (style.strikethrough == FlagState::Enabled) out += "\xc2\xa7m";
    if (style.obfuscated == FlagState::Enabled) out += "\xc2\xa7k";
    return out;
}

std::string renderString(
    const ProtoDefValue& value,
    const BedrockLanguage& language,
    std::size_t depth
) {
    if (depth > BedrockChatMessage::MaxDepth) return {};
    if (value.kind == ProtoDefValue::Kind::String) {
        const auto normalized = fromSectionCodeString(value.stringValue).toProtoDefValue();
        return renderString(normalized, language, depth);
    }
    std::string message;

    if (const auto* text = textValue(value)) {
        message += scalarString(*text);
    } else if (const auto* translate = field(value, "translate");
        translate != nullptr && translate->kind == ProtoDefValue::Kind::String) {
        std::vector<std::string> values;
        if (const auto* with = arrayField(value, "with")) {
            values.reserve(with->arrayValue.size());
            for (const auto& entry : with->arrayValue) {
                values.push_back(renderString(entry, language, depth + 1));
            }
        }
        message += formatTranslation(translationFormat(value, language), values);
    }

    if (const auto* extra = extras(value)) {
        for (const auto& entry : extra->arrayValue) {
            message += renderString(entry, language, depth + 1);
        }
    }

    return truncateLikeJavascript(
        stripLegacyFormatting(message),
        BedrockChatMessage::MaxLength
    );
}

std::string renderMotd(
    const ProtoDefValue& value,
    const BedrockLanguage& language,
    const ChatStyle& parent,
    std::size_t depth
) {
    if (depth > BedrockChatMessage::MaxDepth) return {};
    if (value.kind == ProtoDefValue::Kind::String) {
        const auto normalized = fromSectionCodeString(value.stringValue).toProtoDefValue();
        return renderMotd(normalized, language, parent, depth);
    }
    const auto style = inheritedStyle(value, parent);
    const auto prefix = motdPrefix(style);
    std::string message = prefix;

    if (const auto* text = textValue(value)) {
        message += scalarString(*text);
    } else if (const auto* translate = field(value, "translate");
        translate != nullptr && translate->kind == ProtoDefValue::Kind::String) {
        std::vector<std::string> values;
        if (const auto* with = arrayField(value, "with")) {
            values.reserve(with->arrayValue.size());
            for (const auto& entry : with->arrayValue) {
                auto rendered = renderMotd(entry, language, style, depth + 1);
                if (rendered.find(Section) != std::string::npos) {
                    rendered += "\xc2\xa7r";
                    rendered += prefix;
                }
                values.push_back(std::move(rendered));
            }
        }
        message += formatTranslation(translationFormat(value, language), values);
    }

    if (const auto* extra = extras(value)) {
        for (const auto& entry : extra->arrayValue) {
            message += renderMotd(entry, language, style, depth + 1);
        }
    }

    return truncateLikeJavascript(message, BedrockChatMessage::MaxLength);
}

BedrockChatAnsiCodes defaultAnsiCodes() {
    return {
        {"\xc2\xa7" "0", "\x1b[30m"}, {"\xc2\xa7" "1", "\x1b[34m"},
        {"\xc2\xa7" "2", "\x1b[32m"}, {"\xc2\xa7" "3", "\x1b[36m"},
        {"\xc2\xa7" "4", "\x1b[31m"}, {"\xc2\xa7" "5", "\x1b[35m"},
        {"\xc2\xa7" "6", "\x1b[33m"}, {"\xc2\xa7" "7", "\x1b[37m"},
        {"\xc2\xa7" "8", "\x1b[90m"}, {"\xc2\xa7" "9", "\x1b[94m"},
        {"\xc2\xa7" "a", "\x1b[92m"}, {"\xc2\xa7" "b", "\x1b[96m"},
        {"\xc2\xa7" "c", "\x1b[91m"}, {"\xc2\xa7" "d", "\x1b[95m"},
        {"\xc2\xa7" "e", "\x1b[93m"}, {"\xc2\xa7" "f", "\x1b[97m"},
        {"\xc2\xa7l", "\x1b[1m"}, {"\xc2\xa7o", "\x1b[3m"},
        {"\xc2\xa7n", "\x1b[4m"}, {"\xc2\xa7m", "\x1b[9m"},
        {"\xc2\xa7k", "\x1b[6m"}, {"\xc2\xa7r", "\x1b[0m"}
    };
}

BedrockChatStyles defaultStyles() {
    return {
        {"black", "color:#000000"}, {"dark_blue", "color:#0000AA"},
        {"dark_green", "color:#00AA00"}, {"dark_aqua", "color:#00AAAA"},
        {"dark_red", "color:#AA0000"}, {"dark_purple", "color:#AA00AA"},
        {"gold", "color:#FFAA00"}, {"gray", "color:#AAAAAA"},
        {"dark_gray", "color:#555555"}, {"blue", "color:#5555FF"},
        {"green", "color:#55FF55"}, {"aqua", "color:#55FFFF"},
        {"red", "color:#FF5555"}, {"light_purple", "color:#FF55FF"},
        {"yellow", "color:#FFFF55"}, {"white", "color:#FFFFFF"},
        {"bold", "font-weight:900"},
        {"strikethrough", "text-decoration:line-through"},
        {"underlined", "text-decoration:underline"},
        {"italic", "font-style:italic"}
    };
}

bool formatAllowed(
    const std::vector<std::string>& allowedFormats,
    std::string_view value
) {
    return std::find(allowedFormats.begin(), allowedFormats.end(), value) !=
        allowedFormats.end();
}

std::string rgbStyle(std::string_view color) {
    if (!isHexColor(color)) return {};
    return "color:rgb(" + std::to_string(hexByte(color[1], color[2])) + "," +
        std::to_string(hexByte(color[3], color[4])) + "," +
        std::to_string(hexByte(color[5], color[6])) + ")";
}

std::string renderHtml(
    const ProtoDefValue& value,
    const BedrockLanguage& language,
    const BedrockChatStyles& styles,
    const std::vector<std::string>& allowedFormats,
    std::size_t depth
) {
    if (depth > BedrockChatMessage::MaxDepth) return {};
    if (value.kind == ProtoDefValue::Kind::String) {
        const auto normalized = fromSectionCodeString(value.stringValue).toProtoDefValue();
        return renderHtml(normalized, language, styles, allowedFormats, depth);
    }
    const auto style = ownStyle(value);
    std::vector<std::string> css;

    if (formatAllowed(allowedFormats, "color") && style.color.has_value()) {
        if (isHexColor(*style.color)) {
            css.push_back(rgbStyle(*style.color));
        } else if (const auto found = styles.find(*style.color); found != styles.end()) {
            css.push_back(found->second);
        }
    }

    const auto addFlag = [&](std::string_view name, FlagState state) {
        if (!formatAllowed(allowedFormats, name) || state != FlagState::Enabled) return;
        if (const auto found = styles.find(std::string(name)); found != styles.end()) {
            css.push_back(found->second);
        }
    };
    addFlag("bold", style.bold);
    addFlag("strikethrough", style.strikethrough);
    addFlag("underlined", style.underlined);
    addFlag("italic", style.italic);

    std::string out;
    const bool hasAllowedStyle =
        (formatAllowed(allowedFormats, "color") && style.color.has_value()) ||
        (formatAllowed(allowedFormats, "bold") && style.bold == FlagState::Enabled) ||
        (formatAllowed(allowedFormats, "strikethrough") &&
            style.strikethrough == FlagState::Enabled) ||
        (formatAllowed(allowedFormats, "underlined") &&
            style.underlined == FlagState::Enabled) ||
        (formatAllowed(allowedFormats, "italic") && style.italic == FlagState::Enabled);
    if (hasAllowedStyle) {
        out += "<span style=\"";
        for (std::size_t i = 0; i < css.size(); ++i) {
            if (i) out.push_back(';');
            out += css[i];
        }
        out += "\">";
    } else {
        out += "<span>";
    }

    if (const auto* text = textValue(value); text != nullptr && truthy(text)) {
        out += escapeHtml(scalarString(*text));
    } else if (const auto* translate = field(value, "translate");
        translate != nullptr && translate->kind == ProtoDefValue::Kind::String) {
        std::vector<std::string> values;
        if (const auto* with = arrayField(value, "with")) {
            values.reserve(with->arrayValue.size());
            for (const auto& entry : with->arrayValue) {
                values.push_back(renderHtml(
                    entry,
                    language,
                    styles,
                    allowedFormats,
                    depth + 1
                ));
            }
        }
        out += formatTranslation(
            escapeHtml(translationFormat(value, language)),
            values
        );
    }

    if (const auto* extra = extras(value)) {
        for (const auto& entry : extra->arrayValue) {
            out += renderHtml(entry, language, styles, allowedFormats, depth + 1);
        }
    }
    out += "</span>";
    return out;
}

char lowerAscii(char value) {
    if (value >= 'A' && value <= 'Z') return static_cast<char>(value - 'A' + 'a');
    return value;
}

std::string lowerAscii(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](char character) {
        return lowerAscii(character);
    });
    return out;
}

} // namespace

std::string_view bedrockTextTypeName(BedrockTextType type) {
    switch (type) {
        case BedrockTextType::Raw: return "raw";
        case BedrockTextType::Chat: return "chat";
        case BedrockTextType::Translation: return "translation";
        case BedrockTextType::Popup: return "popup";
        case BedrockTextType::JukeboxPopup: return "jukebox_popup";
        case BedrockTextType::Tip: return "tip";
        case BedrockTextType::System: return "system";
        case BedrockTextType::Whisper: return "whisper";
        case BedrockTextType::Announcement: return "announcement";
        case BedrockTextType::JsonWhisper: return "json_whisper";
        case BedrockTextType::Json: return "json";
        case BedrockTextType::JsonAnnouncement: return "json_announcement";
        case BedrockTextType::Unknown: return "unknown";
    }
    return "unknown";
}

BedrockTextType bedrockTextTypeFromId(uint8_t value) {
    if (value <= static_cast<uint8_t>(BedrockTextType::JsonAnnouncement)) {
        return static_cast<BedrockTextType>(value);
    }
    return BedrockTextType::Unknown;
}

BedrockTextType bedrockTextTypeFromName(std::string_view value) {
    if (const auto slash = value.rfind('/'); slash != std::string_view::npos) {
        value.remove_prefix(slash + 1);
    }
    const auto normalized = lowerAscii(value);
    static const std::unordered_map<std::string, BedrockTextType> names = {
        {"raw", BedrockTextType::Raw}, {"chat", BedrockTextType::Chat},
        {"translation", BedrockTextType::Translation},
        {"popup", BedrockTextType::Popup},
        {"jukebox_popup", BedrockTextType::JukeboxPopup},
        {"tip", BedrockTextType::Tip}, {"system", BedrockTextType::System},
        {"whisper", BedrockTextType::Whisper},
        {"announcement", BedrockTextType::Announcement},
        {"json_whisper", BedrockTextType::JsonWhisper},
        {"json", BedrockTextType::Json},
        {"json_announcement", BedrockTextType::JsonAnnouncement}
    };
    if (const auto found = names.find(normalized); found != names.end()) {
        return found->second;
    }

    unsigned int id = 0;
    const auto parsed = std::from_chars(
        normalized.data(),
        normalized.data() + normalized.size(),
        id
    );
    if (parsed.ec == std::errc() && parsed.ptr == normalized.data() + normalized.size() &&
        id <= std::numeric_limits<uint8_t>::max()) {
        return bedrockTextTypeFromId(static_cast<uint8_t>(id));
    }
    return BedrockTextType::Unknown;
}

BedrockLanguage::BedrockLanguage()
    : data_(std::make_shared<BedrockLanguageData>()) {}

const std::string* BedrockLanguage::find(std::string_view key) const {
    const auto found = data_->entries.find(std::string(key));
    return found == data_->entries.end() ? nullptr : &found->second;
}

bool BedrockLanguage::contains(std::string_view key) const {
    return find(key) != nullptr;
}

std::string BedrockLanguage::valueOr(
    std::string_view key,
    std::string_view fallback
) const {
    if (const auto* found = find(key)) return *found;
    return std::string(fallback);
}

std::size_t BedrockLanguage::size() const { return data_->entries.size(); }
bool BedrockLanguage::empty() const { return data_->entries.empty(); }

BedrockLanguage BedrockLanguageLoader::loadMinecraftData(
    const std::filesystem::path& languageJson
) {
    std::ifstream file(languageJson, std::ios::binary);
    if (!file) {
        throw std::runtime_error(
            "failed to open minecraft-data file: " + languageJson.string()
        );
    }
    const std::string json {
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    };

    ProtoDefValue root;
    try {
        root = ProtoDefJson::parse(json);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "failed to parse minecraft-data JSON " + languageJson.string() + ": " +
            error.what()
        );
    }
    if (root.kind != ProtoDefValue::Kind::Object) {
        throw std::runtime_error("minecraft-data language.json root must be an object");
    }

    BedrockLanguage language;
    language.data_->entries.reserve(root.objectValue.size());
    for (auto& [key, value] : root.objectValue) {
        if (value.kind != ProtoDefValue::Kind::String) {
            throw std::runtime_error(
                "minecraft-data language value must be a string: " + key
            );
        }
        language.data_->entries.emplace(std::move(key), std::move(value.stringValue));
    }
    return language;
}

BedrockMessageBuilder& BedrockMessageBuilder::setBold(bool value) {
    bold_ = value;
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::setItalic(bool value) {
    italic_ = value;
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::setUnderlined(bool value) {
    underlined_ = value;
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::setStrikethrough(bool value) {
    strikethrough_ = value;
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::setObfuscated(bool value) {
    obfuscated_ = value;
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::setColor(std::string value) {
    color_ = std::move(value);
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::setText(std::string value) {
    text_ = std::move(value);
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::setFont(std::string value) {
    font_ = std::move(value);
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::setTranslate(std::string value) {
    translate_ = std::move(value);
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::setInsertion(std::string value) {
    insertion_ = std::move(value);
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::setKeybind(std::string value) {
    keybind_ = std::move(value);
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::setScore(
    std::string name,
    std::string objective
) {
    score_ = ProtoDefValue::object({
        {"name", ProtoDefValue::string(std::move(name))},
        {"objective", ProtoDefValue::string(std::move(objective))}
    });
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::setClickEvent(
    std::string action,
    ProtoDefValue value
) {
    clickEvent_ = ProtoDefValue::object({
        {"action", ProtoDefValue::string(std::move(action))},
        {"value", std::move(value)}
    });
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::setClickEvent(
    std::string action,
    std::string value
) {
    return setClickEvent(std::move(action), ProtoDefValue::string(std::move(value)));
}

BedrockMessageBuilder& BedrockMessageBuilder::setClickEvent(
    std::string action,
    int64_t value
) {
    return setClickEvent(std::move(action), ProtoDefValue::integer(value));
}

BedrockMessageBuilder& BedrockMessageBuilder::setHoverEvent(
    std::string action,
    ProtoDefValue data,
    std::string type
) {
    hoverEvent_ = ProtoDefValue::object({
        {"action", ProtoDefValue::string(std::move(action))},
        {std::move(type), std::move(data)}
    });
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::addExtra(ProtoDefValue value) {
    extra_.push_back(std::move(value));
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::addExtra(
    const BedrockMessageBuilder& value
) {
    return addExtra(value.toProtoDefValue());
}

BedrockMessageBuilder& BedrockMessageBuilder::addExtra(std::string value) {
    return addExtra(ProtoDefValue::string(std::move(value)));
}

BedrockMessageBuilder& BedrockMessageBuilder::addWith(ProtoDefValue value) {
    with_.push_back(std::move(value));
    return *this;
}

BedrockMessageBuilder& BedrockMessageBuilder::addWith(
    const BedrockMessageBuilder& value
) {
    return addWith(value.toProtoDefValue());
}

BedrockMessageBuilder& BedrockMessageBuilder::addWith(std::string value) {
    return addWith(ProtoDefValue::string(std::move(value)));
}

BedrockMessageBuilder& BedrockMessageBuilder::resetFormatting() {
    return setBold(false)
        .setItalic(false)
        .setUnderlined(false)
        .setStrikethrough(false)
        .setObfuscated(false)
        .setColor("reset");
}

ProtoDefValue BedrockMessageBuilder::toProtoDefValue() const {
    std::unordered_map<std::string, ProtoDefValue> object;
    if (strikethrough_) object["strikethrough"] = ProtoDefValue::boolean(*strikethrough_);
    if (obfuscated_) object["obfuscated"] = ProtoDefValue::boolean(*obfuscated_);
    if (underlined_) object["underlined"] = ProtoDefValue::boolean(*underlined_);
    if (clickEvent_) object["clickEvent"] = *clickEvent_;
    if (hoverEvent_) object["hoverEvent"] = *hoverEvent_;
    if (translate_) object["translate"] = ProtoDefValue::string(*translate_);
    if (insertion_) object["insertion"] = ProtoDefValue::string(*insertion_);
    if (italic_) object["italic"] = ProtoDefValue::boolean(*italic_);
    if (color_) object["color"] = ProtoDefValue::string(*color_);
    if (bold_) object["bold"] = ProtoDefValue::boolean(*bold_);
    if (font_) object["font"] = ProtoDefValue::string(*font_);
    if (text_) object["text"] = ProtoDefValue::string(*text_);
    else if (keybind_) object["keybind"] = ProtoDefValue::string(*keybind_);
    else if (score_) object["score"] = *score_;
    if (translate_ && !with_.empty()) object["with"] = ProtoDefValue::array(with_);
    if (!extra_.empty()) object["extra"] = ProtoDefValue::array(extra_);
    return ProtoDefValue::object(std::move(object));
}

std::string BedrockMessageBuilder::toJson() const {
    return ProtoDefJson::stringify(toProtoDefValue());
}

BedrockMessageBuilder BedrockMessageBuilder::fromString(
    std::string_view value,
    char colorSeparator
) {
    struct Piece {
        std::optional<char> code;
        std::string text;
    };
    std::vector<Piece> pieces;

    std::size_t cursor = 0;
    const auto first = value.find(colorSeparator);
    if (first == std::string_view::npos) {
        BedrockMessageBuilder out;
        out.setText(std::string(value));
        return out;
    }
    if (first > 0) pieces.push_back({std::nullopt, std::string(value.substr(0, first))});
    cursor = first;

    while (cursor < value.size()) {
        const auto codeIndex = cursor + 1;
        std::optional<char> code;
        std::size_t textStart = codeIndex;
        if (codeIndex < value.size()) {
            code = value[codeIndex];
            textStart = codeIndex + 1;
        }
        const auto next = value.find(colorSeparator, textStart);
        pieces.push_back({
            code,
            std::string(value.substr(
                textStart,
                next == std::string_view::npos ? std::string_view::npos : next - textStart
            ))
        });
        if (next == std::string_view::npos) break;
        cursor = next;
    }

    auto makePiece = [](const Piece& piece) {
        BedrockMessageBuilder out;
        if (piece.code.has_value()) {
            switch (*piece.code) {
                case '0': out.setColor("black"); break;
                case '1': out.setColor("dark_blue"); break;
                case '2': out.setColor("dark_green"); break;
                case '3': out.setColor("dark_aqua"); break;
                case '4': out.setColor("dark_red"); break;
                case '5': out.setColor("dark_purple"); break;
                case '6': out.setColor("gold"); break;
                case '7': out.setColor("gray"); break;
                case '8': out.setColor("dark_gray"); break;
                case '9': out.setColor("blue"); break;
                case 'a': out.setColor("green"); break;
                case 'b': out.setColor("aqua"); break;
                case 'c': out.setColor("red"); break;
                case 'd': out.setColor("light_purple"); break;
                case 'e': out.setColor("yellow"); break;
                case 'f': out.setColor("white"); break;
                case 'k': out.setObfuscated(true); break;
                case 'l': out.setBold(true); break;
                case 'm': out.setStrikethrough(true); break;
                case 'n': out.setUnderlined(true); break;
                case 'o': out.setItalic(true); break;
                case 'r': out.resetFormatting(); break;
                default: break;
            }
        }
        out.setText(piece.text);
        return out;
    };

    BedrockMessageBuilder out = makePiece(pieces.back());
    for (std::size_t i = pieces.size() - 1; i-- > 0;) {
        auto parent = makePiece(pieces[i]);
        parent.addExtra(out);
        out = std::move(parent);
    }
    return out;
}

ProtoDefValue BedrockChatMessage::normalizeMessage(ProtoDefValue message) {
    if (message.kind == ProtoDefValue::Kind::String) {
        if (message.stringValue.empty()) {
            return ProtoDefValue::object({{"text", ProtoDefValue::string("")}});
        }
        return fromSectionCodeString(message.stringValue).toProtoDefValue();
    }
    if (isNumber(message)) {
        return ProtoDefValue::object({{"text", std::move(message)}});
    }
    if (message.kind == ProtoDefValue::Kind::Array) {
        return ProtoDefValue::object({{"extra", std::move(message)}});
    }
    if (message.kind != ProtoDefValue::Kind::Object) {
        throw std::runtime_error("expected string, number, object, or array chat message");
    }
    return message;
}

void BedrockChatMessage::validate(const ProtoDefValue& message, std::size_t depth) {
    if (depth > 1024) throw std::runtime_error("chat message nesting is excessive");
    if (message.kind == ProtoDefValue::Kind::Array) {
        for (const auto& entry : message.arrayValue) validate(entry, depth + 1);
        return;
    }
    if (message.kind != ProtoDefValue::Kind::Object && !isTextScalar(message)) return;

    if (message.kind == ProtoDefValue::Kind::Object) {
        for (const auto name : {"with", "extra"}) {
            if (const auto* entries = field(message, name)) {
                if (entries->kind != ProtoDefValue::Kind::Array) {
                    throw std::runtime_error(
                        "expected " + std::string(name) + " property to be an array in chat message"
                    );
                }
                for (const auto& entry : entries->arrayValue) validate(entry, depth + 1);
            }
        }
        for (const auto name : {"clickEvent", "hoverEvent"}) {
            if (const auto* event = field(message, name)) {
                if (event->kind != ProtoDefValue::Kind::Object) {
                    throw std::runtime_error(
                        std::string(name) + " must be an object in chat message"
                    );
                }
                const auto* action = field(*event, "action");
                if (action == nullptr || action->kind != ProtoDefValue::Kind::String) {
                    throw std::runtime_error(
                        std::string(name) + " action is missing in chat message"
                    );
                }
            }
        }
    }
}

BedrockChatMessage::BedrockChatMessage(
    std::string message,
    BedrockLanguage language
) : json_(message.empty()
        ? ProtoDefValue::object({{"text", ProtoDefValue::string("")}})
        : fromSectionCodeString(message).toProtoDefValue()),
    language_(std::move(language)) {
    validate(json_);
}

BedrockChatMessage::BedrockChatMessage(
    int64_t message,
    BedrockLanguage language
) : json_(ProtoDefValue::object({{"text", ProtoDefValue::integer(message)}})),
    language_(std::move(language)) {
    validate(json_);
}

BedrockChatMessage::BedrockChatMessage(
    ProtoDefValue message,
    BedrockLanguage language
) : json_(normalizeMessage(std::move(message))), language_(std::move(language)) {
    validate(json_);
}

BedrockChatMessage::BedrockChatMessage(
    const BedrockMessageBuilder& message,
    BedrockLanguage language
) : json_(message.toProtoDefValue()), language_(std::move(language)) {
    validate(json_);
}

const ProtoDefValue& BedrockChatMessage::json() const { return json_; }

std::string BedrockChatMessage::toJson() const {
    return ProtoDefJson::stringify(json_);
}

void BedrockChatMessage::append(const BedrockChatMessage& message) {
    append(message.json_);
}

void BedrockChatMessage::append(const BedrockMessageBuilder& message) {
    append(message.toProtoDefValue());
}

void BedrockChatMessage::append(ProtoDefValue message) {
    auto normalized = normalizeMessage(std::move(message));
    validate(normalized);
    auto found = json_.objectValue.find("extra");
    if (found == json_.objectValue.end()) {
        found = json_.objectValue.emplace(
            "extra",
            ProtoDefValue::array({})
        ).first;
    }
    if (found->second.kind != ProtoDefValue::Kind::Array) {
        throw std::runtime_error("expected extra property to be an array in chat message");
    }
    found->second.arrayValue.push_back(std::move(normalized));
}

void BedrockChatMessage::append(std::string message) {
    append(BedrockChatMessage(std::move(message), language_));
}

BedrockChatMessage BedrockChatMessage::clone() const {
    return BedrockChatMessage(json_, language_);
}

std::size_t BedrockChatMessage::length() const {
    std::size_t count = 0;
    const auto* text = textValue(json_);
    if (truthy(text)) {
        ++count;
    } else if (const auto* with = arrayField(json_, "with")) {
        count += with->arrayValue.size();
    }
    if (const auto* extra = extras(json_)) count += extra->arrayValue.size();
    return count;
}

std::string BedrockChatMessage::getText(std::size_t index) const {
    return getText(index, language_);
}

std::string BedrockChatMessage::getText(
    std::size_t index,
    const BedrockLanguage& language
) const {
    const auto* text = textValue(json_);
    if (truthy(text) && index == 0) {
        return stripLegacyFormatting(scalarString(*text));
    }

    const auto* with = arrayField(json_, "with");
    const auto withSize = with == nullptr ? 0 : with->arrayValue.size();
    if (!truthy(text) && with != nullptr && index < withSize) {
        return renderString(with->arrayValue[index], language, 0);
    }

    if (const auto* extra = extras(json_)) {
        const auto prefix = truthy(text) ? 1u : withSize;
        if (index >= prefix && index - prefix < extra->arrayValue.size()) {
            return renderString(extra->arrayValue[index - prefix], language, 0);
        }
    }
    return {};
}

std::string BedrockChatMessage::toString() const {
    return toString(language_);
}

std::string BedrockChatMessage::toString(const BedrockLanguage& language) const {
    return renderString(json_, language, 0);
}

std::string BedrockChatMessage::valueOf() const { return toString(); }

std::string BedrockChatMessage::toMotd() const { return toMotd(language_); }

std::string BedrockChatMessage::toMotd(const BedrockLanguage& language) const {
    return renderMotd(json_, language, {}, 0);
}

std::string BedrockChatMessage::toAnsi() const { return toAnsi(language_); }

std::string BedrockChatMessage::toAnsi(const BedrockLanguage& language) const {
    return toAnsi(language, defaultAnsiCodes());
}

std::string BedrockChatMessage::toAnsi(
    const BedrockLanguage& language,
    const BedrockChatAnsiCodes& codes
) const {
    auto message = toMotd(language);
    static constexpr std::array<std::string_view, 22> order = {
        "\xc2\xa7" "0", "\xc2\xa7" "1", "\xc2\xa7" "2", "\xc2\xa7" "3", "\xc2\xa7" "4",
        "\xc2\xa7" "5", "\xc2\xa7" "6", "\xc2\xa7" "7", "\xc2\xa7" "8", "\xc2\xa7" "9",
        "\xc2\xa7" "a", "\xc2\xa7" "b", "\xc2\xa7" "c", "\xc2\xa7" "d", "\xc2\xa7" "e",
        "\xc2\xa7" "f", "\xc2\xa7l", "\xc2\xa7o", "\xc2\xa7n", "\xc2\xa7m",
        "\xc2\xa7k", "\xc2\xa7r"
    };
    for (const auto key : order) {
        if (const auto found = codes.find(std::string(key)); found != codes.end()) {
            replaceAll(message, key, found->second);
        }
    }
    message = replaceHexWithAnsi(std::move(message));
    const auto reset = codes.find("\xc2\xa7r");
    const auto resetCode = reset == codes.end() ? std::string() : reset->second;
    return resetCode + truncateLikeJavascript(message, MaxLength) + resetCode;
}

std::string BedrockChatMessage::toHTML() const { return toHTML(language_); }

std::string BedrockChatMessage::toHTML(const BedrockLanguage& language) const {
    return toHTML(language, defaultStyles());
}

std::string BedrockChatMessage::toHTML(
    const BedrockLanguage& language,
    const BedrockChatStyles& styles,
    const std::vector<std::string>& allowedFormats
) const {
    auto out = renderHtml(json_, language, styles, allowedFormats, 0);
    if (javascriptLength(out) > MaxLength) return escapeHtml(toString(language));
    return out;
}

BedrockChatMessage BedrockChatMessage::fromNotch(
    std::string_view message,
    BedrockLanguage language
) {
    try {
        return BedrockChatMessage(
            ProtoDefJson::parse(std::string(message)),
            std::move(language)
        );
    } catch (...) {
        return BedrockChatMessage(std::string(message), std::move(language));
    }
}

BedrockChat::BedrockChat(BedrockLanguage language)
    : language_(std::move(language)) {}

const BedrockLanguage& BedrockChat::language() const { return language_; }
BedrockMessageBuilder BedrockChat::builder() const { return {}; }

BedrockChatMessage BedrockChat::message(std::string value) const {
    return BedrockChatMessage(std::move(value), language_);
}

BedrockChatMessage BedrockChat::message(int64_t value) const {
    return BedrockChatMessage(value, language_);
}

BedrockChatMessage BedrockChat::message(ProtoDefValue value) const {
    return BedrockChatMessage(std::move(value), language_);
}

BedrockChatMessage BedrockChat::message(const BedrockMessageBuilder& value) const {
    return BedrockChatMessage(value, language_);
}

BedrockChatMessage BedrockChat::fromNotch(std::string_view value) const {
    return BedrockChatMessage::fromNotch(value, language_);
}

BedrockChatMessage BedrockChat::fromTextPacket(const BedrockTextPacket& packet) const {
    if (packet.type == BedrockTextType::Json ||
        packet.type == BedrockTextType::JsonWhisper ||
        packet.type == BedrockTextType::JsonAnnouncement) {
        return fromNotch(packet.message);
    }
    if (packet.needsTranslation || packet.type == BedrockTextType::Translation ||
        packet.type == BedrockTextType::Popup ||
        packet.type == BedrockTextType::JukeboxPopup) {
        std::vector<ProtoDefValue> parameters;
        parameters.reserve(packet.parameters.size());
        for (const auto& value : packet.parameters) {
            parameters.push_back(ProtoDefValue::string(value));
        }
        return message(ProtoDefValue::object({
            {"translate", ProtoDefValue::string(packet.message)},
            {"with", ProtoDefValue::array(std::move(parameters))}
        }));
    }
    return message(packet.message);
}

} // namespace bedrock
