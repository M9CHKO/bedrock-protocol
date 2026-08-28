#pragma once

#include <bedrock/client/BedrockClient.hpp>
#include <bedrock/protocol/VersionedPayloadReader.hpp>
#include <bedrock/world/LevelChunkView.hpp>
#include <bedrock/world/WorldView.hpp>
#include <bedrock/world/MinecraftDataAssets.hpp>
#include <bedrock/world/WorldScanner.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace bedrock {

struct BedrockBotOptions : BedrockClientOptions {
    std::string username;

    bool autoLoadMinecraftData = false;
    int32_t minecraftProtocol = -1;
    std::string minecraftDataVersion;
    std::string minecraftDataPath = "data/minecraft-data/bedrock";
    std::string generatedRuntimePath = "data/generated/block-runtime/bedrock";
};

struct BedrockBotState {
    bool loginPlayStatusReceived = false;
    bool resourcePacksInfoReceived = false;
    bool resourcePackStackReceived = false;
    bool startGameReceived = false;
    bool initialized = false;

    uint64_t gamePacketCount = 0;
    uint64_t levelChunkCount = 0;
    uint64_t parsedLevelChunkCount = 0;
    uint64_t failedLevelChunkParseCount = 0;
    uint64_t textCount = 0;

    int32_t lastChunkX = 0;
    int32_t lastChunkZ = 0;
    std::string lastTextMessage;

    bool hasLastLevelChunkView = false;
    LevelChunkView lastLevelChunkView;
};

class BedrockBot {
public:
    using PacketHandler = BedrockClient::PacketHandler;

    explicit BedrockBot(BedrockBotOptions options = {})
        : options_(std::move(options)),
          client_(makeClientOptions(options_)) {
        installStateHandlers();
        loadMinecraftDataIfRequested();
    }

    BedrockClient& client() { return client_; }
    const BedrockClient& client() const { return client_; }

    WorldView& world() { return world_; }
    const WorldView& world() const { return world_; }

    WorldScanner scanner() const { return WorldScanner(world_); }

    const BlockRuntimeRegistry& blockRegistry() const { return blockRegistry_; }
    bool hasBlockRegistry() const { return hasBlockRegistry_; }

    const BedrockBotState& state() const { return state_; }

    std::size_t countBlockNameInChunk(
        int32_t chunkX,
        int32_t chunkZ,
        const std::string& blockName,
        std::size_t storageIndex = 0
    ) const {
        requireBlockRegistry();
        return scanner().countBlockNameInChunk(
            chunkX,
            chunkZ,
            blockRegistry_,
            blockName,
            storageIndex
        );
    }

    std::optional<BlockPosition> findFirstBlockNameInChunk(
        int32_t chunkX,
        int32_t chunkZ,
        const std::string& blockName,
        std::size_t storageIndex = 0
    ) const {
        requireBlockRegistry();
        return scanner().findFirstBlockNameInChunk(
            chunkX,
            chunkZ,
            blockRegistry_,
            blockName,
            storageIndex
        );
    }

    bool joined() const {
        return state_.startGameReceived && state_.initialized;
    }

    void onAny(PacketHandler handler) {
        client_.onAny(std::move(handler));
    }

    void on(const std::string& packetName, PacketHandler handler) {
        client_.on(packetName, std::move(handler));
    }

    template <typename UserHandler>
    void onText(UserHandler handler) {
        client_.onText(std::move(handler));
    }

    template <typename UserHandler>
    void onLevelChunk(UserHandler handler) {
        client_.onLevelChunk(std::move(handler));
    }

    template <typename UserHandler>
    void onStartGame(UserHandler handler) {
        client_.onStartGame(std::move(handler));
    }

    VersionedPacketBatch receiveMcpe(const std::vector<uint8_t>& mcpePayload) {
        return client_.handleMcpePayload(mcpePayload);
    }

    VersionedGamePacket write(
        const std::string& packetName,
        const std::vector<uint8_t>& payload = {}
    ) {
        return client_.write(packetName, payload);
    }

