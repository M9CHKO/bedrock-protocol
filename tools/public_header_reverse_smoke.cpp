#include <bedrock/PacketFromEvent.hpp>
#include <bedrock/legacy/Client.hpp>
#include <bedrock/bot/BedrockBot.hpp>
#include <bedrock/BedrockProtocol.hpp>
#include <bedrock/BedrockProtocolCpp.hpp>
#include <bedrock/client/BedrockClient.hpp>
#include <bedrock/Packet.hpp>
#include <bedrock/Client.hpp>

#include <type_traits>
#include <utility>

static_assert(std::is_same_v<
    decltype(bedrock::createClient(std::declval<bedrock::Options>())),
    bedrock::Client
>);

static_assert(std::is_same_v<
    decltype(bedrock::createClient(std::declval<bedrock::ClientOptions>())),
    bedrock::Client
>);

static_assert(std::is_same_v<bedrock::Packet, bedrock::api::Packet>);

int main() {
    bedrock::legacy::Client legacyClient;
    legacyClient.on("packet", [](bedrock::legacy::Packet&) {});
    return 0;
}
