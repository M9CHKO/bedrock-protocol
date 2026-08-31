#pragma once

#include <bedrock/debug/PacketFieldDecoder.hpp>
#include <bedrock/debug/ProtocolTypeTsvIndex.hpp>
#include <bedrock/generated/GeneratedProtocolTypes.hpp>
#include <bedrock/protodef/ProtoDefContext.hpp>
#include <bedrock/protodef/ProtoDefDecoder.hpp>
#include <bedrock/protodef/ProtoDefField.hpp>
#include <bedrock/protodef/ProtoDefPacketVariables.hpp>
#include <bedrock/protodef/ProtoDefReader.hpp>

#include <optional>
#include <memory>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bedrock {

class ProtoDefPacketDecoder {
public:
    explicit ProtoDefPacketDecoder(
        std::string version,
        ProtocolTypeTsvIndex typeIndex = ProtocolTypeTsvIndex(),
        ProtoDefVariableStorePtr variables = {}
    )
        : version_(std::move(version)),
          typeIndex_(std::move(typeIndex)),
          variables_(variables ? std::move(variables) : makeProtoDefVariableStore()) {}

    ProtoDefPacketDecoder(
        std::string version,
        ProtoDefVariableStorePtr variables
    ) : ProtoDefPacketDecoder(
            std::move(version),
            ProtocolTypeTsvIndex(),
            std::move(variables)
        ) {}

    void setVariable(std::string key, std::string value) const {
        variables_->setVariable(std::move(key), std::move(value));
    }

    template<typename T>
    void setVariable(std::string key, T value) const {
        variables_->setVariable(std::move(key), value);
    }

    std::optional<std::string> variable(const std::string& key) const {
        return variables_->variable(key);
    }

    ProtoDefVariableStorePtr variableStore() const {
        return variables_;
    }

    std::vector<ProtoDefField> decodePacket(
        const std::string& packetName,
        const std::vector<uint8_t>& payload
    ) const {
        return decodePacketImpl(packetName, payload, true);
    }

    // Node's relay deserializer treats EOF/schema failures as packet parse
    // errors. Keep the long-standing best-effort decoder above for inspectors,
    // while exposing a strict boundary for protocol consumers that need the
    // same failure semantics.
    std::vector<ProtoDefField> decodePacketStrict(
        const std::string& packetName,
        const std::vector<uint8_t>& payload
    ) const {
        return decodePacketImpl(packetName, payload, false, true);
    }

    // Validate the same strict schema boundary without retaining a structured
    // field tree. Transparent relays use this before raw forwarding when no
    // packet handler requested decoded parameters.
    void validatePacketStrict(
        const std::string& packetName,
        const std::vector<uint8_t>& payload
    ) const {
        (void) decodePacketImpl(packetName, payload, false, false);
    }

    // Update connection-scoped variables from a packet without retaining its
    // decoded field tree. This preserves the historical best-effort behavior
    // used for already-serialized outbound packets while avoiding a full
    // item_registry materialization.
    void updatePacketVariables(
        const std::string& packetName,
        const std::vector<uint8_t>& payload
    ) const {
        (void) decodePacketImpl(packetName, payload, true, false);
    }

    // Strictly validate a palette-bearing packet and return only its compact
    // runtime-id table. This is intended for diagnostics and downstream
    // registry checks that do not need every component NBT field.
    std::vector<std::pair<int64_t, std::string>> decodeItemPaletteStrict(
        const std::string& packetName,
        const std::vector<uint8_t>& payload
    ) const {
        std::vector<std::pair<int64_t, std::string>> palette;
        (void) decodePacketImpl(
            packetName,
            payload,
            false,
            false,
            &palette
        );
        return palette;
    }

private:
    std::string version_;
    ProtocolTypeTsvIndex typeIndex_;
    ProtoDefVariableStorePtr variables_;

