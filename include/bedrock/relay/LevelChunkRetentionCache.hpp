#pragma once

#include <bedrock/protocol/VersionedPacketCodec.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bedrock {

inline constexpr std::size_t DefaultLevelChunkRetentionMaximumBytes =
    256u * 1024u * 1024u;

struct RetainedLevelChunkKey {
    int32_t dimension = 0;
    int32_t x = 0;
    int32_t z = 0;

    bool operator==(const RetainedLevelChunkKey&) const = default;
};

struct LevelChunkRetentionStats {
    bool enabled = false;
    uint32_t configuredRadiusChunks = 0;
    uint32_t effectiveRadiusChunks = 0;
    bool publisherCenterKnown = false;
    int32_t publisherCenterChunkX = 0;
    int32_t publisherCenterChunkZ = 0;
    bool activeDimensionKnown = false;
    int32_t activeDimension = 0;
    std::size_t residentChunks = 0;
    std::size_t residentBytes = 0;
    std::size_t maximumBytes = 0;
    uint64_t observedLevelChunks = 0;
    uint64_t storedLevelChunks = 0;
    uint64_t replacedLevelChunks = 0;
    uint64_t skippedOutsideRadius = 0;
    uint64_t evictedOutsideRadius = 0;
    uint64_t evictedForMemory = 0;
    uint64_t parseFailures = 0;
    uint64_t worldResets = 0;
};

struct LevelChunkRetentionUpdate {
    bool recognized = false;
    bool stored = false;
    bool replaced = false;
    bool dimensionChanged = false;
    std::size_t evictedOutsideRadius = 0;
    std::size_t evictedForMemory = 0;
};

// Stores the exact final level_chunk bytes already sent to one downstream
// Bedrock session. NetworkChunkPublisherUpdate remains the mechanism that
// tells Minecraft not to discard those chunks; this cache is the relay-side
// backing store and is intentionally bounded to protect mobile processes.
class LevelChunkRetentionCache {
public:
    explicit LevelChunkRetentionCache(
        std::size_t maximumBytes = DefaultLevelChunkRetentionMaximumBytes
    ) : maximumBytes_(std::max<std::size_t>(maximumBytes, 1u)) {}

