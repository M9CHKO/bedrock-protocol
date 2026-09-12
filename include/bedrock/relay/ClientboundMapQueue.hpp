#pragma once

#include <bedrock/protocol/VersionedPacketCodec.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace bedrock {

struct ClientboundMapQueueLimits {
    std::size_t packetsPerFlush = 4;
    std::size_t bytesPerFlush = 512u * 1024u;
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
// retain only the complete encoded packet, not a second copy of its 128x128
// pixel payload. The payload view is reconstructed only for the small batch
// currently being passed to the downstream server.
class ClientboundMapQueue {
public:
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

    bool canEnqueue(const VersionedGamePacket& packet) const noexcept {
        const auto bytes = packet.fullPacket.size();
        // A single packet larger than the configured byte limit must still be
        // forwarded on its own rather than being dropped forever.
        if (entries_.empty()) return true;
        return entries_.size() < limits_.maximumPackets &&
            retainedWireBytes_ <= limits_.maximumBytes &&
            bytes <= limits_.maximumBytes - retainedWireBytes_;
    }

    void enqueue(const VersionedGamePacket& packet) {
        Entry entry;
        entry.packetId = packet.packetId;
        entry.name = packet.name;
        entry.paramsType = packet.paramsType;
        entry.fullPacket = packet.fullPacket;
        entry.payloadOffset = packet.fullPacket.size() >= packet.payload.size()
            ? packet.fullPacket.size() - packet.payload.size()
            : packet.fullPacket.size();

        retainedWireBytes_ += entry.fullPacket.size();
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

    ClientboundMapQueueBatch takeFlush(bool pressure = false) {
        ClientboundMapQueueBatch batch;
        if (entries_.empty()) return batch;

        const auto packetLimit = limits_.packetsPerFlush;
        const auto byteLimit = limits_.bytesPerFlush;
        while (!entries_.empty() && batch.packets.size() < packetLimit) {
            const auto nextBytes = entries_.front().fullPacket.size();
            if (!batch.packets.empty() &&
                nextBytes > byteLimit - std::min(byteLimit, batch.wireBytes)) {
                break;
            }

            auto entry = std::move(entries_.front());
            entries_.pop_front();
            retainedWireBytes_ -= nextBytes;
            batch.wireBytes += nextBytes;
            batch.packets.push_back(std::move(entry).restore());
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
        retainedWireBytes_ = 0;
        stats_ = {};
        return snapshot;
    }

    bool empty() const noexcept {
        return entries_.empty();
    }

private:
    struct Entry {
        std::uint32_t packetId = 0;
        std::string name;
        std::string paramsType;
        std::vector<std::uint8_t> fullPacket;
        std::size_t payloadOffset = 0;

        VersionedGamePacket restore() && {
            VersionedGamePacket packet;
            packet.packetId = packetId;
            packet.name = std::move(name);
            packet.paramsType = std::move(paramsType);
            packet.fullPacket = std::move(fullPacket);
            const auto offset = std::min(payloadOffset, packet.fullPacket.size());
            packet.payload.assign(
                packet.fullPacket.begin() + static_cast<std::ptrdiff_t>(offset),
                packet.fullPacket.end()
            );
            return packet;
        }
    };

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
        stats_.queuedBytes = retainedWireBytes_;
    }

    ClientboundMapQueueLimits limits_;
    std::deque<Entry> entries_;
    std::size_t retainedWireBytes_ = 0;
    ClientboundMapQueueStats stats_;
};

} // namespace bedrock
