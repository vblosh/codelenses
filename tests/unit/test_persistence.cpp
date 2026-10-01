#include <chrono>
#include <filesystem>
#include <thread>

#include "codelenses/db/connection.hpp"
#include "codelenses/db/database.hpp"
#include "codelenses/db/error.hpp"
#include "codelenses/db/statement.hpp"
#include "codelenses/db/transaction.hpp"
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using namespace codelenses;

TEST_CASE("Connection initializes SQLite pragmas correctly", "[db][connection]") {
    const std::string db_file = "test_pragmas.db";
    if (fs::exists(db_file)) {
        fs::remove(db_file);
    }

    auto conn = Connection::open(db_file);

    // 1. foreign_keys = ON
    Statement fk_stmt(conn->handle(), "PRAGMA foreign_keys;");
    REQUIRE(fk_stmt.step());
    REQUIRE(fk_stmt.column_int64(0) == 1);

    // 2. journal_mode = WAL
    Statement jm_stmt(conn->handle(), "PRAGMA journal_mode;");
    REQUIRE(jm_stmt.step());
    REQUIRE(jm_stmt.column_text(0) == "wal");

    // 3. synchronous = NORMAL (1)
    Statement sync_stmt(conn->handle(), "PRAGMA synchronous;");
    REQUIRE(sync_stmt.step());
    REQUIRE(sync_stmt.column_int64(0) == 1);

    // 4. busy_timeout = 5000
    Statement bt_stmt(conn->handle(), "PRAGMA busy_timeout;");
    REQUIRE(bt_stmt.step());
    REQUIRE(bt_stmt.column_int64(0) == 5000);

    conn.reset();
    if (fs::exists(db_file)) {
        fs::remove(db_file);
    }
}

TEST_CASE("Read-only database connections observe committed WAL snapshots", "[db][connection]") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto db_file =
        fs::temp_directory_path() / ("codelenses_readonly_" + std::to_string(nonce) + ".db");
    auto remove_database_files = [&] {
        std::error_code ec;
        fs::remove(db_file, ec);
        fs::remove(db_file.string() + "-wal", ec);
        fs::remove(db_file.string() + "-shm", ec);
    };
    remove_database_files();

    {
        auto writer = Database::open(db_file.string());
        Workspace workspace{.root_path = "/ws/read_only", .name = "Before commit"};
        const auto workspace_id = writer->workspaces().create(workspace);
        workspace.id = workspace_id;

        auto reader = writer->open_reader();
        REQUIRE(reader);

        Transaction read_snapshot(reader->connection(), TransactionType::deferred);
        REQUIRE(reader->workspaces().get_by_id(workspace_id)->name == "Before commit");

        {
            Transaction write_transaction(writer->connection());
            workspace.name = "After commit";
            REQUIRE(writer->workspaces().update(workspace));
            write_transaction.commit();
        }

        CHECK(reader->workspaces().get_by_id(workspace_id)->name == "Before commit");
        CHECK_THROWS_AS(reader->connection().execute("DELETE FROM workspace;"), DbError);

        read_snapshot.commit();
        CHECK(reader->workspaces().get_by_id(workspace_id)->name == "After commit");
    }

    remove_database_files();
}

TEST_CASE("Transaction RAII rollbacks on error and commits on request", "[db][transaction]") {
    auto db = Database::open_memory();

    SECTION("Commit persists data") {
        {
            Transaction tx(db->connection());
            Workspace ws{
                .root_path = "/ws/tx_commit",
                .name = "Commit Test",
            };
            db->workspaces().create(ws);
            tx.commit();
        }

        auto ws = db->workspaces().get_by_root_path("/ws/tx_commit");
        REQUIRE(ws.has_value());
    }

    SECTION("Uncommitted transaction rolls back on scope exit") {
        {
            Transaction tx(db->connection());
            Workspace ws{
                .root_path = "/ws/tx_rollback",
                .name = "Rollback Test",
            };
            db->workspaces().create(ws);
            // tx destroyed without commit()
        }

        auto ws = db->workspaces().get_by_root_path("/ws/tx_rollback");
        REQUIRE_FALSE(ws.has_value());
    }

    SECTION("Explicit rollback discards changes") {
        {
            Transaction tx(db->connection());
            Workspace ws{
                .root_path = "/ws/tx_explicit",
                .name = "Explicit Test",
            };
            db->workspaces().create(ws);
            tx.rollback();
        }

        auto ws = db->workspaces().get_by_root_path("/ws/tx_explicit");
        REQUIRE_FALSE(ws.has_value());
    }
}

