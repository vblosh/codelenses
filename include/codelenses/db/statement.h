#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace codelenses {

class Statement {
public:
    Statement(sqlite3* db, std::string_view sql);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    // Binding parameters (1-based index)
    void bind_null(int index);
    void bind_int64(int index, int64_t value);
    void bind_double(int index, double value);
    void bind_text(int index, std::string_view value);
    void bind_optional_text(int index, const std::optional<std::string>& value);
    void bind_optional_int64(int index, const std::optional<int64_t>& value);
    void bind_bool(int index, bool value);

    // Stepping and execution
    [[nodiscard]] bool step();
    void execute();
    void reset();
    void clear_bindings();

    // Column values (0-based index)
    [[nodiscard]] bool is_null(int col) const;
    [[nodiscard]] int64_t column_int64(int col) const;
    [[nodiscard]] double column_double(int col) const;
    [[nodiscard]] std::string column_text(int col) const;
    [[nodiscard]] std::optional<std::string> column_optional_text(int col) const;
    [[nodiscard]] std::optional<int64_t> column_optional_int64(int col) const;
    [[nodiscard]] bool column_bool(int col) const;

    [[nodiscard]] sqlite3_stmt* handle() const noexcept { return stmt_; }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

} // namespace codelenses
