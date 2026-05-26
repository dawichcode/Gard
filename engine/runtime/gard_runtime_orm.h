// Gard ORM Runtime — lightweight dynamic object system + ORM for AOT
// Provides dynamic field iteration needed for ORM.persist/findAll/etc.

#ifndef GARD_RUNTIME_ORM_H
#define GARD_RUNTIME_ORM_H

#include "gard_runtime_db.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Dynamic Object (runtime key-value store for ORM)
// Allows AOT code to build objects with named fields that
// can be iterated for SQL generation.
// ============================================================

typedef struct GardObjField {
    char* key;
    char* value;       // all values stored as strings for SQL
    int32_t int_val;   // cached int value
    int32_t is_int;    // 1 if value is integer
} GardObjField;

typedef struct GardObj {
    char* class_name;
    char* table_name;
    GardObjField* fields;
    int32_t field_count;
    int32_t field_capacity;
    int32_t id;        // primary key (0 = not persisted)
} GardObj;

// Object lifecycle
GardObj* gard_obj_new(const char* class_name);
void gard_obj_set(GardObj* obj, const char* key, const char* value);
void gard_obj_set_int(GardObj* obj, const char* key, int32_t value);
const char* gard_obj_get(GardObj* obj, const char* key);
int32_t gard_obj_get_int(GardObj* obj, const char* key);
int32_t gard_obj_get_id(GardObj* obj);
void gard_obj_free(GardObj* obj);

// ============================================================
// ORM operations (exact method names from stdlib_core.cpp)
// ============================================================

// ORM.setConnection / ORM.getConnection
void gard_orm_set_connection(GardDb* db);
GardDb* gard_orm_get_connection(void);

// ORM.createTable(db, className, fieldDefs)
// fieldDefs format: "name TEXT, age INTEGER, email TEXT"
int32_t gard_orm_create_table(GardDb* db, const char* table_name, const char* field_defs);

// ORM.persist(db, obj) — INSERT or UPDATE
GardObj* gard_orm_persist(GardDb* db, GardObj* obj);

// ORM.remove(db, obj) — DELETE by id
int32_t gard_orm_remove(GardDb* db, GardObj* obj);

// ORM.findById(db, className, id)
GardObj* gard_orm_find_by_id(GardDb* db, const char* class_name, int32_t id);

// ORM.findAll(db, className)
GardObj** gard_orm_find_all(GardDb* db, const char* class_name, int32_t* out_count);

// ORM.findOne(db, className, whereCol, whereVal)
GardObj* gard_orm_find_one(GardDb* db, const char* class_name, const char* where_col, const char* where_val);

// ORM.instanceSave(obj) — uses global connection
GardObj* gard_orm_instance_save(GardObj* obj);

// ORM.instanceDelete(obj) — uses global connection
int32_t gard_orm_instance_delete(GardObj* obj);

#ifdef __cplusplus
}
#endif

#endif // GARD_RUNTIME_ORM_H