TEST_CASE("Workspace repository and cascade deletion", "[db][workspace][cascade]") {
    auto db = Database::open_memory();

    Workspace ws{
        .root_path = "/home/user/project",
        .name = "Test Project",
        .include_patterns = {"src/**", "include/**"},
        .exclude_patterns = {"build/**"},
        .default_ignores = {".git", "node_modules"},
        .compile_commands_path = "/home/user/project/build/compile_commands.json",
        .default_compile_command = "gcc -Iinclude -DDEBUG=1 -std=c17",
        .revision = 1,
        .status = WorkspaceStatus::idle,
    };
    int64_t ws_id = db->workspaces().create(ws);
    REQUIRE(ws_id > 0);

    // Verify retrieval
    auto fetched = db->workspaces().get_by_id(ws_id);
    REQUIRE(fetched.has_value());
    REQUIRE(fetched->name == "Test Project");
    REQUIRE(fetched->include_patterns.size() == 2);
    REQUIRE(fetched->exclude_patterns.size() == 1);
    REQUIRE(fetched->default_ignores.size() == 2);
    REQUIRE(fetched->compile_commands_path == "/home/user/project/build/compile_commands.json");
    REQUIRE(fetched->default_compile_command == "gcc -Iinclude -DDEBUG=1 -std=c17");
    REQUIRE(fetched->revision == 1);
    REQUIRE(fetched->status == WorkspaceStatus::idle);

    // Update workspace
    fetched->name = "Updated Project";
    fetched->default_compile_command = "clang -Isrc -DRELEASE=1 -std=c2x";
    fetched->status = WorkspaceStatus::indexing;
    REQUIRE(db->workspaces().update(*fetched));

    auto updated = db->workspaces().get_by_id(ws_id);
    REQUIRE(updated->name == "Updated Project");
    REQUIRE(updated->default_compile_command == "clang -Isrc -DRELEASE=1 -std=c2x");
    REQUIRE(updated->status == WorkspaceStatus::indexing);

    // Revision increment
    int64_t new_rev = db->workspaces().increment_revision(ws_id);
    REQUIRE(new_rev == 2);

    // Create child records: job, file, symbol, occurrence, reference, relation, dependency,
    // diagnostic
    IndexJob job{
        .workspace_id = ws_id,
        .job_type = "full",
        .status = "completed",
    };
    int64_t job_id = db->jobs().create(job);
    REQUIRE(job_id > 0);

    FileRecord file{
        .workspace_id = ws_id,
        .path = "/home/user/project/src/main.cpp",
        .relative_path = "src/main.cpp",
        .name = "main.cpp",
        .extension = ".cpp",
        .language = "cpp",
        .size_bytes = 1024,
        .modified_ns = 123456789,
        .content_hash = "hash123",
        .last_index_job_id = job_id,
    };
    int64_t file_id = db->files().insert(file);
    REQUIRE(file_id > 0);

    Symbol sym{
        .workspace_id = ws_id,
        .file_id = file_id,
        .symbol_key = "func:main()",
        .name = "main",
        .kind = "function",
        .language = "cpp",
        .is_definition = true,
        .range = {.start_byte = 10,
                  .end_byte = 50,
                  .start_line = 1,
                  .start_column = 0,
                  .end_line = 5,
                  .end_column = 1},
    };
    int64_t sym_id = db->symbols().insert(sym);
    REQUIRE(sym_id > 0);

    Occurrence occ{
        .workspace_id = ws_id,
        .file_id = file_id,
        .symbol_id = sym_id,
        .occurrence_kind = "definition",
        .name = "main",
        .range = sym.range,
        .confidence = 1.0,
        .resolution = "resolved",
    };
    int64_t occ_id = db->occurrences().insert(occ);
    REQUIRE(occ_id > 0);

    ReferenceOccurrence ref{
        .workspace_id = ws_id,
        .source_file_id = file_id,
        .target_symbol_id = sym_id,
        .name = "main",
        .reference_kind = "call",
        .range = sym.range,
        .resolution = "resolved",
        .confidence = 1.0,
    };
    int64_t ref_id = db->references().insert(ref);
    REQUIRE(ref_id > 0);

    SymbolRelation rel{
        .workspace_id = ws_id,
        .source_symbol_id = sym_id,
        .relation_kind = "calls",
        .target_name = "printf",
        .resolution = "unresolved",
    };
    int64_t rel_id = db->relations().insert(rel);
    REQUIRE(rel_id > 0);

    FileDependency dep{
        .workspace_id = ws_id,
        .source_file_id = file_id,
        .dependency_kind = "include",
        .raw_name = "iostream",
        .resolution = "external",
    };
    int64_t dep_id = db->dependencies().insert(dep);
    REQUIRE(dep_id > 0);

    Diagnostic diag{
        .workspace_id = ws_id,
        .job_id = job_id,
        .file_id = file_id,
        .severity = "info",
        .source = "parser",
        .code = "info.parsed",
        .message = "File parsed successfully",
    };
    int64_t diag_id = db->diagnostics().insert(diag);
    REQUIRE(diag_id > 0);

    // Verify all exist before cascade delete
    REQUIRE(db->files().get_by_id(file_id).has_value());
    REQUIRE(db->symbols().get_by_id(sym_id).has_value());
    REQUIRE(db->occurrences().get_by_id(occ_id).has_value());
    REQUIRE(db->references().get_by_id(ref_id).has_value());
    REQUIRE(db->jobs().get_by_id(job_id).has_value());

    // Cascade delete workspace
    REQUIRE(db->workspaces().delete_by_id(ws_id));

    // Verify cascade deleted ALL child records
    REQUIRE_FALSE(db->workspaces().get_by_id(ws_id).has_value());
    REQUIRE_FALSE(db->files().get_by_id(file_id).has_value());
    REQUIRE_FALSE(db->symbols().get_by_id(sym_id).has_value());
    REQUIRE_FALSE(db->occurrences().get_by_id(occ_id).has_value());
    REQUIRE_FALSE(db->references().get_by_id(ref_id).has_value());
    REQUIRE_FALSE(db->jobs().get_by_id(job_id).has_value());
    REQUIRE(db->relations().find_by_source_symbol(sym_id).empty());
    REQUIRE(db->dependencies().list_by_source_file(file_id).empty());
    REQUIRE(db->diagnostics().list_by_workspace(ws_id).empty());
}

