#pragma once

#include "bedrock/relay/ShulkerDeposit.hpp"
#include <map>
#include <tuple>

namespace bedrock {

// Single-session legacy crafting state machine. Caller serializes ALL access
// with the slot index mutex. No timers, threads, nested-NBT trees or retries.
class AutoCraftStore {
public:
    using Wire = std::vector<uint8_t>;
    using Key = std::tuple<int32_t, int32_t, int32_t>;
    struct Target {
        int32_t x = 0, y = 0, z = 0, runtime = 0, face = 1;
        bool chest = false;
        double distance = 0;
        uint64_t confirmedAt = 0; // last server-confirmed manual opening
        Key key() const { return {x, y, z}; }
    };
    enum class Stage { Idle, Table, OpeningTable, Craft, Chest, OpeningChest, Deposit, Closing };
    struct Action {
        enum Kind { None, Open, Close, CraftOne, DepositOne } kind = None;
        Target target;
        uint32_t window = 0;
        uint8_t windowType = 0;
        std::vector<Wire> transactions;
        std::vector<std::pair<uint8_t, Wire>> updates;
        std::optional<ShulkerDeposit::Plan> deposit;
    };
    static constexpr uint64_t TimeoutMs = 8000;
    static constexpr uint64_t CraftIntervalMs = 1000;
    static constexpr uint64_t TargetLifetimeMs = 120000;
    static int clampCraftInterval(int value) { return std::clamp(value, 100, 5000); }
    static int clampWindowPause(int value) { return std::clamp(value, 300, 3000); }
    int craftIntervalMs = CraftIntervalMs, windowPauseMs = 700;
    bool closeSent = false, clientClosed = false, serverClosed = false;
    uint32_t clientCloseId = 0;
    void configureTiming(int craftMs, int windowMs) {
        craftIntervalMs = clampCraftInterval(craftMs);
        windowPauseMs = clampWindowPause(windowMs);
    }
    Stage stage = Stage::Idle, afterClose = Stage::Idle;
    bool running = false, finishing = false;
    std::string status = "Готово: верстак и сундуки в пределах 4 блоков";
    std::string stopReason;
    uint32_t crafted = 0, stored = 0, window = 0;
    uint8_t windowType = 0;
    uint64_t deadline = 0, nextAt = 0, revision = 0;
    int32_t shellId = 0, chestId = 0, resultId = 0, resultRuntime = 0;
    double startX = 0, startY = 0, startZ = 0;
    Target target;
    std::string templateName;
    ShulkerDeposit::Slot preparedResult;
    std::vector<Target> targets;
    std::set<Key> visitedChests;
    // One run, all craft/unload batches. Only occupied slot headers are kept,
    // never chest NBT. Starting again deliberately rechecks available space.
    std::set<Key> fullChests;
    std::map<Key, Key> chestAliases;
    Key openedChestKey{};

    Key chestKey(Key key) const {
        for (size_t i = 0; i < chestAliases.size(); ++i) {
            const auto found = chestAliases.find(key);
            if (found == chestAliases.end() || found->second == key) break;
            key = found->second;
        }
        return key;
    }
    bool chestFull(const Key& key) const { return fullChests.count(chestKey(key)) != 0; }
    bool allChestsFull() const {
        bool found = false;
        for (const auto& candidate : targets) if (candidate.chest) {
            found = true;
            if (!chestFull(candidate.key())) return false;
        }
        return found;
    }

    bool busy() const { return stage != Stage::Idle; }
    std::optional<Target> nextTarget() const {
        if (stage != Stage::Table && stage != Stage::Chest) return {};
        const bool chest = stage == Stage::Chest;
        for (const auto& candidate : targets)
            if (candidate.chest == chest && (!chest ||
                (!visitedChests.count(candidate.key()) && !chestFull(candidate.key())))) return candidate;
        return {};
    }

