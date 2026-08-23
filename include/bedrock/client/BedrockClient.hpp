#pragma once

#include <bedrock/client/VersionedClientSession.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protocol/VersionedBatchCodec.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/protocol/VersionedPacketCodec.hpp>
#include <bedrock/protocol/VersionedPayloadReader.hpp>

#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace bedrock {

using BedrockClientOptions = VersionedClientSessionOptions;

class BedrockClient {
public:
    using PacketHandler = VersionedClientSession::PacketHandler;
    using Handler = PacketHandler;

    explicit BedrockClient(BedrockClientOptions options = {})
        : options_(std::move(options)),
          session_(options_),
          codec_(VersionedMcpeCodec::forVersion(options_.minecraftVersion)) {}

    // ------------------------------------------------------------
    // Events, bedrock-protocol style
    // ------------------------------------------------------------
    void onAny(PacketHandler handler) {
        session_.onAny(std::move(handler));
    }

    void on(const std::string& packetName, PacketHandler handler) {
        session_.on(packetName, std::move(handler));
    }

    void on(uint32_t packetId, PacketHandler handler) {
        session_.on(packetId, std::move(handler));
    }

    template <typename UserHandler>
    void onPlayStatus(UserHandler handler) {
        session_.onPlayStatus(std::move(handler));
    }

    template <typename UserHandler>
    void onText(UserHandler handler) {
        session_.onText(std::move(handler));
    }

    template <typename UserHandler>
    void onLevelChunk(UserHandler handler) {
        session_.onLevelChunk(std::move(handler));
    }

    template <typename UserHandler>
    void onStartGame(UserHandler handler) {
        session_.onStartGame(std::move(handler));
    }

    // ------------------------------------------------------------
    // Generic write("packet_name", payload)
    // ------------------------------------------------------------
    VersionedGamePacket write(
        const std::string& packetName,
        const std::vector<uint8_t>& payload = {}
    ) {
        auto packet = codec_.packetCodec().makePacketByName(packetName, payload);
        extraOutgoing_.push_back(packet);
        return packet;
    }

    VersionedGamePacket write(
        uint32_t packetId,
        const std::vector<uint8_t>& payload = {}
    ) {
        auto packet = codec_.packetCodec().makePacketById(packetId, payload);
        extraOutgoing_.push_back(packet);
        return packet;
    }

    VersionedGamePacket writePacket(
        const std::string& packetName,
        const std::vector<uint8_t>& payload = {}
    ) {
        return write(packetName, payload);
    }

    VersionedGamePacket writePacket(
        uint32_t packetId,
        const std::vector<uint8_t>& payload = {}
    ) {
        return write(packetId, payload);
    }

    // ------------------------------------------------------------
    // Existing high-level writes
    // ------------------------------------------------------------
    VersionedGamePacket writeNetworkSettingsRequest(uint32_t protocolVersion) {
        return session_.writeNetworkSettingsRequest(protocolVersion);
    }

    VersionedGamePacket writeClientToServerHandshake() {
        return session_.writeClientToServerHandshake();
    }

    VersionedGamePacket writeRequestChunkRadius(int32_t radius) {
        return session_.writeRequestChunkRadius(radius);
    }

    // ------------------------------------------------------------
    // Incoming MCPE -> packets + events
    // ------------------------------------------------------------
    auto handleMcpePayload(const std::vector<uint8_t>& mcpePayload) {
        return session_.handleMcpePayload(mcpePayload);
    }

    auto receiveMcpe(const std::vector<uint8_t>& mcpePayload) {
        return handleMcpePayload(mcpePayload);
    }

    // ------------------------------------------------------------
    // Outgoing queues
    // ------------------------------------------------------------
    bool hasOutgoing() const {
        return session_.hasOutgoing() || !extraOutgoing_.empty();
    }

    std::vector<VersionedGamePacket> takeOutgoingPackets() {
        auto packets = session_.takeOutgoingPackets();

        packets.insert(
            packets.end(),
            std::make_move_iterator(extraOutgoing_.begin()),
            std::make_move_iterator(extraOutgoing_.end())
        );

        extraOutgoing_.clear();
        return packets;
    }

    std::vector<uint8_t> takeOutgoingMcpe(VersionedMcpeCompression compression) {
        return codec_.encodeMcpePayload(takeOutgoingPackets(), compression);
    }

    std::vector<uint8_t> takeOutgoingMcpe() {
        return codec_.encodeMcpePayload(takeOutgoingPackets());
    }

    const BedrockClientOptions& options() const {
        return options_;
    }

private:
    BedrockClientOptions options_;
    VersionedClientSession session_;
    VersionedMcpeCodec codec_;
    std::vector<VersionedGamePacket> extraOutgoing_;
};

inline BedrockClient createBedrockClient(BedrockClientOptions options = {}) {
    return BedrockClient(std::move(options));
}

namespace legacy {

inline BedrockClient createClient(BedrockClientOptions options = {}) {
    return BedrockClient(std::move(options));
}

} // namespace legacy

} // namespace bedrock
