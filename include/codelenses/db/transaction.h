#pragma once

#include <string_view>

struct sqlite3;

namespace codelenses {

class Connection;

enum class TransactionType {
    deferred,
    immediate,
    exclusive,
};

class Transaction {
public:
    explicit Transaction(Connection& conn, TransactionType type = TransactionType::immediate);
    explicit Transaction(sqlite3* db, TransactionType type = TransactionType::immediate);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&& other) noexcept;

    void commit();
    void rollback();

    [[nodiscard]] bool is_active() const noexcept { return is_active_; }

private:
    sqlite3* db_ = nullptr;
    bool is_active_ = false;
};

} // namespace codelenses
