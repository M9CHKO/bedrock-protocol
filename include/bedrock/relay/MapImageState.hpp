#pragma once
#include <bedrock/protocol/VersionedPacketCodec.hpp>
#include <bedrock/generated/GeneratedProtocolTypes.hpp>
#include <array>
#include <span>
#include <optional>

namespace bedrock {

// Bounded, allocation-free validation precedes every allocation/copy. This
// codec deals in plaintext map state, never encrypted batches.
class MapImageCodec {
public:
    using Bytes = std::vector<uint8_t>;
    static constexpr size_t MaxWireBytes = 192 * 1024;
    static constexpr size_t MaxTrackedBytes = 64 * 1024;
    struct State {
        int64_t id = 0;
        Bytes common, included, tracked;
        uint8_t scale = 0;
        bool isVoid = false;
        std::array<uint32_t, 16384> pixels {};
    };
    struct Cursor {
        std::span<const uint8_t> bytes;
        size_t pos = 0;
        uint8_t byte() { if (pos == bytes.size()) throw std::runtime_error("map truncated"); return bytes[pos++]; }
        uint64_t var(unsigned bits = 32) {
            uint64_t out = 0;
            for (unsigned shift = 0; shift < bits; shift += 7) {
                auto b = byte();
                if (bits - shift < 7 && (b & 127u) >= (1u << (bits - shift)))
                    throw std::runtime_error("map varint overflow");
                out |= uint64_t(b & 127) << shift;
                if (!(b & 128)) return out;
            }
            throw std::runtime_error("map varint too long");
        }
        int64_t zig(unsigned bits = 32) { auto v = var(bits); return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1); }
        void skip(size_t n) { if (n > bytes.size() - pos) throw std::runtime_error("map range"); pos += n; }
        uint32_t le32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(byte()) << (i * 8); return v; }
    };
    struct Patch {
        int64_t id = 0;
        uint32_t flags = 0;
        size_t commonBegin = 0, commonEnd = 0, includedBegin = 0, includedEnd = 0;
        size_t trackedBegin = 0, trackedEnd = 0, pixelsBegin = 0;
        uint8_t scale = 0;
        int w = 0, h = 0, x = 0, y = 0;
        bool full() const { return w == 128 && h == 128 && x == 0 && y == 0; }
    };
private:
    VersionedPacketCodec codec_;
    bool origin_, locked_;
