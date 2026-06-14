#include <bedrock/client/BedrockNetworkClient.hpp>

#include <bedrock/auth/BedrockAuthJwt.hpp>
#include <bedrock/auth/BedrockClientDataBuilder.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>

#include <chrono>
#include <cctype>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace bedrock {

namespace {

std::string escapeJson(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

std::string randomUuid() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    uint8_t b[16];
    for (auto& x : b) {
        x = static_cast<uint8_t>(gen() & 0xff);
    }
    b[6] = static_cast<uint8_t>((b[6] & 0x0f) | 0x40);
    b[8] = static_cast<uint8_t>((b[8] & 0x3f) | 0x80);

    const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(hex[b[i] >> 4]);
        out.push_back(hex[b[i] & 0x0f]);
    }
    return out;
}

std::string findFieldValue(
    const std::vector<ProtoDefField>& fields,
    const std::string& name
) {
    for (const auto& field : fields) {
        if (field.path == name) return field.value;
        auto dot = field.path.rfind('.');
        if (dot != std::string::npos && field.path.substr(dot + 1) == name) {
            return field.value;
        }
    }
    return {};
}

uint16_t parseU16Field(
    const std::vector<ProtoDefField>& fields,
    const std::string& name,
    uint16_t fallback
) {
    auto value = findFieldValue(fields, name);
    if (value.empty()) return fallback;
    try {
        return static_cast<uint16_t>(std::stoul(value));
    } catch (const std::exception&) {
        return fallback;
    }
}

int32_t playStatusCode(const VersionedGamePacket& packet) {
    if (packet.name != "play_status" || packet.payload.size() < 4) {
        return -1;
    }

    return static_cast<int32_t>(packet.payload[0]) |
        (static_cast<int32_t>(packet.payload[1]) << 8) |
        (static_cast<int32_t>(packet.payload[2]) << 16) |
        (static_cast<int32_t>(packet.payload[3]) << 24);
}

} // namespace

BedrockNetworkClient::BedrockNetworkClient(BedrockNetworkClientOptions options)
    : options_(normalizeOptions(std::move(options))),
      session_(VersionedClientSessionOptions{
          .minecraftVersion = options_.version,
          .outgoingCompression = VersionedMcpeCompression::DeflateRaw,
          .autoResourcePackResponses = options_.autoResourcePackResponses,
          .autoStartGameInit = options_.autoInitPlayer,
          .clientCacheEnabled = options_.clientCacheEnabled,
          .chunkRadius = options_.chunkRadius
      }),
      packetEncoder_(options_.version) {
    compressionAlgorithm_ = versionAtLeast(options_.version, 1, 19, 30) ? "none" : "deflate";
}

BedrockNetworkClient::~BedrockNetworkClient() {
    close();
}

bool BedrockNetworkClient::connect() {
    if (!closed_.load()) {
        return true;
    }

    try {
        prepareLoginPacket();
    } catch (const std::exception& e) {
        emitError(e.what());
        return false;
    }

    closed_.store(false);
    setStatus(BedrockNetworkClientStatus::Connecting);

    RakNetClientOptions rakOptions;
    rakOptions.host = options_.host;
    rakOptions.port = options_.port;
    rakOptions.mtu = options_.mtu;
    rakOptions.timeoutMs = options_.connectTimeoutMs;
    rakOptions.protocolVersion = session_.definition().protocolVersion() >= 589 ? 11 : 10;

    raknet_ = std::make_unique<RakNetClient>(rakOptions);
    raknet_->onConnected([this]() {
        try {
            handleRakNetConnected();
        } catch (const std::exception& e) {
            emitError(e.what());
        }
    });
    raknet_->onEncapsulated([this](const std::vector<uint8_t>& payload) {
        try {
            handleRakNetPayload(payload);
        } catch (const std::exception& e) {
            const std::string message = e.what();
            emitError(message);
            if (message.find("encrypted payload checksum mismatch") != std::string::npos ||
                message.find("decrypt") != std::string::npos ||
                message.find("decrypted payload") != std::string::npos) {
                close(message);
            }
        }
    });
    raknet_->onClose([this](const std::string& reason) {
        emitClose(reason);
    });

    if (!raknet_->connect()) {
        auto error = raknet_->error();
        closed_.store(true);
        setStatus(BedrockNetworkClientStatus::Disconnected);
        emitError(error);
        return false;
    }

    return true;
}

