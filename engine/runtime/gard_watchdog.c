// Gard Watchdog — Runtime Recovery & Monitoring (Tier 8)
// Compile: gcc -c -O2 gard_watchdog.c -o gard_watchdog.o
// Archive: ar rcs libgard_watchdog.a gard_watchdog.o

#define _GNU_SOURCE
#include "gard_watchdog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

// ============================================================
// ANSI color codes (matching gard_runtime.c style)
// ============================================================

#define WD_RED     "\033[31m"
#define WD_YELLOW  "\033[33m"
#define WD_CYAN    "\033[36m"
#define WD_GRAY    "\033[90m"
#define WD_BOLD    "\033[1m"
#define WD_RESET   "\033[0m"

// ============================================================
// Constants & Defaults
// ============================================================

#define WD_CHECK_INTERVAL_MS   1000        // Watchdog checks every 1 second
#define WD_DEFAULT_MEMORY_WARN (512LL * 1024 * 1024)   // 512 MB
#define WD_DEFAULT_MEMORY_CRIT (1024LL * 1024 * 1024)  // 1 GB
#define WD_DEFAULT_DEADLOCK_MS 30000       // 30 seconds
#define WD_DEFAULT_HEARTBEAT_MS 30000      // 30 seconds
#define WD_MAX_TRACKED_MUTEXES 256         // Max concurrently tracked mutexes

// ============================================================
// Internal: Time utilities
// ============================================================

static int64_t wd_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

// ============================================================
// Tracked Memory (atomic counters)
// ============================================================

// We store the allocation size in a header before the user pointer.
typedef struct {
    size_t size;
} TrackedHeader;

static volatile int64_t g_tracked_bytes = 0;

void* gard_tracked_malloc(size_t size) {
    if (size == 0) return NULL;

    TrackedHeader* hdr = (TrackedHeader*)malloc(sizeof(TrackedHeader) + size);
    if (!hdr) return NULL;

    hdr->size = size;
    __atomic_add_fetch(&g_tracked_bytes, (int64_t)size, __ATOMIC_SEQ_CST);

    return (void*)(hdr + 1);
}

void gard_tracked_free(void* ptr) {
    if (!ptr) return;

    TrackedHeader* hdr = ((TrackedHeader*)ptr) - 1;
    int64_t size = (int64_t)hdr->size;
    __atomic_sub_fetch(&g_tracked_bytes, size, __ATOMIC_SEQ_CST);

    free(hdr);
}

int64_t gard_memory_usage(void) {
    return __atomic_load_n(&g_tracked_bytes, __ATOMIC_SEQ_CST);
}

// ============================================================
// Mutex Tracking (for deadlock detection)
// ============================================================

typedef struct {
    int64_t mutex_id;
    int64_t thread_id;
    int64_t acquired_at_ms;  // timestamp when acquired
    int32_t active;          // 1 = held, 0 = free slot
} MutexRecord;

static MutexRecord g_mutex_records[WD_MAX_TRACKED_MUTEXES];
static pthread_mutex_t g_mutex_records_lock = PTHREAD_MUTEX_INITIALIZER;