    std::vector<ProtoDefField> decodePacketImpl(
        const std::string& packetName,
        const std::vector<uint8_t>& payload,
        bool bestEffort,
        bool collectFields = true,
        std::vector<std::pair<int64_t, std::string>>* itemPalette = nullptr
    ) const {
        auto typeJson = resolveType("packet_" + packetName);
        if (!typeJson.has_value()) {
            if (!bestEffort) {
                throw std::runtime_error(
                    "packet schema not found: " + packetName
                );
            }
            return {};
        }

        if (!collectFields && packetName == "item_registry" &&
            supportsCompactItemRegistry(*typeJson)) {
            return decodeCompactItemRegistry(
                payload,
                bestEffort,
                itemPalette
            );
        }

        PacketFieldCursor cursor(payload);
        ProtoDefReader reader(cursor);
        ProtoDefContext context;
        std::vector<ProtoDefField> out;

        ProtoDefDecoder decoder([this](const std::string& typeName) {
            return this->resolveType(typeName);
        });
        decoder.setVariables(variables_->snapshot());
        decoder.setCollectFields(collectFields);

        const bool streamItemPalette =
            !collectFields && detail::packetCarriesItemPalette(packetName);
        bool firstShieldSeen = false;
        bool awaitingShieldRuntimeId = false;
        std::optional<std::string> shieldRuntimeId;
        std::optional<std::string> currentItemName;
        if (streamItemPalette) {
            decoder.setFieldObserver([&](const ProtoDefField& field) {
                if (field.path == "itemstates[].name") {
                    currentItemName = field.value;
                    if (!firstShieldSeen && field.value == "minecraft:shield") {
                        firstShieldSeen = true;
                        awaitingShieldRuntimeId = true;
                    }
                    return;
                }
                if (awaitingShieldRuntimeId &&
                    field.path == "itemstates[].runtime_id") {
                    shieldRuntimeId = field.value;
                    awaitingShieldRuntimeId = false;
                }
                if (itemPalette && currentItemName &&
                    field.path == "itemstates[].runtime_id") {
                    try {
                        itemPalette->emplace_back(
                            std::stoll(field.value),
                            *currentItemName
                        );
                    } catch (const std::exception&) {
                        // The schema remains authoritative and has already
                        // validated the wire integer. Ignore only a value that
                        // cannot be represented in the diagnostic int64 API.
                    }
                    currentItemName.reset();
                }
            });
        }

        if (bestEffort) {
            try {
                decoder.decode(*typeJson, reader, "", out, context);
            } catch (const std::exception&) {
                // Inspector/event compatibility: retain fields decoded before
                // EOF or a schema mismatch.
            }
        } else {
            decoder.decode(*typeJson, reader, "", out, context);
            if (reader.remaining() != 0) {
                throw std::runtime_error(
                    "packet " + packetName + " has " +
                    std::to_string(reader.remaining()) +
                    " unread byte(s)"
                );
            }
        }

        if (collectFields) {
            detail::updateItemPaletteFromFields(packetName, out, variables_);
        } else if (shieldRuntimeId &&
                   detail::isTruthyVariableKey(*shieldRuntimeId)) {
            detail::updateShieldItemId(
                variables_,
                "minecraft:shield",
                *shieldRuntimeId
            );
        }
        return out;
    }

    bool supportsCompactItemRegistry(const std::string& packetType) const {
        static constexpr std::string_view packetSchema =
            R"(["container",[{"name":"itemstates","type":"Itemstates"}]])";
        static constexpr std::string_view itemstatesSchema =
            R"(["array",{"countType":"varint","type":["container",[{"name":"name","type":"string"},{"name":"runtime_id","type":"li16"},{"name":"component_based","type":"bool"},{"name":"version","type":["mapper",{"type":"zigzag32","mappings":{"0":"legacy","1":"data_driven","2":"none"}}]},{"name":"nbt","type":"nbt"}]]}])";
        if (packetType != packetSchema) return false;
        const auto itemstates = resolveType("Itemstates");
        return itemstates.has_value() && *itemstates == itemstatesSchema;
    }

    std::vector<ProtoDefField> decodeCompactItemRegistry(
        const std::vector<uint8_t>& payload,
        bool bestEffort,
        std::vector<std::pair<int64_t, std::string>>* itemPalette
    ) const {
        PacketFieldCursor cursor(payload);
        ProtoDefReader reader(cursor);
        std::optional<std::string> shieldRuntimeId;

        try {
            const auto count = reader.varuint32();
            // Every valid modern entry occupies multiple bytes. Reject an
            // impossible count before an attacker can force a huge empty loop.
            if (count > payload.size()) {
                throw std::runtime_error(
                    "item_registry count exceeds payload size"
                );
            }

            for (uint32_t index = 0; index < count; ++index) {
                try {
                    auto name = reader.string();
                    const auto runtimeId = static_cast<int16_t>(reader.u16le());
                    (void) reader.boolean();
                    (void) reader.zigzag32();
                    skipProtoDefNbt(
                        reader,
                        BedrockNbtEncoding::LittleVarInt
                    );

                    if (itemPalette) {
                        itemPalette->emplace_back(runtimeId, name);
                    }
                    if (!shieldRuntimeId && name == "minecraft:shield") {
                        shieldRuntimeId = std::to_string(runtimeId);
                    }
                } catch (const std::exception& error) {
                    throw std::runtime_error(
                        "at itemstates[" + std::to_string(index) + "]: " +
                        error.what()
                    );
                }
            }

            if (!bestEffort && reader.remaining() != 0) {
                throw std::runtime_error(
                    "packet item_registry has " +
                    std::to_string(reader.remaining()) +
                    " unread byte(s)"
                );
            }
        } catch (const std::exception&) {
            if (!bestEffort) throw;
        }

        if (shieldRuntimeId &&
            detail::isTruthyVariableKey(*shieldRuntimeId)) {
            detail::updateShieldItemId(
                variables_,
                "minecraft:shield",
                *shieldRuntimeId
            );
        }
        return {};
    }

    std::optional<std::string> resolveType(const std::string& typeName) const {
        auto fromIndex = typeIndex_.findTypeJson(version_, typeName);
        if (fromIndex.has_value()) {
            return fromIndex;
        }

        return bedrock::generatedProtocolTypeJson(version_, typeName);
    }
};

}

 
