#pragma once
// GARDVM Tier 1 — NaN-Boxed Value Representation
// Every Gard value fits in exactly 8 bytes. Zero heap allocation for primitives.
//
// Encoding:
//   If bits are NOT a quiet NaN → raw f64 (double).
//   If bits ARE a quiet NaN → tag bits encode type, payload holds data.
//   Pointer values use the sign bit to distinguish from other NaN-boxed types.

#include <cstdint>
#include <cstring>

namespace gardvm {

// ============================================================================
// NaN-boxing constants
// ============================================================================

static constexpr uint64_t QNAN       = 0x7FFC000000000000ULL;
static constexpr uint64_t SIGN_BIT   = 0x8000000000000000ULL;
static constexpr uint64_t TAG_MASK   = 0x0007000000000000ULL;
static constexpr uint64_t PAYLOAD    = 0x0000FFFFFFFFFFFFULL;

static constexpr uint64_t TAG_INT    = 0x0001000000000000ULL;
static constexpr uint64_t TAG_BOOL   = 0x0002000000000000ULL;
static constexpr uint64_t TAG_NULL   = 0x0003000000000000ULL;
static constexpr uint64_t TAG_CHAR   = 0x0004000000000000ULL;
static constexpr uint64_t TAG_UNDEF  = 0x0005000000000000ULL;

// ============================================================================
// GValue — 8-byte tagged value
// ============================================================================

struct GValue {
    uint64_t bits;

    // --- Constructors (inline, zero-cost) ---

    static GValue makeInt(int32_t i) {
        GValue v;
        v.bits = QNAN | TAG_INT | (uint64_t)(uint32_t)i;
        return v;
    }

    static GValue makeDouble(double d) {
        GValue v;
        std::memcpy(&v.bits, &d, sizeof(double));
        return v;
    }

    static GValue makeBool(bool b) {
        GValue v;
        v.bits = QNAN | TAG_BOOL | (uint64_t)b;
        return v;
    }

    static GValue makeNull() {
        GValue v;
        v.bits = QNAN | TAG_NULL;
        return v;
    }

    static GValue makeUndefined() {
        GValue v;
        v.bits = QNAN | TAG_UNDEF;
        return v;
    }

    static GValue makeChar(uint32_t codepoint) {
        GValue v;
        v.bits = QNAN | TAG_CHAR | (uint64_t)codepoint;
        return v;
    }

    static GValue makePtr(void* p) {
        GValue v;
        v.bits = QNAN | SIGN_BIT | ((uint64_t)(uintptr_t)p & PAYLOAD);
        return v;
    }

    // --- Type checks (single comparison each) ---

    bool isDouble() const {
        return (bits & QNAN) != QNAN;
    }

    bool isInt() const {
        return (bits & (QNAN | TAG_MASK)) == (QNAN | TAG_INT);
    }

    bool isBool() const {
        return (bits & (QNAN | TAG_MASK)) == (QNAN | TAG_BOOL);
    }

    bool isNull() const {
        return bits == (QNAN | TAG_NULL);
    }

    bool isUndefined() const {
        return bits == (QNAN | TAG_UNDEF);
    }

    bool isChar() const {
        return (bits & (QNAN | TAG_MASK)) == (QNAN | TAG_CHAR);
    }

    bool isPtr() const {
        return (bits & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT);
    }

    bool isNumber() const {
        return isDouble() || isInt();
    }

    bool isFalsy() const {
        if (isNull() || isUndefined()) return true;
        if (isBool()) return !asBool();
        if (isInt()) return asInt() == 0;
        if (isDouble()) return asDouble() == 0.0;
        return false;
    }

    // --- Extractors (unchecked — caller verifies type) ---

    double asDouble() const {
        double d;
        std::memcpy(&d, &bits, sizeof(double));
        return d;
    }

    int32_t asInt() const {
        return (int32_t)(uint32_t)(bits & 0xFFFFFFFF);
    }

    bool asBool() const {
        return (bits & 1) != 0;
    }

    uint32_t asChar() const {
        return (uint32_t)(bits & PAYLOAD);
    }

    void* asPtr() const {
        return (void*)(uintptr_t)(bits & PAYLOAD);
    }

    template<typename T>
    T* as() const {
        return static_cast<T*>(asPtr());
    }

    // --- Numeric coercion ---

    double toNumber() const {
        if (isDouble()) return asDouble();
        if (isInt()) return (double)asInt();
        return 0.0;
    }

    int32_t toInt32() const {
        if (isInt()) return asInt();
        if (isDouble()) return (int32_t)asDouble();
        return 0;
    }

    // --- Equality ---

    bool operator==(GValue other) const { return bits == other.bits; }
    bool operator!=(GValue other) const { return bits != other.bits; }

    bool strictEquals(GValue other) const {
        // NaN != NaN (IEEE 754) — check before bitwise comparison
        if (isDouble() && other.isDouble()) {
            return asDouble() == other.asDouble();
        }
        return bits == other.bits;
    }
};

static_assert(sizeof(GValue) == 8, "GValue must be exactly 8 bytes");

// ============================================================================
// Compile-time constants
// ============================================================================

static constexpr GValue GVAL_NULL  = { QNAN | TAG_NULL };
static constexpr GValue GVAL_UNDEF = { QNAN | TAG_UNDEF };
static constexpr GValue GVAL_TRUE  = { QNAN | TAG_BOOL | 1 };
static constexpr GValue GVAL_FALSE = { QNAN | TAG_BOOL | 0 };
static constexpr GValue GVAL_ZERO  = { QNAN | TAG_INT | 0 };
static constexpr GValue GVAL_ONE   = { QNAN | TAG_INT | 1 };

} // namespace gardvm
