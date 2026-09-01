#pragma once

#include <bedrock/protocol/VersionedPayloadReader.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bedrock {

struct TrackedCameraPosition {
    bool known = false;
    bool inputTickKnown = false;
    uint64_t runtimeId = 0;
    uint64_t inputTick = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
    uint64_t updatedAtMs = 0;
};

struct TrackedEntityPosition {
    uint64_t runtimeId = 0;
    std::string type;
    std::string label;
    bool player = false;
    bool item = false;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float width = 0.8f;
    float height = 1.8f;
    float distanceSquared = 0.0f;
    uint64_t updatedAtMs = 0;
};

struct EntityPositionTrackerSnapshot {
    TrackedCameraPosition camera;
    std::vector<TrackedEntityPosition> entities;
    std::size_t totalTrackedEntities = 0;
    uint64_t recognizedPackets = 0;
    uint64_t decodedPackets = 0;
    uint64_t parseFailures = 0;
};

// Tracks the small, stable position prefixes of Bedrock movement/spawn
// packets. It deliberately does not decode entity metadata or mutate packets,
// so telemetry failures can never affect transparent relay forwarding.
class EntityPositionTracker {
public:
    void observeServerbound(const VersionedGamePacket& packet) noexcept {
        if (packet.name != "player_auth_input" &&
            packet.name != "move_player") {
            return;
        }

        std::lock_guard lock(mutex_);
        ++recognizedPackets_;
        try {
            if (packet.name == "player_auth_input") {
                readPlayerAuthInputLocked(packet);
            } else {
                readMovePlayerLocked(packet, true);
            }
            ++decodedPackets_;
        } catch (...) {
            ++parseFailures_;
        }
    }

    // Decodes PlayerAuthInput and applies its dedicated camera-forward
    // vector while holding one lock. This prevents readers from observing
    // the packet's body rotation for a fraction of a frame before the true
    // camera orientation is installed.
    bool observeServerboundWithCameraForward(
        const VersionedGamePacket& packet,
        float forwardX,
        float forwardY,
        float forwardZ,
        uint64_t inputTick = 0,
        bool inputTickKnown = false
    ) noexcept {
        if (packet.name != "player_auth_input") {
            observeServerbound(packet);
            return false;
        }

        std::lock_guard lock(mutex_);
        ++recognizedPackets_;
        try {
            readPlayerAuthInputLocked(packet);
            const bool applied = applyCameraForwardLocked(
                forwardX,
                forwardY,
                forwardZ,
                inputTick,
                inputTickKnown
            );
            ++decodedPackets_;
            return applied;
        } catch (...) {
            ++parseFailures_;
            return false;
        }
    }

    // PlayerAuthInput carries a dedicated camera forward vector on modern
    // Bedrock versions. It may differ from the player's body rotation (for
    // example while flying or when a camera preset is active), so prefer it
    // for screen-space projection whenever the packet decoder exposes it.
    bool observeCameraForward(
        float forwardX,
        float forwardY,
        float forwardZ
    ) noexcept {
        std::lock_guard lock(mutex_);
        return applyCameraForwardLocked(forwardX, forwardY, forwardZ);
    }

    void observeClientbound(const VersionedGamePacket& packet) noexcept {
        if (!isRecognizedClientbound(packet.name)) return;

        std::lock_guard lock(mutex_);
        ++recognizedPackets_;
        try {
            if (packet.name == "start_game") {
                readStartGameLocked(packet);
            } else if (packet.name == "change_dimension") {
                readChangeDimensionLocked(packet);
            } else if (packet.name == "add_player") {
                readAddPlayerLocked(packet);
            } else if (packet.name == "add_entity") {
                readAddEntityLocked(packet);
            } else if (packet.name == "remove_entity") {
                readRemoveEntityLocked(packet);
            } else if (packet.name == "move_entity") {
                readMoveEntityLocked(packet);
            } else if (packet.name == "move_entity_delta") {
                readMoveEntityDeltaLocked(packet);
            } else if (packet.name == "move_player") {
                readMovePlayerLocked(packet, false);
            } else if (packet.name == "player_list") {
                readPlayerListLocked(packet);
            } else if (packet.name == "take_item_entity") {
                readTakeItemEntityLocked(packet);
            }
            ++decodedPackets_;
        } catch (...) {
            ++parseFailures_;
        }
    }

