// Gard ORM Runtime — lightweight dynamic object system + ORM for AOT
// Works with ALL database drivers via gard_runtime_db (SQLite, MySQL, PostgreSQL, MongoDB)
// Compile: gcc -c -O2 -fPIC gard_runtime_orm.c -o gard_runtime_orm.o
// Link with: libgard_runtime_db.a -lsqlite3 [-lmysqlclient] [-lpq] [-lmongoc-1.0 -lbson-1.0]

#include "gard_runtime_orm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ============================================================
// Dynamic Object System
// ============================================================

GardObj* gard_obj_new(const char* class_name) {
    GardObj* obj = (GardObj*)calloc(1, sizeof(GardObj));
    obj->class_name = strdup(class_name ? class_name : "Object");
    // Table name = lowercase class name
    size_t len = strlen(obj->class_name);
    obj->table_name = (char*)malloc(len + 1);
    for (size_t i = 0; i < len; i++) obj->table_name[i] = tolower((unsigned char)obj->class_name[i]);
    obj->table_name[len] = '\0';
    obj->field_capacity = 16;
    obj->fields = (GardObjField*)calloc(obj->field_capacity, sizeof(GardObjField));
    obj->field_count = 0;
    obj->id = 0;
    return obj;
}

static GardObjField* obj_find_field(GardObj* obj, const char* key) {
    if (!obj || !key) return NULL;
    for (int32_t i = 0; i < obj->field_count; i++) {
        if (strcmp(obj->fields[i].key, key) == 0) return &obj->fields[i];
    }
    return NULL;
}

void gard_obj_set(GardObj* obj, const char* key, const char* value) {
    if (!obj || !key) return;
    GardObjField* f = obj_find_field(obj, key);
    if (f) {
        free(f->value);
        f->value = value ? strdup(value) : NULL;
        f->is_int = 0;
        return;
    }
    if (obj->field_count >= obj->field_capacity) {
        obj->field_capacity *= 2;
        obj->fields = (GardObjField*)realloc(obj->fields, obj->field_capacity * sizeof(GardObjField));
    }
    f = &obj->fields[obj->field_count++];
    f->key = strdup(key);
    f->value = value ? strdup(value) : NULL;
    f->int_val = 0;
    f->is_int = 0;
}

void gard_obj_set_int(GardObj* obj, const char* key, int32_t value) {
    if (!obj || !key) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    GardObjField* f = obj_find_field(obj, key);
    if (f) {
        free(f->value);
        f->value = strdup(buf);
        f->int_val = value;
        f->is_int = 1;
        if (strcmp(key, "id") == 0) obj->id = value;
        return;
    }
    if (obj->field_count >= obj->field_capacity) {
        obj->field_capacity *= 2;
        obj->fields = (GardObjField*)realloc(obj->fields, obj->field_capacity * sizeof(GardObjField));
    }
    f = &obj->fields[obj->field_count++];
    f->key = strdup(key);
    f->value = strdup(buf);
    f->int_val = value;
    f->is_int = 1;
    if (strcmp(key, "id") == 0) obj->id = value;
}

const char* gard_obj_get(GardObj* obj, const char* key) {
    GardObjField* f = obj_find_field(obj, key);
    return f ? (f->value ? f->value : "") : "";
}

int32_t gard_obj_get_int(GardObj* obj, const char* key) {
    GardObjField* f = obj_find_field(obj, key);
    if (!f) return 0;
    if (f->is_int) return f->int_val;
    return f->value ? atoi(f->value) : 0;
}

int32_t gard_obj_get_id(GardObj* obj) {
    return obj ? obj->id : 0;
}

void gard_obj_free(GardObj* obj) {
    if (!obj) return;
    for (int32_t i = 0; i < obj->field_count; i++) {
        free(obj->fields[i].key);
        if (obj->fields[i].value) free(obj->fields[i].value);
    }
    free(obj->fields);
    free(obj->class_name);
    free(obj->table_name);
    free(obj);
}

// ============================================================
// ORM — Global Connection
// ============================================================

static GardDb* g_orm_connection = NULL;

void gard_orm_set_connection(GardDb* db) {
    g_orm_connection = db;
}

GardDb* gard_orm_get_connection(void) {
    return g_orm_connection;
}

// ============================================================
// Helper: escape a string value for SQL (single quotes)
// ============================================================

