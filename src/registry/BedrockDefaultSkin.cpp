#include <bedrock/registry/BedrockDefaultSkin.hpp>

#include <bedrock/BedrockKeyExchange.hpp>
#include <bedrock/protodef/ProtoDefJson.hpp>

#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace bedrock {
namespace {

using JsonValue = ProtoDefValue;

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(
            "failed to open minecraft-data file: " + path.string()
        );
    }
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    );
}

const JsonValue& requireKind(
    const JsonValue& value,
    JsonValue::Kind kind,
    std::string_view context
) {
    if (value.kind != kind) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) +
            " has an unexpected JSON type"
        );
    }
    return value;
}

const JsonValue& requireField(
    const JsonValue& object,
    std::string_view field,
    std::string_view context
) {
    requireKind(object, JsonValue::Kind::Object, context);
    const auto* value = object.get(std::string(field));
    if (value == nullptr) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) + " is missing field " +
            std::string(field)
        );
    }
    return *value;
}

const JsonValue* optionalField(const JsonValue& object, std::string_view field) {
    if (object.kind != JsonValue::Kind::Object) return nullptr;
    return object.get(std::string(field));
}

std::string stringValue(const JsonValue& value, std::string_view context) {
    requireKind(value, JsonValue::Kind::String, context);
    return value.stringValue;
}

bool boolValue(const JsonValue& value, std::string_view context) {
    requireKind(value, JsonValue::Kind::Bool, context);
    return value.boolValue;
}

uint32_t uint32Value(const JsonValue& value, std::string_view context) {
    uint64_t number = 0;
    if (value.kind == JsonValue::Kind::UInt) {
        number = value.uintValue;
    } else if (value.kind == JsonValue::Kind::Int && value.intValue >= 0) {
        number = static_cast<uint64_t>(value.intValue);
    } else {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) +
            " must be a non-negative integer"
        );
    }
    if (number > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) + " is out of uint32 range"
        );
    }
    return static_cast<uint32_t>(number);
}

std::vector<uint8_t> decodeBase64(
    const std::string& value,
    std::string_view context
) {
    try {
        return BedrockKeyExchange::base64Decode(value);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) +
            " contains invalid base64: " + error.what()
        );
    }
}

std::string decodedString(const std::string& value, std::string_view context) {
    const auto bytes = decodeBase64(value, context);
    return std::string(bytes.begin(), bytes.end());
}

BedrockSkinImage parseImage(
    const JsonValue& object,
    std::string_view dataField,
    std::string_view widthField,
    std::string_view heightField,
    std::string_view context
) {
    BedrockSkinImage image;
    image.width = uint32Value(requireField(object, widthField, context), widthField);
    image.height = uint32Value(requireField(object, heightField, context), heightField);
    image.dataBase64 = stringValue(requireField(object, dataField, context), dataField);
    image.bytes = decodeBase64(image.dataBase64, dataField);
    if (!image.validRgba()) {
        throw std::runtime_error(
            "minecraft-data " + std::string(context) +
            " RGBA byte count does not match its dimensions"
        );
    }
    return image;
}

} // namespace

std::size_t BedrockSkinImage::expectedRgbaByteCount() const {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
}

bool BedrockSkinImage::validRgba() const {
    return bytes.size() == expectedRgbaByteCount();
}

const ProtoDefValue& BedrockDefaultSkin::raw() const { return raw_; }
const std::string& BedrockDefaultSkin::json() const { return json_; }

const ProtoDefValue* BedrockDefaultSkin::field(std::string_view name) const {
    return raw_.get(std::string(name));
}

const std::string& BedrockDefaultSkin::skinId() const { return skinId_; }
const std::string& BedrockDefaultSkin::armSize() const { return armSize_; }
const std::string& BedrockDefaultSkin::skinColor() const { return skinColor_; }
const std::string& BedrockDefaultSkin::capeId() const { return capeId_; }
bool BedrockDefaultSkin::personaSkin() const { return personaSkin_; }
bool BedrockDefaultSkin::premiumSkin() const { return premiumSkin_; }
bool BedrockDefaultSkin::capeOnClassicSkin() const { return capeOnClassicSkin_; }
const BedrockSkinImage& BedrockDefaultSkin::skinImage() const { return skinImage_; }
const BedrockSkinImage& BedrockDefaultSkin::capeImage() const { return capeImage_; }

