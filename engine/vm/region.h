#pragma once
// GARDVM Tier 3A — Region Allocator
// Bump-pointer allocation with O(1) bulk deallocation.
// Used for function-scoped temporaries that don't escape.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <sys/mman.h>

namespace gardvm {

// ============================================================================
// Page — Single mmap'd memory block
// ============================================================================

struct Page {
    Page*    next;
    uint8_t* base;
    uint8_t* cursor;
    uint8_t* limit;
    size_t   mappedSize;

    size_t used() const { return (size_t)(cursor - base); }
    size_t remaining() const { return (size_t)(limit - cursor); }
};

// ============================================================================
// Region — Bump-pointer allocator
// ============================================================================

class Region {
public:
    static constexpr size_t PAGE_SIZE = 64 * 1024; // 64KB
    static constexpr size_t ALIGN = 8;

    Region() : head_(nullptr), totalAllocated_(0), pageCount_(0) {
        head_ = allocPage(PAGE_SIZE);
    }

    ~Region() { freeAll(); }

    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;

    Region(Region&& other) noexcept
        : head_(other.head_), totalAllocated_(other.totalAllocated_),
          pageCount_(other.pageCount_) {
        other.head_ = nullptr;
        other.totalAllocated_ = 0;
        other.pageCount_ = 0;
    }

    // Allocate n bytes (8-byte aligned). O(1) fast path.
    void* alloc(size_t n) {
        n = (n + ALIGN - 1) & ~(ALIGN - 1);
        if (__builtin_expect(head_ && head_->cursor + n <= head_->limit, 1)) {
            void* ptr = head_->cursor;
            head_->cursor += n;
            totalAllocated_ += n;
            return ptr;
        }
        return allocSlow(n);
    }

    // Typed allocation with placement new
    template<typename T, typename... Args>
    T* create(Args&&... args) {
        void* mem = alloc(sizeof(T));
        return new (mem) T(static_cast<Args&&>(args)...);
    }

    // Allocate zeroed memory
    void* allocZeroed(size_t n) {
        void* ptr = alloc(n);
        if (ptr) std::memset(ptr, 0, n);
        return ptr;
    }

    // Mark/Reset for scoped deallocation
    struct Mark {
        Page*    page;
        uint8_t* cursor;
        size_t   totalAllocated;
    };

    Mark mark() const {
        return { head_, head_ ? head_->cursor : nullptr, totalAllocated_ };
    }

    void resetToMark(const Mark& m) {
        // Free pages allocated after the mark
        while (head_ != m.page) {
            Page* next = head_->next;
            freePage(head_);
            head_ = next;
            pageCount_--;
        }
        if (head_) head_->cursor = m.cursor;
        totalAllocated_ = m.totalAllocated;
    }

    // Free ALL pages
    void freeAll() {
        Page* p = head_;
        while (p) {
            Page* next = p->next;
            freePage(p);
            p = next;
        }
        head_ = nullptr;
        totalAllocated_ = 0;
        pageCount_ = 0;
    }

    // Reset without freeing (reuse first page)
    void reset() {
        if (!head_) return;
        Page* p = head_->next;
        while (p) {
            Page* next = p->next;
            freePage(p);
            p = next;
            pageCount_--;
        }
        head_->next = nullptr;
        head_->cursor = head_->base;
        totalAllocated_ = 0;
    }

    size_t totalAllocated() const { return totalAllocated_; }
    size_t pageCount() const { return pageCount_; }

private:
    Page* allocPage(size_t minSize) {
        size_t mapSize = sizeof(Page) + minSize;
        mapSize = (mapSize + 4095) & ~4095; // page-align

        void* mem = mmap(nullptr, mapSize, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return nullptr;

        Page* page = (Page*)mem;
        page->next = nullptr;
        page->base = (uint8_t*)mem + sizeof(Page);
        page->cursor = page->base;
        page->limit = (uint8_t*)mem + mapSize;
        page->mappedSize = mapSize;
        pageCount_++;
        return page;
    }

    void freePage(Page* page) {
        if (page) munmap(page, page->mappedSize);
    }

    void* allocSlow(size_t n) {
        size_t needed = n > PAGE_SIZE ? n : PAGE_SIZE;
        Page* newPage = allocPage(needed);
        if (!newPage) return nullptr;
        newPage->next = head_;
        head_ = newPage;
        void* ptr = head_->cursor;
        head_->cursor += n;
        totalAllocated_ += n;
        return ptr;
    }

    Page*  head_;
    size_t totalAllocated_;
    size_t pageCount_;
};

// Thread-local region
inline Region& threadRegion() {
    thread_local Region region;
    return region;
}

// RAII scope guard
struct RegionScope {
    Region& region;
    Region::Mark saved;
    RegionScope(Region& r) : region(r), saved(r.mark()) {}
    ~RegionScope() { region.resetToMark(saved); }
    RegionScope(const RegionScope&) = delete;
    RegionScope& operator=(const RegionScope&) = delete;
};

} // namespace gardvm
