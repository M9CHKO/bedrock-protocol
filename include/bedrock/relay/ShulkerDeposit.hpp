#pragma once

#include <bedrock/generated/GeneratedProtocolTypes.hpp>
#include <bedrock/protodef/ProtoDefReader.hpp>
#include <bedrock/protodef/ProtoDefWriter.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

// This index deliberately never enters Item.extra. Its varint byte length is
// enough to find the next slot, even if extra contains millions of NBT nodes.
// Callers must serialize access; no network or Android dependencies here.
class ShulkerDeposit {
public:
    static constexpr std::size_t MaximumCacheBytes = 16u * 1024u * 1024u;
    static constexpr int MinimumIntervalMs = 30;
    static constexpr int MaximumIntervalMs = 3000;
    static constexpr int DefaultIntervalMs = 1000;
    static constexpr uint64_t ResponseTimeoutMs = 8000;
    struct Item {
        int32_t id = 0, stackId = 0, blockRuntimeId = 0;
        uint32_t metadata = 0;
        uint16_t count = 0;
        std::size_t begin = 0, end = 0, extraBegin = 0;
        bool present() const { return id != 0 && count != 0; }
        bool same(const Item& other) const {
            return id == other.id && count == other.count &&
                metadata == other.metadata && blockRuntimeId == other.blockRuntimeId;
        }
    };
    struct Slot {
        Item item;
        std::shared_ptr<const std::vector<uint8_t>> wire;
        bool known = false;
    };
    struct Inventory {
        uint32_t window = 0, slot = 0;
        uint8_t container = 7;
        std::optional<uint32_t> dynamicId;
        bool full = false;
        std::vector<Item> items;
    };
    struct Click {
        int32_t x = 0, y = 0, z = 0;
        uint32_t runtimeId = 0;
    };
    struct Plan {
        uint32_t window = 0;
        uint8_t source = 0, destination = 0;
        uint8_t sourceContainer = 12, destinationContainer = 7;
        uint64_t generation = 0;
        int32_t requestId = 0;
        bool authoritative = false, modern = true;
        std::optional<uint32_t> dynamicId;
        Slot item;
    };

    bool enabled = false, includeHotbar = false, supported = false;
    bool modern = true;
    bool authoritative = false, inventoryReady = false, chestReady = false;
    bool stopped = false;
    uint32_t window = 0, sent = 0, confirmed = 0;
    std::size_t requiredChestSlots = 0;
    uint64_t generation = 0, nextAt = 0;
    int intervalMs = DefaultIntervalMs;
    std::string status = "Выключено";
    std::set<int32_t> shulkerIds;
    std::array<Slot, 36> player {};
    std::vector<Slot> chest;
    std::optional<Plan> pending;
    std::optional<Plan> clientUpdate;
    uint64_t pendingAt = 0;

    static bool matchesLayout(const std::string& version, const std::string& reference) {
        // Fail closed on a changed wire layout, rather than misreading NBT as
        // a slot index. The generated, bundled schema is the source of truth.
        for (const char* type : {"Item", "ItemStacks",
                "packet_inventory_content", "packet_inventory_slot",
                "TransactionActions", "TransactionLegacy", "TransactionUseItem",
                "WindowIDZigzag32", "WindowIDVarint", "BlockCoordinates",
                "StackRequestSlotInfo", "ContainerSlotType", "ItemStackRequest",
                "packet_item_stack_request", "packet_item_stack_response"}) {
            const auto actual = generatedProtocolTypeJson(version, type);
            const auto expected = generatedProtocolTypeJson(reference, type);
            if (!actual || !expected || *actual != *expected) return false;
        }
        if (reference == "1.21.100" && generatedProtocolTypeJson(version, "FullContainerName") !=
            generatedProtocolTypeJson(reference, "FullContainerName")) return false;
        return true;
    }
    static bool supports(const std::string& version) {
        return matchesLayout(version, "1.21.100") || matchesLayout(version, "1.21.2");
    }
    void setVersion(const std::string& version) {
        modern = matchesLayout(version, "1.21.100");
        supported = modern || matchesLayout(version, "1.21.2");
    }

