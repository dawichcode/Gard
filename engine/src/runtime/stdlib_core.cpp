#include "runtime/stdlib_core.h"
#include <sqlite3.h>
#include <mysql/mysql.h>
#include <libpq-fe.h>
#include <mongoc/mongoc.h>
#include <string>
#include <sstream>
#include <cstring>
#include <ctime>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <iostream>

namespace gard {
namespace runtime {
namespace stdlib {

// ===== Database internals =====

struct DbConnection {
    sqlite3* db = nullptr;
    MYSQL* mysql = nullptr;
    PGconn* pg = nullptr;
    mongoc_client_t* mongo = nullptr;
    mongoc_database_t* mongoDb = nullptr;
    std::string url;
    std::string driver; // "sqlite", "postgres", "mysql", "mongodb"
    std::string database; // database name (for mongo)
    bool inTransaction = false;
    int queryCount = 0;
    std::mutex mutex;
};

static std::unordered_map<int, std::shared_ptr<DbConnection>> g_dbConnections;
static std::mutex g_dbMutex;
static int g_nextDbId = 1;

// ===== Connection Pool internals =====

struct PooledConnection {
    int dbId = -1;                          // ID into g_dbConnections
    bool inUse = false;
    std::chrono::steady_clock::time_point lastUsed;
    std::chrono::steady_clock::time_point createdAt;
    int queryCount = 0;
    bool healthy = true;
};

struct ConnectionPool {
    std::string connectionString;
    int minSize = 2;
    int maxSize = 10;
    int idleTimeoutMs = 30000;              // 30s idle timeout
    int maxLifetimeMs = 300000;             // 5min max connection lifetime
    int acquireTimeoutMs = 5000;            // 5s wait for connection
    int healthCheckIntervalMs = 10000;      // 10s between health checks
    bool validateOnAcquire = true;

    std::vector<std::shared_ptr<PooledConnection>> connections;
    std::mutex mutex;
    std::condition_variable cv;

    // Stats
    int totalCreated = 0;
    int totalDestroyed = 0;
    int totalAcquired = 0;
    int totalReleased = 0;
    int totalTimeouts = 0;
    int totalHealthChecksFailed = 0;
};

static std::unordered_map<int, std::shared_ptr<ConnectionPool>> g_pools;
static std::mutex g_poolMutex;
static int g_nextPoolId = 1;

} // namespace stdlib
} // namespace runtime
} // namespace gard

// Global ORM connection ID (outside namespace for extern access from main.cpp)
int g_globalDbId = -1;

// Driver availability flags (checked at runtime)
static bool g_mongoInitialized = false;

namespace gard {
namespace runtime {
namespace stdlib {

void registerCoreModule(VM& vm) {

    // Helper lambdas
    auto getOpt = [](const Value& v, const std::string& key) -> Value {
        if (v.type == ValueType::Object && v.objVal) {
            auto it = v.objVal->fields.find(key);
            if (it != v.objVal->fields.end()) return it->second;
        } else if (v.type == ValueType::Map && v.mapVal) {
            auto it = v.mapVal->entries.find(key);
            if (it != v.mapVal->entries.end()) return it->second;
        }
        return Value::makeNull();
    };

    auto isObjLike = [](const Value& v) -> bool {
        return (v.type == ValueType::Object && v.objVal) ||
               (v.type == ValueType::Map && v.mapVal);
    };

    // Database.connect(connectionString) — connect to database
    // Supports: "sqlite://path/to/db.sqlite", "sqlite::memory:"
    vm.registerNative("Database.connect", [&vm, getOpt, isObjLike](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardConnectionError", "Database.connect: connection string required"); return Value::makeNull(); }

        std::string connStr = a[0].toString();
        auto conn = std::make_shared<DbConnection>();
        conn->url = connStr;

        // Parse connection string
        if (connStr.find("sqlite://") == 0) {
            conn->driver = "sqlite";
            std::string path = connStr.substr(9); // after "sqlite://"
            if (path == ":memory:" || path.empty()) path = ":memory:";

            int rc = sqlite3_open(path.c_str(), &conn->db);
            if (rc != SQLITE_OK) {
                std::string err = conn->db ? sqlite3_errmsg(conn->db) : "unknown error";
                if (conn->db) sqlite3_close(conn->db);
                vm.throwError("GardConnectionError", "Database.connect: failed to open '" + path + "': " + err);
                return Value::makeNull();
            }
            // Enable WAL mode for better concurrency
            sqlite3_exec(conn->db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
            // Enable foreign keys
            sqlite3_exec(conn->db, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
        } else if (connStr.find("postgres://") == 0 || connStr.find("postgresql://") == 0) {
            conn->driver = "postgres";
            // Connect using libpq
            conn->pg = PQconnectdb(connStr.c_str());
            if (PQstatus(conn->pg) != CONNECTION_OK) {
                std::string err = PQerrorMessage(conn->pg);
                PQfinish(conn->pg);
                conn->pg = nullptr;
                vm.throwError("GardConnectionError", "Database.connect: PostgreSQL connection failed: " + err);
                return Value::makeNull();
            }
            // Set UTF-8 encoding
            PQsetClientEncoding(conn->pg, "UTF8");
        } else if (connStr.find("mysql://") == 0) {
            conn->driver = "mysql";
            // Parse: mysql://user:pass@host:port/database
            std::string rest = connStr.substr(8); // after "mysql://"
            std::string user, pass, host = "127.0.0.1", database;
            int port = 3306;

            // Extract user:pass@host:port/database
            size_t atPos = rest.find('@');
            if (atPos != std::string::npos) {
                std::string userPass = rest.substr(0, atPos);
                rest = rest.substr(atPos + 1);
                size_t colonPos = userPass.find(':');
                if (colonPos != std::string::npos) {
                    user = userPass.substr(0, colonPos);
                    pass = userPass.substr(colonPos + 1);
                } else {
                    user = userPass;
                }
            }
            // Extract host:port/database
            size_t slashPos = rest.find('/');
            if (slashPos != std::string::npos) {
                database = rest.substr(slashPos + 1);
                rest = rest.substr(0, slashPos);
            }
            size_t colonPos = rest.find(':');
            if (colonPos != std::string::npos) {
                host = rest.substr(0, colonPos);
                port = std::stoi(rest.substr(colonPos + 1));
            } else if (!rest.empty()) {
                host = rest;
            }

            conn->mysql = mysql_init(nullptr);
            if (!conn->mysql) {
                vm.throwError("GardConnectionError", "Database.connect: MySQL init failed");
                return Value::makeNull();
            }

            // Set connection timeout
            unsigned int timeout = 10;
            mysql_options(conn->mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

            if (!mysql_real_connect(conn->mysql, host.c_str(),
                                    user.empty() ? nullptr : user.c_str(),
                                    pass.empty() ? nullptr : pass.c_str(),
                                    database.empty() ? nullptr : database.c_str(),
                                    port, nullptr, 0)) {
                std::string err = mysql_error(conn->mysql);
                mysql_close(conn->mysql);
                conn->mysql = nullptr;
                vm.throwError("GardConnectionError", "Database.connect: MySQL connection failed: " + err);
                return Value::makeNull();
            }

            // Set UTF-8 charset
            mysql_set_character_set(conn->mysql, "utf8mb4");
        } else if (connStr.find("mongodb://") == 0 || connStr.find("mongodb+srv://") == 0) {
            conn->driver = "mongodb";
            // Initialize mongoc if not done
            if (!g_mongoInitialized) {
                mongoc_init();
                g_mongoInitialized = true;
            }
            conn->mongo = mongoc_client_new(connStr.c_str());
            if (!conn->mongo) {
                vm.throwError("GardConnectionError", "Database.connect: MongoDB client creation failed");
                return Value::makeNull();
            }
            // Extract database name from URL (after last /)
            std::string dbName = "test";
            size_t lastSlash = connStr.rfind('/');
            if (lastSlash != std::string::npos && lastSlash > 10) {
                dbName = connStr.substr(lastSlash + 1);
                size_t qPos = dbName.find('?');
                if (qPos != std::string::npos) dbName = dbName.substr(0, qPos);
            }
            conn->database = dbName;

            // Ping to verify connection
            bson_t* cmd = BCON_NEW("ping", BCON_INT32(1));
            bson_t reply;
            bson_error_t error;
            bool ok = mongoc_client_command_simple(conn->mongo, "admin", cmd, nullptr, &reply, &error);
            bson_destroy(cmd);
            bson_destroy(&reply);
            if (!ok) {
                std::string err = error.message;
                mongoc_client_destroy(conn->mongo);
                conn->mongo = nullptr;
                vm.throwError("GardConnectionError", "Database.connect: MongoDB connection failed: " + err);
                return Value::makeNull();
            }
            conn->mongoDb = mongoc_client_get_database(conn->mongo, dbName.c_str());
        } else {
            vm.throwError("GardConnectionError", "Database.connect: unsupported connection string format. Use sqlite://path, postgres://host/db, mysql://host/db, or mongodb://host/db");
            return Value::makeNull();
        }

        int id = g_nextDbId++;
        { std::lock_guard<std::mutex> lock(g_dbMutex); g_dbConnections[id] = conn; }

        Value dbObj = Value::makeObject("Database");
        dbObj.objVal->fields["_id"] = Value::makeInt(id);
        dbObj.objVal->fields["driver"] = Value::makeString(conn->driver);
        dbObj.objVal->fields["url"] = Value::makeString(conn->url);
        dbObj.objVal->fields["connected"] = Value::makeBool(true);
        return dbObj;
    });

    // Database.execute(db, sql) — execute DDL/DML (CREATE, INSERT, UPDATE, DELETE)
    // Database.execute(db, sql, params?) — execute DDL/DML with optional parameterized bindings
    // Supports: CREATE, INSERT, UPDATE, DELETE, ALTER, DROP
    // Returns: QueryResult { changes, lastInsertId }
    vm.registerNative("Database.execute", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardQueryError", "Database.execute: requires db and SQL string"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_id"].toInt();
        std::shared_ptr<DbConnection> conn;
        { std::lock_guard<std::mutex> lock(g_dbMutex); auto it = g_dbConnections.find(id); if (it == g_dbConnections.end()) { vm.throwError("GardConnectionError", "Database.execute: connection not found"); return Value::makeNull(); } conn = it->second; }

        std::string sql = a[1].toString();

        // Extract optional params array
        bool hasParams = (a.size() >= 3 && a[2].type == ValueType::Array && a[2].arrVal && !a[2].arrVal->elements.empty());

        // --- MySQL ---
        if (conn->driver == "mysql" && conn->mysql) {
            if (hasParams) {
                // Prepared statement with parameter bindings
                MYSQL_STMT* stmt = mysql_stmt_init(conn->mysql);
                if (!stmt) { vm.throwError("GardQueryError", "Database.execute: MySQL stmt init failed"); return Value::makeNull(); }
                if (mysql_stmt_prepare(stmt, sql.c_str(), (unsigned long)sql.size()) != 0) {
                    std::string err = mysql_stmt_error(stmt);
                    mysql_stmt_close(stmt);
                    vm.throwError("GardQueryError", "Database.execute: " + err + " (SQL: " + sql + ")");
                    return Value::makeNull();
                }
                int paramCount = (int)a[2].arrVal->elements.size();
                std::vector<MYSQL_BIND> binds(paramCount);
                std::vector<std::string> strBufs(paramCount);
                std::vector<long long> intBufs(paramCount);
                std::vector<double> dblBufs(paramCount);
                std::memset(binds.data(), 0, sizeof(MYSQL_BIND) * paramCount);
                for (int i = 0; i < paramCount; i++) {
                    Value& param = a[2].arrVal->elements[i];
                    switch (param.type) {
                        case ValueType::Int:
                            intBufs[i] = param.intVal;
                            binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
                            binds[i].buffer = &intBufs[i];
                            break;
                        case ValueType::Double:
                            dblBufs[i] = param.doubleVal;
                            binds[i].buffer_type = MYSQL_TYPE_DOUBLE;
                            binds[i].buffer = &dblBufs[i];
                            break;
                        case ValueType::String:
                            strBufs[i] = param.toString();
                            binds[i].buffer_type = MYSQL_TYPE_STRING;
                            binds[i].buffer = (void*)strBufs[i].c_str();
                            binds[i].buffer_length = (unsigned long)strBufs[i].size();
                            break;
                        case ValueType::Null:
                            binds[i].buffer_type = MYSQL_TYPE_NULL;
                            break;
                        default:
                            strBufs[i] = param.toString();
                            binds[i].buffer_type = MYSQL_TYPE_STRING;
                            binds[i].buffer = (void*)strBufs[i].c_str();
                            binds[i].buffer_length = (unsigned long)strBufs[i].size();
                            break;
                    }
                }
                mysql_stmt_bind_param(stmt, binds.data());
                if (mysql_stmt_execute(stmt) != 0) {
                    std::string err = mysql_stmt_error(stmt);
                    mysql_stmt_close(stmt);
                    vm.throwError("GardQueryError", "Database.execute: " + err + " (SQL: " + sql + ")");
                    return Value::makeNull();
                }
                conn->queryCount++;
                Value result = Value::makeObject("QueryResult");
                result.objVal->fields["changes"] = Value::makeInt(static_cast<int>(mysql_stmt_affected_rows(stmt)));
                result.objVal->fields["lastInsertId"] = Value::makeInt(static_cast<int>(mysql_stmt_insert_id(stmt)));
                mysql_stmt_close(stmt);
                return result;
            }
            // Non-parameterized execution
            if (mysql_query(conn->mysql, sql.c_str()) != 0) {
                vm.throwError("GardQueryError", "Database.execute: " + std::string(mysql_error(conn->mysql)) + " (SQL: " + sql + ")");
                return Value::makeNull();
            }
            // Drain any result set to prevent protocol desync (e.g., if user accidentally passes SELECT)
            MYSQL_RES* discard = mysql_store_result(conn->mysql);
            if (discard) mysql_free_result(discard);
            conn->queryCount++;
            Value result = Value::makeObject("QueryResult");
            result.objVal->fields["changes"] = Value::makeInt(static_cast<int>(mysql_affected_rows(conn->mysql)));
            result.objVal->fields["lastInsertId"] = Value::makeInt(static_cast<int>(mysql_insert_id(conn->mysql)));
            return result;
        }

        // --- PostgreSQL ---
        if (conn->driver == "postgres" && conn->pg) {
            PGresult* res;
            if (hasParams) {
                // Parameterized execution: convert ? placeholders to $1, $2, ...
                std::string pgSql = sql;
                int paramIdx = 1;
                size_t pos = 0;
                while ((pos = pgSql.find('?', pos)) != std::string::npos) {
                    std::string placeholder = "$" + std::to_string(paramIdx++);
                    pgSql.replace(pos, 1, placeholder);
                    pos += placeholder.size();
                }
                int nParams = (int)a[2].arrVal->elements.size();
                std::vector<std::string> paramStrs(nParams);
                std::vector<const char*> paramVals(nParams);
                std::vector<int> paramNulls(nParams, 0);
                for (int i = 0; i < nParams; i++) {
                    if (a[2].arrVal->elements[i].type == ValueType::Null) {
                        paramVals[i] = nullptr;
                    } else {
                        paramStrs[i] = a[2].arrVal->elements[i].toString();
                        paramVals[i] = paramStrs[i].c_str();
                    }
                }
                res = PQexecParams(conn->pg, pgSql.c_str(), nParams, nullptr, paramVals.data(), nullptr, nullptr, 0);
            } else {
                res = PQexec(conn->pg, sql.c_str());
            }
            ExecStatusType status = PQresultStatus(res);
            if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
                std::string err = PQerrorMessage(conn->pg);
                PQclear(res);
                vm.throwError("GardQueryError", "Database.execute: " + err + " (SQL: " + sql + ")");
                return Value::makeNull();
            }
            conn->queryCount++;
            Value result = Value::makeObject("QueryResult");
            char* affected = PQcmdTuples(res);
            result.objVal->fields["changes"] = Value::makeInt(affected && *affected ? std::atoi(affected) : 0);
            // Extract lastInsertId from RETURNING clause if present
            int lastId = 0;
            if (status == PGRES_TUPLES_OK && PQntuples(res) > 0 && PQnfields(res) > 0) {
                // If query has RETURNING, first column of first row is the returned value
                char* val = PQgetvalue(res, 0, 0);
                if (val && *val) lastId = std::atoi(val);
            }
            result.objVal->fields["lastInsertId"] = Value::makeInt(lastId);
            PQclear(res);
            return result;
        }

        // --- MongoDB ---
        if (conn->driver == "mongodb") {
            // MongoDB doesn't use SQL — support basic collection management commands
            // Supported: "CREATE COLLECTION <name>", "DROP COLLECTION <name>"
            std::string upper = sql;
            for (auto& c : upper) c = std::toupper(c);

            if (upper.find("CREATE COLLECTION") == 0 || upper.find("CREATE TABLE") == 0) {
                // Extract collection name (last word)
                size_t lastSpace = sql.rfind(' ');
                if (lastSpace != std::string::npos) {
                    std::string colName = sql.substr(lastSpace + 1);
                    // Remove trailing semicolons/whitespace
                    while (!colName.empty() && (colName.back() == ';' || colName.back() == ' ')) colName.pop_back();
                    bson_error_t error;
                    mongoc_database_create_collection(conn->mongoDb, colName.c_str(), nullptr, &error);
                    // Ignore "already exists" errors
                }
                conn->queryCount++;
                Value result = Value::makeObject("QueryResult");
                result.objVal->fields["changes"] = Value::makeInt(0);
                result.objVal->fields["lastInsertId"] = Value::makeInt(0);
                return result;
            } else if (upper.find("DROP COLLECTION") == 0 || upper.find("DROP TABLE") == 0) {
                size_t lastSpace = sql.rfind(' ');
                if (lastSpace != std::string::npos) {
                    std::string colName = sql.substr(lastSpace + 1);
                    while (!colName.empty() && (colName.back() == ';' || colName.back() == ' ')) colName.pop_back();
                    mongoc_collection_t* col = mongoc_client_get_collection(conn->mongo, conn->database.c_str(), colName.c_str());
                    bson_error_t error;
                    mongoc_collection_drop(col, &error);
                    mongoc_collection_destroy(col);
                }
                conn->queryCount++;
                Value result = Value::makeObject("QueryResult");
                result.objVal->fields["changes"] = Value::makeInt(0);
                result.objVal->fields["lastInsertId"] = Value::makeInt(0);
                return result;
            }

            vm.throwError("GardQueryError", "Database.execute: MongoDB does not support raw SQL. Use ORM methods or supported commands (CREATE COLLECTION, DROP COLLECTION)");
            return Value::makeNull();
        }

        // --- SQLite ---
        if (hasParams) {
            // Prepared statement with parameter bindings (prevents SQL injection)
            sqlite3_stmt* stmt = nullptr;
            int rc = sqlite3_prepare_v2(conn->db, sql.c_str(), -1, &stmt, nullptr);
            if (rc != SQLITE_OK) {
                vm.throwError("GardQueryError", "Database.execute: " + std::string(sqlite3_errmsg(conn->db)) + " (SQL: " + sql + ")");
                return Value::makeNull();
            }
            for (int i = 0; i < (int)a[2].arrVal->elements.size(); i++) {
                Value& param = a[2].arrVal->elements[i];
                int idx = i + 1;
                switch (param.type) {
                    case ValueType::Int: sqlite3_bind_int(stmt, idx, param.intVal); break;
                    case ValueType::Long: sqlite3_bind_int64(stmt, idx, param.longVal); break;
                    case ValueType::Double: sqlite3_bind_double(stmt, idx, param.doubleVal); break;
                    case ValueType::Float: sqlite3_bind_double(stmt, idx, param.floatVal); break;
                    case ValueType::Bool: sqlite3_bind_int(stmt, idx, param.boolVal ? 1 : 0); break;
                    case ValueType::Null: sqlite3_bind_null(stmt, idx); break;
                    case ValueType::String: {
                        std::string s = param.toString();
                        sqlite3_bind_text(stmt, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT);
                        break;
                    }
                    default: {
                        std::string s = param.toString();
                        sqlite3_bind_text(stmt, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT);
                        break;
                    }
                }
            }
            rc = sqlite3_step(stmt);
            if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                std::string err = sqlite3_errmsg(conn->db);
                sqlite3_finalize(stmt);
                vm.throwError("GardQueryError", "Database.execute: " + err + " (SQL: " + sql + ")");
                return Value::makeNull();
            }
            sqlite3_finalize(stmt);
            conn->queryCount++;
            Value result = Value::makeObject("QueryResult");
            result.objVal->fields["changes"] = Value::makeInt(sqlite3_changes(conn->db));
            result.objVal->fields["lastInsertId"] = Value::makeInt(static_cast<int>(sqlite3_last_insert_rowid(conn->db)));
            return result;
        }

        // Non-parameterized SQLite execution
        char* errMsg = nullptr;
        int rc = sqlite3_exec(conn->db, sql.c_str(), nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::string err = errMsg ? errMsg : "unknown error";
            if (errMsg) sqlite3_free(errMsg);
            vm.throwError("GardQueryError", "Database.execute: " + err + " (SQL: " + sql + ")");
            return Value::makeNull();
        }
        conn->queryCount++;
        Value result = Value::makeObject("QueryResult");
        result.objVal->fields["changes"] = Value::makeInt(sqlite3_changes(conn->db));
        result.objVal->fields["lastInsertId"] = Value::makeInt(static_cast<int>(sqlite3_last_insert_rowid(conn->db)));
        return result;
    });

