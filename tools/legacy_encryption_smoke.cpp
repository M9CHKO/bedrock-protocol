#include <bedrock/BedrockEncryption.hpp>
#include <bedrock/Options.hpp>
#include <bedrock/client/BedrockNetworkClient.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/server/BedrockServer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

uint64_t fnv1a64(const std::vector<uint8_t>& bytes) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}

bool checkAlgorithmBoundary(
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& iv
) {
    using Algorithm = bedrock::BedrockCipherAlgorithm;

    const auto protocol201 = bedrock::protocolVersionFor("1.16.201");
    const auto protocol210 = bedrock::protocolVersionFor("1.16.210");
    const auto protocol220 = bedrock::protocolVersionFor("1.16.220");

    if (protocol201 != 422 || protocol210 != 428 || protocol220 != 431 ||
        bedrock::BedrockEncryption::cipherAlgorithmForProtocol(protocol201) !=
            Algorithm::Aes256Cfb8 ||
        bedrock::BedrockEncryption::cipherAlgorithmForProtocol(protocol210) !=
            Algorithm::Aes256Cfb8 ||
        bedrock::BedrockEncryption::cipherAlgorithmForProtocol(protocol220) !=
            Algorithm::Aes256GcmNoTag) {
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] protocol boundary mismatch\n";
        return false;
    }

    auto stream201 = bedrock::BedrockEncryption::createCipherStream(
        protocol201,
        key,
        iv,
        bedrock::BedrockCipherMode::Encrypt
    );
    auto stream210 = bedrock::BedrockEncryption::createCipherStream(
        protocol210,
        key,
        iv,
        bedrock::BedrockCipherMode::Encrypt
    );
    auto stream220 = bedrock::BedrockEncryption::createCipherStream(
        protocol220,
        key,
        iv,
        bedrock::BedrockCipherMode::Encrypt
    );

    if (stream201->algorithm() != Algorithm::Aes256Cfb8 ||
        stream210->algorithm() != Algorithm::Aes256Cfb8 ||
        stream220->algorithm() != Algorithm::Aes256GcmNoTag) {
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] stream factory mismatch\n";
        return false;
    }

    return true;
}

