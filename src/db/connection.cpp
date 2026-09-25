#include "codelenses/db/connection.hpp"

#include "codelenses/db/error.hpp"
#include <sqlite3.h>

namespace codelenses {

std::unique_ptr<Connection> Connection::open(const std::string& path) {
    sqlite3* db = nullptr;
    int rc =
        sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = db ? sqlite3_errmsg(db) : "Unknown error";
        if (db) {
            sqlite3_close(db);
        }
        throw DbError(rc, "Failed to open database at '" + path + "': " + err);
    }

    auto conn = std::make_unique<Connection>(db);
    conn->apply_default_pragmas();
    return conn;
}

std::unique_ptr<Connection> Connection::open_memory() {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = db ? sqlite3_errmsg(db) : "Unknown error";
        if (db) {
            sqlite3_close(db);
        }
        throw DbError(rc, "Failed to open in-memory database: " + err);
    }

    auto conn = std::make_unique<Connection>(db);
    conn->apply_default_pragmas();
    return conn;
}

Connection::Connection(sqlite3* db) : db_(db) {}

Connection::~Connection() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

Connection::Connection(Connection&& other) noexcept : db_(other.db_) {
    other.db_ = nullptr;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        if (db_) {
            sqlite3_close(db_);
        }
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

void Connection::apply_default_pragmas() {
    // Requirements B-04 / docs/database.md:
    // PRAGMA foreign_keys = ON;
    // PRAGMA journal_mode = WAL;
    // PRAGMA synchronous = NORMAL;
    // PRAGMA busy_timeout = 5000;
    // PRAGMA temp_store = MEMORY;
    execute("PRAGMA foreign_keys = ON;");
    execute("PRAGMA journal_mode = WAL;");
    execute("PRAGMA synchronous = NORMAL;");
    execute("PRAGMA busy_timeout = 5000;");
    execute("PRAGMA temp_store = MEMORY;");
}

void Connection::execute(std::string_view sql) {
    char* err_msg = nullptr;
    std::string sql_str(sql);
    int rc = sqlite3_exec(db_, sql_str.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string err = err_msg ? err_msg : "Unknown error";
        sqlite3_free(err_msg);
        throw DbError(rc, "Failed to execute SQL: " + sql_str + " - " + err);
    }
}

int64_t Connection::last_insert_rowid() const noexcept {
    return sqlite3_last_insert_rowid(db_);
}

int Connection::changes() const noexcept {
    return sqlite3_changes(db_);
}

std::string Connection::errmsg() const {
    return db_ ? sqlite3_errmsg(db_) : "";
}

} // namespace codelenses
