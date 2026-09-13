#include <bedrock/events/BedrockPacketEventAdapter.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>

#include <string>
#include <utility>

namespace bedrock {

BedrockPacketEvent BedrockPacketEventAdapter::fromGamePacket(
    const GamePacket& packet,
    const std::string& minecraftVersion,
    ProtoDefVariableStorePtr variables
) {
    BedrockPacketEvent event;
    event.version = minecraftVersion;
    event.packetId = packet.packetId;
    event.packetName = packet.name;
    event.rawPacket = packet.fullPacket;
    event.payload = packet.payload;

    try {
        bedrock::ProtoDefPacketDecoder protoDecoder(minecraftVersion, std::move(variables));
        auto protoFields = protoDecoder.decodePacket(packet.name, packet.payload);

        for (const auto& field : protoFields) {
            BedrockPacketEventField out;
            out.name = field.path;
            out.type = field.type;
            out.value = field.value;
            out.offset = field.offset;
            out.size = field.size;
            event.fields.push_back(out);

            auto dot = out.name.rfind('.');
            if (dot != std::string::npos && dot + 1 < out.name.size()) {
                BedrockPacketEventField alias = out;
                alias.name = out.name.substr(dot + 1);
                event.fields.push_back(std::move(alias));
            }
        }
    } catch (const std::exception& e) {
        event.decodeError = e.what();
    }

    return event;
}

} // namespace bedrock