    static Item readItem(ProtoDefReader& reader) {
        Item item;
        item.begin = reader.offset();
        item.id = reader.zigzag32();
        if (item.id != 0) {
            item.count = reader.u16le();
            item.metadata = reader.varuint32();
            const auto hasStackId = reader.u8();
            if (hasStackId > 1) throw std::runtime_error("invalid item stack flag");
            if (hasStackId) item.stackId = reader.zigzag32();
            item.blockRuntimeId = reader.zigzag32();
            const auto extraBytes = reader.varuint32();
            item.extraBegin = reader.offset();
            reader.skip(extraBytes);
        }
        item.end = reader.offset();
        return item;
    }

    static void readContainer(ProtoDefReader& reader, Inventory& result) {
        result.container = reader.u8();
        const auto present = reader.u8();
        if (present > 1) throw std::runtime_error("invalid dynamic container flag");
        if (present) result.dynamicId = reader.u32le();
    }

    static Inventory readInventory(const std::vector<uint8_t>& bytes, bool full, bool modern = true) {
        PacketFieldCursor cursor(bytes);
        ProtoDefReader reader(cursor);
        Inventory result;
        result.full = full;
        result.window = reader.varuint32();
        result.container = result.window == 0 ? 12 : 7;
        if (full) {
            const auto count = reader.varuint32();
            if (count > 54) throw std::runtime_error("unsupported inventory size");
            result.items.reserve(count);
            for (uint32_t i = 0; i < count; ++i) result.items.push_back(readItem(reader));
            if (modern) { readContainer(reader, result); readItem(reader); } // opaque storage item
        } else {
            result.slot = reader.varuint32();
            if (modern) { readContainer(reader, result); readItem(reader); }
            result.items.push_back(readItem(reader));
        }
        if (reader.remaining()) throw std::runtime_error("inventory trailing bytes");
        return result;
    }

    static constexpr int clampIntervalMs(int value) {
        return std::clamp(value, MinimumIntervalMs, MaximumIntervalMs);
    }

    void configure(bool active, bool hotbar, uint64_t now, int delayMs = DefaultIntervalMs) {
        const bool changed = active != enabled || hotbar != includeHotbar;
        enabled = active;
        includeHotbar = hotbar;
        // A live speed change only affects pacing, never pending requests,
        // server-correction watches, manual pauses, or the opening grace period.
        intervalMs = clampIntervalMs(delayMs);
        if (!changed) return;
        // A request already sent cannot be undone. Keep its acknowledgement
        // state and don't allow a quick off/on to send the same move twice.
        if (!active) status = "Выключено";
        else {
            nextAt = now + 400;
            status = !supported ? "Эта версия протокола пока не поддерживается" :
                stopped ? "Переоткройте сундук для продолжения" : "Откройте сундук";
        }
    }

    void resetSession() {
        close(window);
        player = {};
        inventoryReady = false;
        nextRequestId = -1'500'000;
    }

    void open(uint32_t id, bool verifiedChest, uint64_t now, bool adjacentHalf = false) {
        close(window);
        if (!verifiedChest || id == 0 || id > 100) {
            if (enabled) status = "Нужен обычный или двойной сундук";
            return;
        }
        window = id;
        requiredChestSlots = adjacentHalf ? 54 : 0;
        sent = confirmed = 0;
        stopped = false;
        nextAt = now + 400;
        if (enabled) status = "Ожидание слотов сундука";
    }

    void close(uint32_t id) {
        if (id != window) return; // a late close for an older window is harmless
        if (pending && pending->authoritative) inventoryReady = false;
        ++generation;
        window = 0;
        chestReady = false;
        chest.clear();
        pending.reset();
        clientUpdate.reset();
        watchedPlayer = {};
        watchedChest = {};
        stopped = false;
        if (enabled) status = "Откройте сундук";
    }

    void halt(std::string reason) {
        stopped = true;
        status = std::move(reason);
    }