int BedrockNetworkClient::run() {
    if (!connect()) {
        return 1;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    closedCv_.wait(lock, [this]() {
        return closed_.load();
    });
    return 0;
}

void BedrockNetworkClient::close(const std::string& reason) {
    std::unique_ptr<RakNetClient> raknet;
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        raknet = std::move(raknet_);
    }

    if (raknet) {
        raknet->close(reason);
    } else {
        emitClose(reason);
    }
}

void BedrockNetworkClient::on(const std::string& packetName, PacketHandler handler) {
    if (packetName == "packet") {
        onAny(std::move(handler));
        return;
    }
    namedHandlers_[packetName].push_back(std::move(handler));
}

void BedrockNetworkClient::onAny(PacketHandler handler) {
    anyHandlers_.push_back(std::move(handler));
}

void BedrockNetworkClient::onJoin(std::function<void()> handler) {
    joinHandlers_.push_back(std::move(handler));
}

void BedrockNetworkClient::onClose(ErrorHandler handler) {
    closeHandlers_.push_back(std::move(handler));
}

void BedrockNetworkClient::onError(ErrorHandler handler) {
    errorHandlers_.push_back(std::move(handler));
}

void BedrockNetworkClient::onStatus(StatusHandler handler) {
    statusHandlers_.push_back(std::move(handler));
}

void BedrockNetworkClient::sendPacket(const VersionedGamePacket& packet) {
    sendPackets({packet}, encryptionEnabled_);
}

void BedrockNetworkClient::send(const std::string& packetName, const ProtoDefValue& value) {
    auto payload = packetEncoder_.encodePacket(packetName, value);
    sendPacket(session_.packetCodec().makePacketByName(packetName, payload));
}

void BedrockNetworkClient::write(const std::string& packetName, const ProtoDefValue& value) {
    send(packetName, value);
}

void BedrockNetworkClient::queue(const std::string& packetName, const ProtoDefValue& value) {
    auto payload = packetEncoder_.encodePacket(packetName, value);
    std::lock_guard<std::mutex> lock(sendMutex_);
    queuedPackets_.push_back(session_.packetCodec().makePacketByName(packetName, payload));
}

void BedrockNetworkClient::sendQueued() {
    std::vector<VersionedGamePacket> packets;
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        packets = std::move(queuedPackets_);
        queuedPackets_.clear();
    }
    sendPackets(packets, encryptionEnabled_);
}

BedrockNetworkClientStatus BedrockNetworkClient::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

const BedrockNetworkClientOptions& BedrockNetworkClient::options() const {
    return options_;
}

const VersionedClientSession& BedrockNetworkClient::session() const {
    return session_;
}

VersionedClientSession& BedrockNetworkClient::session() {
    return session_;
}

const BedrockWorld& BedrockNetworkClient::world() const {
    return world_;
}

BedrockWorld& BedrockNetworkClient::world() {
    return world_;
}

const BedrockBlobStore& BedrockNetworkClient::blobStore() const {
    return blobStore_;
}

BedrockBlobStore& BedrockNetworkClient::blobStore() {
    return blobStore_;
}

BedrockNetworkClientOptions BedrockNetworkClient::normalizeOptions(BedrockNetworkClientOptions options) {
    if (options.version.empty() || options.version == "auto" || options.version == "latest") {
        auto versions = ProtocolDefinition::versions();
        if (!versions.empty()) {
            options.version = versions.back();
        }
    }
    if (!ProtocolDefinition::supportsVersion(options.version)) {
        throw std::runtime_error("unsupported Bedrock client version: " + options.version);
    }
    if (options.username.empty()) {
        options.username = "Bot";
    }
    if (options.profile.empty()) {
        options.profile = options.username.empty() ? std::string("Bot") : options.username;
    }
    return options;
}

bool BedrockNetworkClient::versionAtLeast(const std::string& version, int major, int minor, int patch) {
    std::vector<int> parts;
    std::string current;
    for (char c : version) {
        if (c == '.') {
            parts.push_back(current.empty() ? 0 : std::stoi(current));
            current.clear();
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            current.push_back(c);
        }
    }
    parts.push_back(current.empty() ? 0 : std::stoi(current));
    while (parts.size() < 3) parts.push_back(0);

    if (parts[0] != major) return parts[0] > major;
    if (parts[1] != minor) return parts[1] > minor;
    return parts[2] >= patch;
}

