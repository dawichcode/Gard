#pragma once
// GARDVM Tier 3B-3E — Generational Garbage Collector
// Nursery (copying) + OldGen (incremental mark-sweep) + Write Barrier + Large Object Space

#include "value.h"
#include "object.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <sys/mman.h>

namespace gardvm {

// ============================================================================
// Nursery — Young Generation (Semi-Space Copying Collector)
// ============================================================================

class Nursery {
public:
    static constexpr size_t DEFAULT_SIZE = 1 * 1024 * 1024; // 1MB
    static constexpr uint8_t PROMOTE_AGE = 3;

    explicit Nursery(size_t size = DEFAULT_SIZE) : size_(size) {
        base_ = (uint8_t*)std::calloc(1, size);
        fromSpace_ = base_;
        toSpace_ = nullptr;
        allocPtr_ = fromSpace_;
        limit_ = base_ ? base_ + size_ : nullptr;
    }

    ~Nursery() {
        if (base_) std::free(base_);
    }

    // Bump-pointer allocation
    void* alloc(size_t n) {
        n = (n + 7) & ~7;
        if (allocPtr_ + n <= limit_) {
            void* ptr = allocPtr_;
            allocPtr_ += n;
            bytesAllocated_ += n;
            return ptr;
        }
        return nullptr; // full — caller triggers collection
    }

    bool contains(const void* ptr) const {
        return (const uint8_t*)ptr >= fromSpace_ && (const uint8_t*)ptr < limit_;
    }

    bool isFull() const { return allocPtr_ >= limit_; }
    size_t used() const { return (size_t)(allocPtr_ - fromSpace_); }
    size_t capacity() const { return size_; }
    uint64_t collections() const { return collections_; }
    uint64_t totalBytesAllocated() const { return bytesAllocated_; }

    // ========================================================================
    // Collection — Cheney's Copying Algorithm
    // ========================================================================

    struct Stats {
        size_t liveBytes;
        size_t promotedBytes;
        uint64_t pauseNs;
    };

    using RootCallback = void(*)(void* ctx, GValue* slot);
    using PromoteCallback = void*(*)(void* ctx, void* obj, size_t size);

    Stats collect(RootCallback visitRoots, void* rootCtx,
                  PromoteCallback promote, void* promoteCtx,
                  GValue* rememberedSlots, size_t rememberedCount) {
        Stats stats = {};
        uint64_t startNs = clockNs();

        // Set up copy destination
        uint8_t* scanPtr = toSpace_;
        uint8_t* freePtr = toSpace_;
        copyDst_ = &freePtr;
        promoteFn_ = promote;
        promoteCtx_ = promoteCtx;
        promotedBytes_ = 0;

        // Visit roots — each root that points into nursery gets copied
        visitRoots(rootCtx, nullptr); // signal: iterate all roots

        // Visit remembered set (old→young pointers)
        for (size_t i = 0; i < rememberedCount; i++) {
            GValue* slot = &rememberedSlots[i];
            if (slot->isPtr() && contains(slot->asPtr())) {
                void* moved = copyObject(slot->asPtr());
                *slot = GValue::makePtr(moved);
            }
        }

        // Scan copied objects for internal pointers
        while (scanPtr < freePtr) {
            ObjHeader* hdr = (ObjHeader*)scanPtr;
            uint32_t objSize = hdr->size();
            if (objSize == 0) break; // safety
            scanObject(scanPtr, objSize);
            scanPtr += objSize;
        }

        stats.liveBytes = (size_t)(freePtr - toSpace_);
        stats.promotedBytes = promotedBytes_;

        // Swap spaces
        uint8_t* tmp = fromSpace_;
        fromSpace_ = toSpace_;
        toSpace_ = tmp;
        allocPtr_ = freePtr;
        limit_ = fromSpace_ + size_;

        collections_++;
        stats.pauseNs = clockNs() - startNs;
        return stats;
    }

