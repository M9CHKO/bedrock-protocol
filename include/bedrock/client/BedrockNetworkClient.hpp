#pragma once

#include <bedrock/BedrockEncryption.hpp>
#include <bedrock/BedrockKeyExchange.hpp>
#include <bedrock/LoginPacket.hpp>
#include <bedrock/auth/BedrockAuthJwt.hpp>
#include <bedrock/auth/XboxLiveAuth.hpp>
#include <bedrock/client/RakNetClient.hpp>
#include <bedrock/client/VersionedClientSession.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>
#include <bedrock/world/BedrockChunk.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bedrock {

enum class BedrockNetworkClientStatus {
    Disconnected,
    Connecting,
    Authenticating,
    Initializing,
    Initialized
};

struct BedrockNetworkClientOptions {
    std::string host = "localhost";
    uint16_t port = 19132;
    std::string username = "Bot";
    std::string profile;
    std::string version = "latest";
    bool offline = false;
    bool interactiveAuth = true;
    std::string xboxClientId;
    std::filesystem::path authCacheRoot;

    int mtu = 1400;
    int connectTimeoutMs = 9000;
    int batchingIntervalMs = 20;
    bool autoInitPlayer = true;
    bool autoResourcePackResponses = true;
    bool clientCacheEnabled = false;
    bool trackWorld = true;
    int32_t chunkRadius = 20;

    // Optional prebuilt Login packet, including the packet id. Normal bots do
    // not need this; online/offline login packets are generated in-process.
    std::vector<uint8_t> loginPacket;
    std::string clientDataJson;
};

struct BedrockNetworkClientPacketEvent {
    VersionedGamePacket packet;
};

class BedrockNetworkClient {
public:
    using PacketHandler = std::function<void(const BedrockNetworkClientPacketEvent&)>;
    using StatusHandler = std::function<void(BedrockNetworkClientStatus)>;
    using ErrorHandler = std::function<void(const std::string&)>;

    explicit BedrockNetworkClient(BedrockNetworkClientOptions options = {});
    ~BedrockNetworkClient();

    BedrockNetworkClient(const BedrockNetworkClient&) = delete;
    BedrockNetworkClient& operator=(const BedrockNetworkClient&) = delete;

    bool connect();
    int run();
    void close(const std::string& reason = "closed");

    void on(const std::string& packetName, PacketHandler handler);
    void onAny(PacketHandler handler);
    void onJoin(std::function<void()> handler);
    void onClose(ErrorHandler handler);
    void onError(ErrorHandler handler);
    void onStatus(StatusHandler handler);

    void sendPacket(const VersionedGamePacket& packet);
    void send(const std::string& packetName, const ProtoDefValue& value);
    void write(const std::string& packetName, const ProtoDefValue& value);
    void queue(const std::string& packetName, const ProtoDefValue& value);
    void sendQueued();

    BedrockNetworkClientStatus status() const;
    const BedrockNetworkClientOptions& options() const;
    const VersionedClientSession& session() const;
    VersionedClientSession& session();
    const BedrockWorld& world() const;
    BedrockWorld& world();
    const BedrockBlobStore& blobStore() const;
    BedrockBlobStore& blobStore();

private:
    BedrockNetworkClientOptions options_;
    VersionedClientSession session_;
    ProtoDefPacketEncoder packetEncoder_;
    std::unique_ptr<RakNetClient> raknet_;
    BedrockWorld world_;
    BedrockBlobStore blobStore_;
    std::unordered_map<uint64_t, BlobType> pendingBlobTypes_;
    std::vector<BedrockLevelChunkPacket> pendingCachedLevelChunks_;

    mutable std::mutex mutex_;
    std::condition_variable closedCv_;
    std::atomic<bool> closed_ {true};
    BedrockNetworkClientStatus status_ = BedrockNetworkClientStatus::Disconnected;

    std::vector<PacketHandler> anyHandlers_;
    std::unordered_map<std::string, std::vector<PacketHandler>> namedHandlers_;
    std::vector<std::function<void()>> joinHandlers_;
    std::vector<ErrorHandler> closeHandlers_;
    std::vector<ErrorHandler> errorHandlers_;
    std::vector<StatusHandler> statusHandlers_;
    std::vector<VersionedGamePacket> queuedPackets_;

    std::mutex sendMutex_;
    bool compressionReady_ = false;
    std::string compressionAlgorithm_ = "none";
    uint16_t compressionThreshold_ = 512;

    bool encryptionEnabled_ = false;
    DerivedKeyResult encryptionKeys_;
    uint64_t sendCounter_ = 0;
    uint64_t receiveCounter_ = 0;
    std::unique_ptr<BedrockAesGcmStream> encryptStream_;
    std::unique_ptr<BedrockAesGcmStream> decryptStream_;
    BedrockClientKeyPair clientKeys_;

    static BedrockNetworkClientOptions normalizeOptions(BedrockNetworkClientOptions options);
    static bool versionAtLeast(const std::string& version, int major, int minor, int patch);

    void setStatus(BedrockNetworkClientStatus status);
    void emitError(const std::string& message);
    void emitClose(const std::string& reason);
    void emitPacket(const VersionedGamePacket& packet);
    void emitJoin();

    void handleRakNetConnected();
    void handleRakNetPayload(const std::vector<uint8_t>& payload);
    void handlePacket(const VersionedGamePacket& packet);
    void handleLevelChunk(const VersionedGamePacket& packet);
    void handleClientCacheMissResponse(const VersionedGamePacket& packet);
    bool tryStoreLevelChunk(const BedrockLevelChunkPacket& levelChunk);

    void prepareLoginPacket();
    void sendLogin();
    void startEncryptionFromServerHandshake(const VersionedGamePacket& packet);
    void drainSessionOutgoing();
    void sendPackets(const std::vector<VersionedGamePacket>& packets, bool encryptedCompression = false);
    VersionedMcpeCompression choosePlainCompression(const std::vector<VersionedGamePacket>& packets) const;
};

inline BedrockNetworkClient createNetworkClient(BedrockNetworkClientOptions options = {}) {
    return BedrockNetworkClient(std::move(options));
}

} // namespace bedrock
