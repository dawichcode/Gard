#pragma once
// GARDVM Tier 2 — Object Model & Heap Layout
// All heap objects share a common 8-byte header.
// Objects are cache-line friendly where possible.

#include "value.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>

namespace gardvm {

// ============================================================================
// Object Type Enum
// ============================================================================

enum class ObjType : uint8_t {
    String   = 0,
    Array    = 1,
    Map      = 2,
    Object   = 3,
    Closure  = 4,
    Future   = 5,
    Channel  = 6,
    Buffer   = 7,
    Error    = 8,
    // GC internal
    Forward  = 62,
    Free     = 63,
};

// ============================================================================
// Object Header (8 bytes)
// ============================================================================
//
// Bit layout:
//   [63-62] GC mark (2 bits): 0=white, 1=gray, 2=black, 3=pinned
//   [61-58] Age (4 bits): generational age 0-15
//   [57-52] Type (6 bits): ObjType enum
//   [51-32] Shape ID (20 bits): up to 1,048,576 shapes
//   [31-0]  Size (32 bits): total allocation size in bytes

struct ObjHeader {
    uint64_t raw;

    static constexpr uint64_t GC_SHIFT    = 62;
    static constexpr uint64_t AGE_SHIFT   = 58;
    static constexpr uint64_t TYPE_SHIFT  = 52;
    static constexpr uint64_t SHAPE_SHIFT = 32;

    static constexpr uint64_t GC_MASK    = 0xC000000000000000ULL;
    static constexpr uint64_t AGE_MASK   = 0x3C00000000000000ULL;
    static constexpr uint64_t TYPE_MASK  = 0x03F0000000000000ULL;
    static constexpr uint64_t SHAPE_MASK = 0x000FFFFF00000000ULL;
    static constexpr uint64_t SIZE_MASK  = 0x00000000FFFFFFFFULL;

    static constexpr uint8_t GC_WHITE  = 0;
    static constexpr uint8_t GC_GRAY   = 1;
    static constexpr uint8_t GC_BLACK  = 2;
    static constexpr uint8_t GC_PINNED = 3;

    static ObjHeader make(ObjType type, uint32_t shapeId, uint32_t size) {
        ObjHeader h;
        h.raw = ((uint64_t)GC_WHITE << GC_SHIFT) |
                ((uint64_t)0 << AGE_SHIFT) |
                ((uint64_t)(uint8_t)type << TYPE_SHIFT) |
                ((uint64_t)(shapeId & 0xFFFFF) << SHAPE_SHIFT) |
                (uint64_t)size;
        return h;
    }

    uint8_t  gcMark()  const { return (uint8_t)((raw & GC_MASK) >> GC_SHIFT); }
    uint8_t  age()     const { return (uint8_t)((raw & AGE_MASK) >> AGE_SHIFT); }
    ObjType  type()    const { return (ObjType)((raw & TYPE_MASK) >> TYPE_SHIFT); }
    uint32_t shapeId() const { return (uint32_t)((raw & SHAPE_MASK) >> SHAPE_SHIFT); }
    uint32_t size()    const { return (uint32_t)(raw & SIZE_MASK); }

    void setGcMark(uint8_t mark) {
        raw = (raw & ~GC_MASK) | ((uint64_t)(mark & 3) << GC_SHIFT);
    }

    void setAge(uint8_t a) {
        raw = (raw & ~AGE_MASK) | ((uint64_t)(a & 0xF) << AGE_SHIFT);
    }

    void incrementAge() {
        uint8_t a = age();
        if (a < 15) setAge(a + 1);
    }
};

static_assert(sizeof(ObjHeader) == 8, "ObjHeader must be 8 bytes");

// ============================================================================
// GString — Optimized String (48 bytes)
// ============================================================================

enum class StringKind : uint8_t {
    Inline = 0,   // data stored in-object (≤ 22 bytes)
    Heap   = 1,   // heap-allocated buffer
    View   = 2,   // slice of another string (zero-copy)
    Rope   = 3,   // lazy concatenation of two strings
};

static constexpr uint32_t STRING_INLINE_CAP = 22;
static constexpr uint32_t ROPE_MAX_DEPTH = 32;

struct GString {
    ObjHeader header;       // 8 bytes
    uint32_t  length;       // 4 bytes
    uint32_t  hash;         // 4 bytes (0 = not computed)
    StringKind kind;        // 1 byte
    uint8_t   _pad[3];     // 3 bytes

    union {                 // 24 bytes
        char inlineData[24];

        struct {
            char*    buf;
            uint32_t capacity;
            uint32_t _reserved;
        } heap;

        struct {
            GString* parent;
            uint32_t offset;
            uint32_t _reserved;
        } view;

        struct {
            GString* left;
            GString* right;
            uint32_t depth;
            uint32_t _reserved;
        } rope;
    };

    // Get character data. For Rope, this flattens in-place.
    const char* data() {
        switch (kind) {
            case StringKind::Inline: return inlineData;
            case StringKind::Heap:   return heap.buf;
            case StringKind::View:   return view.parent->data() + view.offset;
            case StringKind::Rope:   flatten(); return heap.buf;
        }
        return nullptr;
    }

    const char* dataConst() const {
        switch (kind) {
            case StringKind::Inline: return inlineData;
            case StringKind::Heap:   return heap.buf;
            case StringKind::View:   return view.parent->dataConst() + view.offset;
            case StringKind::Rope:   return nullptr; // must call non-const data() to flatten
        }
        return nullptr;
    }