public:
    explicit MapImageCodec(const std::string& version) : codec_(VersionedPacketCodec::forVersion(version)) {
        auto schema = generatedProtocolTypeJson(version, "packet_clientbound_map_item_data").value_or("");
        if (schema.empty()) throw std::runtime_error("map schema unavailable");
        origin_ = schema.find("\"origin\"") != std::string::npos;
        locked_ = schema.find("\"locked\"") != std::string::npos;
    }
    static void var(Bytes& out, uint64_t v) {
        do { auto b = static_cast<uint8_t>(v & 127); v >>= 7; out.push_back(b | (v ? 128 : 0)); } while (v);
    }
    static void zig(Bytes& out, int64_t v) { var(out, (uint64_t(v) << 1) ^ uint64_t(-(v < 0))); }
    static void append(Bytes& out, const Bytes& v) { out.insert(out.end(), v.begin(), v.end()); }
    static std::optional<int64_t> id(std::span<const uint8_t> raw) noexcept {
        try { return Cursor{raw}.zig(64); } catch (...) { return {}; }
    }
    Patch validate(std::span<const uint8_t> raw) const {
        if (raw.size() > MaxWireBytes) throw std::runtime_error("map wire limit");
        Cursor c{raw}; Patch p;
        p.id = c.zig(64); p.flags = static_cast<uint32_t>(c.var());
        if (p.flags & ~15u) throw std::runtime_error("map flags");
        p.commonBegin = c.pos; c.byte();
        if (locked_ && c.byte() > 1) throw std::runtime_error("map bool");
        if (origin_) { c.zig(); c.zig(); c.zig(); }
        p.commonEnd = c.pos;
        if (p.flags & 8) {
            p.includedBegin = c.pos;
            auto n = c.var(); if (n > 4096) throw std::runtime_error("map inclusion limit");
            while (n--) c.zig(64);
            p.includedEnd = c.pos;
        }
        if (p.flags & 14) p.scale = c.byte();
        if (p.flags & 4) {
            p.trackedBegin = c.pos;
            auto n = c.var(); if (n > 4096) throw std::runtime_error("map object limit");
            while (n--) {
                auto type = c.le32();
                if (type == 0) c.zig(64);
                else if (type == 1) { c.zig(); c.var(); c.zig(); }
                else throw std::runtime_error("map object type");
                if (c.pos - p.trackedBegin > MaxTrackedBytes) throw std::runtime_error("map object bytes");
            }
            n = c.var(); if (n > 4096) throw std::runtime_error("map decoration limit");
            while (n--) {
                c.skip(4); auto length = c.var();
                if (length > MaxTrackedBytes) throw std::runtime_error("map label limit");
                c.skip(static_cast<size_t>(length)); c.var();
                if (c.pos - p.trackedBegin > MaxTrackedBytes) throw std::runtime_error("map decoration bytes");
            }
            p.trackedEnd = c.pos;
            if (p.trackedEnd - p.trackedBegin > MaxTrackedBytes) throw std::runtime_error("map tracked bytes");
        }
        if (p.flags & 2) {
            p.w = static_cast<int>(c.zig()); p.h = static_cast<int>(c.zig());
            p.x = static_cast<int>(c.zig()); p.y = static_cast<int>(c.zig());
            auto n = c.var();
            if (p.w < 0 || p.h < 0 || p.w > 128 || p.h > 128 || p.x < 0 || p.y < 0 ||
                p.x > 128 - p.w || p.y > 128 - p.h || n != uint64_t(p.w * p.h))
                throw std::runtime_error("map rectangle");
            p.pixelsBegin = c.pos;
            while (n--) c.var();
        }
        if (c.pos != raw.size()) throw std::runtime_error("map trailing data");
        return p;
    }
    static void merge(State& state, const Patch& p, const Bytes& raw) {
        auto copy = [&](size_t a, size_t b) { return Bytes(raw.begin() + a, raw.begin() + b); };
        state.id = p.id; state.isVoid = (p.flags & 1) != 0;
        state.common = copy(p.commonBegin, p.commonEnd);
        if (p.flags & 8) state.included = copy(p.includedBegin, p.includedEnd);
        if (p.flags & 14) state.scale = p.scale;
        if (p.flags & 4) state.tracked = copy(p.trackedBegin, p.trackedEnd);
        Cursor c{raw, p.pixelsBegin};
        for (int row = 0; row < p.h; ++row)
            for (int col = 0; col < p.w; ++col)
                state.pixels[(p.y + row) * 128 + p.x + col] = static_cast<uint32_t>(c.var());
    }
    VersionedGamePacket packet(const State& state, bool hidden) const {
        Bytes out; out.reserve(MaxWireBytes);
        zig(out, state.id); var(out, 2 | 4 | (state.isVoid ? 1 : 0) | (state.included.empty() ? 0 : 8));
        append(out, state.common); append(out, state.included); out.push_back(state.scale);
        if (hidden || state.tracked.empty()) { var(out, 0); var(out, 0); }
        else append(out, state.tracked);
        zig(out, 128); zig(out, 128); zig(out, 0); zig(out, 0); var(out, 16384);
        for (auto pixel : state.pixels) var(out, hidden ? 0 : pixel);
        if (out.size() > MaxWireBytes) throw std::runtime_error("rebuilt map limit");
        return codec_.makePacketByName("clientbound_map_item_data", out);
    }
};
} // namespace bedrock
