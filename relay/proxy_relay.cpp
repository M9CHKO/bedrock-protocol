#include <bedrock/bedrock.hpp>

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <thread>

namespace {

struct ElytraFlyState {
    bool enabled = true;
    bool gliding = false;
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    double pitch = 0.0;
    double yaw = 0.0;
    uint64_t runtimeEntityId = 0;
    double speed = 2.2;
};

double degToRad(double value) {
    return value * 3.14159265358979323846 / 180.0;
}

bool isFinite(double value) {
    return std::isfinite(value);
}

uint64_t firstUInt(
    const bedrock::RelayPacketEvent& packet,
    std::initializer_list<const char*> keys
) {
    for (const auto* key : keys) {
        if (packet.has(key)) {
            return packet.getUInt(key);
        }
    }
    return 0;
}

void sendElytraMotion(bedrock::RelayPlayer& player, const ElytraFlyState& state) {
    if (!state.enabled || !state.gliding || state.runtimeEntityId == 0) {
        return;
    }

    double dx = 0.0;
    double dy = 0.0;
    double dz = 0.0;

    if (state.up || (!state.down && !state.left && !state.right)) {
        dx = -std::sin(degToRad(state.yaw));
        dy = -std::sin(degToRad(state.pitch));
        dz = std::cos(degToRad(state.yaw));
    }
    if (state.down) {
        dx = std::sin(degToRad(state.yaw));
        dy = std::sin(degToRad(state.pitch));
        dz = -std::cos(degToRad(state.yaw));
    }
    if (state.left) {
        dx = -std::sin(degToRad(state.yaw - 90.0));
        dy = 0.0;
        dz = std::cos(degToRad(state.yaw - 90.0));
    }
    if (state.right) {
        dx = -std::sin(degToRad(state.yaw + 90.0));
        dy = 0.0;
        dz = std::cos(degToRad(state.yaw + 90.0));
    }

    if (!isFinite(dx)) dx = 0.0;
    if (!isFinite(dy)) dy = 0.0;
    if (!isFinite(dz)) dz = 0.0;

    try {
        player.queue("set_entity_motion", {
            {"runtime_entity_id", bedrock::u64(state.runtimeEntityId)},
            {"velocity", bedrock::object({
                {"x", bedrock::f32(static_cast<float>(dx * state.speed))},
                {"y", bedrock::f32(static_cast<float>(dy * state.speed))},
                {"z", bedrock::f32(static_cast<float>(dz * state.speed))}
            })}
        });
    } catch (const std::exception& e) {
        std::cerr << "[elytra-fly] failed to send set_entity_motion: "
                  << e.what() << "\n";
    }
}

} // namespace

