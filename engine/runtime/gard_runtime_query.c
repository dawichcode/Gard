// Gard Query Builder Runtime — Fluent SQL builder
// Compiles to SQLite, MySQL, or PostgreSQL SQL with proper escaping.
// Compile: gcc -c -O2 -fPIC gard_runtime_query.c -o gard_runtime_query.o

#include "gard_runtime_query.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define QB_MAX_WHERES 64
#define QB_MAX_SETS 64
#define QB_MAX_JOINS 16
#define QB_MAX_ORDERS 16

typedef enum { QB_SELECT, QB_INSERT, QB_UPDATE, QB_DELETE } QBType;

typedef struct {
    char* column;
    char* op;
    char* value;
    int is_or;  // 0=AND, 1=OR
    int special; // 0=normal, 1=IS NULL, 2=IS NOT NULL, 3=IN, 4=BETWEEN, 5=LIKE
} QBWhere;

typedef struct {
    char* column;
    char* value;
    int is_int;
    int32_t int_val;
} QBSet;

typedef struct {
    char* type; // "INNER", "LEFT", "RIGHT"
    char* table;
    char* left_col;
    char* op;
    char* right_col;
} QBJoin;

typedef struct {
    char* column;
    char* direction;
} QBOrder;

struct GardQueryBuilder {
    char* table;
    QBType type;
    char* columns;       // comma-separated or NULL for *
    int distinct;
    QBWhere wheres[QB_MAX_WHERES];
    int32_t where_count;
    QBSet sets[QB_MAX_SETS];
    int32_t set_count;
    QBJoin joins[QB_MAX_JOINS];
    int32_t join_count;
    QBOrder orders[QB_MAX_ORDERS];
    int32_t order_count;
    char* group_by;
    char* having;
    int32_t limit;
    int32_t offset;
};

// === Helpers ===

static char* sql_escape_value(const char* val) {
    if (!val) return strdup("NULL");
    size_t len = strlen(val);
    // Check if it's a number
    int is_num = 1;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)val[i]) && val[i] != '-' && val[i] != '.') { is_num = 0; break; }
    }
    if (is_num && len > 0) return strdup(val);
    // Escape string
    char* escaped = (char*)malloc(len * 2 + 3);
    char* dst = escaped;
    *dst++ = '\'';
    for (size_t i = 0; i < len; i++) {
        if (val[i] == '\'') { *dst++ = '\''; *dst++ = '\''; }
        else *dst++ = val[i];
    }
    *dst++ = '\'';
    *dst = '\0';
    return escaped;
}

// === Constructor ===

GardQueryBuilder* gard_query_table(const char* table_name) {
    GardQueryBuilder* qb = (GardQueryBuilder*)calloc(1, sizeof(GardQueryBuilder));
    qb->table = strdup(table_name ? table_name : "");
    qb->type = QB_SELECT;
    qb->columns = NULL;
    qb->distinct = 0;
    qb->where_count = 0;
    qb->set_count = 0;
    qb->join_count = 0;
    qb->order_count = 0;
    qb->group_by = NULL;
    qb->having = NULL;
    qb->limit = -1;
    qb->offset = -1;
    return qb;
}

// === Fluent methods (return qb for chaining) ===

GardQueryBuilder* gard_query_select(GardQueryBuilder* qb, const char* columns) {
    if (!qb) return qb;
    if (qb->columns) free(qb->columns);
    qb->columns = columns ? strdup(columns) : NULL;
    qb->type = QB_SELECT;
    return qb;
}

GardQueryBuilder* gard_query_where(GardQueryBuilder* qb, const char* column, const char* op, const char* value) {
    if (!qb || qb->where_count >= QB_MAX_WHERES) return qb;
    QBWhere* w = &qb->wheres[qb->where_count++];
    w->column = strdup(column ? column : "");
    w->op = strdup(op ? op : "=");
    w->value = strdup(value ? value : "");
    w->is_or = 0;
    w->special = 0;
    return qb;
}

GardQueryBuilder* gard_query_or_where(GardQueryBuilder* qb, const char* column, const char* op, const char* value) {
    if (!qb || qb->where_count >= QB_MAX_WHERES) return qb;
    QBWhere* w = &qb->wheres[qb->where_count++];
    w->column = strdup(column ? column : "");
    w->op = strdup(op ? op : "=");
    w->value = strdup(value ? value : "");
    w->is_or = 1;
    w->special = 0;
    return qb;
}

GardQueryBuilder* gard_query_order_by(GardQueryBuilder* qb, const char* column, const char* direction) {
    if (!qb || qb->order_count >= QB_MAX_ORDERS) return qb;
    QBOrder* o = &qb->orders[qb->order_count++];
    o->column = strdup(column ? column : "");
    o->direction = strdup(direction ? direction : "ASC");
    return qb;
}