TEST_CASE("Transactional file replacement updates metadata and replaces derived data atomically",
          "[db][replacement]") {
    auto db = Database::open_memory();

    Workspace ws{.root_path = "/ws/replace", .name = "Replace WS"};
    int64_t ws_id = db->workspaces().create(ws);

    FileRecord file{
        .workspace_id = ws_id,
        .path = "/ws/replace/foo.cpp",
        .relative_path = "foo.cpp",
        .name = "foo.cpp",
        .language = "cpp",
        .size_bytes = 100,
        .modified_ns = 1000,
        .content_hash = "v1_hash",
    };
    int64_t file_id = db->files().insert(file);

    // Initial extraction data
    FileIndexData initial_data{
        .size_bytes = 100,
        .modified_ns = 1000,
        .content_hash = "v1_hash",
        .language = "cpp",
        .symbols =
            {
                Symbol{
                    .workspace_id = ws_id,
                    .file_id = file_id,
                    .symbol_key = "func:old_func()",
                    .name = "old_func",
                    .kind = "function",
                    .language = "cpp",
                    .range = {.start_byte = 0,
                              .end_byte = 50,
                              .start_line = 0,
                              .start_column = 0,
                              .end_line = 2,
                              .end_column = 1},
                },
            },
        .occurrences =
            {
                Occurrence{
                    .workspace_id = ws_id,
                    .file_id = file_id,
                    .occurrence_kind = "definition",
                    .name = "old_func",
                    .range = {.start_byte = 0,
                              .end_byte = 50,
                              .start_line = 0,
                              .start_column = 0,
                              .end_line = 2,
                              .end_column = 1},
                },
            },
        .references = {},
        .relations = {},
        .dependencies =
            {
                FileDependency{
                    .workspace_id = ws_id,
                    .source_file_id = file_id,
                    .dependency_kind = "include",
                    .raw_name = "old.h",
                },
            },
    };

    db->replace_file_index(file_id, initial_data);

    // Verify initial data exists
    auto old_syms = db->symbols().list_by_file(file_id);
    REQUIRE(old_syms.size() == 1);
    REQUIRE(old_syms[0].name == "old_func");

    auto old_deps = db->dependencies().list_by_source_file(file_id);
    REQUIRE(old_deps.size() == 1);
    REQUIRE(old_deps[0].raw_name == "old.h");

    auto updated_file = db->files().get_by_id(file_id);
    REQUIRE(updated_file->indexed_at.has_value());

    // Replace with new extraction data
    FileIndexData new_data{
        .size_bytes = 250,
        .modified_ns = 2000,
        .content_hash = "v2_hash",
        .language = "cpp",
        .symbols =
            {
                Symbol{
                    .workspace_id = ws_id,
                    .file_id = file_id,
                    .symbol_key = "func:new_func1()",
                    .name = "new_func1",
                    .kind = "function",
                    .language = "cpp",
                    .range = {.start_byte = 0,
                              .end_byte = 100,
                              .start_line = 0,
                              .start_column = 0,
                              .end_line = 4,
                              .end_column = 1},
                },
                Symbol{
                    .workspace_id = ws_id,
                    .file_id = file_id,
                    .symbol_key = "func:new_func2()",
                    .name = "new_func2",
                    .kind = "function",
                    .language = "cpp",
                    .range = {.start_byte = 110,
                              .end_byte = 220,
                              .start_line = 5,
                              .start_column = 0,
                              .end_line = 9,
                              .end_column = 1},
                },
            },
        .occurrences =
            {
                Occurrence{
                    .workspace_id = ws_id,
                    .file_id = file_id,
                    .occurrence_kind = "definition",
                    .name = "new_func1",
                    .range = {.start_byte = 0,
                              .end_byte = 100,
                              .start_line = 0,
                              .start_column = 0,
                              .end_line = 4,
                              .end_column = 1},
                },
                Occurrence{
                    .workspace_id = ws_id,
                    .file_id = file_id,
                    .occurrence_kind = "definition",
                    .name = "new_func2",
                    .range = {.start_byte = 110,
                              .end_byte = 220,
                              .start_line = 5,
                              .start_column = 0,
                              .end_line = 9,
                              .end_column = 1},
                },
            },
        .references = {},
        .relations = {},
        .dependencies =
            {
                FileDependency{
                    .workspace_id = ws_id,
                    .source_file_id = file_id,
                    .dependency_kind = "include",
                    .raw_name = "new.h",
                },
            },
    };

    db->replace_file_index(file_id, new_data);

    // Verify old records removed, new records present
    auto new_syms = db->symbols().list_by_file(file_id);
    REQUIRE(new_syms.size() == 2);
    REQUIRE(new_syms[0].name == "new_func1");
    REQUIRE(new_syms[1].name == "new_func2");

    auto new_deps = db->dependencies().list_by_source_file(file_id);
    REQUIRE(new_deps.size() == 1);
    REQUIRE(new_deps[0].raw_name == "new.h");

    auto f_final = db->files().get_by_id(file_id);
    REQUIRE(f_final->size_bytes == 250);
    REQUIRE(f_final->content_hash == "v2_hash");

    SECTION("Rollback on failure during file replacement keeps previous state") {
        // Intentionally create invalid data (e.g. invalid start_byte < 0 violating CHECK
        // constraint)
        FileIndexData bad_data = new_data;
        bad_data.symbols.push_back(Symbol{
            .workspace_id = ws_id,
            .file_id = file_id,
            .symbol_key = "func:bad()",
            .name = "bad",
            .kind = "function",
            .language = "cpp",
            .range = {.start_byte = -10,
                      .end_byte = 10}, // Invalid start_byte violates CHECK (start_byte >= 0)
        });

        REQUIRE_THROWS_AS(db->replace_file_index(file_id, bad_data), DbError);

        // Verify the database still has the 2 new_func symbols and was not partially updated or
        // corrupted
        auto current_syms = db->symbols().list_by_file(file_id);
        REQUIRE(current_syms.size() == 2);
        REQUIRE(current_syms[0].name == "new_func1");
        REQUIRE(current_syms[1].name == "new_func2");
    }
}