    void start(ShulkerDeposit& slots, std::vector<Target> nearby,
        double x, double y, double z, uint64_t now) {
        if (busy()) return;
        ++revision;
        if (!slots.supported) { status = "Нужна версия 1.21.2 или 1.21.100"; return; }
        // Never guess stack-request recipe IDs on an authoritative server.
        if (slots.authoritative) { status = "Авто 2: этот сервер требует ItemStackRequest; legacy-крафт отключён"; return; }
        if (!slots.inventoryReady) { status = "Откройте и закройте инвентарь для синхронизации"; return; }
        if (!shellId || !chestId || !resultId || !resultRuntime) {
            status = "Не загружены серверные ID материалов и шалкера"; return;
        }
        if (!preparedResult.wire || preparedResult.item.id != resultId ||
            preparedResult.item.count != 1 || preparedResult.wire->size() > ShulkerDeposit::MaximumCacheBytes) {
            status = "Выберите NBT-шаблон перед запуском Авто 2"; return;
        }
        if (std::none_of(nearby.begin(), nearby.end(), [](const Target& t) { return !t.chest; })) {
            status = "Верстак не найден. Подойдите ближе, откройте и закройте его вручную"; return;
        }
        if (std::none_of(nearby.begin(), nearby.end(), [](const Target& t) { return t.chest; })) {
            status = "Сундук не найден. Подойдите ближе, откройте и закройте его вручную"; return;
        }
        previousEnabled = slots.enabled;
        previousHotbar = slots.includeHotbar;
        frozenResult = preparedResult;
        preparedResult = {};
        slots.configure(false, true, now, slots.intervalMs);
        slots.close(slots.window);
        targets = std::move(nearby);
        std::sort(targets.begin(), targets.end(), [](const Target& a, const Target& b) {
            return a.distance < b.distance;
        });
        startX = x; startY = y; startZ = z;
        running = true; finishing = false; crafted = stored = 0;
        stopReason.clear();
        visitedChests.clear(); fullChests.clear(); chestAliases.clear(); expected = {}; window = 0;
        stage = Stage::Table; nextAt = now; status = "Открываю верстак";
    }

    void stop(ShulkerDeposit& slots, std::string reason, uint64_t now) {
        status = std::move(reason); stopReason = status; running = false; ++revision;
        if (!busy()) { preparedResult = {}; return; }
        // Keep ownership of a delayed open until it arrives (then close it).
        // A fast stop/start cannot reuse an unresolved container request.
        if (stage == Stage::OpeningTable || stage == Stage::OpeningChest) {
            slots.enabled = false; return;
        }
        if (window) {
            if (stage == Stage::Closing && closeSent) {
                afterClose = Stage::Idle; slots.enabled = false; return;
            }
            afterClose = Stage::Idle; stage = Stage::Closing; nextAt = now;
            deadline = 0; closeSent = clientClosed = serverClosed = false; slots.enabled = false;
        } else release(slots);
    }

    void reset(ShulkerDeposit& slots) {
        if (busy()) release(slots);
        running = false; stage = Stage::Idle; window = 0; expected = {};
        frozenResult = {}; preparedResult = {};
        targets.clear(); visitedChests.clear(); fullChests.clear(); chestAliases.clear();
        status = "Сеанс завершён"; ++revision;
    }

    bool opened(ShulkerDeposit& slots, uint32_t id, uint8_t type,
        int32_t x, int32_t y, int32_t z, uint64_t now) {
        if (!busy()) return false;
        const bool opening = stage == Stage::OpeningTable || stage == Stage::OpeningChest;
        const bool isChest = stage == Stage::OpeningChest;
        const int64_t distance = std::abs(int64_t(x) - target.x) + std::abs(int64_t(z) - target.z);
        // Nukkit workbenches use WindowID::None (-1 / wire byte 255),
        // not the dynamic 1..100 range used by physical chests.
        const bool validId = (id >= 1 && id <= 100) || (!isChest && id == 255);
        if (!opening && window != 0 && id == window && type == windowType && y == target.y &&
            distance <= (target.chest ? 1 : 0)) return true; // duplicate response
        if (!opening || !validId || y != target.y ||
            distance > (isChest ? 1 : 0) || type != (isChest ? 0 : 1)) {
            // A response DID arrive. Do not leave Opening* waiting for it
            // again, and do not close a window we do not own.
            status = "Открылось другое окно — остановлено";
            release(slots); return false;
        }
        window = id; windowType = type; deadline = now + TimeoutMs;
        closeSent = clientClosed = serverClosed = false; clientCloseId = 0;
        if (isChest) {
            openedChestKey = {x, y, z};
            visitedChests.insert({x, y, z});
            slots.open(id, true, now, distance == 1);
            slots.configure(running, true, now, slots.intervalMs);
        }
        stage = isChest ? Stage::Deposit : Stage::Craft;
        nextAt = now + windowPauseMs;
        if (!running) stop(slots, status, now);
        else status = isChest ? "Разгружаю шалкеры" : "Крафт с NBT: " + templateName;
        ++revision;
        return true;
    }

