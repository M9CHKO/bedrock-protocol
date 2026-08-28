#include <bedrock/bedrock.hpp>

#include <iostream>

int main() {
    auto client = bedrock::createClient({
        .host = "localhost",
        .port = 19132,
        .username = "PacketBot",
        .version = "1.20.40",
        .offline = true,
        .diagnostics = {
            .debug = bedrock::DebugMode::Json,
            .decodePackets = true
        }
    });

    client.on("packet", [](const bedrock::Packet& packet) {
        std::cout << "packet " << packet.name << " id=" << packet.id << "\n";
    });

    client.on("start_game", [&client](const bedrock::Packet&) {
        std::cout << "Joined world\n";

        client.write("request_chunk_radius", bedrock::object({
            {"chunk_radius", bedrock::i32(20)},
            {"max_radius", bedrock::u32(0)}
        }));

        for (const auto& [name, fields] : client.queuedPacketValues()) {
            std::cout << "outgoing packet requested: " << name
                      << " fields=" << fields.objectValue.size() << "\n";
        }
    });

    client.on("text", [](const bedrock::Packet& packet) {
        auto changed = packet.fields;
        changed["message"] = "[local copy] " + packet.get("message");

        std::cout << changed["message"] << "\n";
    });

    client.on("disconnect", [](const bedrock::Packet& packet) {
        std::cout << "Disconnected";
        if (packet.has("reason")) std::cout << " reason=" << packet.get("reason");
        if (packet.has("message")) std::cout << " message=" << packet.get("message");
        std::cout << "\n";
    });

    return client.run();
}
