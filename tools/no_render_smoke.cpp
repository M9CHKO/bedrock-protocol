#include <bedrock/relay/NoRender.hpp>
#include <bedrock/protodef/ProtoDefPacketEncoder.hpp>
#include <bedrock/relay/BedrockLiveRelay.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace bedrock;
using V = ProtoDefValue;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static V empty() { return V::array({}); }
static V vec() { return V::object({{"x", V::floating(2)}, {"y", V::floating(64)}, {"z", V::floating(3)}}); }
static V meta(std::string key, std::string type, V value) {
    if (key != "flags") value = V::object({{"type", V::string(type)}, {"$value", std::move(value)}});
    return V::object({{"key", V::string(key)}, {"type", V::string(type)}, {"value", std::move(value)}});
}
static std::string field(const std::vector<ProtoDefField>& fields, const std::string& name) {
    for (const auto& f : fields) if (f.path == name) return f.value;
    return "missing";
}
static float scale(const VersionedGamePacket& p, const std::string& version = "1.21.100") {
    auto fields = ProtoDefPacketDecoder(version).decodePacketStrict(p.name, p.payload);
    for (const auto& f : fields) if (f.path.ends_with("].key") && f.value == "38/scale")
        return std::stof(field(fields, f.path.substr(0, f.path.size() - 3) + "value"));
    throw std::runtime_error("scale missing");
}
static VersionedGamePacket encode(const std::string& name, V value, const std::string& version = "1.21.100") {
    return VersionedPacketCodec::forVersion(version).makePacketByName(name,
        ProtoDefPacketEncoder(version).encodePacket(name, value));
}
static VersionedGamePacket actor(uint64_t id = 22, const std::string& type = "minecraft:zombie", const std::string& version = "1.21.100") {
    return encode("add_entity", V::object({
        {"unique_id", V::integer(-static_cast<int64_t>(id))}, {"runtime_id", V::uinteger(id)},
        {"entity_type", V::string(type)}, {"position", vec()}, {"velocity", vec()},
        {"pitch", V::floating(1)}, {"yaw", V::floating(2)}, {"head_yaw", V::floating(3)}, {"body_yaw", V::floating(4)},
        {"attributes", empty()}, {"links", empty()},
        {"properties", V::object({{"ints", empty()}, {"floats", empty()}})},
        {"metadata", V::array({meta("flags", "long", V::object({{"onfire", V::boolean(true)}, {"can_show_nametag", V::boolean(true)}})),
            meta("scale", "float", V::floating(1.5)), meta("nametag", "string", V::string("Original")),
            meta("health", "int", V::integer(20))})}
    }), version);
}
static VersionedGamePacket blockActor(const std::string& version, const std::string& type) {
    auto tag = [](const std::string& type, V value) { return V::object({{"type", V::string(type)}, {"value", std::move(value)}}); };
    return encode("block_entity_data", V::object({
        {"position", V::object({{"x", V::integer(1)}, {"y", V::integer(64)}, {"z", V::integer(3)}})},
        {"nbt", V::object({{"type", V::string("compound")}, {"name", V::string("")}, {"value", V::object({
            {"id", tag("string", V::string(type))}, {"x", tag("int", V::integer(1))},
            {"y", tag("int", V::integer(64))}, {"z", tag("int", V::integer(3))},
            {"CustomName", tag("string", V::string("Visible container"))}
        })}})}
    }), version);
}
static void containerAndRestoreChecks() {
    for (const std::string version : {"1.21.2", "1.21.100"}) {
        NoRender filter(version);
        auto shulker = actor(70, "minecraft:shulker", version);
        require(filter.process(shulker)->fullPacket == shulker.fullPacket, "visible shulker exact");
        for (int i = 0; i < 6; ++i) {
            bool hidden = i % 2 == 0;
            auto updates = filter.configure(false, hidden);
            require(updates.size() == 1 && scale(updates[0], version) == (hidden ? 0.f : 1.5f), "repeated shulker hide/restore");
            for (const auto& type : {"Chest", "ShulkerBox", "EnderChest", "Barrel"}) {
                auto block = blockActor(version, type);
                require(filter.process(block)->fullPacket == block.fullPacket, "container NBT byte-exact during No Render toggles");
            }
        }
        auto update = encode("set_entity_data", V::object({{"runtime_entity_id", V::uinteger(70)},
            {"metadata", V::array({meta("scale", "float", V::floating(2.5))})},
            {"properties", V::object({{"ints", empty()}, {"floats", empty()}})}, {"tick", V::uinteger(987654)}}), version);
        filter.configure(false, true); filter.process(update);
        auto restored = filter.configure(false, false);
        auto fields = ProtoDefPacketDecoder(version).decodePacketStrict(restored[0].name, restored[0].payload);
        require(scale(restored[0], version) == 2.5f && field(fields, "tick") == "987654", "restoration uses latest server scale and tick on both protocols");
        require(filter.failures() == 0, "container and shulker restoration parsing");
    }
}
static VersionedGamePacket mapPacket(int64_t id, int x = 0, uint32_t color = 0xff123456) {
    return encode("clientbound_map_item_data", V::object({
        {"map_id", V::integer(id)},
        {"update_flags", V::object({{"texture", V::boolean(true)}, {"decoration", V::boolean(true)}, {"initialisation", V::boolean(true)}})},
        {"dimension", V::uinteger(0)}, {"locked", V::boolean(false)},
        {"origin", V::object({{"x", V::integer(0)}, {"y", V::integer(0)}, {"z", V::integer(0)}})},
        {"included_in", V::array({V::integer(id)})}, {"scale", V::uinteger(1)},
        {"tracked", V::object({{"objects", empty()}, {"decorations", empty()}})},
        {"texture", V::object({{"width", V::integer(2)}, {"height", V::integer(2)},
            {"x_offset", V::integer(x)}, {"y_offset", V::integer(0)},
            {"pixels", V::array({V::uinteger(color), V::uinteger(color), V::uinteger(color), V::uinteger(color)})}})}
    }));
}
template<class Predicate> static bool waitFor(Predicate predicate) {
    for (int i = 0; i < 600; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
static VersionedGamePacket nextMap(MapDeliveryQueue& queue) {
    std::optional<VersionedGamePacket> result;
    require(waitFor([&] {
        queue.pump([&](const VersionedGamePacket& packet) { result = packet; return true; });
        return result.has_value();
    }), "scheduled map timeout");
    return std::move(*result);
}
static void mapChecks() {
    MapImageCodec codec("1.21.100");
    MapDeliveryQueue queue("1.21.100");
    auto original = mapPacket(-987654321);
    require(queue.intercept(original), "visible map must always be intercepted");
    require(queue.intercept(mapPacket(-987654321, 2, 12345)), "partial map intercepted");
    require(waitFor([&] { auto s = queue.stats(); return !s.workerBusy && s.queuedPackets == 0 && s.prepared; }), "disk merge ready");
    bool called = false;
    require(!queue.pump([&](const auto&) { called = true; return false; }) && called, "backpressure must retain map");
    auto merged = nextMap(queue);
    auto fields = ProtoDefPacketDecoder("1.21.100").decodePacketStrict(merged.name, merged.payload);
    require(merged.fullPacket != original.fullPacket, "never return original partial payload");
    require(field(fields, "texture.pixels[0]") == std::to_string(0xff123456), "earlier rectangle preserved");
    require(field(fields, "texture.pixels[2]") == "12345", "partial rectangle merged");
    require(field(fields, "texture.pixels[128]") == std::to_string(0xff123456), "row stride preserved");
    queue.configureHidden(true);
    auto blank = nextMap(queue);
    fields = ProtoDefPacketDecoder("1.21.100").decodePacketStrict(blank.name, blank.payload);
    require(field(fields, "texture.pixels[0]") == "0", "scheduled blank image");
    queue.configureHidden(false); queue.configureHidden(true); queue.configureHidden(false);
    auto restored = nextMap(queue);
    require(restored.fullPacket == merged.fullPacket, "restore merged image after rapid toggles");
    auto bad = original; bad.payload.pop_back();
    require(queue.intercept(bad), "malformed map is consumed");
    require(queue.stats().malformed == 1 && queue.stats().reloadNeeded == 1, "malformed map marked for reload");
    require(!queue.pump([](const auto&) { throw std::runtime_error("invalid map forwarded"); return true; }), "invalid map never sent");
    MapImageCodec::State state;
    MapImageCodec::merge(state, codec.validate(original.payload), original.payload);
    state.pixels.fill(42);
    auto full = codec.packet(state, false);
    require(queue.intercept(full), "full refresh accepted");
    auto refreshed = nextMap(queue);
    fields = ProtoDefPacketDecoder("1.21.100").decodePacketStrict(refreshed.name, refreshed.payload);
    require(field(fields, "texture.pixels[2]") == "42" && queue.stats().reloadNeeded == 0, "full refresh replaces earlier image and repairs reload");
    queue.intercept(original); queue.reset();
    require(queue.stats().maps == 0 && !queue.pump([](const auto&) { return true; }), "reset cancels queued and prepared maps");
    auto mislabeled = original; mislabeled.name = "text"; mislabeled.packetId = 9;
    require(queue.intercept(mislabeled), "wire map header cannot bypass map gate");
    require(waitFor([&] { return queue.stats().prepared; }), "reset callback fixture ready");
    require(waitFor([&] {
        queue.pump([&](const auto&) { queue.reset(); return false; });
        return queue.stats().maps == 0;
    }), "transport reset during send must not deadlock or replay stale state");

    for (size_t length = 0; length < original.payload.size(); ++length) {
        auto truncated = original;
        truncated.payload.resize(length);
        require(queue.intercept(truncated), "every truncated map must be consumed");
    }
    require(!queue.pump([](const auto&) { return true; }), "truncated maps never emitted");
    auto bounds = original; bounds.payload[0] = 0x80;
    bounds.payload.assign(MapImageCodec::MaxWireBytes + 1, 0xff);
    require(queue.intercept(bounds) && queue.stats().reloadAll, "oversize input bounded before parse/allocation");

    MapDeliveryOptions tiny; tiny.memoryBytes = MapDeliveryQueue::WorkingReserve; tiny.diskBytes = 5 * MapSpoolPages::PageBytes;
    MapDeliveryQueue memory("1.21.100", tiny);
    require(memory.intercept(original) && memory.stats().overflow == 1 && memory.stats().reloadNeeded == 1, "RAM overflow fail closed");
    require(memory.stats().memoryReserved <= memory.stats().memoryBudget, "RAM budget bounded");
    tiny.memoryBytes = 8 * 1024 * 1024;
    MapDeliveryQueue disk("1.21.100", tiny);
    disk.intercept(mapPacket(1)); disk.intercept(mapPacket(2));
    require(waitFor([&] { return disk.stats().overflow == 1 && disk.stats().reloadNeeded == 1; }), "disk overflow fail closed");
    require(disk.stats().diskReserved <= disk.stats().diskBudget, "disk slots bounded");
    for (int i = 3; i < 1000; ++i) disk.intercept(mapPacket(i));
    require(disk.stats().maps <= 261 && disk.stats().reloadAll, "reload index bounded under flood");

    MapDeliveryOptions growthOptions; growthOptions.diskBytes = 26 * MapSpoolPages::PageBytes;
    MapDeliveryQueue growth("1.21.100", growthOptions);
    state.id = 700; state.pixels.fill(0);
    growth.intercept(codec.packet(state, false)); nextMap(growth);
    require(growth.stats().diskReserved == 5 * MapSpoolPages::PageBytes, "small image uses five pages");
    state.pixels.fill(0xffffffffu);
    growth.intercept(codec.packet(state, false)); auto grown = nextMap(growth);
    MapImageCodec::State grownState;
    MapImageCodec::merge(grownState, codec.validate(grown.payload), grown.payload);
    require(grownState.pixels[0] == 0xffffffffu && growth.stats().diskReserved == 21 * MapSpoolPages::PageBytes,
        "record growth preserves data and releases old pages");
    state.id = 701; state.pixels.fill(0);
    growth.intercept(codec.packet(state, false)); nextMap(growth);
    require(growth.stats().diskReserved == 26 * MapSpoolPages::PageBytes && !growth.stats().overflow,
        "old pages reused for another image");
    state.pixels.fill(0xffffffffu);
    growth.intercept(codec.packet(state, false));
    require(waitFor([&] { return growth.stats().overflow == 1; }) && growth.stats().diskReserved == 26 * MapSpoolPages::PageBytes,
        "failed growth preserves hard disk budget");
    state.pixels.fill(0);
    growth.intercept(codec.packet(state, false)); nextMap(growth);
    require(growth.stats().reloadNeeded == 0, "full refresh recovers after growth overflow");

    MapSendBudget budget;
    auto t = MapSendBudget::Time{} + std::chrono::seconds(10);
    for (size_t i = 0; i < MapSendBudget::PacketsPerSecond; ++i) {
        require(budget.allows(t + MapSendBudget::MinimumGap * i, 1000), "paced per-second allowance");
        budget.charge(t + MapSendBudget::MinimumGap * i, 1000);
    }
    require(!budget.allows(t + std::chrono::milliseconds(999), 1), "extra update forbidden");
    t += std::chrono::seconds(100);
    require(budget.allows(t, 200000), "one update after long pause");
    budget.charge(t, 200000);
    require(!budget.allows(t, 1), "no catch-up burst");
    require(!budget.allows(t + std::chrono::milliseconds(250), MapSendBudget::BytesPerSecond - 199999), "byte ceiling independent of packet ceiling");
    require(budget.allows(t + std::chrono::seconds(1), 82000), "rolling byte budget recovers");

    // A regular file cannot be used as a spool directory: worker failure is
    // isolated from gameplay and never causes passthrough.
    tiny.spoolDirectory = std::filesystem::path(__FILE__);
    MapDeliveryQueue failed("1.21.100", tiny); failed.intercept(original);
    require(waitFor([&] { return failed.stats().ioErrors > 0; }), "disk worker reports failure");
    require(failed.stats().reloadNeeded == 1 && !failed.pump([](const auto&) { return true; }), "disk error fail closed");
}
static void liveCheck() {
    std::mutex mutex;
    BedrockServerConnection target;
    std::atomic<bool> joined {false}, sawSpawn {false}, sawHidden {false}, sawRestored {false}, action {false};
    std::atomic<int> mapCount {0};
    std::atomic<bool> originalEscaped {false};
    std::atomic<bool> markerReceived {false};
    std::atomic<bool> schedulerReported {false};
    std::atomic<int> containers {0};
    std::vector<std::pair<MapSendBudget::Time, size_t>> deliveries;
    BedrockServer upstream({.host = "127.0.0.1", .port = 0, .version = "1.21.100", .offline = true});
    upstream.onJoin([&](const BedrockServerConnection& connection) {
        std::lock_guard lock(mutex); target = connection; joined = true;
    });
    auto swing = VersionedPacketCodec::forVersion("1.21.100").makePacketByName("animate", {2, 22});
    upstream.on("animate", [&](const BedrockServerPacketEvent& e) { action = e.packet.fullPacket == swing.fullPacket; });
    upstream.listen();
    BedrockLiveRelayOptions options;
    options.server.host = "127.0.0.1"; options.server.port = 0; options.server.version = "1.21.100"; options.server.offline = true;
    options.upstream.host = "127.0.0.1"; options.upstream.port = upstream.boundPort();
    options.upstream.version = "1.21.100"; options.upstream.offline = true; options.upstream.username = "NoRenderUp";
    BedrockLiveRelay relay(options);
    relay.onDiagnostic([&](const std::string& line) {
        if (line.find("map_scheduler intercepted=") != std::string::npos) schedulerReported = true;
    });
    relay.configureNoRender(false, false);
    relay.listen();
    auto client = createNetworkClient({.host = "127.0.0.1", .port = relay.boundPort(),
        .username = "NoRenderDown", .version = "1.21.100", .offline = true});
    client.on("add_entity", [&](const BedrockNetworkClientPacketEvent& e) { sawSpawn = scale(e.packet) == 1.5f; });
    client.on("set_entity_data", [&](const BedrockNetworkClientPacketEvent& e) {
        if (scale(e.packet) == 0) sawHidden = true;
        if (scale(e.packet) == 1.5f) sawRestored = true;
    });
    client.on("clientbound_map_item_data", [&](const BedrockNetworkClientPacketEvent& e) {
        if (e.packet.fullPacket == mapPacket(1).fullPacket) originalEscaped = true;
        { std::lock_guard lock(mutex); deliveries.emplace_back(MapSendBudget::Clock::now(), e.packet.fullPacket.size() + 5); }
        ++mapCount;
    });
    client.on("animate", [&](const BedrockNetworkClientPacketEvent& e) { markerReceived = e.packet.fullPacket == swing.fullPacket; });
    client.on("block_entity_data", [&](const BedrockNetworkClientPacketEvent& e) {
        if (e.packet.fullPacket == blockActor("1.21.100", "Chest").fullPacket ||
            e.packet.fullPacket == blockActor("1.21.100", "ShulkerBox").fullPacket) ++containers;
    });
    try {
        require(client.connect(), "live connect");
        require(waitFor([&] { return joined.load() && relay.upstreamReady(); }), "live relay ready");
        BedrockServerConnection connection;
        { std::lock_guard lock(mutex); connection = target; }
        upstream.sendPackets(connection, {actor(), mapPacket(1)});
        require(waitFor([&] { return sawSpawn.load() && mapCount.load() == 1; }), "live scheduled visuals");
        require(!originalEscaped.load(), "original payload escaped mixed batch");
        relay.configureNoRender(true, true); relay.pumpNoRender();
        upstream.sendPackets(connection, {blockActor("1.21.100", "Chest"), blockActor("1.21.100", "ShulkerBox")});
        require(waitFor([&] { return sawHidden.load() && mapCount.load() == 2; }), "live hide");
        require(waitFor([&] { return containers.load() == 2; }), "live containers pass while entities hidden");
        client.sendPacket(swing);
        require(waitFor([&] { return action.load(); }), "serverbound action must reach server while hidden");
        relay.configureNoRender(false, false); relay.pumpNoRender();
        require(waitFor([&] { return sawRestored.load() && mapCount.load() == 3; }), "live restore");
        upstream.sendPackets(connection, {blockActor("1.21.100", "Chest"), blockActor("1.21.100", "ShulkerBox")});
        require(waitFor([&] { return containers.load() == 4; }), "live containers pass after restore");
        const auto downstream = relay.server().clients().begin()->second;
        auto malformed = mapPacket(800); malformed.payload.pop_back(); malformed.fullPacket.pop_back();
        // Every public sink must intercept maps, including sendBuffer and a
        // mixed ordinary/map batch. None may emit a forwarded event early.
        relay.server().sendPackets(downstream, {mapPacket(101), malformed, swing});
        relay.server().queuePackets(downstream, {mapPacket(102), malformed});
        relay.server().sendBuffer(downstream, mapPacket(103).fullPacket, true);
        require(relay.queueClientboundPackets(downstream, {mapPacket(104), malformed}), "relay map sink");
        std::vector<VersionedGamePacket> flood;
        for (int id = 200; id < 220; ++id) flood.push_back(mapPacket(id));
        upstream.sendPackets(connection, flood);
        require(waitFor([&] { return markerReceived.load(); }), "ordinary marker passes map flood");
        require(mapCount.load() < 27, "map flood not emitted as a burst");
        action = false; client.sendPacket(swing);
        require(waitFor([&] { return action.load(); }), "gameplay passes map flood");
        require(waitFor([&] { return mapCount.load() >= 10; }), "scheduled flood progress");
        require(waitFor([&] { return schedulerReported.load(); }), "scheduler health visible with item diagnostics disabled");
        {
            std::lock_guard lock(mutex);
            for (size_t i = 0; i < deliveries.size(); ++i) {
                size_t bytes = 0, count = 0;
                for (size_t j = i; j < deliveries.size() && deliveries[j].first - deliveries[i].first < std::chrono::milliseconds(980); ++j) {
                    bytes += deliveries[j].second; ++count;
                }
                require(count <= MapSendBudget::PacketsPerSecond && bytes <= MapSendBudget::BytesPerSecond, "live count and plaintext-byte budget");
            }
        }
        client.close(); relay.close(); upstream.close();
    } catch (...) { client.close(); relay.close(); upstream.close(); throw; }
}
int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "--live") {
            liveCheck(); std::cout << "No Render live relay: hide, gameplay and restore OK\n"; return 0;
        }
        containerAndRestoreChecks();
        NoRender filter("1.21.100");
        auto spawn = actor();
        require(filter.process(spawn)->fullPacket == spawn.fullPacket, "off must be byte-exact");
        require(filter.failures() == 0 && filter.cachedActors() == 1, "actor cache");
        auto hidden = filter.configure(false, true);
        require(hidden.size() == 1 && scale(hidden[0]) == 0, "hide existing actor");
        auto changed = filter.process(actor(23));
        require(changed.has_value() && scale(*changed) == 0, "hide newly spawned actor");
        auto update = encode("set_entity_data", V::object({{"runtime_entity_id", V::uinteger(22)},
            {"metadata", V::array({meta("scale", "float", V::floating(2.5))})},
            {"properties", V::object({{"ints", empty()}, {"floats", empty()}})}, {"tick", V::uinteger(17)}}));
        require(scale(*filter.process(update)) == 0, "server update must not reveal actor");
        auto restored = filter.configure(false, false);
        require(restored.size() == 2 && scale(restored[0]) == 2.5f, "restore latest server scale");
        require(scale(restored[1]) == 1.5f, "restore spawn scale");
        auto fields = ProtoDefPacketDecoder("1.21.100").decodePacketStrict(restored[0].name, restored[0].payload);
        require(field(fields, "metadata[0].value.onfire") == "true", "restore original flags");
        require(field(fields, "metadata[1].value") == "Original", "restore nametag");
        // RemoveActor is a signed unique ID, not a runtime ID.
        auto remove = VersionedPacketCodec::forVersion("1.21.100").makePacketByName("remove_entity", {43});
        filter.process(remove);
        require(filter.configure(false, true).size() == 1, "removed actor must not return");
        filter.configure(false, false);

        // Maps are exclusively tested through their bounded scheduler.
        require(!filter.process(mapPacket(1)), "actor filter must not pass maps");
        mapChecks();
        auto movement = VersionedPacketCodec::forVersion("1.21.100").makePacketByName("move_entity", {1, 2, 3});
        require(filter.process(movement)->fullPacket == movement.fullPacket, "movement untouched");
        filter.reset(); require(filter.cachedActors() == 0, "session reset");
        auto dropped = encode("add_item_entity", V::object({
            {"entity_id_self", V::integer(-50)}, {"runtime_entity_id", V::uinteger(50)},
            {"item", V::object({{"network_id", V::integer(0)}})},
            {"position", vec()}, {"velocity", vec()}, {"metadata", empty()}, {"is_from_fishing", V::boolean(false)}
        }));
        auto player = encode("add_player", V::object({
            {"uuid", V::string("00000000-0000-0000-0000-000000000051")}, {"username", V::string("OtherPlayer")},
            {"runtime_id", V::uinteger(51)}, {"platform_chat_id", V::string("")},
            {"position", vec()}, {"velocity", vec()}, {"pitch", V::floating(0)}, {"yaw", V::floating(0)}, {"head_yaw", V::floating(0)},
            {"held_item", V::object({{"network_id", V::integer(0)}})}, {"gamemode", V::integer(0)},
            {"metadata", empty()}, {"properties", V::object({{"ints", empty()}, {"floats", empty()}})},
            {"unique_id", V::integer(-51)}, {"permission_level", V::integer(1)}, {"command_permission", V::integer(0)},
            {"abilities", empty()}, {"links", empty()}, {"device_id", V::string("")}, {"device_os", V::integer(1)}
        }));
        NoRender entities("1.21.100"); entities.configure(false, true);
        require(scale(*entities.process(dropped)) == 0, "dropped-item spawn hidden");
        require(scale(*entities.process(player)) == 0, "player spawn hidden");
        require(entities.failures() == 0, "item/player metadata parser");
        auto defaults = entities.configure(false, false);
        require(defaults.size() == 2 && scale(defaults[0]) == 1 && scale(defaults[1]) == 1, "restore absent metadata defaults");
        entities.process(VersionedPacketCodec::forVersion("1.21.100").makePacketByName("remove_entity", {101}));
        require(entities.cachedActors() == 1, "player removed by signed unique ID");
        NoRender self("1.21.100"); self.configure(false, true);
        self.process(VersionedPacketCodec::forVersion("1.21.100").makePacketByName("start_game", {2, 22}));
        require(self.process(actor())->fullPacket == actor().fullPacket && self.cachedActors() == 0, "own player excluded");
        std::cout << "No Render: entity hide/restore, flags, removals, scheduled maps, patches, rate/byte limits, overflow and no passthrough OK\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