    bool closed(ShulkerDeposit& slots, uint32_t id, uint64_t now, bool fromClient) {
        // window=0 means there is NO owned window, not player inventory 0.
        if (!busy() || !window) return false;
        const bool pending = stage == Stage::Closing && closeSent;
        const bool alias = pending && id == 255 &&
            (fromClient || (clientClosed && clientCloseId == 255));
        if (id != window && !alias) return false;
        if (pending) {
            if (fromClient) { clientClosed = true; clientCloseId = id; }
            else serverClosed = true;
            ++revision;
            if (!clientClosed || !serverClosed) {
                status = clientClosed ? "Жду подтверждения сервера" : "Жду закрытия окна Minecraft";
                return true;
            }
            slots.close(window); window = 0;
            stage = afterClose; nextAt = now + windowPauseMs; deadline = 0;
            if (stage == Stage::Idle) { status = stopReason; release(slots); }
        } else if (fromClient) {
            slots.close(window); window = 0;
            stop(slots, "Окно закрыто вручную — автоматизация остановлена", now);
        } else {
            // A server echo alone is not proof that the GUI disappeared.
            stop(slots, "Сервер закрыл окно — автоматизация остановлена", now);
            serverClosed = true;
        }
        return true;
    }

    // Observe only server updates, never our local prediction. Corrections
    // stop the cycle instead of repeatedly spending materials on stale slots.
    void inventory(ShulkerDeposit& slots, const ShulkerDeposit::Inventory& update, uint64_t now) {
        if (!running || update.window != 0) return;
        const auto first = update.full ? 0u : update.slot;
        for (size_t i = 0; i < update.items.size() && first + i < 36; ++i) {
            auto& watch = expected[first + i];
            if (watch && !watch->item.same(update.items[i])) {
                stop(slots, "Сервер скорректировал крафт. Проверьте инвентарь", now); return;
            }
            if (watch && watch->item.id == resultId) {
                const auto& actual = slots.player[first + i];
                if (!sameExtra(*watch, actual)) {
                    stop(slots, "Сервер изменил NBT результата — остановлено", now); return;
                }
            }
        }
    }