    void configure(bool enabled, uint32_t radiusChunks) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = enabled;
        configuredRadiusChunks_ = radiusChunks;
        effectiveRadiusChunks_ = std::max(
            publisherRadiusChunks_,
            configuredRadiusChunks_
        );
        if (!enabled_) {
            clearWorldUnlocked();
            effectiveRadiusChunks_ = configuredRadiusChunks_;
            return;
        }
        if (!publisherCenterKnown_) {
            effectiveRadiusChunks_ = configuredRadiusChunks_;
        }
        (void) evictOutsideRadiusUnlocked();
        (void) evictForMemoryUnlocked();
    }

    LevelChunkRetentionUpdate observeLevelChunk(
        const VersionedGamePacket& packet
    ) noexcept {
        LevelChunkRetentionUpdate update;
        if (packet.name != "level_chunk") return update;
        update.recognized = true;

        RetainedLevelChunkKey key;
        try {
            key = readKey(packet);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (enabled_) ++parseFailures_;
            return update;
        }

        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!enabled_) return update;
            ++observedLevelChunks_;

            if (activeDimensionKnown_ && activeDimension_ != key.dimension) {
                clearWorldUnlocked();
                ++worldResets_;
                update.dimensionChanged = true;
            }
            activeDimensionKnown_ = true;
            activeDimension_ = key.dimension;

            if (publisherCenterKnown_ && !insideRadiusUnlocked(key)) {
                ++skippedOutsideRadius_;
                return update;
            }

            std::vector<uint8_t> bytes = packet.fullPacket;
            if (bytes.empty()) {
                VersionedPacketCodec::writeVarUInt(bytes, packet.packetId);
                bytes.insert(bytes.end(), packet.payload.begin(), packet.payload.end());
            }

            auto found = chunks_.find(key);
            if (found != chunks_.end()) {
                residentBytes_ -= found->second.bytes.size();
                found->second.bytes = std::move(bytes);
                found->second.sequence = ++sequence_;
                residentBytes_ += found->second.bytes.size();
                ++replacedLevelChunks_;
                update.replaced = true;
            } else {
                RetainedLevelChunk entry;
                entry.bytes = std::move(bytes);
                entry.sequence = ++sequence_;
                const auto [inserted, didInsert] = chunks_.emplace(
                    key,
                    std::move(entry)
                );
                if (!didInsert) return update;
                residentBytes_ += inserted->second.bytes.size();
            }
            ++storedLevelChunks_;
            update.stored = true;
            update.evictedForMemory = evictForMemoryUnlocked();
            return update;
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (enabled_) ++parseFailures_;
            return update;
        }
    }

    LevelChunkRetentionUpdate updatePublisherWindow(
        int32_t centerBlockX,
        int32_t centerBlockZ,
        uint32_t effectiveRadiusBlocks
    ) noexcept {
        LevelChunkRetentionUpdate update;
        update.recognized = true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) return update;

        publisherCenterKnown_ = true;
        publisherCenterChunkX_ = floorDivideBy16(centerBlockX);
        publisherCenterChunkZ_ = floorDivideBy16(centerBlockZ);
        publisherRadiusChunks_ = static_cast<uint32_t>(
            (static_cast<uint64_t>(effectiveRadiusBlocks) + 15u) / 16u
        );
        effectiveRadiusChunks_ = std::max(
            configuredRadiusChunks_,
            publisherRadiusChunks_
        );
        update.evictedOutsideRadius = evictOutsideRadiusUnlocked();
        return update;
    }

    void resetWorld() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        clearWorldUnlocked();
        effectiveRadiusChunks_ = configuredRadiusChunks_;
        ++worldResets_;
    }

    void resetSession() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        clearWorldUnlocked();
        effectiveRadiusChunks_ = configuredRadiusChunks_;
        observedLevelChunks_ = 0;
        storedLevelChunks_ = 0;
        replacedLevelChunks_ = 0;
        skippedOutsideRadius_ = 0;
        evictedOutsideRadius_ = 0;
        evictedForMemory_ = 0;
        parseFailures_ = 0;
        worldResets_ = 0;
        sequence_ = 0;
    }

    LevelChunkRetentionStats stats() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        LevelChunkRetentionStats out;
        out.enabled = enabled_;
        out.configuredRadiusChunks = configuredRadiusChunks_;
        out.effectiveRadiusChunks = effectiveRadiusChunks_;
        out.publisherCenterKnown = publisherCenterKnown_;
        out.publisherCenterChunkX = publisherCenterChunkX_;
        out.publisherCenterChunkZ = publisherCenterChunkZ_;
        out.activeDimensionKnown = activeDimensionKnown_;
        out.activeDimension = activeDimension_;
        out.residentChunks = chunks_.size();
        out.residentBytes = residentBytes_;
        out.maximumBytes = maximumBytes_;
        out.observedLevelChunks = observedLevelChunks_;
        out.storedLevelChunks = storedLevelChunks_;
        out.replacedLevelChunks = replacedLevelChunks_;
        out.skippedOutsideRadius = skippedOutsideRadius_;
        out.evictedOutsideRadius = evictedOutsideRadius_;
        out.evictedForMemory = evictedForMemory_;
        out.parseFailures = parseFailures_;
        out.worldResets = worldResets_;
        return out;
    }

    bool contains(int32_t dimension, int32_t x, int32_t z) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return chunks_.contains({dimension, x, z});
    }

    std::optional<std::vector<uint8_t>> packetBytes(
        int32_t dimension,
        int32_t x,
        int32_t z
    ) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = chunks_.find({dimension, x, z});
        if (found == chunks_.end()) return std::nullopt;
        return found->second.bytes;
    }

