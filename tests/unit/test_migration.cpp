#include <filesystem>

#include "codelenses/db/connection.hpp"
#include "codelenses/db/database.hpp"
#include "codelenses/db/error.hpp"
#include "codelenses/db/migration.hpp"
#include "codelenses/db/statement.hpp"
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

    SECTION("Applying pending migrations upgrades to latest") {
        runner.apply_pending(*conn);
        REQUIRE(runner.get_current_version(*conn) == runner.max_supported_version());
        auto applied = runner.get_applied_versions(*conn);
        REQUIRE(applied.size() == static_cast<size_t>(runner.max_supported_version()));
        REQUIRE(applied.back() == runner.max_supported_version());

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
        REQUIRE(runner.get_current_version(*conn) == runner.max_supported_version());
    }
}

TEST_CASE("MigrationRunner supports upgrading schema versions", "[migration]") {
    auto conn = Connection::open_memory();
    MigrationRunner runner;
    int64_t v = runner.max_supported_version();
    runner.apply_pending(*conn);
    REQUIRE(runner.get_current_version(*conn) == v);

    // Register a new migration v + 1
    runner.register_migration(Migration{
        .version = v + 1,
        .name = "999_add_test_column",
        .up_sql = "ALTER TABLE workspace ADD COLUMN description TEXT;",
    });

    REQUIRE(runner.max_supported_version() == v + 1);
    runner.apply_pending(*conn);

    REQUIRE(runner.get_current_version(*conn) == v + 1);
    auto applied = runner.get_applied_versions(*conn);
    REQUIRE(applied.size() == static_cast<size_t>(v + 1));
    REQUIRE(applied.back() == v + 1);

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
    REQUIRE(db->migration_runner().get_current_version(db->connection()) ==
            db->migration_runner().max_supported_version());

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
        REQUIRE(db->migration_runner().get_current_version(db->connection()) ==
                db->migration_runner().max_supported_version());

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
        REQUIRE(db->migration_runner().get_current_version(db->connection()) ==
                db->migration_runner().max_supported_version());

        auto ws = db->workspaces().get_by_root_path("/workspace/reopen");
        REQUIRE(ws.has_value());
        REQUIRE(ws->name == "Reopen Test");
    }

    if (fs::exists(db_file)) {
        fs::remove(db_file);
    }
}

TEST_CASE("MigrationRunner preserves populated workspace and child rows when upgrading v1 to v2",
          "[migration]") {
    auto conn = Connection::open_memory();
    conn->execute("PRAGMA foreign_keys = ON;");

    // 1. Manually apply v1 schema and register migration 1
    MigrationRunner runner;
    runner.ensure_migration_table(*conn);
    {
        Transaction tx(*conn, TransactionType::immediate);
        conn->execute(MigrationRunner::initial_schema_sql());
        conn->execute(
            "INSERT INTO schema_migration (version, name) VALUES (1, '001_initial_schema');");
        tx.commit();
    }
    REQUIRE(runner.get_current_version(*conn) == 1);

    // 2. Populate v1 database with workspace and child rows
    conn->execute(R"SQL(
        INSERT INTO workspace (id, root_path, name)
        VALUES (1, '/workspace/project1', 'Project One');

        INSERT INTO file (id, workspace_id, path, relative_path, name, language, size_bytes, modified_ns, content_hash)
        VALUES (10, 1, '/workspace/project1/main.cpp', 'main.cpp', 'main.cpp', 'cpp', 100, 1000, 'hash123');

        INSERT INTO symbol (id, workspace_id, file_id, symbol_key, name, kind, language, is_definition,
                            start_byte, end_byte, start_line, start_column, end_line, end_column)
        VALUES (100, 1, 10, 'key_foo', 'foo', 'function', 'cpp', 1, 0, 10, 1, 1, 1, 11);

        INSERT INTO occurrence (workspace_id, file_id, symbol_id, name, occurrence_kind, resolution,
                                confidence, start_byte, end_byte, start_line, start_column, end_line, end_column)
        VALUES (1, 10, 100, 'foo', 'definition', 'resolved', 1.0, 0, 10, 1, 1, 1, 11);

        INSERT INTO file_content (file_id, workspace_id, content, content_hash)
        VALUES (10, 1, 'void foo() {}', 'hash123');
    )SQL");

    // Verify initial row counts in v1
    {
        Statement s_ws(conn->handle(), "SELECT count(*) FROM workspace;");
        REQUIRE(s_ws.step());
        REQUIRE(s_ws.column_int64(0) == 1);

        Statement s_file(conn->handle(), "SELECT count(*) FROM file;");
        REQUIRE(s_file.step());
        REQUIRE(s_file.column_int64(0) == 1);

        Statement s_sym(conn->handle(), "SELECT count(*) FROM symbol;");
        REQUIRE(s_sym.step());
        REQUIRE(s_sym.column_int64(0) == 1);

        Statement s_occ(conn->handle(), "SELECT count(*) FROM occurrence;");
        REQUIRE(s_occ.step());
        REQUIRE(s_occ.column_int64(0) == 1);

        Statement s_content(conn->handle(), "SELECT count(*) FROM file_content;");
        REQUIRE(s_content.step());
        REQUIRE(s_content.column_int64(0) == 1);
    }

    // 3. Run apply_pending to execute migration 002 (table rebuild)
    REQUIRE_NOTHROW(runner.apply_pending(*conn));
    REQUIRE(runner.get_current_version(*conn) == runner.max_supported_version());

    // 4. Assert all rows survived intact and foreign key integrity is preserved
    {
        Statement s_ws(conn->handle(), "SELECT id, root_path, name, kind FROM workspace WHERE id = 1;");
        REQUIRE(s_ws.step());
        REQUIRE(s_ws.column_int64(0) == 1);
        REQUIRE(s_ws.column_text(1) == "/workspace/project1");
        REQUIRE(s_ws.column_text(2) == "Project One");
        REQUIRE(s_ws.column_text(3) == "project");

        Statement s_file(conn->handle(), "SELECT count(*) FROM file WHERE id = 10 AND workspace_id = 1;");
        REQUIRE(s_file.step());
        REQUIRE(s_file.column_int64(0) == 1);

        Statement s_sym(conn->handle(), "SELECT count(*) FROM symbol WHERE id = 100 AND workspace_id = 1;");
        REQUIRE(s_sym.step());
        REQUIRE(s_sym.column_int64(0) == 1);

        Statement s_occ(conn->handle(), "SELECT count(*) FROM occurrence WHERE workspace_id = 1;");
        REQUIRE(s_occ.step());
        REQUIRE(s_occ.column_int64(0) == 1);

        Statement s_content(conn->handle(), "SELECT count(*) FROM file_content WHERE file_id = 10;");
        REQUIRE(s_content.step());
        REQUIRE(s_content.column_int64(0) == 1);

        // Verify foreign key checks pass
        Statement fk_check(conn->handle(), "PRAGMA foreign_key_check;");
        REQUIRE_FALSE(fk_check.step());
    }
}

