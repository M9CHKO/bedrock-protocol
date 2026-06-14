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
    bool hasPosition = false;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double pitch = 0.0;
    double yaw = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    double vz = 0.0;
    uint64_t runtimeEntityId = 0;
    double speed = 1.0;
    bool hasPredictedPosition = false;
    double predictedX = 0.0;
    double predictedY = 0.0;
    double predictedZ = 0.0;
    int correctionCooldownTicks = 0;
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

void computeElytraDelta(const ElytraFlyState& state, double& dx, double& dy, double& dz) {
    dx = 0.0;
    dy = 0.0;
    dz = 0.0;

    if (state.up) {
        dx += -std::sin(degToRad(state.yaw));
        dy += -std::sin(degToRad(state.pitch));
        dz += std::cos(degToRad(state.yaw));
    }
    if (state.down) {
        dx += std::sin(degToRad(state.yaw));
        dy += std::sin(degToRad(state.pitch));
        dz += -std::cos(degToRad(state.yaw));
    }
    if (state.left) {
        dx += -std::sin(degToRad(state.yaw - 90.0));
        dz += std::cos(degToRad(state.yaw - 90.0));
    }
    if (state.right) {
        dx += -std::sin(degToRad(state.yaw + 90.0));
        dz += std::cos(degToRad(state.yaw + 90.0));
    }

    if (!isFinite(dx)) dx = 0.0;
    if (!isFinite(dy)) dy = 0.0;
    if (!isFinite(dz)) dz = 0.0;

    const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length > 0.000001) {
        dx /= length;
        dy /= length;
        dz /= length;
    }

    dx *= state.speed;
    dy *= state.speed;
    dz *= state.speed;
}

bool hasElytraDirectionInput(const ElytraFlyState& state) {
    return state.up || state.down || state.left || state.right;
}

void resetElytraVelocity(ElytraFlyState& state) {
    state.vx = 0.0;
    state.vy = 0.0;
    state.vz = 0.0;
}

void updatePositionFromPacket(ElytraFlyState& state, const bedrock::RelayPacketEvent& packet) {
    if (!packet.has("position.x") ||
        !packet.has("position.y") ||
        !packet.has("position.z")) {
        return;
    }

    state.x = packet.getDouble("position.x", state.x);
    state.y = packet.getDouble("position.y", state.y);
    state.z = packet.getDouble("position.z", state.z);
    state.hasPosition = true;
}

void acceptElytraCorrection(ElytraFlyState& state, const bedrock::RelayPacketEvent& packet) {
    if (!state.gliding) {
        updatePositionFromPacket(state, packet);
        return;
    }

    if (!packet.has("position.x") ||
        !packet.has("position.y") ||
        !packet.has("position.z")) {
        return;
    }

    const double oldX = state.x;
    const double oldY = state.y;
    const double oldZ = state.z;
    updatePositionFromPacket(state, packet);
    resetElytraVelocity(state);
    state.predictedX = state.x;
    state.predictedY = state.y;
    state.predictedZ = state.z;
    state.hasPredictedPosition = true;
    state.correctionCooldownTicks = 2;

    const double dx = state.x - oldX;
    const double dy = state.y - oldY;
    const double dz = state.z - oldZ;
    const double correctionDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (correctionDistance > 0.25) {
        std::cout << "[elytra-fly] correction reset base distance="
                  << correctionDistance << "\n";
    }
}

