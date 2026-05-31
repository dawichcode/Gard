// GARDVM Tier 1 — Value Test
// Validates all NaN-boxing roundtrips, type checks, and edge cases.
// Compile: g++ -std=c++17 -O2 -o value_test value_test.cpp && ./value_test

#include "value.h"
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cmath>
#include <chrono>

using namespace gardvm;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(expr) do { \
    if (expr) { tests_passed++; } \
    else { tests_failed++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); } \
} while(0)

void test_int() {
    TEST(GValue::makeInt(0).isInt());
    TEST(GValue::makeInt(0).asInt() == 0);
    TEST(GValue::makeInt(42).asInt() == 42);
    TEST(GValue::makeInt(-1).asInt() == -1);
    TEST(GValue::makeInt(2147483647).asInt() == 2147483647);   // INT32_MAX
    TEST(GValue::makeInt(-2147483648).asInt() == -2147483648); // INT32_MIN
    TEST(GValue::makeInt(1).isInt());
    TEST(!GValue::makeInt(1).isDouble());
    TEST(!GValue::makeInt(1).isBool());
    TEST(!GValue::makeInt(1).isNull());
    TEST(!GValue::makeInt(1).isPtr());
    TEST(!GValue::makeInt(1).isChar());
    // Falsy
    TEST(GValue::makeInt(0).isFalsy());
    TEST(!GValue::makeInt(1).isFalsy());
    TEST(!GValue::makeInt(-1).isFalsy());
}

void test_double() {
    TEST(GValue::makeDouble(3.14).isDouble());
    TEST(GValue::makeDouble(3.14).asDouble() == 3.14);
    TEST(GValue::makeDouble(0.0).isDouble());
    TEST(GValue::makeDouble(0.0).asDouble() == 0.0);
    TEST(GValue::makeDouble(-1.5).asDouble() == -1.5);
    TEST(GValue::makeDouble(1e308).asDouble() == 1e308);
    TEST(GValue::makeDouble(-1e308).asDouble() == -1e308);
    TEST(GValue::makeDouble(5e-324).asDouble() == 5e-324); // smallest positive
    TEST(!GValue::makeDouble(1.0).isInt());
    TEST(!GValue::makeDouble(1.0).isBool());
    TEST(!GValue::makeDouble(1.0).isNull());
    TEST(!GValue::makeDouble(1.0).isPtr());
    // Falsy
    TEST(GValue::makeDouble(0.0).isFalsy());
    TEST(!GValue::makeDouble(0.1).isFalsy());
    // Infinity
    TEST(GValue::makeDouble(INFINITY).isDouble());
    TEST(GValue::makeDouble(INFINITY).asDouble() == INFINITY);
    TEST(GValue::makeDouble(-INFINITY).asDouble() == -INFINITY);
}

void test_bool() {
    TEST(GValue::makeBool(true).isBool());
    TEST(GValue::makeBool(true).asBool() == true);
    TEST(GValue::makeBool(false).isBool());
    TEST(GValue::makeBool(false).asBool() == false);
    TEST(!GValue::makeBool(true).isInt());
    TEST(!GValue::makeBool(true).isDouble());
    TEST(!GValue::makeBool(true).isNull());
    // Falsy
    TEST(GValue::makeBool(false).isFalsy());
    TEST(!GValue::makeBool(true).isFalsy());
    // Constants
    TEST(GVAL_TRUE.asBool() == true);
    TEST(GVAL_FALSE.asBool() == false);
    TEST(GVAL_TRUE.isBool());
    TEST(GVAL_FALSE.isBool());
}

void test_null() {
    TEST(GValue::makeNull().isNull());
    TEST(!GValue::makeNull().isInt());
    TEST(!GValue::makeNull().isDouble());
    TEST(!GValue::makeNull().isBool());
    TEST(!GValue::makeNull().isPtr());
    TEST(GValue::makeNull().isFalsy());
    TEST(GVAL_NULL.isNull());
    TEST(GVAL_NULL.isFalsy());
}

void test_undefined() {
    TEST(GValue::makeUndefined().isUndefined());
    TEST(GValue::makeUndefined().isFalsy());
    TEST(!GValue::makeUndefined().isNull());
    TEST(GVAL_UNDEF.isUndefined());
}