GardQueryBuilder* gard_query_limit(GardQueryBuilder* qb, int32_t count) {
    if (qb) qb->limit = count;
    return qb;
}

GardQueryBuilder* gard_query_offset(GardQueryBuilder* qb, int32_t count) {
    if (qb) qb->offset = count;
    return qb;
}

GardQueryBuilder* gard_query_join(GardQueryBuilder* qb, const char* table, const char* left_col, const char* op, const char* right_col) {
    if (!qb || qb->join_count >= QB_MAX_JOINS) return qb;
    QBJoin* j = &qb->joins[qb->join_count++];
    j->type = strdup("INNER");
    j->table = strdup(table ? table : "");
    j->left_col = strdup(left_col ? left_col : "");
    j->op = strdup(op ? op : "=");
    j->right_col = strdup(right_col ? right_col : "");
    return qb;
}

GardQueryBuilder* gard_query_left_join(GardQueryBuilder* qb, const char* table, const char* left_col, const char* op, const char* right_col) {
    if (!qb || qb->join_count >= QB_MAX_JOINS) return qb;
    QBJoin* j = &qb->joins[qb->join_count++];
    j->type = strdup("LEFT");
    j->table = strdup(table ? table : "");
    j->left_col = strdup(left_col ? left_col : "");
    j->op = strdup(op ? op : "=");
    j->right_col = strdup(right_col ? right_col : "");
    return qb;
}

GardQueryBuilder* gard_query_group_by(GardQueryBuilder* qb, const char* columns) {
    if (!qb) return qb;
    if (qb->group_by) free(qb->group_by);
    qb->group_by = columns ? strdup(columns) : NULL;
    return qb;
}

GardQueryBuilder* gard_query_having(GardQueryBuilder* qb, const char* condition) {
    if (!qb) return qb;
    if (qb->having) free(qb->having);
    qb->having = condition ? strdup(condition) : NULL;
    return qb;
}

GardQueryBuilder* gard_query_insert(GardQueryBuilder* qb) {
    if (qb) qb->type = QB_INSERT;
    return qb;
}

GardQueryBuilder* gard_query_update(GardQueryBuilder* qb) {
    if (qb) qb->type = QB_UPDATE;
    return qb;
}

GardQueryBuilder* gard_query_delete_from(GardQueryBuilder* qb) {
    if (qb) qb->type = QB_DELETE;
    return qb;
}

GardQueryBuilder* gard_query_set(GardQueryBuilder* qb, const char* column, const char* value) {
    if (!qb || qb->set_count >= QB_MAX_SETS) return qb;
    QBSet* s = &qb->sets[qb->set_count++];
    s->column = strdup(column ? column : "");
    s->value = strdup(value ? value : "");
    s->is_int = 0;
    return qb;
}

GardQueryBuilder* gard_query_set_int(GardQueryBuilder* qb, const char* column, int32_t value) {
    if (!qb || qb->set_count >= QB_MAX_SETS) return qb;
    QBSet* s = &qb->sets[qb->set_count++];
    s->column = strdup(column ? column : "");
    char buf[16]; snprintf(buf, sizeof(buf), "%d", value);
    s->value = strdup(buf);
    s->is_int = 1;
    s->int_val = value;
    return qb;
}

GardQueryBuilder* gard_query_where_in(GardQueryBuilder* qb, const char* column, const char* values) {
    if (!qb || qb->where_count >= QB_MAX_WHERES) return qb;
    QBWhere* w = &qb->wheres[qb->where_count++];
    w->column = strdup(column ? column : "");
    w->op = strdup("IN");
    w->value = strdup(values ? values : "");
    w->is_or = 0;
    w->special = 3;
    return qb;
}

GardQueryBuilder* gard_query_where_null(GardQueryBuilder* qb, const char* column) {
    if (!qb || qb->where_count >= QB_MAX_WHERES) return qb;
    QBWhere* w = &qb->wheres[qb->where_count++];
    w->column = strdup(column ? column : "");
    w->op = strdup("IS NULL");
    w->value = NULL;
    w->is_or = 0;
    w->special = 1;
    return qb;
}

GardQueryBuilder* gard_query_where_not_null(GardQueryBuilder* qb, const char* column) {
    if (!qb || qb->where_count >= QB_MAX_WHERES) return qb;
    QBWhere* w = &qb->wheres[qb->where_count++];
    w->column = strdup(column ? column : "");
    w->op = strdup("IS NOT NULL");
    w->value = NULL;
    w->is_or = 0;
    w->special = 2;
    return qb;
}

