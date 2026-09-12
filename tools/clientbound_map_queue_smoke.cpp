#include <bedrock/relay/ClientboundMapQueue.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#ifndef BEDROCK_MAP_STRESS_COUNT
#define BEDROCK_MAP_STRESS_COUNT 0
#endif

namespace {

constexpr std::size_t payloadBytes = BEDROCK_MAP_STRESS_COUNT > 0
    ? 81950u
    : 16u * 1024u;

bedrock::VersionedGamePacket mapPacket(std::uint32_t sequence) {
    bedrock::VersionedGamePacket packet;
    packet.packetId = 0x43;
    packet.name = "clientbound_map_item_data";
    packet.paramsType = "packet_map_item_data";
    packet.fullPacket.resize(payloadBytes + 2);
    packet.fullPacket[0] = 0x43;
    packet.fullPacket[1] = static_cast<std::uint8_t>(sequence & 0xffu);
    packet.payload.assign(packet.fullPacket.begin() + 1, packet.fullPacket.end());
    for (std::size_t i = 5; i < packet.fullPacket.size(); ++i) {
        packet.fullPacket[i] = static_cast<std::uint8_t>(
            (i * 31u + static_cast<std::size_t>(sequence) * 17u) & 0xffu
        );
        packet.payload[i - 1] = packet.fullPacket[i];
    }
    packet.fullPacket[1] = static_cast<std::uint8_t>(sequence);
    packet.fullPacket[2] = static_cast<std::uint8_t>(sequence >> 8u);
    packet.fullPacket[3] = static_cast<std::uint8_t>(sequence >> 16u);
    packet.fullPacket[4] = static_cast<std::uint8_t>(sequence >> 24u);
    std::copy(
        packet.fullPacket.begin() + 1,
        packet.fullPacket.begin() + 5,
        packet.payload.begin()
    );
    return packet;
}

std::uint32_t sequenceOf(const bedrock::VersionedGamePacket& packet) {
    return static_cast<std::uint32_t>(packet.fullPacket[1]) |
        (static_cast<std::uint32_t>(packet.fullPacket[2]) << 8u) |
        (static_cast<std::uint32_t>(packet.fullPacket[3]) << 16u) |
        (static_cast<std::uint32_t>(packet.fullPacket[4]) << 24u);
}

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[CLIENTBOUND-MAP-QUEUE-SMOKE] " << message << "\n";
    }
    return condition;
}

bool runCount(std::size_t count) {
    bedrock::ClientboundMapQueue queue({
        .packetsPerFlush = 4,
        .bytesPerFlush = 512u * 1024u,
        .maximumPackets = 4096,
        .maximumBytes = 256u * 1024u * 1024u
    });
    for (std::size_t sequence = 0; sequence < count; ++sequence) {
        auto packet = mapPacket(static_cast<std::uint32_t>(sequence));
        queue.recordReceived(packet.fullPacket.size());
        auto prepared = queue.prepare(packet);
        if (!queue.canEnqueue(prepared)) {
            return check(false, "unexpected hard-limit pressure at " +
                std::to_string(sequence) + "/" + std::to_string(count));
        }
        queue.enqueue(std::move(prepared));
    }

    std::vector<std::uint32_t> observed;
    observed.reserve(count);
    while (!queue.empty()) {
        auto batch = queue.takeFlush();
        if (!check(!batch.packets.empty(), "empty flush from non-empty queue")) {
            return false;
        }
        if (!check(batch.packets.size() <= 4, "packet flush limit exceeded")) {
            return false;
        }
        if (!check(
                batch.wireBytes <= 512u * 1024u || batch.packets.size() == 1,
                "byte flush limit exceeded")) {
            return false;
        }
        for (const auto& packet : batch.packets) {
            if (!check(
                    packet.payload.size() + 1 == packet.fullPacket.size(),
                    "payload was not reconstructed from compact storage")) {
                return false;
            }
            observed.push_back(sequenceOf(packet));
        }
        queue.markForwarded(batch);
    }

    bool ok = true;
    ok &= check(observed.size() == count, "missing or duplicate map packets");
    for (std::size_t i = 0; i < observed.size(); ++i) {
        ok &= check(observed[i] == i, "map order changed at " + std::to_string(i));
        if (!ok) break;
    }
    const auto stats = queue.stats();
    ok &= check(stats.received == count, "received counter mismatch");
    ok &= check(stats.enqueued == count, "enqueued counter mismatch");
    ok &= check(stats.forwarded == count, "forwarded counter mismatch");
    ok &= check(stats.queuedPackets == 0 && stats.queuedBytes == 0,
        "queue did not drain completely");
    ok &= check(stats.peakQueuePackets == count, "peak packet count mismatch");
#if BEDROCK_MAP_STRESS_COUNT > 0
    ok &= check(
        stats.peakQueueBytes < count * (payloadBytes + 2),
        "3600-map queue did not use compact storage"
    );
#endif
    return ok;
}

bool runPressureCase() {
    bedrock::ClientboundMapQueue queue({
        .packetsPerFlush = 2,
        .bytesPerFlush = payloadBytes * 2 + 8,
        .maximumPackets = 2,
        .maximumBytes = payloadBytes * 2 + 8
    });
    auto first = mapPacket(1);
    auto second = mapPacket(2);
    auto third = mapPacket(3);
    queue.enqueue(queue.prepare(first));
    queue.enqueue(queue.prepare(second));
    auto preparedThird = queue.prepare(third);
    bool ok = check(
        !queue.canEnqueue(preparedThird),
        "hard limit was not enforced"
    );
    auto pressure = queue.takeFlush(true);
    queue.markForwarded(pressure);
    ok &= check(
        queue.canEnqueue(preparedThird),
        "pressure flush did not make room"
    );
    queue.enqueue(std::move(preparedThird));
    auto tail = queue.takeFlush();
    queue.markForwarded(tail);
    const auto stats = queue.stats();
    ok &= check(stats.pressureFlushes == 1, "pressure counter mismatch");
    ok &= check(stats.forwarded == 3, "pressure case lost a packet");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
#if BEDROCK_MAP_STRESS_COUNT > 0
    ok &= runCount(BEDROCK_MAP_STRESS_COUNT);
#else
    for (const auto count : {64u, 256u, 512u, 1024u}) {
        ok &= runCount(count);
    }
    ok &= runPressureCase();
#endif
    if (!ok) return 1;
    std::cout << "[CLIENTBOUND-MAP-QUEUE-SMOKE] ok";
#if BEDROCK_MAP_STRESS_COUNT > 0
    std::cout << ": " << BEDROCK_MAP_STRESS_COUNT << " maps";
#endif
    std::cout << "\n";
    return 0;
}