TEST_CASE("File state comparison and stale record cleanup", "[db][files][cleanup]") {
    auto db = Database::open_memory();

    Workspace ws{.root_path = "/ws/files", .name = "File State WS"};
    int64_t ws_id = db->workspaces().create(ws);

    int64_t f1 = db->files().insert(FileRecord{
        .workspace_id = ws_id,
        .path = "/ws/files/a.cpp",
        .relative_path = "a.cpp",
        .name = "a.cpp",
        .size_bytes = 100,
        .modified_ns = 1000,
        .content_hash = "hash_a",
    });

    int64_t f2 = db->files().insert(FileRecord{
        .workspace_id = ws_id,
        .path = "/ws/files/b.cpp",
        .relative_path = "b.cpp",
        .name = "b.cpp",
        .size_bytes = 200,
        .modified_ns = 2000,
        .content_hash = "hash_b",
    });

    // Add a symbol to f2
    db->symbols().insert(Symbol{
        .workspace_id = ws_id,
        .file_id = f2,
        .symbol_key = "func:b()",
        .name = "b_func",
        .kind = "function",
        .language = "cpp",
        .range = {.start_byte = 0,
                  .end_byte = 50,
                  .start_line = 0,
                  .start_column = 0,
                  .end_line = 1,
                  .end_column = 1},
    });

    // Query states
    auto states = db->files().get_file_states(ws_id);
    REQUIRE(states.size() == 2);
    REQUIRE(states[0].relative_path == "a.cpp");
    REQUIRE(states[0].content_hash == "hash_a");
    REQUIRE(states[1].relative_path == "b.cpp");

    // Mark missing as deleted: suppose discovery only found f1
    int64_t deleted_count = db->files().mark_missing_as_deleted(ws_id, {f1});
    REQUIRE(deleted_count == 1);

    auto f2_rec = db->files().get_by_id(f2);
    REQUIRE(f2_rec->is_deleted == true);

    auto f1_rec = db->files().get_by_id(f1);
    REQUIRE(f1_rec->is_deleted == false);

    // Derived data cleanup (Section 7)
    db->files().cleanup_deleted_files_derived_data(ws_id);

    // Verify symbols of f2 deleted
    REQUIRE(db->symbols().list_by_file(f2).empty());

    // Tombstone still exists
    REQUIRE(db->files().get_by_id(f2).has_value());

    // Permanently remove tombstones
    int64_t purged = db->files().delete_tombstones(ws_id);
    REQUIRE(purged == 1);
    REQUIRE_FALSE(db->files().get_by_id(f2).has_value());
}

TEST_CASE("Symbol outline, smallest range at offset, and symbol key queries", "[db][symbols]") {
    auto db = Database::open_memory();

    Workspace ws{.root_path = "/ws/sym", .name = "Symbols WS"};
    int64_t ws_id = db->workspaces().create(ws);

    FileRecord file{
        .workspace_id = ws_id,
        .path = "/ws/sym/code.cpp",
        .relative_path = "code.cpp",
        .name = "code.cpp",
    };
    int64_t file_id = db->files().insert(file);

    // Class from byte 0 to 200
    int64_t class_id = db->symbols().insert(Symbol{
        .workspace_id = ws_id,
        .file_id = file_id,
        .symbol_key = "class:MyClass",
        .name = "MyClass",
        .kind = "class",
        .language = "cpp",
        .range = {.start_byte = 0,
                  .end_byte = 200,
                  .start_line = 0,
                  .start_column = 0,
                  .end_line = 20,
                  .end_column = 1},
    });

    // Nested method inside MyClass from byte 50 to 100
    int64_t method_id = db->symbols().insert(Symbol{
        .workspace_id = ws_id,
        .file_id = file_id,
        .symbol_key = "method:MyClass::foo()",
        .name = "foo",
        .kind = "method",
        .language = "cpp",
        .scope_symbol_id = class_id,
        .range = {.start_byte = 50,
                  .end_byte = 100,
                  .start_line = 5,
                  .start_column = 4,
                  .end_line = 8,
                  .end_column = 5},
    });

    // Independent function from byte 210 to 250
    db->symbols().insert(Symbol{
        .workspace_id = ws_id,
        .file_id = file_id,
        .symbol_key = "func:bar()",
        .name = "bar",
        .kind = "function",
        .language = "cpp",
        .range = {.start_byte = 210,
                  .end_byte = 250,
                  .start_line = 22,
                  .start_column = 0,
                  .end_line = 25,
                  .end_column = 1},
    });

    // 1. Outline query (sorted by start_byte, end_byte DESC, name)
    auto outline = db->symbols().list_by_file(file_id);
    REQUIRE(outline.size() == 3);
    REQUIRE(outline[0].name == "MyClass");
    REQUIRE(outline[1].name == "foo");
    REQUIRE(outline[2].name == "bar");

    // 2. Position query at offset 60 (inside both MyClass (0..200) and foo (50..100))
    // Section 8: ORDER BY (end_byte - start_byte) ASC LIMIT 1 selects the innermost symbol (foo)
    auto innermost = db->symbols().find_at_offset(file_id, 60);
    REQUIRE(innermost.has_value());
    REQUIRE(innermost->id == method_id);
    REQUIRE(innermost->name == "foo");

    // Position query at offset 10 (inside MyClass, outside foo)
    auto outer = db->symbols().find_at_offset(file_id, 10);
    REQUIRE(outer.has_value());
    REQUIRE(outer->id == class_id);
    REQUIRE(outer->name == "MyClass");

    // 3. Query by key
    auto by_key = db->symbols().get_by_key(ws_id, "class:MyClass");
    REQUIRE(by_key.size() == 1);
    REQUIRE(by_key[0].name == "MyClass");
}