    /** Adds a dropped item after the versioned packet decoder skipped Item. */
    void observeDecodedItemEntity(
        int64_t uniqueId,
        uint64_t runtimeId,
        std::string label,
        float x,
        float y,
        float z
    ) noexcept {
        std::lock_guard lock(mutex_);
        ++recognizedPackets_;
        try {
            requirePosition(x, y, z);
            eraseRuntimeLocked(runtimeId);
            if (runtimeId == camera_.runtimeId) return;

            EntityRecord entity;
            entity.runtimeId = runtimeId;
            entity.uniqueId = uniqueId;
            entity.type = "minecraft:item";
            entity.label = label.empty() ? "Предмет" : std::move(label);
            entity.item = true;
            entity.x = x;
            entity.y = y;
            entity.z = z;
            entity.width = 0.25f;
            entity.height = 0.25f;
            entity.updatedAtMs = monotonicMilliseconds();
            entities_[runtimeId] = std::move(entity);
            if (uniqueId != 0) uniqueToRuntime_[uniqueId] = runtimeId;
            trimLocked();
            ++decodedPackets_;
        } catch (...) {
            ++parseFailures_;
        }
    }

    EntityPositionTrackerSnapshot snapshot(
        float maximumDistance = 320.0f,
        std::size_t maximumEntities = 96
    ) const {
        std::lock_guard lock(mutex_);

        EntityPositionTrackerSnapshot out;
        out.camera = camera_;
        out.totalTrackedEntities = entities_.size();
        out.recognizedPackets = recognizedPackets_;
        out.decodedPackets = decodedPackets_;
        out.parseFailures = parseFailures_;
        out.entities.reserve(std::min(maximumEntities, entities_.size()));

        const float maximumDistanceSquared =
            maximumDistance * maximumDistance;
        for (const auto& [runtimeId, entity] : entities_) {
            if (runtimeId == camera_.runtimeId) continue;

            auto copy = entity;
            if (camera_.known) {
                const float dx = copy.x - camera_.x;
                const float dy = copy.y - camera_.y;
                const float dz = copy.z - camera_.z;
                copy.distanceSquared = dx * dx + dy * dy + dz * dz;
                if (!std::isfinite(copy.distanceSquared) ||
                    copy.distanceSquared > maximumDistanceSquared) {
                    continue;
                }
            }
            out.entities.push_back(std::move(copy));
        }

        std::sort(
            out.entities.begin(),
            out.entities.end(),
            [](const auto& left, const auto& right) {
                if (left.player != right.player) return left.player;
                if (left.item != right.item) return !left.item;
                return left.distanceSquared < right.distanceSquared;
            }
        );
        if (out.entities.size() > maximumEntities) {
            out.entities.resize(maximumEntities);
        }
        return out;
    }

    TrackedCameraPosition cameraSnapshot() const noexcept {
        std::lock_guard lock(mutex_);
        return camera_;
    }

    void clear() noexcept {
        std::lock_guard lock(mutex_);
        clearSessionLocked();
    }

private:
    static constexpr float PlayerEyeHeight = 1.62f;

    struct EntityRecord : TrackedEntityPosition {
        int64_t uniqueId = 0;
        std::string uuid;
    };

    mutable std::mutex mutex_;
    TrackedCameraPosition camera_;
    std::unordered_map<uint64_t, EntityRecord> entities_;
    std::unordered_map<int64_t, uint64_t> uniqueToRuntime_;
    std::unordered_map<std::string, uint64_t> uuidToRuntime_;
    uint64_t recognizedPackets_ = 0;
    uint64_t decodedPackets_ = 0;
    uint64_t parseFailures_ = 0;