bool applyElytraAuthInput(bedrock::RelayPacketEvent& packet, ElytraFlyState& state) {
    if (!state.enabled || !state.gliding || !state.hasPosition) {
        return false;
    }

    if (state.correctionCooldownTicks > 0) {
        --state.correctionCooldownTicks;
        packet.set("position.x", state.x);
        packet.set("position.y", state.y);
        packet.set("position.z", state.z);
        packet.set("delta.x", 0.0);
        packet.set("delta.y", 0.0);
        packet.set("delta.z", 0.0);
        return true;
    }

    if (!hasElytraDirectionInput(state)) {
        resetElytraVelocity(state);
        state.predictedX = state.x;
        state.predictedY = state.y;
        state.predictedZ = state.z;
        state.hasPredictedPosition = true;

        packet.set("position.x", state.x);
        packet.set("position.y", state.y);
        packet.set("position.z", state.z);
        packet.set("delta.x", 0.0);
        packet.set("delta.y", 0.0);
        packet.set("delta.z", 0.0);
        return true;
    }

    double dx = 0.0;
    double dy = 0.0;
    double dz = 0.0;
    computeElytraDelta(state, dx, dy, dz);

    state.vx = dx;
    state.vy = dy;
    state.vz = dz;

    state.x += state.vx;
    state.y += state.vy;
    state.z += state.vz;
    state.predictedX = state.x;
    state.predictedY = state.y;
    state.predictedZ = state.z;
    state.hasPredictedPosition = true;

    packet.set("position.x", state.x);
    packet.set("position.y", state.y);
    packet.set("position.z", state.z);
    packet.set("delta.x", state.vx);
    packet.set("delta.y", state.vy);
    packet.set("delta.z", state.vz);

    static uint64_t patchedPackets = 0;
    ++patchedPackets;
    if (patchedPackets == 1 || (patchedPackets % 20) == 0) {
        std::cout << "[elytra-fly] patched player_auth_input"
                  << " pos=(" << state.x << "," << state.y << "," << state.z << ")"
                  << " delta=(" << state.vx << "," << state.vy << "," << state.vz << ")"
                  << "\n";
    }

    return true;
}

} // namespace

int main() {
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    bedrock::Relay relay({
        .version = "1.21.100",
        .host = "0.0.0.0",
        .port = 19132,
        .motd = "Bedrock Protocol C++ Relay",
        .username = "RelayBot",
        .offline = false,
        .autoSyncMovementMutations = true,
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

            if (packet.name == "correct_player_move_prediction") {
                acceptElytraCorrection(*elytra, packet);
                elytra->pitch = packet.getDouble("rotation.x", elytra->pitch);
                elytra->yaw = packet.getDouble("rotation.z", elytra->yaw);
            }

            if (packet.name == "move_player") {
                const auto runtimeId = firstUInt(packet, {
                    "runtime_id",
                    "runtime_entity_id",
                    "entity_id_self"
                });
                if (runtimeId != 0 &&
                    (elytra->runtimeEntityId == 0 ||
                     runtimeId == elytra->runtimeEntityId)) {
                    if (elytra->runtimeEntityId == 0 && runtimeId != 0) {
                        elytra->runtimeEntityId = runtimeId;
                    }
                    acceptElytraCorrection(*elytra, packet);
                    elytra->pitch = packet.getDouble("pitch", elytra->pitch);
                    elytra->yaw = packet.getDouble("yaw", elytra->yaw);
                }
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

        player.on("serverbound", [elytra](bedrock::RelayPacketEvent& packet, bedrock::RelayPacketDestination& des) {
            if (packet.name == "player_auth_input") {
                bool startedGlidingThisTick = false;
                if (!elytra->gliding) {
                    updatePositionFromPacket(*elytra, packet);
                }
                elytra->pitch = packet.getDouble("pitch", elytra->pitch);
                elytra->yaw = packet.getDouble("yaw", elytra->yaw);
                elytra->up = packet.getBool("input_data.up", false);
                elytra->down = packet.getBool("input_data.down", false);
                elytra->left = packet.getBool("input_data.left", false);
                elytra->right = packet.getBool("input_data.right", false);

                if (packet.getBool("input_data.start_gliding")) {
                    const bool wasGliding = elytra->gliding;
                    elytra->gliding = true;
                    if (!wasGliding) {
                        resetElytraVelocity(*elytra);
                        elytra->hasPredictedPosition = false;
                        elytra->correctionCooldownTicks = 0;
                        packet.set("position.x", elytra->x);
                        packet.set("position.y", elytra->y);
                        packet.set("position.z", elytra->z);
                        packet.set("delta.x", 0.0);
                        packet.set("delta.y", 0.0);
                        packet.set("delta.z", 0.0);
                        startedGlidingThisTick = true;
                        std::cout << "[elytra-fly] start_gliding velocity_reset\n";
                    } else {
                        std::cout << "[elytra-fly] start_gliding\n";
                    }
                }
                if (packet.getBool("input_data.stop_gliding")) {
                    elytra->gliding = false;
                    resetElytraVelocity(*elytra);
                    elytra->hasPredictedPosition = false;
                    elytra->correctionCooldownTicks = 0;
                    std::cout << "[elytra-fly] stop_gliding\n";
                }

                if (!startedGlidingThisTick) {
                    applyElytraAuthInput(packet, *elytra);
                }
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