TEST_CASE("Referencer, caller, and callee navigation queries", "[db][references]") {
    auto db = Database::open_memory();

    Workspace ws{.root_path = "/ws/ref", .name = "References WS"};
    int64_t ws_id = db->workspaces().create(ws);

    int64_t f_header = db->files().insert(FileRecord{
        .workspace_id = ws_id,
        .path = "/ws/ref/calc.h",
        .relative_path = "calc.h",
        .name = "calc.h",
    });

    int64_t f_src = db->files().insert(FileRecord{
        .workspace_id = ws_id,
        .path = "/ws/ref/app.cpp",
        .relative_path = "app.cpp",
        .name = "app.cpp",
    });

    // Target function calculate() in calc.h
    int64_t target_func = db->symbols().insert(Symbol{
        .workspace_id = ws_id,
        .file_id = f_header,
        .symbol_key = "func:calculate()",
        .name = "calculate",
        .kind = "function",
        .language = "cpp",
        .is_definition = true,
        .range = {.start_byte = 10,
                  .end_byte = 60,
                  .start_line = 1,
                  .start_column = 0,
                  .end_line = 3,
                  .end_column = 1},
    });

    // Calling function run_app() in app.cpp
    int64_t caller_func = db->symbols().insert(Symbol{
        .workspace_id = ws_id,
        .file_id = f_src,
        .symbol_key = "func:run_app()",
        .name = "run_app",
        .qualified_name = "app::run_app",
        .kind = "function",
        .language = "cpp",
        .is_definition = true,
        .range = {.start_byte = 0,
                  .end_byte = 100,
                  .start_line = 0,
                  .start_column = 0,
                  .end_line = 8,
                  .end_column = 1},
    });

    // Reference from run_app() to calculate()
    db->references().insert(ReferenceOccurrence{
        .workspace_id = ws_id,
        .source_file_id = f_src,
        .source_symbol_id = caller_func,
        .target_symbol_id = target_func,
        .name = "calculate",
        .reference_kind = "call",
        .range = {.start_byte = 40,
                  .end_byte = 49,
                  .start_line = 4,
                  .start_column = 4,
                  .end_line = 4,
                  .end_column = 13},
        .resolution = "resolved",
        .confidence = 1.0,
    });

    // 1. find_referencers
    auto referencers = db->references().find_referencers(ws_id, target_func);
    REQUIRE(referencers.size() == 1);
    const auto& ref = referencers[0];
    REQUIRE(ref.name == "calculate");
    REQUIRE(ref.reference_kind == "call");
    REQUIRE(ref.relative_path == "app.cpp");
    REQUIRE(ref.start_line == 4);
    REQUIRE(ref.start_column == 4);
    REQUIRE(ref.containing_symbol_id == caller_func);
    REQUIRE(ref.containing_symbol_name == "run_app");
    REQUIRE(ref.containing_qualified_name == "app::run_app");
    REQUIRE(ref.resolution == "resolved");
    REQUIRE(ref.confidence == 1.0);

    // 2. find_callers of calculate()
    auto callers = db->references().find_callers(target_func);
    REQUIRE(callers.size() == 1);
    REQUIRE(callers[0].symbol_id == caller_func);
    REQUIRE(callers[0].name == "run_app");
    REQUIRE(callers[0].qualified_name == "app::run_app");

    // 3. find_callees of run_app()
    auto callees = db->references().find_callees(caller_func);
    REQUIRE(callees.size() == 1);
    REQUIRE(callees[0].symbol_id == target_func);
    REQUIRE(callees[0].name == "calculate");
}

