// Gard Query Builder Runtime — Fluent SQL builder for AOT
// Builds driver-agnostic queries that compile to SQLite/MySQL/PostgreSQL SQL.
// No external dependencies.

#ifndef GARD_RUNTIME_QUERY_H
#define GARD_RUNTIME_QUERY_H

#include "gard_runtime_db.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GardQueryBuilder GardQueryBuilder;

// Query.table(name) — start a query builder
GardQueryBuilder* gard_query_table(const char* table_name);

// Query.select(qb, columns) — columns is comma-separated: "name, age"
GardQueryBuilder* gard_query_select(GardQueryBuilder* qb, const char* columns);

// Query.where(qb, column, op, value)
GardQueryBuilder* gard_query_where(GardQueryBuilder* qb, const char* column, const char* op, const char* value);

// Query.orWhere(qb, column, op, value)
GardQueryBuilder* gard_query_or_where(GardQueryBuilder* qb, const char* column, const char* op, const char* value);

// Query.orderBy(qb, column, direction)
GardQueryBuilder* gard_query_order_by(GardQueryBuilder* qb, const char* column, const char* direction);

// Query.limit(qb, count)
GardQueryBuilder* gard_query_limit(GardQueryBuilder* qb, int32_t count);

// Query.offset(qb, count)
GardQueryBuilder* gard_query_offset(GardQueryBuilder* qb, int32_t count);

// Query.join(qb, table, left_col, op, right_col)
GardQueryBuilder* gard_query_join(GardQueryBuilder* qb, const char* table, const char* left_col, const char* op, const char* right_col);

// Query.leftJoin(qb, table, left_col, op, right_col)
GardQueryBuilder* gard_query_left_join(GardQueryBuilder* qb, const char* table, const char* left_col, const char* op, const char* right_col);

// Query.groupBy(qb, columns)
GardQueryBuilder* gard_query_group_by(GardQueryBuilder* qb, const char* columns);

// Query.having(qb, condition)
GardQueryBuilder* gard_query_having(GardQueryBuilder* qb, const char* condition);

// Query.insert(qb) — set type to INSERT
GardQueryBuilder* gard_query_insert(GardQueryBuilder* qb);

// Query.update(qb) — set type to UPDATE
GardQueryBuilder* gard_query_update(GardQueryBuilder* qb);

// Query.deleteFrom(qb) — set type to DELETE
GardQueryBuilder* gard_query_delete_from(GardQueryBuilder* qb);

// Query.set(qb, column, value) — set column value for INSERT/UPDATE
GardQueryBuilder* gard_query_set(GardQueryBuilder* qb, const char* column, const char* value);

// Query.setInt(qb, column, value) — set integer column value
GardQueryBuilder* gard_query_set_int(GardQueryBuilder* qb, const char* column, int32_t value);

// Query.whereIn(qb, column, values) — values is comma-separated
GardQueryBuilder* gard_query_where_in(GardQueryBuilder* qb, const char* column, const char* values);

// Query.whereNull(qb, column)
GardQueryBuilder* gard_query_where_null(GardQueryBuilder* qb, const char* column);

// Query.whereNotNull(qb, column)
GardQueryBuilder* gard_query_where_not_null(GardQueryBuilder* qb, const char* column);

// Query.whereBetween(qb, column, min, max)
GardQueryBuilder* gard_query_where_between(GardQueryBuilder* qb, const char* column, const char* min_val, const char* max_val);

// Query.whereLike(qb, column, pattern)
GardQueryBuilder* gard_query_where_like(GardQueryBuilder* qb, const char* column, const char* pattern);

// Query.distinct(qb)
GardQueryBuilder* gard_query_distinct(GardQueryBuilder* qb);

// Query.count(qb, column) — SELECT COUNT(column)
GardQueryBuilder* gard_query_count(GardQueryBuilder* qb, const char* column);

// Query.sum(qb, column) — SELECT SUM(column)
GardQueryBuilder* gard_query_sum(GardQueryBuilder* qb, const char* column);

// Query.toSQL(qb) — compile to SQL string (default: sqlite dialect)
char* gard_query_to_sql(GardQueryBuilder* qb);

// Query.toSQLDialect(qb, driver) — compile to specific dialect
char* gard_query_to_sql_dialect(GardQueryBuilder* qb, const char* driver);

// Database.run(db, qb) — execute query builder against database
GardDbResultSet* gard_db_run_query(GardDb* db, GardQueryBuilder* qb);

// Free query builder
void gard_query_free(GardQueryBuilder* qb);

#ifdef __cplusplus
}
#endif

#endif // GARD_RUNTIME_QUERY_H
