// Compile the actual shared handler, not a second implementation of its
// predicates. Nothing starts a relay, reads user auth or contacts a server.
#include "../android/relay-app/app/src/main/cpp/native_bridge.cpp"
#include <iostream>

namespace {
using V = bedrock::ProtoDefValue;

void requireCraft(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

V craftItem(int id, int count = 1, int metadata = 0) {
    if (!id) return V::object({{"network_id", V::integer(0)}});
    return V::object({
        {"network_id", V::integer(id)}, {"count", V::integer(count)},
        {"metadata", V::integer(metadata)}, {"has_stack_id", V::integer(0)},
        {"block_runtime_id", V::integer(12)},
        {"extra", V::object({{"has_nbt", V::string("false")},
            {"can_place_on", V::array({})}, {"can_destroy", V::array({})}})}
    });
}

V cachedCraftExtra() {
    using N = bedrock::NbtValue;
    const auto leaf = N::compound({{"Name", N::string("minecraft:stone")},
        {"Count", N::byte(64)}, {"Slot", N::byte(0)}});
    const auto nested = N::compound({{"Name", N::string("minecraft:shulker_box")},
        {"Count", N::byte(1)}, {"tag", N::compound({
            {"Items", N::list(bedrock::NbtTagType::Compound, std::vector<N>(27, leaf))}
        })}});
    const auto nbt = bedrock::nbtDocumentToProtoDefValue({"", N::compound({
        {"CustomName", N::string("Weathertop craft regression")},
        {"Items", N::list(bedrock::NbtTagType::Compound, std::vector<N>(27, nested))}
    })});
    bedrock::ProtoDefWriter writer;
    bedrock::writeProtoDefNbt(writer, nbt, bedrock::BedrockNbtEncoding::LittleEndian);
    auto extra = craftItem(205).objectValue.at("extra");
    extra.objectValue["has_nbt"] = V::string("true");
    extra.objectValue["nbt"] = V::object({{"version", V::integer(1)}, {"nbt", V::bytes(writer.take())}});
    return extra;
}

V sourceAction(int id, std::string source, uint32_t action = 7) {
    auto value = V::object({{"source_type", V::string(source)}, {"slot", V::integer(0)},
        {"old_item", craftItem(id)}, {"new_item", craftItem(0)}});
    if (source == "container") value.objectValue["inventory_id"] = V::string("crafting_result");
    else value.objectValue["action"] = V::uinteger(action);
    return value;
}

V destinationAction(int id, std::string window, int slot, int count = 1, int metadata = 0) {
    return V::object({{"source_type", V::string("container")}, {"inventory_id", V::string(window)},
        {"slot", V::integer(slot)}, {"old_item", craftItem(0)}, {"new_item", craftItem(id, count, metadata)}});
}

bedrock::BedrockRelayPacketEvent craftEvent(const std::string& version, RelayState& state, std::vector<V> actions) {
    const auto params = V::object({{"transaction", V::object({
        {"legacy", V::object({{"legacy_request_id", V::integer(0)}})},
        {"transaction_type", V::string("normal")}, {"actions", V::array(std::move(actions))}
    })}});
    bedrock::ProtoDefPacketEncoder encoder(version, state.itemProtocolVariables);
    bedrock::BedrockRelayPacketEvent event;
    event.direction = bedrock::BedrockRelayDirection::Serverbound;
    event.packet = bedrock::VersionedPacketCodec::forVersion(version).makePacketByName(
        "inventory_transaction", encoder.encodePacket("inventory_transaction", params));
    return event;
}

void verifyCraft(const std::string& version, RelayState& state, const V& extra,
    const std::vector<V>& actions, std::size_t destination, bool expected) {
    const auto oldCount = state.nbtCraftRewriteCount;
    auto event = craftEvent(version, state, actions);
    const auto incomingBytes = event.packet.payload;
    state.maybeRewriteNbtCraft(version, event);
    requireCraft((event.replacements.size() == 1) == expected, "Unexpected craft match/rejection");
    requireCraft(!event.canceled && event.packet.payload == incomingBytes, "Original packet must stay intact");
    requireCraft(state.nbtCraftArmed, "Continuous mode was disarmed by a craft or unrelated action");
    requireCraft(state.nbtCraftRewriteCount == oldCount + (expected ? 1 : 0), "Unexpected rewrite count");
    if (!expected) return;
    bedrock::BedrockRelayPacketEvent outgoing;
    outgoing.packet = event.replacements.front();
    bedrock::RelayPacketEvent decoded(version, outgoing, state.itemProtocolVariables, true, true);
    const auto* resultActions = decoded.value("transaction.actions");
    requireCraft(resultActions && resultActions->arrayValue.size() == actions.size(), "Action count changed");
    const auto& result = resultActions->arrayValue[0].objectValue.at("old_item");
    const auto& added = resultActions->arrayValue[destination].objectValue.at("new_item");
    for (const auto* item : {&result, &added}) {
        const auto& actualExtra = item->objectValue.at("extra");
        requireCraft(actualExtra.objectValue.at("has_nbt").stringValue == "true", "Missing NBT flag");
        const auto& bytes = actualExtra.objectValue.at("nbt").objectValue.at("nbt");
        requireCraft(bytes.kind == V::Kind::Bytes && bytes.bytesValue ==
            extra.objectValue.at("nbt").objectValue.at("nbt").bytesValue, "Nested NBT bytes changed");
        for (const auto* field : {"network_id", "count", "metadata", "block_runtime_id", "has_stack_id"})
            requireCraft(packetInteger(item->get(field)) == packetInteger(actions[0].get("old_item")->get(field)),
                "Original recipe result identity changed");
    }
    requireCraft(resultActions->arrayValue[destination].get("inventory_id")->stringValue ==
        actions[destination].get("inventory_id")->stringValue, "Destination window changed");
    requireCraft(packetInteger(resultActions->arrayValue[destination].get("slot")) ==
        packetInteger(actions[destination].get("slot")), "Destination slot changed");
    for (std::size_t i = 1; i < actions.size(); ++i) {
        if (i == destination) continue;
        requireCraft(resultActions->arrayValue[i].get("new_item")->get("extra")->get("has_nbt")->stringValue == "false",
            "NBT leaked into unrelated action");
    }
}
}

int main() {
    try {
        const auto extra = cachedCraftExtra();
        int total = 0;
        for (const std::string version : {"1.21.2", "1.21.100"}) {
            const int before = total;
            // Deliberately different palette IDs: matching must not hardcode
            // the shulker ID from another version or another server.
            for (int id : {205, 9013}) {
                auto state = std::make_shared<RelayState>();
                state->destinationPort = 19132;
                state->itemNames.emplace(id, "minecraft:undyed_shulker_box");
                state->itemProtocolVariables->setVariable("ShieldItemID", 513);
                state->nbtCraftExtraCache = std::make_shared<const V>(extra);
                state->nbtCraftArmed = true;
                for (const auto& source : {sourceAction(id, "craft"), sourceAction(id, "craft", 4294967292u),
                        sourceAction(id, "craft_slot"), sourceAction(id, "container")}) {
                    for (const auto& dest : {destinationAction(id, "inventory", 10),
                            destinationAction(id, "ui", 0), destinationAction(id, "ui", 50)}) {
                        for (int repeat = 0; repeat < 3; ++repeat) {
                            verifyCraft(version, *state, extra, {source, dest}, 1, true); ++total;
                        }
                    }
                }
                const auto source = sourceAction(id, "craft");
                for (const auto& invalid : {destinationAction(id, "first", 0), destinationAction(id + 1, "ui", 0),
                        destinationAction(id, "ui", 0, 2), destinationAction(id, "ui", 0, 1, 3)}) {
                    verifyCraft(version, *state, extra, {source, invalid}, 1, false); ++total;
                }
                auto occupied = destinationAction(id, "ui", 0);
                occupied.objectValue["old_item"] = craftItem(id);
                verifyCraft(version, *state, extra, {source, occupied}, 1, false); ++total;
                auto move = source;
                move.objectValue["source_type"] = V::string("container");
                move.objectValue["inventory_id"] = V::string("inventory");
                verifyCraft(version, *state, extra, {move, destinationAction(id, "ui", 0)}, 1, false); ++total;
                verifyCraft(version, *state, extra, {sourceAction(id, "craft", 9), destinationAction(id, "ui", 0)}, 1, false); ++total;
                // Skip an unrelated addition before the matching UI pair.
                verifyCraft(version, *state, extra, {source, destinationAction(54, "inventory", 3),
                    destinationAction(id, "ui", 0)}, 2, true); ++total;
                state->itemNames[id] = "minecraft:stone";
                verifyCraft(version, *state, extra, {source, destinationAction(id, "ui", 0)}, 1, false); ++total;
            }
            std::cout << "PASS " << version << ": " << total - before << " craft cases" << std::endl;
        }
        std::cout << "PASS total=" << total << "; continuous crafting; opaque nested NBT; unrelated moves preserved" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << std::endl;
        return 1;
    }
}