void gard_watchdog_mutex_acquired(int64_t thread_id, int64_t mutex_id) {
    pthread_mutex_lock(&g_mutex_records_lock);

    // Find a free slot or reuse one with the same mutex_id
    int free_slot = -1;
    for (int i = 0; i < WD_MAX_TRACKED_MUTEXES; i++) {
        if (g_mutex_records[i].active && g_mutex_records[i].mutex_id == mutex_id) {
            // Already tracked — update (shouldn't happen normally, but handle gracefully)
            g_mutex_records[i].thread_id = thread_id;
            g_mutex_records[i].acquired_at_ms = wd_time_ms();
            pthread_mutex_unlock(&g_mutex_records_lock);
            return;
        }
        if (!g_mutex_records[i].active && free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot >= 0) {
        g_mutex_records[free_slot].mutex_id = mutex_id;
        g_mutex_records[free_slot].thread_id = thread_id;
        g_mutex_records[free_slot].acquired_at_ms = wd_time_ms();
        g_mutex_records[free_slot].active = 1;
    }
    // If no free slot, silently drop — non-critical

    pthread_mutex_unlock(&g_mutex_records_lock);
}

void gard_watchdog_mutex_released(int64_t mutex_id) {
    pthread_mutex_lock(&g_mutex_records_lock);

    for (int i = 0; i < WD_MAX_TRACKED_MUTEXES; i++) {
        if (g_mutex_records[i].active && g_mutex_records[i].mutex_id == mutex_id) {
            g_mutex_records[i].active = 0;
            break;
        }
    }

    pthread_mutex_unlock(&g_mutex_records_lock);
}

// ============================================================
// Watchdog State
// ============================================================

typedef struct {
    // Configuration (protected by state_lock)
    int64_t memory_warn_threshold;
    int64_t memory_crit_threshold;
    int32_t deadlock_timeout_ms;
    int32_t heartbeat_timeout_ms;

    // Pressure callback
    void (*pressure_callback)(int32_t level);

    // Heartbeat tracking
    volatile int64_t last_heartbeat_ms;

    // Thread management
    pthread_t thread;
    pthread_mutex_t state_lock;
    volatile int32_t running;
    volatile int32_t started;

    // Pressure state (to avoid spamming)
    int32_t last_pressure_level;
} WatchdogState;

static WatchdogState g_watchdog = {
    .memory_warn_threshold = WD_DEFAULT_MEMORY_WARN,
    .memory_crit_threshold = WD_DEFAULT_MEMORY_CRIT,
    .deadlock_timeout_ms = WD_DEFAULT_DEADLOCK_MS,
    .heartbeat_timeout_ms = WD_DEFAULT_HEARTBEAT_MS,
    .pressure_callback = NULL,
    .last_heartbeat_ms = 0,
    .state_lock = PTHREAD_MUTEX_INITIALIZER,
    .running = 0,
    .started = 0,
    .last_pressure_level = 0,
};

// ============================================================
// Warning Output (colored, matching gard_panic style)
// ============================================================

static void wd_warn(const char* category, const char* message) {
    fprintf(stderr, "\n");
    fprintf(stderr, WD_BOLD WD_YELLOW "── Gard Watchdog Warning ──────────────────" WD_RESET "\n");
    fprintf(stderr, " " WD_BOLD "Category:" WD_RESET " %s%s%s\n", WD_CYAN, category, WD_RESET);
    fprintf(stderr, " " WD_BOLD "Message:" WD_RESET "  %s\n", message);
    fprintf(stderr, WD_BOLD WD_YELLOW "───────────────────────────────────────────" WD_RESET "\n");
    fprintf(stderr, "\n");
}

static void wd_critical(const char* category, const char* message) {
    fprintf(stderr, "\n");
    fprintf(stderr, WD_BOLD WD_RED "══ Gard Watchdog CRITICAL ═════════════════" WD_RESET "\n");
    fprintf(stderr, " " WD_BOLD "Category:" WD_RESET " %s%s%s\n", WD_CYAN, category, WD_RESET);
    fprintf(stderr, " " WD_BOLD "Message:" WD_RESET "  %s\n", message);
    fprintf(stderr, WD_BOLD WD_RED "════════════════════════════════════════════" WD_RESET "\n");
    fprintf(stderr, "\n");
}

// ============================================================
// Watchdog Checks
// ============================================================

static void wd_check_deadlocks(int32_t timeout_ms) {
    int64_t now = wd_time_ms();

    pthread_mutex_lock(&g_mutex_records_lock);

    for (int i = 0; i < WD_MAX_TRACKED_MUTEXES; i++) {
        if (g_mutex_records[i].active) {
            int64_t held_ms = now - g_mutex_records[i].acquired_at_ms;
            if (held_ms > (int64_t)timeout_ms) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "Mutex 0x%llx held by thread %lld for %lld ms (threshold: %d ms)",
                    (unsigned long long)g_mutex_records[i].mutex_id,
                    (long long)g_mutex_records[i].thread_id,
                    (long long)held_ms,
                    timeout_ms);
                // Unlock before printing to avoid holding the lock during I/O
                pthread_mutex_unlock(&g_mutex_records_lock);
                wd_warn("Potential Deadlock", msg);
                return; // Only report one per cycle to avoid spam
            }
        }
    }

    pthread_mutex_unlock(&g_mutex_records_lock);
}

static void wd_check_memory_pressure(int64_t warn_threshold, int64_t crit_threshold,
                                      void (*callback)(int32_t), int32_t* last_level) {
    int64_t usage = gard_memory_usage();
    int32_t level = 0;

    if (usage >= crit_threshold) {
        level = 2;
    } else if (usage >= warn_threshold) {
        level = 1;
    }

    // Only fire when level changes or escalates
    if (level > 0 && level != *last_level) {
        *last_level = level;

        char msg[256];
        snprintf(msg, sizeof(msg),
            "Tracked memory: %lld MB (warning: %lld MB, critical: %lld MB)",
            (long long)(usage / (1024 * 1024)),
            (long long)(warn_threshold / (1024 * 1024)),
            (long long)(crit_threshold / (1024 * 1024)));

        if (level == 2) {
            wd_critical("Memory Pressure", msg);
        } else {
            wd_warn("Memory Pressure", msg);
        }

        // Invoke user callback if registered
        if (callback) {
            callback(level);
        }
    } else if (level == 0 && *last_level != 0) {
        // Memory dropped below warning — reset
        *last_level = 0;
    }
}