    void observeInventory(const std::vector<uint8_t>& bytes, bool full, uint64_t /*now*/) {
        const auto update = readInventory(bytes, full, modern);
        if (update.window != 0 && update.window != window) return;
        const bool own = update.window == 0;
        if (own && full && update.items.size() != player.size()) {
            inventoryReady = false;
            player = {};
            halt("Неизвестный размер инвентаря");
            return;
        }
        if (!own && full) {
            if ((update.items.size() != 27 && update.items.size() != 54) ||
                (requiredChestSlots && update.items.size() != requiredChestSlots)) {
                halt("Это не сундук на 27 или 54 слота");
                chestReady = false;
                return;
            }
            chest.resize(update.items.size());
            chestDynamicId = update.dynamicId;
            chestContainerId = update.container;
            // Barrel/shulker/ender chest identities must not be guessed from
            // slot count. A matching physical chest click is also required.
            // Nukkit uses a zero placeholder here in its legacy codec.
            if (update.container != 7 && (authoritative || update.container != 0)) {
                halt("Неподдерживаемый тип контейнера");
                chestReady = false;
                return;
            }
        }
        if (own) playerContainerId = update.container;
        const auto size = own ? player.size() : chest.size();
        const auto first = full ? 0u : update.slot;
        if (first > size || update.items.size() > size - first) {
            if (own) inventoryReady = false;
            else chestReady = false;
            halt("Получен неверный номер слота");
            return;
        }
        // Release replaced items first; a full update must not temporarily
        // hold two whole inventories of opaque nested shulkers.
        if (own && full) for (auto& slot : player) slot.wire.reset();
        for (std::size_t i = 0; i < update.items.size(); ++i) {
            const auto index = first + i;
            const auto& item = update.items[i];
            auto& target = own ? player[index] : chest[index];
            auto& watch = own ? watchedPlayer[index] : watchedChest[index];
            if (watch && !target.item.same(item)) {
                halt("Сервер скорректировал перенос. Переоткройте сундук");
            }
            target = cacheItem(item, bytes, own);
            if (pending) {
                if (own && index == pending->source) {
                    pendingSourceSeen = !item.present();
                }
                if (!own && index == pending->destination) {
                    const auto& source = pending->item.item;
                    const auto& wire = pending->item.wire;
                    const auto extraSize = item.end - item.extraBegin;
                    pendingDestinationSeen = item.same(source) && wire &&
                        extraSize == source.end - source.extraBegin &&
                        std::equal(bytes.begin() + item.extraBegin, bytes.begin() + item.end,
                            wire->begin() + (source.extraBegin - source.begin));
                    if (item.same(source) && !pendingDestinationSeen) {
                        halt("Сервер изменил данные шалкера. Разгрузка остановлена");
                    }
                }
            }
        }
        if (own && full) inventoryReady = true;
        if (!own && full) chestReady = true;
        if (pending && pendingSourceSeen && pendingDestinationSeen) {
            ++confirmed;
            pending.reset();
            if (!stopped && enabled) status = "Перенос подтверждён сервером";
        }
    }

    // Observe only slot headers of genuine client moves. This keeps legacy
    // inventory state current on servers which deliberately omit success
    // echoes. Opening/clicking a block is not a manual inventory move.
    std::optional<Click> observeTransaction(const std::vector<uint8_t>& bytes) {
        PacketFieldCursor cursor(bytes);
        ProtoDefReader reader(cursor);
        const auto legacy = reader.zigzag32();
        if (legacy != 0) {
            const auto count = reader.varuint32();
            if (count > 128) throw std::runtime_error("legacy slot list too large");
            for (uint32_t i = 0; i < count; ++i) {
                reader.u8();
                const auto slots = reader.varuint32();
                if (slots > 256) throw std::runtime_error("legacy slots too large");
                reader.skip(slots);
            }
        }
        const auto type = reader.varuint32();
        if (type > 4) throw std::runtime_error("unknown transaction type");
        const auto count = reader.varuint32();
        if (count > 256) throw std::runtime_error("too many transaction actions");
        struct Change { uint32_t slot; Item item; };
        std::vector<Change> changes;
        for (uint32_t i = 0; i < count; ++i) {
            const auto source = reader.varuint32();
            int32_t inventory = -1;
            if (source == 0) inventory = reader.zigzag32();
            else if (source == 2 || source == 100 || source == 99999) reader.varuint32();
            else if (source != 1 && source != 3) throw std::runtime_error("unknown action source");
            const auto slot = reader.varuint32();
            readItem(reader);
            auto item = readItem(reader);
            if (inventory == 0 && slot < player.size()) changes.push_back({slot, item});
        }
        std::optional<Click> click;
        if (type == 2) {
            const auto action = reader.varuint32();
            if (modern) reader.varuint32(); // trigger was added after 1.21.2
            Click value;
            value.x = reader.zigzag32();
            value.y = static_cast<int32_t>(reader.varuint32());
            value.z = reader.zigzag32();
            reader.zigzag32(); // face
            reader.zigzag32(); // hotbar slot
            readItem(reader);
            reader.skip(24); // position + click position
            value.runtimeId = reader.varuint32();
            if (modern) reader.varuint32(); // prediction
            if (action == 0) click = value;
        }
        if (type <= 2 && reader.remaining()) throw std::runtime_error("transaction trailing bytes");
        if (window && count) manualInteraction();
        for (const auto& change : changes) {
            player[change.slot] = cacheItem(change.item, bytes, true);
        }
        return click;
    }