void BedrockNetworkClient::setStatus(BedrockNetworkClientStatus status) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = status;
    }
    for (auto& handler : statusHandlers_) {
        handler(status);
    }
}

void BedrockNetworkClient::emitError(const std::string& message) {
    for (auto& handler : errorHandlers_) {
        handler(message);
    }
}

void BedrockNetworkClient::emitClose(const std::string& reason) {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true)) {
        return;
    }
    setStatus(BedrockNetworkClientStatus::Disconnected);
    for (auto& handler : closeHandlers_) {
        handler(reason);
    }
    closedCv_.notify_all();
}

void BedrockNetworkClient::emitPacket(const VersionedGamePacket& packet) {
    BedrockNetworkClientPacketEvent event;
    event.packet = packet;

    for (auto& handler : anyHandlers_) {
        handler(event);
    }
    auto it = namedHandlers_.find(packet.name);
    if (it != namedHandlers_.end()) {
        for (auto& handler : it->second) {
            handler(event);
        }
    }
}

void BedrockNetworkClient::emitJoin() {
    for (auto& handler : joinHandlers_) {
        handler();
    }
}

void BedrockNetworkClient::handleRakNetConnected() {
    if (versionAtLeast(options_.version, 1, 19, 30) && session_.definition().hasPacket("request_network_settings")) {
        auto request = session_.writeNetworkSettingsRequest(session_.definition().protocolVersion());
        sendPackets({request}, false);
        session_.takeOutgoingPackets();
        return;
    }

    sendLogin();
}

void BedrockNetworkClient::handleRakNetPayload(const std::vector<uint8_t>& payload) {
    if (payload.empty() || payload[0] != 0xfe) {
        return;
    }

    VersionedMcpePayload decoded;
    if (encryptionEnabled_) {
        if (!decryptStream_) {
            throw std::runtime_error("decrypt stream is not initialized");
        }

        if (payload.empty() || payload[0] != 0xfe) {
            throw std::runtime_error("encrypted MCPE payload missing 0xfe header");
        }

        std::vector<uint8_t> encryptedOnly(payload.begin() + 1, payload.end());
        auto aesPlaintext = decryptStream_->process(encryptedOnly);

        if (aesPlaintext.size() < 8) {
            throw std::runtime_error("decrypted payload too small for checksum");
        }

        std::vector<uint8_t> compressionPacket(
            aesPlaintext.begin(),
            aesPlaintext.end() - 8
        );

        std::vector<uint8_t> receivedChecksum(
            aesPlaintext.end() - 8,
            aesPlaintext.end()
        );

        auto expectedChecksum = BedrockEncryption::computeChecksum(
            compressionPacket,
            receiveCounter_,
            encryptionKeys_.secretKeyBytes
        );

        if (receivedChecksum != expectedChecksum) {
            throw std::runtime_error("encrypted payload checksum mismatch");
        }

        receiveCounter_++;
        decoded = session_.mcpeCodec().decodeCompressionPacket(compressionPacket);
    } else {
        decoded = session_.mcpeCodec().decodeMcpePayload(payload);
    }

    for (const auto& packet : decoded.batch.packets) {
        handlePacket(packet);
    }

    drainSessionOutgoing();
}

void BedrockNetworkClient::handlePacket(const VersionedGamePacket& packet) {
    emitPacket(packet);

    if (packet.name == "network_settings") {
        ProtoDefPacketDecoder decoder(options_.version);
        auto fields = decoder.decodePacket(packet.name, packet.payload);
        compressionThreshold_ = parseU16Field(fields, "compression_threshold", compressionThreshold_);
        auto algorithm = findFieldValue(fields, "compression_algorithm");
        compressionAlgorithm_ = algorithm.empty() ? "deflate" : algorithm;
        compressionReady_ = true;

        if (status() == BedrockNetworkClientStatus::Connecting) {
            sendLogin();
        }
        return;
    }

    if (packet.name == "server_to_client_handshake") {
        startEncryptionFromServerHandshake(packet);
        return;
    }

    if (packet.name == "level_chunk" && options_.trackWorld) {
        handleLevelChunk(packet);
    }

    if (packet.name == "client_cache_miss_response" && options_.trackWorld) {
        handleClientCacheMissResponse(packet);
    }

    session_.handlePacket(packet);

    if (packet.name == "play_status") {
        const int32_t code = playStatusCode(packet);
        if (code == 0 &&
            this->status() == BedrockNetworkClientStatus::Authenticating) {
            setStatus(BedrockNetworkClientStatus::Initializing);
            emitJoin();
        }
        if (code == 3) {
            setStatus(BedrockNetworkClientStatus::Initialized);
        }
    }
}

