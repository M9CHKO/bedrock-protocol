#include <bedrock/registry/BedrockFeatureRegistry.hpp>
#include <bedrock/world/MinecraftDataAssets.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bedrock::BedrockFeatureRegistry features(
    const bedrock::MinecraftDataAssets& assets,
    const std::string& version
) {
    return assets.loadBedrockFeatureRegistryByVersion(version);
}

} // namespace

int main() {
    try {
        bedrock::MinecraftDataAssets assets;

        const auto paths = assets.resolveByVersion("1.26.0");
        require(
            paths.featuresJson.filename() == "features.json" &&
                paths.featuresJson.parent_path().filename() == "common",
            "Bedrock features.json path mismatch"
        );
        require(
            paths.protocolVersionsJson.filename() == "protocolVersions.json",
            "Bedrock protocolVersions.json path mismatch"
        );

        auto current = features(assets, "1.26.0");
        require(current.featureCount() == 16, "Bedrock feature count mismatch");
        require(
            current.version().minecraftVersion == "1.26.0" &&
                current.version().protocol == 924,
            "selected feature version mismatch"
        );
        require(current.supportsFeature("usesPalettedChunks"), "palette feature missing");
        require(current.supportsFeature("tallWorld"), "tall-world feature missing");
        require(current.supportsFeature("blockHashes"), "block hash feature missing");
        require(
            current.supportsFeature("compressorInPacketHeader"),
            "compressor-header feature missing"
        );
        require(
            current.supportsFeature("itemRegistryPacket"),
            "item-registry packet feature missing"
        );
        require(
            current.supportsFeature("newLoginIdentityFields"),
            "new login identity feature missing"
        );
        require(
            !current.supportsFeature("smallWorld"),
            "obsolete small-world feature leaked into current data"
        );
        require(
            !current.supportsFeature("missing") &&
                current.supportFeature("missing").kind ==
                    bedrock::ProtoDefValue::Kind::Bool &&
                !current.supportFeature("missing").boolValue,
            "unknown feature must return false"
        );

        require(
            current.featureString("whereDurabilityIsSerialized") ==
                std::optional<std::string_view>("Damage"),
            "durability value feature mismatch"
        );
        require(
            current.featureString("typeOfValueForEnchantLevel") ==
                std::optional<std::string_view>("short"),
            "enchantment level value feature mismatch"
        );
        require(
            current.featureString("nbtNameForEnchant") ==
                std::optional<std::string_view>("ench"),
            "enchantment NBT name feature mismatch"
        );
        require(
            current.supportFeature("whereDurabilityIsSerialized").kind ==
                bedrock::ProtoDefValue::Kind::String,
            "value feature lost its JSON type"
        );
        const auto* blockHashes = current.feature("blockHashes");
        require(
            blockHashes != nullptr && blockHashes->supported() &&
                !blockHashes->description.empty(),
            "feature definition metadata mismatch"
        );

        require(current.isSameVersion("1.26.0"), "same-version comparison failed");
        require(current.isSameVersion("1.26"), "major .0 alias comparison failed");
        require(
            current.isNewerThan("1.21.130") &&
                current.isNewerOrEqualTo("1.20.61") &&
                !current.isOlderThan("1.16.201"),
            "newer-version comparisons failed"
        );
        bool unknownVersionRejected = false;
        try {
            (void) current.isOlderThan("9.99.0");
        } catch (const std::runtime_error&) {
            unknownVersionRejected = true;
        }
        require(unknownVersionRejected, "unknown comparison version was accepted");

        auto smallWorld = features(assets, "0.14.3");
        require(smallWorld.supportsFeature("smallWorld"), "0.14.3 smallWorld mismatch");
        require(
            !smallWorld.supportsFeature("usesPalettedChunks") &&
                smallWorld.isOlderThan("1.16.201"),
            "legacy feature range mismatch"
        );

        require(
            features(assets, "1.16.220").supportsFeature(
                "itemSerializeUsesAuxValue"
            ),
            "1.16.220 auxiliary-value boundary mismatch"
        );
        require(
            !features(assets, "1.17.0").supportsFeature(
                "itemSerializeUsesAuxValue"
            ),
            "1.17.0 auxiliary-value boundary mismatch"
        );
        require(
            !features(assets, "1.17.40").supportsFeature("tallWorld") &&
                features(assets, "1.18.0").supportsFeature("tallWorld"),
            "tall-world boundary mismatch"
        );
        require(
            !features(assets, "1.19.70").supportsFeature("blockHashes") &&
                features(assets, "1.19.80").supportsFeature("blockHashes"),
            "block-hash boundary mismatch"
        );
        require(
            !features(assets, "1.20.50").supportsFeature(
                "compressorInPacketHeader"
            ) &&
                features(assets, "1.20.61").supportsFeature(
                    "compressorInPacketHeader"
                ),
            "compressor-header boundary mismatch"
        );
        require(
            !features(assets, "1.21.50").supportsFeature("itemRegistryPacket") &&
                features(assets, "1.21.60").supportsFeature("itemRegistryPacket"),
            "item-registry packet boundary mismatch"
        );
        require(
            !features(assets, "1.21.80").supportsFeature(
                "newLoginIdentityFields"
            ) &&
                features(assets, "1.21.90").supportsFeature(
                    "newLoginIdentityFields"
                ),
            "new login identity boundary mismatch"
        );

        require(
            assets.resolveByProtocol(594).version.minecraftVersion == "1.20.15",
            "duplicate protocol 594 must select the newest Bedrock release"
        );
        require(
            assets.resolveByProtocol(567).version.minecraftVersion == "1.19.62",
            "duplicate protocol 567 must select the newest Bedrock release"
        );

        std::cout << "bedrock feature registry smoke ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bedrock feature registry smoke failed: " << error.what() << '\n';
        return 1;
    }
}
