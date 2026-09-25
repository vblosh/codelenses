#include "codelenses/db/statement.h"

#include "codelenses/db/error.h"
#include <sqlite3.h>

namespace codelenses {

Statement::Statement(sqlite3* db, std::string_view sql) : db_(db) {
    std::string sql_str(sql);
    int rc =
        sqlite3_prepare_v2(db_, sql_str.c_str(), static_cast<int>(sql_str.size()), &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "Unknown error";
        throw DbError(rc, "Failed to prepare SQL: " + sql_str + " - " + err);
    }
}

Statement::~Statement() {
    if (stmt_) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

Statement::Statement(Statement&& other) noexcept : db_(other.db_), stmt_(other.stmt_) {
    other.db_ = nullptr;
    other.stmt_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (stmt_) {
            sqlite3_finalize(stmt_);
        }
        db_ = other.db_;
        stmt_ = other.stmt_;
        other.db_ = nullptr;
        other.stmt_ = nullptr;
    }
    return *this;
}

void Statement::bind_null(int index) {
    int rc = sqlite3_bind_null(stmt_, index);
    if (rc != SQLITE_OK) {
        throw DbError(rc, "Failed to bind NULL at index " + std::to_string(index));
    }
}

void Statement::bind_int64(int index, int64_t value) {
    int rc = sqlite3_bind_int64(stmt_, index, value);
    if (rc != SQLITE_OK) {
        throw DbError(rc, "Failed to bind int64 at index " + std::to_string(index));
    }
}

void Statement::bind_double(int index, double value) {
    int rc = sqlite3_bind_double(stmt_, index, value);
    if (rc != SQLITE_OK) {
        throw DbError(rc, "Failed to bind double at index " + std::to_string(index));
    }
}

void Statement::bind_text(int index, std::string_view value) {
    int rc = sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                               SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        throw DbError(rc, "Failed to bind text at index " + std::to_string(index));
    }
}

void Statement::bind_optional_text(int index, const std::optional<std::string>& value) {
    if (value.has_value()) {
        bind_text(index, *value);
    } else {
        bind_null(index);
    }
}

void Statement::bind_optional_int64(int index, const std::optional<int64_t>& value) {
    if (value.has_value()) {
        bind_int64(index, *value);
    } else {
        bind_null(index);
    }
}

void Statement::bind_bool(int index, bool value) {
    bind_int64(index, value ? 1 : 0);
}

bool Statement::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    std::string err = db_ ? sqlite3_errmsg(db_) : "Unknown error";
    throw DbError(rc, "Failed to step statement: " + err);
}

void Statement::execute() {
    int rc = sqlite3_step(stmt_);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "Unknown error";
        throw DbError(rc, "Failed to execute statement: " + err);
    }
}

void Statement::reset() {
    if (stmt_) {
        sqlite3_reset(stmt_);
    }
}

void Statement::clear_bindings() {
    if (stmt_) {
        sqlite3_clear_bindings(stmt_);
    }
}

bool Statement::is_null(int col) const {
    return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

int64_t Statement::column_int64(int col) const {
    return sqlite3_column_int64(stmt_, col);
}

double Statement::column_double(int col) const {
    return sqlite3_column_double(stmt_, col);
}

std::string Statement::column_text(int col) const {
    const unsigned char* text = sqlite3_column_text(stmt_, col);
    if (!text) {
        return "";
    }
    int bytes = sqlite3_column_bytes(stmt_, col);
    return std::string(reinterpret_cast<const char*>(text), static_cast<size_t>(bytes));
}

std::optional<std::string> Statement::column_optional_text(int col) const {
    if (is_null(col)) {
        return std::nullopt;
    }
    return column_text(col);
}

std::optional<int64_t> Statement::column_optional_int64(int col) const {
    if (is_null(col)) {
        return std::nullopt;
    }
    return column_int64(col);
}

bool Statement::column_bool(int col) const {
    return sqlite3_column_int64(stmt_, col) != 0;
}

} // namespace codelenses
