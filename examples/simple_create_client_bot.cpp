#include <bedrock/bedrock.hpp>

#include <iostream>

int main() {
    auto client = bedrock::createClient({
        .host = "localhost",
        .port = 19132,
        .username = "Notch",
        .offline = true,
        .diagnostics = {
            .debug = bedrock::DebugMode::Events,
            .decodePackets = false
        }
    });

    client.on("start_game", [](const bedrock::Packet&) {
        std::cout << "Joined world\n";
    });

    client.on("disconnect", [](const bedrock::Packet& packet) {
        std::cout << "Disconnected";
        if (packet.has("reason")) std::cout << " reason=" << packet.get("reason");
        if (packet.has("message")) std::cout << " message=" << packet.get("message");
        std::cout << "\n";
    });

    client.on("packet", [](const bedrock::Packet& packet) {
        std::cout << "packet " << packet.name << " id=" << packet.id << "\n";
    });

    return client.run();
}
