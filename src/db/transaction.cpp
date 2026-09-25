#include "codelenses/db/transaction.h"

#include "codelenses/db/connection.h"
#include "codelenses/db/error.h"
#include <sqlite3.h>

namespace codelenses {

namespace {

const char* begin_sql_for_type(TransactionType type) {
    switch (type) {
    case TransactionType::deferred:
        return "BEGIN DEFERRED;";
    case TransactionType::immediate:
        return "BEGIN IMMEDIATE;";
    case TransactionType::exclusive:
        return "BEGIN EXCLUSIVE;";
    }
    return "BEGIN IMMEDIATE;";
}

void exec_simple(sqlite3* db, const char* sql) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string err = err_msg ? err_msg : "Unknown error";
        sqlite3_free(err_msg);
        throw DbError(rc, "Transaction command '" + std::string(sql) + "' failed: " + err);
    }
}

} // namespace

Transaction::Transaction(Connection& conn, TransactionType type)
    : Transaction(conn.handle(), type) {}

Transaction::Transaction(sqlite3* db, TransactionType type) : db_(db) {
    if (!db_) {
        throw DbError("Cannot create transaction on null database handle");
    }
    exec_simple(db_, begin_sql_for_type(type));
    is_active_ = true;
}

Transaction::~Transaction() {
    if (is_active_ && db_) {
        try {
            rollback();
        } catch (...) {
            // Destructors must not throw
        }
    }
}

Transaction::Transaction(Transaction&& other) noexcept
    : db_(other.db_), is_active_(other.is_active_) {
    other.db_ = nullptr;
    other.is_active_ = false;
}

Transaction& Transaction::operator=(Transaction&& other) noexcept {
    if (this != &other) {
        if (is_active_ && db_) {
            try {
                rollback();
            } catch (...) {
            }
        }
        db_ = other.db_;
        is_active_ = other.is_active_;
        other.db_ = nullptr;
        other.is_active_ = false;
    }
    return *this;
}

void Transaction::commit() {
    if (!is_active_) {
        throw DbError("Cannot commit inactive transaction");
    }
    try {
        exec_simple(db_, "COMMIT;");
        is_active_ = false;
    } catch (...) {
        // If COMMIT fails, attempt to rollback
        try {
            exec_simple(db_, "ROLLBACK;");
        } catch (...) {
        }
        is_active_ = false;
        throw;
    }
}

void Transaction::rollback() {
    if (!is_active_) {
        return;
    }
    is_active_ = false;
    exec_simple(db_, "ROLLBACK;");
}

} // namespace codelenses