    bool applyCameraForwardLocked(
        float forwardX,
        float forwardY,
        float forwardZ,
        uint64_t inputTick = 0,
        bool inputTickKnown = false
    ) noexcept {
        if (!camera_.known || !std::isfinite(forwardX) ||
            !std::isfinite(forwardY) || !std::isfinite(forwardZ)) {
            return false;
        }

        const float length = std::sqrt(
            forwardX * forwardX +
            forwardY * forwardY +
            forwardZ * forwardZ
        );
        if (!std::isfinite(length) || length < 0.0001f) return false;

        const float normalizedX = forwardX / length;
        const float normalizedY = std::clamp(forwardY / length, -1.0f, 1.0f);
        const float normalizedZ = forwardZ / length;
        constexpr float RadiansToDegrees = 57.29577951308232f;
        camera_.pitch = std::asin(-normalizedY) * RadiansToDegrees;
        const float horizontalLength = std::sqrt(
            normalizedX * normalizedX + normalizedZ * normalizedZ
        );
        const float vectorYaw = std::atan2(-normalizedX, normalizedZ) *
            RadiansToDegrees;
        // Yaw becomes mathematically undefined when the camera points nearly
        // straight up/down. A few float bits in X/Z can otherwise turn a
        // microscopic touch into a 90-180 degree screen-space jump. The
        // Only fall back to PlayerAuthInput yaw extremely close to the pole,
        // where the forward vector truly cannot encode yaw. The old 0.14
        // threshold started replacing camera_orientation above about 82° and
        // visibly displaced creator/flight cameras at otherwise stable views.
        constexpr float StableYawBlendStart = 0.002f;
        constexpr float StableYawBlendEnd = 0.02f;
        float yawWeight = std::clamp(
            (horizontalLength - StableYawBlendStart) /
                (StableYawBlendEnd - StableYawBlendStart),
            0.0f,
            1.0f
        );
        yawWeight = yawWeight * yawWeight * (3.0f - 2.0f * yawWeight);
        float yawDelta = std::fmod(
            vectorYaw - camera_.yaw + 540.0f,
            360.0f
        ) - 180.0f;
        camera_.yaw += yawDelta * yawWeight;
        camera_.inputTick = inputTick;
        camera_.inputTickKnown = inputTickKnown;
        camera_.updatedAtMs = monotonicMilliseconds();
        return true;
    }

    static bool isRecognizedClientbound(std::string_view name) noexcept {
        return name == "start_game" || name == "change_dimension" ||
            name == "add_player" || name == "add_entity" ||
            name == "remove_entity" || name == "move_entity" ||
            name == "move_entity_delta" || name == "move_player" ||
            name == "player_list" || name == "take_item_entity";
    }

