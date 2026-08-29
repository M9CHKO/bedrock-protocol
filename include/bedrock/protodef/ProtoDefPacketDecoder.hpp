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
#include <string>
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
        return decodePacketImpl(packetName, payload, false);
    }

private:
    std::string version_;
    ProtocolTypeTsvIndex typeIndex_;
    ProtoDefVariableStorePtr variables_;

    std::vector<ProtoDefField> decodePacketImpl(
        const std::string& packetName,
        const std::vector<uint8_t>& payload,
        bool bestEffort
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

        PacketFieldCursor cursor(payload);
        ProtoDefReader reader(cursor);
        ProtoDefContext context;
        std::vector<ProtoDefField> out;

        ProtoDefDecoder decoder([this](const std::string& typeName) {
            return this->resolveType(typeName);
        });
        decoder.setVariables(variables_->snapshot());

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

        detail::updateItemPaletteFromFields(packetName, out, variables_);
        return out;
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

 
