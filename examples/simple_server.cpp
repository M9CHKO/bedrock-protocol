#include <bedrock/bedrock.hpp>

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    auto server = bedrock::createServer({
        .host = "0.0.0.0",
        .port = 19132,
        .version = "1.20.40",
        .motd = {{"motd", "Bedrock Protocol C++"}},
        .maxPlayers = 3
    });

    server.onConnect([](const bedrock::BedrockServerConnection& connection) {
        std::cout << "connect " << connection.address << ":" << connection.port << "\n";
    });

    server.onAny([](const bedrock::BedrockServerPacketEvent& event) {
        std::cout << "packet " << event.packet.name << "\n";
    });

    server.onJoin([](const bedrock::BedrockServerConnection& connection) {
        std::cout << "join " << connection.address << ":" << connection.port << "\n";
    });

    std::cout << "listening on port " << server.boundPort() << "\n";

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
