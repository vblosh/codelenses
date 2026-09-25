#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct sqlite3;

namespace codelenses {

class Connection {
public:
    static std::unique_ptr<Connection> open(const std::string& path);
    static std::unique_ptr<Connection> open_memory();

    explicit Connection(sqlite3* db);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    [[nodiscard]] sqlite3* handle() const noexcept { return db_; }

    void execute(std::string_view sql);
    [[nodiscard]] int64_t last_insert_rowid() const noexcept;
    [[nodiscard]] int changes() const noexcept;
    [[nodiscard]] std::string errmsg() const;

    void apply_default_pragmas();

private:
    sqlite3* db_ = nullptr;
};

} // namespace codelenses