TEST_CASE("Ambiguous candidates and unresolved calls in references and search", "[db][references]") {
    auto db = Database::open_memory();

    Workspace ws{.root_path = "/ws/unres", .name = "Unresolved WS"};
    int64_t ws_id = db->workspaces().create(ws);

    int64_t f_iface = db->files().insert(FileRecord{
        .workspace_id = ws_id,
        .path = "/ws/unres/interfaces.h",
        .relative_path = "interfaces.h",
        .name = "interfaces.h",
    });

    int64_t f_impl = db->files().insert(FileRecord{
        .workspace_id = ws_id,
        .path = "/ws/unres/formatter.h",
        .relative_path = "formatter.h",
        .name = "formatter.h",
    });

    int64_t f_sink = db->files().insert(FileRecord{
        .workspace_id = ws_id,
        .path = "/ws/unres/sink.cpp",
        .relative_path = "sink.cpp",
        .name = "sink.cpp",
    });

    // Interface method IFormatter::Format
    int64_t iface_format = db->symbols().insert(Symbol{
        .workspace_id = ws_id,
        .file_id = f_iface,
        .symbol_key = "method:IFormatter::Format(ostream&,const LogRecord&)",
        .name = "Format",
        .qualified_name = "IFormatter::Format",
        .kind = "method",
        .language = "cpp",
        .signature = "virtual void Format(std::ostream& os, const LogRecord& logdata) = 0",
        .is_declaration = true,
        .range = {.start_byte = 10, .end_byte = 80, .start_line = 5, .start_column = 4, .end_line = 5, .end_column = 70},
    });

    // Impl method DefaultFormatter::Format
    int64_t impl_format = db->symbols().insert(Symbol{
        .workspace_id = ws_id,
        .file_id = f_impl,
        .symbol_key = "method:DefaultFormatter::Format(ostream&,const LogRecord&)",
        .name = "Format",
        .qualified_name = "DefaultFormatter::Format",
        .kind = "method",
        .language = "cpp",
        .signature = "void Format(std::ostream& os, const LogRecord& logdata) override",
        .is_definition = true,
        .range = {.start_byte = 10, .end_byte = 80, .start_line = 8, .start_column = 4, .end_line = 8, .end_column = 70},
    });

    // Caller method OstreamSink::Log in sink.cpp
    int64_t caller_log = db->symbols().insert(Symbol{
        .workspace_id = ws_id,
        .file_id = f_sink,
        .symbol_key = "method:OstreamSink::Log(const LogRecord&)",
        .name = "Log",
        .qualified_name = "OstreamSink::Log",
        .kind = "method",
        .language = "cpp",
        .signature = "void Log(const LogRecord& record)",
        .is_definition = true,
        .range = {.start_byte = 10, .end_byte = 120, .start_line = 10, .start_column = 0, .end_line = 15, .end_column = 1},
    });

    // Reference 1: Ambiguous call (formatter->Format(*os, logdata)) where candidates include both
    db->references().insert(ReferenceOccurrence{
        .workspace_id = ws_id,
        .source_file_id = f_sink,
        .source_symbol_id = caller_log,
        .target_symbol_id = iface_format,
        .name = "Format",
        .reference_kind = "call",
        .range = {.start_byte = 40, .end_byte = 46, .start_line = 12, .start_column = 19, .end_line = 12, .end_column = 25},
        .resolution = "ambiguous",
        .confidence = 0.5,
        .metadata_json = "{\"candidates\":[" + std::to_string(iface_format) + "," + std::to_string(impl_format) + "]}",
    });

    // Reference 2: Completely unresolved call to Format in another function
    db->references().insert(ReferenceOccurrence{
        .workspace_id = ws_id,
        .source_file_id = f_sink,
        .source_symbol_id = caller_log,
        .target_symbol_id = std::nullopt,
        .name = "Format",
        .reference_kind = "call",
        .range = {.start_byte = 80, .end_byte = 86, .start_line = 14, .start_column = 4, .end_line = 14, .end_column = 10},
        .resolution = "unresolved",
        .confidence = 0.0,
    });

    // 1. find_referencers for IFormatter::Format should return both ambiguous and unresolved calls
    auto refs_iface = db->references().find_referencers(ws_id, iface_format);
    REQUIRE(refs_iface.size() == 2);

    // 2. find_referencers for DefaultFormatter::Format should also match the ambiguous call via candidate JSON and unresolved call
    auto refs_impl = db->references().find_referencers(ws_id, impl_format);
    REQUIRE(refs_impl.size() == 2);

    // 3. find_callers should find OstreamSink::Log for both methods
    auto callers_iface = db->references().find_callers(ws_id, iface_format);
    REQUIRE(callers_iface.size() == 1);
    REQUIRE(callers_iface[0].symbol_id == caller_log);
    REQUIRE(callers_iface[0].name == "Log");

    auto callers_impl = db->references().find_callers(ws_id, impl_format);
    REQUIRE(callers_impl.size() == 1);
    REQUIRE(callers_impl[0].symbol_id == caller_log);

    // 4. find_unresolved_calls
    auto unres = db->references().find_unresolved_calls(ws_id, "Format", 10);
    REQUIRE(unres.size() == 2);
    REQUIRE(unres[0].name == "Format");
    REQUIRE(unres[0].kind == "unresolved_call");
    REQUIRE(unres[0].relative_path == "sink.cpp");
}

TEST_CASE("FTS5 full-text symbol search triggers and queries", "[db][fts5]") {
    auto db = Database::open_memory();

    Workspace ws{.root_path = "/ws/fts", .name = "FTS WS"};
    int64_t ws_id = db->workspaces().create(ws);

    FileRecord file{
        .workspace_id = ws_id,
        .path = "/ws/fts/math.cpp",
        .relative_path = "math.cpp",
        .name = "math.cpp",
    };
    int64_t file_id = db->files().insert(file);

    // 1. Insert symbol -> triggers automatic insertion into symbol_search
    int64_t sym_id = db->symbols().insert(Symbol{
        .workspace_id = ws_id,
        .file_id = file_id,
        .symbol_key = "func:matrix_multiply()",
        .name = "matrix_multiply",
        .qualified_name = "math::matrix_multiply",
        .kind = "function",
        .language = "cpp",
        .signature = "Matrix matrix_multiply(const Matrix& a, const Matrix& b)",
        .documentation = "Multiplies two matrices",
        .range = {.start_byte = 0,
                  .end_byte = 80,
                  .start_line = 0,
                  .start_column = 0,
                  .end_line = 3,
                  .end_column = 1},
    });

    // Search by name
    auto results = db->fts().search_symbols(ws_id, "matrix_multiply");
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].id == sym_id);
    REQUIRE(results[0].name == "matrix_multiply");
    REQUIRE(results[0].qualified_name == "math::matrix_multiply");

    // Search by token in signature or documentation
    auto doc_results = db->fts().search_symbols(ws_id, "Multiplies");
    REQUIRE(doc_results.size() == 1);
    REQUIRE(doc_results[0].id == sym_id);

    // 2. Update symbol -> triggers update in symbol_search
    Statement update_sym(db->connection().handle(), R"SQL(
        UPDATE symbol
        SET name = 'tensor_multiply',
            qualified_name = 'math::tensor_multiply',
            signature = 'Matrix tensor_multiply(const Matrix& a, const Matrix& b)',
            documentation = 'Multiplies two tensors'
        WHERE id = ?;
    )SQL");
    update_sym.bind_int64(1, sym_id);
    update_sym.execute();

    auto updated_results = db->fts().search_symbols(ws_id, "tensor_multiply");
    REQUIRE(updated_results.size() == 1);

    auto old_query = db->fts().search_symbols(ws_id, "matrix_multiply");
    REQUIRE(old_query.empty());

    // 3. Delete symbol -> triggers deletion in symbol_search
    REQUIRE(db->symbols().delete_by_file(file_id));
    auto deleted_query = db->fts().search_symbols(ws_id, "tensor_multiply");
    REQUIRE(deleted_query.empty());
}

