#include <bedrock/bedrock.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace {

bedrock::PacketValue vec3(float x, float y, float z) {
    return bedrock::object({
        {"x", bedrock::f32(x)},
        {"y", bedrock::f32(y)},
        {"z", bedrock::f32(z)}
    });
}

bedrock::PacketValue makeMovePlayer(
    uint64_t runtimeId,
    float x,
    float y,
    float z,
    uint64_t tick
) {
    return bedrock::object({
        {"runtime_id", bedrock::u64(runtimeId)},
        {"position", vec3(x, y, z)},
        {"pitch", bedrock::f32(0.0f)},
        {"yaw", bedrock::f32(0.0f)},
        {"head_yaw", bedrock::f32(0.0f)},
        {"mode", bedrock::str("normal")},
        {"on_ground", bedrock::boolean(true)},
        {"ridden_runtime_id", bedrock::u64(0)},
        {"tick", bedrock::u64(tick)}
    });
}

} // namespace

int main() {
    auto client = bedrock::createClient({
        .host = "localhost",
        .port = 19132,
        .username = "MediumBot",
        .version = "1.20.40",
        .offline = true,
        .diagnostics = {
            .debug = bedrock::DebugMode::Events,
            .decodePackets = true
        }
    });

    uint64_t runtimeId = 1;
    uint64_t tick = 1;

    client.on("start_game", [&](const bedrock::Packet& packet) {
        if (packet.has("runtime_entity_id")) {
            try {
                runtimeId = std::stoull(packet.get("runtime_entity_id"));
            } catch (...) {
                runtimeId = 1;
            }
        }

        std::cout << "Joined world as runtime_id=" << runtimeId << "\n";

        client.write("request_chunk_radius", bedrock::object({
            {"chunk_radius", bedrock::i32(20)},
            {"max_radius", bedrock::u32(0)}
        }));

        client.write("move_player", makeMovePlayer(
            runtimeId,
            0.0f,
            64.0f,
            0.0f,
            tick++
        ));
    });

    client.on("text", [&](const bedrock::Packet& packet) {
        const std::string message = packet.get("message");
        std::cout << "chat: " << message << "\n";

        if (message == "!jump") {
            client.write("move_player", makeMovePlayer(
                runtimeId,
                0.0f,
                65.0f,
                0.0f,
                tick++
            ));
        }
    });

    client.on("disconnect", [](const bedrock::Packet& packet) {
        std::cout << "Disconnected";
        if (packet.has("reason")) std::cout << " reason=" << packet.get("reason");
        if (packet.has("message")) std::cout << " message=" << packet.get("message");
        std::cout << "\n";
    });

    return client.run();
}
