#include <bedrock/Client.hpp>
#include <bedrock/BedrockProtocol.hpp>
#include <bedrock/BedrockProtocolCpp.hpp>
#include <bedrock/Packet.hpp>
#include <bedrock/PacketFromEvent.hpp>
#include <bedrock/bedrock.hpp>
#include <bedrock/bot/BedrockBot.hpp>
#include <bedrock/client/BedrockClient.hpp>
#include <bedrock/legacy/Client.hpp>

#include <type_traits>
#include <utility>

static_assert(std::is_same_v<
    decltype(bedrock::createClient(std::declval<bedrock::Options>())),
    bedrock::Client
>);

static_assert(std::is_same_v<
    decltype(bedrock::createBedrockClient(
        std::declval<bedrock::BedrockClientOptions>()
    )),
    bedrock::BedrockClient
>);

int main() {
    bedrock::Packet packet;
    bedrock::legacy::Packet legacyPacket;
    bedrock::legacy::Client legacyClient;
    legacyClient.on("packet", [](bedrock::legacy::Packet&) {});
    legacyClient.emit(std::move(legacyPacket));
    return packet.fields.empty() ? 0 : 1;
}
