#include <bedrock/relay/EntityPositionTracker.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void writeVarUInt(std::vector<uint8_t>& out, uint64_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        if (value != 0) byte |= 0x80;
        out.push_back(byte);
    } while (value != 0);
}

void writeVarInt(std::vector<uint8_t>& out, int64_t value) {
    const auto unsignedValue = static_cast<uint64_t>(value);
    const auto raw = (unsignedValue << 1) ^
        static_cast<uint64_t>(value >> 63);
    writeVarUInt(out, raw);
}

void writeFloat(std::vector<uint8_t>& out, float value) {
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    out.push_back(static_cast<uint8_t>(raw));
    out.push_back(static_cast<uint8_t>(raw >> 8));
    out.push_back(static_cast<uint8_t>(raw >> 16));
    out.push_back(static_cast<uint8_t>(raw >> 24));
}

void writeString(std::vector<uint8_t>& out, const std::string& value) {
    writeVarUInt(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void writeUuid(std::vector<uint8_t>& out, uint8_t seed) {
    for (uint8_t index = 0; index < 16; ++index) {
        out.push_back(static_cast<uint8_t>(seed + index));
    }
}

bedrock::VersionedGamePacket packet(
    std::string name,
    std::vector<uint8_t> payload
) {
    bedrock::VersionedGamePacket out;
    out.name = std::move(name);
    out.payload = std::move(payload);
    return out;
}

bool near(float left, float right) {
    return std::abs(left - right) < 0.001f;
}

} // namespace

int main() {
    bedrock::EntityPositionTracker tracker;

    std::vector<uint8_t> start;
    writeVarInt(start, 42);
    writeVarUInt(start, 42);
    writeVarInt(start, 0);
    writeFloat(start, 100.0f);
    writeFloat(start, 64.0f);
    writeFloat(start, -50.0f);
    writeFloat(start, 10.0f);
    writeFloat(start, 90.0f);
    tracker.observeClientbound(packet("start_game", std::move(start)));

    auto state = tracker.snapshot();
    if (!state.camera.known || state.camera.runtimeId != 42 ||
        !near(state.camera.x, 100.0f) || !near(state.camera.y, 65.62f) ||
        !near(state.camera.pitch, 10.0f) ||
        !near(state.camera.yaw, 90.0f)) {
        std::cerr << "start_game camera was not decoded\n";
        return 1;
    }

    std::vector<uint8_t> addEntity;
    writeVarInt(addEntity, 7);
    writeVarUInt(addEntity, 77);
    writeString(addEntity, "minecraft:zombie");
    writeFloat(addEntity, 110.0f);
    writeFloat(addEntity, 64.0f);
    writeFloat(addEntity, -50.0f);
    tracker.observeClientbound(packet("add_entity", std::move(addEntity)));

    std::vector<uint8_t> ignoredItem;
    writeVarInt(ignoredItem, 8);
    writeVarUInt(ignoredItem, 78);
    writeString(ignoredItem, "minecraft:item");
    writeFloat(ignoredItem, 101.0f);
    writeFloat(ignoredItem, 64.0f);
    writeFloat(ignoredItem, -50.0f);
    tracker.observeClientbound(packet("add_entity", std::move(ignoredItem)));

    state = tracker.snapshot();
    if (state.entities.size() != 1 || state.entities[0].runtimeId != 77 ||
        state.entities[0].type != "minecraft:zombie") {
        std::cerr << "living-entity filter did not work\n";
        return 1;
    }

    tracker.observeDecodedItemEntity(
        8,
        78,
        "minecraft:diamond",
        101.0f,
        64.0f,
        -50.0f
    );
    state = tracker.snapshot();
    if (state.entities.size() != 2 || !state.entities[1].item ||
        state.entities[1].runtimeId != 78 ||
        state.entities[1].label != "minecraft:diamond") {
        std::cerr << "decoded dropped item was not tracked separately\n";
        return 1;
    }

    std::vector<uint8_t> takeItem;
    writeVarUInt(takeItem, 78);
    tracker.observeClientbound(packet("take_item_entity", std::move(takeItem)));
    state = tracker.snapshot();
    if (state.entities.size() != 1 || state.entities[0].item) {
        std::cerr << "taken dropped item was not removed\n";
        return 1;
    }

    std::vector<uint8_t> delta;
    writeVarUInt(delta, 77);
    delta.push_back(0x05);
    delta.push_back(0x00);
    writeFloat(delta, 112.5f);
    writeFloat(delta, -48.5f);
    tracker.observeClientbound(packet("move_entity_delta", std::move(delta)));
    state = tracker.snapshot();
    if (state.entities.size() != 1 ||
        !near(state.entities[0].x, 112.5f) ||
        !near(state.entities[0].z, -48.5f)) {
        std::cerr << "move_entity_delta was not applied\n";
        return 1;
    }

    std::vector<uint8_t> moveEntity;
    writeVarUInt(moveEntity, 77);
    moveEntity.push_back(0x00);
    writeFloat(moveEntity, 113.0f);
    writeFloat(moveEntity, 65.0f);
    writeFloat(moveEntity, -47.0f);
    tracker.observeClientbound(packet("move_entity", std::move(moveEntity)));
    state = tracker.snapshot();
    if (state.entities.size() != 1 ||
        !near(state.entities[0].x, 113.0f) ||
        !near(state.entities[0].y, 65.0f) ||
        !near(state.entities[0].z, -47.0f)) {
        std::cerr << "move_entity base position was not preserved\n";
        return 1;
    }

    std::vector<uint8_t> addPlayer;
    writeUuid(addPlayer, 0x20);
    writeString(addPlayer, "RelayFriend");
    writeVarUInt(addPlayer, 88);
    writeString(addPlayer, "");
    writeFloat(addPlayer, 105.0f);
    writeFloat(addPlayer, 64.0f);
    writeFloat(addPlayer, -50.0f);
    tracker.observeClientbound(packet("add_player", std::move(addPlayer)));
    state = tracker.snapshot();
    if (state.entities.size() != 2 || !state.entities[0].player ||
        state.entities[0].label != "RelayFriend") {
        std::cerr << "add_player was not decoded or prioritised\n";
        return 1;
    }

    std::vector<uint8_t> movePlayer;
    writeVarUInt(movePlayer, 88);
    writeFloat(movePlayer, 106.0f);
    writeFloat(movePlayer, 65.62f);
    writeFloat(movePlayer, -49.0f);
    writeFloat(movePlayer, 0.0f);
    writeFloat(movePlayer, 0.0f);
    writeFloat(movePlayer, 0.0f);
    tracker.observeClientbound(packet("move_player", std::move(movePlayer)));
    state = tracker.snapshot();
    if (!near(state.entities[0].x, 106.0f) ||
        !near(state.entities[0].y, 64.0f) ||
        !near(state.entities[0].z, -49.0f)) {
        std::cerr << "move_player eye-height normalization failed\n";
        return 1;
    }

    std::vector<uint8_t> auth;
    writeFloat(auth, -12.0f);
    writeFloat(auth, 180.0f);
    writeFloat(auth, 102.0f);
    writeFloat(auth, 70.0f);
    writeFloat(auth, -49.0f);
    tracker.observeServerbound(packet("player_auth_input", std::move(auth)));
    state = tracker.snapshot();
    if (!near(state.camera.x, 102.0f) || !near(state.camera.y, 70.0f) ||
        !near(state.camera.pitch, -12.0f) ||
        !near(state.camera.yaw, 180.0f)) {
        std::cerr << "player_auth_input camera was not decoded\n";
        return 1;
    }

    if (!tracker.observeCameraForward(-1.0f, 0.0f, 0.0f)) {
        std::cerr << "camera forward vector was rejected\n";
        return 1;
    }
    state = tracker.snapshot();
    if (!near(state.camera.pitch, 0.0f) ||
        !near(state.camera.yaw, 90.0f)) {
        std::cerr << "camera forward vector did not override body rotation\n";
        return 1;
    }

    const auto recognizedBeforeAtomic = state.recognizedPackets;
    const auto decodedBeforeAtomic = state.decodedPackets;
    std::vector<uint8_t> atomicAuth;
    writeFloat(atomicAuth, 45.0f);
    writeFloat(atomicAuth, -45.0f);
    writeFloat(atomicAuth, 103.0f);
    writeFloat(atomicAuth, 71.0f);
    writeFloat(atomicAuth, -48.0f);
    if (!tracker.observeServerboundWithCameraForward(
            packet("player_auth_input", std::move(atomicAuth)),
            0.0f,
            0.0f,
            1.0f,
            12'345,
            true
        )) {
        std::cerr << "atomic camera-forward update was rejected\n";
        return 1;
    }
    state = tracker.snapshot();
    if (!near(state.camera.x, 103.0f) || !near(state.camera.y, 71.0f) ||
        !near(state.camera.z, -48.0f) ||
        !near(state.camera.pitch, 0.0f) ||
        !near(state.camera.yaw, 0.0f) ||
        !state.camera.inputTickKnown ||
        state.camera.inputTick != 12'345 ||
        state.recognizedPackets != recognizedBeforeAtomic + 1 ||
        state.decodedPackets != decodedBeforeAtomic + 1) {
        std::cerr << "atomic camera sample was not published as one packet\n";
        return 1;
    }

    std::vector<uint8_t> verticalAuth;
    writeFloat(verticalAuth, 89.9f);
    writeFloat(verticalAuth, 37.0f);
    writeFloat(verticalAuth, 103.0f);
    writeFloat(verticalAuth, 71.0f);
    writeFloat(verticalAuth, -48.0f);
    if (!tracker.observeServerboundWithCameraForward(
            packet("player_auth_input", std::move(verticalAuth)),
            0.00001f,
            -1.0f,
            -0.00001f,
            12'346,
            true
        )) {
        std::cerr << "near-vertical camera-forward update was rejected\n";
        return 1;
    }
    state = tracker.snapshot();
    if (!near(state.camera.pitch, 90.0f) ||
        !near(state.camera.yaw, 37.0f)) {
        std::cerr << "near-vertical camera yaw was not stabilised\n";
        return 1;
    }

    const auto cameraOnly = tracker.cameraSnapshot();
    if (!cameraOnly.known || !near(cameraOnly.x, state.camera.x) ||
        !near(cameraOnly.y, state.camera.y) ||
        !near(cameraOnly.z, state.camera.z) ||
        !near(cameraOnly.pitch, state.camera.pitch) ||
        !near(cameraOnly.yaw, state.camera.yaw) ||
        cameraOnly.inputTickKnown != state.camera.inputTickKnown ||
        cameraOnly.inputTick != state.camera.inputTick ||
        cameraOnly.updatedAtMs != state.camera.updatedAtMs) {
        std::cerr << "camera-only snapshot did not match full snapshot\n";
        return 1;
    }

    if (tracker.observeCameraForward(0.0f, 0.0f, 0.0f)) {
        std::cerr << "zero camera forward vector was accepted\n";
        return 1;
    }

    std::vector<uint8_t> removePlayer;
    removePlayer.push_back(1);
    writeVarUInt(removePlayer, 1);
    writeUuid(removePlayer, 0x20);
    tracker.observeClientbound(packet("player_list", std::move(removePlayer)));

    std::vector<uint8_t> removeEntity;
    writeVarInt(removeEntity, 7);
    tracker.observeClientbound(packet("remove_entity", std::move(removeEntity)));
    state = tracker.snapshot();
    if (!state.entities.empty()) {
        std::cerr << "entity removal was not applied\n";
        return 1;
    }

    tracker.observeClientbound(packet("add_entity", {0xff}));
    state = tracker.snapshot();
    if (state.parseFailures != 1 || state.decodedPackets +
            state.parseFailures != state.recognizedPackets) {
        std::cerr << "malformed packet accounting is inconsistent\n";
        return 1;
    }

    std::cout << "entity position tracker smoke passed\n";
    return 0;
}