    // Database.query(db, sql, params?) — parameterized SELECT query
    vm.registerNative("Database.query", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardQueryError", "Database.query: requires db and SQL string"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_id"].toInt();
        std::shared_ptr<DbConnection> conn;
        { std::lock_guard<std::mutex> lock(g_dbMutex); auto it = g_dbConnections.find(id); if (it == g_dbConnections.end()) { vm.throwError("GardConnectionError", "Database.query: connection not found"); return Value::makeNull(); } conn = it->second; }

        std::string sql = a[1].toString();

        // MySQL query path
        if (conn->driver == "mysql" && conn->mysql) {
            // For parameterized queries, use prepared statements
            if (a.size() >= 3 && a[2].type == ValueType::Array && a[2].arrVal && !a[2].arrVal->elements.empty()) {
                MYSQL_STMT* stmt = mysql_stmt_init(conn->mysql);
                if (!stmt) { vm.throwError("GardQueryError", "Database.query: MySQL stmt init failed"); return Value::makeNull(); }
                if (mysql_stmt_prepare(stmt, sql.c_str(), (unsigned long)sql.size()) != 0) {
                    std::string err = mysql_stmt_error(stmt);
                    mysql_stmt_close(stmt);
                    vm.throwError("GardQueryError", "Database.query: " + err + " (SQL: " + sql + ")");
                    return Value::makeNull();
                }

                // Bind parameters
                int paramCount = (int)a[2].arrVal->elements.size();
                std::vector<MYSQL_BIND> binds(paramCount);
                std::vector<std::string> strBufs(paramCount);
                std::vector<long long> intBufs(paramCount);
                std::vector<double> dblBufs(paramCount);
                std::memset(binds.data(), 0, sizeof(MYSQL_BIND) * paramCount);

                for (int i = 0; i < paramCount; i++) {
                    Value& param = a[2].arrVal->elements[i];
                    switch (param.type) {
                        case ValueType::Int:
                            intBufs[i] = param.intVal;
                            binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
                            binds[i].buffer = &intBufs[i];
                            break;
                        case ValueType::Double:
                            dblBufs[i] = param.doubleVal;
                            binds[i].buffer_type = MYSQL_TYPE_DOUBLE;
                            binds[i].buffer = &dblBufs[i];
                            break;
                        case ValueType::String:
                            strBufs[i] = param.toString();
                            binds[i].buffer_type = MYSQL_TYPE_STRING;
                            binds[i].buffer = (void*)strBufs[i].c_str();
                            binds[i].buffer_length = (unsigned long)strBufs[i].size();
                            break;
                        case ValueType::Null:
                            binds[i].buffer_type = MYSQL_TYPE_NULL;
                            break;
                        default:
                            strBufs[i] = param.toString();
                            binds[i].buffer_type = MYSQL_TYPE_STRING;
                            binds[i].buffer = (void*)strBufs[i].c_str();
                            binds[i].buffer_length = (unsigned long)strBufs[i].size();
                            break;
                    }
                }
                mysql_stmt_bind_param(stmt, binds.data());
                if (mysql_stmt_execute(stmt) != 0) {
                    std::string err = mysql_stmt_error(stmt);
                    mysql_stmt_close(stmt);
                    vm.throwError("GardQueryError", "Database.query: " + err);
                    return Value::makeNull();
                }
                // For prepared statements, use store_result + fetch
                mysql_stmt_store_result(stmt);
                MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
                Value rows = Value::makeArray();
                if (meta) {
                    int colCount = mysql_num_fields(meta);
                    MYSQL_FIELD* fields = mysql_fetch_fields(meta);
                    // Allocate result binds
                    std::vector<MYSQL_BIND> resBind(colCount);
                    std::vector<std::vector<char>> resBufs(colCount, std::vector<char>(1024));
                    std::vector<unsigned long> resLens(colCount);
                    std::vector<char> resNulls(colCount, 0);
                    std::memset(resBind.data(), 0, sizeof(MYSQL_BIND) * colCount);
                    for (int c = 0; c < colCount; c++) {
                        resBind[c].buffer_type = MYSQL_TYPE_STRING;
                        resBind[c].buffer = resBufs[c].data();
                        resBind[c].buffer_length = 1024;
                        resBind[c].length = &resLens[c];
                        resBind[c].is_null = reinterpret_cast<bool*>(&resNulls[c]);
                    }
                    mysql_stmt_bind_result(stmt, resBind.data());
                    while (mysql_stmt_fetch(stmt) == 0) {
                        Value row = Value::makeObject("Row");
                        for (int c = 0; c < colCount; c++) {
                            std::string colName = fields[c].name;
                            if (resNulls[c]) { row.objVal->fields[colName] = Value::makeNull(); }
                            else { row.objVal->fields[colName] = Value::makeString(std::string(resBufs[c].data(), resLens[c])); }
                        }
                        rows.arrVal->elements.push_back(row);
                    }
                    mysql_free_result(meta);
                }
                mysql_stmt_close(stmt);
                conn->queryCount++;
                return rows;
            } else {
                // Simple query without params
                if (mysql_query(conn->mysql, sql.c_str()) != 0) {
                    vm.throwError("GardQueryError", "Database.query: " + std::string(mysql_error(conn->mysql)) + " (SQL: " + sql + ")");
                    return Value::makeNull();
                }
                MYSQL_RES* result = mysql_store_result(conn->mysql);
                Value rows = Value::makeArray();
                if (result) {
                    int colCount = mysql_num_fields(result);
                    MYSQL_FIELD* fields = mysql_fetch_fields(result);
                    MYSQL_ROW row;
                    while ((row = mysql_fetch_row(result))) {
                        unsigned long* lengths = mysql_fetch_lengths(result);
                        Value rowObj = Value::makeObject("Row");
                        for (int c = 0; c < colCount; c++) {
                            std::string colName = fields[c].name;
                            if (row[c] == nullptr) { rowObj.objVal->fields[colName] = Value::makeNull(); }
                            else { rowObj.objVal->fields[colName] = Value::makeString(std::string(row[c], lengths[c])); }
                        }
                        rows.arrVal->elements.push_back(rowObj);
                    }
                    mysql_free_result(result);
                }
                conn->queryCount++;
                return rows;
            }
        }

        // PostgreSQL query path
        if (conn->driver == "postgres" && conn->pg) {
            PGresult* res;
            if (a.size() >= 3 && a[2].type == ValueType::Array && a[2].arrVal && !a[2].arrVal->elements.empty()) {
                // Parameterized query: convert ? to $1, $2, etc.
                std::string pgSql = sql;
                int paramIdx = 1;
                size_t pos = 0;
                while ((pos = pgSql.find('?', pos)) != std::string::npos) {
                    std::string placeholder = "$" + std::to_string(paramIdx++);
                    pgSql.replace(pos, 1, placeholder);
                    pos += placeholder.size();
                }
                int nParams = (int)a[2].arrVal->elements.size();
                std::vector<std::string> paramStrs(nParams);
                std::vector<const char*> paramVals(nParams);
                for (int i = 0; i < nParams; i++) {
                    paramStrs[i] = a[2].arrVal->elements[i].toString();
                    paramVals[i] = paramStrs[i].c_str();
                }
                res = PQexecParams(conn->pg, pgSql.c_str(), nParams, nullptr, paramVals.data(), nullptr, nullptr, 0);
            } else {
                res = PQexec(conn->pg, sql.c_str());
            }
            if (PQresultStatus(res) != PGRES_TUPLES_OK) {
                std::string err = PQerrorMessage(conn->pg);
                PQclear(res);
                vm.throwError("GardQueryError", "Database.query: " + err + " (SQL: " + sql + ")");
                return Value::makeNull();
            }
            Value rows = Value::makeArray();
            int rowCount = PQntuples(res);
            int colCount = PQnfields(res);
            for (int r = 0; r < rowCount; r++) {
                Value row = Value::makeObject("Row");
                for (int c = 0; c < colCount; c++) {
                    std::string colName = PQfname(res, c);
                    if (PQgetisnull(res, r, c)) { row.objVal->fields[colName] = Value::makeNull(); }
                    else {
                        char* val = PQgetvalue(res, r, c);
                        // Try to detect type
                        Oid type = PQftype(res, c);
                        if (type == 23 || type == 20 || type == 21) { // int4, int8, int2
                            row.objVal->fields[colName] = Value::makeInt(std::atoi(val));
                        } else if (type == 700 || type == 701) { // float4, float8
                            row.objVal->fields[colName] = Value::makeDouble(std::atof(val));
                        } else if (type == 16) { // bool
                            row.objVal->fields[colName] = Value::makeBool(val[0] == 't');
                        } else {
                            row.objVal->fields[colName] = Value::makeString(val);
                        }
                    }
                }
                rows.arrVal->elements.push_back(row);
            }
            PQclear(res);
            conn->queryCount++;
            return rows;
        }

