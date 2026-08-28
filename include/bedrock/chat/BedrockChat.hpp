#pragma once

#include <bedrock/protodef/ProtoDefValue.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bedrock {

enum class BedrockTextType : uint8_t {
    Raw = 0,
    Chat = 1,
    Translation = 2,
    Popup = 3,
    JukeboxPopup = 4,
    Tip = 5,
    System = 6,
    Whisper = 7,
    Announcement = 8,
    JsonWhisper = 9,
    Json = 10,
    JsonAnnouncement = 11,
    Unknown = 0xff
};

std::string_view bedrockTextTypeName(BedrockTextType type);
BedrockTextType bedrockTextTypeFromName(std::string_view value);
BedrockTextType bedrockTextTypeFromId(uint8_t value);

struct BedrockTextPacket {
    BedrockTextType type = BedrockTextType::Raw;
    bool needsTranslation = false;
    std::string sourceName;
    std::string message;
    std::vector<std::string> parameters;
    std::string xuid;
    std::string platformChatId;
    std::string filteredMessage;
};

struct BedrockLanguageData;
class BedrockLanguageLoader;

class BedrockLanguage {
public:
    BedrockLanguage();

    const std::string* find(std::string_view key) const;
    bool contains(std::string_view key) const;
    std::string valueOr(
        std::string_view key,
        std::string_view fallback
    ) const;
    std::size_t size() const;
    bool empty() const;

private:
    friend class BedrockLanguageLoader;

    std::shared_ptr<BedrockLanguageData> data_;
};

class BedrockLanguageLoader {
public:
    static BedrockLanguage loadMinecraftData(
        const std::filesystem::path& languageJson
    );
};

class BedrockMessageBuilder {
public:
    BedrockMessageBuilder& setBold(bool value);
    BedrockMessageBuilder& setItalic(bool value);
    BedrockMessageBuilder& setUnderlined(bool value);
    BedrockMessageBuilder& setStrikethrough(bool value);
    BedrockMessageBuilder& setObfuscated(bool value);
    BedrockMessageBuilder& setColor(std::string value);
    BedrockMessageBuilder& setText(std::string value);
    BedrockMessageBuilder& setFont(std::string value = "minecraft:default");
    BedrockMessageBuilder& setTranslate(std::string value);
    BedrockMessageBuilder& setInsertion(std::string value);
    BedrockMessageBuilder& setKeybind(std::string value);
    BedrockMessageBuilder& setScore(std::string name, std::string objective);
    BedrockMessageBuilder& setClickEvent(
        std::string action,
        ProtoDefValue value
    );
    BedrockMessageBuilder& setClickEvent(std::string action, std::string value);
    BedrockMessageBuilder& setClickEvent(std::string action, int64_t value);
    BedrockMessageBuilder& setHoverEvent(
        std::string action,
        ProtoDefValue data,
        std::string type = "contents"
    );

    BedrockMessageBuilder& addExtra(ProtoDefValue value);
    BedrockMessageBuilder& addExtra(const BedrockMessageBuilder& value);
    BedrockMessageBuilder& addExtra(std::string value);
    BedrockMessageBuilder& addWith(ProtoDefValue value);
    BedrockMessageBuilder& addWith(const BedrockMessageBuilder& value);
    BedrockMessageBuilder& addWith(std::string value);

    BedrockMessageBuilder& resetFormatting();

    ProtoDefValue toProtoDefValue() const;
    std::string toJson() const;

    static BedrockMessageBuilder fromString(
        std::string_view value,
        char colorSeparator = '&'
    );

private:
    std::optional<bool> bold_;
    std::optional<bool> italic_;
    std::optional<bool> underlined_;
    std::optional<bool> strikethrough_;
    std::optional<bool> obfuscated_;
    std::optional<std::string> color_;
    std::optional<std::string> text_;
    std::optional<std::string> font_;
    std::optional<std::string> translate_;
    std::optional<std::string> insertion_;
    std::optional<std::string> keybind_;
    std::optional<ProtoDefValue> score_;
    std::optional<ProtoDefValue> clickEvent_;
    std::optional<ProtoDefValue> hoverEvent_;
    std::vector<ProtoDefValue> with_;
    std::vector<ProtoDefValue> extra_;
};

using BedrockChatAnsiCodes = std::unordered_map<std::string, std::string>;
using BedrockChatStyles = std::unordered_map<std::string, std::string>;

class BedrockChatMessage {
public:
    static constexpr std::size_t MaxDepth = 8;
    static constexpr std::size_t MaxLength = 4096;

    explicit BedrockChatMessage(
        std::string message,
        BedrockLanguage language = {}
    );
    explicit BedrockChatMessage(
        int64_t message,
        BedrockLanguage language = {}
    );
    explicit BedrockChatMessage(
        ProtoDefValue message,
        BedrockLanguage language = {}
    );
    explicit BedrockChatMessage(
        const BedrockMessageBuilder& message,
        BedrockLanguage language = {}
    );

    const ProtoDefValue& json() const;
    std::string toJson() const;

    void append(const BedrockChatMessage& message);
    void append(const BedrockMessageBuilder& message);
    void append(ProtoDefValue message);
    void append(std::string message);

    BedrockChatMessage clone() const;
    std::size_t length() const;
    std::string getText(std::size_t index) const;
    std::string getText(std::size_t index, const BedrockLanguage& language) const;

    std::string toString() const;
    std::string toString(const BedrockLanguage& language) const;
    std::string valueOf() const;

    std::string toMotd() const;
    std::string toMotd(const BedrockLanguage& language) const;
    std::string toAnsi() const;
    std::string toAnsi(const BedrockLanguage& language) const;
    std::string toAnsi(
        const BedrockLanguage& language,
        const BedrockChatAnsiCodes& codes
    ) const;
    std::string toHTML() const;
    std::string toHTML(const BedrockLanguage& language) const;
    std::string toHTML(
        const BedrockLanguage& language,
        const BedrockChatStyles& styles,
        const std::vector<std::string>& allowedFormats = {
            "color", "bold", "strikethrough", "underlined", "italic"
        }
    ) const;

    static BedrockChatMessage fromNotch(
        std::string_view message,
        BedrockLanguage language = {}
    );

private:
    ProtoDefValue json_;
    BedrockLanguage language_;

    static ProtoDefValue normalizeMessage(ProtoDefValue message);
    static void validate(const ProtoDefValue& message, std::size_t depth = 0);
};

class BedrockChat {
public:
    explicit BedrockChat(BedrockLanguage language = {});

    const BedrockLanguage& language() const;
    BedrockMessageBuilder builder() const;
    BedrockChatMessage message(std::string value) const;
    BedrockChatMessage message(int64_t value) const;
    BedrockChatMessage message(ProtoDefValue value) const;
    BedrockChatMessage message(const BedrockMessageBuilder& value) const;
    BedrockChatMessage fromNotch(std::string_view value) const;
    BedrockChatMessage fromTextPacket(const BedrockTextPacket& packet) const;

private:
    BedrockLanguage language_;
};

} // namespace bedrock
