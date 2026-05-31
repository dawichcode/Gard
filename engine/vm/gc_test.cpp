// GARDVM Tier 3 — Memory Management Tests
// Validates Region allocator, Nursery, OldGen, LargeObjectSpace, RememberedSet.
// Compile: g++ -std=c++17 -O2 -o gc_test gc_test.cpp && ./gc_test

#include "value.h"
#include "object.h"
#include "region.h"
#include "gc.h"
#include <cstdio>
#include <cassert>
#include <chrono>

using namespace gardvm;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(expr) do { \
    if (expr) { tests_passed++; } \
    else { tests_failed++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); } \
} while(0)

// ============================================================================
// 3A — Region Allocator Tests
// ============================================================================

void test_region_basic() {
    Region r;
    void* p1 = r.alloc(64);
    void* p2 = r.alloc(128);
    void* p3 = r.alloc(256);
    TEST(p1 != nullptr);
    TEST(p2 != nullptr);
    TEST(p3 != nullptr);
    TEST(p1 != p2);
    TEST(p2 != p3);
    TEST(r.totalAllocated() == 64 + 128 + 256);
    TEST(r.pageCount() >= 1);
}

void test_region_alignment() {
    Region r;
    // Odd sizes should be rounded up to 8-byte alignment
    void* p1 = r.alloc(1);
    void* p2 = r.alloc(3);
    void* p3 = r.alloc(7);
    TEST(((uintptr_t)p1 & 7) == 0);
    TEST(((uintptr_t)p2 & 7) == 0);
    TEST(((uintptr_t)p3 & 7) == 0);
}

void test_region_mark_reset() {
    Region r;
    void* p1 = r.alloc(64);
    Region::Mark m = r.mark();
    void* p2 = r.alloc(128);
    void* p3 = r.alloc(256);
    TEST(r.totalAllocated() == 64 + 128 + 256);

    r.resetToMark(m);
    TEST(r.totalAllocated() == 64);

    // Allocating again reuses the space
    void* p4 = r.alloc(128);
    TEST(p4 != nullptr);
    (void)p1; (void)p2; (void)p3;
}

void test_region_large_alloc() {
    Region r;
    // Allocate more than one page (64KB)
    void* big = r.alloc(100 * 1024); // 100KB
    TEST(big != nullptr);
    TEST(r.pageCount() >= 2); // needed a new page
}

void test_region_reset() {
    Region r;
    r.alloc(1000);
    r.alloc(2000);
    r.reset();
    TEST(r.totalAllocated() == 0);
    TEST(r.pageCount() == 1); // keeps first page
    // Can still allocate after reset
    void* p = r.alloc(64);
    TEST(p != nullptr);
}

void test_region_scope() {
    Region r;
    r.alloc(96);  // 96 bytes (already 8-aligned)
    {
        RegionScope scope(r);
        r.alloc(200);
        r.alloc(304);
        TEST(r.totalAllocated() == 96 + 200 + 304);
    }
    // After scope, allocations inside are freed
    TEST(r.totalAllocated() == 96);
}

// ============================================================================
// 3B — Nursery Tests
// ============================================================================

void test_nursery_alloc() {
    Nursery nursery(64 * 1024); // 64KB for testing
    void* p1 = nursery.alloc(48);
    void* p2 = nursery.alloc(48);
    TEST(p1 != nullptr);
    TEST(p2 != nullptr);
    TEST(p1 != p2);
    TEST(nursery.contains(p1));
    TEST(nursery.contains(p2));
    TEST(nursery.used() == 96);
}

void test_nursery_full() {
    Nursery nursery(256); // tiny nursery
    void* p1 = nursery.alloc(128);
    void* p2 = nursery.alloc(128);
    void* p3 = nursery.alloc(128); // should fail
    TEST(p1 != nullptr);
    TEST(p2 != nullptr);
    TEST(p3 == nullptr); // full
    TEST(nursery.isFull() || nursery.used() == 256);
}

void test_nursery_contains() {
    Nursery nursery(4096);
    void* inside = nursery.alloc(64);
    int stackVar = 42;
    TEST(nursery.contains(inside));
    TEST(!nursery.contains(&stackVar));
    TEST(!nursery.contains(nullptr));
}

// ============================================================================
// 3C — OldGen Tests
// ============================================================================

void test_oldgen_alloc() {
    OldGen old(1024 * 1024); // 1MB max
    void* p1 = old.alloc(48);
    void* p2 = old.alloc(96);
    TEST(p1 != nullptr);
    TEST(p2 != nullptr);
    TEST(p1 != p2);
    TEST(old.usedBytes() == 48 + 96);
}

void test_oldgen_promote() {
    OldGen old(1024 * 1024);
    uint8_t fakeObj[48];
    ObjHeader* hdr = (ObjHeader*)fakeObj;
    *hdr = ObjHeader::make(ObjType::Object, 1, 48);

    void* promoted = old.promote(fakeObj, 48);
    TEST(promoted != nullptr);
    ObjHeader* promHdr = (ObjHeader*)promoted;
    TEST(promHdr->type() == ObjType::Object);
    TEST(promHdr->shapeId() == 1);
    TEST(promHdr->size() == 48);
}