        // MongoDB query path (not SQL-based)
        if (conn->driver == "mongodb") {
            vm.throwError("GardQueryError", "Database.query: MongoDB does not support SQL queries. Use ORM methods");
            return Value::makeNull();
        }

        // SQLite query path
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(conn->db, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            vm.throwError("GardQueryError", "Database.query: " + std::string(sqlite3_errmsg(conn->db)) + " (SQL: " + sql + ")");
            return Value::makeNull();
        }

        // Bind parameters if provided
        if (a.size() >= 3 && a[2].type == ValueType::Array && a[2].arrVal) {
            for (int i = 0; i < (int)a[2].arrVal->elements.size(); i++) {
                Value& param = a[2].arrVal->elements[i];
                switch (param.type) {
                    case ValueType::Int: sqlite3_bind_int(stmt, i + 1, param.intVal); break;
                    case ValueType::Long: sqlite3_bind_int64(stmt, i + 1, param.longVal); break;
                    case ValueType::Double: sqlite3_bind_double(stmt, i + 1, param.doubleVal); break;
                    case ValueType::Float: sqlite3_bind_double(stmt, i + 1, param.floatVal); break;
                    case ValueType::String: {
                        std::string s = param.toString();
                        sqlite3_bind_text(stmt, i + 1, s.c_str(), (int)s.size(), SQLITE_TRANSIENT);
                        break;
                    }
                    case ValueType::Null: sqlite3_bind_null(stmt, i + 1); break;
                    case ValueType::Bool: sqlite3_bind_int(stmt, i + 1, param.boolVal ? 1 : 0); break;
                    default: sqlite3_bind_text(stmt, i + 1, param.toString().c_str(), -1, SQLITE_TRANSIENT); break;
                }
            }
        }

