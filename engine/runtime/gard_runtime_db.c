// Gard Database Runtime — C implementation
// Supports: SQLite, MySQL, PostgreSQL, MongoDB
//
// Compile:
//   gcc -c -O2 -fPIC gard_runtime_db.c -o gard_runtime_db.o \
//       $(pkg-config --cflags libmongoc-1.0 2>/dev/null)
//
// Link (add what's available):
//   -lsqlite3 -lmysqlclient -lpq -lmongoc-1.0 -lbson-1.0

#include "gard_runtime_db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Conditional includes based on available libraries
#ifdef __has_include
  #if __has_include(<sqlite3.h>)
    #include <sqlite3.h>
    #define GARD_HAS_SQLITE 1
  #endif
  #if __has_include(<mysql/mysql.h>)
    #include <mysql/mysql.h>
    #define GARD_HAS_MYSQL 1
  #endif
  #if __has_include(<libpq-fe.h>)
    #include <libpq-fe.h>
    #define GARD_HAS_POSTGRES 1
  #endif
  #if __has_include(<mongoc/mongoc.h>)
    #include <mongoc/mongoc.h>
    #define GARD_HAS_MONGO 1
  #endif
#else
  // Fallback: assume SQLite is always available
  #include <sqlite3.h>
  #define GARD_HAS_SQLITE 1
#endif

// Driver enum
enum GardDbDriver {
    GARD_DB_SQLITE = 0,
    GARD_DB_MYSQL = 1,
    GARD_DB_POSTGRES = 2,
    GARD_DB_MONGODB = 3,
    GARD_DB_NONE = -1
};

struct GardDb {
    enum GardDbDriver driver;
    int32_t query_count;
    int32_t in_transaction;
    char* url;
    char* database_name;
    // Driver-specific handles
#ifdef GARD_HAS_SQLITE
    sqlite3* sqlite;
#endif
#ifdef GARD_HAS_MYSQL
    MYSQL* mysql;
#endif
#ifdef GARD_HAS_POSTGRES
    PGconn* pg;
#endif
#ifdef GARD_HAS_MONGO
    mongoc_client_t* mongo;
    mongoc_database_t* mongo_db;
#endif
};

#ifdef GARD_HAS_MONGO
static int g_mongo_initialized = 0;
#endif

// === Database.connect ===

