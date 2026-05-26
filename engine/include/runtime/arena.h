#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>

namespace gard {
namespace runtime {

// Arena allocator: bump-pointer allocation for short-lived objects.
// Allocations are O(1) — just increment a pointer.
// Deallocation is bulk — reset the entire arena at once.
//
// Usage pattern:
//   - Function entry: mark the arena position
//   - Function body: allocate temporaries from arena
//   - Function exit: reset to the marked position (frees all temporaries)
//
// This eliminates malloc/free overhead for objects that don't escape
// the current function scope (the common case in loops).

class Arena {
public:
    static constexpr size_t DEFAULT_BLOCK_SIZE = 64 * 1024; // 64KB blocks

    Arena(size_t blockSize = DEFAULT_BLOCK_SIZE)
        : blockSize_(blockSize), offset_(0) {
        blocks_.push_back(std::make_unique<uint8_t[]>(blockSize));
        current_ = blocks_.back().get();
    }

    // Allocate n bytes (aligned to 8 bytes)
    void* alloc(size_t n) {
        n = (n + 7) & ~7; // align to 8 bytes
        if (offset_ + n > blockSize_) {
            // Current block full — allocate a new one
            size_t newSize = n > blockSize_ ? n : blockSize_;
            blocks_.push_back(std::make_unique<uint8_t[]>(newSize));
            current_ = blocks_.back().get();
            offset_ = 0;
        }
        void* ptr = current_ + offset_;
        offset_ += n;
        totalAllocated_ += n;
        return ptr;
    }

    // Typed allocation
    template<typename T, typename... Args>
    T* create(Args&&... args) {
        void* mem = alloc(sizeof(T));
        return new (mem) T(std::forward<Args>(args)...);
    }

    // Save current position (for scoped reset)
    struct Mark {
        size_t blockIndex;
        size_t offset;
    };

    Mark mark() const {
        return {blocks_.size() - 1, offset_};
    }

    // Reset to a previous mark (frees everything allocated after the mark)
    void resetToMark(const Mark& m) {
        // Free blocks allocated after the mark
        while (blocks_.size() > m.blockIndex + 1) {
            blocks_.pop_back();
        }
        current_ = blocks_.back().get();
        offset_ = m.offset;
    }

    // Reset entire arena (frees everything, keeps first block)
    void reset() {
        while (blocks_.size() > 1) blocks_.pop_back();
        current_ = blocks_[0].get();
        offset_ = 0;
        totalAllocated_ = 0;
    }

    size_t totalAllocated() const { return totalAllocated_; }
    size_t blockCount() const { return blocks_.size(); }

private:
    size_t blockSize_;
    size_t offset_;
    uint8_t* current_;
    size_t totalAllocated_ = 0;
    std::vector<std::unique_ptr<uint8_t[]>> blocks_;
};

// Thread-local arena for the VM (avoids lock contention)
Arena& getThreadArena();

} // namespace runtime
} // namespace gard