void test_char() {
    TEST(GValue::makeChar('A').isChar());
    TEST(GValue::makeChar('A').asChar() == 65);
    TEST(GValue::makeChar(0).asChar() == 0);
    TEST(GValue::makeChar(0x1F600).asChar() == 0x1F600); // emoji codepoint
    TEST(!GValue::makeChar('x').isInt());
    TEST(!GValue::makeChar('x').isPtr());
}

void test_pointer() {
    int x = 42;
    void* ptr = &x;
    GValue v = GValue::makePtr(ptr);
    TEST(v.isPtr());
    TEST(v.asPtr() == ptr);
    TEST(!v.isInt());
    TEST(!v.isDouble());
    TEST(!v.isNull());
    TEST(!v.isBool());
    // Null pointer
    GValue np = GValue::makePtr(nullptr);
    TEST(np.isPtr());
    TEST(np.asPtr() == nullptr);
    // Typed extraction
    TEST(v.as<int>() == &x);
}

void test_equality() {
    TEST(GValue::makeInt(5) == GValue::makeInt(5));
    TEST(GValue::makeInt(5) != GValue::makeInt(6));
    TEST(GValue::makeDouble(1.0) == GValue::makeDouble(1.0));
    TEST(GValue::makeDouble(1.0) != GValue::makeDouble(2.0));
    TEST(GValue::makeBool(true) == GValue::makeBool(true));
    TEST(GValue::makeBool(true) != GValue::makeBool(false));
    TEST(GValue::makeNull() == GValue::makeNull());
    TEST(GValue::makeNull() != GValue::makeInt(0));
    TEST(GValue::makeInt(0) != GValue::makeDouble(0.0));
    TEST(GValue::makeInt(0) != GValue::makeBool(false));
    // Constants
    TEST(GVAL_ZERO == GValue::makeInt(0));
    TEST(GVAL_ONE == GValue::makeInt(1));
    TEST(GVAL_NULL == GValue::makeNull());
}

void test_coercion() {
    TEST(GValue::makeInt(42).toNumber() == 42.0);
    TEST(GValue::makeDouble(3.14).toNumber() == 3.14);
    TEST(GValue::makeNull().toNumber() == 0.0);
    TEST(GValue::makeInt(42).toInt32() == 42);
    TEST(GValue::makeDouble(3.7).toInt32() == 3);
    TEST(GValue::makeDouble(-2.9).toInt32() == -2);
}

void test_strict_equals() {
    TEST(GValue::makeInt(1).strictEquals(GValue::makeInt(1)));
    TEST(!GValue::makeInt(1).strictEquals(GValue::makeInt(2)));
    TEST(GValue::makeDouble(1.0).strictEquals(GValue::makeDouble(1.0)));
    // NaN != NaN
    GValue nan1 = GValue::makeDouble(NAN);
    GValue nan2 = GValue::makeDouble(NAN);
    TEST(!nan1.strictEquals(nan2));
}

void test_isNumber() {
    TEST(GValue::makeInt(1).isNumber());
    TEST(GValue::makeDouble(1.0).isNumber());
    TEST(!GValue::makeBool(true).isNumber());
    TEST(!GValue::makeNull().isNumber());
    TEST(!GValue::makePtr(nullptr).isNumber());
}

void benchmark() {
    auto start = std::chrono::high_resolution_clock::now();
    volatile int32_t sink = 0;
    for (int64_t i = 0; i < 1000000000LL; i++) {
        GValue v = GValue::makeInt((int32_t)i);
        sink = v.asInt();
    }
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    printf("Benchmark: 1B makeInt+asInt roundtrips in %.1f ms (%.2f ns/op)\n",
           ms, ms * 1e6 / 1e9);
    if (ms < 1000.0) {
        printf("  PASS (< 1 second)\n");
    } else {
        printf("  FAIL (> 1 second)\n");
    }
    (void)sink;
}

int main() {
    test_int();
    test_double();
    test_bool();
    test_null();
    test_undefined();
    test_char();
    test_pointer();
    test_equality();
    test_coercion();
    test_strict_equals();
    test_isNumber();

    printf("\n=== Tier 1 Value Tests ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    if (tests_failed > 0) {
        printf("\nFAILED\n");
        return 1;
    }
    printf("\nALL PASSED\n\n");

    benchmark();
    return 0;
}
