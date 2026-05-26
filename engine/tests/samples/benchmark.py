import time

def hrtime():
    return time.perf_counter_ns()

# --- Fibonacci recursive ---
def fib(n):
    if n <= 1: return n
    return fib(n - 1) + fib(n - 2)

# --- Fibonacci iterative ---
def fibIter(n):
    a, b = 0, 1
    for _ in range(n):
        a, b = b, a + b
    return a

# --- Arithmetic ---
def benchArithmetic(n):
    s = 0
    for i in range(n):
        s = s + i * 2 - 1
        s = s // 1 + i % 7
    return s

# --- Array operations ---
def benchArray(n):
    arr = [0] * n
    for i in range(n):
        arr[i] = i * i
    s = 0
    for i in range(n):
        s += arr[i]
    return s

# --- Nested loops ---
def benchNestedLoops(n):
    s = 0
    for i in range(n):
        for j in range(n):
            s += 1
    return s

# --- Bubble sort ---
def bubbleSort(n):
    arr = [n - i for i in range(n)]
    for i in range(n - 1):
        for j in range(n - i - 1):
            if arr[j] > arr[j + 1]:
                arr[j], arr[j + 1] = arr[j + 1], arr[j]
    return arr[0]

# --- Sieve ---
def sieve(n):
    isPrime = [1] * n
    isPrime[0] = isPrime[1] = 0
    i = 2
    while i * i < n:
        if isPrime[i]:
            j = i * i
            while j < n:
                isPrime[j] = 0
                j += i
        i += 1
    return sum(isPrime)

# --- Exceptions ---
def benchExceptions(n):
    caught = 0
    for i in range(n):
        try:
            if i % 10 == 0:
                raise RuntimeError("test")
        except:
            caught += 1
    return caught

if __name__ == "__main__":
    print("========================================")
    print("  Python Benchmark Suite")
    print("========================================")

    totalStart = hrtime()

    t0 = hrtime()
    r1 = benchArithmetic(10000000)
    t1 = hrtime()
    print(f"1. Arithmetic (10M)       : {(t1-t0)//1000000} ms")

    t2 = hrtime()
    r2 = fib(30)
    t3 = hrtime()
    print(f"2. Fib recursive(35)       : {(t3-t2)//1000000} ms | result: {r2}")

    t4 = hrtime()
    r3 = fibIter(100000)
    t5 = hrtime()
    print(f"3. Fib iterative(100k)     : {(t5-t4)//1000000} ms")

    t6 = hrtime()
    r4 = benchArray(100000)
    t7 = hrtime()
    print(f"4. Array ops (100K)         : {(t7-t6)//1000000} ms")

    t8 = hrtime()
    r5 = benchNestedLoops(5000)
    t9 = hrtime()
    print(f"5. Nested loops (5000x5000)  : {(t9-t8)//1000000} ms | ops: {r5}")

    t10 = hrtime()
    r6 = bubbleSort(500)
    t11 = hrtime()
    print(f"6. Bubble sort (500)        : {(t11-t10)//1000000} ms")

    t12 = hrtime()
    r7 = sieve(100000)
    t13 = hrtime()
    print(f"7. Sieve primes (100K)       : {(t13-t12)//1000000} ms | primes: {r7}")

    t14 = hrtime()
    r8 = benchExceptions(1000)
    t15 = hrtime()
    print(f"8. Exceptions (1k)         : {(t15-t14)//1000000} ms | caught: {r8}")

    totalEnd = hrtime()
    print("========================================")
    print(f"  Total: {(totalEnd-totalStart)//1000000} ms")
    print("========================================")
