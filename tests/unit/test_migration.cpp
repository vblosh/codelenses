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

TEST_CASE("Database migration from every supported schema version (I-03)", "[migration][schema]") {
    SECTION("Migrating from Version 0 (uninitialized database) creates all core tables and indices") {
        auto conn = Connection::open_memory();
        MigrationRunner runner;

        // Version 0: no tables exist yet
        REQUIRE(runner.get_current_version(*conn) == 0);
        REQUIRE(runner.get_applied_versions(*conn).empty());

        // Apply pending -> migrates from v0 to v1
        runner.apply_pending(*conn);
        REQUIRE(runner.get_current_version(*conn) == 1);
        auto applied = runner.get_applied_versions(*conn);
        REQUIRE(applied.size() == 1);
        CHECK(applied[0] == 1);

        // Verify all core tables exist in sqlite_master
        std::vector<std::string> expected_tables = {
            "workspace", "index_job", "file", "symbol", "occurrence",
            "reference_occurrence", "symbol_relation", "file_dependency",
            "diagnostic", "file_content", "symbol_search"
        };
        for (const auto& tbl : expected_tables) {
            Statement stmt(conn->handle(),
                           "SELECT count(*) FROM sqlite_master WHERE type IN ('table', 'view') AND name = ?;");
            stmt.bind_text(1, tbl);
            REQUIRE(stmt.step());
            CHECK(stmt.column_int64(0) == 1);
        }

        // Verify primary indexes are present
        std::vector<std::string> expected_indexes = {
            "workspace_status_idx", "job_workspace_idx", "job_status_idx",
            "file_workspace_idx", "file_workspace_path_idx", "symbol_file_range_idx",
            "symbol_workspace_name_idx", "occurrence_symbol_idx", "reference_target_idx",
            "relation_source_idx", "dependency_source_idx", "diagnostic_job_idx"
        };
        for (const auto& idx : expected_indexes) {
            Statement stmt(conn->handle(),
                           "SELECT count(*) FROM sqlite_master WHERE type = 'index' AND name = ?;");
            stmt.bind_text(1, idx);
            REQUIRE(stmt.step());
            CHECK(stmt.column_int64(0) == 1);
        }
    }

    SECTION("Migrating from Version 1 with pre-populated domain data to Version 2 and Version 3") {
        auto conn = Connection::open_memory();
        MigrationRunner runner;

        // 1. Establish Version 1
        runner.apply_pending(*conn);
        REQUIRE(runner.get_current_version(*conn) == 1);

        // 2. Pre-populate realistic domain data into Version 1 schema
        conn->execute("INSERT INTO workspace (id, root_path, name) VALUES (10, '/repo/project', 'Project 10');");
        conn->execute("INSERT INTO file (id, workspace_id, path, relative_path, name, language, size_bytes) "
                      "VALUES (101, 10, '/repo/project/main.cpp', 'main.cpp', 'main.cpp', 'cpp', 256);");
        conn->execute("INSERT INTO symbol (id, workspace_id, file_id, symbol_key, name, qualified_name, kind, language, is_definition, "
                      "start_byte, end_byte, start_line, start_column, end_line, end_column) "
                      "VALUES (1001, 10, 101, 'cpp:main.cpp:process', 'process', 'project::process', 'function', 'cpp', 1, 0, 100, 1, 0, 10, 1);");
        conn->execute("INSERT INTO occurrence (id, workspace_id, file_id, symbol_id, occurrence_kind, name, "
                      "start_byte, end_byte, start_line, start_column, end_line, end_column) "
                      "VALUES (501, 10, 101, 1001, 'definition', 'process', 0, 100, 1, 0, 10, 1);");
        conn->execute("INSERT INTO symbol_relation (id, workspace_id, source_symbol_id, target_symbol_id, relation_kind, confidence) "
                      "VALUES (701, 10, 1001, 1001, 'calls', 1.0);");

        // 3. Register Version 2: adds branch_name column to workspace and a workspace_metadata table
        runner.register_migration(Migration{
            .version = 2,
            .name = "002_workspace_branch_and_metadata",
            .up_sql = R"SQL(
                ALTER TABLE workspace ADD COLUMN branch_name TEXT NOT NULL DEFAULT 'main';
                CREATE TABLE IF NOT EXISTS workspace_metadata (
                    workspace_id    INTEGER NOT NULL REFERENCES workspace(id) ON DELETE CASCADE,
                    meta_key        TEXT NOT NULL,
                    meta_value      TEXT,
                    PRIMARY KEY(workspace_id, meta_key)
                );
                CREATE INDEX IF NOT EXISTS idx_file_extension ON file(extension);
            )SQL",
        });

        // 4. Register Version 3: adds doc_summary to symbol and symbol_alias table
        runner.register_migration(Migration{
            .version = 3,
            .name = "003_symbol_doc_and_alias",
            .up_sql = R"SQL(
                ALTER TABLE symbol ADD COLUMN doc_summary TEXT;
                CREATE TABLE IF NOT EXISTS symbol_alias (
                    symbol_id   INTEGER NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
                    alias_name  TEXT NOT NULL,
                    PRIMARY KEY(symbol_id, alias_name)
                );
            )SQL",
        });

        REQUIRE(runner.max_supported_version() == 3);

        // 5. Apply pending migrations (migrates from v1 -> v2 -> v3)
        runner.apply_pending(*conn);
        REQUIRE(runner.get_current_version(*conn) == 3);

        auto applied = runner.get_applied_versions(*conn);
        REQUIRE(applied.size() == 3);
        CHECK(applied[0] == 1);
        CHECK(applied[1] == 2);
        CHECK(applied[2] == 3);

        // 6. Verify pre-existing data is intact
        Statement check_ws(conn->handle(), "SELECT name, branch_name FROM workspace WHERE id = 10;");
        REQUIRE(check_ws.step());
        CHECK(check_ws.column_text(0) == "Project 10");
        CHECK(check_ws.column_text(1) == "main"); // Default from v2 migration

        Statement check_sym(conn->handle(), "SELECT name, doc_summary FROM symbol WHERE id = 1001;");
        REQUIRE(check_sym.step());
        CHECK(check_sym.column_text(0) == "process");
        CHECK(check_sym.is_null(1)); // NULL by default in newly added column

        // 7. Verify new v2 and v3 features function properly
        conn->execute("INSERT INTO workspace_metadata (workspace_id, meta_key, meta_value) "
                      "VALUES (10, 'git.remote', 'https://github.com/example/repo');");
        Statement check_meta(conn->handle(), "SELECT meta_value FROM workspace_metadata WHERE workspace_id = 10;");
        REQUIRE(check_meta.step());
        CHECK(check_meta.column_text(0) == "https://github.com/example/repo");

        conn->execute("INSERT INTO symbol_alias (symbol_id, alias_name) VALUES (1001, 'proc_fast');");
        Statement check_alias(conn->handle(), "SELECT alias_name FROM symbol_alias WHERE symbol_id = 1001;");
        REQUIRE(check_alias.step());
        CHECK(check_alias.column_text(0) == "proc_fast");

        // 8. Verify cascade deletion across migrated tables
        conn->execute("DELETE FROM workspace WHERE id = 10;");
        Statement check_deleted_meta(conn->handle(), "SELECT count(*) FROM workspace_metadata WHERE workspace_id = 10;");
        REQUIRE(check_deleted_meta.step());
        CHECK(check_deleted_meta.column_int64(0) == 0);

        Statement check_deleted_file(conn->handle(), "SELECT count(*) FROM file WHERE workspace_id = 10;");
        REQUIRE(check_deleted_file.step());
        CHECK(check_deleted_file.column_int64(0) == 0);
    }

    SECTION("Migrating from intermediate Version 2 to Version 3") {
        auto conn = Connection::open_memory();
        MigrationRunner runner;

        runner.apply_pending(*conn); // v1
        runner.register_migration(Migration{
            .version = 2,
            .name = "002_intermediate",
            .up_sql = "ALTER TABLE workspace ADD COLUMN tag TEXT;",
        });
        runner.apply_pending(*conn); // v2
        REQUIRE(runner.get_current_version(*conn) == 2);

        // Now register v3 on a database already at v2
        runner.register_migration(Migration{
            .version = 3,
            .name = "003_upgrade_from_v2",
            .up_sql = "ALTER TABLE workspace ADD COLUMN priority INTEGER DEFAULT 1;",
        });
        REQUIRE(runner.max_supported_version() == 3);

        // Migration from intermediate v2 applies only v3
        runner.apply_pending(*conn);
        REQUIRE(runner.get_current_version(*conn) == 3);
        auto applied = runner.get_applied_versions(*conn);
        REQUIRE(applied.size() == 3);
        CHECK(applied == std::vector<int64_t>{1, 2, 3});

        // Verify column added in v3
        conn->execute("INSERT INTO workspace (id, root_path, name, tag, priority) VALUES (20, '/test/v2', 'V2 Workspace', 'beta', 5);");
        Statement check(conn->handle(), "SELECT tag, priority FROM workspace WHERE id = 20;");
        REQUIRE(check.step());
        CHECK(check.column_text(0) == "beta");
        CHECK(check.column_int64(1) == 5);
    }

    SECTION("Applying pending migrations on latest version is idempotent") {
        auto conn = Connection::open_memory();
        MigrationRunner runner;
        runner.register_migration(Migration{
            .version = 2,
            .name = "002_idempotency_check",
            .up_sql = "ALTER TABLE workspace ADD COLUMN note TEXT;",
        });
        runner.apply_pending(*conn);
        REQUIRE(runner.get_current_version(*conn) == 2);

        // Call again on already-migrated database
        REQUIRE_NOTHROW(runner.apply_pending(*conn));
        REQUIRE(runner.get_current_version(*conn) == 2);
        auto applied = runner.get_applied_versions(*conn);
        REQUIRE(applied.size() == 2);
        CHECK(applied == std::vector<int64_t>{1, 2});
    }

    SECTION("Transactional rollback on failed migration preserves previous version and schema integrity") {
        auto conn = Connection::open_memory();
        MigrationRunner runner;
        runner.apply_pending(*conn);
        REQUIRE(runner.get_current_version(*conn) == 1);

        // Register a migration with invalid SQL
        runner.register_migration(Migration{
            .version = 2,
            .name = "002_failing_migration",
            .up_sql = "ALTER TABLE workspace ADD COLUMN valid_col TEXT; SYNTAX_ERROR_FAIL_HERE;",
        });

        // Must throw and roll back transaction
        REQUIRE_THROWS(runner.apply_pending(*conn));

        // Version must remain 1
        CHECK(runner.get_current_version(*conn) == 1);
        auto applied = runner.get_applied_versions(*conn);
        CHECK(applied.size() == 1);
        CHECK(applied[0] == 1);

        // Partial column 'valid_col' must NOT have been committed
        Statement check(conn->handle(), "SELECT count(*) FROM pragma_table_info('workspace') WHERE name = 'valid_col';");
        REQUIRE(check.step());
        CHECK(check.column_int64(0) == 0);
    }

    SECTION("Disk-based multi-version migration lifecycle on temporary SQLite file") {
        auto pid = std::to_string(::getpid());
        auto now = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path temp_db = fs::temp_directory_path() / ("test_migration_disk_" + pid + "_" + now + ".db");

        if (fs::exists(temp_db)) {
            fs::remove(temp_db);
        }

        // Phase 1: create and populate DB at Version 1
        {
            auto conn = Connection::open(temp_db.string());
            MigrationRunner runner;
            runner.apply_pending(*conn);
            REQUIRE(runner.get_current_version(*conn) == 1);

            conn->execute("INSERT INTO workspace (id, root_path, name) VALUES (1, '/disk/ws', 'Disk Workspace');");
        }

        // Phase 2: reopen DB from disk and upgrade from Version 1 to Version 2 and 3
        {
            auto conn = Connection::open(temp_db.string());
            MigrationRunner runner;
            runner.register_migration(Migration{
                .version = 2,
                .name = "002_disk_col",
                .up_sql = "ALTER TABLE workspace ADD COLUMN disk_tag TEXT DEFAULT 'v2';",
            });
            runner.register_migration(Migration{
                .version = 3,
                .name = "003_disk_table",
                .up_sql = "CREATE TABLE disk_audit (id INTEGER PRIMARY KEY, msg TEXT);",
            });

            CHECK(runner.get_current_version(*conn) == 1);
            runner.apply_pending(*conn);
            CHECK(runner.get_current_version(*conn) == 3);

            conn->execute("INSERT INTO disk_audit (msg) VALUES ('Migration completed');");
        }

        // Phase 3: reopen again, verify everything persisted on disk
        {
            auto conn = Connection::open(temp_db.string());
            MigrationRunner runner;
            runner.register_migration(Migration{.version = 2, .name = "002_disk_col", .up_sql = ""});
            runner.register_migration(Migration{.version = 3, .name = "003_disk_table", .up_sql = ""});

            CHECK(runner.get_current_version(*conn) == 3);

            Statement stmt_ws(conn->handle(), "SELECT name, disk_tag FROM workspace WHERE id = 1;");
            REQUIRE(stmt_ws.step());
            CHECK(stmt_ws.column_text(0) == "Disk Workspace");
            CHECK(stmt_ws.column_text(1) == "v2");

            Statement stmt_audit(conn->handle(), "SELECT msg FROM disk_audit WHERE id = 1;");
            REQUIRE(stmt_audit.step());
            CHECK(stmt_audit.column_text(0) == "Migration completed");
        }

        std::error_code ec;
        fs::remove(temp_db, ec);
        fs::remove(temp_db.string() + "-wal", ec);
        fs::remove(temp_db.string() + "-shm", ec);
    }
}