    // Compute FNV-1a hash
    uint32_t computeHash() {
        if (hash != 0) return hash;
        const char* s = data();
        if (!s) return 0;
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < length; i++) {
            h ^= (uint8_t)s[i];
            h *= 16777619u;
        }
        if (h == 0) h = 1; // reserve 0 for "not computed"
        hash = h;
        return h;
    }

    // Flatten rope in-place (iterative, no recursion)
    // NOTE: buffer allocated via malloc (lives outside GC heap).
    // This is intentional — flattened strings are long-lived and
    // the buffer pointer is stable (not moved by GC).
    void flatten() {
        if (kind != StringKind::Rope) return;

        char* buf = (char*)std::malloc(length + 1);
        if (!buf) return;
        uint32_t pos = 0;

        // Iterative traversal using explicit stack
        struct Entry { GString* node; };
        Entry stack[64];
        int top = 0;
        stack[top++] = { this };

        while (top > 0) {
            GString* node = stack[--top].node;
            if (!node) continue;
            switch (node->kind) {
                case StringKind::Inline:
                    std::memcpy(buf + pos, node->inlineData, node->length);
                    pos += node->length;
                    break;
                case StringKind::Heap:
                    std::memcpy(buf + pos, node->heap.buf, node->length);
                    pos += node->length;
                    break;
                case StringKind::View:
                    std::memcpy(buf + pos, node->view.parent->data() + node->view.offset, node->length);
                    pos += node->length;
                    break;
                case StringKind::Rope:
                    // Push right first (LIFO → left processed first)
                    if (top < 63) stack[top++] = { node->rope.right };
                    if (top < 63) stack[top++] = { node->rope.left };
                    break;
            }
        }
        buf[length] = '\0';

        // Convert to Heap in-place
        kind = StringKind::Heap;
        heap.buf = buf;
        heap.capacity = length + 1;
    }
};

static_assert(sizeof(GString) == 48, "GString must be 48 bytes");

// ============================================================================
// GArray — Dynamic Array (32 bytes)
// ============================================================================

struct GArray {
    ObjHeader header;       // 8 bytes
    int32_t   length;       // 4 bytes
    int32_t   capacity;     // 4 bytes
    GValue*   data;         // 8 bytes
    uint8_t   elemType;     // 0=mixed, 1=i32, 2=i64, 3=f64
    uint8_t   _pad[7];     // 7 bytes alignment
};

static_assert(sizeof(GArray) == 32, "GArray must be 32 bytes");

// ============================================================================
// GMapEntry — Single hash map entry (24 bytes)
// ============================================================================

struct GMapEntry {
    GValue   key;           // 8 bytes
    GValue   value;         // 8 bytes
    uint32_t hash;          // 4 bytes (cached key hash)
    int16_t  psl;           // 2 bytes (probe sequence length, Robin Hood)
    uint8_t  occupied;      // 1 byte
    uint8_t  _pad;          // 1 byte
};

static_assert(sizeof(GMapEntry) == 24, "GMapEntry must be 24 bytes");

// ============================================================================
// GMap — Hash Map (32 bytes)
// ============================================================================

struct GMap {
    ObjHeader   header;     // 8 bytes
    int32_t     count;      // 4 bytes
    int32_t     capacity;   // 4 bytes (always power of 2)
    GMapEntry*  entries;    // 8 bytes
    uint8_t     _pad[8];   // 8 bytes alignment
};

static_assert(sizeof(GMap) == 32, "GMap must be 32 bytes");

// ============================================================================
// GObject — Class Instance (8 + N*8 bytes)
// ============================================================================

struct GObject {
    ObjHeader header;       // 8 bytes
    GValue    fields[];     // flexible array — count from shape
};

// ============================================================================
// GClosure — Function Closure (16 + N*8 bytes)
// ============================================================================

struct GClosure {
    ObjHeader header;       // 8 bytes
    uint16_t  funcIndex;    // 2 bytes
    uint16_t  captureCount; // 2 bytes
    uint32_t  _pad;         // 4 bytes alignment
    GValue    captures[];   // flexible array
};

// ============================================================================
// GError — Exception Object (8 + fields)
// ============================================================================

struct GError {
    ObjHeader header;       // 8 bytes
    GValue    type;         // 8 bytes (string: "NullReferenceError", etc.)
    GValue    message;      // 8 bytes (string)
    int32_t   line;         // 4 bytes
    int32_t   column;       // 4 bytes
    GValue    caller;       // 8 bytes (string: function name)
};

// ============================================================================
// Inline field access (fastest path — used by interpreter and JIT)
// ============================================================================

inline GValue objectGetField(const GObject* obj, uint16_t slotIndex) {
    return obj->fields[slotIndex];
}

inline void objectSetField(GObject* obj, uint16_t slotIndex, GValue val) {
    obj->fields[slotIndex] = val;
}

inline GValue objectGetFieldByOffset(const GObject* obj, uint16_t byteOffset) {
    return *(const GValue*)((const uint8_t*)obj + sizeof(ObjHeader) + byteOffset);
}

inline void objectSetFieldByOffset(GObject* obj, uint16_t byteOffset, GValue val) {
    *(GValue*)((uint8_t*)obj + sizeof(ObjHeader) + byteOffset) = val;
}

} // namespace gardvm