void test_oldgen_freelist() {
    OldGen old(1024 * 1024);
    // Allocate and then sweep to populate free list
    void* p1 = old.alloc(64);
    void* p2 = old.alloc(64);
    void* p3 = old.alloc(64);

    // Mark p2 as dead (white), p1 and p3 as live (black)
    ObjHeader* h1 = (ObjHeader*)p1;
    ObjHeader* h2 = (ObjHeader*)p2;
    ObjHeader* h3 = (ObjHeader*)p3;
    *h1 = ObjHeader::make(ObjType::Object, 0, 64);
    *h2 = ObjHeader::make(ObjType::Object, 0, 64);
    *h3 = ObjHeader::make(ObjType::Object, 0, 64);
    h1->setGcMark(ObjHeader::GC_BLACK);
    // h2 stays WHITE (dead)
    h3->setGcMark(ObjHeader::GC_BLACK);

    size_t reclaimed = old.sweep();
    TEST(reclaimed == 64); // p2 was reclaimed

    // Now allocate from free list
    void* p4 = old.alloc(64);
    TEST(p4 != nullptr);
    // Should reuse p2's memory
    TEST(p4 == p2);
}

// ============================================================================
// 3D — Write Barrier + Remembered Set Tests
// ============================================================================

void test_remembered_set() {
    RememberedSet rs;
    GValue slots[4];
    rs.add(&slots[0]);
    rs.add(&slots[1]);
    rs.add(&slots[2]);
    TEST(rs.count() == 3);
    rs.clear();
    TEST(rs.count() == 0);
}

void test_write_barrier() {
    GC gc;
    // Allocate in nursery
    void* young = gc.allocNursery(48);
    GValue youngVal = GValue::makePtr(young);

    // Simulate old-gen slot
    GValue oldSlot = GVAL_NULL;
    gc.writeBarrier(&oldSlot, youngVal);
    TEST(gc.rememberedSet().count() == 1);
}

// ============================================================================
// 3E — Large Object Space Tests
// ============================================================================

void test_large_objects() {
    LargeObjectSpace los;
    void* big = los.alloc(16 * 1024); // 16KB
    TEST(big != nullptr);
    TEST(los.contains(big));
    TEST(los.count() == 1);
    TEST(los.totalBytes() >= 16 * 1024);

    void* big2 = los.alloc(32 * 1024);
    TEST(big2 != nullptr);
    TEST(los.count() == 2);

    los.free(big);
    TEST(los.count() == 1);
    TEST(!los.contains(big));
    TEST(los.contains(big2));

    los.free(big2);
    TEST(los.count() == 0);
}

// ============================================================================
// GC Unified Interface Tests
// ============================================================================

void test_gc_unified() {
    GC gc;
    // Nursery allocation
    void* p1 = gc.allocNursery(48);
    TEST(p1 != nullptr);
    TEST(gc.nurseryUsed() == 48);

    // Old gen allocation
    void* p2 = gc.allocOld(96);
    TEST(p2 != nullptr);
    TEST(gc.oldGenUsed() == 96);

    // Large object
    void* p3 = gc.allocOld(16 * 1024);
    TEST(p3 != nullptr);
    TEST(gc.largeObjects().count() == 1);
}

// ============================================================================
// Benchmarks
// ============================================================================

void benchmark_region() {
    Region r;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 10000000; i++) {
        r.alloc(32);
        if (i % 1000 == 999) r.reset(); // periodic reset to avoid OOM
    }
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    printf("Benchmark: 10M region allocs in %.1f ms (%.1f ns/op)\n", ms, ms * 1e6 / 1e7);
    if (ms < 50.0) printf("  PASS (< 50ms)\n");
    else printf("  WARN (> 50ms)\n");
}

void benchmark_nursery() {
    Nursery nursery(2 * 1024 * 1024);
    // Fill nursery to ~50%
    for (int i = 0; i < 20000; i++) {
        nursery.alloc(48);
    }
    // Measure collection time (no roots = everything is dead)
    auto start = std::chrono::high_resolution_clock::now();
    auto stats = nursery.collect(
        [](void*, GValue*) {}, nullptr,
        [](void*, void* obj, size_t sz) -> void* { return nullptr; }, nullptr,
        nullptr, 0
    );
    auto end = std::chrono::high_resolution_clock::now();
    double us = std::chrono::duration<double, std::micro>(end - start).count();
    printf("Benchmark: Nursery collection in %.1f μs (live=%zu, promoted=%zu)\n",
           us, stats.liveBytes, stats.promotedBytes);
    if (us < 80.0) printf("  PASS (< 80μs)\n");
    else printf("  WARN (> 80μs)\n");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    test_region_basic();
    test_region_alignment();
    test_region_mark_reset();
    test_region_large_alloc();
    test_region_reset();
    test_region_scope();
    test_nursery_alloc();
    test_nursery_full();
    test_nursery_contains();
    test_oldgen_alloc();
    test_oldgen_promote();
    test_oldgen_freelist();
    test_remembered_set();
    test_write_barrier();
    test_large_objects();
    test_gc_unified();

    printf("\n=== Tier 3 Memory Management Tests ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    if (tests_failed > 0) {
        printf("\nFAILED\n");
        return 1;
    }
    printf("\nALL PASSED\n\n");

    benchmark_region();
    benchmark_nursery();
    return 0;
}