bool checkNodeCfb8Goldens(
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& iv
) {
    // Generated directly by node_modules/bedrock-protocol@3.53.0 with
    // crypto.createCipheriv('aes-256-cfb8'), zlib level 7 and counters 0/1.
    const std::vector<uint8_t> compressed0 {
        0x63, 0x66, 0x64, 0x62, 0x06, 0x00
    };
    const std::vector<uint8_t> checksum0 {
        0x42, 0xa9, 0x14, 0xcb, 0xf8, 0xf5, 0xf4, 0x0f
    };
    const std::vector<uint8_t> ciphertext0 {
        0x39, 0x7d, 0xb9, 0x3f, 0x84, 0x49, 0x84,
        0xa5, 0x69, 0x88, 0x10, 0x75, 0xeb, 0x71
    };

    const std::vector<uint8_t> compressed1 {
        0x63, 0xb9, 0xb7, 0x76, 0xdf, 0x7b, 0x00
    };
    const std::vector<uint8_t> checksum1 {
        0xa5, 0x5d, 0xe5, 0xfe, 0x65, 0xca, 0xdb, 0x73
    };
    const std::vector<uint8_t> ciphertext1Continuous {
        0xde, 0xf1, 0x0b, 0xf7, 0xba, 0x9d, 0xd9, 0xec,
        0x65, 0x7b, 0x4f, 0xad, 0xf8, 0x90, 0xff
    };
    const std::vector<uint8_t> ciphertext1Fresh {
        0x39, 0xa2, 0x9b, 0xbd, 0x0f, 0xd8, 0x5a, 0x9f,
        0xbb, 0xe4, 0x53, 0xb7, 0xfc, 0xae, 0x96
    };

    const auto plaintext0 = bedrock::BedrockEncryption::makeAesPlaintext(
        compressed0,
        0,
        key
    );
    const auto plaintext1 = bedrock::BedrockEncryption::makeAesPlaintext(
        compressed1,
        1,
        key
    );

    if (std::vector<uint8_t>(plaintext0.end() - 8, plaintext0.end()) != checksum0 ||
        std::vector<uint8_t>(plaintext1.end() - 8, plaintext1.end()) != checksum1) {
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] Node checksum golden mismatch\n";
        return false;
    }

    auto encrypt = bedrock::BedrockEncryption::createCipherStream(
        422,
        key,
        iv,
        bedrock::BedrockCipherMode::Encrypt
    );
    if (!encrypt->process({}).empty()) {
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] empty CFB8 update emitted bytes\n";
        return false;
    }

    const auto encrypted0 = encrypt->process(plaintext0);
    const auto encrypted1 = encrypt->process(plaintext1);
    if (encrypted0 != ciphertext0 || encrypted1 != ciphertext1Continuous) {
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] sequential Node ciphertext mismatch\n";
        return false;
    }

    auto freshEncrypt = bedrock::BedrockEncryption::createCipherStream(
        428,
        key,
        iv,
        bedrock::BedrockCipherMode::Encrypt
    );
    if (freshEncrypt->process(plaintext1) != ciphertext1Fresh ||
        ciphertext1Fresh == ciphertext1Continuous) {
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] CFB8 continuity was not preserved\n";
        return false;
    }

    auto decrypt = bedrock::BedrockEncryption::createCipherStream(
        422,
        key,
        iv,
        bedrock::BedrockCipherMode::Decrypt
    );
    if (decrypt->process(ciphertext0) != plaintext0 ||
        decrypt->process(ciphertext1Continuous) != plaintext1) {
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] sequential CFB8 decrypt mismatch\n";
        return false;
    }

    return true;
}

bool checkNodeGcmGoldens(
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& iv
) {
    // Generated with Node's crypto.createCipheriv('aes-256-gcm') using one
    // continuous cipher and four update() calls. The 103-byte chunk matches
    // the encrypted body size from the Android checksum failure.
    const std::vector<std::size_t> sizes {9, 17, 103, 4097};
    const std::vector<uint64_t> expectedHashes {
        UINT64_C(0xbf614c097c48f4d6),
        UINT64_C(0xaf2ca5ff94b1d8e0),
        UINT64_C(0x184e50c574aab1cd),
        UINT64_C(0x76324096aa124763)
    };

    std::vector<std::vector<uint8_t>> chunks;
    chunks.reserve(sizes.size());
    for (std::size_t chunkIndex = 0;
         chunkIndex < sizes.size();
         ++chunkIndex) {
        std::vector<uint8_t> chunk(sizes[chunkIndex]);
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            chunk[i] = static_cast<uint8_t>(
                i * 131u + chunkIndex * 29u + 0x4du
            );
        }
        chunks.push_back(std::move(chunk));
    }

    auto encrypt = bedrock::BedrockEncryption::createCipherStream(
        827,
        key,
        iv,
        bedrock::BedrockCipherMode::Encrypt
    );
    auto decrypt = bedrock::BedrockEncryption::createCipherStream(
        827,
        key,
        iv,
        bedrock::BedrockCipherMode::Decrypt
    );

    std::vector<uint8_t> allCiphertext;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const auto encrypted = encrypt->process(chunks[i]);
        if (encrypted.size() != chunks[i].size() ||
            fnv1a64(encrypted) != expectedHashes[i]) {
            std::cerr << "[LEGACY-ENCRYPTION-SMOKE] sequential Node GCM "
                         "ciphertext mismatch at chunk " << i << "\n";
            return false;
        }
        if (decrypt->process(encrypted) != chunks[i]) {
            std::cerr << "[LEGACY-ENCRYPTION-SMOKE] sequential GCM decrypt "
                         "mismatch at chunk " << i << "\n";
            return false;
        }
        allCiphertext.insert(
            allCiphertext.end(),
            encrypted.begin(),
            encrypted.end()
        );
    }
    if (fnv1a64(allCiphertext) != UINT64_C(0x02cd55986d14d647)) {
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] aggregate Node GCM "
                     "ciphertext mismatch\n";
        return false;
    }
    return true;
}

