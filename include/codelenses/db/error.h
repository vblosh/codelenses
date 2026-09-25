#pragma once

#include <stdexcept>
#include <string>

namespace codelenses {

class DbError : public std::runtime_error {
public:
    explicit DbError(const std::string& message) : std::runtime_error(message) {}

    DbError(int error_code, const std::string& message)
        : std::runtime_error("SQLite error (" + std::to_string(error_code) + "): " + message),
          error_code_(error_code) {}

    [[nodiscard]] int error_code() const noexcept { return error_code_; }

private:
    int error_code_ = 0;
};

class MigrationError : public std::runtime_error {
public:
    explicit MigrationError(const std::string& message)
        : std::runtime_error("Migration error: " + message) {}
};

} // namespace codelenses
