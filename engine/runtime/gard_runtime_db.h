// Gard Database Runtime — C implementation
// Supports: SQLite, MySQL, PostgreSQL, MongoDB
// Compile: gcc -c -O2 -fPIC gard_runtime_db.c -o gard_runtime_db.o $(pkg-config --cflags libmongoc-1.0 2>/dev/null)
// Link: -lsqlite3 -lmysqlclient -lpq -lmongoc-1.0 -lbson-1.0

#ifndef GARD_RUNTIME_DB_H
#define GARD_RUNTIME_DB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque database connection handle
typedef struct GardDb GardDb;

// Query result for execute (INSERT/UPDATE/DELETE)
typedef struct {
    int32_t changes;
    int32_t last_insert_id;
} GardDbResult;

// Row from a query result
typedef struct GardDbRow {
    char** col_names;
    char** col_values;
    int32_t col_count;
} GardDbRow;

// Query result set
typedef struct {
    GardDbRow* rows;
    int32_t row_count;
    int32_t col_count;
} GardDbResultSet;

// === Database.connect / Database.close ===
GardDb* gard_db_connect(const char* connection_string);
void gard_db_close(GardDb* db);
int32_t gard_db_is_connected(GardDb* db);
const char* gard_db_get_driver(GardDb* db);
int32_t gard_db_get_query_count(GardDb* db);

// === Database.execute ===
GardDbResult gard_db_execute(GardDb* db, const char* sql);

// === Database.query ===
GardDbResultSet* gard_db_query(GardDb* db, const char* sql);
void gard_db_result_free(GardDbResultSet* rs);

// === Transactions ===
int32_t gard_db_begin_transaction(GardDb* db);
int32_t gard_db_commit(GardDb* db);
int32_t gard_db_rollback(GardDb* db);

#ifdef __cplusplus
}
#endif

#endif // GARD_RUNTIME_DB_H