    Action poll(ShulkerDeposit& slots, uint64_t now) {
        Action out;
        if (!busy()) return out;
        if (stage == Stage::OpeningTable || stage == Stage::OpeningChest) {
            if (now >= deadline) {
                status = "Нет ответа на открытие. Проверьте доступ к блоку";
                release(slots);
            }
            return out;
        }
        if (stage == Stage::Closing) {
            if (deadline) {
                if (now >= deadline) {
                    status = clientClosed ? "Сервер не подтвердил закрытие — остановлено" :
                        "Minecraft не подтвердил закрытие. Закройте окно вручную";
                    release(slots);
                }
                return out;
            }
            if (now < nextAt) return out;
            out.kind = Action::Close; out.window = window; out.windowType = windowType; out.target = target;
            deadline = now + TimeoutMs; closeSent = true;
            status = "Закрываю окно Minecraft";
            return out;
        }
        if (!running || now < nextAt) return out;
        if (slots.stopped || !slots.inventoryReady) {
            stop(slots, slots.stopped ? slots.status : "Инвентарь не синхронизирован", now); return out;
        }
        if (stage == Stage::Table || stage == Stage::Chest) {
            const bool chest = stage == Stage::Chest;
            if (allChestsFull()) {
                stop(slots, "Все доступные сундуки заполнены — Авто 2 остановлено", now); return out;
            }
            const auto found = nextTarget();
            if (!found) {
                stop(slots, chest ? "Доступные сундуки заполнены или недоступны" : "Нет доступного верстака", now);
                return out;
            }
            target = *found;
            if (chest) visitedChests.insert(target.key());
            stage = chest ? Stage::OpeningChest : Stage::OpeningTable;
            deadline = now + TimeoutMs;
            status = chest ? "Открываю сундук" : "Открываю верстак";
            out.kind = Action::Open; out.target = target;
            return out;
        }
        if (stage == Stage::Craft) {
            int destination = -1, chestSlot = -1, shellA = -1, shellB = -1;
            for (int i = 0; i < 36; ++i) {
                const auto& slot = slots.player[i];
                if (!slot.known) { stop(slots, "Неизвестный слот инвентаря", now); return out; }
                if (!slot.item.present()) { if (destination < 0) destination = i; continue; }
                if (!slot.wire) continue;
                if (slot.item.id == chestId && chestSlot < 0) chestSlot = i;
                if (slot.item.id == shellId) {
                    if (shellA < 0) { shellA = i; if (slot.item.count >= 2) shellB = i; }
                    else if (shellB < 0) shellB = i;
                }
            }
            finishing = chestSlot < 0 || shellA < 0 || shellB < 0;
            if (finishing || destination < 0) {
                if (!hasShulkers(slots)) { stop(slots, finishing ? "Материалы закончились — пополните ресурсы" : "Нет места в инвентаре", now); return out; }
                transitionClose(slots, Stage::Chest, now); return poll(slots, now);
            }
            out.kind = Action::CraftOne;
            std::map<int, ShulkerDeposit::Slot> changed;
            ProtoDefWriter fill;
            fill.zigzag32(0); fill.varuint32(0); fill.varuint32(6);
            const int sourceSlots[] = {shellA, chestSlot, shellB};
            for (int i = 0; i < 3; ++i) {
                const int index = sourceSlots[i];
                const auto before = changed.count(index) ? changed.at(index) : slots.player[index];
                auto after = withCount(before, before.item.count - 1);
                const auto unit = withCount(before, 1);
                fill.varuint32(0); fill.zigzag32(0); fill.varuint32(index);
                fill.bytes(*before.wire); fill.bytes(wire(after));
                fill.varuint32(100); fill.varuint32(9); fill.varuint32(i * 3);
                fill.zigzag32(0); fill.bytes(wire(unit));
                changed[index] = std::move(after);
            }
            auto result = frozenResult;
            if (slots.cachedBytes() > ShulkerDeposit::MaximumCacheBytes - result.wire->size()) {
                stop(slots, "Лимит памяти NBT: разгрузите инвентарь", now);
                out.kind = Action::None; return out;
            }
            ProtoDefWriter take;
            take.zigzag32(0); take.varuint32(0); take.varuint32(2);
            take.varuint32(100); take.varuint32(7); take.varuint32(0);
            take.bytes(wire(result)); take.zigzag32(0);
            take.varuint32(0); take.zigzag32(0); take.varuint32(destination);
            take.zigzag32(0); take.bytes(wire(result));
            changed[destination] = result;
            out.transactions = {fill.take(), take.take()};
            for (const auto& [index, slot] : changed) {
                slots.player[index] = slot;
                expected[index] = slot;
                out.updates.push_back({static_cast<uint8_t>(index), wire(slot)});
            }
            ++crafted; ++revision; nextAt = now + craftIntervalMs;
            return out;
        }
        if (stage == Stage::Deposit) {
            if (!slots.chestReady) {
                if (now >= deadline) stop(slots, "Сундук не прислал слоты", now);
                return out;
            }
            auto plan = slots.poll(now);
            if (plan) {
                expected[plan->source] = ShulkerDeposit::Slot{{}, {}, true};
                out.kind = Action::DepositOne; out.deposit = std::move(plan);
                ++stored; ++revision; return out;
            }
            if (slots.pending) return out;
            // Includes the case where the LAST deposited shulker filled the
            // last free slot. Remember it before deciding to craft again.
            rememberChestCapacity(slots);
            if (!hasShulkers(slots)) {
                if (finishing) { stop(slots, "Шалкеры выгружены. Материалы закончились — пополните ресурсы", now); }
                else if (allChestsFull()) { stop(slots, "Все доступные сундуки заполнены — Авто 2 остановлено", now); }
                else { visitedChests.clear(); transitionClose(slots, Stage::Table, now); }
            } else if (std::none_of(slots.chest.begin(), slots.chest.end(), [](const auto& slot) {
                return slot.known && !slot.item.present();
            })) transitionClose(slots, Stage::Chest, now);
        }
        return out;
    }

