#include <filesystem>

#include "codelenses/db/connection.h"
#include "codelenses/db/database.h"
#include "codelenses/db/error.h"
#include "codelenses/db/migration.h"
#include "codelenses/db/statement.h"
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using namespace codelenses;

TEST_CASE("MigrationRunner creates migration table and applies initial schema", "[migration]") {
    auto conn = Connection::open_memory();
    MigrationRunner runner;

    SECTION("Initial state has version 0") {
        REQUIRE(runner.get_current_version(*conn) == 0);
        REQUIRE(runner.get_applied_versions(*conn).empty());
    }

    SECTION("Applying pending migrations upgrades to v1") {
        runner.apply_pending(*conn);
        REQUIRE(runner.get_current_version(*conn) == 1);
        auto applied = runner.get_applied_versions(*conn);
        REQUIRE(applied.size() == 1);
        REQUIRE(applied[0] == 1);

        // Verify tables exist by inserting/selecting
        Statement stmt(conn->handle(), "SELECT count(*) FROM workspace;");
        REQUIRE(stmt.step());
        REQUIRE(stmt.column_int64(0) == 0);

        Statement stmt_sym(conn->handle(), "SELECT count(*) FROM symbol;");
        REQUIRE(stmt_sym.step());
        REQUIRE(stmt_sym.column_int64(0) == 0);

        Statement stmt_fts(conn->handle(), "SELECT count(*) FROM symbol_search;");
        REQUIRE(stmt_fts.step());
        REQUIRE(stmt_fts.column_int64(0) == 0);
    }

    SECTION("Re-applying pending migrations is idempotent") {
        runner.apply_pending(*conn);
        REQUIRE_NOTHROW(runner.apply_pending(*conn));
        REQUIRE(runner.get_current_version(*conn) == 1);
    }
}

TEST_CASE("MigrationRunner supports upgrading schema versions", "[migration]") {
    auto conn = Connection::open_memory();
    MigrationRunner runner;
    runner.apply_pending(*conn);
    REQUIRE(runner.get_current_version(*conn) == 1);

    // Register a new migration v2
    runner.register_migration(Migration{
        .version = 2,
        .name = "002_add_test_column",
        .up_sql = "ALTER TABLE workspace ADD COLUMN description TEXT;",
    });

    REQUIRE(runner.max_supported_version() == 2);
    runner.apply_pending(*conn);

    REQUIRE(runner.get_current_version(*conn) == 2);
    auto applied = runner.get_applied_versions(*conn);
    REQUIRE(applied.size() == 2);
    REQUIRE(applied[0] == 1);
    REQUIRE(applied[1] == 2);

    // Verify newly added column exists
    conn->execute("INSERT INTO workspace (root_path, name, description) VALUES ('/test', 'test', "
                  "'A description');");
    Statement check(conn->handle(), "SELECT description FROM workspace WHERE root_path = '/test';");
    REQUIRE(check.step());
    REQUIRE(check.column_text(0) == "A description");
}

TEST_CASE("MigrationRunner rejects newer unsupported schema versions", "[migration]") {
    auto conn = Connection::open_memory();
    MigrationRunner runner;
    runner.ensure_migration_table(*conn);

    // Manually insert an unsupported future version
    conn->execute(
        "INSERT INTO schema_migration (version, name) VALUES (999, '999_future_schema');");

    REQUIRE(runner.get_current_version(*conn) == 999);
    REQUIRE_THROWS_AS(runner.apply_pending(*conn), MigrationError);
}

TEST_CASE("Database facade automatically applies migrations on open", "[database][migration]") {
    auto db = Database::open_memory(/*apply_migrations=*/true);
    REQUIRE(db != nullptr);
    REQUIRE(db->migration_runner().get_current_version(db->connection()) == 1);

    // Verify repositories are accessible
    auto ws_list = db->workspaces().list_all();
    REQUIRE(ws_list.empty());
}

TEST_CASE("Reopening an existing database file retains schema and data", "[database][migration]") {
    const std::string db_file = "test_reopen_migration.db";
    if (fs::exists(db_file)) {
        fs::remove(db_file);
    }

    {
        auto db = Database::open(db_file, /*apply_migrations=*/true);
        REQUIRE(db->migration_runner().get_current_version(db->connection()) == 1);

        Workspace ws{
            .root_path = "/workspace/reopen",
            .name = "Reopen Test",
        };
        int64_t id = db->workspaces().create(ws);
        REQUIRE(id > 0);
    }

    // Reopen without re-creating
    {
        auto db = Database::open(db_file, /*apply_migrations=*/true);
        REQUIRE(db->migration_runner().get_current_version(db->connection()) == 1);

        auto ws = db->workspaces().get_by_root_path("/workspace/reopen");
        REQUIRE(ws.has_value());
        REQUIRE(ws->name == "Reopen Test");
    }

    if (fs::exists(db_file)) {
        fs::remove(db_file);
    }
}
