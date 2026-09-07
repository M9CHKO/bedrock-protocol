#include <bedrock/relay/ShulkerDeposit.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <chrono>
#include <iostream>

using Deposit = bedrock::ShulkerDeposit;
using Bytes = std::vector<uint8_t>;

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static Bytes item(int32_t id, std::size_t extra = 10) {
    bedrock::ProtoDefWriter w;
    w.zigzag32(id);
    if (id) {
        w.u16le(1); w.varuint32(0); w.u8(1); w.zigzag32(41);
        w.zigzag32(12); w.varuint32(static_cast<uint32_t>(extra));
        w.bytes(Bytes(extra, 0));
    }
    return w.take();
}

static Bytes inventory(uint32_t window, const std::vector<Bytes>& items) {
    bedrock::ProtoDefWriter w;
    w.varuint32(window); w.varuint32(static_cast<uint32_t>(items.size()));
    for (const auto& value : items) w.bytes(value);
    w.u8(window == 0 ? 12 : 7); w.boolValue(false); w.zigzag32(0);
    return w.take();
}

static Deposit setup(bool authoritative = true) {
    Deposit d;
    d.supported = Deposit::supports("1.21.100");
    require(d.supported, "current wire layout supported");
    d.shulkerIds = {205, 218};
    d.authoritative = authoritative;
    d.configure(true, false, 0);
    auto slots = std::vector<Bytes>(36, item(0));
    slots[0] = item(218); slots[9] = item(205); slots[10] = item(218);
    slots[11] = item(54); // chest items themselves must not be moved
    d.observeInventory(inventory(0, slots), true, 0);
    d.open(7, true, 0);
    auto chest = std::vector<Bytes>(27, item(0));
    chest[0] = item(54);
    d.observeInventory(inventory(7, chest), true, 0);
    return d;
}

static void stateTests() {
    auto d = setup();
    require(!d.poll(399), "wait until chest open settles");
    const auto first = d.poll(400);
    require(first && first->source == 9 && first->destination == 1, "only shulkers into empty slots, protect hotbar");
    require(!d.poll(1000), "one request in flight");
    d.response(first->requestId + 1, true, 1000);
    require(d.pending.has_value(), "ignore foreign response");
    d.response(first->requestId, true, 1000, 99);
    require(d.confirmed == 1 && d.clientUpdate, "confirmed slot update");
    const auto echoed = Deposit::readInventory(Deposit::slotPayload(7, 1, d.clientUpdate->item.wire.get()), false);
    require(echoed.items[0].stackId == 99, "use authoritative destination stack ID, not old source ID");
    require(!d.poll(1399), "early acknowledgement cannot bypass configured interval");
    auto second = d.poll(1400);
    require(second && second->source == 10 && second->destination == 2, "all shulkers, not one-shot");
    d.close(8);
    require(d.window == 7 && d.pending, "late unrelated close must not cancel active chest");
    d.response(second->requestId, false, 1400);
    require(d.stopped && !d.poll(10000), "rejection stops without retries");
    d.close(7);
    require(!d.pending && !d.window && d.chest.empty(), "close releases chest and pending");

    d = setup();
    const auto timeout = d.poll(400);
    require(timeout.has_value(), "plan before timeout");
    d.response(timeout->requestId, true, 500);
    require(d.pending && d.confirmed == 0, "response missing destination ID must await real slots");
    d.poll(9000);
    require(d.stopped && d.pending, "timeout does not retry ambiguous transfer");
    d.configure(false, false, 9001); d.configure(true, false, 9002);
    require(!d.poll(10000), "toggle must not duplicate pending move");

    d = setup();
    d.manualInteraction();
    require(d.stopped && !d.poll(500), "manual move pauses automation");
    d.configure(false, false, 600); d.configure(true, false, 700);
    require(!d.poll(1200), "toggle cannot resume stale manually changed chest");
    d.open(8, false, 1000);
    require(!d.window, "do not deposit in unverified containers/ender chests/shulkers");
    d.open(9, true, 1000);
    require(!d.poll(2000), "must wait for actual contents of new window");
    d.observeInventory(inventory(9, std::vector<Bytes>(54, item(54))), true, 2100);
    require(!d.poll(2500), "full double chest cannot overwrite");

    d = setup();
    d.open(8, true, 0, true);
    d.observeInventory(inventory(8, std::vector<Bytes>(27, item(0))), true, 10);
    require(!d.poll(400), "adjacent click cannot authorize single chest");
    d.open(8, true, 500, true);
    d.observeInventory(inventory(8, std::vector<Bytes>(54, item(0))), true, 510);
    require(d.poll(900).has_value(), "click either half of double chest");

    d = setup();
    d.configure(false, false, 0);
    d.observeInventory(inventory(7, std::vector<Bytes>(27, item(0))), true, 10);
    require(!d.poll(400), "disabled never sends");
    d.configure(true, false, 500);
    require(d.poll(900).has_value(), "button enables deposit inside already open chest");

    d = setup();
    d.configure(true, true, 0);
    require(d.poll(400)->source == 0, "hotbar opt-in");
    d.resetSession();
    require(!d.inventoryReady && !d.poll(20000) && d.cachedBytes() == 0, "disconnect frees cache");
}

