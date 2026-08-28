#include <bedrock/bedrock.hpp>

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    auto server = bedrock::createServer({
        .host = "0.0.0.0",
        .port = 19132,
        .version = "1.20.40",
        .motd = {"Bedrock Protocol C++", "Example world"},
        .maxPlayers = 3
    });

    server.on("connect", [](const bedrock::Player& player) {
        std::cout << "connect " << player.address << ":" << player.port << "\n";

        player.onLogin([player](const bedrock::BedrockServerPacketEvent&) {
            const auto profile = player.profile();
            if (profile) {
                std::cout << "login " << profile->name
                          << " xuid=" << profile->xuid << "\n";
            }
        });
        player.on("packet", bedrock::Player::PacketHandler(
            [](const bedrock::BedrockServerPacketEvent& event) {
                std::cout << "packet " << event.packet.name << "\n";
            }
        ));
        player.on("join", bedrock::Player::VoidHandler([player] {
            std::cout << "join " << player.address << ":" << player.port << "\n";
        }));
        player.on("close", bedrock::Player::VoidHandler([player] {
            std::cout << "close " << player.address << ":" << player.port << "\n";
        }));
    });

    std::cout << "listening on port " << server.boundPort() << "\n";

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
