#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace codelenses {

class Connection;

struct Migration {
    int64_t version = 0;
    std::string name;
    std::string up_sql;
};

class MigrationRunner {
public:
    MigrationRunner();

    void register_migration(Migration migration);
    void ensure_migration_table(Connection& conn);

    [[nodiscard]] int64_t get_current_version(Connection& conn);
    [[nodiscard]] std::vector<int64_t> get_applied_versions(Connection& conn);

    void apply_pending(Connection& conn);

    [[nodiscard]] int64_t max_supported_version() const;
    [[nodiscard]] const std::vector<Migration>& registered_migrations() const {
        return migrations_;
    }

    static std::string initial_schema_sql();

private:
    std::vector<Migration> migrations_;
};

} // namespace codelenses