GardDb* gard_db_connect(const char* connection_string) {
    if (!connection_string) return NULL;

    GardDb* db = (GardDb*)calloc(1, sizeof(GardDb));
    db->driver = GARD_DB_NONE;
    db->query_count = 0;
    db->in_transaction = 0;
    db->url = strdup(connection_string);

    // --- SQLite ---
    if (strncmp(connection_string, "sqlite://", 9) == 0) {
#ifdef GARD_HAS_SQLITE
        db->driver = GARD_DB_SQLITE;
        const char* path = connection_string + 9;
        if (path[0] == '\0' || strcmp(path, ":memory:") == 0) path = ":memory:";
        int rc = sqlite3_open(path, &db->sqlite);
        if (rc != SQLITE_OK) {
            if (db->sqlite) sqlite3_close(db->sqlite);
            free(db->url); free(db);
            return NULL;
        }
        sqlite3_exec(db->sqlite, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
        sqlite3_exec(db->sqlite, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
        return db;
#else
        free(db->url); free(db);
        return NULL;
#endif
    }

    // --- PostgreSQL ---
    if (strncmp(connection_string, "postgres://", 11) == 0 ||
        strncmp(connection_string, "postgresql://", 13) == 0) {
#ifdef GARD_HAS_POSTGRES
        db->driver = GARD_DB_POSTGRES;
        db->pg = PQconnectdb(connection_string);
        if (PQstatus(db->pg) != CONNECTION_OK) {
            PQfinish(db->pg);
            free(db->url); free(db);
            return NULL;
        }
        PQsetClientEncoding(db->pg, "UTF8");
        return db;
#else
        free(db->url); free(db);
        return NULL;
#endif
    }

    // --- MySQL ---
    if (strncmp(connection_string, "mysql://", 8) == 0) {
#ifdef GARD_HAS_MYSQL
        db->driver = GARD_DB_MYSQL;
        // Parse: mysql://user:pass@host:port/database
        char* rest = strdup(connection_string + 8);
        char *user = NULL, *pass = NULL, *host = "127.0.0.1", *database = NULL;
        int port = 3306;

        char* at = strchr(rest, '@');
        char* host_start = rest;
        if (at) {
            *at = '\0';
            char* colon = strchr(rest, ':');
            if (colon) { *colon = '\0'; user = rest; pass = colon + 1; }
            else { user = rest; }
            host_start = at + 1;
        }
        char* slash = strchr(host_start, '/');
        if (slash) { *slash = '\0'; database = slash + 1; }
        char* colon = strchr(host_start, ':');
        if (colon) { *colon = '\0'; host = host_start; port = atoi(colon + 1); }
        else if (host_start[0]) { host = host_start; }

        db->mysql = mysql_init(NULL);
        if (!db->mysql) { free(rest); free(db->url); free(db); return NULL; }
        unsigned int timeout = 10;
        mysql_options(db->mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        if (!mysql_real_connect(db->mysql, host,
                                user && user[0] ? user : NULL,
                                pass && pass[0] ? pass : NULL,
                                database && database[0] ? database : NULL,
                                port, NULL, 0)) {
            mysql_close(db->mysql);
            free(rest); free(db->url); free(db);
            return NULL;
        }
        mysql_set_character_set(db->mysql, "utf8mb4");
        free(rest);
        return db;
#else
        free(db->url); free(db);
        return NULL;
#endif
    }

    // --- MongoDB ---
    if (strncmp(connection_string, "mongodb://", 10) == 0 ||
        strncmp(connection_string, "mongodb+srv://", 14) == 0) {
#ifdef GARD_HAS_MONGO
        db->driver = GARD_DB_MONGODB;
        if (!g_mongo_initialized) { mongoc_init(); g_mongo_initialized = 1; }
        db->mongo = mongoc_client_new(connection_string);
        if (!db->mongo) { free(db->url); free(db); return NULL; }
        // Extract database name from URL
        const char* last_slash = strrchr(connection_string, '/');
        char* db_name = strdup("test");
        if (last_slash && (last_slash - connection_string) > 10) {
            free(db_name);
            db_name = strdup(last_slash + 1);
            char* q = strchr(db_name, '?');
            if (q) *q = '\0';
        }
        db->database_name = db_name;
        // Ping to verify
        bson_t* cmd = BCON_NEW("ping", BCON_INT32(1));
        bson_t reply;
        bson_error_t error;
        int ok = mongoc_client_command_simple(db->mongo, "admin", cmd, NULL, &reply, &error);
        bson_destroy(cmd);
        bson_destroy(&reply);
        if (!ok) {
            mongoc_client_destroy(db->mongo);
            free(db_name); free(db->url); free(db);
            return NULL;
        }
        db->mongo_db = mongoc_client_get_database(db->mongo, db_name);
        return db;
#else
        free(db->url); free(db);
        return NULL;
#endif
    }

    // Unknown driver
    free(db->url); free(db);
    return NULL;
}

// === Database.close ===

void gard_db_close(GardDb* db) {
    if (!db) return;
#ifdef GARD_HAS_SQLITE
    if (db->driver == GARD_DB_SQLITE && db->sqlite) sqlite3_close(db->sqlite);
#endif
#ifdef GARD_HAS_MYSQL
    if (db->driver == GARD_DB_MYSQL && db->mysql) mysql_close(db->mysql);
#endif
#ifdef GARD_HAS_POSTGRES
    if (db->driver == GARD_DB_POSTGRES && db->pg) PQfinish(db->pg);
#endif
#ifdef GARD_HAS_MONGO
    if (db->driver == GARD_DB_MONGODB) {
        if (db->mongo_db) mongoc_database_destroy(db->mongo_db);
        if (db->mongo) mongoc_client_destroy(db->mongo);
    }
#endif
    if (db->url) free(db->url);
    if (db->database_name) free(db->database_name);
    free(db);
}

int32_t gard_db_is_connected(GardDb* db) {
    return db != NULL ? 1 : 0;
}

const char* gard_db_get_driver(GardDb* db) {
    if (!db) return "none";
    switch (db->driver) {
        case GARD_DB_SQLITE: return "sqlite";
        case GARD_DB_MYSQL: return "mysql";
        case GARD_DB_POSTGRES: return "postgres";
        case GARD_DB_MONGODB: return "mongodb";
        default: return "none";
    }
}

int32_t gard_db_get_query_count(GardDb* db) {
    return db ? db->query_count : 0;
}

// === Database.execute ===

GardDbResult gard_db_execute(GardDb* db, const char* sql) {
    GardDbResult result = {0, 0};
    if (!db || !sql) return result;

#ifdef GARD_HAS_SQLITE
    if (db->driver == GARD_DB_SQLITE && db->sqlite) {
        char* err = NULL;
        int rc = sqlite3_exec(db->sqlite, sql, NULL, NULL, &err);
        if (err) sqlite3_free(err);
        if (rc == SQLITE_OK) {
            result.changes = sqlite3_changes(db->sqlite);
            result.last_insert_id = (int32_t)sqlite3_last_insert_rowid(db->sqlite);
        }
        db->query_count++;
        return result;
    }
#endif

#ifdef GARD_HAS_MYSQL
    if (db->driver == GARD_DB_MYSQL && db->mysql) {
        if (mysql_query(db->mysql, sql) == 0) {
            MYSQL_RES* discard = mysql_store_result(db->mysql);
            if (discard) mysql_free_result(discard);
            result.changes = (int32_t)mysql_affected_rows(db->mysql);
            result.last_insert_id = (int32_t)mysql_insert_id(db->mysql);
        }
        db->query_count++;
        return result;
    }
#endif

#ifdef GARD_HAS_POSTGRES
    if (db->driver == GARD_DB_POSTGRES && db->pg) {
        PGresult* res = PQexec(db->pg, sql);
        ExecStatusType status = PQresultStatus(res);
        if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) {
            char* affected = PQcmdTuples(res);
            result.changes = (affected && *affected) ? atoi(affected) : 0;
            if (status == PGRES_TUPLES_OK && PQntuples(res) > 0 && PQnfields(res) > 0) {
                char* val = PQgetvalue(res, 0, 0);
                if (val && *val) result.last_insert_id = atoi(val);
            }
        }
        PQclear(res);
        db->query_count++;
        return result;
    }
#endif

    return result;
}

// === Database.query ===

GardDbResultSet* gard_db_query(GardDb* db, const char* sql) {
    if (!db || !sql) return NULL;

    GardDbResultSet* rs = (GardDbResultSet*)calloc(1, sizeof(GardDbResultSet));

#ifdef GARD_HAS_SQLITE
    if (db->driver == GARD_DB_SQLITE && db->sqlite) {
        sqlite3_stmt* stmt = NULL;
        int rc = sqlite3_prepare_v2(db->sqlite, sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) { free(rs); return NULL; }

        int col_count = sqlite3_column_count(stmt);
        rs->col_count = col_count;

        // Collect rows
        int capacity = 64;
        rs->rows = (GardDbRow*)malloc(capacity * sizeof(GardDbRow));
        rs->row_count = 0;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (rs->row_count >= capacity) {
                capacity *= 2;
                rs->rows = (GardDbRow*)realloc(rs->rows, capacity * sizeof(GardDbRow));
            }
            GardDbRow* row = &rs->rows[rs->row_count];
            row->col_count = col_count;
            row->col_names = (char**)malloc(col_count * sizeof(char*));
            row->col_values = (char**)malloc(col_count * sizeof(char*));
            for (int c = 0; c < col_count; c++) {
                row->col_names[c] = strdup(sqlite3_column_name(stmt, c));
                const char* val = (const char*)sqlite3_column_text(stmt, c);
                row->col_values[c] = val ? strdup(val) : NULL;
            }
            rs->row_count++;
        }
        sqlite3_finalize(stmt);
        db->query_count++;
        return rs;
    }
#endif

#ifdef GARD_HAS_MYSQL
    if (db->driver == GARD_DB_MYSQL && db->mysql) {
        if (mysql_query(db->mysql, sql) != 0) { free(rs); return NULL; }
        MYSQL_RES* result = mysql_store_result(db->mysql);
        if (!result) { free(rs); return NULL; }

        int col_count = mysql_num_fields(result);
        MYSQL_FIELD* fields = mysql_fetch_fields(result);
        rs->col_count = col_count;
        rs->row_count = (int32_t)mysql_num_rows(result);
        rs->rows = (GardDbRow*)malloc(rs->row_count * sizeof(GardDbRow));

        MYSQL_ROW row;
        int r = 0;
        while ((row = mysql_fetch_row(result))) {
            unsigned long* lengths = mysql_fetch_lengths(result);
            rs->rows[r].col_count = col_count;
            rs->rows[r].col_names = (char**)malloc(col_count * sizeof(char*));
            rs->rows[r].col_values = (char**)malloc(col_count * sizeof(char*));
            for (int c = 0; c < col_count; c++) {
                rs->rows[r].col_names[c] = strdup(fields[c].name);
                rs->rows[r].col_values[c] = row[c] ? strndup(row[c], lengths[c]) : NULL;
            }
            r++;
        }
        mysql_free_result(result);
        db->query_count++;
        return rs;
    }
#endif

#ifdef GARD_HAS_POSTGRES
    if (db->driver == GARD_DB_POSTGRES && db->pg) {
        PGresult* res = PQexec(db->pg, sql);
        if (PQresultStatus(res) != PGRES_TUPLES_OK) { PQclear(res); free(rs); return NULL; }

        int row_count = PQntuples(res);
        int col_count = PQnfields(res);
        rs->col_count = col_count;
        rs->row_count = row_count;
        rs->rows = (GardDbRow*)malloc(row_count * sizeof(GardDbRow));

        for (int r = 0; r < row_count; r++) {
            rs->rows[r].col_count = col_count;
            rs->rows[r].col_names = (char**)malloc(col_count * sizeof(char*));
            rs->rows[r].col_values = (char**)malloc(col_count * sizeof(char*));
            for (int c = 0; c < col_count; c++) {
                rs->rows[r].col_names[c] = strdup(PQfname(res, c));
                rs->rows[r].col_values[c] = PQgetisnull(res, r, c) ? NULL : strdup(PQgetvalue(res, r, c));
            }
        }
        PQclear(res);
        db->query_count++;
        return rs;
    }
#endif

    free(rs);
    return NULL;
}

void gard_db_result_free(GardDbResultSet* rs) {
    if (!rs) return;
    for (int r = 0; r < rs->row_count; r++) {
        for (int c = 0; c < rs->rows[r].col_count; c++) {
            free(rs->rows[r].col_names[c]);
            if (rs->rows[r].col_values[c]) free(rs->rows[r].col_values[c]);
        }
        free(rs->rows[r].col_names);
        free(rs->rows[r].col_values);
    }
    free(rs->rows);
    free(rs);
}

// === Transactions ===

int32_t gard_db_begin_transaction(GardDb* db) {
    if (!db || db->in_transaction) return 0;

#ifdef GARD_HAS_SQLITE
    if (db->driver == GARD_DB_SQLITE && db->sqlite) {
        char* err = NULL;
        int rc = sqlite3_exec(db->sqlite, "BEGIN TRANSACTION;", NULL, NULL, &err);
        if (err) sqlite3_free(err);
        if (rc == SQLITE_OK) { db->in_transaction = 1; return 1; }
        return 0;
    }
#endif
#ifdef GARD_HAS_MYSQL
    if (db->driver == GARD_DB_MYSQL && db->mysql) {
        if (mysql_query(db->mysql, "START TRANSACTION") == 0) { db->in_transaction = 1; return 1; }
        return 0;
    }
#endif
#ifdef GARD_HAS_POSTGRES
    if (db->driver == GARD_DB_POSTGRES && db->pg) {
        PGresult* res = PQexec(db->pg, "BEGIN");
        int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
        PQclear(res);
        if (ok) { db->in_transaction = 1; return 1; }
        return 0;
    }
#endif
    return 0;
}

int32_t gard_db_commit(GardDb* db) {
    if (!db || !db->in_transaction) return 0;

#ifdef GARD_HAS_SQLITE
    if (db->driver == GARD_DB_SQLITE && db->sqlite) {
        char* err = NULL;
        int rc = sqlite3_exec(db->sqlite, "COMMIT;", NULL, NULL, &err);
        if (err) sqlite3_free(err);
        db->in_transaction = 0;
        return rc == SQLITE_OK ? 1 : 0;
    }
#endif
#ifdef GARD_HAS_MYSQL
    if (db->driver == GARD_DB_MYSQL && db->mysql) {
        int ok = (mysql_query(db->mysql, "COMMIT") == 0);
        db->in_transaction = 0;
        return ok ? 1 : 0;
    }
#endif
#ifdef GARD_HAS_POSTGRES
    if (db->driver == GARD_DB_POSTGRES && db->pg) {
        PGresult* res = PQexec(db->pg, "COMMIT");
        int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
        PQclear(res);
        db->in_transaction = 0;
        return ok ? 1 : 0;
    }
#endif
    return 0;
}

int32_t gard_db_rollback(GardDb* db) {
    if (!db || !db->in_transaction) return 0;

#ifdef GARD_HAS_SQLITE
    if (db->driver == GARD_DB_SQLITE && db->sqlite) {
        char* err = NULL;
        sqlite3_exec(db->sqlite, "ROLLBACK;", NULL, NULL, &err);
        if (err) sqlite3_free(err);
        db->in_transaction = 0;
        return 1;
    }
#endif
#ifdef GARD_HAS_MYSQL
    if (db->driver == GARD_DB_MYSQL && db->mysql) {
        mysql_query(db->mysql, "ROLLBACK");
        db->in_transaction = 0;
        return 1;
    }
#endif
#ifdef GARD_HAS_POSTGRES
    if (db->driver == GARD_DB_POSTGRES && db->pg) {
        PGresult* res = PQexec(db->pg, "ROLLBACK");
        PQclear(res);
        db->in_transaction = 0;
        return 1;
    }
#endif
    return 0;
}
