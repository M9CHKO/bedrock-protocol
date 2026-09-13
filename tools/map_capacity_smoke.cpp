#include <bedrock/relay/MapRequestQueue.hpp>
#include <bitset>
#include <iostream>

using namespace bedrock;
using namespace std::chrono_literals;
static void require(bool b, const char* why) { if (!b) throw std::runtime_error(why); }
static constexpr size_t Count = 4000;
static constexpr int64_t Base = int64_t(1) << 40;
static VersionedGamePacket request(const std::string& version, int64_t id) {
    std::vector<uint8_t> bytes; MapImageCodec::zig(bytes, id);
    bytes.insert(bytes.end(), {0,0,0,0});
    return VersionedPacketCodec::forVersion(version).makePacketByName("map_info_request", bytes);
}
static void capacity(const std::string& version) {
    MapImageCodec codec(version); MapRequestQueue requests(version); MapDeliveryQueue images(version);
    auto now = MapSendBudget::Clock::now() + 1h;
    for (size_t i = 0; i < Count; ++i) requests.intercept(request(version, Base + i), now);
    require(requests.stats().pending == Count && requests.stats().overflow == 0, "all 4000 hidden requests retained");
    requests.intercept(request(version, Base + Count), now);
    require(requests.stats().pending == Count && requests.stats().overflow == 1, "request 4001 consumed at exact cap");
    require(!requests.pump([](const auto&){return true;}, true, 0, now), "hidden demand held");
    for (int phase = 0; phase < 3; ++phase) {
        images.configureHidden(phase == 1);
        std::bitset<Count> seen; size_t count = 0;
        const auto deadline = MapSendBudget::Clock::now() + 100s;
        while (count != Count && MapSendBudget::Clock::now() < deadline) {
            if (phase == 0) requests.pump([&](const VersionedGamePacket& packet) {
                const auto id = MapImageCodec::id(packet.payload).value();
                MapImageCodec::State state; state.id = id; state.common = {0,0,0,0,0};
                state.included = {0}; state.tracked = {0,0};
                state.pixels.fill(0xff000000u | static_cast<uint32_t>(id - Base));
                require(images.intercept(codec.packet(state, false)), "map response intercepted");
                requests.response(id); return true;
            }, false, images.stats().pending, now);
            images.pump([&](const VersionedGamePacket& p) {
                auto patch = codec.validate(p.payload); MapImageCodec::State state;
                MapImageCodec::merge(state, patch, p.payload);
                require(state.id >= Base && state.id < Base + Count, "spooled map ID intact");
                auto index = static_cast<size_t>(state.id - Base);
                require(!seen[index], "no repeated output instead of missing map");
                const auto color = phase == 1 ? 0u : 0xff000000u | static_cast<uint32_t>(index);
                for (auto pixel : state.pixels) require(pixel == color, "all stored pixels preserved across hide/restore");
                seen.set(index); ++count; return true;
            }, now);
            now += 125ms; // Virtual scheduler time: no ten-minute wall-clock wait.
            auto s = images.stats();
            require(!s.overflow && !s.malformed && !s.ioErrors && !s.reloadAll, "no image loss within capacity");
            require(s.memoryReserved <= s.memoryBudget && s.diskReserved <= s.diskBudget, "RAM/disk caps remain bounded");
            std::this_thread::sleep_for(100us);
        }
        require(count == Count && seen.all(), "all 4000 maps complete, including last map");
        std::cout << version << " phase=" << phase << " all=" << count << " disk=" << images.stats().diskReserved << '\n';
    }
    require(requests.stats().sent == Count && requests.stats().pending == 0, "all requests sent once");
    require(images.stats().maps == Count && images.stats().sent == Count * 3, "all maps retained for restoration");
    MapImageCodec::State extra; extra.id = Base + Count; extra.common = {0,0,0,0,0}; extra.tracked = {0,0};
    images.intercept(codec.packet(extra, false));
    require(images.stats().maps == Count && images.stats().overflow == 1 && images.stats().reloadAll, "image 4001 consumed at exact cap");
    images.reset(); require(images.stats().diskReserved == 0 && images.stats().maps == 0, "reset cancels page index");
}
static void pages() {
    MapSpoolPages p(3 * MapSpoolPages::PageBytes);
    auto a = p.allocate(1), b = p.allocate(4097);
    require(a && b && a->first == 0 && b->first == 1 && p.reserved() == 12288, "page rounding and hard cap");
    require(!p.allocate(1) && p.reserved() == 12288, "full allocation fails without state change");
    p.release(*a); auto reused = p.allocate(4000);
    require(reused && reused->first == 0, "freed pages reused");
    p.reset(); require(p.reserved() == 0 && p.allocate(12288).has_value(), "allocator reset");
}
int main() {
    try {
        pages(); for (const std::string v : {"1.21.2", "1.21.100"}) capacity(v);
        std::cout << "4000-map capacity: OK\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
