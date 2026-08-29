#pragma once

#include <bedrock/debug/ProtocolTypeTsvIndex.hpp>
#include <bedrock/generated/GeneratedProtocolTypes.hpp>
#include <bedrock/protodef/ProtoDefEncoder.hpp>
#include <bedrock/protodef/ProtoDefPacketVariables.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>
#include <bedrock/protodef/ProtoDefWriter.hpp>

#include <optional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bedrock {

class ProtoDefPacketEncoder {
public:
    explicit ProtoDefPacketEncoder(
        std::string version,
        ProtocolTypeTsvIndex typeIndex = ProtocolTypeTsvIndex(),
        ProtoDefVariableStorePtr variables = {}
    )
        : version_(std::move(version)),
          typeIndex_(std::move(typeIndex)),
          variables_(variables ? std::move(variables) : makeProtoDefVariableStore()) {}

    ProtoDefPacketEncoder(
        std::string version,
        ProtoDefVariableStorePtr variables
    ) : ProtoDefPacketEncoder(
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

    std::vector<uint8_t> encodePacket(
        const std::string& packetName,
        const ProtoDefValue& value
    ) const {
        detail::updateItemPaletteFromValue(packetName, value, variables_);

        auto typeJson = resolveType("packet_" + packetName);
        if (!typeJson.has_value()) {
            throw std::runtime_error("packet schema not found: " + packetName);
        }

        ProtoDefWriter writer;

        ProtoDefEncoder encoder([this](const std::string& typeName) {
            return this->resolveType(typeName);
        });
        encoder.setVariables(variables_->snapshot());

        encoder.encode(*typeJson, value, writer);
        return writer.take();
    }

private:
    std::string version_;
    ProtocolTypeTsvIndex typeIndex_;
    ProtoDefVariableStorePtr variables_;

    std::optional<std::string> resolveType(const std::string& typeName) const {
        auto fromIndex = typeIndex_.findTypeJson(version_, typeName);
        if (fromIndex.has_value()) {
            return fromIndex;
        }

        return bedrock::generatedProtocolTypeJson(version_, typeName);
    }
};

}
