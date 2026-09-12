#pragma once

#include <bedrock/protocol/VersionedPacketCodec.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

namespace bedrock {

struct ClientboundMapQueueLimits {
    std::size_t packetsPerFlush = 1;
    std::size_t bytesPerFlush = 128u * 1024u;
    std::size_t maximumPackets = 4096;
    std::size_t maximumBytes = 256u * 1024u * 1024u;
};

struct ClientboundMapQueueStats {
    std::uint64_t received = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t forwarded = 0;
    std::uint64_t pressureFlushes = 0;
    std::size_t queuedPackets = 0;
    std::size_t queuedBytes = 0;
    std::size_t peakQueuePackets = 0;
    std::size_t peakQueueBytes = 0;
    std::size_t largestMapPacket = 0;
};

struct ClientboundMapQueueBatch {
    std::vector<VersionedGamePacket> packets;
    std::size_t wireBytes = 0;
};

// A session-owned, wire-preserving queue for map updates. Backlogged entries
// retain a losslessly compressed copy of the complete encoded packet when it
// saves space, rather than expanding the 128x128 pixels into decoded values.
// The exact full packet and payload view are reconstructed only for the small
// batch currently being passed to the downstream server.
class ClientboundMapQueue {
public:
    struct PreparedEntry {
        std::uint32_t packetId = 0;
        std::string name;
        std::string paramsType;
        std::vector<std::uint8_t> storedPacket;
        std::size_t wireBytes = 0;
        std::size_t payloadOffset = 0;
        bool compressed = false;
    };

    explicit ClientboundMapQueue(ClientboundMapQueueLimits limits = {})
        : limits_(normalize(limits)) {}

    const ClientboundMapQueueLimits& limits() const noexcept {
        return limits_;
    }

    void recordReceived(std::size_t wireBytes) noexcept {
        ++stats_.received;
        stats_.largestMapPacket = std::max(
            stats_.largestMapPacket,
            wireBytes
        );
    }

    PreparedEntry prepare(const VersionedGamePacket& packet) const {
        PreparedEntry entry;
        entry.packetId = packet.packetId;
        entry.name = packet.name;
        entry.paramsType = packet.paramsType;
        entry.wireBytes = packet.fullPacket.size();
        entry.payloadOffset = packet.fullPacket.size() >= packet.payload.size()
            ? packet.fullPacket.size() - packet.payload.size()
            : packet.fullPacket.size();
        entry.storedPacket = packet.fullPacket;

        if (!packet.fullPacket.empty()) {
            const auto bound = compressBound(
                static_cast<uLong>(packet.fullPacket.size())
            );
            std::vector<std::uint8_t> compressed(bound);
            uLongf compressedBytes = bound;
            const auto result = compress2(
                reinterpret_cast<Bytef*>(compressed.data()),
                &compressedBytes,
                reinterpret_cast<const Bytef*>(packet.fullPacket.data()),
                static_cast<uLong>(packet.fullPacket.size()),
                Z_BEST_SPEED
            );
            if (result == Z_OK && compressedBytes < packet.fullPacket.size()) {
                compressed.resize(static_cast<std::size_t>(compressedBytes));
                entry.storedPacket = std::move(compressed);
                entry.compressed = true;
            }
        }
        return entry;
    }

    bool canEnqueue(const PreparedEntry& entry) const noexcept {
        const auto bytes = entry.storedPacket.size();
        // A single packet larger than the configured byte limit must still be
        // forwarded on its own rather than being dropped forever.
        if (entries_.empty()) return true;
        return entries_.size() < limits_.maximumPackets &&
            retainedStorageBytes_ <= limits_.maximumBytes &&
            bytes <= limits_.maximumBytes - retainedStorageBytes_;
    }

    bool canEnqueue(const VersionedGamePacket& packet) const {
        return canEnqueue(prepare(packet));
    }

