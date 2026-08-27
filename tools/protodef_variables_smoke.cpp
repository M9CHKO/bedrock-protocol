#include <bedrock/debug/ProtocolTypeTsvIndex.hpp>
#include <bedrock/events/BedrockPacketEventDispatcher.hpp>
#include <bedrock/protocol/VersionedPacketCodec.hpp>
#include <bedrock/protodef/ProtoDefContext.hpp>
#include <bedrock/protodef/ProtoDefDecoder.hpp>
#include <bedrock/protodef/ProtoDefEncoder.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/protodef/ProtoDefReader.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>
#include <bedrock/protodef/ProtoDefWriter.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Value = bedrock::ProtoDefValue;

Value object(std::unordered_map<std::string, Value> fields) {
    return Value::object(std::move(fields));
}

Value array(std::vector<Value> values = {}) {
    return Value::array(std::move(values));
}

std::vector<uint8_t> unhex(const std::string& text) {
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    if ((text.size() & 1u) != 0) throw std::runtime_error("odd hex length");
    std::vector<uint8_t> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = digit(text[i]);
        const int low = digit(text[i + 1]);
        if (high < 0 || low < 0) throw std::runtime_error("invalid hex");
        out.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return out;
}

bool sameBytes(
    const std::string& label,
    const std::vector<uint8_t>& actual,
    const std::vector<uint8_t>& expected
) {
    if (actual == expected) return true;
    std::cerr << "[FAIL] " << label << " byte mismatch: actual="
              << actual.size() << " expected=" << expected.size() << "\n";
    return false;
}

Value shieldItem(int32_t networkId = 513) {
    return object({
        {"network_id", Value::integer(networkId)},
        {"count", Value::uinteger(1)},
        {"metadata", Value::uinteger(0)},
        {"has_stack_id", Value::uinteger(0)},
        {"block_runtime_id", Value::integer(0)},
        {"extra", object({
            {"has_nbt", Value::string("false")},
            {"can_place_on", array()},
            {"can_destroy", array()},
            {"blocking_tick", Value::integer(123)}
        })}
    });
}

Value ordinaryItem(int32_t networkId = 514) {
    auto value = shieldItem(networkId);
    value.objectValue["extra"].objectValue.erase("blocking_tick");
    return value;
}

Value emptyNbt() {
    return object({
        {"type", Value::string("compound")},
        {"name", Value::string("")},
        {"value", object({})}
    });
}

Value itemRegistry() {
    return object({
        {"itemstates", array({
            object({
                {"name", Value::string("minecraft:shield")},
                {"runtime_id", Value::integer(513)},
                {"component_based", Value::boolean(false)},
                {"version", Value::string("none")},
                {"nbt", emptyNbt()}
            })
        })}
    });
}

Value inventorySlot() {
    return object({
        {"window_id", Value::string("inventory")},
        {"slot", Value::uinteger(0)},
        {"container", object({
            {"container_id", Value::string("inventory")},
            {"dynamic_container_id", Value::null()}
        })},
        {"storage_item", object({{"network_id", Value::integer(0)}})},
        {"item", shieldItem()}
    });
}

std::optional<std::string> typeFor(
    const bedrock::ProtocolTypeTsvIndex& index,
    const std::string& version,
    const std::string& name
) {
    return index.findTypeJson(version, name);
}

const bedrock::ProtoDefField* field(
    const std::vector<bedrock::ProtoDefField>& fields,
    const std::string& path
) {
    const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& item) {
        return item.path == path;
    });
    return found == fields.end() ? nullptr : &*found;
}

bool checkRawItemVectors() {
    constexpr const char* version = "1.21.100";
    bedrock::ProtocolTypeTsvIndex index;
    const auto itemType = typeFor(index, version, "Item");
    if (!itemType.has_value()) {
        std::cerr << "[FAIL] Item schema missing\n";
        return false;
    }

    auto resolver = [&](const std::string& name) {
        return typeFor(index, version, name);
    };

    bool ok = true;
    bedrock::ProtoDefEncoder encoder(resolver);
    encoder.setVariable("ShieldItemID", 513);

    bedrock::ProtoDefWriter shieldWriter;
    encoder.encode(*itemType, shieldItem(), shieldWriter);
    const auto shieldGolden = unhex(
        "8208010000000012000000000000000000007b00000000000000"
    );
    ok = sameBytes("shield Item Node golden", shieldWriter.take(), shieldGolden) && ok;

    bedrock::ProtoDefWriter ordinaryWriter;
    encoder.encode(*itemType, ordinaryItem(), ordinaryWriter);
    ok = sameBytes(
        "ordinary Item Node golden",
        ordinaryWriter.take(),
        unhex("840801000000000a00000000000000000000")
    ) && ok;

    try {
        bedrock::PacketFieldCursor cursor(shieldGolden);
        bedrock::ProtoDefReader reader(cursor);
        bedrock::ProtoDefContext context;
        std::vector<bedrock::ProtoDefField> fields;
        bedrock::ProtoDefDecoder decoder(resolver);
        decoder.setVariable("ShieldItemID", 513);
        decoder.decode(*itemType, reader, "item", fields, context);
        const auto* blockingTick = field(fields, "item.extra.blocking_tick");
        if (reader.remaining() != 0 || !blockingTick || blockingTick->value != "123") {
            std::cerr << "[FAIL] shield Item dynamic decode branch\n";
            ok = false;
        }
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] shield Item decode: " << error.what() << "\n";
        ok = false;
    }

    return ok;
}

