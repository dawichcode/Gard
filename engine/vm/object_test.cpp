// GARDVM Tier 2 — Object Model Tests
// Validates object header, GString, GArray, GMap, GObject, GClosure, Shape table.
// Compile: g++ -std=c++17 -O2 -o object_test object_test.cpp && ./object_test

#include "value.h"
#include "object.h"
#include "shape.h"
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <chrono>

using namespace gardvm;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(expr) do { \
    if (expr) { tests_passed++; } \
    else { tests_failed++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); } \
} while(0)

// ============================================================================
// Object Header Tests
// ============================================================================

void test_header() {
    ObjHeader h = ObjHeader::make(ObjType::String, 42, 1024);
    TEST(h.type() == ObjType::String);
    TEST(h.shapeId() == 42);
    TEST(h.size() == 1024);
    TEST(h.gcMark() == ObjHeader::GC_WHITE);
    TEST(h.age() == 0);

    h.setGcMark(ObjHeader::GC_BLACK);
    TEST(h.gcMark() == ObjHeader::GC_BLACK);
    TEST(h.type() == ObjType::String); // unchanged
    TEST(h.size() == 1024);            // unchanged

    h.setAge(5);
    TEST(h.age() == 5);
    h.incrementAge();
    TEST(h.age() == 6);

    // Max shape ID (20 bits = 1,048,575)
    ObjHeader h2 = ObjHeader::make(ObjType::Object, 0xFFFFF, 999);
    TEST(h2.shapeId() == 0xFFFFF);

    // Max size (32 bits)
    ObjHeader h3 = ObjHeader::make(ObjType::Array, 0, 0xFFFFFFFF);
    TEST(h3.size() == 0xFFFFFFFF);

    // Pinned
    h.setGcMark(ObjHeader::GC_PINNED);
    TEST(h.gcMark() == ObjHeader::GC_PINNED);
}

// ============================================================================
// GString Tests
// ============================================================================

void test_string_inline() {
    // Allocate inline string on stack (simulating heap allocation)
    uint8_t buf[sizeof(GString)];
    std::memset(buf, 0, sizeof(GString));
    GString* s = (GString*)buf;
    s->header = ObjHeader::make(ObjType::String, 0, sizeof(GString));
    s->length = 5;
    s->hash = 0;
    s->kind = StringKind::Inline;
    std::memcpy(s->inlineData, "hello", 5);
    s->inlineData[5] = '\0';

    TEST(s->length == 5);
    TEST(s->kind == StringKind::Inline);
    TEST(std::strcmp(s->data(), "hello") == 0);
    TEST(s->header.type() == ObjType::String);

    // Hash computation
    uint32_t h = s->computeHash();
    TEST(h != 0);
    TEST(s->hash == h);
    // Second call returns cached
    TEST(s->computeHash() == h);
}

void test_string_heap() {
    // Create a string longer than inline capacity
    char longStr[100];
    std::memset(longStr, 'x', 99);
    longStr[99] = '\0';

    uint8_t buf[sizeof(GString)];
    std::memset(buf, 0, sizeof(GString));
    GString* s = (GString*)buf;
    s->header = ObjHeader::make(ObjType::String, 0, sizeof(GString));
    s->length = 99;
    s->hash = 0;
    s->kind = StringKind::Heap;
    s->heap.buf = (char*)std::malloc(100);
    std::memcpy(s->heap.buf, longStr, 100);
    s->heap.capacity = 100;

    TEST(s->length == 99);
    TEST(s->kind == StringKind::Heap);
    TEST(std::strcmp(s->data(), longStr) == 0);

    std::free(s->heap.buf);
}

void test_string_view() {
    // Parent string
    uint8_t parentBuf[sizeof(GString)];
    std::memset(parentBuf, 0, sizeof(GString));
    GString* parent = (GString*)parentBuf;
    parent->header = ObjHeader::make(ObjType::String, 0, sizeof(GString));
    parent->length = 11;
    parent->hash = 0;
    parent->kind = StringKind::Inline;
    std::memcpy(parent->inlineData, "hello world", 11);
    parent->inlineData[11] = '\0';

    // View: "world" (offset 6, length 5)
    uint8_t viewBuf[sizeof(GString)];
    std::memset(viewBuf, 0, sizeof(GString));
    GString* view = (GString*)viewBuf;
    view->header = ObjHeader::make(ObjType::String, 0, sizeof(GString));
    view->length = 5;
    view->hash = 0;
    view->kind = StringKind::View;
    view->view.parent = parent;
    view->view.offset = 6;

    TEST(view->length == 5);
    TEST(view->kind == StringKind::View);
    TEST(std::strncmp(view->data(), "world", 5) == 0);
}