static void legacyTests() {
    auto d = setup(false);
    auto nukkitContents = inventory(7, std::vector<Bytes>(27, item(0)));
    nukkitContents[nukkitContents.size() - 3] = 0; // Nukkit fullContainerName placeholder
    d.observeInventory(nukkitContents, true, 0);
    require(!d.stopped && d.chestReady, "legacy container name zero compatibility");
    d = setup(false);
    const auto plan = d.poll(400);
    require(plan && !plan->authoritative && d.clientUpdate, "legacy prediction available");
    const auto payload = Deposit::legacyPayload(*plan);
    bedrock::ProtoDefPacketDecoder decoder("1.21.100");
    // The general decoder independently validates our hand-written packet.
    decoder.decodePacketStrict("inventory_transaction", payload);
    auto probe = setup(false);
    probe.close(7);
    probe.observeTransaction(payload);
    require(!probe.player[9].item.present(), "legacy transaction removes exact source");
    require(!d.poll(1399), "legacy interval bounded");
    require(d.confirmed == 0, "silence must not be reported as confirmation");
    require(d.poll(1400)->source == 10, "legacy continues without server success echo");
    // A delayed rejection must halt even after the next legacy operation.
    const auto rollback = Deposit::slotPayload(0, 9, plan->item.wire.get());
    d.observeInventory(rollback, false, 1500);
    require(d.stopped && !d.poll(20000), "late correction halts legacy transfer");

    d = setup(false);
    const auto move = d.poll(400);
    d.observeInventory(Deposit::slotPayload(0, 9, nullptr), false, 500);
    d.observeInventory(Deposit::slotPayload(7, 1, move->item.wire.get()), false, 600);
    require(d.confirmed == 1, "legacy counts real echoes separately");
    auto typed = Deposit::readInventory(Deposit::slotPayload(7, 1, move->item.wire.get(), 123), false);
    require(typed.dynamicId == 123 && typed.items[0].id == 205, "dynamic container and opaque slot roundtrip");

    d = setup(false);
    const auto changed = d.poll(400);
    auto differentNbt = *changed->item.wire;
    differentNbt.back() ^= 1;
    d.observeInventory(Deposit::slotPayload(0, 9, nullptr), false, 500);
    d.observeInventory(Deposit::slotPayload(7, 1, &differentNbt), false, 600);
    require(d.stopped && d.confirmed == 0, "same item ID/count is not proof of identical NBT");
}

static void largeTests() {
    // Intentionally invalid NBT inside valid length-delimited descriptors:
    // the index must not attempt to interpret or normalize this byte region.
    auto heavy = item(205, 220000);
    std::fill(heavy.end() - 220000, heavy.end(), 0xff);
    auto payload = inventory(7, std::vector<Bytes>(27, heavy));
    auto d = setup(false);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) d.observeInventory(payload, true, 10);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    require(d.chest.size() == 27, "large chest indexed");
    for (const auto& slot : d.chest) require(!slot.wire, "never retain chest NBT");
    require(d.cachedBytes() < 1000, "large chest does not grow retained bytes");
    std::cout << "opaque chest bytes=" << payload.size() << " scans=1000 elapsedMs=" << elapsed << '\n';

    auto slots = std::vector<Bytes>(36, item(0));
    slots[9] = heavy;
    d.observeInventory(inventory(0, slots), true, 10);
    d.observeInventory(inventory(7, std::vector<Bytes>(54, item(0))), true, 10);
    const auto plan = d.poll(400);
    require(plan && *plan->item.wire == heavy, "nested item bytes kept exactly");
    const auto transaction = Deposit::legacyPayload(*plan);
    auto target = setup(false);
    target.close(7);
    target.observeTransaction(transaction);
    require(!target.player[9].item.present(), "large item transaction bypasses NBT");

    bool threw = false;
    payload.resize(payload.size() - 30);
    try { Deposit::readInventory(payload, true); }
    catch (const std::exception&) { threw = true; }
    require(threw, "truncated payload rejected before move");
    threw = false;
    try { Deposit::readInventory(inventory(7, std::vector<Bytes>(55, item(0))), true); }
    catch (const std::exception&) { threw = true; }
    require(threw, "oversized slot array bounded");

    d = setup();
    d.close(7);
    for (int i = 0; i < 100; ++i) d.observeInventory(inventory(0, slots), true, i);
    require(d.cachedBytes() == heavy.size(), "repeated inventory snapshots release previous buffers");
    d.resetSession();
    require(d.cachedBytes() == 0, "all opaque buffers released at disconnect");
}