bool checkChecksumEdgeSemantics(
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& iv
) {
    struct SliceCase {
        std::vector<uint8_t> input;
        std::vector<uint8_t> packet;
        std::vector<uint8_t> checksum;
        std::string message;
    };

    // These are the exact decrypted prefixes and messages produced by a
    // fresh Node AES-256-CFB8 decipher when given 1/5/7/8 zero bytes.
    const std::vector<SliceCase> cases {
        {
            {0x5a},
            {},
            {0x5a},
            "Checksum mismatch 5a != f4b32977b259a87e"
        },
        {
            {0x5a, 0x78, 0xc8, 0xfc, 0xd0},
            {0x5a, 0x78},
            {0xc8, 0xfc, 0xd0},
            "Checksum mismatch c8fcd0 != 0942c6b822d9eb40"
        },
        {
            {0x5a, 0x78, 0xc8, 0xfc, 0xd0, 0x3d, 0xdd},
            {0x5a, 0x78, 0xc8, 0xfc, 0xd0, 0x3d},
            {0xdd},
            "Checksum mismatch dd != 71bfc77c7291ec2e"
        },
        {
            {0x5a, 0x78, 0xc8, 0xfc, 0xd0, 0x3d, 0xdd, 0xf6},
            {},
            {0x5a, 0x78, 0xc8, 0xfc, 0xd0, 0x3d, 0xdd, 0xf6},
            "Checksum mismatch 5a78c8fcd03dddf6 != f4b32977b259a87e"
        }
    };

    for (const auto& item : cases) {
        uint64_t counter = 0;
        const auto verification = bedrock::BedrockEncryption::verifyAesPlaintext(
            item.input,
            counter,
            key
        );
        if (counter != 1 || verification.matches() ||
            verification.packetPlaintext != item.packet ||
            verification.receivedChecksum != item.checksum ||
            verification.mismatchMessage() != item.message) {
            std::cerr << "[LEGACY-ENCRYPTION-SMOKE] Buffer.slice checksum edge mismatch\n";
            return false;
        }
    }

    auto emptyDecrypt = bedrock::BedrockEncryption::createCipherStream(
        422,
        key,
        iv,
        bedrock::BedrockCipherMode::Decrypt
    );
    uint64_t emptyCounter = 0;
    if (bedrock::BedrockEncryption::decryptAndVerify(
            *emptyDecrypt,
            {},
            emptyCounter,
            key
        ).has_value() || emptyCounter != 0) {
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] empty ciphertext consumed receive counter\n";
        return false;
    }

    // A valid checksum consumes the counter before raw-deflate validation.
    // The single 0x00 byte is intentionally not a complete deflate stream.
    const std::vector<uint8_t> badDeflate {0x00};
    const auto badDeflatePlaintext = bedrock::BedrockEncryption::makeAesPlaintext(
        badDeflate,
        0,
        key
    );
    auto badEncrypt = bedrock::BedrockEncryption::createCipherStream(
        422,
        key,
        iv,
        bedrock::BedrockCipherMode::Encrypt
    );
    auto badDecrypt = bedrock::BedrockEncryption::createCipherStream(
        422,
        key,
        iv,
        bedrock::BedrockCipherMode::Decrypt
    );
    const auto badCiphertext = badEncrypt->process(badDeflatePlaintext);
    uint64_t badCounter = 0;
    auto badVerification = bedrock::BedrockEncryption::decryptAndVerify(
        *badDecrypt,
        badCiphertext,
        badCounter,
        key
    );
    if (!badVerification || !badVerification->matches() || badCounter != 1 ||
        badVerification->packetPlaintext != badDeflate) {
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] valid bad-deflate checksum setup failed\n";
        return false;
    }

    auto codec = bedrock::VersionedMcpeCodec::forVersion("1.16.201");
    try {
        (void) codec.decodeEncryptedCompressionPacket(
            badVerification->packetPlaintext
        );
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] malformed deflate unexpectedly decoded\n";
        return false;
    } catch (const std::exception&) {
        if (badCounter != 1) {
            std::cerr << "[LEGACY-ENCRYPTION-SMOKE] deflate failure rolled back counter\n";
            return false;
        }
    }

    return true;
}