    std::vector<VersionedGamePacket> takeOutgoingPackets() {
        auto packets = client_.takeOutgoingPackets();

        for (const auto& packet : packets) {
            if (packet.name == "set_local_player_as_initialized") {
                state_.initialized = true;
            }
        }

        return packets;
    }

    std::vector<uint8_t> takeOutgoingMcpe() {
        auto mcpe = client_.takeOutgoingMcpe();
        if (state_.startGameReceived) {
            state_.initialized = true;
        }
        return mcpe;
    }

private:
    BedrockBotOptions options_;
    BedrockClient client_;
    BedrockBotState state_;
    WorldView world_;
    BlockRuntimeRegistry blockRegistry_;
    bool hasBlockRegistry_ = false;

    void requireBlockRegistry() const {
        if (!hasBlockRegistry_) {
            throw std::runtime_error(
                "BedrockBot block registry is not loaded; enable autoLoadMinecraftData"
            );
        }
    }

    static BedrockClientOptions makeClientOptions(const BedrockBotOptions& options) {
        BedrockClientOptions out;
        out.minecraftVersion = options.minecraftVersion;
        out.outgoingCompression = options.outgoingCompression;
        out.chunkRadius = options.chunkRadius;
        return out;
    }

    void loadMinecraftDataIfRequested() {
        if (!options_.autoLoadMinecraftData) {
            return;
        }

        MinecraftDataAssets assets(
            options_.minecraftDataPath,
            options_.generatedRuntimePath
        );

        if (!options_.minecraftDataVersion.empty()) {
            blockRegistry_ = assets.loadBlockRuntimeRegistryByVersion(
                options_.minecraftDataVersion
            );
            hasBlockRegistry_ = true;
            return;
        }

        if (options_.minecraftProtocol >= 0) {
            blockRegistry_ = assets.loadBlockRuntimeRegistryByProtocol(
                options_.minecraftProtocol
            );
            hasBlockRegistry_ = true;
            return;
        }

        throw std::runtime_error(
            "BedrockBot autoLoadMinecraftData requires minecraftDataVersion or minecraftProtocol"
        );
    }

    void installStateHandlers() {
        client_.onAny([this](const VersionedGamePacket& packet) {
            ++state_.gamePacketCount;

            if (packet.name == "play_status") {
                state_.loginPlayStatusReceived = true;
            } else if (packet.name == "resource_packs_info") {
                state_.resourcePacksInfoReceived = true;
            } else if (packet.name == "resource_pack_stack") {
                state_.resourcePackStackReceived = true;
            } else if (packet.name == "start_game") {
                state_.startGameReceived = true;
            } else if (packet.name == "level_chunk") {
                ++state_.levelChunkCount;

                auto chunk = VersionedPayloadReader::readLevelChunk(packet);
                state_.lastChunkX = chunk.chunkX;
                state_.lastChunkZ = chunk.chunkZ;

                try {
                    state_.lastLevelChunkView = LevelChunkView::parse(packet.payload);
                    world_.put(state_.lastLevelChunkView);
                    state_.hasLastLevelChunkView = true;
                    ++state_.parsedLevelChunkCount;
                } catch (...) {
                    ++state_.failedLevelChunkParseCount;
                }
            } else if (packet.name == "text") {
                ++state_.textCount;
                auto text = VersionedPayloadReader::readText(packet);
                state_.lastTextMessage = text.message;
            }
        });
    }
};

inline BedrockBot createBedrockBot(BedrockBotOptions options = {}) {
    return BedrockBot(std::move(options));
}

// Typed compatibility only. A direct createBot({...}) call belongs to the
// normal in-process BotOptions facade from <bedrock/bedrock.hpp>.
template <typename NativeBotOptions>
requires std::is_same_v<
    std::remove_cvref_t<NativeBotOptions>,
    BedrockBotOptions
>
inline BedrockBot createBot(NativeBotOptions&& options) {
    return createBedrockBot(
        BedrockBotOptions(std::forward<NativeBotOptions>(options))
    );
}

} // namespace bedrock