void BedrockNetworkClient::handleLevelChunk(const VersionedGamePacket& packet) {
    try {
        auto levelChunk = BedrockLevelChunkCodec::decodePacketPayload(packet.payload);
        if (!tryStoreLevelChunk(levelChunk)) {
            pendingCachedLevelChunks_.push_back(std::move(levelChunk));
        }
    } catch (const std::exception& e) {
        emitError("level_chunk decode failed: " + std::string(e.what()));
    }
}

void BedrockNetworkClient::handleClientCacheMissResponse(const VersionedGamePacket& packet) {
    try {
        auto blobs = BedrockLevelChunkCodec::decodeClientCacheMissResponsePayload(packet.payload);
        for (auto& blob : blobs) {
            BlobEntry entry;
            entry.type = BlobType::Biomes;
            auto typeIt = pendingBlobTypes_.find(blob.hash);
            if (typeIt != pendingBlobTypes_.end()) {
                entry.type = typeIt->second;
            }
            entry.buffer = std::move(blob.payload);
            blobStore_.set(blob.hash, std::move(entry));
            pendingBlobTypes_.erase(blob.hash);
        }

        std::vector<BedrockLevelChunkPacket> stillPending;
        for (const auto& levelChunk : pendingCachedLevelChunks_) {
            if (!tryStoreLevelChunk(levelChunk)) {
                stillPending.push_back(levelChunk);
            }
        }
        pendingCachedLevelChunks_ = std::move(stillPending);
    } catch (const std::exception& e) {
        emitError("client_cache_miss_response decode failed: " + std::string(e.what()));
    }
}

bool BedrockNetworkClient::tryStoreLevelChunk(const BedrockLevelChunkPacket& levelChunk) {
    if (!levelChunk.cacheEnabled) {
        auto column = BedrockLevelChunkCodec::decodeNoCacheColumn(levelChunk);
        world_.setLoadedColumn(levelChunk.x, levelChunk.z, std::move(column));
        return true;
    }

    BedrockClientCacheBlobStatus status;
    for (auto hash : levelChunk.blobHashes) {
        pendingBlobTypes_[hash] = BlobType::Biomes;
        if (blobStore_.has(hash)) {
            status.have.push_back(hash);
        } else {
            status.missing.push_back(hash);
        }
    }

    if (!levelChunk.blobHashes.empty()) {
        auto payload = BedrockLevelChunkCodec::encodeClientCacheBlobStatusPayload(status);
        sendPacket(session_.packetCodec().makePacketByName("client_cache_blob_status", payload));
    }

    BedrockChunkColumn column(levelChunk.x, levelChunk.z);
    column.setBounds(-4, 20);
    auto misses = column.networkDecodeCached(levelChunk.blobHashes, blobStore_, levelChunk.payload);
    if (!misses.empty()) {
        return false;
    }
    world_.setLoadedColumn(levelChunk.x, levelChunk.z, std::move(column));
    return true;
}

void BedrockNetworkClient::prepareLoginPacket() {
    if (!options_.loginPacket.empty()) {
        if (clientKeys_.privateKeyPem.empty()) {
            clientKeys_ = XboxLiveAuth::loadOrCreateProfileKeyPair(
                options_.profile,
                options_.authCacheRoot
            );
        }
        return;
    }

    auto generated = XboxLiveAuth::makeLoginPacket({
        .profileName = options_.profile,
        .version = options_.version,
        .protocolVersion = session_.definition().protocolVersion(),
        .serverAddress = options_.host + ":" + std::to_string(options_.port),
        .offline = options_.offline,
        .interactiveAuth = options_.interactiveAuth,
        .xboxClientId = options_.xboxClientId,
        .cacheRoot = options_.authCacheRoot,
        .clientDataJson = options_.clientDataJson,
        .onDeviceCode = [](const XboxDeviceCodeInfo& info) {
            std::cout << "[XBOX] Open: " << info.verificationUri << "\n";
            std::cout << "[XBOX] Code: " << info.userCode << "\n";
            if (!info.message.empty()) {
                std::cout << "[XBOX] " << info.message << "\n";
            }
        },
        .onLog = [](const std::string& message) {
            std::cout << "[XBOX] " << message << "\n";
        }
    });

    options_.loginPacket = std::move(generated.loginPacket);
    clientKeys_ = std::move(generated.keyPair);
}