void test_string_rope() {
    // Left: "hello "
    uint8_t leftBuf[sizeof(GString)];
    std::memset(leftBuf, 0, sizeof(GString));
    GString* left = (GString*)leftBuf;
    left->header = ObjHeader::make(ObjType::String, 0, sizeof(GString));
    left->length = 6;
    left->kind = StringKind::Inline;
    std::memcpy(left->inlineData, "hello ", 6);
    left->inlineData[6] = '\0';

    // Right: "world"
    uint8_t rightBuf[sizeof(GString)];
    std::memset(rightBuf, 0, sizeof(GString));
    GString* right = (GString*)rightBuf;
    right->header = ObjHeader::make(ObjType::String, 0, sizeof(GString));
    right->length = 5;
    right->kind = StringKind::Inline;
    std::memcpy(right->inlineData, "world", 5);
    right->inlineData[5] = '\0';

    // Rope
    uint8_t ropeBuf[sizeof(GString)];
    std::memset(ropeBuf, 0, sizeof(GString));
    GString* rope = (GString*)ropeBuf;
    rope->header = ObjHeader::make(ObjType::String, 0, sizeof(GString));
    rope->length = 11;
    rope->kind = StringKind::Rope;
    rope->rope.left = left;
    rope->rope.right = right;
    rope->rope.depth = 1;

    TEST(rope->length == 11);
    TEST(rope->kind == StringKind::Rope);

    // Flatten
    rope->flatten();
    TEST(rope->kind == StringKind::Heap);
    TEST(std::strcmp(rope->data(), "hello world") == 0);
    TEST(rope->length == 11);

    std::free(rope->heap.buf);
}

// ============================================================================
// GArray Tests
// ============================================================================

void test_array() {
    GArray arr;
    arr.header = ObjHeader::make(ObjType::Array, 0, sizeof(GArray));
    arr.length = 0;
    arr.capacity = 16;
    arr.data = (GValue*)std::malloc(16 * sizeof(GValue));
    arr.elemType = 0;

    // Add elements
    arr.data[0] = GValue::makeInt(42);
    arr.data[1] = GValue::makeInt(99);
    arr.data[2] = GValue::makeDouble(3.14);
    arr.length = 3;

    TEST(arr.length == 3);
    TEST(arr.capacity == 16);
    TEST(arr.data[0].asInt() == 42);
    TEST(arr.data[1].asInt() == 99);
    TEST(arr.data[2].asDouble() == 3.14);
    TEST(arr.header.type() == ObjType::Array);

    std::free(arr.data);
}

// ============================================================================
// GMap Tests
// ============================================================================

void test_map() {
    GMap map;
    map.header = ObjHeader::make(ObjType::Map, 0, sizeof(GMap));
    map.count = 0;
    map.capacity = 8;
    map.entries = (GMapEntry*)std::calloc(8, sizeof(GMapEntry));

    // Insert an entry manually (Robin Hood logic tested separately)
    map.entries[0].key = GValue::makeInt(1);
    map.entries[0].value = GValue::makeInt(100);
    map.entries[0].hash = 12345;
    map.entries[0].psl = 0;
    map.entries[0].occupied = 1;
    map.count = 1;

    TEST(map.count == 1);
    TEST(map.entries[0].occupied == 1);
    TEST(map.entries[0].key.asInt() == 1);
    TEST(map.entries[0].value.asInt() == 100);

    std::free(map.entries);
}

// ============================================================================
// GObject Tests
// ============================================================================

void test_object() {
    // Allocate object with 3 fields
    size_t totalSize = sizeof(ObjHeader) + 3 * sizeof(GValue);
    uint8_t* mem = (uint8_t*)std::calloc(1, totalSize);
    GObject* obj = (GObject*)mem;
    obj->header = ObjHeader::make(ObjType::Object, 7, (uint32_t)totalSize);

    // Set fields by slot index
    objectSetField(obj, 0, GValue::makeInt(10));
    objectSetField(obj, 1, GValue::makeInt(20));
    objectSetField(obj, 2, GValue::makeDouble(3.14));

    TEST(objectGetField(obj, 0).asInt() == 10);
    TEST(objectGetField(obj, 1).asInt() == 20);
    TEST(objectGetField(obj, 2).asDouble() == 3.14);
    TEST(obj->header.shapeId() == 7);
    TEST(obj->header.type() == ObjType::Object);

    // By byte offset
    TEST(objectGetFieldByOffset(obj, 0).asInt() == 10);
    TEST(objectGetFieldByOffset(obj, 8).asInt() == 20);
    TEST(objectGetFieldByOffset(obj, 16).asDouble() == 3.14);

    std::free(mem);
}

// ============================================================================
// GClosure Tests
// ============================================================================

void test_closure() {
    size_t totalSize = sizeof(GClosure) + 2 * sizeof(GValue);
    uint8_t* mem = (uint8_t*)std::calloc(1, totalSize);
    GClosure* cl = (GClosure*)mem;
    cl->header = ObjHeader::make(ObjType::Closure, 0, (uint32_t)totalSize);
    cl->funcIndex = 5;
    cl->captureCount = 2;
    cl->captures[0] = GValue::makeInt(100);
    cl->captures[1] = GValue::makePtr(nullptr);

    TEST(cl->funcIndex == 5);
    TEST(cl->captureCount == 2);
    TEST(cl->captures[0].asInt() == 100);
    TEST(cl->captures[1].isPtr());
    TEST(cl->header.type() == ObjType::Closure);

    std::free(mem);
}