private:
    struct RetainedLevelChunkKeyHash {
        std::size_t operator()(const RetainedLevelChunkKey& key) const noexcept {
            std::size_t seed = std::hash<int32_t>{}(key.dimension);
            seed ^= std::hash<int32_t>{}(key.x) + 0x9e3779b9u +
                (seed << 6u) + (seed >> 2u);
            seed ^= std::hash<int32_t>{}(key.z) + 0x9e3779b9u +
                (seed << 6u) + (seed >> 2u);
            return seed;
        }
    };

    struct RetainedLevelChunk {
        std::vector<uint8_t> bytes;
        uint64_t sequence = 0;
    };

    mutable std::mutex mutex_;
    std::unordered_map<
        RetainedLevelChunkKey,
        RetainedLevelChunk,
        RetainedLevelChunkKeyHash
    > chunks_;
    std::size_t maximumBytes_;
    std::size_t residentBytes_ = 0;
    bool enabled_ = false;
    uint32_t configuredRadiusChunks_ = 0;
    uint32_t effectiveRadiusChunks_ = 0;
    uint32_t publisherRadiusChunks_ = 0;
    bool publisherCenterKnown_ = false;
    int32_t publisherCenterChunkX_ = 0;
    int32_t publisherCenterChunkZ_ = 0;
    bool activeDimensionKnown_ = false;
    int32_t activeDimension_ = 0;
    uint64_t sequence_ = 0;
    uint64_t observedLevelChunks_ = 0;
    uint64_t storedLevelChunks_ = 0;
    uint64_t replacedLevelChunks_ = 0;
    uint64_t skippedOutsideRadius_ = 0;
    uint64_t evictedOutsideRadius_ = 0;
    uint64_t evictedForMemory_ = 0;
    uint64_t parseFailures_ = 0;
    uint64_t worldResets_ = 0;

    static RetainedLevelChunkKey readKey(const VersionedGamePacket& packet) {
        std::size_t offset = 0;
        const auto readZigZag32 = [&packet](std::size_t& cursor) {
            const uint32_t raw = VersionedPacketCodec::readVarUInt(
                packet.payload,
                cursor
            );
            return static_cast<int32_t>(
                (raw >> 1u) ^ (0u - (raw & 1u))
            );
        };
        RetainedLevelChunkKey key;
        key.x = readZigZag32(offset);
        key.z = readZigZag32(offset);
        key.dimension = readZigZag32(offset);
        return key;
    }

    static int32_t floorDivideBy16(int32_t value) noexcept {
        const int64_t wide = value;
        if (wide >= 0) return static_cast<int32_t>(wide / 16);
        return static_cast<int32_t>(-((-wide + 15) / 16));
    }

    uint64_t distanceFromPublisherUnlocked(
        const RetainedLevelChunkKey& key
    ) const noexcept {
        const auto dx = static_cast<uint64_t>(std::max<int64_t>(
            static_cast<int64_t>(key.x) - publisherCenterChunkX_,
            static_cast<int64_t>(publisherCenterChunkX_) - key.x
        ));
        const auto dz = static_cast<uint64_t>(std::max<int64_t>(
            static_cast<int64_t>(key.z) - publisherCenterChunkZ_,
            static_cast<int64_t>(publisherCenterChunkZ_) - key.z
        ));
        return std::max(dx, dz);
    }

    bool insideRadiusUnlocked(const RetainedLevelChunkKey& key) const noexcept {
        if (!publisherCenterKnown_) return true;
        return distanceFromPublisherUnlocked(key) <= effectiveRadiusChunks_;
    }

    std::size_t evictOutsideRadiusUnlocked() noexcept {
        if (!publisherCenterKnown_) return 0;
        std::size_t evicted = 0;
        for (auto it = chunks_.begin(); it != chunks_.end();) {
            if (insideRadiusUnlocked(it->first)) {
                ++it;
                continue;
            }
            residentBytes_ -= it->second.bytes.size();
            it = chunks_.erase(it);
            ++evicted;
        }
        evictedOutsideRadius_ += evicted;
        return evicted;
    }

    std::size_t evictForMemoryUnlocked() noexcept {
        std::size_t evicted = 0;
        while (residentBytes_ > maximumBytes_ && !chunks_.empty()) {
            auto victim = chunks_.end();
            uint64_t victimDistance = 0;
            for (auto it = chunks_.begin(); it != chunks_.end(); ++it) {
                const uint64_t distance = publisherCenterKnown_
                    ? distanceFromPublisherUnlocked(it->first)
                    : 0;
                if (victim == chunks_.end() || distance > victimDistance ||
                    (distance == victimDistance &&
                     it->second.sequence < victim->second.sequence)) {
                    victim = it;
                    victimDistance = distance;
                }
            }
            residentBytes_ -= victim->second.bytes.size();
            chunks_.erase(victim);
            ++evicted;
        }
        evictedForMemory_ += evicted;
        return evicted;
    }

    void clearWorldUnlocked() noexcept {
        chunks_.clear();
        residentBytes_ = 0;
        publisherCenterKnown_ = false;
        publisherRadiusChunks_ = 0;
        publisherCenterChunkX_ = 0;
        publisherCenterChunkZ_ = 0;
        activeDimensionKnown_ = false;
        activeDimension_ = 0;
    }
};

} // namespace bedrock
