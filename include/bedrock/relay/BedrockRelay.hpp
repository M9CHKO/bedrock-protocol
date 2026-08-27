#pragma once

#include <bedrock/Options.hpp>
#include <bedrock/client/BedrockClient.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>

#include <functional>
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bedrock {

enum class BedrockRelayDirection {
    Clientbound,
    Serverbound
};

struct BedrockRelayOptions {
    BedrockClientOptions clientOptions;
    bool forwardServerbound = true;
    bool forwardClientbound = true;

    // Mirrors the JavaScript relay option that forces client_cache_status.
    bool enableChunkCaching = false;
    bool forceClientCacheStatus = true;
    bool skipClientboundLoginSuccess = true;

    // Bedrock servers may send level_chunk before start_game. JS relay holds
    // those chunks until start_game has passed through to the downstream client.
    bool queueClientboundLevelChunksUntilStartGame = true;

    // Old option names kept for source compatibility with early C++ examples.
    bool forwardClientToServer = true;
    bool forwardServerToClient = true;
};

struct BedrockRelayPacketEvent {
    BedrockRelayDirection direction = BedrockRelayDirection::Clientbound;
    // Empty for the pure packet relay. BedrockLiveRelay fills this with the
    // downstream Player identity so concurrent upstreams remain routable.
    std::string sessionId;
    VersionedGamePacket packet;
    bool canceled = false;
    std::vector<VersionedGamePacket> replacements;

    void cancel() {
        canceled = true;
        replacements.clear();
    }

    void replace(VersionedGamePacket replacement) {
        canceled = false;
        replacements.clear();
        replacements.push_back(std::move(replacement));
    }

    void replace(std::vector<VersionedGamePacket> replacementPackets) {
        canceled = false;
        replacements = std::move(replacementPackets);
    }
};

struct BedrockRelayFrame {
    std::vector<VersionedGamePacket> packets;
    std::vector<VersionedGamePacket> forwardedPackets;
    std::vector<uint8_t> forwardedMcpe;
    bool mutated = false;
    bool queued = false;

    bool empty() const {
        return packets.empty();
    }

    bool forwardedEmpty() const {
        return forwardedPackets.empty() && forwardedMcpe.empty();
    }
};

class BedrockRelay {
public:
    using PacketHandler = BedrockClient::PacketHandler;
    using RelayPacketHandler = std::function<void(BedrockRelayPacketEvent&)>;

    explicit BedrockRelay(BedrockRelayOptions options = {})
        : options_(normalizeOptions(std::move(options))),
          upstream_(options_.clientOptions),
          downstream_(options_.clientOptions),
          mcpeCodec_(VersionedMcpeCodec::forVersion(options_.clientOptions.minecraftVersion)) {}

    BedrockClient& upstream() { return upstream_; }
    BedrockClient& downstream() { return downstream_; }

    bool downstreamJoined() const {
        return startRelaying_;
    }

    bool upstreamJoined() const {
        return upstreamReady_;
    }

    std::vector<BedrockRelayFrame> markDownstreamJoined() {
        startRelaying_ = true;
        return flushDownQueue();
    }

    std::vector<BedrockRelayFrame> markUpstreamJoined() {
        upstreamReady_ = true;
        return flushUpQueue();
    }

    std::vector<BedrockRelayFrame> flushDownQueue() {
        std::vector<BedrockRelayFrame> frames;
        frames.reserve(downQueue_.size());

        auto queued = std::move(downQueue_);
        downQueue_.clear();

        for (const auto& mcpePayload : queued) {
            frames.push_back(handleClientboundMcpe(mcpePayload));
        }

        return frames;
    }

    std::vector<BedrockRelayFrame> flushUpQueue() {
        std::vector<BedrockRelayFrame> frames;
        frames.reserve(upQueue_.size());

        auto queued = std::move(upQueue_);
        upQueue_.clear();

        for (const auto& mcpePayload : queued) {
            frames.push_back(handleServerboundMcpe(mcpePayload));
        }

        return frames;
    }

    void onClientbound(RelayPacketHandler handler) {
        clientboundHandlers_.push_back(std::move(handler));
    }

    void onServerbound(RelayPacketHandler handler) {
        serverboundHandlers_.push_back(std::move(handler));
    }

    void on(const std::string& direction, RelayPacketHandler handler) {
        if (direction == "clientbound") {
            onClientbound(std::move(handler));
            return;
        }

        if (direction == "serverbound") {
            onServerbound(std::move(handler));
            return;
        }

        throw std::runtime_error("unknown relay direction: " + direction);
    }

    void onServerPacket(PacketHandler handler) {
        onClientbound([handler = std::move(handler)](BedrockRelayPacketEvent& event) mutable {
            handler(event.packet);
        });
    }

    void onClientPacket(PacketHandler handler) {
        onServerbound([handler = std::move(handler)](BedrockRelayPacketEvent& event) mutable {
            handler(event.packet);
        });
    }

    BedrockRelayFrame handleClientboundMcpe(const std::vector<uint8_t>& mcpePayload) {
        if (!startRelaying_) {
            BedrockRelayFrame frame;
            frame.queued = true;
            downQueue_.push_back(mcpePayload);
            return frame;
        }

        auto batch = upstream_.handleMcpePayload(mcpePayload);
        return handleDecodedBatch(
            BedrockRelayDirection::Clientbound,
            std::move(batch.packets),
            mcpePayload,
            options_.forwardClientbound && options_.forwardServerToClient
        );
    }

    BedrockRelayFrame handleServerboundMcpe(const std::vector<uint8_t>& mcpePayload) {
        if (startRelaying_ && !upstreamReady_) {
            BedrockRelayFrame frame;
            frame.queued = true;
            upQueue_.push_back(mcpePayload);
            return frame;
        }

        auto batch = downstream_.handleMcpePayload(mcpePayload);
        return handleDecodedBatch(
            BedrockRelayDirection::Serverbound,
            std::move(batch.packets),
            mcpePayload,
            options_.forwardServerbound && options_.forwardClientToServer
        );
    }

    BedrockRelayFrame handleServerToClientMcpe(const std::vector<uint8_t>& mcpePayload) {
        return handleClientboundMcpe(mcpePayload);
    }

    BedrockRelayFrame handleClientToServerMcpe(const std::vector<uint8_t>& mcpePayload) {
        return handleServerboundMcpe(mcpePayload);
    }

    std::vector<VersionedGamePacket> takeUpstreamAutoResponses() {
        return upstream_.takeOutgoingPackets();
    }

    std::vector<VersionedGamePacket> takeDownstreamAutoResponses() {
        return downstream_.takeOutgoingPackets();
    }

    std::size_t queuedClientboundPacketCount() const {
        return queuedClientboundPackets_.size();
    }

    std::size_t downQueueSize() const {
        return downQueue_.size();
    }

    std::size_t upQueueSize() const {
        return upQueue_.size();
    }

private:
    BedrockRelayOptions options_;
    BedrockClient upstream_;
    BedrockClient downstream_;
    VersionedMcpeCodec mcpeCodec_;
    std::vector<RelayPacketHandler> clientboundHandlers_;
    std::vector<RelayPacketHandler> serverboundHandlers_;
    std::vector<VersionedGamePacket> queuedClientboundPackets_;
    std::vector<std::vector<uint8_t>> downQueue_;
    std::vector<std::vector<uint8_t>> upQueue_;
    bool startRelaying_ = false;
    bool upstreamReady_ = false;
    bool sentClientboundStartGame_ = false;

    static BedrockRelayOptions normalizeOptions(BedrockRelayOptions options) {
        (void) validateVersion(options.clientOptions.minecraftVersion);
        return options;
    }

    BedrockRelayFrame handleDecodedBatch(
        BedrockRelayDirection direction,
        std::vector<VersionedGamePacket> packets,
        const std::vector<uint8_t>& originalMcpePayload,
        bool shouldForward
    ) {
        BedrockRelayFrame frame;
        frame.packets = std::move(packets);

        if (!shouldForward) {
            frame.mutated = true;
            return frame;
        }

        bool changed = false;
        for (const auto& packet : frame.packets) {
            if (shouldSkipPacket(direction, packet)) {
                changed = true;
                continue;
            }

            auto forwarded = applyHandlers(direction, packet, changed);

            for (auto& candidate : forwarded) {
                if (direction == BedrockRelayDirection::Clientbound) {
                    appendClientboundPacket(std::move(candidate), frame.forwardedPackets, changed);
                } else {
                    appendServerboundPacket(std::move(candidate), frame.forwardedPackets, changed);
                }
            }
        }

        if (!changed && frame.forwardedPackets.size() == frame.packets.size()) {
            frame.forwardedMcpe = originalMcpePayload;
            return frame;
        }

        frame.mutated = true;
        if (!frame.forwardedPackets.empty()) {
            frame.forwardedMcpe = mcpeCodec_.encodeMcpePayload(
                frame.forwardedPackets,
                options_.clientOptions.outgoingCompression
            );
        }
        return frame;
    }

    bool shouldSkipPacket(BedrockRelayDirection direction, const VersionedGamePacket& packet) const {
        return direction == BedrockRelayDirection::Clientbound &&
            options_.skipClientboundLoginSuccess &&
            packet.name == "play_status" &&
            isPlayStatusLoginSuccess(packet);
    }

    static bool isPlayStatusLoginSuccess(const VersionedGamePacket& packet) {
        // play_status.status is encoded as the first enum/varint value in the
        // bundled Bedrock schemas. JavaScript relay skips login_success because
        // the downstream proxy server has already sent it.
        return !packet.payload.empty() && packet.payload[0] == 0x00;
    }

    std::vector<VersionedGamePacket> applyHandlers(
        BedrockRelayDirection direction,
        const VersionedGamePacket& packet,
        bool& changed
    ) {
        BedrockRelayPacketEvent event;
        event.direction = direction;
        event.packet = packet;

        if (direction == BedrockRelayDirection::Serverbound &&
            options_.forceClientCacheStatus &&
            packet.name == "client_cache_status" &&
            mcpeCodec_.definition().hasPacket("client_cache_status")) {
            event.replace(mcpeCodec_.packetCodec().makePacketByName(
                "client_cache_status",
                { static_cast<uint8_t>(options_.enableChunkCaching ? 1 : 0) }
            ));
        }

        auto& handlers = direction == BedrockRelayDirection::Clientbound
            ? clientboundHandlers_
            : serverboundHandlers_;

        for (auto& handler : handlers) {
            handler(event);
        }

        if (event.canceled) {
            changed = true;
            return {};
        }

        if (!event.replacements.empty()) {
            changed = true;
            return std::move(event.replacements);
        }

        return { std::move(event.packet) };
    }

    void appendClientboundPacket(
        VersionedGamePacket packet,
        std::vector<VersionedGamePacket>& out,
        bool& changed
    ) {
        if (packet.name == "start_game") {
            sentClientboundStartGame_ = true;
            out.push_back(std::move(packet));

            if (!queuedClientboundPackets_.empty()) {
                changed = true;
                out.insert(
                    out.end(),
                    std::make_move_iterator(queuedClientboundPackets_.begin()),
                    std::make_move_iterator(queuedClientboundPackets_.end())
                );
                queuedClientboundPackets_.clear();
            }
            return;
        }

        if (options_.queueClientboundLevelChunksUntilStartGame &&
            !sentClientboundStartGame_ &&
            packet.name == "level_chunk") {
            changed = true;
            queuedClientboundPackets_.push_back(std::move(packet));
            return;
        }

        out.push_back(std::move(packet));
    }

    void appendServerboundPacket(
        VersionedGamePacket packet,
        std::vector<VersionedGamePacket>& out,
        bool&
    ) {
        out.push_back(std::move(packet));
    }
};

inline BedrockRelay createRelay(BedrockRelayOptions options = {}) {
    return BedrockRelay(std::move(options));
}

} // namespace bedrock