int main() {
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    bedrock::Relay relay({
        .version = "1.21.2",
        .host = "0.0.0.0",
        .port = 19132,
        .motd = "Bedrock Protocol C++ Relay",
        .username = "RelayBot",
        .offline = false,
        .destination = {
            .host = "cpe.ign.gg",
            .port = 19132
        }
    });

    relay.onError([](const std::string& message) {
        std::cerr << "[relay error] " << message << "\n";
    });

    relay.onStatus([](const bedrock::BedrockLiveRelayStatus& status) {
        std::cout << "[relay status]"
                  << " listening=" << status.listening
                  << " downstream=" << status.downstreamJoined
                  << " upstream_started=" << status.upstreamStarted
                  << " upstream_ready=" << status.upstreamReady
                  << " port=" << status.boundPort
                  << "\n";
    });

    relay.onConnect([](bedrock::RelayPlayer& player) {
        std::cout << "[relay] New connection "
                  << player.connection.address << ":"
                  << player.connection.port << "\n";

        auto elytra = std::make_shared<ElytraFlyState>();
        auto* relayPlayer = &player;

        player.on("clientbound", [elytra](bedrock::RelayPacketEvent& packet) {
            if (packet.name == "start_game") {
                elytra->runtimeEntityId = firstUInt(packet, {
                    "runtime_entity_id",
                    "runtime_id",
                    "entity_id_self"
                });
                std::cout << "[elytra-fly] runtime_entity_id="
                          << elytra->runtimeEntityId << "\n";
            }

            if (packet.name == "text") {
                std::cout << "[server chat] " << packet.get("message") << "\n";
            }

            // Example: send a packet to the Minecraft client.
            // player.queue("text", {
            //     {"type", bedrock::str("raw")},
            //     {"needs_translation", bedrock::boolean(false)},
            //     {"source_name", bedrock::str("")},
            //     {"message", bedrock::str("Hello from C++ relay")},
            //     {"xuid", bedrock::str("")},
            //     {"platform_chat_id", bedrock::str("")}
            // });
        });

        player.on("serverbound", [elytra, relayPlayer](bedrock::RelayPacketEvent& packet, bedrock::RelayPacketDestination& des) {
            if (packet.name == "player_auth_input") {
                elytra->pitch = packet.getDouble("pitch", elytra->pitch);
                elytra->yaw = packet.getDouble("yaw", elytra->yaw);
                elytra->up = packet.getBool("input_data.up", elytra->up);
                elytra->down = packet.getBool("input_data.down", elytra->down);
                elytra->left = packet.getBool("input_data.left", elytra->left);
                elytra->right = packet.getBool("input_data.right", elytra->right);

                if (packet.getBool("input_data.start_gliding")) {
                    elytra->gliding = true;
                    std::cout << "[elytra-fly] start_gliding\n";
                }
                if (packet.getBool("input_data.stop_gliding")) {
                    elytra->gliding = false;
                    std::cout << "[elytra-fly] stop_gliding\n";
                }

                sendElytraMotion(*relayPlayer, *elytra);
            }

            if (packet.name == "move_player") {
                elytra->pitch = packet.getDouble("pitch", elytra->pitch);
                elytra->yaw = packet.getDouble("yaw", elytra->yaw);
                const auto runtimeId = firstUInt(packet, {
                    "runtime_id",
                    "runtime_entity_id",
                    "entity_id_self"
                });
                if (elytra->runtimeEntityId == 0 && runtimeId != 0) {
                    elytra->runtimeEntityId = runtimeId;
                }
            }

            if (packet.name == "player_action") {
                const auto runtimeId = firstUInt(packet, {
                    "runtime_entity_id",
                    "runtime_id",
                    "entity_id_self"
                });
                if (elytra->runtimeEntityId == 0 && runtimeId != 0) {
                    elytra->runtimeEntityId = runtimeId;
                }

                const auto action = packet.get("action");
                if (action == "start_glide") {
                    elytra->gliding = true;
                    std::cout << "[elytra-fly] start_glide\n";
                }
                if (action == "stop_glide") {
                    elytra->gliding = false;
                    std::cout << "[elytra-fly] stop_glide\n";
                }

                sendElytraMotion(*relayPlayer, *elytra);
            }

            if (packet.name == "text") {
                std::cout << "[client chat] " << packet.get("message") << "\n";

                // Example: edit packet params before forwarding to upstream.
                // packet.set("message", packet.get("message") + " [relay]");
            }

            // Example: cancel forwarding to the upstream server.
            // if (packet.name == "command_request" && packet.get("command") == "/blocked") {
            //     des.cancel();
            // }

            // Example: send a packet to the upstream server.
            // player.upstream.queue("text", {
            //     {"type", bedrock::str("chat")},
            //     {"needs_translation", bedrock::boolean(false)},
            //     {"source_name", bedrock::str("RelayBot")},
            //     {"message", bedrock::str("Hello upstream")},
            //     {"xuid", bedrock::str("")},
            //     {"platform_chat_id", bedrock::str("")}
            // });
        });
    });

    relay.listen();
    std::cout << "Relay listening on 0.0.0.0:19132\n";

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