TEST_CASE("FTS5 source file search", "[db][fts5][file_search]") {
    auto db = Database::open_memory();

    Workspace ws{.root_path = "/ws/file_search", .name = "File Search WS"};
    int64_t ws_id = db->workspaces().create(ws);

    FileRecord file{
        .workspace_id = ws_id,
        .path = "/ws/file_search/hello.txt",
        .relative_path = "hello.txt",
        .name = "hello.txt",
    };
    int64_t file_id = db->files().insert(file);

    db->fts().insert_or_update_file_content(FileContent{
        .file_id = file_id,
        .workspace_id = ws_id,
        .content = "The quick brown fox jumps over the lazy dog",
        .content_hash = "h1",
    });

    auto results = db->fts().search_files(ws_id, "fox");
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].file_id == file_id);
    REQUIRE(results[0].snippet.find("fox") != std::string::npos);
}

TEST_CASE("WAL mode enables readers while writer operates", "[db][wal]") {
    const std::string db_file = "test_wal_concurrency.db";
    if (fs::exists(db_file)) {
        fs::remove(db_file);
    }

    // Initialize database
    {
        auto db = Database::open(db_file, /*apply_migrations=*/true);
        Workspace ws{.root_path = "/ws/wal", .name = "WAL WS"};
        db->workspaces().create(ws);
    }

    // Open writer connection and reader connection simultaneously
    auto writer = Database::open(db_file, /*apply_migrations=*/false);
    auto reader = Database::open(db_file, /*apply_migrations=*/false);

    // Start a transaction on writer
    {
        Transaction tx(writer->connection(), TransactionType::immediate);
        Workspace ws{.root_path = "/ws/wal_writer", .name = "Writer WS"};
        writer->workspaces().create(ws);

        // Reader should still be able to query without blocking or error
        auto reader_ws = reader->workspaces().get_by_root_path("/ws/wal");
        REQUIRE(reader_ws.has_value());
        REQUIRE(reader_ws->name == "WAL WS");

        tx.commit();
    }

    // After commit, reader sees new workspace
    auto reader_new_ws = reader->workspaces().get_by_root_path("/ws/wal_writer");
    REQUIRE(reader_new_ws.has_value());
    REQUIRE(reader_new_ws->name == "Writer WS");

    writer.reset();
    reader.reset();
    if (fs::exists(db_file)) {
        fs::remove(db_file);
    }
}

TEST_CASE("Transactional file replacement remaps symbol IDs to newly generated row IDs",
          "[db][replacement][remap]") {
    auto db = Database::open_memory();

    Workspace ws{.root_path = "/ws/remap", .name = "Remap WS"};
    int64_t ws_id = db->workspaces().create(ws);

    FileRecord file{
        .workspace_id = ws_id,
        .path = "/ws/remap/nested.cpp",
        .relative_path = "nested.cpp",
        .name = "nested.cpp",
    };
    int64_t file_id = db->files().insert(file);

    // Provide input symbols with temporary IDs: class (id 100), method (id 200, scope 100)
    FileIndexData data{
        .size_bytes = 500,
        .modified_ns = 1234,
        .language = "cpp",
        .symbols =
            {
                Symbol{
                    .id = 100, // temporary input ID
                    .workspace_id = ws_id,
                    .file_id = file_id,
                    .symbol_key = "class:Container",
                    .name = "Container",
                    .kind = "class",
                    .language = "cpp",
                    .range = {.start_byte = 0,
                              .end_byte = 300,
                              .start_line = 0,
                              .start_column = 0,
                              .end_line = 10,
                              .end_column = 1},
                },
                Symbol{
                    .id = 200, // temporary input ID
                    .workspace_id = ws_id,
                    .file_id = file_id,
                    .symbol_key = "method:Container::action()",
                    .name = "action",
                    .kind = "method",
                    .language = "cpp",
                    .scope_symbol_id = 100, // links to class Container by temporary ID
                    .range = {.start_byte = 50,
                              .end_byte = 150,
                              .start_line = 2,
                              .start_column = 4,
                              .end_line = 6,
                              .end_column = 5},
                },
            },
        .occurrences =
            {
                Occurrence{
                    .workspace_id = ws_id,
                    .file_id = file_id,
                    .symbol_id = 100, // links to Container by temporary ID
                    .occurrence_kind = "definition",
                    .name = "Container",
                    .range = {.start_byte = 0,
                              .end_byte = 300,
                              .start_line = 0,
                              .start_column = 0,
                              .end_line = 10,
                              .end_column = 1},
                },
                Occurrence{
                    .workspace_id = ws_id,
                    .file_id = file_id,
                    .symbol_id = 200, // links to action by temporary ID
                    .occurrence_kind = "definition",
                    .name = "action",
                    .range = {.start_byte = 50,
                              .end_byte = 150,
                              .start_line = 2,
                              .start_column = 4,
                              .end_line = 6,
                              .end_column = 5},
                },
            },
        .references =
            {
                ReferenceOccurrence{
                    .workspace_id = ws_id,
                    .source_file_id = file_id,
                    .source_symbol_id = 200, // action
                    .target_symbol_id = 100, // Container
                    .name = "Container",
                    .reference_kind = "instantiation",
                    .range = {.start_byte = 60,
                              .end_byte = 70,
                              .start_line = 3,
                              .start_column = 8,
                              .end_line = 3,
                              .end_column = 17},
                },
            },
        .relations =
            {
                SymbolRelation{
                    .workspace_id = ws_id,
                    .source_symbol_id = 100, // Container
                    .target_symbol_id = 200, // action
                    .relation_kind = "contains",
                },
            },
    };

    db->replace_file_index(file_id, data);

    // Retrieve inserted symbols from database
    auto syms = db->symbols().list_by_file(file_id);
    REQUIRE(syms.size() == 2);
    int64_t new_class_id = syms[0].id;
    int64_t new_method_id = syms[1].id;
    REQUIRE(new_class_id != 100);
    REQUIRE(new_method_id != 200);

    // 1. Verify nested symbol scope_symbol_id was remapped
    REQUIRE(syms[1].scope_symbol_id.has_value());
    REQUIRE(*syms[1].scope_symbol_id == new_class_id);

    // 2. Verify occurrences were remapped
    auto occs = db->occurrences().list_by_file(file_id);
    REQUIRE(occs.size() == 2);
    REQUIRE(occs[0].symbol_id.has_value());
    REQUIRE(*occs[0].symbol_id == new_class_id);
    REQUIRE(occs[1].symbol_id.has_value());
    REQUIRE(*occs[1].symbol_id == new_method_id);

    // 3. Verify references were remapped
    auto refs = db->references().find_referencers(ws_id, new_class_id);
    REQUIRE(refs.size() == 1);
    REQUIRE(refs[0].containing_symbol_id == new_method_id);
    REQUIRE(refs[0].containing_symbol_name == "action");

    // 4. Verify relations were remapped
    auto rels = db->relations().find_by_source_symbol(new_class_id);
    REQUIRE(rels.size() == 1);
    REQUIRE(rels[0].target_symbol_id == new_method_id);
}

