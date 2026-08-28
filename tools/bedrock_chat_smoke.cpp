#include <bedrock/api/Client.hpp>
#include <bedrock/chat/BedrockChat.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/protocol/PacketPayloadReader.hpp>
#include <bedrock/protocol/VersionedPayloadReader.hpp>
#include <bedrock/world/MinecraftDataAssets.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bedrock::ProtoDefValue text(std::string value) {
    return bedrock::ProtoDefValue::object({
        {"text", bedrock::ProtoDefValue::string(std::move(value))}
    });
}

} // namespace

int main() {
    try {
        bedrock::MinecraftDataAssets assets;
        const auto paths = assets.resolveByVersion("1.21.100");
        require(
            paths.languageDirectory == "1.21.70",
            "latest Bedrock language remap mismatch"
        );
        require(
            paths.languageJson.filename() == "language.json",
            "language.json path was not resolved"
        );

        const auto chat = assets.loadBedrockChatByProtocol(827);
        require(chat.language().size() == 10883, "latest Bedrock language count mismatch");
        require(
            chat.language().valueOr("chat.type.text", "") == "<%s> %s",
            "Bedrock chat translation mismatch"
        );
        require(
            chat.language().contains("commands.generic.unknown"),
            "Bedrock command translation is missing"
        );
        require(
            chat.language().valueOr("missing.key", "fallback") == "fallback",
            "language fallback mismatch"
        );

        const std::string section = "\xc2\xa7";
        const auto legacy = chat.message(
            section + "aHello " + section + "lBedrock" + section + "r!"
        );
        require(legacy.toString() == "Hello Bedrock!", "legacy plain output mismatch");
        require(
            legacy.toMotd() == section + "aHello " + section + "a" + section +
                "lBedrock" + section + "a" + section + "l!",
            "legacy MOTD output mismatch"
        );
        require(
            legacy.toAnsi() ==
                "\x1b[0m\x1b[92mHello \x1b[92m\x1b[1mBedrock\x1b[92m\x1b[1m!\x1b[0m",
            "legacy ANSI output mismatch"
        );
        require(
            legacy.toHTML() ==
                "<span style=\"color:#55FF55\">Hello "
                "<span style=\"font-weight:900\">Bedrock<span>!</span></span></span>",
            "legacy HTML output mismatch"
        );

        const auto translated = chat.message(bedrock::ProtoDefValue::object({
            {"translate", bedrock::ProtoDefValue::string("chat.type.text")},
            {"with", bedrock::ProtoDefValue::array({
                bedrock::ProtoDefValue::object({
                    {"text", bedrock::ProtoDefValue::string("Steve")},
                    {"color", bedrock::ProtoDefValue::string("aqua")}
                }),
                bedrock::ProtoDefValue::object({
                    {"text", bedrock::ProtoDefValue::string("Hello")},
                    {"color", bedrock::ProtoDefValue::string("green")}
                })
            })}
        }));
        require(translated.toString() == "<Steve> Hello", "translated plain output mismatch");
        require(
            translated.toMotd() ==
                "<" + section + "bSteve" + section + "r> " + section + "aHello" +
                section + "r",
            "translated MOTD output mismatch"
        );
        require(
            translated.toAnsi() ==
                "\x1b[0m<\x1b[96mSteve\x1b[0m> \x1b[92mHello\x1b[0m\x1b[0m",
            "translated ANSI output mismatch"
        );
        require(
            translated.toHTML() ==
                "<span>&lt;<span style=\"color:#55FFFF\">Steve</span>&gt; "
                "<span style=\"color:#55FF55\">Hello</span></span>",
            "translated HTML output mismatch"
        );
        require(translated.length() == 2, "translated message length mismatch");
        require(translated.getText(0) == "Steve", "translated getText(0) mismatch");
        require(translated.getText(1) == "Hello", "translated getText(1) mismatch");

        const auto fallback = chat.message(bedrock::ProtoDefValue::object({
            {"translate", bedrock::ProtoDefValue::string("non.existent.key")},
            {"fallback", bedrock::ProtoDefValue::string("Hello %s!")},
            {"with", bedrock::ProtoDefValue::array({
                bedrock::ProtoDefValue::string("World")
            })},
            {"color", bedrock::ProtoDefValue::string("red")}
        }));
        require(fallback.toString() == "Hello World!", "translation fallback mismatch");
        require(
            fallback.toMotd() == section + "cHello " + section + "cWorld" +
                section + "r" + section + "c!",
            "fallback MOTD mismatch"
        );

        const auto positional = chat.message(bedrock::ProtoDefValue::object({
            {"translate", bedrock::ProtoDefValue::string("%2$s %1$s %%")},
            {"with", bedrock::ProtoDefValue::array({
                bedrock::ProtoDefValue::string("first"),
                bedrock::ProtoDefValue::string("second")
            })}
        }));
        require(positional.toString() == "second first %", "positional format mismatch");
        const auto positionalPercent = chat.message(bedrock::ProtoDefValue::object({
            {"translate", bedrock::ProtoDefValue::string("%1$%")},
            {"with", bedrock::ProtoDefValue::array({
                bedrock::ProtoDefValue::string("yes")
            })}
        }));
        require(positionalPercent.toString() == "yes", "positional percent format mismatch");

        const auto nestedLegacy = chat.message(bedrock::ProtoDefValue::object({
            {"translate", bedrock::ProtoDefValue::string("X%s")},
            {"with", bedrock::ProtoDefValue::array({
                bedrock::ProtoDefValue::string(section + "aY")
            })}
        }));
        require(nestedLegacy.toString() == "XY", "nested legacy plain output mismatch");
        require(
            nestedLegacy.toMotd() == "X" + section + "aY" + section + "r",
            "nested legacy MOTD output mismatch"
        );
        require(
            nestedLegacy.toHTML() ==
                "<span>X<span style=\"color:#55FF55\">Y</span></span>",
            "nested legacy HTML output mismatch"
        );

        const auto rich = chat.message(bedrock::ProtoDefValue::object({
            {"color", bedrock::ProtoDefValue::string("blue")},
            {"translate", bedrock::ProtoDefValue::string("chat.type.text")},
            {"with", bedrock::ProtoDefValue::array({
                bedrock::ProtoDefValue::object({
                    {"text", bedrock::ProtoDefValue::string("IM_U9G")},
                    {"color", bedrock::ProtoDefValue::string("aqua")}
                }),
                bedrock::ProtoDefValue::object({
                    {"text", bedrock::ProtoDefValue::string("yo sup")},
                    {"color", bedrock::ProtoDefValue::string("green")}
                })
            })},
            {"extra", bedrock::ProtoDefValue::array({
                bedrock::ProtoDefValue::object({
                    {"text", bedrock::ProtoDefValue::string("test")},
                    {"color", bedrock::ProtoDefValue::string("#ff0000")},
                    {"strikethrough", bedrock::ProtoDefValue::boolean(true)}
                })
            })}
        }));
        require(
            rich.toHTML() ==
                "<span style=\"color:#5555FF\">&lt;"
                "<span style=\"color:#55FFFF\">IM_U9G</span>&gt; "
                "<span style=\"color:#55FF55\">yo sup</span>"
                "<span style=\"color:rgb(255,0,0);text-decoration:line-through\">"
                "test</span></span>",
            "rich HTML output mismatch"
        );
        require(
            rich.toHTML(
                chat.language(),
                {
                    {"black", "color:#000000"}, {"dark_blue", "color:#0000AA"},
                    {"dark_green", "color:#00AA00"}, {"dark_aqua", "color:#00AAAA"},
                    {"dark_red", "color:#AA0000"}, {"dark_purple", "color:#AA00AA"},
                    {"gold", "color:#FFAA00"}, {"gray", "color:#AAAAAA"},
                    {"dark_gray", "color:#555555"}, {"blue", "color:#5555FF"},
                    {"green", "color:#55FF55"}, {"aqua", "color:#55FFFF"},
                    {"red", "color:#FF5555"}, {"light_purple", "color:#FF55FF"},
                    {"yellow", "color:#FFFF55"}, {"white", "color:#FFFFFF"}
                },
                {"color"}
            ) ==
                "<span style=\"color:#5555FF\">&lt;"
                "<span style=\"color:#55FFFF\">IM_U9G</span>&gt; "
                "<span style=\"color:#55FF55\">yo sup</span>"
                "<span style=\"color:rgb(255,0,0)\">test</span></span>",
            "allowed HTML formats mismatch"
        );

        auto arrayMessage = chat.message(bedrock::ProtoDefValue::array({
            text("A"),
            text("B")
        }));
        require(arrayMessage.toString() == "AB", "array chat message mismatch");
        require(arrayMessage.length() == 2, "array chat length mismatch");
        arrayMessage.append(" " + section + "aC");
        require(arrayMessage.toString() == "AB C", "chat append mismatch");
        require(arrayMessage.clone().toMotd().find("C") != std::string::npos, "chat clone mismatch");

        auto builder = chat.builder()
            .setTranslate("chat.type.text")
            .addWith(bedrock::BedrockMessageBuilder().setText("Alex"))
            .addWith("Builder")
            .setColor("yellow")
            .setClickEvent("suggest_command", "/tell Alex ")
            .setHoverEvent("show_text", text("hover"));
        const auto builtValue = builder.toProtoDefValue();
        require(builtValue.get("clickEvent") != nullptr, "builder click event missing");
        require(builtValue.get("hoverEvent") != nullptr, "builder hover event missing");
        require(chat.message(builder).toString() == "<Alex> Builder", "builder output mismatch");

        const auto parsedJson = chat.fromNotch("{\"text\":\"<tag>&\",\"color\":\"#ff0000\"}");
        require(parsedJson.toString() == "<tag>&", "fromNotch JSON mismatch");
        require(
            parsedJson.toHTML() ==
                "<span style=\"color:rgb(255,0,0)\">&lt;tag&gt;&amp;</span>",
            "fromNotch HTML escaping mismatch"
        );
        require(
            chat.fromNotch("plain " + section + "aok").toString() == "plain ok",
            "fromNotch plain fallback mismatch"
        );
        require(
            chat.fromNotch(
                "{\"text\":\"\\u041f\\u0440\\u0438\\u0432\\u0435\\u0442 "
                "\\ud83d\\ude00\"}"
            ).toString() ==
                "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 "
                "\xf0\x9f\x98\x80",
            "fromNotch unicode escape mismatch"
        );

        require(
            chat.message(std::string(5000, 'x')).toString().size() == 4096,
            "chat length limit mismatch"
        );
        auto deep = text("a");
        for (int i = 0; i < 10; ++i) {
            deep = bedrock::ProtoDefValue::object({
                {"translate", bedrock::ProtoDefValue::string("%1$s")},
                {"with", bedrock::ProtoDefValue::array({std::move(deep)})}
            });
        }
        require(chat.message(std::move(deep)).toString().empty(), "chat depth limit mismatch");

        bedrock::BedrockTextPacket packet;
        packet.type = bedrock::BedrockTextType::Translation;
        packet.needsTranslation = true;
        packet.message = "chat.type.text";
        packet.parameters = {"PacketUser", "Packet text"};
        packet.filteredMessage = "filtered";
        require(
            chat.fromTextPacket(packet).toString() == "<PacketUser> Packet text",
            "Bedrock translation packet conversion mismatch"
        );
        packet.type = bedrock::BedrockTextType::Json;
        packet.needsTranslation = false;
        packet.message = "{\"text\":\"JSON packet\",\"color\":\"green\"}";
        require(
            chat.fromTextPacket(packet).toMotd() == section + "aJSON packet",
            "Bedrock JSON text packet conversion mismatch"
        );

        require(
            bedrock::bedrockTextTypeFromName("2/translation") ==
                bedrock::BedrockTextType::Translation,
            "mapped Bedrock text type parsing mismatch"
        );
        require(
            bedrock::bedrockTextTypeName(bedrock::BedrockTextType::JsonAnnouncement) ==
                "json_announcement",
            "Bedrock text type name mismatch"
        );

        bedrock::api::Packet apiPacket;
        apiPacket.name = "text";
        apiPacket.fields = {
            {"type", "translation"},
            {"needs_translation", "true"},
            {"message", "chat.type.text"},
            {"parameters[1]", "hello"},
            {"parameters[0]", "ApiUser"},
            {"xuid", "123"},
            {"platform_chat_id", "platform"},
            {"filtered_message", "filtered"}
        };
        const auto apiText = bedrock::api::textPacketFromPacket(apiPacket);
        require(apiText.type == bedrock::BedrockTextType::Translation, "API text type mismatch");
        require(apiText.needsTranslation, "API text translation flag mismatch");
        require(
            apiText.parameters == std::vector<std::string>({"ApiUser", "hello"}),
            "API text parameters mismatch"
        );
        require(apiText.filteredMessage == "filtered", "API filtered text mismatch");
        require(
            chat.fromTextPacket(apiText).toString() == "<ApiUser> hello",
            "API text/chat integration mismatch"
        );

        bedrock::ProtoDefPacketEncoder packetEncoder("1.21.100");
        const auto payload = packetEncoder.encodePacket(
            "text",
            bedrock::ProtoDefValue::object({
                {"type", bedrock::ProtoDefValue::string("translation")},
                {"needs_translation", bedrock::ProtoDefValue::boolean(true)},
                {"message", bedrock::ProtoDefValue::string("chat.type.text")},
                {"parameters", bedrock::ProtoDefValue::array({
                    bedrock::ProtoDefValue::string("WireUser"),
                    bedrock::ProtoDefValue::string("Wire text")
                })},
                {"xuid", bedrock::ProtoDefValue::string("456")},
                {"platform_chat_id", bedrock::ProtoDefValue::string("wire-platform")},
                {"filtered_message", bedrock::ProtoDefValue::string("wire-filtered")}
            })
        );
        bedrock::VersionedGamePacket gamePacket;
        gamePacket.name = "text";
        gamePacket.payload = payload;
        const auto versionedText = bedrock::VersionedPayloadReader::readText(gamePacket);
        require(versionedText.type == 2, "versioned text type mismatch");
        require(versionedText.needsTranslation, "versioned translation flag mismatch");
        require(
            versionedText.parameters ==
                std::vector<std::string>({"WireUser", "Wire text"}),
            "versioned text parameters mismatch"
        );
        require(versionedText.xuid == "456", "versioned text xuid mismatch");
        require(
            versionedText.platformChatId == "wire-platform",
            "versioned platform chat id mismatch"
        );
        require(
            versionedText.filteredMessage == "wire-filtered",
            "versioned filtered message mismatch"
        );

        const auto payloadText = bedrock::PacketPayloadReader::readText(payload);
        require(payloadText.type == 2, "payload text type mismatch");
        require(
            payloadText.parameters ==
                std::vector<std::string>({"WireUser", "Wire text"}),
            "payload text parameters mismatch"
        );
        require(payloadText.xuid == "456", "payload text xuid mismatch");
        require(
            payloadText.filteredMessage == "wire-filtered",
            "payload filtered message mismatch"
        );

        std::cout << "bedrock chat smoke ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bedrock chat smoke failed: " << error.what() << '\n';
        return 1;
    }
}