void BedrockNetworkClient::sendLogin() {
    setStatus(BedrockNetworkClientStatus::Authenticating);
    prepareLoginPacket();
    auto packet = session_.packetCodec().decodeFullPacket(options_.loginPacket);
    sendPackets({packet}, false);
}

void BedrockNetworkClient::startEncryptionFromServerHandshake(const VersionedGamePacket& packet) {
    ProtoDefPacketDecoder decoder(options_.version);
    auto fields = decoder.decodePacket(packet.name, packet.payload);
    auto token = findFieldValue(fields, "token");
    if (token.empty()) {
        throw std::runtime_error("server_to_client_handshake has no token");
    }

    encryptionKeys_ = BedrockKeyExchange::deriveFromServerHandshakeJwtAndPrivateKeyPem(
        token,
        clientKeys_.privateKeyPem
    );
    encryptionEnabled_ = true;
    sendCounter_ = 0;
    receiveCounter_ = 0;
    encryptStream_ = std::make_unique<BedrockAesGcmStream>(
        encryptionKeys_.secretKeyBytes,
        encryptionKeys_.iv16,
        BedrockAesGcmStream::Mode::Encrypt
    );
    decryptStream_ = std::make_unique<BedrockAesGcmStream>(
        encryptionKeys_.secretKeyBytes,
        encryptionKeys_.iv16,
        BedrockAesGcmStream::Mode::Decrypt
    );

    auto handshake = session_.writeClientToServerHandshake();
    sendPackets({handshake}, true);
    session_.takeOutgoingPackets();
}

void BedrockNetworkClient::drainSessionOutgoing() {
    auto outgoing = session_.takeOutgoingPackets();
    if (!outgoing.empty()) {
        sendPackets(outgoing, encryptionEnabled_);
    }
}

void BedrockNetworkClient::sendPackets(
    const std::vector<VersionedGamePacket>& packets,
    bool encryptedCompression
) {
    std::lock_guard<std::mutex> lock(sendMutex_);

    if (packets.empty() || !raknet_) {
        return;
    }

    if (encryptionEnabled_) {
        if (!encryptStream_) {
            throw std::runtime_error("encrypt stream is not initialized");
        }

        // Match bedrock-protocol's Framer: encrypted packets are still only
        // compressed when the framed batch is larger than the negotiated
        // threshold. Small control packets such as client_cache_status must
        // remain uncompressed.
        auto compressionPacket = session_.mcpeCodec().encodeCompressionPacket(
            packets,
            choosePlainCompression(packets)
        );

        auto aesPlaintext = BedrockEncryption::makeAesPlaintext(
            compressionPacket,
            sendCounter_++,
            encryptionKeys_.secretKeyBytes
        );

        auto encryptedOnly = encryptStream_->process(aesPlaintext);

        std::vector<uint8_t> encrypted;
        encrypted.reserve(1 + encryptedOnly.size());
        encrypted.push_back(0xfe);
        encrypted.insert(encrypted.end(), encryptedOnly.begin(), encryptedOnly.end());

        raknet_->sendReliable(encrypted);
        return;
    }

    auto compression = choosePlainCompression(packets);
    auto mcpe = session_.mcpeCodec().encodeMcpePayload(packets, compression);
    raknet_->sendReliable(mcpe);
}

VersionedMcpeCompression BedrockNetworkClient::choosePlainCompression(
    const std::vector<VersionedGamePacket>& packets
) const {
    if (compressionAlgorithm_ == "none" || compressionAlgorithm_ == "snappy") {
        return VersionedMcpeCompression::Uncompressed;
    }

    if (!compressionReady_ && versionAtLeast(options_.version, 1, 19, 30)) {
        return VersionedMcpeCompression::Uncompressed;
    }

    auto framed = session_.batchCodec().encodeFramedBatch(packets);
    if (framed.size() > compressionThreshold_) {
        return VersionedMcpeCompression::DeflateRaw;
    }

    return VersionedMcpeCompression::Uncompressed;
}

} // namespace bedrock