static void wholeInventoryTest() {
    auto d = setup(false);
    auto slots = std::vector<Bytes>(36, item(205, 110000));
    d.observeInventory(inventory(0, slots), true, 0);
    d.observeInventory(inventory(7, std::vector<Bytes>(54, item(0))), true, 0);
    for (uint64_t i = 0; i < 27; ++i) {
        auto plan = d.poll(400 + i * Deposit::DefaultIntervalMs);
        require(plan && plan->source == i + 9 && plan->destination == i,
            "every main inventory shulker must be moved exactly once");
        require(*plan->item.wire == slots[i + 9], "whole inventory preserves exact data");
        d.clientUpdate.reset(); // emulate the immediate client slot delivery
    }
    require(!d.poll(30000) && d.sent == 27 && d.confirmed == 0,
        "legacy finishes all slots without inventing confirmations");
    for (std::size_t i = 0; i < 9; ++i) require(d.player[i].item.present(), "hotbar preserved");
    for (std::size_t i = 9; i < 36; ++i) require(!d.player[i].item.present(), "source emptied");
    for (std::size_t i = 0; i < 27; ++i) require(d.chest[i].item.id == 205 && !d.chest[i].wire, "destination indexed only");
}

static void speedTests() {
    require(Deposit::clampIntervalMs(-2147483647 - 1) == 30, "negative interval clamped");
    require(Deposit::clampIntervalMs(2147483647) == 3000, "large interval clamped");
    require(Deposit::clampIntervalMs(1000) == 1000, "default interval preserved");
    for (bool authoritative : {false, true}) {
        auto d = setup(authoritative);
        d.configure(true, false, 0, 30);
        require(!d.poll(399), "fast setting retains opening grace period");
        auto first = d.poll(400);
        require(first.has_value(), "first fast move");
        if (authoritative) {
            require(!d.poll(440), "fast setting still waits for acknowledgement");
            d.response(first->requestId, true, 450, 99);
        }
        const uint64_t nextTime = authoritative ? 450 : 430;
        if (!authoritative) require(!d.poll(429), "30ms is a lower bound");
        auto second = d.poll(nextTime);
        require(second && second->source == 10, "30ms move continues safely");
        require(!d.poll(nextTime), "no catch-up burst at the same timestamp");
    }

    auto d = setup(false);
    const auto first = d.poll(400);
    d.configure(true, false, 410, 3000);
    require(d.pending && d.pending->requestId == first->requestId,
        "speed change preserves pending operation");
    require(!d.poll(3399), "live slowdown measured from last actual send");
    d.configure(true, false, 3399, 30);
    require(d.poll(3399).has_value(), "live speedup applies without restarting module");
    require(!d.poll(3399), "speedup cannot create a backlog burst");
    d.manualInteraction();
    d.configure(true, false, 3400, 100);
    require(d.stopped && !d.poll(10000), "speed change never resumes manual pause");

    d = setup();
    auto request = d.poll(400);
    d.configure(true, false, 410, 30);
    require(!d.poll(8399) && !d.stopped, "speed does not shorten response timeout");
    require(!d.poll(8400) && d.stopped && d.pending, "timeout remains fail-closed at fast speed");
    d.configure(false, false, 8410, 30);
    d.configure(true, false, 8420, 30);
    require(!d.poll(9000), "fast off/on never duplicates timed-out request");

    // Real slot echoes use the same pacing as explicit modern responses.
    d = setup(false);
    request = d.poll(400);
    d.observeInventory(Deposit::slotPayload(0, 9, nullptr), false, 410);
    d.observeInventory(Deposit::slotPayload(7, 1, request->item.wire.get()), false, 420);
    require(d.confirmed == 1 && !d.poll(1399) && d.poll(1400), "slot echoes retain minimum interval");

    d = setup(false);
    d.configure(true, true, 0, 30);
    require(d.poll(400).has_value(), "start before a long stall");
    require(d.poll(10000).has_value(), "one move after a long stall");
    require(!d.poll(10000) && !d.poll(10029), "long stall creates no catch-up sends");
    require(d.poll(10030).has_value(), "pacing restarts at actual send time");
}

int main() {
    try { stateTests(); legacyTests(); largeTests(); wholeInventoryTest(); speedTests(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Shulker deposit smoke passed\n";
}
