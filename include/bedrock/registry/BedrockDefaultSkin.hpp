#pragma once

#include <bedrock/protodef/ProtoDefValue.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bedrock {

struct BedrockSkinImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::string dataBase64;
    std::vector<uint8_t> bytes;

    std::size_t expectedRgbaByteCount() const;
    bool validRgba() const;
};

struct BedrockSkinAnimation {
    uint32_t expression = 0;
    uint32_t frames = 0;
    uint32_t type = 0;
    BedrockSkinImage image;
};

struct BedrockPersonaPiece {
    bool isDefault = false;
    std::string packId;
    std::string pieceId;
    std::string pieceType;
    std::string productId;
};

struct BedrockPieceTintColors {
    std::string pieceType;
    std::vector<std::string> colors;
};

class BedrockDefaultSkinLoader;

// Typed view over minecraft-data's Bedrock `defaultSkin`/steve object. The
// original object and JSON are retained so callers can forward every field,
// including fields added by newer Bedrock protocol revisions.
class BedrockDefaultSkin {
public:
    const ProtoDefValue& raw() const;
    const std::string& json() const;
    const ProtoDefValue* field(std::string_view name) const;

    const std::string& skinId() const;
    const std::string& armSize() const;
    const std::string& skinColor() const;
    const std::string& capeId() const;
    bool personaSkin() const;
    bool premiumSkin() const;
    bool capeOnClassicSkin() const;

    const BedrockSkinImage& skinImage() const;
    const BedrockSkinImage& capeImage() const;
    const std::vector<BedrockSkinAnimation>& animatedImages() const;
    const std::vector<BedrockPersonaPiece>& personaPieces() const;
    const std::vector<BedrockPieceTintColors>& pieceTintColors() const;

    const std::string& skinGeometryDataBase64() const;
    const std::string& skinGeometryDataJson() const;
    const std::string& skinResourcePatchBase64() const;
    const std::string& skinResourcePatchJson() const;
    const std::optional<std::string>& skinGeometryDataEngineVersionBase64() const;
    const std::optional<std::string>& skinGeometryDataEngineVersion() const;
    const std::string& skinAnimationDataBase64() const;
    const std::vector<uint8_t>& skinAnimationData() const;

private:
    friend class BedrockDefaultSkinLoader;

    ProtoDefValue raw_;
    std::string json_;
    std::string skinId_;
    std::string armSize_;
    std::string skinColor_;
    std::string capeId_;
    bool personaSkin_ = false;
    bool premiumSkin_ = false;
    bool capeOnClassicSkin_ = false;
    BedrockSkinImage skinImage_;
    BedrockSkinImage capeImage_;
    std::vector<BedrockSkinAnimation> animatedImages_;
    std::vector<BedrockPersonaPiece> personaPieces_;
    std::vector<BedrockPieceTintColors> pieceTintColors_;
    std::string skinGeometryDataBase64_;
    std::string skinGeometryDataJson_;
    std::string skinResourcePatchBase64_;
    std::string skinResourcePatchJson_;
    std::optional<std::string> skinGeometryDataEngineVersionBase64_;
    std::optional<std::string> skinGeometryDataEngineVersion_;
    std::string skinAnimationDataBase64_;
    std::vector<uint8_t> skinAnimationData_;
};

class BedrockDefaultSkinLoader {
public:
    static BedrockDefaultSkin loadMinecraftData(
        const std::filesystem::path& steveJson
    );
};

} // namespace bedrock