    void manualInteraction() {
        if (!window) return;
        halt("Ручное перемещение — пауза до следующего открытия сундука");
        watchedPlayer = {};
        watchedChest = {};
    }

    std::optional<Plan> poll(uint64_t now) {
        if (pending) {
            if (pending->authoritative) {
                if (now - pendingAt >= ResponseTimeoutMs) {
                    halt("Нет ответа сервера. Переоткройте сундук");
                    // Keep pending until close: no duplicate on toggle.
                }
                return {};
            }
            if (now - pendingAt < static_cast<uint64_t>(intervalMs)) return {};
            pending.reset(); // legacy prediction, NOT a server acknowledgement
        }
        if (!enabled || !supported || stopped || !window || now < nextAt) return {};
        // Pace actual sends, not scheduled ticks. Never catch up with a burst
        // after a slow packet; even a fast server acknowledgement cannot bypass
        // the selected interval. One authoritative request remains in flight.
        if (sent && now - pendingAt < static_cast<uint64_t>(intervalMs)) return {};
        if (!inventoryReady || !chestReady) {
            status = !inventoryReady ? "Ожидание инвентаря — переоткройте его" : "Ожидание слотов сундука";
            return {};
        }
        std::size_t source = player.size();
        for (std::size_t i = includeHotbar ? 0u : 9u; i < player.size(); ++i) {
            if (player[i].known && player[i].item.present() &&
                shulkerIds.count(player[i].item.id)) {
                if (!player[i].wire) {
                    halt("Недостаточно памяти для безопасного переноса этого шалкера");
                    return {};
                }
                source = i;
                break;
            }
        }
        if (source == player.size()) {
            status = sent ? "Разгрузка завершена" : "Нет шалкеров для разгрузки";
            return {};
        }
        auto empty = std::find_if(chest.begin(), chest.end(), [](const Slot& slot) {
            return slot.known && slot.item.id == 0;
        });
        if (empty == chest.end()) { status = "Сундук заполнен"; return {}; }
        if (authoritative && (player[source].item.stackId <= 0 || player[source].item.count > 255)) {
            halt("Сервер ещё не прислал ID стопки шалкера");
            return {};
        }
        Plan plan;
        plan.window = window;
        plan.generation = generation;
        plan.source = static_cast<uint8_t>(source);
        plan.destination = static_cast<uint8_t>(empty - chest.begin());
        plan.sourceContainer = playerContainerId;
        plan.destinationContainer = chestContainerId;
        plan.item = player[source];
        plan.authoritative = authoritative;
        plan.modern = modern;
        plan.dynamicId = chestDynamicId;
        plan.requestId = nextRequestId--;
        pending = plan;
        pendingAt = now;
        pendingSourceSeen = pendingDestinationSeen = false;
        ++sent;
        if (!authoritative) {
            player[source] = Slot{{}, {}, true};
            empty->item = plan.item.item;
            watchedPlayer[source] = true;
            watchedChest[plan.destination] = true;
            clientUpdate = plan;
        }
        status = authoritative ? "Ожидание подтверждения сервера" : "Перенос: legacy, без отдельного подтверждения";
        return plan;
    }

    void response(int32_t request, bool accepted, uint64_t /*now*/, int32_t destinationStackId = 0) {
        if (!pending || !pending->authoritative || pending->requestId != request) return;
        if (!accepted) {
            halt("Сервер отклонил перенос. Переоткройте сундук");
            pending.reset();
            return;
        }
        // Without the new destination stack ID wait for actual slot echoes;
        // never show a descriptor carrying the stale source stack ID.
        if (destinationStackId <= 0) return;
        pending->item.wire = withStackId(*pending->item.wire, destinationStackId);
        pending->item.item.stackId = destinationStackId;
        player[pending->source] = Slot{{}, {}, true};
        chest[pending->destination] = Slot{pending->item.item, {}, true};
        clientUpdate = pending;
        ++confirmed;
        pending.reset();
    }