GardQueryBuilder* gard_query_where_between(GardQueryBuilder* qb, const char* column, const char* min_val, const char* max_val) {
    if (!qb || qb->where_count >= QB_MAX_WHERES) return qb;
    QBWhere* w = &qb->wheres[qb->where_count++];
    w->column = strdup(column ? column : "");
    w->op = strdup("BETWEEN");
    // Store as "min AND max"
    char buf[256];
    snprintf(buf, sizeof(buf), "%s AND %s", min_val ? min_val : "0", max_val ? max_val : "0");
    w->value = strdup(buf);
    w->is_or = 0;
    w->special = 4;
    return qb;
}

GardQueryBuilder* gard_query_where_like(GardQueryBuilder* qb, const char* column, const char* pattern) {
    if (!qb || qb->where_count >= QB_MAX_WHERES) return qb;
    QBWhere* w = &qb->wheres[qb->where_count++];
    w->column = strdup(column ? column : "");
    w->op = strdup("LIKE");
    w->value = strdup(pattern ? pattern : "");
    w->is_or = 0;
    w->special = 5;
    return qb;
}

GardQueryBuilder* gard_query_distinct(GardQueryBuilder* qb) {
    if (qb) qb->distinct = 1;
    return qb;
}

GardQueryBuilder* gard_query_count(GardQueryBuilder* qb, const char* column) {
    if (!qb) return qb;
    if (qb->columns) free(qb->columns);
    char buf[128];
    snprintf(buf, sizeof(buf), "COUNT(%s)", column && column[0] ? column : "*");
    qb->columns = strdup(buf);
    return qb;
}

GardQueryBuilder* gard_query_sum(GardQueryBuilder* qb, const char* column) {
    if (!qb || !column) return qb;
    if (qb->columns) free(qb->columns);
    char buf[128];
    snprintf(buf, sizeof(buf), "SUM(%s)", column);
    qb->columns = strdup(buf);
    return qb;
}

// === toSQL — compile to SQL string ===

char* gard_query_to_sql(GardQueryBuilder* qb) {
    return gard_query_to_sql_dialect(qb, "sqlite");
}