        // Execute and collect rows
        Value rows = Value::makeArray();
        int colCount = sqlite3_column_count(stmt);
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            Value row = Value::makeObject("Row");
            for (int col = 0; col < colCount; col++) {
                std::string colName = sqlite3_column_name(stmt, col);
                int colType = sqlite3_column_type(stmt, col);
                switch (colType) {
                    case SQLITE_INTEGER: row.objVal->fields[colName] = Value::makeInt(sqlite3_column_int(stmt, col)); break;
                    case SQLITE_FLOAT: row.objVal->fields[colName] = Value::makeDouble(sqlite3_column_double(stmt, col)); break;
                    case SQLITE_TEXT: row.objVal->fields[colName] = Value::makeString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, col))); break;
                    case SQLITE_NULL: row.objVal->fields[colName] = Value::makeNull(); break;
                    default: row.objVal->fields[colName] = Value::makeString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, col))); break;
                }
            }
            rows.arrVal->elements.push_back(row);
        }
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            vm.throwError("GardQueryError", "Database.query: " + std::string(sqlite3_errmsg(conn->db)));
            return Value::makeNull();
        }
        conn->queryCount++;
        return rows;
    });

    // Database.transaction(db, callback) — begin/commit/rollback
    vm.registerNative("Database.beginTransaction", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardTransactionError", "Database.beginTransaction: requires db"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_id"].toInt();
        std::shared_ptr<DbConnection> conn;
        { std::lock_guard<std::mutex> lock(g_dbMutex); auto it = g_dbConnections.find(id); if (it == g_dbConnections.end()) { vm.throwError("GardConnectionError", "Database.beginTransaction: connection not found"); return Value::makeNull(); } conn = it->second; }
        if (conn->inTransaction) { vm.throwError("GardTransactionError", "Database.beginTransaction: already in a transaction"); return Value::makeNull(); }

        if (conn->driver == "mysql" && conn->mysql) {
            if (mysql_query(conn->mysql, "START TRANSACTION") != 0) {
                vm.throwError("GardTransactionError", "Database.beginTransaction: " + std::string(mysql_error(conn->mysql)));
                return Value::makeNull();
            }
        } else if (conn->driver == "postgres" && conn->pg) {
            PGresult* res = PQexec(conn->pg, "BEGIN");
            if (PQresultStatus(res) != PGRES_COMMAND_OK) {
                std::string err = PQerrorMessage(conn->pg); PQclear(res);
                vm.throwError("GardTransactionError", "Database.beginTransaction: " + err);
                return Value::makeNull();
            }
            PQclear(res);
        } else if (conn->driver == "mongodb") {
            vm.throwError("GardTransactionError", "Database.beginTransaction: MongoDB transactions require replica sets");
            return Value::makeNull();
        } else {
            char* errMsg = nullptr;
            int rc = sqlite3_exec(conn->db, "BEGIN TRANSACTION;", nullptr, nullptr, &errMsg);
            if (rc != SQLITE_OK) {
                std::string err = errMsg ? errMsg : "unknown"; if (errMsg) sqlite3_free(errMsg);
                vm.throwError("GardTransactionError", "Database.beginTransaction: " + err);
                return Value::makeNull();
            }
        }
        conn->inTransaction = true;
        return Value::makeBool(true);
    });

    vm.registerNative("Database.commit", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardTransactionError", "Database.commit: requires db"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_id"].toInt();
        std::shared_ptr<DbConnection> conn;
        { std::lock_guard<std::mutex> lock(g_dbMutex); auto it = g_dbConnections.find(id); if (it == g_dbConnections.end()) { vm.throwError("GardConnectionError", "connection not found"); return Value::makeNull(); } conn = it->second; }
        if (!conn->inTransaction) { vm.throwError("GardTransactionError", "Database.commit: no active transaction"); return Value::makeNull(); }

        if (conn->driver == "mysql" && conn->mysql) {
            if (mysql_query(conn->mysql, "COMMIT") != 0) {
                vm.throwError("GardTransactionError", "Database.commit: " + std::string(mysql_error(conn->mysql)));
                return Value::makeNull();
            }
        } else if (conn->driver == "postgres" && conn->pg) {
            PGresult* res = PQexec(conn->pg, "COMMIT");
            if (PQresultStatus(res) != PGRES_COMMAND_OK) { std::string err = PQerrorMessage(conn->pg); PQclear(res); vm.throwError("GardTransactionError", "Database.commit: " + err); return Value::makeNull(); }
            PQclear(res);
        } else {
            char* errMsg = nullptr;
            int rc = sqlite3_exec(conn->db, "COMMIT;", nullptr, nullptr, &errMsg);
            if (rc != SQLITE_OK) {
                std::string err = errMsg ? errMsg : "unknown"; if (errMsg) sqlite3_free(errMsg);
                vm.throwError("GardTransactionError", "Database.commit: " + err);
                return Value::makeNull();
            }
        }
        conn->inTransaction = false;
        return Value::makeBool(true);
    });

    vm.registerNative("Database.rollback", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardTransactionError", "Database.rollback: requires db"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_id"].toInt();
        std::shared_ptr<DbConnection> conn;
        { std::lock_guard<std::mutex> lock(g_dbMutex); auto it = g_dbConnections.find(id); if (it == g_dbConnections.end()) { vm.throwError("GardConnectionError", "connection not found"); return Value::makeNull(); } conn = it->second; }
        if (!conn->inTransaction) { vm.throwError("GardTransactionError", "Database.rollback: no active transaction"); return Value::makeNull(); }

        if (conn->driver == "mysql" && conn->mysql) {
            mysql_query(conn->mysql, "ROLLBACK");
        } else if (conn->driver == "postgres" && conn->pg) {
            PGresult* res = PQexec(conn->pg, "ROLLBACK"); PQclear(res);
        } else {
            char* errMsg = nullptr;
            sqlite3_exec(conn->db, "ROLLBACK;", nullptr, nullptr, &errMsg);
            if (errMsg) sqlite3_free(errMsg);
        }
        conn->inTransaction = false;
        return Value::makeBool(true);
    });

    // Database.close(db) — close connection
    vm.registerNative("Database.close", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardConnectionError", "Database.close: requires db"); return Value::makeNull(); }
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_dbMutex);
        auto it = g_dbConnections.find(id);
        if (it != g_dbConnections.end()) {
            if (it->second->db) sqlite3_close(it->second->db);
            if (it->second->mysql) mysql_close(it->second->mysql);
            if (it->second->pg) PQfinish(it->second->pg);
            if (it->second->mongoDb) mongoc_database_destroy(it->second->mongoDb);
            if (it->second->mongo) mongoc_client_destroy(it->second->mongo);
            g_dbConnections.erase(it);
        }
        a[0].objVal->fields["connected"] = Value::makeBool(false);
        return Value::makeNull();
    });

    // Database.isConnected(db)
    vm.registerNative("Database.isConnected", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto it = a[0].objVal->fields.find("connected");
        return Value::makeBool(it != a[0].objVal->fields.end() && it->second.toBool());
    });

    // Database.getQueryCount(db)
    vm.registerNative("Database.getQueryCount", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        int id = a[0].objVal->fields["_id"].toInt();
        std::lock_guard<std::mutex> lock(g_dbMutex);
        auto it = g_dbConnections.find(id);
        if (it == g_dbConnections.end()) return Value::makeInt(0);
        return Value::makeInt(it->second->queryCount);
    });

    // Database.getDriver(db)
    vm.registerNative("Database.getDriver", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        auto it = a[0].objVal->fields.find("driver");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeString("");
    });

    // ===== ORM: @Table ActiveRecord Pattern =====

    // ORM.setConnection(db) — set the global DB connection for instance methods
    vm.registerNative("ORM.setConnection", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        ::g_globalDbId = a[0].objVal->fields["_id"].toInt();
        return Value::makeBool(true);
    });

    // ORM.getConnection() — get the global DB id
    vm.registerNative("ORM.getConnection", [](const std::vector<Value>& a) -> Value {
        return Value::makeInt(::g_globalDbId);
    });

    // Instance method: object.save() — uses global connection
    // The VM routes "save" on any object here via suffix match
    vm.registerNative("ORM.instanceSave", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardORMError", "save: not an object"); return Value::makeNull(); }
        if (::g_globalDbId < 0) { vm.throwError("GardORMError", "save: no database connection set. Call ORM.setConnection(db) first"); return Value::makeNull(); }
        std::shared_ptr<DbConnection> conn;
        { std::lock_guard<std::mutex> lock(g_dbMutex); auto it = g_dbConnections.find(::g_globalDbId); if (it == g_dbConnections.end()) { vm.throwError("GardConnectionError", "save: database connection lost"); return Value::makeNull(); } conn = it->second; }

        Value& obj = const_cast<Value&>(a[0]);
        std::string className = obj.objVal->className;
        std::string tableName = className;
        for (auto& c : tableName) c = std::tolower(c);
        auto tnIt = obj.objVal->fields.find("_tableName");
        if (tnIt != obj.objVal->fields.end() && tnIt->second.type == ValueType::String) tableName = tnIt->second.toString();

        auto idIt = obj.objVal->fields.find("id");
        bool isUpdate = (idIt != obj.objVal->fields.end() && idIt->second.toInt() > 0);

        // Auto-timestamp: set created_at/updated_at or createdAt/updatedAt
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char timeBuf[64];
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::string nowStr(timeBuf);

        if (!isUpdate) {
            // INSERT: set created_at and updated_at
            if (obj.objVal->fields.count("created_at") || obj.objVal->fields.count("createdAt")) {
                std::string key = obj.objVal->fields.count("created_at") ? "created_at" : "createdAt";
                obj.objVal->fields[key] = Value::makeString(nowStr);
            }
        }
        // Always set updated_at on save
        if (obj.objVal->fields.count("updated_at")) obj.objVal->fields["updated_at"] = Value::makeString(nowStr);
        else if (obj.objVal->fields.count("updatedAt")) obj.objVal->fields["updatedAt"] = Value::makeString(nowStr);

        // MongoDB save path
        if (conn->driver == "mongodb" && conn->mongo && conn->mongoDb) {
            mongoc_collection_t* col = mongoc_client_get_collection(conn->mongo, conn->database.c_str(), tableName.c_str());
            bson_t* doc = bson_new();
            for (auto& [k, v] : obj.objVal->fields) {
                if (k[0] == '_') continue;
                if (k == "id" && v.toInt() == 0 && v.type != ValueType::String) continue;
                if (v.type == ValueType::Int) BSON_APPEND_INT32(doc, k.c_str(), v.intVal);
                else if (v.type == ValueType::Double) BSON_APPEND_DOUBLE(doc, k.c_str(), v.doubleVal);
                else if (v.type == ValueType::Bool) BSON_APPEND_BOOL(doc, k.c_str(), v.boolVal);
                else if (v.type == ValueType::Null) BSON_APPEND_NULL(doc, k.c_str());
                else { std::string s = v.toString(); BSON_APPEND_UTF8(doc, k.c_str(), s.c_str()); }
            }
            if (isUpdate) {
                // Update by _id
                bson_t* filter = bson_new();
                auto idVal = obj.objVal->fields.find("id");
                if (idVal != obj.objVal->fields.end() && idVal->second.type == ValueType::String) {
                    bson_oid_t oid; bson_oid_init_from_string(&oid, idVal->second.toString().c_str());
                    BSON_APPEND_OID(filter, "_id", &oid);
                } else {
                    BSON_APPEND_INT32(filter, "_id", idVal->second.toInt());
                }
                bson_t* update = BCON_NEW("$set", BCON_DOCUMENT(doc));
                bson_error_t error;
                if (!mongoc_collection_update_one(col, filter, update, nullptr, nullptr, &error)) {
                    bson_destroy(filter); bson_destroy(update); bson_destroy(doc); mongoc_collection_destroy(col);
                    vm.throwError("GardORMError", "save (mongodb update): " + std::string(error.message));
                    return Value::makeNull();
                }
                bson_destroy(filter); bson_destroy(update);
            } else {
                // Insert
                bson_error_t error;
                if (!mongoc_collection_insert_one(col, doc, nullptr, nullptr, &error)) {
                    bson_destroy(doc); mongoc_collection_destroy(col);
                    vm.throwError("GardORMError", "save (mongodb insert): " + std::string(error.message));
                    return Value::makeNull();
                }
                // Get the inserted _id (auto-generated ObjectId)
                bson_iter_t iter;
                if (bson_iter_init_find(&iter, doc, "_id") && BSON_ITER_HOLDS_OID(&iter)) {
                    char oidStr[25];
                    bson_oid_to_string(bson_iter_oid(&iter), oidStr);
                    obj.objVal->fields["id"] = Value::makeString(oidStr);
                }
            }
            bson_destroy(doc);
            mongoc_collection_destroy(col);
            conn->queryCount++;
            return obj;
        }

        std::vector<std::string> colNames;
        std::vector<Value> colValues;
        for (auto& [k, v] : obj.objVal->fields) {
            if (k[0] == '_') continue;
            if (isUpdate && k == "id") continue;
            colNames.push_back(k);
            // If value is an enum variant (Object with _ordinal and _name), store the name
            if (v.type == ValueType::Object && v.objVal && v.objVal->fields.count("_ordinal") && v.objVal->fields.count("_name")) {
                colValues.push_back(v.objVal->fields["_name"]);
            } else {
                colValues.push_back(v);
            }
        }

        if (isUpdate) {
            std::string sql = "UPDATE " + tableName + " SET ";
            for (size_t i = 0; i < colNames.size(); i++) { if (i > 0) sql += ", "; sql += colNames[i] + " = ?"; }
            sql += " WHERE id = ?";
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(conn->db, sql.c_str(), -1, &stmt, nullptr);
            for (size_t i = 0; i < colValues.size(); i++) {
                Value& v = colValues[i]; int idx = (int)i + 1;
                if (v.type == ValueType::Int) sqlite3_bind_int(stmt, idx, v.intVal);
                else if (v.type == ValueType::String) { std::string s = v.toString(); sqlite3_bind_text(stmt, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT); }
                else if (v.type == ValueType::Double) sqlite3_bind_double(stmt, idx, v.doubleVal);
                else if (v.type == ValueType::Bool) sqlite3_bind_int(stmt, idx, v.boolVal ? 1 : 0);
                else sqlite3_bind_null(stmt, idx);
            }
            sqlite3_bind_int(stmt, (int)colValues.size() + 1, idIt->second.toInt());
            int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) { vm.throwError("GardORMError", "save: " + std::string(sqlite3_errmsg(conn->db))); return Value::makeNull(); }
        } else {
            std::string colStr, placeholders;
            for (size_t i = 0; i < colNames.size(); i++) { if (i > 0) { colStr += ", "; placeholders += ", "; } colStr += colNames[i]; placeholders += "?"; }
            std::string sql = "INSERT INTO " + tableName + " (" + colStr + ") VALUES (" + placeholders + ")";
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(conn->db, sql.c_str(), -1, &stmt, nullptr);
            for (size_t i = 0; i < colValues.size(); i++) {
                Value& v = colValues[i]; int idx = (int)i + 1;
                if (v.type == ValueType::Int) sqlite3_bind_int(stmt, idx, v.intVal);
                else if (v.type == ValueType::String) { std::string s = v.toString(); sqlite3_bind_text(stmt, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT); }
                else if (v.type == ValueType::Double) sqlite3_bind_double(stmt, idx, v.doubleVal);
                else if (v.type == ValueType::Bool) sqlite3_bind_int(stmt, idx, v.boolVal ? 1 : 0);
                else sqlite3_bind_null(stmt, idx);
            }
            int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) { vm.throwError("GardORMError", "save: " + std::string(sqlite3_errmsg(conn->db))); return Value::makeNull(); }
            obj.objVal->fields["id"] = Value::makeInt((int)sqlite3_last_insert_rowid(conn->db));
        }
        conn->queryCount++;
        return obj;
    });

    // Instance method: object.delete() — uses global connection
    vm.registerNative("ORM.instanceDelete", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardORMError", "delete: not an object"); return Value::makeNull(); }
        if (::g_globalDbId < 0) { vm.throwError("GardORMError", "delete: no database connection set"); return Value::makeNull(); }
        std::shared_ptr<DbConnection> conn;
        { std::lock_guard<std::mutex> lock(g_dbMutex); auto it = g_dbConnections.find(::g_globalDbId); if (it == g_dbConnections.end()) { vm.throwError("GardConnectionError", "delete: connection lost"); return Value::makeNull(); } conn = it->second; }

        std::string className = a[0].objVal->className;
        std::string tableName = className;
        for (auto& c : tableName) c = std::tolower(c);
        auto idIt = a[0].objVal->fields.find("id");
        if (idIt == a[0].objVal->fields.end() || idIt->second.toInt() <= 0) { vm.throwError("GardORMError", "delete: object has no id"); return Value::makeNull(); }

        std::string sql = "DELETE FROM " + tableName + " WHERE id = ?";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn->db, sql.c_str(), -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, idIt->second.toInt());
        sqlite3_step(stmt); sqlite3_finalize(stmt);
        conn->queryCount++;
        return Value::makeBool(true);
    });

    // ===== ORM Relationships =====

    // ORM.hasMany(db, parent, childClass, foreignKey?)
    vm.registerNative("ORM.hasMany", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardORMError", "ORM.hasMany: requires db, parent, and child class"); return Value::makeNull(); }
        std::string childClass = a[2].toString(); std::string childTable = childClass; for (auto& c : childTable) c = std::tolower(c);
        std::string parentClass = a[1].objVal->className;
        std::string fk = a.size() >= 4 ? a[3].toString() : ""; if (fk.empty()) { fk = parentClass; fk[0] = std::tolower(fk[0]); fk += "Id"; }
        int parentId = a[1].objVal->fields["id"].toInt();
        std::string sql = "SELECT * FROM " + childTable + " WHERE " + fk + " = ?";
        Value params = Value::makeArray(); params.arrVal->elements.push_back(Value::makeInt(parentId));
        Value rows = vm.callNative("Database.query", {a[0], Value::makeString(sql), params});
        // Re-tag objects with correct className
        if (rows.type == ValueType::Array && rows.arrVal) {
            for (auto& row : rows.arrVal->elements) { if (row.objVal) row.objVal->className = childClass; }
        }
        return rows;
    });

    // ORM.hasOne(db, parent, childClass, foreignKey?)
    vm.registerNative("ORM.hasOne", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardORMError", "ORM.hasOne: requires db, parent, and child class"); return Value::makeNull(); }
        std::string childClass = a[2].toString(); std::string childTable = childClass; for (auto& c : childTable) c = std::tolower(c);
        std::string parentClass = a[1].objVal->className;
        std::string fk = a.size() >= 4 ? a[3].toString() : ""; if (fk.empty()) { fk = parentClass; fk[0] = std::tolower(fk[0]); fk += "Id"; }
        int parentId = a[1].objVal->fields["id"].toInt();
        std::string sql = "SELECT * FROM " + childTable + " WHERE " + fk + " = ? LIMIT 1";
        Value params = Value::makeArray(); params.arrVal->elements.push_back(Value::makeInt(parentId));
        Value rows = vm.callNative("Database.query", {a[0], Value::makeString(sql), params});
        if (rows.type == ValueType::Array && rows.arrVal && !rows.arrVal->elements.empty()) {
            Value& r = rows.arrVal->elements[0]; if (r.objVal) r.objVal->className = childClass; return r;
        }
        return Value::makeNull();
    });

    // ORM.belongsTo(db, child, parentClass, foreignKey?)
    vm.registerNative("ORM.belongsTo", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardORMError", "ORM.belongsTo: requires db, child, and parent class"); return Value::makeNull(); }
        std::string parentClass = a[2].toString(); std::string parentTable = parentClass; for (auto& c : parentTable) c = std::tolower(c);
        std::string fk = a.size() >= 4 ? a[3].toString() : ""; if (fk.empty()) { fk = parentClass; fk[0] = std::tolower(fk[0]); fk += "Id"; }
        auto fkIt = a[1].objVal->fields.find(fk);
        if (fkIt == a[1].objVal->fields.end()) { vm.throwError("GardORMError", "ORM.belongsTo: no foreign key '" + fk + "' on child"); return Value::makeNull(); }
        int parentId = fkIt->second.toInt();
        std::string sql = "SELECT * FROM " + parentTable + " WHERE id = ? LIMIT 1";
        Value params = Value::makeArray(); params.arrVal->elements.push_back(Value::makeInt(parentId));
        Value rows = vm.callNative("Database.query", {a[0], Value::makeString(sql), params});
        if (rows.type == ValueType::Array && rows.arrVal && !rows.arrVal->elements.empty()) {
            Value& r = rows.arrVal->elements[0]; if (r.objVal) r.objVal->className = parentClass; return r;
        }
        return Value::makeNull();
    });

    // ORM.manyToMany(db, obj, relatedClass, pivotTable, localKey?, foreignKey?)
    vm.registerNative("ORM.manyToMany", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardORMError", "ORM.manyToMany: requires db, object, related class, pivot table"); return Value::makeNull(); }
        std::string relClass = a[2].toString(); std::string relTable = relClass; for (auto& c : relTable) c = std::tolower(c);
        std::string pivot = a[3].toString();
        std::string objClass = a[1].objVal->className;
        std::string lk = a.size() >= 5 ? a[4].toString() : ""; if (lk.empty()) { lk = objClass; lk[0] = std::tolower(lk[0]); lk += "_id"; }
        std::string fk = a.size() >= 6 ? a[5].toString() : ""; if (fk.empty()) { fk = relClass; fk[0] = std::tolower(fk[0]); fk += "_id"; }
        int objId = a[1].objVal->fields["id"].toInt();
        std::string sql = "SELECT " + relTable + ".* FROM " + relTable + " INNER JOIN " + pivot + " ON " + relTable + ".id = " + pivot + "." + fk + " WHERE " + pivot + "." + lk + " = ?";
        Value params = Value::makeArray(); params.arrVal->elements.push_back(Value::makeInt(objId));
        Value rows = vm.callNative("Database.query", {a[0], Value::makeString(sql), params});
        if (rows.type == ValueType::Array && rows.arrVal) {
            for (auto& row : rows.arrVal->elements) { if (row.objVal) row.objVal->className = relClass; }
        }
        return rows;
    });

    // ORM.attach(db, obj, related, pivotTable, localKey?, foreignKey?)
    vm.registerNative("ORM.attach", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal || !a[1].objVal || !a[2].objVal) { vm.throwError("GardORMError", "ORM.attach: requires db, obj, related, pivot"); return Value::makeNull(); }
        std::string pivot = a[3].toString();
        std::string objClass = a[1].objVal->className; std::string relClass = a[2].objVal->className;
        std::string lk = a.size() >= 5 ? a[4].toString() : ""; if (lk.empty()) { lk = objClass; lk[0] = std::tolower(lk[0]); lk += "_id"; }
        std::string fk = a.size() >= 6 ? a[5].toString() : ""; if (fk.empty()) { fk = relClass; fk[0] = std::tolower(fk[0]); fk += "_id"; }
        std::string sql = "INSERT OR IGNORE INTO " + pivot + " (" + lk + ", " + fk + ") VALUES (" + std::to_string(a[1].objVal->fields["id"].toInt()) + ", " + std::to_string(a[2].objVal->fields["id"].toInt()) + ")";
        vm.callNative("Database.execute", {a[0], Value::makeString(sql)});
        return Value::makeBool(true);
    });

    // ORM.detach(db, obj, related, pivotTable, localKey?, foreignKey?)
    vm.registerNative("ORM.detach", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal || !a[1].objVal || !a[2].objVal) { vm.throwError("GardORMError", "ORM.detach: requires db, obj, related, pivot"); return Value::makeNull(); }
        std::string pivot = a[3].toString();
        std::string objClass = a[1].objVal->className; std::string relClass = a[2].objVal->className;
        std::string lk = a.size() >= 5 ? a[4].toString() : ""; if (lk.empty()) { lk = objClass; lk[0] = std::tolower(lk[0]); lk += "_id"; }
        std::string fk = a.size() >= 6 ? a[5].toString() : ""; if (fk.empty()) { fk = relClass; fk[0] = std::tolower(fk[0]); fk += "_id"; }
        std::string sql = "DELETE FROM " + pivot + " WHERE " + lk + " = " + std::to_string(a[1].objVal->fields["id"].toInt()) + " AND " + fk + " = " + std::to_string(a[2].objVal->fields["id"].toInt());
        vm.callNative("Database.execute", {a[0], Value::makeString(sql)});
        return Value::makeBool(true);
    });

    // ORM.save(object) — INSERT or UPDATE based on whether id is set
    // Reads className → looks up @Table annotation → builds SQL from fields
     vm.registerNative("ORM.persist", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) {
            vm.throwError("GardORMError", "ORM.persist: requires db connection and object");
            return Value::makeNull();
        }
        Value& dbVal = const_cast<Value&>(a[0]);
        Value& obj = const_cast<Value&>(a[1]);
        int dbId = dbVal.objVal->fields["_id"].toInt();

        std::shared_ptr<DbConnection> conn;
        { std::lock_guard<std::mutex> lock(g_dbMutex); auto it = g_dbConnections.find(dbId); if (it == g_dbConnections.end()) { vm.throwError("GardConnectionError", "ORM.save: no database connection"); return Value::makeNull(); } conn = it->second; }

        std::string className = obj.objVal->className;
        // Table name defaults to lowercase className
        std::string tableName = className;
        for (auto& c : tableName) c = std::tolower(c);
        // Check if _tableName override exists
        auto tnIt = obj.objVal->fields.find("_tableName");
        if (tnIt != obj.objVal->fields.end() && tnIt->second.type == ValueType::String) tableName = tnIt->second.toString();

        // Determine if INSERT or UPDATE (check if _id field > 0)
        auto idIt = obj.objVal->fields.find("id");
        bool isUpdate = (idIt != obj.objVal->fields.end() && idIt->second.toInt() > 0);

        // Collect fields (skip internal _ prefixed fields)
        std::string cols, vals, updates;
        std::vector<std::string> colNames;
        std::vector<Value> colValues;
        for (auto& [k, v] : obj.objVal->fields) {
            if (k[0] == '_') continue; // skip internal
            if (isUpdate && k == "id") continue; // don't update primary key
            colNames.push_back(k);
            colValues.push_back(v);
        }

        if (isUpdate) {
            // UPDATE tableName SET col1=val1, col2=val2 WHERE id = ?
            std::string sql = "UPDATE " + tableName + " SET ";
            for (size_t i = 0; i < colNames.size(); i++) {
                if (i > 0) sql += ", ";
                sql += colNames[i] + " = ?";
            }
            sql += " WHERE id = ?";

            sqlite3_stmt* stmt = nullptr;
            int rc = sqlite3_prepare_v2(conn->db, sql.c_str(), -1, &stmt, nullptr);
            if (rc != SQLITE_OK) { vm.throwError("GardORMError", "ORM.save (update): " + std::string(sqlite3_errmsg(conn->db))); return Value::makeNull(); }
            for (size_t i = 0; i < colValues.size(); i++) {
                Value& v = colValues[i];
                int idx = (int)i + 1;
                if (v.type == ValueType::Int) sqlite3_bind_int(stmt, idx, v.intVal);
                else if (v.type == ValueType::String) { std::string s = v.toString(); sqlite3_bind_text(stmt, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT); }
                else if (v.type == ValueType::Double) sqlite3_bind_double(stmt, idx, v.doubleVal);
                else if (v.type == ValueType::Bool) sqlite3_bind_int(stmt, idx, v.boolVal ? 1 : 0);
                else if (v.type == ValueType::Null) sqlite3_bind_null(stmt, idx);
                else { std::string s = v.toString(); sqlite3_bind_text(stmt, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT); }
            }
            sqlite3_bind_int(stmt, (int)colValues.size() + 1, idIt->second.toInt());
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) { vm.throwError("GardORMError", "ORM.save (update): " + std::string(sqlite3_errmsg(conn->db))); return Value::makeNull(); }
        } else {
            // INSERT INTO tableName (col1, col2) VALUES (?, ?)
            std::string colStr, placeholders;
            for (size_t i = 0; i < colNames.size(); i++) {
                if (i > 0) { colStr += ", "; placeholders += ", "; }
                colStr += colNames[i];
                placeholders += "?";
            }
            std::string sql = "INSERT INTO " + tableName + " (" + colStr + ") VALUES (" + placeholders + ")";

            sqlite3_stmt* stmt = nullptr;
            int rc = sqlite3_prepare_v2(conn->db, sql.c_str(), -1, &stmt, nullptr);
            if (rc != SQLITE_OK) { vm.throwError("GardORMError", "ORM.save (insert): " + std::string(sqlite3_errmsg(conn->db))); return Value::makeNull(); }
            for (size_t i = 0; i < colValues.size(); i++) {
                Value& v = colValues[i];
                int idx = (int)i + 1;
                if (v.type == ValueType::Int) sqlite3_bind_int(stmt, idx, v.intVal);
                else if (v.type == ValueType::String) { std::string s = v.toString(); sqlite3_bind_text(stmt, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT); }
                else if (v.type == ValueType::Double) sqlite3_bind_double(stmt, idx, v.doubleVal);
                else if (v.type == ValueType::Bool) sqlite3_bind_int(stmt, idx, v.boolVal ? 1 : 0);
                else if (v.type == ValueType::Null) sqlite3_bind_null(stmt, idx);
                else { std::string s = v.toString(); sqlite3_bind_text(stmt, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT); }
            }
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) { vm.throwError("GardORMError", "ORM.save (insert): " + std::string(sqlite3_errmsg(conn->db))); return Value::makeNull(); }
            // Set the auto-generated id on the object
            int lastId = (int)sqlite3_last_insert_rowid(conn->db);
            obj.objVal->fields["id"] = Value::makeInt(lastId);
        }
        conn->queryCount++;
        return obj;
    });

    // ORM.remove(db, object) — DELETE by id (explicit db version)
    vm.registerNative("ORM.remove", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardORMError", "ORM.remove: requires db and object"); return Value::makeNull(); }
        int dbId = a[0].objVal->fields["_id"].toInt();
        std::shared_ptr<DbConnection> conn;
        { std::lock_guard<std::mutex> lock(g_dbMutex); auto it = g_dbConnections.find(dbId); if (it == g_dbConnections.end()) { vm.throwError("GardConnectionError", "ORM.delete: no connection"); return Value::makeNull(); } conn = it->second; }

        std::string className = a[1].objVal->className;
        std::string tableName = className;
        for (auto& c : tableName) c = std::tolower(c);
        auto tnIt = a[1].objVal->fields.find("_tableName");
        if (tnIt != a[1].objVal->fields.end() && tnIt->second.type == ValueType::String) tableName = tnIt->second.toString();

        auto idIt = a[1].objVal->fields.find("id");
        if (idIt == a[1].objVal->fields.end() || idIt->second.toInt() <= 0) { vm.throwError("GardORMError", "ORM.delete: object has no id"); return Value::makeNull(); }

        std::string sql = "DELETE FROM " + tableName + " WHERE id = ?";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn->db, sql.c_str(), -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, idIt->second.toInt());
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        conn->queryCount++;
        return Value::makeBool(true);
    });

    // ORM.findAll(db, className) — SELECT * FROM table
    vm.registerNative("ORM.findAll", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardORMError", "ORM.findAll: requires db and class name"); return Value::makeNull(); }
        int dbId = a[0].objVal->fields["_id"].toInt();
        std::shared_ptr<DbConnection> conn;
        { std::lock_guard<std::mutex> lock(g_dbMutex); auto it = g_dbConnections.find(dbId); if (it == g_dbConnections.end()) { vm.throwError("GardConnectionError", "ORM.findAll: no connection"); return Value::makeNull(); } conn = it->second; }

        std::string className = a[1].toString();
        std::string tableName = className;
        for (auto& c : tableName) c = std::tolower(c);

        // MongoDB path
        if (conn->driver == "mongodb" && conn->mongo && conn->mongoDb) {
            mongoc_collection_t* col = mongoc_client_get_collection(conn->mongo, conn->database.c_str(), tableName.c_str());
            bson_t* filter = bson_new(); // empty filter = find all
            mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(col, filter, nullptr, nullptr);
            Value rows = Value::makeArray();
            const bson_t* doc;
            while (mongoc_cursor_next(cursor, &doc)) {
                Value row = Value::makeObject(className);
                row.objVal->fields["_tableName"] = Value::makeString(tableName);
                bson_iter_t iter;
                if (bson_iter_init(&iter, doc)) {
                    while (bson_iter_next(&iter)) {
                        std::string key = bson_iter_key(&iter);
                        if (key == "_id") {
                            // Convert ObjectId to string
                            if (BSON_ITER_HOLDS_OID(&iter)) {
                                char oidStr[25];
                                bson_oid_to_string(bson_iter_oid(&iter), oidStr);
                                row.objVal->fields["id"] = Value::makeString(oidStr);
                            } else {
                                row.objVal->fields["id"] = Value::makeInt(bson_iter_int32(&iter));
                            }
                        } else if (BSON_ITER_HOLDS_INT32(&iter)) {
                            row.objVal->fields[key] = Value::makeInt(bson_iter_int32(&iter));
                        } else if (BSON_ITER_HOLDS_INT64(&iter)) {
                            row.objVal->fields[key] = Value::makeInt(static_cast<int>(bson_iter_int64(&iter)));
                        } else if (BSON_ITER_HOLDS_DOUBLE(&iter)) {
                            row.objVal->fields[key] = Value::makeDouble(bson_iter_double(&iter));
                        } else if (BSON_ITER_HOLDS_UTF8(&iter)) {
                            uint32_t len;
                            row.objVal->fields[key] = Value::makeString(bson_iter_utf8(&iter, &len));
                        } else if (BSON_ITER_HOLDS_BOOL(&iter)) {
                            row.objVal->fields[key] = Value::makeBool(bson_iter_bool(&iter));
                        } else if (BSON_ITER_HOLDS_NULL(&iter)) {
                            row.objVal->fields[key] = Value::makeNull();
                        } else {
                            row.objVal->fields[key] = Value::makeString(bson_iter_utf8(&iter, nullptr) ? bson_iter_utf8(&iter, nullptr) : "");
                        }
                    }
                }
                rows.arrVal->elements.push_back(row);
            }
            mongoc_cursor_destroy(cursor);
            bson_destroy(filter);
            mongoc_collection_destroy(col);
            conn->queryCount++;
            return rows;
        }

        // PostgreSQL path
        if (conn->driver == "postgres" && conn->pg) {
            std::string sql = "SELECT * FROM " + tableName;
            PGresult* res = PQexec(conn->pg, sql.c_str());
            if (PQresultStatus(res) != PGRES_TUPLES_OK) { std::string err = PQerrorMessage(conn->pg); PQclear(res); vm.throwError("GardORMError", "ORM.findAll: " + err); return Value::makeNull(); }
            Value rows = Value::makeArray();
            int rowCount = PQntuples(res); int colCount = PQnfields(res);
            for (int r = 0; r < rowCount; r++) {
                Value row = Value::makeObject(className);
                row.objVal->fields["_tableName"] = Value::makeString(tableName);
                for (int c = 0; c < colCount; c++) {
                    std::string cn = PQfname(res, c);
                    if (PQgetisnull(res, r, c)) { row.objVal->fields[cn] = Value::makeNull(); }
                    else {
                        Oid type = PQftype(res, c);
                        char* val = PQgetvalue(res, r, c);
                        if (type == 23 || type == 20 || type == 21) row.objVal->fields[cn] = Value::makeInt(std::atoi(val));
                        else if (type == 700 || type == 701) row.objVal->fields[cn] = Value::makeDouble(std::atof(val));
                        else if (type == 16) row.objVal->fields[cn] = Value::makeBool(val[0] == 't');
                        else row.objVal->fields[cn] = Value::makeString(val);
                    }
                }
                rows.arrVal->elements.push_back(row);
            }
            PQclear(res); conn->queryCount++; return rows;
        }

        // MySQL path
        if (conn->driver == "mysql" && conn->mysql) {
            std::string sql = "SELECT * FROM " + tableName;
            if (mysql_query(conn->mysql, sql.c_str()) != 0) { vm.throwError("GardORMError", "ORM.findAll: " + std::string(mysql_error(conn->mysql))); return Value::makeNull(); }
            MYSQL_RES* result = mysql_store_result(conn->mysql);
            Value rows = Value::makeArray();
            if (result) {
                int colCount = mysql_num_fields(result); MYSQL_FIELD* fields = mysql_fetch_fields(result); MYSQL_ROW row;
                while ((row = mysql_fetch_row(result))) {
                    unsigned long* lengths = mysql_fetch_lengths(result);
                    Value rowObj = Value::makeObject(className);
                    rowObj.objVal->fields["_tableName"] = Value::makeString(tableName);
                    for (int c = 0; c < colCount; c++) {
                        std::string cn = fields[c].name;
                        if (row[c] == nullptr) rowObj.objVal->fields[cn] = Value::makeNull();
                        else rowObj.objVal->fields[cn] = Value::makeString(std::string(row[c], lengths[c]));
                    }
                    rows.arrVal->elements.push_back(rowObj);
                }
                mysql_free_result(result);
            }
            conn->queryCount++; return rows;
        }

        // SQLite path
        std::string sql = "SELECT * FROM " + tableName;
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(conn->db, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) { vm.throwError("GardORMError", "ORM.findAll: " + std::string(sqlite3_errmsg(conn->db))); return Value::makeNull(); }

        Value rows = Value::makeArray();
        int colCount = sqlite3_column_count(stmt);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Value row = Value::makeObject(className);
            row.objVal->fields["_tableName"] = Value::makeString(tableName);
            for (int col = 0; col < colCount; col++) {
                std::string colName = sqlite3_column_name(stmt, col);
                int colType = sqlite3_column_type(stmt, col);
                switch (colType) {
                    case SQLITE_INTEGER: row.objVal->fields[colName] = Value::makeInt(sqlite3_column_int(stmt, col)); break;
                    case SQLITE_FLOAT: row.objVal->fields[colName] = Value::makeDouble(sqlite3_column_double(stmt, col)); break;
                    case SQLITE_TEXT: row.objVal->fields[colName] = Value::makeString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, col))); break;
                    case SQLITE_NULL: row.objVal->fields[colName] = Value::makeNull(); break;
                    default: row.objVal->fields[colName] = Value::makeString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, col))); break;
                }
            }
            rows.arrVal->elements.push_back(row);
        }
        sqlite3_finalize(stmt);
        conn->queryCount++;
        return rows;
    });

    // ORM.findById(db, className, id) — SELECT * FROM table WHERE id = ?
    vm.registerNative("ORM.findById", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardORMError", "ORM.findById: requires db, class name, and id"); return Value::makeNull(); }
        int dbId = a[0].objVal->fields["_id"].toInt();
        std::shared_ptr<DbConnection> conn;
        { std::lock_guard<std::mutex> lock(g_dbMutex); auto it = g_dbConnections.find(dbId); if (it == g_dbConnections.end()) { vm.throwError("GardConnectionError", "ORM.findById: no connection"); return Value::makeNull(); } conn = it->second; }

        std::string className = a[1].toString();
        std::string tableName = className;
        for (auto& c : tableName) c = std::tolower(c);
        int targetId = a[2].toInt();

        std::string sql = "SELECT * FROM " + tableName + " WHERE id = ?";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn->db, sql.c_str(), -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, targetId);

        Value result = Value::makeNull();
        int colCount = sqlite3_column_count(stmt);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = Value::makeObject(className);
            result.objVal->fields["_tableName"] = Value::makeString(tableName);
            for (int col = 0; col < colCount; col++) {
                std::string colName = sqlite3_column_name(stmt, col);
                int colType = sqlite3_column_type(stmt, col);
                switch (colType) {
                    case SQLITE_INTEGER: result.objVal->fields[colName] = Value::makeInt(sqlite3_column_int(stmt, col)); break;
                    case SQLITE_FLOAT: result.objVal->fields[colName] = Value::makeDouble(sqlite3_column_double(stmt, col)); break;
                    case SQLITE_TEXT: result.objVal->fields[colName] = Value::makeString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, col))); break;
                    default: result.objVal->fields[colName] = Value::makeNull(); break;
                }
            }
        }
        sqlite3_finalize(stmt);
        conn->queryCount++;
        return result;
    });

    // ORM.findOne(db, className, conditions) — SELECT * FROM table WHERE ... LIMIT 1
    vm.registerNative("ORM.findOne", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardORMError", "ORM.findOne: requires db, class name, and conditions"); return Value::makeNull(); }
        int dbId = a[0].objVal->fields["_id"].toInt();
        std::shared_ptr<DbConnection> conn;
        { std::lock_guard<std::mutex> lock(g_dbMutex); auto it = g_dbConnections.find(dbId); if (it == g_dbConnections.end()) { vm.throwError("GardConnectionError", "ORM.findOne: no connection"); return Value::makeNull(); } conn = it->second; }

        std::string className = a[1].toString();
        std::string tableName = className;
        for (auto& c : tableName) c = std::tolower(c);

        // Build WHERE from conditions object/map
        std::string where;
        std::vector<Value> params;
        if (a[2].type == ValueType::Object && a[2].objVal) {
            for (auto& [k, v] : a[2].objVal->fields) { if (!where.empty()) where += " AND "; where += k + " = ?"; params.push_back(v); }
        } else if (a[2].type == ValueType::Map && a[2].mapVal) {
            for (auto& [k, v] : a[2].mapVal->entries) { if (!where.empty()) where += " AND "; where += k + " = ?"; params.push_back(v); }
        }

        std::string sql = "SELECT * FROM " + tableName + (where.empty() ? "" : " WHERE " + where) + " LIMIT 1";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(conn->db, sql.c_str(), -1, &stmt, nullptr);
        for (size_t i = 0; i < params.size(); i++) {
            if (params[i].type == ValueType::Int) sqlite3_bind_int(stmt, (int)i+1, params[i].intVal);
            else if (params[i].type == ValueType::String) { std::string s = params[i].toString(); sqlite3_bind_text(stmt, (int)i+1, s.c_str(), (int)s.size(), SQLITE_TRANSIENT); }
            else sqlite3_bind_text(stmt, (int)i+1, params[i].toString().c_str(), -1, SQLITE_TRANSIENT);
        }

        Value result = Value::makeNull();
        int colCount = sqlite3_column_count(stmt);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = Value::makeObject(className);
            result.objVal->fields["_tableName"] = Value::makeString(tableName);
            for (int col = 0; col < colCount; col++) {
                std::string colName = sqlite3_column_name(stmt, col);
                int colType = sqlite3_column_type(stmt, col);
                switch (colType) {
                    case SQLITE_INTEGER: result.objVal->fields[colName] = Value::makeInt(sqlite3_column_int(stmt, col)); break;
                    case SQLITE_FLOAT: result.objVal->fields[colName] = Value::makeDouble(sqlite3_column_double(stmt, col)); break;
                    case SQLITE_TEXT: result.objVal->fields[colName] = Value::makeString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, col))); break;
                    default: result.objVal->fields[colName] = Value::makeNull(); break;
                }
            }
        }
        sqlite3_finalize(stmt);
        conn->queryCount++;
        return result;
    });

    // ORM.createTable(db, className, fields) — CREATE TABLE from class structure
    // fields is an object: {colName: "type", ...} e.g. {name: "TEXT", age: "INTEGER"}
    vm.registerNative("ORM.createTable", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardORMError", "ORM.createTable: requires db, table name, and field definitions"); return Value::makeNull(); }
        int dbId = a[0].objVal->fields["_id"].toInt();
        std::shared_ptr<DbConnection> conn;
        { std::lock_guard<std::mutex> lock(g_dbMutex); auto it = g_dbConnections.find(dbId); if (it == g_dbConnections.end()) { vm.throwError("GardConnectionError", "ORM.createTable: no connection"); return Value::makeNull(); } conn = it->second; }

        std::string tableName = a[1].toString();
        for (auto& c : tableName) c = std::tolower(c);

        std::string sql = "CREATE TABLE IF NOT EXISTS " + tableName + " (id INTEGER PRIMARY KEY AUTOINCREMENT";
        // Add columns from fields object/map
        if (a[2].type == ValueType::Object && a[2].objVal) {
            for (auto& [k, v] : a[2].objVal->fields) { sql += ", " + k + " " + v.toString(); }
        } else if (a[2].type == ValueType::Map && a[2].mapVal) {
            for (auto& [k, v] : a[2].mapVal->entries) { sql += ", " + k + " " + v.toString(); }
        }
        sql += ")";

        char* errMsg = nullptr;
        int rc = sqlite3_exec(conn->db, sql.c_str(), nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::string err = errMsg ? errMsg : "unknown"; if (errMsg) sqlite3_free(errMsg);
            vm.throwError("GardORMError", "ORM.createTable: " + err);
            return Value::makeNull();
        }
        return Value::makeBool(true);
    });

    // ===== Dynamic Query Builder =====
    // Builds driver-agnostic queries that compile to SQLite/MySQL/PostgreSQL SQL

    // Query.table(tableName) — start a query builder
    vm.registerNative("Query.table", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        Value qb = Value::makeObject("QueryBuilder");
        qb.objVal->fields["_table"] = Value::makeString(a[0].toString());
        qb.objVal->fields["_type"] = Value::makeString("select");
        qb.objVal->fields["_columns"] = Value::makeArray();
        qb.objVal->fields["_wheres"] = Value::makeArray();
        qb.objVal->fields["_orderBy"] = Value::makeArray();
        qb.objVal->fields["_limit"] = Value::makeInt(-1);
        qb.objVal->fields["_offset"] = Value::makeInt(-1);
        qb.objVal->fields["_joins"] = Value::makeArray();
        qb.objVal->fields["_groupBy"] = Value::makeArray();
        qb.objVal->fields["_having"] = Value::makeString("");
        qb.objVal->fields["_values"] = Value::makeObject("Values");
        qb.objVal->fields["_sets"] = Value::makeObject("Sets");
        return qb;
    });

    // Query.select(qb, ...columns) — set columns to select
    vm.registerNative("Query.select", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_type"] = Value::makeString("select");
        auto& cols = a[0].objVal->fields["_columns"];
        if (!cols.arrVal) cols = Value::makeArray();
        for (size_t i = 1; i < a.size(); i++) cols.arrVal->elements.push_back(a[i]);
        return a[0];
    });

    // Query.where(qb, column, operator, value) — add WHERE condition
    vm.registerNative("Query.where", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) return Value::makeNull();
        auto& wheres = a[0].objVal->fields["_wheres"];
        if (!wheres.arrVal) wheres = Value::makeArray();
        Value cond = Value::makeObject("WhereClause");
        cond.objVal->fields["column"] = a[1];
        cond.objVal->fields["op"] = a[2];
        cond.objVal->fields["value"] = a[3];
        cond.objVal->fields["logic"] = Value::makeString("AND");
        wheres.arrVal->elements.push_back(cond);
        return a[0];
    });

    // Query.orWhere(qb, column, operator, value) — add OR WHERE condition
    vm.registerNative("Query.orWhere", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) return Value::makeNull();
        auto& wheres = a[0].objVal->fields["_wheres"];
        if (!wheres.arrVal) wheres = Value::makeArray();
        Value cond = Value::makeObject("WhereClause");
        cond.objVal->fields["column"] = a[1];
        cond.objVal->fields["op"] = a[2];
        cond.objVal->fields["value"] = a[3];
        cond.objVal->fields["logic"] = Value::makeString("OR");
        wheres.arrVal->elements.push_back(cond);
        return a[0];
    });

    // Query.orderBy(qb, column, direction) — add ORDER BY
    vm.registerNative("Query.orderBy", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        auto& orders = a[0].objVal->fields["_orderBy"];
        if (!orders.arrVal) orders = Value::makeArray();
        Value ord = Value::makeObject("OrderClause");
        ord.objVal->fields["column"] = a[1];
        ord.objVal->fields["dir"] = a.size() >= 3 ? a[2] : Value::makeString("ASC");
        orders.arrVal->elements.push_back(ord);
        return a[0];
    });

    // Query.limit(qb, count)
    vm.registerNative("Query.limit", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_limit"] = a[1];
        return a[0];
    });

    // Query.offset(qb, count)
    vm.registerNative("Query.offset", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_offset"] = a[1];
        return a[0];
    });

    // Query.join(qb, table, leftCol, op, rightCol) — INNER JOIN
    vm.registerNative("Query.join", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) return Value::makeNull();
        auto& joins = a[0].objVal->fields["_joins"];
        if (!joins.arrVal) joins = Value::makeArray();
        Value j = Value::makeObject("JoinClause");
        j.objVal->fields["type"] = Value::makeString("INNER");
        j.objVal->fields["table"] = a[1];
        j.objVal->fields["left"] = a[2];
        j.objVal->fields["op"] = a[3];
        j.objVal->fields["right"] = a[4];
        joins.arrVal->elements.push_back(j);
        return a[0];
    });

    // Query.leftJoin(qb, table, leftCol, op, rightCol)
    vm.registerNative("Query.leftJoin", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) return Value::makeNull();
        auto& joins = a[0].objVal->fields["_joins"];
        if (!joins.arrVal) joins = Value::makeArray();
        Value j = Value::makeObject("JoinClause");
        j.objVal->fields["type"] = Value::makeString("LEFT");
        j.objVal->fields["table"] = a[1];
        j.objVal->fields["left"] = a[2];
        j.objVal->fields["op"] = a[3];
        j.objVal->fields["right"] = a[4];
        joins.arrVal->elements.push_back(j);
        return a[0];
    });

    // Query.groupBy(qb, ...columns)
    vm.registerNative("Query.groupBy", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        auto& groups = a[0].objVal->fields["_groupBy"];
        if (!groups.arrVal) groups = Value::makeArray();
        for (size_t i = 1; i < a.size(); i++) groups.arrVal->elements.push_back(a[i]);
        return a[0];
    });

    // Query.insert(qb) — set type to insert
    vm.registerNative("Query.insert", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_type"] = Value::makeString("insert");
        return a[0];
    });

    // Query.update(qb) — set type to update
    vm.registerNative("Query.update", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_type"] = Value::makeString("update");
        return a[0];
    });

    // Query.delete(qb) — set type to delete
    vm.registerNative("Query.deleteFrom", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_type"] = Value::makeString("delete");
        return a[0];
    });

    // Query.set(qb, column, value) — set column value for INSERT/UPDATE
    vm.registerNative("Query.set", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeNull();
        auto& sets = a[0].objVal->fields["_sets"];
        if (!sets.objVal) sets = Value::makeObject("Sets");
        sets.objVal->fields[a[1].toString()] = a[2];
        return a[0];
    });

    // Query.whereIn(qb, column, values) — WHERE column IN (...)
    vm.registerNative("Query.whereIn", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeNull();
        auto& wheres = a[0].objVal->fields["_wheres"];
        if (!wheres.arrVal) wheres = Value::makeArray();
        Value cond = Value::makeObject("WhereClause");
        cond.objVal->fields["column"] = a[1];
        cond.objVal->fields["op"] = Value::makeString("IN");
        cond.objVal->fields["value"] = a[2]; // array of values
        cond.objVal->fields["logic"] = Value::makeString("AND");
        wheres.arrVal->elements.push_back(cond);
        return a[0];
    });

    // Query.whereNotIn(qb, column, values) — WHERE column NOT IN (...)
    vm.registerNative("Query.whereNotIn", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeNull();
        auto& wheres = a[0].objVal->fields["_wheres"];
        if (!wheres.arrVal) wheres = Value::makeArray();
        Value cond = Value::makeObject("WhereClause");
        cond.objVal->fields["column"] = a[1];
        cond.objVal->fields["op"] = Value::makeString("NOT IN");
        cond.objVal->fields["value"] = a[2];
        cond.objVal->fields["logic"] = Value::makeString("AND");
        wheres.arrVal->elements.push_back(cond);
        return a[0];
    });

    // Query.whereNull(qb, column) — WHERE column IS NULL
    vm.registerNative("Query.whereNull", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        auto& wheres = a[0].objVal->fields["_wheres"];
        if (!wheres.arrVal) wheres = Value::makeArray();
        Value cond = Value::makeObject("WhereClause");
        cond.objVal->fields["column"] = a[1];
        cond.objVal->fields["op"] = Value::makeString("IS NULL");
        cond.objVal->fields["value"] = Value::makeNull();
        cond.objVal->fields["logic"] = Value::makeString("AND");
        wheres.arrVal->elements.push_back(cond);
        return a[0];
    });

    // Query.whereNotNull(qb, column) — WHERE column IS NOT NULL
    vm.registerNative("Query.whereNotNull", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        auto& wheres = a[0].objVal->fields["_wheres"];
        if (!wheres.arrVal) wheres = Value::makeArray();
        Value cond = Value::makeObject("WhereClause");
        cond.objVal->fields["column"] = a[1];
        cond.objVal->fields["op"] = Value::makeString("IS NOT NULL");
        cond.objVal->fields["value"] = Value::makeNull();
        cond.objVal->fields["logic"] = Value::makeString("AND");
        wheres.arrVal->elements.push_back(cond);
        return a[0];
    });

    // Query.whereBetween(qb, column, min, max) — WHERE column BETWEEN min AND max
    vm.registerNative("Query.whereBetween", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) return Value::makeNull();
        auto& wheres = a[0].objVal->fields["_wheres"];
        if (!wheres.arrVal) wheres = Value::makeArray();
        Value cond = Value::makeObject("WhereClause");
        cond.objVal->fields["column"] = a[1];
        cond.objVal->fields["op"] = Value::makeString("BETWEEN");
        Value range = Value::makeArray();
        range.arrVal->elements.push_back(a[2]);
        range.arrVal->elements.push_back(a[3]);
        cond.objVal->fields["value"] = range;
        cond.objVal->fields["logic"] = Value::makeString("AND");
        wheres.arrVal->elements.push_back(cond);
        return a[0];
    });

    // Query.whereLike(qb, column, pattern) — WHERE column LIKE pattern
    vm.registerNative("Query.whereLike", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeNull();
        auto& wheres = a[0].objVal->fields["_wheres"];
        if (!wheres.arrVal) wheres = Value::makeArray();
        Value cond = Value::makeObject("WhereClause");
        cond.objVal->fields["column"] = a[1];
        cond.objVal->fields["op"] = Value::makeString("LIKE");
        cond.objVal->fields["value"] = a[2];
        cond.objVal->fields["logic"] = Value::makeString("AND");
        wheres.arrVal->elements.push_back(cond);
        return a[0];
    });

    // Query.having(qb, condition) — HAVING clause (raw SQL)
    vm.registerNative("Query.having", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_having"] = a[1];
        return a[0];
    });

    // Query.rightJoin(qb, table, leftCol, op, rightCol)
    vm.registerNative("Query.rightJoin", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 5 || !a[0].objVal) return Value::makeNull();
        auto& joins = a[0].objVal->fields["_joins"];
        if (!joins.arrVal) joins = Value::makeArray();
        Value j = Value::makeObject("JoinClause");
        j.objVal->fields["type"] = Value::makeString("RIGHT");
        j.objVal->fields["table"] = a[1];
        j.objVal->fields["left"] = a[2];
        j.objVal->fields["op"] = a[3];
        j.objVal->fields["right"] = a[4];
        joins.arrVal->elements.push_back(j);
        return a[0];
    });

    // Query.distinct(qb) — SELECT DISTINCT
    vm.registerNative("Query.distinct", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_distinct"] = Value::makeBool(true);
        return a[0];
    });

    // Query.count(qb, column?) — SELECT COUNT(column) or COUNT(*)
    vm.registerNative("Query.count", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        std::string col = a.size() >= 2 ? a[1].toString() : "*";
        a[0].objVal->fields["_columns"] = Value::makeArray();
        a[0].objVal->fields["_columns"].arrVal->elements.push_back(Value::makeString("COUNT(" + col + ")"));
        a[0].objVal->fields["_rawColumns"] = Value::makeBool(true);
        return a[0];
    });

    // Query.sum(qb, column) — SELECT SUM(column)
    vm.registerNative("Query.sum", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_columns"] = Value::makeArray();
        a[0].objVal->fields["_columns"].arrVal->elements.push_back(Value::makeString("SUM(" + a[1].toString() + ")"));
        a[0].objVal->fields["_rawColumns"] = Value::makeBool(true);
        return a[0];
    });

    // Query.avg(qb, column) — SELECT AVG(column)
    vm.registerNative("Query.avg", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_columns"] = Value::makeArray();
        a[0].objVal->fields["_columns"].arrVal->elements.push_back(Value::makeString("AVG(" + a[1].toString() + ")"));
        a[0].objVal->fields["_rawColumns"] = Value::makeBool(true);
        return a[0];
    });

    // Query.max(qb, column) — SELECT MAX(column)
    vm.registerNative("Query.max", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_columns"] = Value::makeArray();
        a[0].objVal->fields["_columns"].arrVal->elements.push_back(Value::makeString("MAX(" + a[1].toString() + ")"));
        a[0].objVal->fields["_rawColumns"] = Value::makeBool(true);
        return a[0];
    });

    // Query.min(qb, column) — SELECT MIN(column)
    vm.registerNative("Query.min", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_columns"] = Value::makeArray();
        a[0].objVal->fields["_columns"].arrVal->elements.push_back(Value::makeString("MIN(" + a[1].toString() + ")"));
        a[0].objVal->fields["_rawColumns"] = Value::makeBool(true);
        return a[0];
    });

    // Query.raw(qb, rawSQL) — inject raw SQL expression as column
    vm.registerNative("Query.raw", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        auto& cols = a[0].objVal->fields["_columns"];
        if (!cols.arrVal) cols = Value::makeArray();
        cols.arrVal->elements.push_back(a[1]);
        a[0].objVal->fields["_rawColumns"] = Value::makeBool(true);
        return a[0];
    });

    // Query.alias(qb, alias) — set table alias
    vm.registerNative("Query.alias", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_alias"] = a[1];
        return a[0];
    });

    // Query.upsert(qb, conflictColumns) — INSERT ... ON CONFLICT (SQLite/Postgres) or ON DUPLICATE KEY (MySQL)
    vm.registerNative("Query.upsert", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["_type"] = Value::makeString("upsert");
        if (a.size() >= 2) a[0].objVal->fields["_conflictCol"] = a[1];
        return a[0];
    });

    // Query.toSQL(qb, driver?) — compile query builder to SQL string
    // Handles dialect differences between SQLite, MySQL, PostgreSQL
    vm.registerNative("Query.toSQL", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        std::string driver = a.size() >= 2 ? a[1].toString() : "sqlite";
        auto& fields = a[0].objVal->fields;
        std::string table = fields["_table"].toString();
        std::string type = fields["_type"].toString();
        std::string sql;

        // Helper: quote identifier based on driver
        auto quoteId = [&driver](const std::string& id) -> std::string {
            if (driver == "mysql") return "`" + id + "`";
            if (driver == "postgres") return "\"" + id + "\"";
            return id; // SQLite doesn't require quoting
        };

        // Helper: quote value
        auto quoteVal = [](const Value& v) -> std::string {
            if (v.type == ValueType::Null) return "NULL";
            if (v.type == ValueType::Int || v.type == ValueType::Long) return std::to_string(v.toInt());
            if (v.type == ValueType::Double || v.type == ValueType::Float) return std::to_string(v.toDouble());
            if (v.type == ValueType::Bool) return v.toBool() ? "1" : "0";
            // String: escape single quotes
            std::string s = v.toString();
            std::string escaped;
            for (char c : s) { if (c == '\'') escaped += "''"; else escaped += c; }
            return "'" + escaped + "'";
        };

        if (type == "select") {
            sql = "SELECT ";
            // DISTINCT
            auto distIt = fields.find("_distinct");
            if (distIt != fields.end() && distIt->second.toBool()) sql += "DISTINCT ";
            auto& cols = fields["_columns"];
            auto rawIt = fields.find("_rawColumns");
            bool rawCols = (rawIt != fields.end() && rawIt->second.toBool());
            if (!cols.arrVal || cols.arrVal->elements.empty()) { sql += "*"; }
            else {
                for (size_t i = 0; i < cols.arrVal->elements.size(); i++) {
                    if (i > 0) sql += ", ";
                    if (rawCols) sql += cols.arrVal->elements[i].toString();
                    else sql += quoteId(cols.arrVal->elements[i].toString());
                }
            }
            sql += " FROM " + quoteId(table);
            // Table alias
            auto aliasIt = fields.find("_alias");
            if (aliasIt != fields.end() && aliasIt->second.type == ValueType::String && !aliasIt->second.toString().empty()) {
                sql += " AS " + quoteId(aliasIt->second.toString());
            }
        } else if (type == "insert") {
            sql = "INSERT INTO " + quoteId(table);
            auto& sets = fields["_sets"];
            if (sets.objVal && !sets.objVal->fields.empty()) {
                std::string cols, vals;
                bool first = true;
                for (auto& [k, v] : sets.objVal->fields) {
                    if (!first) { cols += ", "; vals += ", "; }
                    cols += quoteId(k);
                    vals += quoteVal(v);
                    first = false;
                }
                sql += " (" + cols + ") VALUES (" + vals + ")";
            }
        } else if (type == "update") {
            sql = "UPDATE " + quoteId(table) + " SET ";
            auto& sets = fields["_sets"];
            if (sets.objVal) {
                bool first = true;
                for (auto& [k, v] : sets.objVal->fields) {
                    if (!first) sql += ", ";
                    sql += quoteId(k) + " = " + quoteVal(v);
                    first = false;
                }
            }
        } else if (type == "delete") {
            sql = "DELETE FROM " + quoteId(table);
        } else if (type == "upsert") {
            // INSERT ... ON CONFLICT (SQLite/Postgres) or ON DUPLICATE KEY UPDATE (MySQL)
            sql = "INSERT INTO " + quoteId(table);
            auto& sets = fields["_sets"];
            if (sets.objVal && !sets.objVal->fields.empty()) {
                std::string cols, vals, updates;
                bool first = true;
                for (auto& [k, v] : sets.objVal->fields) {
                    if (!first) { cols += ", "; vals += ", "; updates += ", "; }
                    cols += quoteId(k);
                    vals += quoteVal(v);
                    updates += quoteId(k) + " = " + quoteVal(v);
                    first = false;
                }
                sql += " (" + cols + ") VALUES (" + vals + ")";
                auto conflictIt = fields.find("_conflictCol");
                std::string conflictCol = (conflictIt != fields.end()) ? conflictIt->second.toString() : "id";
                if (driver == "mysql") {
                    sql += " ON DUPLICATE KEY UPDATE " + updates;
                } else {
                    // SQLite and PostgreSQL
                    sql += " ON CONFLICT (" + quoteId(conflictCol) + ") DO UPDATE SET " + updates;
                }
            }
        }

        // JOINs
        auto& joins = fields["_joins"];
        if (joins.arrVal) {
            for (auto& j : joins.arrVal->elements) {
                if (!j.objVal) continue;
                sql += " " + j.objVal->fields["type"].toString() + " JOIN " +
                       quoteId(j.objVal->fields["table"].toString()) + " ON " +
                       quoteId(j.objVal->fields["left"].toString()) + " " +
                       j.objVal->fields["op"].toString() + " " +
                       quoteId(j.objVal->fields["right"].toString());
            }
        }

        // WHERE (handles =, >, <, IN, NOT IN, IS NULL, IS NOT NULL, BETWEEN, LIKE)
        auto& wheres = fields["_wheres"];
        if (wheres.arrVal && !wheres.arrVal->elements.empty()) {
            sql += " WHERE ";
            for (size_t i = 0; i < wheres.arrVal->elements.size(); i++) {
                auto& w = wheres.arrVal->elements[i];
                if (!w.objVal) continue;
                if (i > 0) sql += " " + w.objVal->fields["logic"].toString() + " ";
                std::string col = quoteId(w.objVal->fields["column"].toString());
                std::string op = w.objVal->fields["op"].toString();
                Value& val = w.objVal->fields["value"];

                if (op == "IN" || op == "NOT IN") {
                    sql += col + " " + op + " (";
                    if (val.arrVal) {
                        for (size_t vi = 0; vi < val.arrVal->elements.size(); vi++) {
                            if (vi > 0) sql += ", ";
                            sql += quoteVal(val.arrVal->elements[vi]);
                        }
                    }
                    sql += ")";
                } else if (op == "IS NULL" || op == "IS NOT NULL") {
                    sql += col + " " + op;
                } else if (op == "BETWEEN") {
                    if (val.arrVal && val.arrVal->elements.size() >= 2) {
                        sql += col + " BETWEEN " + quoteVal(val.arrVal->elements[0]) + " AND " + quoteVal(val.arrVal->elements[1]);
                    }
                } else if (op == "LIKE") {
                    sql += col + " LIKE " + quoteVal(val);
                } else {
                    sql += col + " " + op + " " + quoteVal(val);
                }
            }
        }

        // GROUP BY
        auto& groups = fields["_groupBy"];
        if (groups.arrVal && !groups.arrVal->elements.empty()) {
            sql += " GROUP BY ";
            for (size_t i = 0; i < groups.arrVal->elements.size(); i++) {
                if (i > 0) sql += ", ";
                sql += quoteId(groups.arrVal->elements[i].toString());
            }
        }

        // HAVING
        std::string having = fields["_having"].toString();
        if (!having.empty()) {
            sql += " HAVING " + having;
        }

        // ORDER BY
        auto& orders = fields["_orderBy"];
        if (orders.arrVal && !orders.arrVal->elements.empty()) {
            sql += " ORDER BY ";
            for (size_t i = 0; i < orders.arrVal->elements.size(); i++) {
                if (i > 0) sql += ", ";
                auto& o = orders.arrVal->elements[i];
                if (o.objVal) sql += quoteId(o.objVal->fields["column"].toString()) + " " + o.objVal->fields["dir"].toString();
            }
        }

        // LIMIT/OFFSET (dialect differences)
        int limit = fields["_limit"].toInt();
        int offset = fields["_offset"].toInt();
        if (limit >= 0) {
            sql += " LIMIT " + std::to_string(limit);
        }
        if (offset >= 0) {
            sql += " OFFSET " + std::to_string(offset);
        }

        return Value::makeString(sql);
    });

    // ===== Connection Pool =====
    // Real connection pooling with health checks, idle timeout, wait queue, and statistics

    // ConnectionPool.create(connectionString, options?) — create a new pool
    // options: { min: 2, max: 10, idleTimeout: 30000, maxLifetime: 300000, acquireTimeout: 5000, validateOnAcquire: true }
    vm.registerNative("ConnectionPool.create", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardPoolError", "ConnectionPool.create: connection string required"); return Value::makeNull(); }
        std::string connStr = a[0].toString();

        auto pool = std::make_shared<ConnectionPool>();
        pool->connectionString = connStr;

        // Parse options
        if (a.size() >= 2 && a[1].type == ValueType::Object && a[1].objVal) {
            auto& opts = a[1].objVal->fields;
            auto getInt = [&opts](const std::string& key, int def) -> int {
                auto it = opts.find(key);
                return (it != opts.end()) ? it->second.toInt() : def;
            };
            pool->minSize = getInt("min", 2);
            pool->maxSize = getInt("max", 10);
            pool->idleTimeoutMs = getInt("idleTimeout", 30000);
            pool->maxLifetimeMs = getInt("maxLifetime", 300000);
            pool->acquireTimeoutMs = getInt("acquireTimeout", 5000);
            pool->healthCheckIntervalMs = getInt("healthCheckInterval", 10000);
            auto valIt = opts.find("validateOnAcquire");
            if (valIt != opts.end()) pool->validateOnAcquire = valIt->second.toBool();
        } else if (a.size() >= 2 && a[1].type == ValueType::Map && a[1].mapVal) {
            auto& opts = a[1].mapVal->entries;
            auto getInt = [&opts](const std::string& key, int def) -> int {
                auto it = opts.find(key);
                return (it != opts.end()) ? it->second.toInt() : def;
            };
            pool->minSize = getInt("min", 2);
            pool->maxSize = getInt("max", 10);
            pool->idleTimeoutMs = getInt("idleTimeout", 30000);
            pool->maxLifetimeMs = getInt("maxLifetime", 300000);
            pool->acquireTimeoutMs = getInt("acquireTimeout", 5000);
            pool->healthCheckIntervalMs = getInt("healthCheckInterval", 10000);
            auto valIt = opts.find("validateOnAcquire");
            if (valIt != opts.end()) pool->validateOnAcquire = valIt->second.toBool();
        }

        // Validate pool config
        if (pool->minSize < 0) pool->minSize = 0;
        if (pool->maxSize < 1) { vm.throwError("GardPoolError", "ConnectionPool.create: max must be >= 1"); return Value::makeNull(); }
        if (pool->minSize > pool->maxSize) { vm.throwError("GardPoolError", "ConnectionPool.create: min cannot exceed max"); return Value::makeNull(); }

        // Pre-create minimum connections
        for (int i = 0; i < pool->minSize; i++) {
            std::vector<Value> connectArgs = {Value::makeString(connStr)};
            Value dbObj = vm.callNative("Database.connect", connectArgs);
            if (dbObj.type == ValueType::Null || !dbObj.objVal) {
                // Connection failed — destroy already-created connections and error
                for (auto& pc : pool->connections) {
                    if (pc->dbId >= 0) {
                        std::lock_guard<std::mutex> lock(g_dbMutex);
                        g_dbConnections.erase(pc->dbId);
                    }
                }
                vm.throwError("GardPoolError", "ConnectionPool.create: failed to create minimum connections for '" + connStr + "'");
                return Value::makeNull();
            }
            auto pc = std::make_shared<PooledConnection>();
            pc->dbId = dbObj.objVal->fields["_id"].toInt();
            pc->inUse = false;
            pc->lastUsed = std::chrono::steady_clock::now();
            pc->createdAt = std::chrono::steady_clock::now();
            pc->healthy = true;
            pool->connections.push_back(pc);
            pool->totalCreated++;
        }

        int poolId = g_nextPoolId++;
        { std::lock_guard<std::mutex> lock(g_poolMutex); g_pools[poolId] = pool; }

        Value poolObj = Value::makeObject("ConnectionPool");
        poolObj.objVal->fields["_poolId"] = Value::makeInt(poolId);
        poolObj.objVal->fields["connectionString"] = Value::makeString(connStr);
        poolObj.objVal->fields["min"] = Value::makeInt(pool->minSize);
        poolObj.objVal->fields["max"] = Value::makeInt(pool->maxSize);
        poolObj.objVal->fields["size"] = Value::makeInt((int)pool->connections.size());
        return poolObj;
    });

    // ConnectionPool.acquire(pool) — get a connection from the pool (blocks up to acquireTimeout)
    vm.registerNative("ConnectionPool.acquire", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardPoolError", "ConnectionPool.acquire: requires pool object"); return Value::makeNull(); }
        int poolId = a[0].objVal->fields["_poolId"].toInt();
        std::shared_ptr<ConnectionPool> pool;
        { std::lock_guard<std::mutex> lock(g_poolMutex); auto it = g_pools.find(poolId); if (it == g_pools.end()) { vm.throwError("GardPoolError", "ConnectionPool.acquire: pool not found"); return Value::makeNull(); } pool = it->second; }

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(pool->acquireTimeoutMs);

        std::unique_lock<std::mutex> lock(pool->mutex);

        while (true) {
            // 1. Try to find an idle healthy connection
            for (auto& pc : pool->connections) {
                if (!pc->inUse && pc->healthy) {
                    // Check max lifetime
                    auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - pc->createdAt).count();
                    if (age > pool->maxLifetimeMs) {
                        // Connection expired — mark unhealthy, will be cleaned up
                        pc->healthy = false;
                        continue;
                    }
                    // Check idle timeout
                    auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - pc->lastUsed).count();
                    if (idle > pool->idleTimeoutMs) {
                        pc->healthy = false;
                        continue;
                    }
                    // Validate on acquire if enabled
                    if (pool->validateOnAcquire) {
                        std::shared_ptr<DbConnection> conn;
                        { std::lock_guard<std::mutex> dbLock(g_dbMutex); auto it = g_dbConnections.find(pc->dbId); if (it != g_dbConnections.end()) conn = it->second; }
                        if (!conn) { pc->healthy = false; continue; }
                        // Health check: simple query
                        bool valid = false;
                        if (conn->driver == "sqlite" && conn->db) {
                            char* err = nullptr;
                            int rc = sqlite3_exec(conn->db, "SELECT 1", nullptr, nullptr, &err);
                            if (rc == SQLITE_OK) valid = true;
                            if (err) sqlite3_free(err);
                        } else if (conn->driver == "mysql" && conn->mysql) {
                            valid = (mysql_ping(conn->mysql) == 0);
                        } else if (conn->driver == "postgres" && conn->pg) {
                            PGresult* res = PQexec(conn->pg, "SELECT 1");
                            valid = (PQresultStatus(res) == PGRES_TUPLES_OK);
                            PQclear(res);
                        } else if (conn->driver == "mongodb" && conn->mongo) {
                            bson_t* cmd = BCON_NEW("ping", BCON_INT32(1));
                            bson_t reply;
                            bson_error_t error;
                            valid = mongoc_client_command_simple(conn->mongo, "admin", cmd, nullptr, &reply, &error);
                            bson_destroy(cmd);
                            bson_destroy(&reply);
                        }
                        if (!valid) {
                            pc->healthy = false;
                            pool->totalHealthChecksFailed++;
                            continue;
                        }
                    }
                    // Acquire this connection
                    pc->inUse = true;
                    pc->lastUsed = std::chrono::steady_clock::now();
                    pool->totalAcquired++;

                    // Return the db object
                    std::shared_ptr<DbConnection> conn;
                    { std::lock_guard<std::mutex> dbLock(g_dbMutex); auto it = g_dbConnections.find(pc->dbId); if (it != g_dbConnections.end()) conn = it->second; }
                    Value dbObj = Value::makeObject("Database");
                    dbObj.objVal->fields["_id"] = Value::makeInt(pc->dbId);
                    dbObj.objVal->fields["_poolId"] = Value::makeInt(poolId);
                    dbObj.objVal->fields["driver"] = Value::makeString(conn ? conn->driver : "unknown");
                    dbObj.objVal->fields["url"] = Value::makeString(pool->connectionString);
                    dbObj.objVal->fields["connected"] = Value::makeBool(true);
                    dbObj.objVal->fields["pooled"] = Value::makeBool(true);
                    return dbObj;
                }
            }

            // 2. Try to create a new connection if under max
            int totalConns = (int)pool->connections.size();
            if (totalConns < pool->maxSize) {
                lock.unlock();
                std::vector<Value> connectArgs = {Value::makeString(pool->connectionString)};
                Value dbObj = vm.callNative("Database.connect", connectArgs);
                lock.lock();
                if (dbObj.type != ValueType::Null && dbObj.objVal) {
                    auto pc = std::make_shared<PooledConnection>();
                    pc->dbId = dbObj.objVal->fields["_id"].toInt();
                    pc->inUse = true;
                    pc->lastUsed = std::chrono::steady_clock::now();
                    pc->createdAt = std::chrono::steady_clock::now();
                    pc->healthy = true;
                    pool->connections.push_back(pc);
                    pool->totalCreated++;
                    pool->totalAcquired++;

                    dbObj.objVal->fields["_poolId"] = Value::makeInt(poolId);
                    dbObj.objVal->fields["pooled"] = Value::makeBool(true);
                    return dbObj;
                }
                // Connection creation failed — fall through to wait
            }

            // 3. Wait for a connection to be released (with timeout)
            if (std::chrono::steady_clock::now() >= deadline) {
                pool->totalTimeouts++;
                vm.throwError("GardPoolTimeoutError", "ConnectionPool.acquire: timed out waiting for available connection (pool exhausted, max=" + std::to_string(pool->maxSize) + ")");
                return Value::makeNull();
            }
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            pool->cv.wait_for(lock, remaining);
            if (std::chrono::steady_clock::now() >= deadline) {
                pool->totalTimeouts++;
                vm.throwError("GardPoolTimeoutError", "ConnectionPool.acquire: timed out waiting for available connection (pool exhausted, max=" + std::to_string(pool->maxSize) + ")");
                return Value::makeNull();
            }
        }
    });

    // ConnectionPool.release(pool, db) — return a connection to the pool
    vm.registerNative("ConnectionPool.release", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardPoolError", "ConnectionPool.release: requires pool and db connection"); return Value::makeNull(); }
        int poolId = a[0].objVal->fields["_poolId"].toInt();
        int dbId = a[1].objVal->fields["_id"].toInt();
        std::shared_ptr<ConnectionPool> pool;
        { std::lock_guard<std::mutex> lock(g_poolMutex); auto it = g_pools.find(poolId); if (it == g_pools.end()) { vm.throwError("GardPoolError", "ConnectionPool.release: pool not found"); return Value::makeNull(); } pool = it->second; }

        std::lock_guard<std::mutex> lock(pool->mutex);
        bool found = false;
        for (auto& pc : pool->connections) {
            if (pc->dbId == dbId && pc->inUse) {
                pc->inUse = false;
                pc->lastUsed = std::chrono::steady_clock::now();
                pool->totalReleased++;
                found = true;
                break;
            }
        }
        if (!found) {
            vm.throwError("GardPoolError", "ConnectionPool.release: connection not owned by this pool or already released");
            return Value::makeNull();
        }
        // Notify waiting acquirers
        pool->cv.notify_one();
        return Value::makeBool(true);
    });

    // ConnectionPool.destroy(pool) — close all connections and destroy the pool
    vm.registerNative("ConnectionPool.destroy", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardPoolError", "ConnectionPool.destroy: requires pool object"); return Value::makeNull(); }
        int poolId = a[0].objVal->fields["_poolId"].toInt();
        std::shared_ptr<ConnectionPool> pool;
        { std::lock_guard<std::mutex> lock(g_poolMutex); auto it = g_pools.find(poolId); if (it == g_pools.end()) { vm.throwError("GardPoolError", "ConnectionPool.destroy: pool not found"); return Value::makeNull(); } pool = it->second; }

        std::lock_guard<std::mutex> lock(pool->mutex);
        // Check for in-use connections
        int inUseCount = 0;
        for (auto& pc : pool->connections) { if (pc->inUse) inUseCount++; }
        if (inUseCount > 0) {
            vm.throwError("GardPoolError", "ConnectionPool.destroy: " + std::to_string(inUseCount) + " connection(s) still in use. Release all connections before destroying the pool");
            return Value::makeNull();
        }
        // Close all connections
        for (auto& pc : pool->connections) {
            if (pc->dbId >= 0) {
                std::shared_ptr<DbConnection> conn;
                { std::lock_guard<std::mutex> dbLock(g_dbMutex); auto it = g_dbConnections.find(pc->dbId); if (it != g_dbConnections.end()) { conn = it->second; g_dbConnections.erase(it); } }
                if (conn) {
                    if (conn->db) { sqlite3_close(conn->db); conn->db = nullptr; }
                    if (conn->mysql) { mysql_close(conn->mysql); conn->mysql = nullptr; }
                    if (conn->pg) { PQfinish(conn->pg); conn->pg = nullptr; }
                    if (conn->mongo) { mongoc_client_destroy(conn->mongo); conn->mongo = nullptr; }
                }
                pool->totalDestroyed++;
            }
        }
        pool->connections.clear();
        // Remove pool from global map
        { std::lock_guard<std::mutex> pLock(g_poolMutex); g_pools.erase(poolId); }
        return Value::makeBool(true);
    });

    // ConnectionPool.stats(pool) — get pool statistics
    vm.registerNative("ConnectionPool.stats", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardPoolError", "ConnectionPool.stats: requires pool object"); return Value::makeNull(); }
        int poolId = a[0].objVal->fields["_poolId"].toInt();
        std::shared_ptr<ConnectionPool> pool;
        { std::lock_guard<std::mutex> lock(g_poolMutex); auto it = g_pools.find(poolId); if (it == g_pools.end()) { vm.throwError("GardPoolError", "ConnectionPool.stats: pool not found"); return Value::makeNull(); } pool = it->second; }

        std::lock_guard<std::mutex> lock(pool->mutex);
        int active = 0, idle = 0, unhealthy = 0;
        for (auto& pc : pool->connections) {
            if (pc->inUse) active++;
            else if (pc->healthy) idle++;
            else unhealthy++;
        }

        Value stats = Value::makeObject("PoolStats");
        stats.objVal->fields["active"] = Value::makeInt(active);
        stats.objVal->fields["idle"] = Value::makeInt(idle);
        stats.objVal->fields["unhealthy"] = Value::makeInt(unhealthy);
        stats.objVal->fields["total"] = Value::makeInt((int)pool->connections.size());
        stats.objVal->fields["min"] = Value::makeInt(pool->minSize);
        stats.objVal->fields["max"] = Value::makeInt(pool->maxSize);
        stats.objVal->fields["totalCreated"] = Value::makeInt(pool->totalCreated);
        stats.objVal->fields["totalDestroyed"] = Value::makeInt(pool->totalDestroyed);
        stats.objVal->fields["totalAcquired"] = Value::makeInt(pool->totalAcquired);
        stats.objVal->fields["totalReleased"] = Value::makeInt(pool->totalReleased);
        stats.objVal->fields["totalTimeouts"] = Value::makeInt(pool->totalTimeouts);
        stats.objVal->fields["totalHealthChecksFailed"] = Value::makeInt(pool->totalHealthChecksFailed);
        return stats;
    });

    // ConnectionPool.evict(pool) — remove idle/unhealthy connections, shrink to min
    vm.registerNative("ConnectionPool.evict", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardPoolError", "ConnectionPool.evict: requires pool object"); return Value::makeNull(); }
        int poolId = a[0].objVal->fields["_poolId"].toInt();
        std::shared_ptr<ConnectionPool> pool;
        { std::lock_guard<std::mutex> lock(g_poolMutex); auto it = g_pools.find(poolId); if (it == g_pools.end()) { vm.throwError("GardPoolError", "ConnectionPool.evict: pool not found"); return Value::makeNull(); } pool = it->second; }

        std::lock_guard<std::mutex> lock(pool->mutex);
        auto now = std::chrono::steady_clock::now();
        int evicted = 0;

        // Remove unhealthy and expired idle connections (keep at least minSize total healthy)
        auto it = pool->connections.begin();
        while (it != pool->connections.end()) {
            auto& pc = *it;
            if (pc->inUse) { ++it; continue; }

            bool shouldEvict = false;
            if (!pc->healthy) {
                shouldEvict = true;
            } else {
                auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - pc->createdAt).count();
                auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - pc->lastUsed).count();
                if (age > pool->maxLifetimeMs) shouldEvict = true;
                else if (idle > pool->idleTimeoutMs) {
                    // Only evict idle if we're above minSize
                    int healthyCount = 0;
                    for (auto& c : pool->connections) { if (c->healthy && c != pc) healthyCount++; }
                    if (healthyCount >= pool->minSize) shouldEvict = true;
                }
            }

            if (shouldEvict) {
                // Close the underlying connection
                if (pc->dbId >= 0) {
                    std::shared_ptr<DbConnection> conn;
                    { std::lock_guard<std::mutex> dbLock(g_dbMutex); auto dit = g_dbConnections.find(pc->dbId); if (dit != g_dbConnections.end()) { conn = dit->second; g_dbConnections.erase(dit); } }
                    if (conn) {
                        if (conn->db) { sqlite3_close(conn->db); conn->db = nullptr; }
                        if (conn->mysql) { mysql_close(conn->mysql); conn->mysql = nullptr; }
                        if (conn->pg) { PQfinish(conn->pg); conn->pg = nullptr; }
                        if (conn->mongo) { mongoc_client_destroy(conn->mongo); conn->mongo = nullptr; }
                    }
                    pool->totalDestroyed++;
                }
                it = pool->connections.erase(it);
                evicted++;
            } else {
                ++it;
            }
        }

        Value result = Value::makeObject("EvictResult");
        result.objVal->fields["evicted"] = Value::makeInt(evicted);
        result.objVal->fields["remaining"] = Value::makeInt((int)pool->connections.size());
        return result;
    });

    // ConnectionPool.resize(pool, newMin, newMax) — dynamically resize pool bounds
    vm.registerNative("ConnectionPool.resize", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardPoolError", "ConnectionPool.resize: requires pool, newMin, newMax"); return Value::makeNull(); }
        int poolId = a[0].objVal->fields["_poolId"].toInt();
        int newMin = a[1].toInt();
        int newMax = a[2].toInt();
        if (newMin < 0 || newMax < 1 || newMin > newMax) { vm.throwError("GardPoolError", "ConnectionPool.resize: invalid bounds (min=" + std::to_string(newMin) + ", max=" + std::to_string(newMax) + ")"); return Value::makeNull(); }

        std::shared_ptr<ConnectionPool> pool;
        { std::lock_guard<std::mutex> lock(g_poolMutex); auto it = g_pools.find(poolId); if (it == g_pools.end()) { vm.throwError("GardPoolError", "ConnectionPool.resize: pool not found"); return Value::makeNull(); } pool = it->second; }

        std::lock_guard<std::mutex> lock(pool->mutex);
        pool->minSize = newMin;
        pool->maxSize = newMax;

        // If current connections exceed new max, mark excess idle ones for eviction
        int currentHealthy = 0;
        for (auto& pc : pool->connections) { if (pc->healthy && !pc->inUse) currentHealthy++; }
        // We don't forcibly close in-use connections, just adjust bounds
        // Eviction will handle cleanup on next evict() or acquire()

        Value result = Value::makeObject("ConnectionPool");
        result.objVal->fields["_poolId"] = Value::makeInt(poolId);
        result.objVal->fields["min"] = Value::makeInt(newMin);
        result.objVal->fields["max"] = Value::makeInt(newMax);
        result.objVal->fields["size"] = Value::makeInt((int)pool->connections.size());
        return result;
    });

    // ConnectionPool.isHealthy(pool) — check if pool has available healthy connections
    vm.registerNative("ConnectionPool.isHealthy", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardPoolError", "ConnectionPool.isHealthy: requires pool object"); return Value::makeNull(); }
        int poolId = a[0].objVal->fields["_poolId"].toInt();
        std::shared_ptr<ConnectionPool> pool;
        { std::lock_guard<std::mutex> lock(g_poolMutex); auto it = g_pools.find(poolId); if (it == g_pools.end()) return Value::makeBool(false); pool = it->second; }

        std::lock_guard<std::mutex> lock(pool->mutex);
        for (auto& pc : pool->connections) {
            if (pc->healthy && !pc->inUse) return Value::makeBool(true);
        }
        // Also healthy if we can still create new connections
        return Value::makeBool((int)pool->connections.size() < pool->maxSize);
    });

    // Database.run(db, queryBuilder) — execute a query builder against the connected db
    vm.registerNative("Database.run", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) {
            vm.throwError("GardQueryError", "Database.run: requires db and query builder");
            return Value::makeNull();
        }
        int id = a[0].objVal->fields["_id"].toInt();
        std::shared_ptr<DbConnection> conn;
        { std::lock_guard<std::mutex> lock(g_dbMutex); auto it = g_dbConnections.find(id); if (it == g_dbConnections.end()) { vm.throwError("GardConnectionError", "Database.run: connection not found"); return Value::makeNull(); } conn = it->second; }

        // Compile query to SQL for the connected driver
        std::string driver = conn->driver;
        // Call Query.toSQL internally
        std::string type = a[1].objVal->fields["_type"].toString();
        // Build SQL using the same logic as Query.toSQL
        // We'll just call the native directly
        std::vector<Value> toSqlArgs = {a[1], Value::makeString(driver)};
        // Find and call Query.toSQL
        Value sqlVal = Value::makeNull();
        if (vm.hasNative("Query.toSQL")) {
            sqlVal = vm.callNative("Query.toSQL", toSqlArgs);
        }
        if (sqlVal.type == ValueType::Null || sqlVal.toString().empty()) {
            vm.throwError("GardQueryError", "Database.run: failed to compile query");
            return Value::makeNull();
        }
        std::string sql = sqlVal.toString();

        // Execute based on query type
        if (type == "select") {
            std::vector<Value> queryArgs = {a[0], Value::makeString(sql)};
            if (vm.hasNative("Database.query")) return vm.callNative("Database.query", queryArgs);
        } else {
            std::vector<Value> execArgs = {a[0], Value::makeString(sql)};
            if (vm.hasNative("Database.execute")) return vm.callNative("Database.execute", execArgs);
        }
        return Value::makeNull();
    });
}

} // namespace stdlib
} // namespace runtime
} // namespace gard
