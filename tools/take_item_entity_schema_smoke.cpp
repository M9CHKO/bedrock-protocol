#include <bedrock/debug/PacketSchemaTsvIndex.hpp>
#include <bedrock/debug/ProtocolSchemaIndex.hpp>
#include <bedrock/debug/ProtocolTypeTsvIndex.hpp>
#include <bedrock/generated/GeneratedProtocolTypes.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protocol/VersionedMcpeCodec.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using RuntimeIds = std::pair<uint64_t, uint64_t>;

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[TAKE-ITEM-ENTITY-SCHEMA-SMOKE] " << message << "\n";
    }
    return condition;
}

std::optional<std::string> schemaType(
    const bedrock::PacketSchemaInfo& schema,
    const std::string& fieldName
) {
    for (const auto& field : schema.fields) {
        if (field.name == fieldName) return field.type;
    }
    return std::nullopt;
}

std::optional<uint64_t> decodedValue(
    const std::vector<bedrock::ProtoDefField>& fields,
    const std::string& path
) {
    for (const auto& field : fields) {
        if (field.path != path) continue;
        try {
            return std::stoull(field.value);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

bedrock::ProtoDefValue packetValue(const RuntimeIds& ids) {
    return bedrock::ProtoDefValue::object({
        {"runtime_entity_id", bedrock::ProtoDefValue::uinteger(ids.first)},
        {"target", bedrock::ProtoDefValue::uinteger(ids.second)}
    });
}

bool roundTrip(
    const std::string& version,
    const RuntimeIds& ids,
    const bedrock::ProtocolTypeTsvIndex& typeIndex,
    const std::string& source
) {
    bool ok = true;
    try {
        const bedrock::ProtoDefPacketEncoder encoder(version, typeIndex);
        const bedrock::ProtoDefPacketDecoder decoder(version, typeIndex);
        const auto payload = encoder.encodePacket(
            "take_item_entity",
            packetValue(ids)
        );
        const auto fields = decoder.decodePacketStrict(
            "take_item_entity",
            payload
        );
        const auto runtimeEntityId = decodedValue(fields, "runtime_entity_id");
        const auto target = decodedValue(fields, "target");

        ok &= check(
            runtimeEntityId == std::optional<uint64_t>(ids.first),
            version + " " + source +
                ": runtime_entity_id changed during round-trip"
        );
        ok &= check(
            target == std::optional<uint64_t>(ids.second),
            version + " " + source + ": target changed during round-trip"
        );

        if (runtimeEntityId && target) {
            const auto reencoded = encoder.encodePacket(
                "take_item_entity",
                packetValue({*runtimeEntityId, *target})
            );
            ok &= check(
                reencoded == payload,
                version + " " + source + ": re-encoded payload differs"
            );
        }

        const auto codec = bedrock::VersionedMcpeCodec::forVersion(version);
        const auto full = codec.packetCodec().makePacketByName(
            "take_item_entity",
            payload
        );
        const auto decodedPacket = codec.packetCodec().decodeFullPacket(
            full.fullPacket
        );
        ok &= check(
            decodedPacket.name == "take_item_entity" &&
                decodedPacket.payload == payload &&
                decodedPacket.fullPacket == full.fullPacket,
            version + " " + source + ": packet codec changed payload bytes"
        );
        const auto fullFields = decoder.decodePacketStrict(
            decodedPacket.name,
            decodedPacket.payload
        );
        ok &= check(
            decodedValue(fullFields, "runtime_entity_id") ==
                    std::optional<uint64_t>(ids.first) &&
                decodedValue(fullFields, "target") ==
                    std::optional<uint64_t>(ids.second),
            version + " " + source + ": full-packet decode changed IDs"
        );
    } catch (const std::exception& error) {
        ok = false;
        std::cerr << "[TAKE-ITEM-ENTITY-SCHEMA-SMOKE] " << version << " "
                  << source << ": " << error.what() << "\n";
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    const auto versions = bedrock::ProtocolDefinition::versions();
    auto registeredVersionSet = versions;
    auto generatedVersions = bedrock::generatedProtocolTypeVersions();
    std::sort(registeredVersionSet.begin(), registeredVersionSet.end());
    std::sort(generatedVersions.begin(), generatedVersions.end());
    ok &= check(!versions.empty(), "registered protocol version list is empty");
    ok &= check(
        generatedVersions == registeredVersionSet,
        "GeneratedProtocolTypes version registry differs from ProtocolDefinition"
    );

    const bedrock::PacketSchemaTsvIndex packetTsv;
    const bedrock::ProtocolTypeTsvIndex protocolTsv;
    const bedrock::ProtocolTypeTsvIndex generatedOnly(
        std::filesystem::path("data/generated/does-not-exist")
    );
    const std::vector<RuntimeIds> cases {
        {17u, 42u},
        {0x100000001ULL, 0x200000002ULL},
        {
            std::numeric_limits<uint64_t>::max() - 1u,
            std::numeric_limits<uint64_t>::max()
        }
    };

    std::size_t checkedVersions = 0;
    for (const auto& version : versions) {
        const auto definition = bedrock::ProtocolDefinition::forVersion(version);
        if (!definition.hasPacket("take_item_entity")) continue;
        ++checkedVersions;

        const auto jsonSchema =
            bedrock::ProtocolSchemaIndex::forVersion(version).findPacket(
                "take_item_entity"
            );
        const auto tsvSchema = packetTsv.findPacket(
            version,
            "take_item_entity"
        );
        const auto tsvType = protocolTsv.findTypeJson(
            version,
            "packet_take_item_entity"
        );
        const auto cppType = bedrock::generatedProtocolTypeJson(
            version,
            "packet_take_item_entity"
        );

        ok &= check(jsonSchema.has_value(), version + ": protocol.json schema missing");
        ok &= check(tsvSchema.has_value(), version + ": packet-schema TSV missing");
        ok &= check(tsvType.has_value(), version + ": protocol-types TSV missing");
        ok &= check(cppType.has_value(), version + ": generated C++ schema missing");
        if (!jsonSchema || !tsvSchema || !tsvType || !cppType) continue;

        ok &= check(
            schemaType(*jsonSchema, "runtime_entity_id") ==
                std::optional<std::string>("varint64"),
            version + ": protocol.json runtime_entity_id is not varint64"
        );
        ok &= check(
            schemaType(*jsonSchema, "target") ==
                std::optional<std::string>("varint64"),
            version + ": protocol.json target is not varint64"
        );
        ok &= check(
            schemaType(*tsvSchema, "runtime_entity_id") ==
                std::optional<std::string>("\"varint64\""),
            version + ": packet-schema TSV runtime_entity_id is not varint64"
        );
        ok &= check(
            schemaType(*tsvSchema, "target") ==
                std::optional<std::string>("\"varint64\""),
            version + ": packet-schema TSV target is not varint64"
        );
        ok &= check(
            *cppType == *tsvType,
            version + ": generated C++ schema differs from protocol-types TSV"
        );

        for (const auto& ids : cases) {
            ok &= roundTrip(version, ids, protocolTsv, "TSV");
            ok &= roundTrip(version, ids, generatedOnly, "generated C++");
        }
    }

    ok &= check(
        checkedVersions == versions.size(),
        "take_item_entity is absent from one or more registered versions"
    );

    if (!ok) return 1;
    std::cout << "[TAKE-ITEM-ENTITY-SCHEMA-SMOKE] OK versions="
              << checkedVersions << "\n";
    return 0;
}
