#pragma once

#include <bedrock/Options.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/protocol/VersionedPayloadReader.hpp>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bedrock {

struct VersionedClientSessionOptions {
    std::string minecraftVersion = std::string(CURRENT_VERSION);
    VersionedMcpeCompression outgoingCompression = VersionedMcpeCompression::DeflateRaw;

    bool autoResourcePackResponses = true;
    bool autoStartGameInit = true;

    bool clientCacheEnabled = false;
    int32_t chunkRadius = 20;

    // Bedrock varlong -1:
    // ff ff ff ff ff ff ff ff 7f
    std::vector<uint8_t> localPlayerRuntimeEntityId = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f
    };
};

class VersionedClientSession {
public:
    using PacketHandler = std::function<void(const VersionedGamePacket&)>;

    explicit VersionedClientSession(VersionedClientSessionOptions options = {})
        : options_(std::move(options)),
          mcpeCodec_(VersionedMcpeCodec::forVersion(options_.minecraftVersion)) {}

    const VersionedClientSessionOptions& options() const {
        return options_;
    }

    const VersionedMcpeCodec& mcpeCodec() const {
        return mcpeCodec_;
    }

    const VersionedBatchCodec& batchCodec() const {
        return mcpeCodec_.batchCodec();
    }

    const VersionedPacketCodec& packetCodec() const {
        return mcpeCodec_.packetCodec();
    }

    const ProtocolDefinition& definition() const {
        return mcpeCodec_.definition();
    }

    void onAny(PacketHandler handler) {
        anyHandlers_.push_back(std::move(handler));
    }

    void on(const std::string& packetName, PacketHandler handler) {
        nameHandlers_[packetName].push_back(std::move(handler));
    }

    void on(uint32_t packetId, PacketHandler handler) {
        idHandlers_[packetId].push_back(std::move(handler));
    }

    template <typename UserHandler>
    void onPlayStatus(UserHandler handler) {
        on("play_status", [handler = std::move(handler)](const VersionedGamePacket& packet) mutable {
            auto parsed = VersionedPayloadReader::readPlayStatus(packet);
            callTypedHandler(handler, parsed, packet);
        });
    }

    template <typename UserHandler>
    void onText(UserHandler handler) {
        on("text", [handler = std::move(handler)](const VersionedGamePacket& packet) mutable {
            auto parsed = VersionedPayloadReader::readText(packet);
            callTypedHandler(handler, parsed, packet);
        });
    }

    template <typename UserHandler>
    void onLevelChunk(UserHandler handler) {
        on("level_chunk", [handler = std::move(handler)](const VersionedGamePacket& packet) mutable {
            auto parsed = VersionedPayloadReader::readLevelChunk(packet);
            callTypedHandler(handler, parsed, packet);
        });
    }

    template <typename UserHandler>
    void onStartGame(UserHandler handler) {
        on("start_game", [handler = std::move(handler)](const VersionedGamePacket& packet) mutable {
            auto parsed = VersionedPayloadReader::readStartGame(packet);
            callTypedHandler(handler, parsed, packet);
        });
    }


    VersionedPacketBatch handleMcpePayload(const std::vector<uint8_t>& mcpePayload) {
        auto decoded = mcpeCodec_.decodeMcpePayload(mcpePayload);
        handleBatch(decoded.batch);
        return decoded.batch;
    }

    void handleBatch(const VersionedPacketBatch& batch) {
        for (const auto& packet : batch.packets) {
            handlePacket(packet);
        }
    }

    void handlePacket(const VersionedGamePacket& packet) {
        if (packet.name == "start_game") {
            seenStartGame_ = true;
        }

        dispatch(packet);
        autoRespond(packet);
    }

    VersionedGamePacket writePacketByName(
        const std::string& packetName,
        const std::vector<uint8_t>& payload = {}
    ) {
        auto packet = packetCodec().makePacketByName(packetName, payload);
        outgoing_.push_back(packet);
        return packet;
    }

    VersionedGamePacket writePacketById(
        uint32_t packetId,
        const std::vector<uint8_t>& payload = {}
    ) {
        auto packet = packetCodec().makePacketById(packetId, payload);
        outgoing_.push_back(packet);
        return packet;
    }

    VersionedGamePacket writeNetworkSettingsRequest(uint32_t protocolVersion) {
        // request_network_settings uses big-endian uint32 protocol version:
        // 827 = 0x0000033b -> 00 00 03 3b
        std::vector<uint8_t> payload = {
            static_cast<uint8_t>((protocolVersion >> 24) & 0xff),
            static_cast<uint8_t>((protocolVersion >> 16) & 0xff),
            static_cast<uint8_t>((protocolVersion >> 8) & 0xff),
            static_cast<uint8_t>(protocolVersion & 0xff)
        };

        return writePacketByName("request_network_settings", payload);
    }

    VersionedGamePacket writeClientToServerHandshake() {
        return writePacketByName("client_to_server_handshake", {});
    }

    VersionedGamePacket writeResourcePackHaveAllPacks() {
        return writePacketByName("resource_pack_client_response", { 0x03, 0x00, 0x00 });
    }

    VersionedGamePacket writeResourcePackCompleted() {
        return writePacketByName("resource_pack_client_response", { 0x04, 0x00, 0x00 });
    }

    VersionedGamePacket writeClientCacheStatus(bool enabled) {
        return writePacketByName("client_cache_status", { static_cast<uint8_t>(enabled ? 1 : 0) });
    }