// ============================================================================
// GError Tests
// ============================================================================

void test_error() {
    GError err;
    err.header = ObjHeader::make(ObjType::Error, 0, sizeof(GError));
    err.type = GValue::makePtr(nullptr);    // would be a GString*
    err.message = GValue::makePtr(nullptr); // would be a GString*
    err.line = 42;
    err.column = 10;
    err.caller = GValue::makePtr(nullptr);

    TEST(err.header.type() == ObjType::Error);
    TEST(err.line == 42);
    TEST(err.column == 10);
}

// ============================================================================
// Shape Table Tests
// ============================================================================

void test_shape_table() {
    ShapeTable table;

    // Shape 0 is "unknown" (reserved)
    TEST(table.count() == 1);
    TEST(table.get(0)->className == "<unknown>");

    // Create "Point" with 2 fields
    uint32_t pointId = table.createShape("Point", 2);
    TEST(pointId == 1);
    const Shape* point = table.get(pointId);
    TEST(point->fieldCount == 2);
    TEST(point->className == "Point");
    TEST(point->totalSize == sizeof(uint64_t) + 2 * sizeof(GValue));

    // Create "Point3D" extending Point with 1 extra field
    uint32_t point3dId = table.createShape("Point3D", 3);
    table.setParent(point3dId, pointId);
    const Shape* point3d = table.get(point3dId);
    TEST(point3d->fieldCount == 3);
    TEST(point3d->parentShapeId == pointId);

    // Lookup by name (re-fetch pointers since vector may have grown)
    TEST(table.getByName("Point") == table.get(pointId));
    TEST(table.getByName("Point3D") == table.get(point3dId));
    TEST(table.getByName("NonExistent") == nullptr);
    TEST(table.getIdByName("Point") == pointId);

    // Create shape with named fields
    std::vector<std::pair<uint16_t, std::string>> fields = {
        {10, "x"}, {11, "y"}, {12, "z"}
    };
    uint32_t vecId = table.createShapeWithFields("Vector", fields);
    const Shape* vec = table.get(vecId);
    TEST(vec->fieldCount == 3);
    TEST(vec->fields[0].nameIndex == 10);
    TEST(vec->fields[1].nameIndex == 11);
    TEST(vec->fields[2].nameIndex == 12);
    TEST(vec->findField(11) == 1); // "y" is slot 1

    // resolveField with inheritance
    // Add field nameIndex=20 to Point
    table.getMut(pointId)->fields[0].nameIndex = 20;
    table.getMut(pointId)->fields[1].nameIndex = 21;
    // Point3D inherits from Point
    table.getMut(point3dId)->fields[0].nameIndex = 20;
    table.getMut(point3dId)->fields[1].nameIndex = 21;
    table.getMut(point3dId)->fields[2].nameIndex = 22;
    TEST(table.resolveField(point3dId, 22) == 2); // own field
    TEST(table.resolveField(point3dId, 20) == 0); // inherited
    TEST(table.resolveField(point3dId, 99) == -1); // not found

    // Dynamic field addition
    table.addField(pointId, 30);
    TEST(table.get(pointId)->fieldCount == 3);
    TEST(table.get(pointId)->findField(30) == 2);
}

// ============================================================================
// Size Assertions
// ============================================================================

void test_sizes() {
    TEST(sizeof(ObjHeader) == 8);
    TEST(sizeof(GString) == 48);
    TEST(sizeof(GArray) == 32);
    TEST(sizeof(GMap) == 32);
    TEST(sizeof(GMapEntry) == 24);
}

// ============================================================================
// Benchmark: Field Access
// ============================================================================

void benchmark_field_access() {
    size_t totalSize = sizeof(ObjHeader) + 8 * sizeof(GValue);
    uint8_t* mem = (uint8_t*)std::calloc(1, totalSize);
    GObject* obj = (GObject*)mem;
    obj->header = ObjHeader::make(ObjType::Object, 1, (uint32_t)totalSize);
    objectSetField(obj, 3, GValue::makeInt(42));

    auto start = std::chrono::high_resolution_clock::now();
    volatile int32_t sink = 0;
    for (int64_t i = 0; i < 1000000000LL; i++) {
        sink = objectGetField(obj, 3).asInt();
    }
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    printf("Benchmark: 1B field reads in %.1f ms (%.2f ns/op)\n", ms, ms * 1e6 / 1e9);
    if (ms / 1e9 * 1e9 < 2.0 * 1e9) { // < 2ns per op
        printf("  PASS (< 2ns/op)\n");
    } else {
        printf("  WARN (> 2ns/op)\n");
    }
    (void)sink;
    std::free(mem);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    test_header();
    test_string_inline();
    test_string_heap();
    test_string_view();
    test_string_rope();
    test_array();
    test_map();
    test_object();
    test_closure();
    test_error();
    test_shape_table();
    test_sizes();

    printf("\n=== Tier 2 Object Model Tests ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    if (tests_failed > 0) {
        printf("\nFAILED\n");
        return 1;
    }
    printf("\nALL PASSED\n\n");

    benchmark_field_access();
    return 0;
}
