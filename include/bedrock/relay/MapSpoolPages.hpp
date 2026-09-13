#pragma once
#include <bitset>
#include <algorithm>
#include <cstddef>
#include <optional>

namespace bedrock {
// Worker-owned allocator. Fixed 16 KiB bitmap, no file I/O or unbounded free
// list. Records use only the required 4 KiB pages, not a maximum-size slot.
class MapSpoolPages {
public:
    static constexpr size_t PageBytes = 4096, MaxBytes = 512 * 1024 * 1024;
    struct Block { size_t first = 0, count = 0; };
private:
    std::bitset<MaxBytes / PageBytes> occupied_;
    size_t limit_, used_ = 0;
public:
    explicit MapSpoolPages(size_t bytes) : limit_(std::min(bytes, MaxBytes) / PageBytes) {}
    std::optional<Block> allocate(size_t bytes) {
        if (!bytes || bytes > limit_ * PageBytes) return {};
        const auto count = (bytes + PageBytes - 1) / PageBytes;
        size_t run = 0;
        for (size_t i = 0; i < limit_; ++i) {
            run = occupied_[i] ? 0 : run + 1;
            if (run != count) continue;
            Block b{i + 1 - count, count};
            for (size_t j = b.first; j <= i; ++j) occupied_.set(j);
            used_ += count; return b;
        }
        return {};
    }
    void release(Block b) {
        for (size_t j = b.first; j < b.first + b.count; ++j) occupied_.reset(j);
        used_ -= b.count;
    }
    void reset() { occupied_.reset(); used_ = 0; }
    size_t reserved() const { return used_ * PageBytes; }
};
} // namespace bedrock