    VersionedGamePacket writeRequestChunkRadius(int32_t radius) {
        if (radius < 0) {
            throw std::runtime_error("request_chunk_radius radius must be non-negative");
        }

        std::vector<uint8_t> payload;

        uint32_t value =
            (static_cast<uint32_t>(radius) << 1) ^
            static_cast<uint32_t>(radius >> 31);

        while (value >= 0x80) {
            payload.push_back(static_cast<uint8_t>((value & 0x7f) | 0x80));
            value >>= 7;
        }

        payload.push_back(static_cast<uint8_t>(value));

        // max_radius = false
        // minecraft-data 1.21.100:
        // request_chunk_radius.chunk_radius = zigzag32
        // radius=20 -> 0x28, max_radius=false -> 0x00
        payload.push_back(0x00);

        return writePacketByName("request_chunk_radius", payload);
    }

    VersionedGamePacket writeSetLocalPlayerAsInitialized(
        const std::vector<uint8_t>& runtimeEntityIdVarLong
    ) {
        return writePacketByName("set_local_player_as_initialized", runtimeEntityIdVarLong);
    }

    VersionedGamePacket writeSetLocalPlayerAsInitializedMinusOne() {
        return writeSetLocalPlayerAsInitialized(options_.localPlayerRuntimeEntityId);
    }

    std::vector<VersionedGamePacket> takeOutgoingPackets() {
        auto out = std::move(outgoing_);
        outgoing_.clear();
        return out;
    }

    std::vector<VersionedGamePacket> drainOutgoing() {
        return takeOutgoingPackets();
    }

    std::vector<uint8_t> takeOutgoingFramedBatch() {
        auto packets = takeOutgoingPackets();

        if (packets.empty()) {
            return {};
        }

        return batchCodec().encodeFramedBatch(packets);
    }

    std::vector<uint8_t> takeOutgoingMcpe() {
        return takeOutgoingMcpe(options_.outgoingCompression);
    }

    std::vector<uint8_t> takeOutgoingMcpe(VersionedMcpeCompression compression) {
        auto packets = takeOutgoingPackets();

        if (packets.empty()) {
            return {};
        }

        return mcpeCodec_.encodeMcpePayload(packets, compression);
    }

    bool hasOutgoing() const {
        return !outgoing_.empty();
    }

    std::size_t outgoingCount() const {
        return outgoing_.size();
    }

    bool seenStartGame() const {
        return seenStartGame_;
    }

    bool sentPostStartGameInit() const {
        return sentPostStartGameInit_;
    }

    void setAutoResourcePackResponses(bool enabled) {
        options_.autoResourcePackResponses = enabled;
    }

    void setAutoStartGameInit(bool enabled) {
        options_.autoStartGameInit = enabled;
    }

private:
    VersionedClientSessionOptions options_;
    VersionedMcpeCodec mcpeCodec_;

    std::vector<PacketHandler> anyHandlers_;
    std::unordered_map<uint32_t, std::vector<PacketHandler>> idHandlers_;
    std::unordered_map<std::string, std::vector<PacketHandler>> nameHandlers_;

    std::vector<VersionedGamePacket> outgoing_;

    bool seenStartGame_ = false;
    bool sentPostStartGameInit_ = false;

    template <typename UserHandler, typename Parsed>
    static void callTypedHandler(
        UserHandler& handler,
        Parsed& parsed,
        const VersionedGamePacket& packet
    ) {
        if constexpr (std::is_invocable_v<UserHandler&, Parsed&, const VersionedGamePacket&>) {
            handler(parsed, packet);
        } else if constexpr (std::is_invocable_v<UserHandler&, const Parsed&, const VersionedGamePacket&>) {
            handler(parsed, packet);
        } else if constexpr (std::is_invocable_v<UserHandler&, Parsed&>) {
            handler(parsed);
        } else if constexpr (std::is_invocable_v<UserHandler&, const Parsed&>) {
            handler(parsed);
        } else if constexpr (std::is_invocable_v<UserHandler&, Parsed>) {
            handler(parsed);
        } else {
            static_assert(
                std::is_invocable_v<UserHandler&, Parsed&>,
                "typed handler must accept Parsed, Parsed&, const Parsed&, or Parsed plus raw VersionedGamePacket"
            );
        }
    }

    void dispatch(const VersionedGamePacket& packet) {
        for (auto& handler : anyHandlers_) {
            handler(packet);
        }

        auto idIt = idHandlers_.find(packet.packetId);
        if (idIt != idHandlers_.end()) {
            for (auto& handler : idIt->second) {
                handler(packet);
            }
        }

        auto nameIt = nameHandlers_.find(packet.name);
        if (nameIt != nameHandlers_.end()) {
            for (auto& handler : nameIt->second) {
                handler(packet);
            }
        }
    }

    void autoRespond(const VersionedGamePacket& packet) {
        if (options_.autoResourcePackResponses) {
            if (packet.name == "resource_packs_info") {
                writeResourcePackCompleted();
                return;
            }

            if (packet.name == "resource_pack_stack") {
                writeResourcePackCompleted();
                return;
            }
        }

        if (options_.autoStartGameInit && packet.name == "start_game" && !sentPostStartGameInit_) {
            writeClientCacheStatus(options_.clientCacheEnabled);
            writeRequestChunkRadius(options_.chunkRadius);
            writeSetLocalPlayerAsInitializedMinusOne();

            sentPostStartGameInit_ = true;
        }
    }
};

} // namespace bedrock