    static Wire wire(const ShulkerDeposit::Slot& slot) { return slot.wire ? *slot.wire : Wire{0}; }
    static ShulkerDeposit::Slot withCount(const ShulkerDeposit::Slot& slot, uint16_t count) {
        if (!count) return {{}, {}, true};
        if (!slot.wire) throw std::runtime_error("missing ingredient descriptor");
        PacketFieldCursor cursor(*slot.wire); ProtoDefReader reader(cursor);
        reader.zigzag32(); const auto offset = reader.offset();
        auto bytes = *slot.wire; bytes.at(offset) = count & 255; bytes.at(offset + 1) = count >> 8;
        auto result = slot; result.item.count = count;
        result.wire = std::make_shared<const Wire>(std::move(bytes)); return result;
    }

private:
    bool previousEnabled = false, previousHotbar = false;
    std::array<std::optional<ShulkerDeposit::Slot>, 36> expected{};
    ShulkerDeposit::Slot frozenResult;
    void rememberChestCapacity(const ShulkerDeposit& slots) {
        if (!slots.chestReady || slots.stopped || slots.pending ||
            (slots.chest.size() != 27 && slots.chest.size() != 54)) return;
        // Different requested/returned positions identify a pair only after
        // the server has supplied a real 54-slot chest. Neighbours alone do
        // not prove pairing (two independent single chests may touch).
        if (slots.chest.size() == 54 && openedChestKey != target.key()) {
            auto a = chestKey(target.key()), b = chestKey(openedChestKey);
            if (a != b) {
                const bool full = fullChests.erase(a) || fullChests.count(b);
                chestAliases[a] = b;
                if (full) fullChests.insert(b);
            }
        }
        if (std::all_of(slots.chest.begin(), slots.chest.end(), [](const auto& slot) {
            return slot.known && slot.item.present();
        })) fullChests.insert(chestKey(target.key()));
    }
    static bool sameExtra(const ShulkerDeposit::Slot& a, const ShulkerDeposit::Slot& b) {
        if (!a.wire || !b.wire) return false;
        const auto aOffset = a.item.extraBegin - a.item.begin;
        const auto bOffset = b.item.extraBegin - b.item.begin;
        return aOffset <= a.wire->size() && bOffset <= b.wire->size() &&
            a.wire->size() - aOffset == b.wire->size() - bOffset &&
            std::equal(a.wire->begin() + aOffset, a.wire->end(), b.wire->begin() + bOffset);
    }
    static bool hasShulkers(const ShulkerDeposit& slots) {
        return std::any_of(slots.player.begin(), slots.player.end(), [&](const auto& slot) {
            return slot.known && slot.item.present() && slots.shulkerIds.count(slot.item.id);
        });
    }
    void release(ShulkerDeposit& slots) {
        slots.close(slots.window);
        slots.enabled = previousEnabled; slots.includeHotbar = previousHotbar;
        running = false; stage = Stage::Idle; window = 0; expected = {};
        closeSent = clientClosed = serverClosed = false; clientCloseId = 0;
        frozenResult = {}; preparedResult = {};
        targets.clear(); visitedChests.clear(); ++revision;
    }
    void transitionClose(ShulkerDeposit& slots, Stage next, uint64_t now) {
        slots.enabled = false; afterClose = next; stage = Stage::Closing;
        deadline = 0; nextAt = now;
        closeSent = clientClosed = serverClosed = false; clientCloseId = 0;
    }
};
} // namespace bedrock
