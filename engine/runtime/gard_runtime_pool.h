// Gard Connection Pool Runtime — Production-grade connection pooling
// Features: min/max bounds, health checks, idle eviction, acquire timeout,
//           wait queue (condition variable), statistics, thread-safe
// No external dependencies beyond pthreads and gard_runtime_db.

#ifndef GARD_RUNTIME_POOL_H
#define GARD_RUNTIME_POOL_H

#include "gard_runtime_db.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GardConnectionPool GardConnectionPool;

// Pool statistics
typedef struct {
    int32_t active;
    int32_t idle;
    int32_t total;
    int32_t min_size;
    int32_t max_size;
    int32_t total_created;
    int32_t total_destroyed;
    int32_t total_acquired;
    int32_t total_released;
    int32_t total_timeouts;
    int32_t total_health_failures;
} GardPoolStats;

// ConnectionPool.create(connectionString, minSize, maxSize, acquireTimeoutMs)
GardConnectionPool* gard_pool_create(const char* connection_string, int32_t min_size, int32_t max_size, int32_t acquire_timeout_ms);

// ConnectionPool.acquire(pool) — get a connection (blocks up to timeout)
GardDb* gard_pool_acquire(GardConnectionPool* pool);

// ConnectionPool.release(pool, db) — return connection to pool
int32_t gard_pool_release(GardConnectionPool* pool, GardDb* db);

// ConnectionPool.destroy(pool) — close all connections
void gard_pool_destroy(GardConnectionPool* pool);

// ConnectionPool.stats(pool) — get pool statistics
GardPoolStats gard_pool_stats(GardConnectionPool* pool);

// ConnectionPool.evict(pool) — remove idle/unhealthy connections
int32_t gard_pool_evict(GardConnectionPool* pool);

// ConnectionPool.resize(pool, newMin, newMax)
int32_t gard_pool_resize(GardConnectionPool* pool, int32_t new_min, int32_t new_max);

// ConnectionPool.isHealthy(pool) — check if pool has available connections
int32_t gard_pool_is_healthy(GardConnectionPool* pool);

#ifdef __cplusplus
}
#endif

#endif // GARD_RUNTIME_POOL_H
