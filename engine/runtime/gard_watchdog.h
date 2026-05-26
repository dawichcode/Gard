// Gard Watchdog — Runtime Recovery & Monitoring (Tier 8)
// Provides deadlock detection, memory pressure monitoring, and scheduler stall detection.
// Compile: gcc -c -O2 gard_watchdog.c -o gard_watchdog.o
// Archive: ar rcs libgard_watchdog.a gard_watchdog.o

#ifndef GARD_WATCHDOG_H
#define GARD_WATCHDOG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Watchdog Control
// ============================================================

// Start the watchdog background thread.
// The watchdog periodically checks for deadlocks, memory pressure, and scheduler stalls.
// Safe to call multiple times — subsequent calls are no-ops if already running.
void gard_watchdog_start(void);

// Stop the watchdog background thread.
// Blocks until the watchdog thread has exited.
void gard_watchdog_stop(void);

// Signal liveness from the main event loop / scheduler.
// If no heartbeat is received within 30s (default), the watchdog logs a stall warning.
void gard_watchdog_heartbeat(void);

// ============================================================
// Configuration
// ============================================================

// Set the memory warning threshold in bytes (default: 512MB).
// When tracked allocations exceed this, a level-1 (warning) pressure event fires.
void gard_watchdog_set_memory_limit(int64_t bytes);

// Set the critical memory threshold in bytes (default: 1GB).
// When tracked allocations exceed this, a level-2 (critical) pressure event fires.
void gard_watchdog_set_memory_critical(int64_t bytes);

// Set the deadlock detection timeout in milliseconds (default: 30000ms).
// If a mutex is held longer than this, a deadlock warning is emitted.
void gard_watchdog_set_deadlock_timeout(int32_t ms);

// Set the heartbeat stall timeout in milliseconds (default: 30000ms).
// If no heartbeat is received within this period, a scheduler stall is reported.
void gard_watchdog_set_heartbeat_timeout(int32_t ms);

// Register a callback for memory pressure events.
// level: 1 = warning threshold exceeded, 2 = critical threshold exceeded
// The callback is invoked from the watchdog thread.
void gard_watchdog_on_pressure(void (*callback)(int32_t level));

// ============================================================
// Tracked Memory Allocation
// ============================================================

// Allocate memory with tracking. Updates the global allocation counter atomically.
void* gard_tracked_malloc(size_t size);

// Free tracked memory. Decrements the global allocation counter atomically.
// ptr must have been allocated with gard_tracked_malloc.
void gard_tracked_free(void* ptr);

// Query the current total tracked memory usage in bytes.
int64_t gard_memory_usage(void);

// ============================================================
// Mutex Monitoring (for deadlock detection)
// ============================================================

// Register a mutex acquisition (called when a mutex is locked).
// thread_id: identifier for the holding thread
// mutex_id: opaque identifier for the mutex (e.g., pointer cast to int64_t)
void gard_watchdog_mutex_acquired(int64_t thread_id, int64_t mutex_id);

// Register a mutex release (called when a mutex is unlocked).
void gard_watchdog_mutex_released(int64_t mutex_id);

#ifdef __cplusplus
}
#endif

#endif // GARD_WATCHDOG_H
