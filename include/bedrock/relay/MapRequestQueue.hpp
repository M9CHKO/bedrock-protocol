#pragma once
#include <bedrock/relay/MapDeliveryQueue.hpp>

namespace bedrock {

// Demand-side pacing complements MapDeliveryQueue: a small downstream image
// queue alone cannot prevent thousands of simultaneous requests to the backend.
class MapRequestQueue {
public:
    using Clock = MapSendBudget::Clock;
    using Time = Clock::time_point;
    static constexpr size_t MaxBytes = 256 * 1024, MaxEntries = 4000;
    static constexpr size_t MaxPayload = 10 + 4 + 16384 * 6;
    struct Stats {
        uint64_t intercepted = 0, sent = 0, duplicate = 0, malformed = 0, overflow = 0;
        size_t pending = 0, inFlight = 0, bytes = 0;
    };
private:
    struct Request { int64_t id; uint32_t header; std::vector<uint8_t> payload; };
    struct Flight { Request request; uint64_t ticket; Time expires; bool awaiting = true; };
    VersionedPacketCodec codec_;
    bool clientPixels_;
    mutable std::mutex mutex_;
    std::deque<Request> pending_;
    std::deque<Flight> flights_;
    MapSendBudget budget_;
    Stats counts_;
    size_t bytes_ = 0;
    uint64_t generation_ = 0, ticket_ = 0;
    bool sending_ = false;
    void expire(Time now) {
        for (auto it = flights_.begin(); it != flights_.end();) {
            if (now >= it->expires) { bytes_ -= it->request.payload.size(); it = flights_.erase(it); }
            else ++it;
        }
    }
public:
    explicit MapRequestQueue(const std::string& version) : codec_(VersionedPacketCodec::forVersion(version)),
        clientPixels_(generatedProtocolTypeJson(version, "packet_map_info_request").value_or("").find("client_pixels") != std::string::npos) {}
    static bool isRequest(const VersionedGamePacket& p) noexcept {
        if (p.name == "map_info_request" || (p.packetId & 1023u) == 68u) return true;
        try { return (MapImageCodec::Cursor{p.fullPacket}.var() & 1023u) == 68u; }
        catch (...) { return false; }
    }
    bool intercept(const VersionedGamePacket& p, Time now = Clock::now()) noexcept {
        if (!isRequest(p)) return false;
        try {
            std::lock_guard lock(mutex_); ++counts_.intercepted; expire(now);
            int64_t id; uint32_t header = 68;
            try {
                if (p.payload.size() > MaxPayload) throw std::runtime_error("map request size");
                MapImageCodec::Cursor c{p.payload}; id = c.zig(64);
                if (clientPixels_) {
                    auto count = c.le32();
                    if (count > 16384 || p.payload.size() - c.pos != size_t(count) * 6)
                        throw std::runtime_error("map request pixels");
                    while (count--) {
                        c.le32(); auto index = unsigned(c.byte()); index |= unsigned(c.byte()) << 8;
                        if (index >= 16384) throw std::runtime_error("map request index");
                    }
                }
                if (c.pos != p.payload.size()) throw std::runtime_error("map request trailing data");
                if (!p.fullPacket.empty()) {
                    MapImageCodec::Cursor wire{p.fullPacket}; header = static_cast<uint32_t>(wire.var());
                    if ((header & 1023u) != 68u || wire.bytes.size() - wire.pos != p.payload.size() ||
                        !std::equal(p.payload.begin(), p.payload.end(), wire.bytes.begin() + wire.pos))
                        throw std::runtime_error("map request wire mismatch");
                }
            } catch (...) { ++counts_.malformed; return true; }
            const auto same = [&](const Request& r) { return r.id == id && r.header == header && r.payload == p.payload; };
            // Only byte-identical requests coalesce. Distinct client_pixels
            // updates retain FIFO order; replacing them could lose map edits.
            for (const auto& r : pending_) if (same(r)) { ++counts_.duplicate; return true; }
            for (const auto& f : flights_) if (same(f.request)) { ++counts_.duplicate; return true; }
            if (pending_.size() + flights_.size() >= MaxEntries || p.payload.size() > MaxBytes - bytes_) {
                ++counts_.overflow; return true;
            }
            pending_.push_back({id, header, p.payload}); bytes_ += p.payload.size();
        } catch (...) { try { std::lock_guard lock(mutex_); ++counts_.overflow; } catch (...) {} }
        return true; // No error/overflow path forwards requests directly.
    }
    void response(int64_t id) {
        std::lock_guard lock(mutex_);
        for (auto& f : flights_) if (f.request.id == id) f.awaiting = false;
    }
    bool pump(const std::function<bool(const VersionedGamePacket&)>& sender, bool paused,
              size_t pendingImages, Time now = Clock::now()) {
        std::unique_lock lock(mutex_); expire(now);
        if (paused || pendingImages >= 4 || sending_ || pending_.empty()) return false;
        size_t outstanding = 0;
        for (const auto& f : flights_) outstanding += f.awaiting;
        if (outstanding >= 2) return false;
        auto next = std::find_if(pending_.begin(), pending_.end(), [&](const Request& r) {
            return std::none_of(flights_.begin(), flights_.end(), [&](const Flight& f) { return f.awaiting && f.request.id == r.id; });
        });
        if (next == pending_.end() || !budget_.allows(now, next->payload.size() + 10)) return false;
        auto packet = codec_.makePacketByName("map_info_request", next->payload);
        packet.packetId = next->header;
        packet.fullPacket = codec_.encodeFullPacketById(next->header, next->payload);
        const auto gen = generation_, ticket = ++ticket_;
        flights_.push_back({std::move(*next), ticket, now + std::chrono::seconds(5)});
        pending_.erase(next); sending_ = true;
        lock.unlock();
        bool accepted = false;
        try { accepted = sender(packet); }
        catch (...) {
            lock.lock(); sending_ = false;
            // An exception can occur after encryption/submission: do not retry
            // ambiguously sent bytes. Normal teardown owns the error.
            budget_.charge(std::max(now, Clock::now()), packet.fullPacket.size() + 5);
            throw;
        }
        lock.lock(); sending_ = false;
        if (accepted) { ++counts_.sent; budget_.charge(std::max(now, Clock::now()), packet.fullPacket.size() + 5); }
        if (!accepted && gen == generation_) {
            auto it = std::find_if(flights_.begin(), flights_.end(), [&](const Flight& f) { return f.ticket == ticket; });
            if (it != flights_.end()) { pending_.push_front(std::move(it->request)); flights_.erase(it); }
        }
        return accepted;
    }
    void reset() {
        std::lock_guard lock(mutex_); ++generation_; pending_.clear(); flights_.clear(); bytes_ = 0;
        // Neither dimension changes nor reconnect-in-place grant burst credit.
    }
    Stats stats() const {
        std::lock_guard lock(mutex_); auto s = counts_; s.pending = pending_.size(); s.bytes = bytes_;
        for (const auto& f : flights_) s.inFlight += f.awaiting;
        return s;
    }
};
} // namespace bedrock
