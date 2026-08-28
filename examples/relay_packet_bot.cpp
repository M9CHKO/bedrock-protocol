#include <bedrock/bedrock.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>

#include <iostream>

int main() {
    bedrock::BedrockRelayOptions options;
    options.clientOptions.minecraftVersion = "1.20.40";
    options.clientOptions.outgoingCompression = bedrock::VersionedMcpeCompression::Uncompressed;
    options.clientOptions.autoResourcePackResponses = false;
    options.clientOptions.autoStartGameInit = false;
    options.enableChunkCaching = false;

    auto relay = bedrock::createPacketRelay(options);
    auto codec = bedrock::VersionedMcpeCodec::forVersion(options.clientOptions.minecraftVersion);
    relay.markDownstreamJoined();
    relay.markUpstreamJoined();

    relay.on("serverbound", [](bedrock::BedrockRelayPacketEvent& event) {
        std::cout << "serverbound " << event.packet.name << "\n";
    });

    relay.on("clientbound", [](bedrock::BedrockRelayPacketEvent& event) {
        std::cout << "clientbound " << event.packet.name << "\n";

        if (event.packet.name == "play_status") {
            event.cancel();
        }
    });

    auto clientCacheStatus = codec.packetCodec().makePacketByName("client_cache_status", {0x01});
    auto serverboundMcpe = codec.encodeMcpePayload(
        {clientCacheStatus},
        bedrock::VersionedMcpeCompression::Uncompressed
    );
    auto serverboundFrame = relay.handleServerboundMcpe(serverboundMcpe);

    std::cout
        << "forwarded serverbound packets="
        << serverboundFrame.forwardedPackets.size()
        << " client_cache_status="
        << static_cast<int>(serverboundFrame.forwardedPackets[0].payload[0])
        << "\n";

    auto playStatus = codec.packetCodec().makePacketByName("play_status", {0x01});
    auto clientboundMcpe = codec.encodeMcpePayload(
        {playStatus},
        bedrock::VersionedMcpeCompression::Uncompressed
    );
    auto clientboundFrame = relay.handleClientboundMcpe(clientboundMcpe);

    std::cout
        << "forwarded clientbound packets="
        << clientboundFrame.forwardedPackets.size()
        << "\n";

    return 0;
}
