#pragma once

#include <bedrock/events/BedrockPacketEvent.hpp>
#include <bedrock/protocol/GamePacket.hpp>
#include <bedrock/protodef/ProtoDefVariables.hpp>

#include <string>

namespace bedrock {

class BedrockPacketEventAdapter {
public:
    static BedrockPacketEvent fromGamePacket(
        const GamePacket& packet,
        const std::string& minecraftVersion,
        ProtoDefVariableStorePtr variables = {}
    );
};

} // namespace bedrock
