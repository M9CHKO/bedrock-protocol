#pragma once

#include <bedrock/protocol/VersionedPayloadReader.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>

namespace bedrock {

// Client-only presentation filter. It never drops movement, inventory, actor
// creation/removal or serverbound traffic. Call under the relay dispatch lock.
class NoRender {
    using Bytes = std::vector<uint8_t>;
    struct Actor {
        int64_t uniqueId = 0;
        uint64_t tick = 0;
        std::map<uint32_t, Bytes> original;
    };
    std::string version_;
    VersionedPacketCodec codec_;
    ProtoDefPacketDecoder decoder_;
    bool properties_ = false;
    bool actorsHidden_ = false;
    uint64_t self_ = 0;
    std::map<uint64_t, Actor> actors_;
    std::size_t failures_ = 0, capacitySkips_ = 0;
    static constexpr std::array<uint32_t, 5> VisualKeys {0, 4, 38, 81, 84};

    static void var(Bytes& out, uint64_t value) {
        do { auto b = static_cast<uint8_t>(value & 127); value >>= 7;
            out.push_back(b | (value ? 128 : 0)); } while (value);
    }
    static void zig(Bytes& out, int64_t value) {
        var(out, (static_cast<uint64_t>(value) << 1) ^
            static_cast<uint64_t>(-(value < 0)));
    }
    static void append(Bytes& out, const Bytes& bytes) {
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    static Bytes slice(const Bytes& data, std::size_t begin, std::size_t end) {
        if (begin > end || end > data.size()) throw std::runtime_error("No Render range");
        return Bytes(data.begin() + begin, data.begin() + end);
    }
    static bool visual(uint32_t key) {
        return std::find(VisualKeys.begin(), VisualKeys.end(), key) != VisualKeys.end();
    }
    static Bytes defaultEntry(uint32_t key) {
        Bytes out; var(out, key);
        if (key == 0) { var(out, 7); zig(out, 0); }
        else if (key == 38) { var(out, 3); append(out, {0, 0, 128, 63}); }
        else if (key == 81) { var(out, 0); out.push_back(0); }
        else { var(out, 4); var(out, 0); }
        return out;
    }
    static Bytes entry(const Actor& actor, uint32_t key, bool hidden) {
        auto it = actor.original.find(key);
        Bytes original = it == actor.original.end() ? defaultEntry(key) : it->second;
        if (!hidden) return original;
        if (key == 0) {
            VersionedPayloadCursor in(original);
            in.readVarUInt();
            if (in.readVarUInt() != 7) throw std::runtime_error("No Render flags type");
            auto flags = static_cast<uint64_t>(in.readVarLong());
            flags |= uint64_t(1) << 5; // invisible
            flags &= ~((uint64_t(1) << 14) | (uint64_t(1) << 15));
            Bytes out; var(out, 0); var(out, 7); zig(out, static_cast<int64_t>(flags));
            return out;
        }
        auto out = defaultEntry(key);
        if (key == 38) out = {38, 3, 0, 0, 0, 0}; // zero scale hides equipment too
        return out;
    }
    VersionedGamePacket actorUpdate(uint64_t id, const Actor& actor, bool hidden) const {
        Bytes out; var(out, id); var(out, VisualKeys.size());
        for (auto key : VisualKeys) append(out, entry(actor, key, hidden));
        if (properties_) { var(out, 0); var(out, 0); }
        var(out, actor.tick); // Do not rewind a previously observed metadata update to tick zero.
        return codec_.makePacketByName("set_entity_data", out);
    }
    static VersionedGamePacket replaced(const VersionedGamePacket& packet, Bytes payload) {
        auto out = packet;
        out.fullPacket.resize(packet.fullPacket.size() - packet.payload.size());
        append(out.fullPacket, payload); // preserve subclient bits in header
        out.payload = std::move(payload);
        return out;
    }
    std::optional<VersionedGamePacket> actorPacket(const VersionedGamePacket& packet) {
        if (packet.payload.size() > 1024 * 1024) throw std::runtime_error("No Render oversized actor");
        const auto fields = decoder_.decodePacketStrict(packet.name, packet.payload);
        uint64_t id = 0;
        int64_t unique = 0;
        uint64_t tick = 0;
        std::size_t begin = packet.payload.size(), end = 0;
        std::vector<std::size_t> starts;
        for (const auto& f : fields) {
            if (f.path == "runtime_id" || f.path == "runtime_entity_id") id = std::stoull(f.value);
            if (f.path == "unique_id" || f.path == "entity_id_self") unique = std::stoll(f.value);
            if (f.path == "tick") tick = std::stoull(f.value);
            if (f.path == "metadata.$count") begin = f.offset;
            if (f.path.rfind("metadata[", 0) == 0) {
                end = std::max(end, f.offset + f.size);
                if (f.path.ends_with("].key")) starts.push_back(f.offset);
            }
        }
        if (!id || id == self_) return packet;
        if (packet.name == "set_entity_data" && !actors_.contains(id)) return packet;
        if (begin == packet.payload.size()) throw std::runtime_error("No Render metadata not found");
        VersionedPayloadCursor countIn(packet.payload);
        countIn.readBytes(begin);
        const auto count = countIn.readVarUInt();
        end = std::max(end, countIn.offset());
        if (starts.size() != count) throw std::runtime_error("No Render metadata count");
        const bool spawn = packet.name != "set_entity_data";
        if (!actors_.contains(id) && actors_.size() >= 4096) { ++capacitySkips_; return packet; }
        Actor actor = spawn ? Actor{} : actors_[id];
        if (spawn) actor.uniqueId = unique;
        actor.tick = std::max(actor.tick, tick);
        std::vector<Bytes> kept;
        for (std::size_t i = 0; i < starts.size(); ++i) {
            auto raw = slice(packet.payload, starts[i], i + 1 < starts.size() ? starts[i + 1] : end);
            VersionedPayloadCursor in(raw);
            const auto key = in.readVarUInt();
            if (visual(key)) {
                const auto type = in.readVarUInt();
                const auto expectedType = key == 0 ? 7u : key == 38 ? 3u : key == 81 ? 0u : 4u;
                if (type != expectedType) throw std::runtime_error("No Render visual metadata type");
                if (raw.size() > 4096) throw std::runtime_error("No Render oversized visual metadata");
                actor.original[key] = std::move(raw);
            } else kept.push_back(std::move(raw));
        }
        actors_[id] = actor;
        if (!actorsHidden_) return packet;
        Bytes out = slice(packet.payload, 0, begin);
        var(out, kept.size() + VisualKeys.size());
        for (const auto& raw : kept) append(out, raw);
        for (auto key : VisualKeys) append(out, entry(actor, key, true));
        append(out, slice(packet.payload, end, packet.payload.size()));
        return replaced(packet, std::move(out));
    }

public:
    explicit NoRender(const std::string& version)
        : version_(version), codec_(VersionedPacketCodec::forVersion(version)), decoder_(version) {
        decoder_.setPreserveNbtBytes(true);
        auto metadata = generatedProtocolTypeJson(version, "packet_set_entity_data").value_or("");
        properties_ = metadata.find("\"properties\"") != std::string::npos;
    }
    void reset() {
        actors_.clear(); self_ = 0;
        decoder_ = ProtoDefPacketDecoder(version_);
        decoder_.setPreserveNbtBytes(true);
    }
    std::vector<VersionedGamePacket> configure(bool mapsHidden, bool actorsHidden) {
        (void) mapsHidden; // Maps belong exclusively to MapDeliveryQueue.
        std::vector<VersionedGamePacket> updates;
        if (actorsHidden_ != actorsHidden)
            for (const auto& [id, actor] : actors_) if (id != self_) updates.push_back(actorUpdate(id, actor, actorsHidden));
        actorsHidden_ = actorsHidden;
        return updates;
    }
    std::size_t failures() const { return failures_; }
    std::size_t capacitySkips() const { return capacitySkips_; }
    std::size_t cachedActors() const { return actors_.size(); }
    std::optional<VersionedGamePacket> process(const VersionedGamePacket& packet) {
        try {
            if (packet.name == "start_game") {
                reset();
                VersionedPayloadCursor in(packet.payload); in.readVarLong(); self_ = in.readVarULong();
                decoder_.updatePacketVariables(packet.name, packet.payload);
            } else if (packet.name == "change_dimension") {
                actors_.clear();
            } else if (packet.name == "item_registry") {
                decoder_.updatePacketVariables(packet.name, packet.payload);
            } else if (packet.name == "remove_entity") {
                VersionedPayloadCursor in(packet.payload); const auto id = in.readVarLong();
                std::erase_if(actors_, [id](const auto& pair) { return pair.second.uniqueId == id; });
            } else if (packet.name == "take_item_entity") {
                VersionedPayloadCursor in(packet.payload); actors_.erase(in.readVarULong());
            } else if (packet.name == "clientbound_map_item_data") return std::nullopt;
            else if (packet.name == "add_entity" || packet.name == "add_player" ||
                packet.name == "add_item_entity" || packet.name == "set_entity_data") return actorPacket(packet);
        } catch (...) { ++failures_; } // Actor-only parsing may fail open; maps never enter it.
        return packet;
    }
};
} // namespace bedrock