    static uint64_t monotonicMilliseconds() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }

    static bool finitePosition(float x, float y, float z) noexcept {
        constexpr float MaximumCoordinate = 40'000'000.0f;
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
            std::abs(x) <= MaximumCoordinate &&
            std::abs(y) <= MaximumCoordinate &&
            std::abs(z) <= MaximumCoordinate;
    }

    static void requirePosition(float x, float y, float z) {
        if (!finitePosition(x, y, z)) {
            throw std::runtime_error("invalid entity position");
        }
    }

    static std::string readUuid(VersionedPayloadCursor& cursor) {
        static constexpr char Hex[] = "0123456789abcdef";
        std::string out(32, '0');
        for (std::size_t index = 0; index < 16; ++index) {
            const auto byte = cursor.readU8();
            out[index * 2] = Hex[(byte >> 4) & 0x0f];
            out[index * 2 + 1] = Hex[byte & 0x0f];
        }
        return out;
    }

    static std::pair<float, float> dimensionsFor(
        std::string_view type,
        bool player
    ) noexcept {
        if (player) return {0.6f, 1.8f};
        if (type.ends_with("ender_dragon")) return {8.0f, 4.0f};
        if (type.ends_with("ghast")) return {4.0f, 4.0f};
        if (type.ends_with("wither")) return {0.9f, 3.5f};
        if (type.ends_with("wither_skeleton")) return {0.7f, 2.4f};
        if (type.ends_with("enderman")) return {0.6f, 2.9f};
        if (type.ends_with("iron_golem")) return {1.4f, 2.7f};
        if (type.ends_with("warden")) return {0.9f, 2.9f};
        if (type.ends_with("ravager")) return {1.2f, 1.9f};
        if (type.ends_with("cave_spider")) return {0.7f, 0.5f};
        if (type.ends_with("spider")) return {1.4f, 0.9f};
        if (type.ends_with("chicken")) return {0.4f, 0.7f};
        if (type.ends_with("rabbit")) return {0.4f, 0.5f};
        if (type.ends_with("bee")) return {0.6f, 0.6f};
        if (type.ends_with("silverfish") ||
            type.ends_with("endermite")) return {0.4f, 0.3f};
        if (type.ends_with("creeper")) return {0.6f, 1.7f};
        if (type.ends_with("drowned") || type.ends_with("piglin")) {
            return {0.6f, 1.95f};
        }
        if (type.ends_with("zombie") || type.ends_with("skeleton") ||
            type.ends_with("blaze") || type.ends_with("witch") ||
            type.ends_with("villager") || type.ends_with("villager_v2") ||
            type.ends_with("pillager") || type.ends_with("vindicator")) {
            return {0.6f, 1.8f};
        }
        if (type.ends_with("horse") || type.ends_with("donkey") ||
            type.ends_with("mule")) return {1.4f, 1.6f};
        if (type.ends_with("camel")) return {1.7f, 2.375f};
        if (type.ends_with("cow") || type.ends_with("mooshroom")) {
            return {0.9f, 1.4f};
        }
        if (type.ends_with("sheep") || type.ends_with("goat")) {
            return {0.9f, 1.3f};
        }
        if (type.ends_with("wolf")) return {0.6f, 0.85f};
        if (type.ends_with("llama")) return {0.9f, 1.87f};
        if (type.ends_with("panda")) return {1.125f, 1.25f};
        if (type.ends_with("polar_bear")) return {1.3f, 1.4f};
        if (type.ends_with("fox")) return {1.25f, 0.5f};
        if (type.ends_with("cat") || type.ends_with("ocelot")) {
            return {0.3f, 0.35f};
        }
        if (type.ends_with("hoglin") || type.ends_with("zoglin")) {
            return {1.4f, 1.4f};
        }
        if (type.ends_with("phantom")) return {0.9f, 0.5f};
        if (type.ends_with("shulker")) return {1.0f, 1.0f};
        if (type.ends_with("turtle")) return {1.2f, 0.4f};
        if (type.ends_with("dolphin")) return {0.9f, 0.6f};
        if (type.ends_with("cod")) return {0.5f, 0.25f};
        if (type.ends_with("salmon")) return {0.7f, 0.5f};
        if (type.ends_with("tropicalfish")) return {0.6f, 0.6f};
        if (type.ends_with("pufferfish")) return {0.7f, 0.7f};
        if (type.ends_with("axolotl")) return {0.7f, 0.42f};
        if (type.ends_with("glow_squid") || type.ends_with("squid")) {
            return {0.8f, 0.8f};
        }
        if (type.ends_with("vex")) return {0.4f, 0.8f};
        if (type.ends_with("parrot")) return {0.5f, 0.9f};
        return {0.8f, 1.8f};
    }

    static bool trackableEntityType(std::string_view type) noexcept {
        static constexpr std::array<std::string_view, 34> Excluded {
            "minecraft:item",
            "minecraft:xp_orb",
            "minecraft:arrow",
            "minecraft:spectral_arrow",
            "minecraft:thrown_trident",
            "minecraft:snowball",
            "minecraft:egg",
            "minecraft:ender_pearl",
            "minecraft:eye_of_ender_signal",
            "minecraft:fireball",
            "minecraft:small_fireball",
            "minecraft:dragon_fireball",
            "minecraft:wither_skull",
            "minecraft:shulker_bullet",
            "minecraft:llama_spit",
            "minecraft:evocation_fang",
            "minecraft:falling_block",
            "minecraft:tnt",
            "minecraft:lightning_bolt",
            "minecraft:area_effect_cloud",
            "minecraft:fishing_hook",
            "minecraft:fireworks_rocket",
            "minecraft:firework_rocket",
            "minecraft:painting",
            "minecraft:leash_knot",
            "minecraft:boat",
            "minecraft:chest_boat",
            "minecraft:minecart",
            "minecraft:hopper_minecart",
            "minecraft:command_block_minecart",
            "minecraft:chest_minecart",
            "minecraft:tnt_minecart",
            "minecraft:spawner_minecart",
            "minecraft:camera"
        };
        return !type.empty() &&
            std::find(Excluded.begin(), Excluded.end(), type) ==
                Excluded.end();
    }

    void clearSessionLocked() noexcept {
        camera_ = {};
        entities_.clear();
        uniqueToRuntime_.clear();
        uuidToRuntime_.clear();
    }

    void clearEntitiesLocked() noexcept {
        entities_.clear();
        uniqueToRuntime_.clear();
        uuidToRuntime_.clear();
    }

    void trimLocked() {
        constexpr std::size_t MaximumStoredEntities = 1024;
        if (entities_.size() <= MaximumStoredEntities) return;

        auto oldest = entities_.end();
        for (auto it = entities_.begin(); it != entities_.end(); ++it) {
            if (oldest == entities_.end() ||
                it->second.updatedAtMs < oldest->second.updatedAtMs) {
                oldest = it;
            }
        }
        if (oldest != entities_.end()) eraseRuntimeLocked(oldest->first);
    }

    void eraseRuntimeLocked(uint64_t runtimeId) noexcept {
        const auto found = entities_.find(runtimeId);
        if (found == entities_.end()) return;
        if (found->second.uniqueId != 0) {
            uniqueToRuntime_.erase(found->second.uniqueId);
        }
        if (!found->second.uuid.empty()) {
            uuidToRuntime_.erase(found->second.uuid);
        }
        entities_.erase(found);
    }

    void updateCameraLocked(
        float x,
        float y,
        float z,
        float pitch,
        float yaw
    ) {
        requirePosition(x, y, z);
        if (!std::isfinite(pitch) || !std::isfinite(yaw)) {
            throw std::runtime_error("invalid camera rotation");
        }
        camera_.known = true;
        camera_.x = x;
        camera_.y = y;
        camera_.z = z;
        camera_.pitch = pitch;
        camera_.yaw = yaw;
        camera_.inputTickKnown = false;
        camera_.inputTick = 0;
        camera_.updatedAtMs = monotonicMilliseconds();
    }

    void readStartGameLocked(const VersionedGamePacket& packet) {
        VersionedPayloadCursor cursor(packet.payload);
        (void) cursor.readVarLong();
        const auto runtimeId = cursor.readVarULong();
        (void) cursor.readVarInt();
        const float x = cursor.readF32LE();
        const float y = cursor.readF32LE();
        const float z = cursor.readF32LE();
        const float pitch = cursor.readF32LE();
        const float yaw = cursor.readF32LE();

        clearSessionLocked();
        camera_.runtimeId = runtimeId;
        updateCameraLocked(x, y + PlayerEyeHeight, z, pitch, yaw);
    }

    void readChangeDimensionLocked(const VersionedGamePacket& packet) {
        VersionedPayloadCursor cursor(packet.payload);
        (void) cursor.readVarInt();
        const float x = cursor.readF32LE();
        const float y = cursor.readF32LE();
        const float z = cursor.readF32LE();
        clearEntitiesLocked();
        updateCameraLocked(
            x,
            y + PlayerEyeHeight,
            z,
            camera_.pitch,
            camera_.yaw
        );
    }

    void readPlayerAuthInputLocked(const VersionedGamePacket& packet) {
        VersionedPayloadCursor cursor(packet.payload);
        const float pitch = cursor.readF32LE();
        const float yaw = cursor.readF32LE();
        const float x = cursor.readF32LE();
        const float y = cursor.readF32LE();
        const float z = cursor.readF32LE();
        updateCameraLocked(x, y, z, pitch, yaw);
    }

    void readAddPlayerLocked(const VersionedGamePacket& packet) {
        VersionedPayloadCursor cursor(packet.payload);
        const auto uuid = readUuid(cursor);
        auto username = cursor.readString();
        const auto runtimeId = cursor.readVarULong();
        (void) cursor.readString();
        const float x = cursor.readF32LE();
        const float y = cursor.readF32LE();
        const float z = cursor.readF32LE();
        requirePosition(x, y, z);
        if (runtimeId == camera_.runtimeId) return;

        EntityRecord entity;
        entity.runtimeId = runtimeId;
        entity.type = "minecraft:player";
        entity.label = username.empty() ? "Игрок" : std::move(username);
        entity.player = true;
        entity.x = x;
        entity.y = y;
        entity.z = z;
        const auto [width, height] = dimensionsFor(entity.type, true);
        entity.width = width;
        entity.height = height;
        entity.updatedAtMs = monotonicMilliseconds();
        entity.uuid = uuid;

        eraseRuntimeLocked(runtimeId);
        entities_[runtimeId] = std::move(entity);
        uuidToRuntime_[uuid] = runtimeId;
        trimLocked();
    }

    void readAddEntityLocked(const VersionedGamePacket& packet) {
        VersionedPayloadCursor cursor(packet.payload);
        const auto uniqueId = cursor.readVarLong();
        const auto runtimeId = cursor.readVarULong();
        auto type = cursor.readString();
        const float x = cursor.readF32LE();
        const float y = cursor.readF32LE();
        const float z = cursor.readF32LE();
        requirePosition(x, y, z);

        eraseRuntimeLocked(runtimeId);
        if (!trackableEntityType(type) || runtimeId == camera_.runtimeId) {
            return;
        }

        EntityRecord entity;
        entity.runtimeId = runtimeId;
        entity.uniqueId = uniqueId;
        entity.type = std::move(type);
        const auto prefix = entity.type.find(':');
        entity.label = prefix == std::string::npos
            ? entity.type
            : entity.type.substr(prefix + 1);
        entity.x = x;
        entity.y = y;
        entity.z = z;
        const auto [width, height] = dimensionsFor(entity.type, false);
        entity.width = width;
        entity.height = height;
        entity.updatedAtMs = monotonicMilliseconds();

        entities_[runtimeId] = std::move(entity);
        if (uniqueId != 0) uniqueToRuntime_[uniqueId] = runtimeId;
        trimLocked();
    }

    void readRemoveEntityLocked(const VersionedGamePacket& packet) {
        VersionedPayloadCursor cursor(packet.payload);
        const auto uniqueId = cursor.readVarLong();
        const auto mapped = uniqueToRuntime_.find(uniqueId);
        if (mapped != uniqueToRuntime_.end()) {
            eraseRuntimeLocked(mapped->second);
            return;
        }
        if (uniqueId >= 0) {
            eraseRuntimeLocked(static_cast<uint64_t>(uniqueId));
        }
    }

    void readTakeItemEntityLocked(const VersionedGamePacket& packet) {
        VersionedPayloadCursor cursor(packet.payload);
        eraseRuntimeLocked(cursor.readVarULong());
    }

    void readMoveEntityLocked(const VersionedGamePacket& packet) {
        VersionedPayloadCursor cursor(packet.payload);
        const auto runtimeId = cursor.readVarULong();
        (void) cursor.readU8();
        const float x = cursor.readF32LE();
        const float y = cursor.readF32LE();
        const float z = cursor.readF32LE();
        requirePosition(x, y, z);
        const auto found = entities_.find(runtimeId);
        if (found == entities_.end()) return;
        found->second.x = x;
        found->second.y = y;
        found->second.z = z;
        found->second.updatedAtMs = monotonicMilliseconds();
    }

    void readMoveEntityDeltaLocked(const VersionedGamePacket& packet) {
        VersionedPayloadCursor cursor(packet.payload);
        const auto runtimeId = cursor.readVarULong();
        const auto flags = cursor.readU16LE();
        const auto found = entities_.find(runtimeId);

        float x = found == entities_.end() ? 0.0f : found->second.x;
        float y = found == entities_.end() ? 0.0f : found->second.y;
        float z = found == entities_.end() ? 0.0f : found->second.z;
        if ((flags & 0x01u) != 0) x = cursor.readF32LE();
        if ((flags & 0x02u) != 0) y = cursor.readF32LE();
        if ((flags & 0x04u) != 0) z = cursor.readF32LE();
        if (found == entities_.end()) return;
        requirePosition(x, y, z);
        found->second.x = x;
        found->second.y = y;
        found->second.z = z;
        found->second.updatedAtMs = monotonicMilliseconds();
    }

    void readMovePlayerLocked(
        const VersionedGamePacket& packet,
        bool serverbound
    ) {
        VersionedPayloadCursor cursor(packet.payload);
        const auto runtimeId = cursor.readVarULong();
        const float x = cursor.readF32LE();
        const float y = cursor.readF32LE();
        const float z = cursor.readF32LE();
        const float pitch = cursor.readF32LE();
        const float yaw = cursor.readF32LE();
        (void) cursor.readF32LE();
        requirePosition(x, y, z);

        if (serverbound || runtimeId == camera_.runtimeId) {
            if (camera_.runtimeId == 0) camera_.runtimeId = runtimeId;
            updateCameraLocked(x, y, z, pitch, yaw);
            return;
        }

        const auto found = entities_.find(runtimeId);
        if (found == entities_.end()) return;
        found->second.x = x;
        // MovePlayer uses the Bedrock network eye position, whereas
        // AddPlayer and our render records use the feet/base position.
        found->second.y = y - PlayerEyeHeight;
        found->second.z = z;
        found->second.updatedAtMs = monotonicMilliseconds();
    }

    void readPlayerListLocked(const VersionedGamePacket& packet) {
        VersionedPayloadCursor cursor(packet.payload);
        const auto action = cursor.readU8();
        const auto count = cursor.readVarUInt();
        if (count > 4096) {
            throw std::runtime_error("player list is too large");
        }
        if (action != 1) return;

        for (uint32_t index = 0; index < count; ++index) {
            const auto uuid = readUuid(cursor);
            const auto mapped = uuidToRuntime_.find(uuid);
            if (mapped != uuidToRuntime_.end()) {
                eraseRuntimeLocked(mapped->second);
            }
        }
    }
};

} // namespace bedrock