bool checkPacketPaletteState() {
    constexpr const char* version = "1.21.100";
    const auto codec = bedrock::VersionedPacketCodec::forVersion(version);
    const auto variables = bedrock::makeProtoDefVariableStore();
    bedrock::ProtoDefPacketEncoder encoder(version, variables);
    bedrock::ProtoDefPacketDecoder decoder(version, variables);
    bool ok = true;

    try {
        const auto registryPayload = encoder.encodePacket("item_registry", itemRegistry());
        const auto registryFull = codec.encodeFullPacketByName("item_registry", registryPayload);
        const auto registryGolden = unhex(
            "a20101106d696e6563726166743a736869656c64010200040a0000"
        );
        ok = sameBytes("item_registry Node golden", registryFull, registryGolden) && ok;
        if (encoder.variable("ShieldItemID") != std::optional<std::string>("513")) {
            std::cerr << "[FAIL] outbound palette did not set ShieldItemID\n";
            ok = false;
        }

        const auto slotPayload = encoder.encodePacket("inventory_slot", inventorySlot());
        const auto slotFull = codec.encodeFullPacketByName("inventory_slot", slotPayload);
        const auto slotGolden = unhex(
            "3200001d00008208010000000012000000000000000000007b00000000000000"
        );
        ok = sameBytes("inventory_slot Node golden", slotFull, slotGolden) && ok;

        const auto freshVariables = bedrock::makeProtoDefVariableStore();
        bedrock::ProtoDefPacketEncoder pairedEncoder(version, freshVariables);
        bedrock::ProtoDefPacketDecoder pairedDecoder(version, freshVariables);
        const auto decodedRegistry = codec.decodeFullPacket(registryGolden);
        (void) pairedDecoder.decodePacket(decodedRegistry.name, decodedRegistry.payload);
        if (pairedDecoder.variable("ShieldItemID") != std::optional<std::string>("513")) {
            std::cerr << "[FAIL] inbound palette did not set ShieldItemID\n";
            ok = false;
        }
        const auto pairedSlot = codec.encodeFullPacketByName(
            "inventory_slot",
            pairedEncoder.encodePacket("inventory_slot", inventorySlot())
        );
        ok = sameBytes("shared decoder/encoder palette", pairedSlot, slotGolden) && ok;

        const auto decodedSlot = codec.decodeFullPacket(slotGolden);
        const auto slotFields = pairedDecoder.decodePacket(decodedSlot.name, decodedSlot.payload);
        const auto* blockingTick = field(slotFields, "item.extra.blocking_tick");
        if (!blockingTick || blockingTick->value != "123") {
            std::cerr << "[FAIL] packet decoder did not use ShieldItemID\n";
            ok = false;
        }
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] packet palette state: " << error.what() << "\n";
        ok = false;
    }

    return ok;
}

bool checkEventDispatcherPaletteState() {
    constexpr const char* version = "1.21.100";
    const auto codec = bedrock::VersionedPacketCodec::forVersion(version);
    bool ok = true;

    try {
        const auto registry = codec.decodeFullPacket(unhex(
            "a20101106d696e6563726166743a736869656c64010200040a0000"
        ));
        const auto slot = codec.decodeFullPacket(unhex(
            "3200001d00008208010000000012000000000000000000007b00000000000000"
        ));
        const bedrock::GamePacket registryPacket{
            .packetId = registry.packetId,
            .name = registry.name,
            .payload = registry.payload,
            .fullPacket = registry.fullPacket
        };
        const bedrock::GamePacket slotPacket{
            .packetId = slot.packetId,
            .name = slot.name,
            .payload = slot.payload,
            .fullPacket = slot.fullPacket
        };

        bedrock::BedrockPacketEventDispatcher dispatcher(version);
        (void) dispatcher.dispatch(registryPacket);
        const auto shieldId = dispatcher.variableStore()->variable("ShieldItemID");
        if (shieldId != std::optional<std::string>("513")) {
            std::cerr << "[FAIL] event dispatcher did not retain item palette state\n";
            ok = false;
        }

        const auto event = dispatcher.dispatch(slotPacket);
        if (event.fieldValue("item.extra.blocking_tick") != "123") {
            std::cerr << "[FAIL] event dispatcher did not reuse ShieldItemID\n";
            ok = false;
        }
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] event dispatcher palette state: "
                  << error.what() << "\n";
        ok = false;
    }

    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = checkRawItemVectors() && ok;
    ok = checkPacketPaletteState() && ok;
    ok = checkEventDispatcherPaletteState() && ok;
    if (!ok) return 1;
    std::cout << "[PROTODEF-VARIABLES] dynamic switches and ShieldItemID palette state ok\n";
    return 0;
}