TEST_CASE("FTS search handles malformed queries via phrase fallback without throwing",
          "[db][fts5][malformed]") {
    auto db = Database::open_memory();

    Workspace ws{.root_path = "/ws/malformed", .name = "Malformed WS"};
    int64_t ws_id = db->workspaces().create(ws);

    FileRecord file{
        .workspace_id = ws_id,
        .path = "/ws/malformed/syntax.cpp",
        .relative_path = "syntax.cpp",
        .name = "syntax.cpp",
    };
    int64_t file_id = db->files().insert(file);

    db->symbols().insert(Symbol{
        .workspace_id = ws_id,
        .file_id = file_id,
        .symbol_key = "func:parse_query()",
        .name = "parse_query",
        .kind = "function",
        .language = "cpp",
        .signature = "void parse_query(std::string: input)",
        .documentation = "Parses complex user queries",
        .range = {.start_byte = 0,
                  .end_byte = 50,
                  .start_line = 0,
                  .start_column = 0,
                  .end_line = 2,
                  .end_column = 1},
    });

    db->fts().insert_or_update_file_content(FileContent{
        .file_id = file_id,
        .workspace_id = ws_id,
        .content = "void parse_query(std::string: input) { return; }",
        .content_hash = "h1",
    });

    // Queries with colons, unbalanced quotes, or syntax operators that trigger FTS5 syntax errors
    // but contain tokens present in the indexed document
    std::vector<std::string> malformed_matching_queries = {
        "parse_query:",
        "\"parse_query",
        "parse_query*",
    };

    for (const auto& q : malformed_matching_queries) {
        // Must not throw DbError; should fallback to phrase search and find parse_query
        auto sym_res = db->fts().search_symbols(ws_id, q);
        CHECK_FALSE(sym_res.empty());

        auto file_res = db->fts().search_files(ws_id, q);
        CHECK_FALSE(file_res.empty());
    }

    // Malformed queries with tokens not present in the indexed document
    std::vector<std::string> malformed_non_matching_queries = {
        "AND NOT",
        "\"unclosed",
        "nonexistent:",
    };

    for (const auto& q : malformed_non_matching_queries) {
        // Must not throw DbError; executes gracefully without crashing and returns empty
        auto sym_res = db->fts().search_symbols(ws_id, q);
        CHECK(sym_res.empty());

        auto file_res = db->fts().search_files(ws_id, q);
        CHECK(file_res.empty());
    }
}

TEST_CASE("FTS source search excludes tombstoned files and cleans up file content",
          "[db][fts5][tombstone]") {
    auto db = Database::open_memory();

    Workspace ws{.root_path = "/ws/tombstone", .name = "Tombstone WS"};
    int64_t ws_id = db->workspaces().create(ws);

    int64_t f1 = db->files().insert(FileRecord{
        .workspace_id = ws_id,
        .path = "/ws/tombstone/live.txt",
        .relative_path = "live.txt",
        .name = "live.txt",
    });

    int64_t f2 = db->files().insert(FileRecord{
        .workspace_id = ws_id,
        .path = "/ws/tombstone/dead.txt",
        .relative_path = "dead.txt",
        .name = "dead.txt",
    });

    db->fts().insert_or_update_file_content(FileContent{
        .file_id = f1,
        .workspace_id = ws_id,
        .content = "common secret keyword in live file",
        .content_hash = "h_live",
    });

    db->fts().insert_or_update_file_content(FileContent{
        .file_id = f2,
        .workspace_id = ws_id,
        .content = "common secret keyword in deleted file",
        .content_hash = "h_dead",
    });

    // Before tombstoning, both files match
    auto both = db->fts().search_files(ws_id, "secret");
    REQUIRE(both.size() == 2);

    // Tombstone f2 (mark is_deleted = 1)
    db->files().mark_deleted(f2);

    // 1. Immediately after tombstoning, search_files must exclude f2
    auto only_live = db->fts().search_files(ws_id, "secret");
    REQUIRE(only_live.size() == 1);
    REQUIRE(only_live[0].file_id == f1);

    // 2. Perform derived data cleanup
    db->files().cleanup_deleted_files_derived_data(ws_id);

    // 3. After derived data cleanup, search_files still only returns f1
    auto after_cleanup = db->fts().search_files(ws_id, "secret");
    REQUIRE(after_cleanup.size() == 1);
    REQUIRE(after_cleanup[0].file_id == f1);
}
