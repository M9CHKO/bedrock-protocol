#pragma once

#include <bedrock/legacy/Packet.hpp>
#include <bedrock/events/BedrockPacketEvent.hpp>

namespace bedrock {

inline legacy::Packet packetFromEvent(const BedrockPacketEvent& event) {
    legacy::Packet packet;

    packet.name = event.packetName;
    packet.size = event.rawPacket.size();
    packet.buffer = event.payload;
    packet.fullBuffer = event.rawPacket;
    packet.decodeError = event.decodeError;

    for (const auto& field : event.fields) {
        packet.params[field.name] = field.value;
    }

    return packet;
}

} // namespace bedrock