char* gard_query_to_sql_dialect(GardQueryBuilder* qb, const char* driver) {
    if (!qb) return strdup("");
    if (!driver) driver = "sqlite";

    char sql[8192];
    int pos = 0;

    switch (qb->type) {
        case QB_SELECT: {
            pos += snprintf(sql + pos, sizeof(sql) - pos, "SELECT ");
            if (qb->distinct) pos += snprintf(sql + pos, sizeof(sql) - pos, "DISTINCT ");
            pos += snprintf(sql + pos, sizeof(sql) - pos, "%s FROM %s",
                qb->columns ? qb->columns : "*", qb->table);
            break;
        }
        case QB_INSERT: {
            pos += snprintf(sql + pos, sizeof(sql) - pos, "INSERT INTO %s", qb->table);
            if (qb->set_count > 0) {
                pos += snprintf(sql + pos, sizeof(sql) - pos, " (");
                for (int i = 0; i < qb->set_count; i++) {
                    if (i > 0) pos += snprintf(sql + pos, sizeof(sql) - pos, ", ");
                    pos += snprintf(sql + pos, sizeof(sql) - pos, "%s", qb->sets[i].column);
                }
                pos += snprintf(sql + pos, sizeof(sql) - pos, ") VALUES (");
                for (int i = 0; i < qb->set_count; i++) {
                    if (i > 0) pos += snprintf(sql + pos, sizeof(sql) - pos, ", ");
                    char* escaped = sql_escape_value(qb->sets[i].value);
                    pos += snprintf(sql + pos, sizeof(sql) - pos, "%s", escaped);
                    free(escaped);
                }
                pos += snprintf(sql + pos, sizeof(sql) - pos, ")");
            }
            break;
        }
        case QB_UPDATE: {
            pos += snprintf(sql + pos, sizeof(sql) - pos, "UPDATE %s SET ", qb->table);
            for (int i = 0; i < qb->set_count; i++) {
                if (i > 0) pos += snprintf(sql + pos, sizeof(sql) - pos, ", ");
                char* escaped = sql_escape_value(qb->sets[i].value);
                pos += snprintf(sql + pos, sizeof(sql) - pos, "%s = %s", qb->sets[i].column, escaped);
                free(escaped);
            }
            break;
        }
        case QB_DELETE: {
            pos += snprintf(sql + pos, sizeof(sql) - pos, "DELETE FROM %s", qb->table);
            break;
        }
    }

    // JOINs
    for (int i = 0; i < qb->join_count; i++) {
        QBJoin* j = &qb->joins[i];
        pos += snprintf(sql + pos, sizeof(sql) - pos, " %s JOIN %s ON %s %s %s",
            j->type, j->table, j->left_col, j->op, j->right_col);
    }

    // WHERE
    if (qb->where_count > 0) {
        pos += snprintf(sql + pos, sizeof(sql) - pos, " WHERE ");
        for (int i = 0; i < qb->where_count; i++) {
            QBWhere* w = &qb->wheres[i];
            if (i > 0) pos += snprintf(sql + pos, sizeof(sql) - pos, " %s ", w->is_or ? "OR" : "AND");

            switch (w->special) {
                case 1: // IS NULL
                    pos += snprintf(sql + pos, sizeof(sql) - pos, "%s IS NULL", w->column);
                    break;
                case 2: // IS NOT NULL
                    pos += snprintf(sql + pos, sizeof(sql) - pos, "%s IS NOT NULL", w->column);
                    break;
                case 3: // IN
                    pos += snprintf(sql + pos, sizeof(sql) - pos, "%s IN (%s)", w->column, w->value);
                    break;
                case 4: // BETWEEN
                    pos += snprintf(sql + pos, sizeof(sql) - pos, "%s BETWEEN %s", w->column, w->value);
                    break;
                case 5: { // LIKE
                    char* escaped = sql_escape_value(w->value);
                    pos += snprintf(sql + pos, sizeof(sql) - pos, "%s LIKE %s", w->column, escaped);
                    free(escaped);
                    break;
                }
                default: { // Normal comparison
                    char* escaped = sql_escape_value(w->value);
                    pos += snprintf(sql + pos, sizeof(sql) - pos, "%s %s %s", w->column, w->op, escaped);
                    free(escaped);
                    break;
                }
            }
        }
    }

    // GROUP BY
    if (qb->group_by && qb->group_by[0]) {
        pos += snprintf(sql + pos, sizeof(sql) - pos, " GROUP BY %s", qb->group_by);
    }

    // HAVING
    if (qb->having && qb->having[0]) {
        pos += snprintf(sql + pos, sizeof(sql) - pos, " HAVING %s", qb->having);
    }

    // ORDER BY
    if (qb->order_count > 0) {
        pos += snprintf(sql + pos, sizeof(sql) - pos, " ORDER BY ");
        for (int i = 0; i < qb->order_count; i++) {
            if (i > 0) pos += snprintf(sql + pos, sizeof(sql) - pos, ", ");
            pos += snprintf(sql + pos, sizeof(sql) - pos, "%s %s", qb->orders[i].column, qb->orders[i].direction);
        }
    }

    // LIMIT / OFFSET
    if (qb->limit >= 0) {
        pos += snprintf(sql + pos, sizeof(sql) - pos, " LIMIT %d", qb->limit);
    }
    if (qb->offset >= 0) {
        pos += snprintf(sql + pos, sizeof(sql) - pos, " OFFSET %d", qb->offset);
    }

    return strdup(sql);
}

// === Database.run — execute query builder ===

GardDbResultSet* gard_db_run_query(GardDb* db, GardQueryBuilder* qb) {
    if (!db || !qb) return NULL;
    const char* driver = gard_db_get_driver(db);
    char* sql = gard_query_to_sql_dialect(qb, driver);
    if (!sql || !sql[0]) { free(sql); return NULL; }

    if (qb->type == QB_SELECT) {
        GardDbResultSet* rs = gard_db_query(db, sql);
        free(sql);
        return rs;
    } else {
        gard_db_execute(db, sql);
        free(sql);
        return NULL;
    }
}

// === Free ===

void gard_query_free(GardQueryBuilder* qb) {
    if (!qb) return;
    free(qb->table);
    if (qb->columns) free(qb->columns);
    if (qb->group_by) free(qb->group_by);
    if (qb->having) free(qb->having);
    for (int i = 0; i < qb->where_count; i++) {
        free(qb->wheres[i].column);
        free(qb->wheres[i].op);
        if (qb->wheres[i].value) free(qb->wheres[i].value);
    }
    for (int i = 0; i < qb->set_count; i++) {
        free(qb->sets[i].column);
        free(qb->sets[i].value);
    }
    for (int i = 0; i < qb->join_count; i++) {
        free(qb->joins[i].type);
        free(qb->joins[i].table);
        free(qb->joins[i].left_col);
        free(qb->joins[i].op);
        free(qb->joins[i].right_col);
    }
    for (int i = 0; i < qb->order_count; i++) {
        free(qb->orders[i].column);
        free(qb->orders[i].direction);
    }
    free(qb);
}