static void wd_check_heartbeat(int64_t last_hb, int32_t timeout_ms) {
    if (last_hb == 0) {
        // No heartbeat ever sent — skip (watchdog started but event loop not yet active)
        return;
    }

    int64_t now = wd_time_ms();
    int64_t elapsed = now - last_hb;

    if (elapsed > (int64_t)timeout_ms) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "No heartbeat received for %lld ms (threshold: %d ms) — scheduler stall detected",
            (long long)elapsed, timeout_ms);
        wd_warn("Scheduler Stall", msg);
    }
}

// ============================================================
// Watchdog Thread
// ============================================================

static void* wd_thread_func(void* arg) {
    (void)arg;

    while (__atomic_load_n(&g_watchdog.running, __ATOMIC_SEQ_CST)) {
        // Read configuration under lock
        pthread_mutex_lock(&g_watchdog.state_lock);
        int64_t warn_thresh = g_watchdog.memory_warn_threshold;
        int64_t crit_thresh = g_watchdog.memory_crit_threshold;
        int32_t deadlock_ms = g_watchdog.deadlock_timeout_ms;
        int32_t heartbeat_ms = g_watchdog.heartbeat_timeout_ms;
        void (*callback)(int32_t) = g_watchdog.pressure_callback;
        int64_t last_hb = g_watchdog.last_heartbeat_ms;
        pthread_mutex_unlock(&g_watchdog.state_lock);

        // Perform checks
        wd_check_deadlocks(deadlock_ms);
        wd_check_memory_pressure(warn_thresh, crit_thresh, callback, &g_watchdog.last_pressure_level);
        wd_check_heartbeat(last_hb, heartbeat_ms);

        // Sleep for the check interval (1 second)
        struct timespec ts;
        ts.tv_sec = WD_CHECK_INTERVAL_MS / 1000;
        ts.tv_nsec = (WD_CHECK_INTERVAL_MS % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }

    return NULL;
}

// ============================================================
// Public API
// ============================================================

void gard_watchdog_start(void) {
    // Prevent double-start
    if (__atomic_load_n(&g_watchdog.started, __ATOMIC_SEQ_CST)) {
        return;
    }

    __atomic_store_n(&g_watchdog.running, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_watchdog.started, 1, __ATOMIC_SEQ_CST);

    // Initialize mutex records
    memset(g_mutex_records, 0, sizeof(g_mutex_records));

    // Reset pressure state
    g_watchdog.last_pressure_level = 0;

    // Set initial heartbeat time so we don't immediately trigger a stall
    // (heartbeat check is skipped if last_heartbeat_ms == 0)

    int rc = pthread_create(&g_watchdog.thread, NULL, wd_thread_func, NULL);
    if (rc != 0) {
        fprintf(stderr, WD_BOLD WD_RED "[Gard Watchdog] Failed to start watchdog thread (rc=%d)" WD_RESET "\n", rc);
        __atomic_store_n(&g_watchdog.running, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_watchdog.started, 0, __ATOMIC_SEQ_CST);
    }
}

void gard_watchdog_stop(void) {
    if (!__atomic_load_n(&g_watchdog.started, __ATOMIC_SEQ_CST)) {
        return;
    }

    __atomic_store_n(&g_watchdog.running, 0, __ATOMIC_SEQ_CST);
    pthread_join(g_watchdog.thread, NULL);
    __atomic_store_n(&g_watchdog.started, 0, __ATOMIC_SEQ_CST);
}

void gard_watchdog_heartbeat(void) {
    int64_t now = wd_time_ms();
    pthread_mutex_lock(&g_watchdog.state_lock);
    g_watchdog.last_heartbeat_ms = now;
    pthread_mutex_unlock(&g_watchdog.state_lock);
}

void gard_watchdog_set_memory_limit(int64_t bytes) {
    pthread_mutex_lock(&g_watchdog.state_lock);
    g_watchdog.memory_warn_threshold = bytes;
    pthread_mutex_unlock(&g_watchdog.state_lock);
}

void gard_watchdog_set_memory_critical(int64_t bytes) {
    pthread_mutex_lock(&g_watchdog.state_lock);
    g_watchdog.memory_crit_threshold = bytes;
    pthread_mutex_unlock(&g_watchdog.state_lock);
}

void gard_watchdog_set_deadlock_timeout(int32_t ms) {
    pthread_mutex_lock(&g_watchdog.state_lock);
    g_watchdog.deadlock_timeout_ms = ms;
    pthread_mutex_unlock(&g_watchdog.state_lock);
}

void gard_watchdog_set_heartbeat_timeout(int32_t ms) {
    pthread_mutex_lock(&g_watchdog.state_lock);
    g_watchdog.heartbeat_timeout_ms = ms;
    pthread_mutex_unlock(&g_watchdog.state_lock);
}

void gard_watchdog_on_pressure(void (*callback)(int32_t level)) {
    pthread_mutex_lock(&g_watchdog.state_lock);
    g_watchdog.pressure_callback = callback;
    pthread_mutex_unlock(&g_watchdog.state_lock);
}