    // Copy a single object (called by root visitor for each root)
    void* copyObject(void* obj) {
        if (!obj || !contains(obj)) return obj;

        ObjHeader* hdr = (ObjHeader*)obj;

        // Already forwarded?
        if (hdr->type() == ObjType::Forward) {
            return (void*)(uintptr_t)hdr->size(); // forwarding address in size field
        }

        uint32_t objSize = hdr->size();
        uint8_t age = hdr->age();

        // Promote if old enough
        if (age >= PROMOTE_AGE && promoteFn_) {
            void* promoted = promoteFn_(promoteCtx_, obj, objSize);
            // Install forwarding pointer
            hdr->raw = ObjHeader::make(ObjType::Forward, 0, (uint32_t)(uintptr_t)promoted).raw;
            promotedBytes_ += objSize;
            return promoted;
        }

        // Copy to toSpace
        uint8_t* dst = *copyDst_;
        std::memcpy(dst, obj, objSize);
        *copyDst_ += objSize;

        // Increment age on the copy
        ObjHeader* newHdr = (ObjHeader*)dst;
        newHdr->incrementAge();

        // Install forwarding pointer in old location
        hdr->raw = ObjHeader::make(ObjType::Forward, 0, (uint32_t)(uintptr_t)dst).raw;

        return dst;
    }

private:
    void scanObject(uint8_t* obj, uint32_t size) {
        ObjHeader* hdr = (ObjHeader*)obj;
        switch (hdr->type()) {
            case ObjType::Array: {
                GArray* arr = (GArray*)obj;
                for (int32_t i = 0; i < arr->length; i++) {
                    if (arr->data[i].isPtr()) {
                        void* moved = copyObject(arr->data[i].asPtr());
                        arr->data[i] = GValue::makePtr(moved);
                    }
                }
                break;
            }
            case ObjType::Object: {
                GObject* o = (GObject*)obj;
                uint32_t fieldCount = (size - sizeof(ObjHeader)) / sizeof(GValue);
                for (uint32_t i = 0; i < fieldCount; i++) {
                    if (o->fields[i].isPtr()) {
                        void* moved = copyObject(o->fields[i].asPtr());
                        o->fields[i] = GValue::makePtr(moved);
                    }
                }
                break;
            }
            case ObjType::Closure: {
                GClosure* c = (GClosure*)obj;
                for (uint16_t i = 0; i < c->captureCount; i++) {
                    if (c->captures[i].isPtr()) {
                        void* moved = copyObject(c->captures[i].asPtr());
                        c->captures[i] = GValue::makePtr(moved);
                    }
                }
                break;
            }
            case ObjType::Map: {
                GMap* m = (GMap*)obj;
                for (int32_t i = 0; i < m->capacity; i++) {
                    if (m->entries[i].occupied) {
                        if (m->entries[i].key.isPtr()) {
                            void* moved = copyObject(m->entries[i].key.asPtr());
                            m->entries[i].key = GValue::makePtr(moved);
                        }
                        if (m->entries[i].value.isPtr()) {
                            void* moved = copyObject(m->entries[i].value.asPtr());
                            m->entries[i].value = GValue::makePtr(moved);
                        }
                    }
                }
                break;
            }
            case ObjType::String: {
                GString* s = (GString*)obj;
                if (s->kind == StringKind::Rope) {
                    if (s->rope.left) {
                        void* moved = copyObject(s->rope.left);
                        s->rope.left = (GString*)moved;
                    }
                    if (s->rope.right) {
                        void* moved = copyObject(s->rope.right);
                        s->rope.right = (GString*)moved;
                    }
                } else if (s->kind == StringKind::View) {
                    if (s->view.parent) {
                        void* moved = copyObject(s->view.parent);
                        s->view.parent = (GString*)moved;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    static uint64_t clockNs() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }

    size_t   size_;
    uint8_t* base_;
    uint8_t* fromSpace_;
    uint8_t* toSpace_;
    uint8_t* allocPtr_;
    uint8_t* limit_;
    size_t   bytesAllocated_ = 0;
    uint64_t collections_ = 0;

    // Collection state
    uint8_t** copyDst_ = nullptr;
    PromoteCallback promoteFn_ = nullptr;
    void* promoteCtx_ = nullptr;
    size_t promotedBytes_ = 0;
};

// ============================================================================
// OldGen — Incremental Mark-Sweep with Segregated Free Lists
// ============================================================================

class OldGen {
public:
    static constexpr size_t DEFAULT_MAX = 64 * 1024 * 1024;
    static constexpr size_t BLOCK_SIZE = 1024 * 1024; // 1MB blocks
    static constexpr int MARK_INCREMENT = 1024;
    static constexpr int NUM_SIZE_CLASSES = 9; // 16,32,64,128,256,512,1024,2048,large

    explicit OldGen(size_t maxSize = DEFAULT_MAX) : maxSize_(maxSize) {}

    ~OldGen() {
        for (auto& blk : blocks_) munmap(blk.base, blk.mappedSize);
    }

    // Allocate from old gen (for promoted objects)
    void* alloc(size_t n) {
        n = (n + 7) & ~7;
        // Try free list first
        void* ptr = allocFromFreeList(n);
        if (ptr) return ptr;
        // Allocate from current block
        if (!curBlock_ || curBlock_->cursor + n > curBlock_->limit) {
            if (!allocBlock()) return nullptr;
        }
        ptr = curBlock_->cursor;
        curBlock_->cursor += n;
        usedBytes_ += n;
        return ptr;
    }

    // Promote an object from nursery
    void* promote(void* nurseryObj, size_t size) {
        void* dst = alloc(size);
        if (dst) std::memcpy(dst, nurseryObj, size);
        return dst;
    }

    // Incremental mark step. Returns true when mark phase is complete.
    bool markStep(GValue* grayWorklist, int& grayCount) {
        int processed = 0;
        while (grayCount > 0 && processed < MARK_INCREMENT) {
            grayCount--;
            GValue obj = grayWorklist[grayCount];
            if (!obj.isPtr()) continue;
            ObjHeader* hdr = (ObjHeader*)obj.asPtr();
            if (hdr->gcMark() == ObjHeader::GC_BLACK) continue;
            hdr->setGcMark(ObjHeader::GC_BLACK);
            // Push children to gray list (simplified — real impl scans fields)
            processed++;
        }
        return grayCount == 0;
    }

    // Sweep: reclaim white objects, build free lists
    size_t sweep() {
        size_t reclaimed = 0;
        for (auto& blk : blocks_) {
            uint8_t* ptr = blk.base;
            while (ptr < blk.cursor) {
                ObjHeader* hdr = (ObjHeader*)ptr;
                uint32_t objSize = hdr->size();
                if (objSize == 0) break;

                if (hdr->gcMark() == ObjHeader::GC_WHITE) {
                    // Dead — add to free list
                    addToFreeList(ptr, objSize);
                    reclaimed += objSize;
                    usedBytes_ -= objSize;
                } else {
                    // Live — reset mark for next cycle
                    hdr->setGcMark(ObjHeader::GC_WHITE);
                }
                ptr += objSize;
            }
        }
        return reclaimed;
    }

    bool needsCollection() const { return usedBytes_ > maxSize_ * 3 / 4; }
    size_t usedBytes() const { return usedBytes_; }
    size_t maxSize() const { return maxSize_; }

private:
    struct Block {
        uint8_t* base;
        uint8_t* cursor;
        uint8_t* limit;
        size_t   mappedSize;
    };

    struct FreeNode {
        FreeNode* next;
        size_t    size;
    };

    bool allocBlock() {
        size_t mapSize = BLOCK_SIZE;
        uint8_t* mem = (uint8_t*)mmap(nullptr, mapSize, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return false;
        blocks_.push_back({ mem, mem, mem + mapSize, mapSize });
        curBlock_ = &blocks_.back();
        return true;
    }

    int sizeClass(size_t n) const {
        if (n <= 16) return 0;
        if (n <= 32) return 1;
        if (n <= 64) return 2;
        if (n <= 128) return 3;
        if (n <= 256) return 4;
        if (n <= 512) return 5;
        if (n <= 1024) return 6;
        if (n <= 2048) return 7;
        return 8; // large
    }

    void* allocFromFreeList(size_t n) {
        int cls = sizeClass(n);
        // Try exact class first, then larger classes
        for (int c = cls; c < NUM_SIZE_CLASSES; c++) {
            if (freeLists_[c]) {
                FreeNode* node = freeLists_[c];
                if (node->size >= n) {
                    freeLists_[c] = node->next;
                    usedBytes_ += n;
                    return (void*)node;
                }
            }
        }
        return nullptr;
    }

    void addToFreeList(void* ptr, size_t size) {
        int cls = sizeClass(size);
        FreeNode* node = (FreeNode*)ptr;
        node->size = size;
        node->next = freeLists_[cls];
        freeLists_[cls] = node;
    }

    size_t maxSize_;
    size_t usedBytes_ = 0;
    std::vector<Block> blocks_;
    Block* curBlock_ = nullptr;
    FreeNode* freeLists_[NUM_SIZE_CLASSES] = {};
};

// ============================================================================
// Large Object Space — Objects > 8KB (mmap'd individually)
// ============================================================================

class LargeObjectSpace {
public:
    static constexpr size_t THRESHOLD = 8 * 1024;

    void* alloc(size_t n) {
        size_t mapSize = (n + 4095) & ~4095;
        void* mem = mmap(nullptr, mapSize, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return nullptr;
        objects_.push_back({ mem, mapSize });
        totalBytes_ += mapSize;
        return mem;
    }

    void free(void* ptr) {
        for (size_t i = 0; i < objects_.size(); i++) {
            if (objects_[i].ptr == ptr) {
                munmap(objects_[i].ptr, objects_[i].size);
                totalBytes_ -= objects_[i].size;
                objects_[i] = objects_.back();
                objects_.pop_back();
                return;
            }
        }
    }

    bool contains(const void* ptr) const {
        for (const auto& obj : objects_) {
            if (ptr >= obj.ptr && (const uint8_t*)ptr < (const uint8_t*)obj.ptr + obj.size)
                return true;
        }
        return false;
    }

    size_t totalBytes() const { return totalBytes_; }
    size_t count() const { return objects_.size(); }

private:
    struct LargeObj { void* ptr; size_t size; };
    std::vector<LargeObj> objects_;
    size_t totalBytes_ = 0;
};

// ============================================================================
// Remembered Set (Write Barrier Support)
// ============================================================================

class RememberedSet {
public:
    void add(GValue* slot) {
        if (count_ < MAX_SLOTS) {
            slots_[count_++] = slot;
        }
    }

    void clear() { count_ = 0; }
    GValue** slots() { return slots_; }
    size_t count() const { return count_; }

private:
    static constexpr size_t MAX_SLOTS = 16384;
    GValue* slots_[MAX_SLOTS];
    size_t count_ = 0;
};

// ============================================================================
// GC — Unified Interface
// ============================================================================

class GC {
public:
    GC() {}

    // Allocate from nursery (fast path) — simple bump allocator
    void* allocNursery(size_t n) {
        n = (n + 7) & ~7;
        void* ptr = nursery_.alloc(n);
        if (ptr) return ptr;
        return nullptr;
    }

    // Collect nursery with real roots (called by interpreter)
    void collectWithRoots(Nursery::RootCallback visitRoots, void* rootCtx) {
        nursery_.collect(
            visitRoots, rootCtx,
            [](void* ctx, void* obj, size_t sz) -> void* {
                return ((GC*)ctx)->oldGen_.promote(obj, sz);
            }, this,
            (GValue*)rememberedSet_.slots(), rememberedSet_.count()
        );
        rememberedSet_.clear();
    }

    // Allocate in old gen (for large or known long-lived objects)
    void* allocOld(size_t n) {
        if (n >= LargeObjectSpace::THRESHOLD) {
            return largeObjects_.alloc(n);
        }
        return oldGen_.alloc(n);
    }

    // Write barrier: call when storing a pointer into an old-gen object
    void writeBarrier(GValue* slot, GValue newVal) {
        if (newVal.isPtr() && nursery_.contains(newVal.asPtr())) {
            rememberedSet_.add(slot);
        }
    }

    // Accessors
    Nursery& nursery() { return nursery_; }
    OldGen& oldGen() { return oldGen_; }
    LargeObjectSpace& largeObjects() { return largeObjects_; }
    RememberedSet& rememberedSet() { return rememberedSet_; }

    size_t nurseryUsed() const { return nursery_.used(); }
    size_t oldGenUsed() const { return oldGen_.usedBytes(); }
    uint64_t nurseryCollections() const { return nursery_.collections(); }

private:
    Nursery nursery_;
    OldGen oldGen_;
    LargeObjectSpace largeObjects_;
    RememberedSet rememberedSet_;
};

} // namespace gardvm
