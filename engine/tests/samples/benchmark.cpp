#include <cstdio>
#include <cstdint>
#include <chrono>
#include <vector>
#include <stdexcept>

using namespace std::chrono;

int64_t hrtime() {
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// --- Fibonacci recursive ---
int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

// --- Fibonacci iterative ---
int fibIter(int n) {
    int a = 0, b = 1;
    for (int i = 0; i < n; i++) {
        int temp = a + b;
        a = b;
        b = temp;
    }
    return a;
}

// --- Arithmetic ---
int64_t benchArithmetic(int n) {
    int64_t sum = 0;
    for (int i = 0; i < n; i++) {
        sum = sum + i * 2 - 1;
        sum = sum / 1 + i % 7;
    }
    return sum;
}

// --- Array operations ---
int64_t benchArray(int n) {
    std::vector<int64_t> arr(n);
    for (int i = 0; i < n; i++) {
        arr[i] = (int64_t)i * i;
    }
    int64_t sum = 0;
    for (int i = 0; i < n; i++) {
        sum += arr[i];
    }
    return sum;
}

// --- Nested loops ---
int64_t benchNestedLoops(int n) {
    int64_t sum = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            sum++;
        }
    }
    return sum;
}

// --- Bubble sort ---
int bubbleSort(int n) {
    std::vector<int> arr(n);
    for (int i = 0; i < n; i++) arr[i] = n - i;
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                int temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
    }
    return arr[0];
}

// --- Sieve ---
int sieve(int n) {
    std::vector<int> isPrime(n, 1);
    isPrime[0] = isPrime[1] = 0;
    for (int i = 2; i * i < n; i++) {
        if (isPrime[i]) {
            for (int j = i * i; j < n; j += i) isPrime[j] = 0;
        }
    }
    int count = 0;
    for (int i = 0; i < n; i++) if (isPrime[i]) count++;
    return count;
}

// --- Exceptions ---
int benchExceptions(int n) {
    int caught = 0;
    for (int i = 0; i < n; i++) {
        try {
            if (i % 10 == 0) throw std::runtime_error("test");
        } catch (...) {
            caught++;
        }
    }
    return caught;
}

int main() {
    printf("========================================\n");
    printf("  C++ Native Benchmark Suite\n");
    printf("========================================\n");

    auto totalStart = hrtime();

    auto t0 = hrtime();
    auto r1 = benchArithmetic(1000000000);
    auto t1 = hrtime();
    printf("1. Arithmetic (1B)       : %ld ms\n", (t1-t0)/1000000);

    auto t2 = hrtime();
    auto r2 = fib(35);
    auto t3 = hrtime();
    printf("2. Fib recursive(35)       : %ld ms | result: %d\n", (t3-t2)/1000000, r2);

    auto t4 = hrtime();
    auto r3 = fibIter(100000);
    auto t5 = hrtime();
    printf("3. Fib iterative(100k)     : %ld ms\n", (t5-t4)/1000000);

    auto t6 = hrtime();
    auto r4 = benchArray(100000);
    auto t7 = hrtime();
    printf("4. Array ops (100K)         : %ld ms\n", (t7-t6)/1000000);

    auto t8 = hrtime();
    auto r5 = benchNestedLoops(5000);
    auto t9 = hrtime();
    printf("5. Nested loops (5000x5000)  : %ld ms | ops: %ld\n", (t9-t8)/1000000, r5);

    auto t10 = hrtime();
    auto r6 = bubbleSort(500);
    auto t11 = hrtime();
    printf("6. Bubble sort (500)        : %ld ms\n", (t11-t10)/1000000);

    auto t12 = hrtime();
    auto r7 = sieve(100000);
    auto t13 = hrtime();
    printf("7. Sieve primes (100K)       : %ld ms | primes: %d\n", (t13-t12)/1000000, r7);

    auto t14 = hrtime();
    auto r8 = benchExceptions(1000);
    auto t15 = hrtime();
    printf("8. Exceptions (1k)         : %ld ms | caught: %d\n", (t15-t14)/1000000, r8);

    auto totalEnd = hrtime();
    printf("========================================\n");
    printf("  Total: %ld ms\n", (totalEnd-totalStart)/1000000);
    printf("========================================\n");
    return 0;
}