const std::vector<BedrockSkinAnimation>& BedrockDefaultSkin::animatedImages() const {
    return animatedImages_;
}

const std::vector<BedrockPersonaPiece>& BedrockDefaultSkin::personaPieces() const {
    return personaPieces_;
}

const std::vector<BedrockPieceTintColors>& BedrockDefaultSkin::pieceTintColors() const {
    return pieceTintColors_;
}

const std::string& BedrockDefaultSkin::skinGeometryDataBase64() const {
    return skinGeometryDataBase64_;
}

const std::string& BedrockDefaultSkin::skinGeometryDataJson() const {
    return skinGeometryDataJson_;
}

const std::string& BedrockDefaultSkin::skinResourcePatchBase64() const {
    return skinResourcePatchBase64_;
}

const std::string& BedrockDefaultSkin::skinResourcePatchJson() const {
    return skinResourcePatchJson_;
}

const std::optional<std::string>&
BedrockDefaultSkin::skinGeometryDataEngineVersionBase64() const {
    return skinGeometryDataEngineVersionBase64_;
}

const std::optional<std::string>&
BedrockDefaultSkin::skinGeometryDataEngineVersion() const {
    return skinGeometryDataEngineVersion_;
}

const std::string& BedrockDefaultSkin::skinAnimationDataBase64() const {
    return skinAnimationDataBase64_;
}

const std::vector<uint8_t>& BedrockDefaultSkin::skinAnimationData() const {
    return skinAnimationData_;
}

