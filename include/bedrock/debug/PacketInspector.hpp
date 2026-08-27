#pragma once

#include <bedrock/protocol/GamePacket.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefVariables.hpp>

#include <iostream>
#include <string>

namespace bedrock {

class PacketInspector {
public:
    explicit PacketInspector(
        std::string minecraftVersion,
        ProtoDefVariableStorePtr variables = {}
    ) : minecraftVersion_(std::move(minecraftVersion)),
        variables_(variables ? std::move(variables) : makeProtoDefVariableStore()) {}

    ProtoDefVariableStorePtr variableStore() const {
        return variables_;
    }

    void setVariable(std::string key, std::string value) {
        variables_->setVariable(std::move(key), std::move(value));
    }

    void inspect(const GamePacket& packet) const {
        ProtoDefPacketDecoder decoder(minecraftVersion_, variables_);
        auto fields = decoder.decodePacket(packet.name, packet.payload);

        std::cout << "[INSPECT] packet "
                  << packet.name
                  << " id="
                  << packet.packetId
                  << " fields="
                  << fields.size()
                  << "\n";

        for (const auto& field : fields) {
            std::cout << "  "
                      << field.path
                      << "="
                      << field.value
                      << " type="
                      << field.type
                      << " size="
                      << field.size
                      << "\n";
        }
    }

private:
    std::string minecraftVersion_;
    ProtoDefVariableStorePtr variables_;
};

} // namespace bedrock
