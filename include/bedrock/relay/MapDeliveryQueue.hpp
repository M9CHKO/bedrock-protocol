#pragma once
#include <bedrock/relay/MapImageState.hpp>
#include <bedrock/relay/MapSpoolPages.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <fcntl.h>
#if defined(_WIN32) && !defined(__CYGWIN__)
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace bedrock {

// Both limits apply in every rolling second, with no accumulated catch-up
// credit. Charge complete plaintext packet + batch length prefix, not zlib size.
class MapSendBudget {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr size_t PacketsPerSecond = 8, BytesPerSecond = 512 * 1024;
    static constexpr auto MinimumGap = std::chrono::milliseconds(125);
private:
    std::deque<std::pair<Time, size_t>> sent_;
    Time next_ {};
public:
    bool allows(Time now, size_t bytes) {
        while (!sent_.empty() && now - sent_.front().first >= std::chrono::seconds(1)) sent_.pop_front();
        size_t total = bytes;
        for (auto& item : sent_) total += item.second;
        return now >= next_ && sent_.size() < PacketsPerSecond && total <= BytesPerSecond;
    }
    void charge(Time now, size_t bytes) {
        sent_.emplace_back(now, bytes);
        next_ = now + MinimumGap;
    }
};

struct MapDeliveryOptions {
    std::filesystem::path spoolDirectory;
    size_t memoryBytes = 8 * 1024 * 1024;
    size_t diskBytes = 512 * 1024 * 1024;
};

class MapDeliveryQueue {
public:
    // Includes a conservative arena allowance for the fixed-size index,
    // worker's one active image/record, one prepared packet and serialization
    // temporaries. The bounded intake uses only the remainder of the budget.
    // Two additional MiB cover the larger bounded request/index node sets,
    // request payloads and one active request's serialization/cipher temporaries.
    static constexpr size_t WorkingReserve = 5 * 1024 * 1024;
    static constexpr size_t MaxMaps = 4000;
    struct Stats {
        size_t memoryBudget = 0, memoryReserved = 0, queuedBytes = 0, queuedPackets = 0;
        size_t diskBudget = 0, diskReserved = 0, maps = 0, pending = 0, reloadNeeded = 0;
        uint64_t intercepted = 0, malformed = 0, overflow = 0, ioErrors = 0, sent = 0;
        bool reloadAll = false, workerBusy = false, prepared = false;
    };
private:
    using Bytes = MapImageCodec::Bytes;
    struct Entry {
        MapSpoolPages::Block block;
        size_t queued = 0;
        uint64_t revision = 0, order = 0;
        bool stored = false, dirty = false, reload = false;
    };
    struct Job { Bytes raw; MapImageCodec::Patch patch; uint64_t revision = 0; };
    struct Ready { VersionedGamePacket packet; int64_t id; uint64_t generation, revision; };
    MapDeliveryOptions options_;
    MapImageCodec codec_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<int64_t, Entry> entries_;
    std::deque<Job> jobs_;
    std::optional<Ready> ready_;
    MapSendBudget budget_;
    size_t queuedBytes_ = 0, diskReserved_ = 0, maxIndex_ = 0;
    uint64_t generation_ = 1, order_ = 0;
    bool hidden_ = false, stopping_ = false, busy_ = false, sending_ = false;
    Stats counters_;
    std::thread worker_;

    void markReload(std::optional<int64_t> id) {
        if (!id) { counters_.reloadAll = true; return; }
        auto it = entries_.find(*id);
        if (it == entries_.end()) {
            if (entries_.size() >= maxIndex_) { counters_.reloadAll = true; return; }
            it = entries_.emplace(*id, Entry{}).first;
        }
        it->second.reload = true;
        ++it->second.revision; // invalidate any older prepared image
        it->second.dirty = false;
    }
    static std::FILE* openSpool(const std::filesystem::path& directory) {
        // Exclusive, private, delete-on-close storage. POSIX unlink immediately
        // also guarantees cleanup if Android kills the process without teardown.
        if (directory.empty()) return std::tmpfile();
        std::filesystem::create_directories(directory);
#if defined(_WIN32) && !defined(__CYGWIN__)
        for (unsigned n = 0; n < 64; ++n) {
            auto path = directory / ("map-spool-" + std::to_string(_getpid()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(n));
            int fd = _wopen(path.c_str(), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY | _O_TEMPORARY,
                _S_IREAD | _S_IWRITE);
            if (fd >= 0) { auto* f = _fdopen(fd, "w+b"); if (!f) _close(fd); return f; }
        }
        return nullptr;
#else
        auto name = (directory / "map-spool-XXXXXX").string();
        std::vector<char> path(name.begin(), name.end()); path.push_back(0);
        int fd = mkstemp(path.data());
        if (fd < 0) return nullptr;
        if (unlink(path.data()) != 0) { close(fd); return nullptr; }
        auto* file = fdopen(fd, "w+b");
        if (!file) close(fd);
        return file;
#endif
    }
    static void seek(std::FILE* f, MapSpoolPages::Block block) {
        // Disk cap is <=512 MiB, safely representable even with 32-bit long.
        if (std::fseek(f, static_cast<long>(block.first * MapSpoolPages::PageBytes), SEEK_SET)) throw std::runtime_error("map spool seek");
    }
    MapImageCodec::State read(std::FILE* f, MapSpoolPages::Block block) {
        seek(f, block); uint32_t length = 0;
        if (!block.count || std::fread(&length, sizeof(length), 1, f) != 1 || length > MapImageCodec::MaxWireBytes ||
            length > block.count * MapSpoolPages::PageBytes - sizeof(length))
            throw std::runtime_error("map spool header");
        Bytes raw(length);
        if (std::fread(raw.data(), 1, length, f) != length) throw std::runtime_error("map spool read");
        auto patch = codec_.validate(raw); MapImageCodec::State state;
        MapImageCodec::merge(state, patch, raw); return state;
    }
    bool write(std::FILE* f, MapSpoolPages& pages, Entry& entry, const MapImageCodec::State& state) {
        auto rebuilt = codec_.packet(state, false);
        auto length = static_cast<uint32_t>(rebuilt.payload.size());
        auto block = entry.block;
        const bool grow = length + sizeof(length) > block.count * MapSpoolPages::PageBytes;
        if (grow) {
            auto allocated = pages.allocate(length + sizeof(length));
            if (!allocated) return false;
            block = *allocated;
        }
        try {
            seek(f, block);
            if (std::fwrite(&length, sizeof(length), 1, f) != 1 ||
                std::fwrite(rebuilt.payload.data(), 1, length, f) != length || std::fflush(f))
                throw std::runtime_error("map spool write");
        } catch (...) { if (grow) pages.release(block); throw; }
        if (grow) { pages.release(entry.block); entry.block = block; }
        return true;
    }
    auto candidate() {
        auto selected = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            const auto& e = it->second;
            if (e.dirty && e.stored && !e.queued && !e.reload &&
                (selected == entries_.end() || e.order < selected->second.order)) selected = it;
        }
        return selected;
    }
    void run() noexcept {
        std::unique_ptr<std::FILE, decltype(&std::fclose)> file(nullptr, &std::fclose);
        MapSpoolPages pages(options_.diskBytes);
        uint64_t fileGeneration = 0;
        unsigned inputBurst = 0;
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            cv_.wait(lock, [&] { return stopping_ || generation_ != fileGeneration ||
                !jobs_.empty() || (!ready_ && candidate() != entries_.end()); });
            if (stopping_) break;
            if (generation_ != fileGeneration) {
                fileGeneration = generation_;
                lock.unlock(); file.reset(); pages.reset(); lock.lock();
                continue;
            }
            // Disk intake must not starve delivery when new maps keep arriving.
            bool input = !jobs_.empty() && (ready_ || inputBurst < 4 || candidate() == entries_.end());
            inputBurst = input ? inputBurst + 1 : 0;
            Job job; int64_t id; Entry entry; uint64_t gen = generation_; bool hidden = hidden_;
            if (input) {
                job = std::move(jobs_.front()); jobs_.pop_front(); queuedBytes_ -= job.raw.size();
                id = job.patch.id; entry = entries_.at(id);
            } else {
                auto it = candidate(); if (ready_ || it == entries_.end()) continue;
                id = it->first; entry = it->second;
            }
            busy_ = true;
            lock.unlock();
            std::optional<VersionedGamePacket> prepared;
            bool ok = false, fullDisk = false;
            try {
                if (!file) file.reset(openSpool(options_.spoolDirectory));
                if (!file) throw std::runtime_error("map spool unavailable");
                auto state = entry.stored ? read(file.get(), entry.block) : MapImageCodec::State{};
                if (input) {
                    MapImageCodec::merge(state, job.patch, job.raw);
                    fullDisk = !write(file.get(), pages, entry, state);
                }
                else prepared = codec_.packet(state, hidden);
                ok = !fullDisk;
            } catch (...) { /* No fallback to the input payload, ever. */ }
            lock.lock(); busy_ = false;
            if (gen != generation_) continue;
            diskReserved_ = pages.reserved();
            auto& current = entries_.at(id);
            if (input) --current.queued;
            if (!ok) {
                if (fullDisk) ++counters_.overflow; else ++counters_.ioErrors;
                markReload(id);
                // A partial write invalidates the old record too. A future full
                // refresh can recover it without reading a damaged record.
                if (!fullDisk) current.stored = false;
                if (file) std::clearerr(file.get());
            } else if (input) {
                current.block = entry.block;
                current.stored = true;
                if (job.patch.full() && (job.patch.flags & 14) == 14 && current.revision == job.revision)
                    current.reload = false;
                if (!current.reload) { current.dirty = true; if (!current.order) current.order = ++order_; }
            } else if (current.revision == entry.revision && !current.reload) {
                ready_ = Ready{std::move(*prepared), id, gen, entry.revision};
            }
        }
        lock.unlock(); // file destruction/IO never under the network admission lock
    }
public:
    static bool isMapPacket(const VersionedGamePacket& packet) noexcept {
        if (packet.name == "clientbound_map_item_data" || (packet.packetId & 1023u) == 67u) return true;
        try { return (MapImageCodec::Cursor{packet.fullPacket}.var() & 1023u) == 67u; }
        catch (...) { return false; }
    }
    explicit MapDeliveryQueue(const std::string& version, MapDeliveryOptions options = {})
        : options_(std::move(options)), codec_(version) {
        options_.memoryBytes = std::clamp(options_.memoryBytes, WorkingReserve, size_t(8 * 1024 * 1024));
        options_.diskBytes = std::min(options_.diskBytes, size_t(512 * 1024 * 1024));
        maxIndex_ = std::min(MaxMaps, options_.diskBytes / MapSpoolPages::PageBytes + 256);
        worker_ = std::thread([this] { run(); });
    }
    ~MapDeliveryQueue() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        cv_.notify_all(); if (worker_.joinable()) worker_.join();
    }
    // True means consumed, not necessarily stored. In particular invalid maps,
    // storage errors and overflow can NEVER escape to normal client forwarding.
    bool intercept(const VersionedGamePacket& packet) noexcept {
        if (!isMapPacket(packet)) return false;
        try {
            std::lock_guard lock(mutex_); ++counters_.intercepted;
            MapImageCodec::Patch patch;
            try { patch = codec_.validate(packet.payload); }
            catch (...) { ++counters_.malformed; markReload(MapImageCodec::id(packet.payload)); return true; }
            auto it = entries_.find(patch.id);
            if (it == entries_.end() && entries_.size() >= maxIndex_) {
                ++counters_.overflow; markReload(patch.id); return true;
            }
            if (it == entries_.end()) it = entries_.emplace(patch.id, Entry{}).first;
            auto& entry = it->second;
            // Only a complete image+metadata snapshot can replace pending
            // patches without losing earlier decorations/included map IDs.
            if (patch.full() && (patch.flags & 14) == 14) {
                for (auto j = jobs_.begin(); j != jobs_.end();) {
                    if (j->patch.id == patch.id) { queuedBytes_ -= j->raw.size(); --entry.queued; j = jobs_.erase(j); }
                    else ++j;
                }
            }
            if (jobs_.size() >= 128 || packet.payload.size() > options_.memoryBytes - WorkingReserve - queuedBytes_) {
                ++counters_.overflow; markReload(patch.id); return true;
            }
            // Copy only the bounded plaintext payload, never full incoming batch.
            jobs_.push_back({packet.payload, patch, entry.revision + 1});
            queuedBytes_ += packet.payload.size(); ++entry.queued; ++entry.revision;
            if (ready_ && ready_->id == patch.id) ready_.reset();
            cv_.notify_one(); return true;
        } catch (...) {
            // Even an allocation failure has a bounded reload marker.
            try { std::lock_guard lock(mutex_); ++counters_.overflow; counters_.reloadAll = true; } catch (...) {}
            return true;
        }
    }
    void configureHidden(bool hidden) {
        std::lock_guard lock(mutex_);
        if (hidden_ == hidden) return;
        hidden_ = hidden; ready_.reset();
        for (auto& [id, e] : entries_) { ++e.revision; e.dirty = e.stored; e.order = ++order_; }
        cv_.notify_one();
    }
    void reset() {
        std::lock_guard lock(mutex_); ++generation_; entries_.clear(); jobs_.clear(); ready_.reset();
        queuedBytes_ = diskReserved_ = 0; counters_.reloadAll = false;
        // Preserve the rate history across dimension resets: no new burst credit.
        cv_.notify_one();
    }
    // Called only by the downstream scheduler. Sender must synchronously test
    // backpressure and send through the ordinary per-client encryption lock.
    // False means nothing was encrypted or queued; the prepared map is retained.
    bool pump(const std::function<bool(const VersionedGamePacket&)>& sender,
              MapSendBudget::Time now = MapSendBudget::Clock::now()) {
        std::unique_lock lock(mutex_);
        if (!ready_ || sending_) return false;
        auto& e = entries_.at(ready_->id);
        if (ready_->generation != generation_ || ready_->revision != e.revision || e.reload) {
            ready_.reset(); cv_.notify_one(); return false;
        }
        const auto bytes = ready_->packet.fullPacket.size() + 5;
        if (!budget_.allows(now, bytes)) return false;
        auto sending = std::move(*ready_); ready_.reset();
        e.dirty = false; sending_ = true;
        lock.unlock();
        bool accepted;
        try { accepted = sender(sending.packet); }
        catch (...) {
            lock.lock(); sending_ = false;
            budget_.charge(std::max(now, MapSendBudget::Clock::now()), bytes);
            if (sending.generation == generation_) markReload(sending.id);
            cv_.notify_one(); throw;
        }
        // Transport callbacks may synchronously reset/disconnect this session.
        // Never hold the queue mutex during callbacks, compression or encryption.
        lock.lock(); sending_ = false;
        if (accepted) { budget_.charge(std::max(now, MapSendBudget::Clock::now()), bytes); ++counters_.sent; }
        if (sending.generation == generation_) {
            auto& current = entries_.at(sending.id);
            if (current.revision == sending.revision && !current.reload) {
                if (accepted) current.order = 0;
                else {
                    current.dirty = true;
                    if (!ready_) ready_ = std::move(sending);
                }
            }
        }
        cv_.notify_one(); return accepted;
    }
    Stats stats() const {
        std::lock_guard lock(mutex_); auto s = counters_;
        s.memoryBudget = options_.memoryBytes; s.memoryReserved = WorkingReserve + queuedBytes_;
        s.queuedBytes = queuedBytes_; s.queuedPackets = jobs_.size();
        s.diskBudget = options_.diskBytes; s.diskReserved = diskReserved_; s.maps = entries_.size();
        s.workerBusy = busy_; s.prepared = ready_.has_value();
        for (auto& [id, e] : entries_) { s.pending += e.dirty || e.queued; s.reloadNeeded += e.reload; }
        return s;
    }
};
} // namespace bedrock