static char* sql_escape(const char* val) {
    if (!val) return strdup("NULL");
    size_t len = strlen(val);
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

// ============================================================
// ORM.createTable — works with all drivers via gard_db_execute
// ============================================================

int32_t gard_orm_create_table(GardDb* db, const char* table_name, const char* field_defs) {
    if (!db || !table_name) return 0;
    // Build CREATE TABLE SQL (driver-agnostic for basic types)
    char sql[4096];
    const char* driver = gard_db_get_driver(db);

    if (strcmp(driver, "mysql") == 0) {
        snprintf(sql, sizeof(sql),
            "CREATE TABLE IF NOT EXISTS %s (id INT AUTO_INCREMENT PRIMARY KEY%s%s) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
            table_name, field_defs && field_defs[0] ? ", " : "", field_defs ? field_defs : "");
    } else if (strcmp(driver, "postgres") == 0) {
        snprintf(sql, sizeof(sql),
            "CREATE TABLE IF NOT EXISTS %s (id SERIAL PRIMARY KEY%s%s)",
            table_name, field_defs && field_defs[0] ? ", " : "", field_defs ? field_defs : "");
    } else {
        // SQLite (default)
        snprintf(sql, sizeof(sql),
            "CREATE TABLE IF NOT EXISTS %s (id INTEGER PRIMARY KEY AUTOINCREMENT%s%s)",
            table_name, field_defs && field_defs[0] ? ", " : "", field_defs ? field_defs : "");
    }

    GardDbResult r = gard_db_execute(db, sql);
    (void)r;
    return 1;
}

// ============================================================
// ORM.persist — INSERT or UPDATE (works with all drivers)
// ============================================================

GardObj* gard_orm_persist(GardDb* db, GardObj* obj) {
    if (!db || !obj) return obj;

    int is_update = (obj->id > 0);

    if (is_update) {
        // UPDATE table SET col1=val1, col2=val2 WHERE id = N
        char sql[8192];
        int pos = snprintf(sql, sizeof(sql), "UPDATE %s SET ", obj->table_name);
        int first = 1;
        for (int32_t i = 0; i < obj->field_count; i++) {
            if (strcmp(obj->fields[i].key, "id") == 0) continue;
            if (!first) pos += snprintf(sql + pos, sizeof(sql) - pos, ", ");
            if (obj->fields[i].is_int) {
                pos += snprintf(sql + pos, sizeof(sql) - pos, "%s = %d",
                    obj->fields[i].key, obj->fields[i].int_val);
            } else {
                char* escaped = sql_escape(obj->fields[i].value);
                pos += snprintf(sql + pos, sizeof(sql) - pos, "%s = %s",
                    obj->fields[i].key, escaped);
                free(escaped);
            }
            first = 0;
        }
        pos += snprintf(sql + pos, sizeof(sql) - pos, " WHERE id = %d", obj->id);
        gard_db_execute(db, sql);
    } else {
        // INSERT INTO table (col1, col2) VALUES (val1, val2)
        char cols[4096] = "";
        char vals[4096] = "";
        int cpos = 0, vpos = 0;
        int first = 1;
        for (int32_t i = 0; i < obj->field_count; i++) {
            if (strcmp(obj->fields[i].key, "id") == 0) continue;
            if (!first) {
                cpos += snprintf(cols + cpos, sizeof(cols) - cpos, ", ");
                vpos += snprintf(vals + vpos, sizeof(vals) - vpos, ", ");
            }
            cpos += snprintf(cols + cpos, sizeof(cols) - cpos, "%s", obj->fields[i].key);
            if (obj->fields[i].is_int) {
                vpos += snprintf(vals + vpos, sizeof(vals) - vpos, "%d", obj->fields[i].int_val);
            } else {
                char* escaped = sql_escape(obj->fields[i].value);
                vpos += snprintf(vals + vpos, sizeof(vals) - vpos, "%s", escaped);
                free(escaped);
            }
            first = 0;
        }

        char sql[8192];
        const char* driver = gard_db_get_driver(db);
        if (strcmp(driver, "postgres") == 0) {
            // PostgreSQL: use RETURNING id to get the inserted id
            snprintf(sql, sizeof(sql), "INSERT INTO %s (%s) VALUES (%s) RETURNING id",
                obj->table_name, cols, vals);
            GardDbResultSet* rs = gard_db_query(db, sql);
            if (rs && rs->row_count > 0 && rs->rows[0].col_count > 0 && rs->rows[0].col_values[0]) {
                obj->id = atoi(rs->rows[0].col_values[0]);
                gard_obj_set_int(obj, "id", obj->id);
            }
            gard_db_result_free(rs);
        } else {
            // SQLite and MySQL: use last_insert_id from execute result
            snprintf(sql, sizeof(sql), "INSERT INTO %s (%s) VALUES (%s)",
                obj->table_name, cols, vals);
            GardDbResult r = gard_db_execute(db, sql);
            if (r.last_insert_id > 0) {
                obj->id = r.last_insert_id;
                gard_obj_set_int(obj, "id", r.last_insert_id);
            }
        }
    }
    return obj;
}

// ============================================================
// ORM.remove — DELETE by id (works with all drivers)
// ============================================================

int32_t gard_orm_remove(GardDb* db, GardObj* obj) {
    if (!db || !obj || obj->id <= 0) return 0;
    char sql[512];
    snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE id = %d", obj->table_name, obj->id);
    gard_db_execute(db, sql);
    return 1;
}

// ============================================================
// ORM.findById — works with all drivers via gard_db_query
// ============================================================

GardObj* gard_orm_find_by_id(GardDb* db, const char* class_name, int32_t id) {
    if (!db || !class_name || id <= 0) return NULL;
    char table[256];
    size_t len = strlen(class_name);
    for (size_t i = 0; i < len && i < 255; i++) table[i] = tolower((unsigned char)class_name[i]);
    table[len < 255 ? len : 255] = '\0';

    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE id = %d", table, id);
    GardDbResultSet* rs = gard_db_query(db, sql);
    if (!rs || rs->row_count == 0) { gard_db_result_free(rs); return NULL; }

    GardObj* obj = gard_obj_new(class_name);
    GardDbRow* row = &rs->rows[0];
    for (int32_t c = 0; c < row->col_count; c++) {
        if (strcmp(row->col_names[c], "id") == 0) {
            gard_obj_set_int(obj, "id", row->col_values[c] ? atoi(row->col_values[c]) : 0);
        } else {
            gard_obj_set(obj, row->col_names[c], row->col_values[c]);
        }
    }
    gard_db_result_free(rs);
    return obj;
}

// ============================================================
// ORM.findAll — works with all drivers via gard_db_query
// ============================================================

GardObj** gard_orm_find_all(GardDb* db, const char* class_name, int32_t* out_count) {
    if (out_count) *out_count = 0;
    if (!db || !class_name) return NULL;
    char table[256];
    size_t len = strlen(class_name);
    for (size_t i = 0; i < len && i < 255; i++) table[i] = tolower((unsigned char)class_name[i]);
    table[len < 255 ? len : 255] = '\0';

    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT * FROM %s", table);
    GardDbResultSet* rs = gard_db_query(db, sql);
    if (!rs || rs->row_count == 0) { gard_db_result_free(rs); return NULL; }

    GardObj** results = (GardObj**)malloc(rs->row_count * sizeof(GardObj*));
    for (int32_t r = 0; r < rs->row_count; r++) {
        GardObj* obj = gard_obj_new(class_name);
        GardDbRow* row = &rs->rows[r];
        for (int32_t c = 0; c < row->col_count; c++) {
            if (strcmp(row->col_names[c], "id") == 0) {
                gard_obj_set_int(obj, "id", row->col_values[c] ? atoi(row->col_values[c]) : 0);
            } else {
                gard_obj_set(obj, row->col_names[c], row->col_values[c]);
            }
        }
        results[r] = obj;
    }
    if (out_count) *out_count = rs->row_count;
    gard_db_result_free(rs);
    return results;
}

// ============================================================
// ORM.findOne — works with all drivers
// ============================================================

GardObj* gard_orm_find_one(GardDb* db, const char* class_name, const char* where_col, const char* where_val) {
    if (!db || !class_name || !where_col || !where_val) return NULL;
    char table[256];
    size_t len = strlen(class_name);
    for (size_t i = 0; i < len && i < 255; i++) table[i] = tolower((unsigned char)class_name[i]);
    table[len < 255 ? len : 255] = '\0';

    char* escaped = sql_escape(where_val);
    char sql[1024];
    const char* driver = gard_db_get_driver(db);
    if (strcmp(driver, "mysql") == 0) {
        snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s = %s LIMIT 1", table, where_col, escaped);
    } else if (strcmp(driver, "postgres") == 0) {
        snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s = %s LIMIT 1", table, where_col, escaped);
    } else {
        snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s = %s LIMIT 1", table, where_col, escaped);
    }
    free(escaped);

    GardDbResultSet* rs = gard_db_query(db, sql);
    if (!rs || rs->row_count == 0) { gard_db_result_free(rs); return NULL; }

    GardObj* obj = gard_obj_new(class_name);
    GardDbRow* row = &rs->rows[0];
    for (int32_t c = 0; c < row->col_count; c++) {
        if (strcmp(row->col_names[c], "id") == 0) {
            gard_obj_set_int(obj, "id", row->col_values[c] ? atoi(row->col_values[c]) : 0);
        } else {
            gard_obj_set(obj, row->col_names[c], row->col_values[c]);
        }
    }
    gard_db_result_free(rs);
    return obj;
}

// ============================================================
// ORM.instanceSave / ORM.instanceDelete (use global connection)
// ============================================================

GardObj* gard_orm_instance_save(GardObj* obj) {
    if (!g_orm_connection || !obj) return obj;
    return gard_orm_persist(g_orm_connection, obj);
}

int32_t gard_orm_instance_delete(GardObj* obj) {
    if (!g_orm_connection || !obj) return 0;
    return gard_orm_remove(g_orm_connection, obj);
}