BedrockDefaultSkin BedrockDefaultSkinLoader::loadMinecraftData(
    const std::filesystem::path& steveJson
) {
    BedrockDefaultSkin skin;
    skin.json_ = readTextFile(steveJson);
    try {
        skin.raw_ = ProtoDefJson::parse(skin.json_);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "failed to parse minecraft-data JSON " + steveJson.string() +
            ": " + error.what()
        );
    }
    requireKind(skin.raw_, JsonValue::Kind::Object, "steve.json root");

    skin.skinId_ = stringValue(requireField(skin.raw_, "SkinId", "default skin"), "SkinId");
    skin.armSize_ = stringValue(requireField(skin.raw_, "ArmSize", "default skin"), "ArmSize");
    skin.skinColor_ = stringValue(requireField(skin.raw_, "SkinColor", "default skin"), "SkinColor");
    skin.capeId_ = stringValue(requireField(skin.raw_, "CapeId", "default skin"), "CapeId");
    skin.personaSkin_ = boolValue(
        requireField(skin.raw_, "PersonaSkin", "default skin"),
        "PersonaSkin"
    );
    skin.premiumSkin_ = boolValue(
        requireField(skin.raw_, "PremiumSkin", "default skin"),
        "PremiumSkin"
    );
    skin.capeOnClassicSkin_ = boolValue(
        requireField(skin.raw_, "CapeOnClassicSkin", "default skin"),
        "CapeOnClassicSkin"
    );

    skin.skinImage_ = parseImage(
        skin.raw_,
        "SkinData",
        "SkinImageWidth",
        "SkinImageHeight",
        "default skin"
    );
    skin.capeImage_ = parseImage(
        skin.raw_,
        "CapeData",
        "CapeImageWidth",
        "CapeImageHeight",
        "default cape"
    );

    skin.skinGeometryDataBase64_ = stringValue(
        requireField(skin.raw_, "SkinGeometryData", "default skin"),
        "SkinGeometryData"
    );
    skin.skinGeometryDataJson_ = decodedString(
        skin.skinGeometryDataBase64_,
        "SkinGeometryData"
    );
    skin.skinResourcePatchBase64_ = stringValue(
        requireField(skin.raw_, "SkinResourcePatch", "default skin"),
        "SkinResourcePatch"
    );
    skin.skinResourcePatchJson_ = decodedString(
        skin.skinResourcePatchBase64_,
        "SkinResourcePatch"
    );
    if (const auto* engine = optionalField(
            skin.raw_,
            "SkinGeometryDataEngineVersion"
        ); engine != nullptr && engine->kind != JsonValue::Kind::Null) {
        skin.skinGeometryDataEngineVersionBase64_ = stringValue(
            *engine,
            "SkinGeometryDataEngineVersion"
        );
        skin.skinGeometryDataEngineVersion_ = decodedString(
            *skin.skinGeometryDataEngineVersionBase64_,
            "SkinGeometryDataEngineVersion"
        );
    }
    skin.skinAnimationDataBase64_ = stringValue(
        requireField(skin.raw_, "SkinAnimationData", "default skin"),
        "SkinAnimationData"
    );
    skin.skinAnimationData_ = decodeBase64(
        skin.skinAnimationDataBase64_,
        "SkinAnimationData"
    );

    const auto& animations = requireKind(
        requireField(skin.raw_, "AnimatedImageData", "default skin"),
        JsonValue::Kind::Array,
        "AnimatedImageData"
    );
    skin.animatedImages_.reserve(animations.arrayValue.size());
    for (const auto& value : animations.arrayValue) {
        BedrockSkinAnimation animation;
        animation.expression = uint32Value(
            requireField(value, "AnimationExpression", "skin animation"),
            "AnimationExpression"
        );
        animation.frames = uint32Value(
            requireField(value, "Frames", "skin animation"),
            "Frames"
        );
        animation.type = uint32Value(
            requireField(value, "Type", "skin animation"),
            "Type"
        );
        animation.image = parseImage(
            value,
            "Image",
            "ImageWidth",
            "ImageHeight",
            "skin animation"
        );
        skin.animatedImages_.push_back(std::move(animation));
    }

    const auto& pieces = requireKind(
        requireField(skin.raw_, "PersonaPieces", "default skin"),
        JsonValue::Kind::Array,
        "PersonaPieces"
    );
    skin.personaPieces_.reserve(pieces.arrayValue.size());
    for (const auto& value : pieces.arrayValue) {
        BedrockPersonaPiece piece;
        piece.isDefault = boolValue(
            requireField(value, "IsDefault", "persona piece"),
            "IsDefault"
        );
        piece.packId = stringValue(
            requireField(value, "PackId", "persona piece"),
            "PackId"
        );
        piece.pieceId = stringValue(
            requireField(value, "PieceId", "persona piece"),
            "PieceId"
        );
        piece.pieceType = stringValue(
            requireField(value, "PieceType", "persona piece"),
            "PieceType"
        );
        piece.productId = stringValue(
            requireField(value, "ProductId", "persona piece"),
            "ProductId"
        );
        skin.personaPieces_.push_back(std::move(piece));
    }

    const auto& tints = requireKind(
        requireField(skin.raw_, "PieceTintColors", "default skin"),
        JsonValue::Kind::Array,
        "PieceTintColors"
    );
    skin.pieceTintColors_.reserve(tints.arrayValue.size());
    for (const auto& value : tints.arrayValue) {
        BedrockPieceTintColors tint;
        tint.pieceType = stringValue(
            requireField(value, "PieceType", "piece tint colors"),
            "PieceType"
        );
        const auto& colors = requireKind(
            requireField(value, "Colors", "piece tint colors"),
            JsonValue::Kind::Array,
            "Colors"
        );
        tint.colors.reserve(colors.arrayValue.size());
        for (const auto& color : colors.arrayValue) {
            tint.colors.push_back(stringValue(color, "persona tint color"));
        }
        skin.pieceTintColors_.push_back(std::move(tint));
    }

    return skin;
}

} // namespace bedrock