bool checkStrictLegacyCompression() {
    auto codec = bedrock::VersionedMcpeCodec::forVersion("1.16.201");
    const auto handshake = codec.packetCodec().makePacketByName(
        "client_to_server_handshake",
        {}
    );
    const auto encryptedCompression = codec.encodeEncryptedCompressionPacket(
        {handshake},
        7
    );
    const auto decoded = codec.decodeEncryptedCompressionPacket(
        encryptedCompression
    );
    if (decoded.batch.packets.size() != 1 ||
        decoded.batch.packets[0].name != "client_to_server_handshake" ||
        decoded.batch.packets[0].fullPacket != handshake.fullPacket) {
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] strict legacy deflate roundtrip failed\n";
        return false;
    }

    const auto uncompressed = codec.batchCodec().encodeFramedBatch({handshake});
    try {
        (void) codec.decodeEncryptedCompressionPacket(uncompressed);
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] encrypted decoder accepted uncompressed legacy batch\n";
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

bool checkLocalLegacyHandshake() {
    std::atomic<bool> serverJoined {false};
    std::atomic<bool> clientJoined {false};
    std::atomic<bool> clientError {false};

    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.16.201",
        .motd = {{"motd", "Legacy Encryption Smoke"}},
        .maxPlayers = 1,
        .offline = true
    });
    server.onJoin([&](const bedrock::BedrockServerConnection&) {
        serverJoined = true;
    });
    server.listen();

    bedrock::BedrockNetworkClient client({
        .host = "127.0.0.1",
        .port = server.boundPort(),
        .username = "LegacySmoke",
        .version = "1.16.201",
        .offline = true,
        .connectTimeoutMs = 1500,
        .compressionLevel = 7
    });
    client.onJoin([&]() {
        clientJoined = true;
    });
    client.onError([&](const std::string& error) {
        clientError = true;
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] client error: " << error << "\n";
    });

    if (!client.connect()) {
        server.close();
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] local client connect failed\n";
        return false;
    }

    for (int i = 0; i < 200 &&
         (!serverJoined.load() || !clientJoined.load()) &&
         !clientError.load();
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const bool ok = serverJoined.load() && clientJoined.load() && !clientError.load();
    client.close();
    server.close();

    if (!ok) {
        std::cerr << "[LEGACY-ENCRYPTION-SMOKE] local 1.16.201 handshake failed\n";
    }
    return ok;
}

} // namespace

int main() {
    std::vector<uint8_t> key(32);
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(i);
    }
    const std::vector<uint8_t> iv(key.begin(), key.begin() + 16);

    bool ok = true;
    ok = checkAlgorithmBoundary(key, iv) && ok;
    ok = checkNodeCfb8Goldens(key, iv) && ok;
    ok = checkNodeGcmGoldens(key, iv) && ok;
    ok = checkChecksumEdgeSemantics(key, iv) && ok;
    ok = checkStrictLegacyCompression() && ok;
    ok = checkLocalLegacyHandshake() && ok;

    if (ok) {
        std::cout << "[LEGACY-ENCRYPTION-SMOKE] ok\n";
    }
    return ok ? 0 : 1;
}