    static std::vector<uint8_t> legacyPayload(const Plan& plan) {
        if (!plan.item.wire || plan.window == 0 || plan.window > 100) {
            throw std::runtime_error("invalid deposit plan");
        }
        ProtoDefWriter writer;
        writer.zigzag32(0); // legacy_request_id
        writer.varuint32(0); // normal
        writer.varuint32(2);
        writer.varuint32(0); writer.zigzag32(0); writer.varuint32(plan.source);
        writer.bytes(*plan.item.wire); writer.zigzag32(0);
        writer.varuint32(0); writer.zigzag32(static_cast<int32_t>(plan.window));
        writer.varuint32(plan.destination);
        writer.zigzag32(0); writer.bytes(*plan.item.wire);
        return writer.take();
    }

    static std::vector<uint8_t> slotPayload(uint32_t windowId, uint8_t slot,
        const std::vector<uint8_t>* item, std::optional<uint32_t> dynamicId = {},
        uint8_t containerId = 255, bool modern = true) {
        ProtoDefWriter writer;
        writer.varuint32(windowId); writer.varuint32(slot);
        if (modern) {
            writer.u8(containerId == 255 ? (windowId == 0 ? 12 : 7) : containerId);
            writer.boolValue(dynamicId.has_value());
            if (dynamicId) writer.u32le(*dynamicId);
            writer.zigzag32(0); // storage_item
        }
        if (item) writer.bytes(*item);
        else writer.zigzag32(0);
        return writer.take();
    }

    static ProtoDefValue stackSlotValue(bool modern, std::string name, uint8_t index,
        int32_t id, std::optional<uint32_t> dynamic = {}) {
        using Value = ProtoDefValue;
        Value type = Value::string(std::move(name));
        if (modern) type = Value::object({{"container_id", std::move(type)},
            {"dynamic_container_id", dynamic ? Value::uinteger(*dynamic) : Value::null()}});
        return Value::object({{"slot_type", std::move(type)}, {"slot", Value::uinteger(index)},
            {"stack_id", Value::integer(id)}});
    }
    static ProtoDefValue stackRequestValue(const Plan& plan) {
        using Value = ProtoDefValue;
        return Value::object({{"requests", Value::array({Value::object({
            {"request_id", Value::integer(plan.requestId)},
            {"actions", Value::array({Value::object({{"type_id", Value::string("place")},
                {"count", Value::uinteger(plan.item.item.count)},
                {"source", stackSlotValue(plan.modern, "hotbar_and_inventory", plan.source, plan.item.item.stackId)},
                {"destination", stackSlotValue(plan.modern, "container", plan.destination, 0, plan.dynamicId)}})})},
            {"custom_names", Value::array({})}, {"cause", Value::string("chat_public")}
        })})}});
    }

    std::size_t cachedBytes() const {
        std::size_t bytes = 0;
        for (const auto& slot : player) if (slot.wire) bytes += slot.wire->size();
        if (pending && pending->item.wire) bytes += pending->item.wire->size();
        return bytes;
    }

private:
    int32_t nextRequestId = -1'500'000;
    std::optional<uint32_t> chestDynamicId;
    uint8_t playerContainerId = 12, chestContainerId = 7;
    std::array<bool, 36> watchedPlayer {};
    std::array<bool, 54> watchedChest {};
    bool pendingSourceSeen = false, pendingDestinationSeen = false;

    static std::shared_ptr<const std::vector<uint8_t>> withStackId(const std::vector<uint8_t>& wire, int32_t id) {
        PacketFieldCursor cursor(wire);
        ProtoDefReader reader(cursor);
        ProtoDefWriter writer;
        writer.zigzag32(reader.zigzag32());
        writer.u16le(reader.u16le()); writer.varuint32(reader.varuint32());
        if (reader.u8()) reader.zigzag32();
        writer.u8(1); writer.zigzag32(id);
        writer.bytes(wire.data() + reader.offset(), reader.remaining());
        return std::make_shared<const std::vector<uint8_t>>(writer.take());
    }

    Slot cacheItem(const Item& item, const std::vector<uint8_t>& bytes, bool retain) {
        Slot slot {item, {}, true};
        if (retain && item.present() && shulkerIds.count(item.id)) {
            const auto length = item.end - item.begin;
            if (length <= MaximumCacheBytes && cachedBytes() <= MaximumCacheBytes - length) {
                slot.wire = std::make_shared<const std::vector<uint8_t>>(
                    bytes.begin() + item.begin, bytes.begin() + item.end);
            }
        }
        return slot;
    }
};

} // namespace bedrock