    void enqueue(PreparedEntry entry) {
        retainedStorageBytes_ += entry.storedPacket.size();
        entries_.push_back(std::move(entry));
        ++stats_.enqueued;
        refreshQueueStats();
        stats_.peakQueuePackets = std::max(
            stats_.peakQueuePackets,
            stats_.queuedPackets
        );
        stats_.peakQueueBytes = std::max(
            stats_.peakQueueBytes,
            stats_.queuedBytes
        );
    }

    void enqueue(const VersionedGamePacket& packet) {
        enqueue(prepare(packet));
    }

    ClientboundMapQueueBatch takeFlush(bool pressure = false) {
        ClientboundMapQueueBatch batch;
        if (entries_.empty()) return batch;

        const auto packetLimit = limits_.packetsPerFlush;
        const auto byteLimit = limits_.bytesPerFlush;
        while (!entries_.empty() && batch.packets.size() < packetLimit) {
            const auto nextBytes = entries_.front().wireBytes;
            if (!batch.packets.empty() &&
                nextBytes > byteLimit - std::min(byteLimit, batch.wireBytes)) {
                break;
            }

            auto entry = std::move(entries_.front());
            entries_.pop_front();
            retainedStorageBytes_ -= entry.storedPacket.size();
            batch.wireBytes += nextBytes;
            batch.packets.push_back(restore(std::move(entry)));
        }
        if (pressure && !batch.packets.empty()) {
            ++stats_.pressureFlushes;
        }
        refreshQueueStats();
        return batch;
    }

    void markForwarded(const ClientboundMapQueueBatch& batch) noexcept {
        stats_.forwarded += batch.packets.size();
    }

    ClientboundMapQueueStats stats() const noexcept {
        return stats_;
    }

    ClientboundMapQueueStats clear() noexcept {
        const auto snapshot = stats_;
        entries_.clear();
        retainedStorageBytes_ = 0;
        stats_ = {};
        return snapshot;
    }

    bool empty() const noexcept {
        return entries_.empty();
    }

private:
    static ClientboundMapQueueLimits normalize(
        ClientboundMapQueueLimits limits
    ) noexcept {
        limits.packetsPerFlush = std::max<std::size_t>(limits.packetsPerFlush, 1);
        limits.bytesPerFlush = std::max<std::size_t>(limits.bytesPerFlush, 1);
        limits.maximumPackets = std::max<std::size_t>(limits.maximumPackets, 1);
        limits.maximumBytes = std::max<std::size_t>(limits.maximumBytes, 1);
        return limits;
    }

    void refreshQueueStats() noexcept {
        stats_.queuedPackets = entries_.size();
        stats_.queuedBytes = retainedStorageBytes_;
    }

    static VersionedGamePacket restore(PreparedEntry entry) {
        VersionedGamePacket packet;
        packet.packetId = entry.packetId;
        packet.name = std::move(entry.name);
        packet.paramsType = std::move(entry.paramsType);
        if (entry.compressed) {
            packet.fullPacket.resize(entry.wireBytes);
            uLongf restoredBytes = static_cast<uLongf>(packet.fullPacket.size());
            const auto result = uncompress(
                reinterpret_cast<Bytef*>(packet.fullPacket.data()),
                &restoredBytes,
                reinterpret_cast<const Bytef*>(entry.storedPacket.data()),
                static_cast<uLong>(entry.storedPacket.size())
            );
            if (result != Z_OK || restoredBytes != entry.wireBytes) {
                throw std::runtime_error(
                    "clientbound map queue decompression failed"
                );
            }
        } else {
            packet.fullPacket = std::move(entry.storedPacket);
        }
        const auto offset = std::min(
            entry.payloadOffset,
            packet.fullPacket.size()
        );
        packet.payload.assign(
            packet.fullPacket.begin() + static_cast<std::ptrdiff_t>(offset),
            packet.fullPacket.end()
        );
        return packet;
    }

    ClientboundMapQueueLimits limits_;
    std::deque<PreparedEntry> entries_;
    std::size_t retainedStorageBytes_ = 0;
    ClientboundMapQueueStats stats_;
};

} // namespace bedrock
