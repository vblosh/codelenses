#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "codelenses/db/database.hpp"
#include "codelenses/index/indexer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace codelenses;
using namespace codelenses::index;

namespace {

struct PipelineTestWorkspace {
    fs::path root;
    fs::path db_file;
    std::unique_ptr<Database> db;

    PipelineTestWorkspace() {
        auto pid = std::to_string(::getpid());
        auto now = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        root = fs::temp_directory_path() / ("codelenses_test_pipe_" + pid + "_" + now);
        db_file = fs::temp_directory_path() / ("codelenses_test_pipe_" + pid + "_" + now + ".db");
        fs::remove_all(root);
        fs::remove(db_file);
        fs::create_directories(root);

        db = Database::open(db_file.string(), true);
    }

    ~PipelineTestWorkspace() {
        db.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::remove(db_file, ec);
        fs::remove(db_file.string() + "-wal", ec);
        fs::remove(db_file.string() + "-shm", ec);
    }

    void write_file(const fs::path& rel_path, const std::string& content) {
        auto full = root / rel_path;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out << content;
    }

    void remove_file(const fs::path& rel_path) {
        auto full = root / rel_path;
        std::error_code ec;
        fs::remove(full, ec);
    }

    int64_t create_workspace(const std::string& name = "Test WS") {
        Workspace ws{
            .root_path = root.string(),
            .name = name,
        };
        return db->workspaces().create(ws);
    }
};

} // namespace

TEST_CASE("Index job lifecycle and progress counters (D-06)", "[index][pipeline]") {
    PipelineTestWorkspace env;
    env.write_file("src/main.c", "int main() { return 0; }\n");
    env.write_file("src/util.c", "int add(int a, int b) { return a + b; }\n");

    int64_t ws_id = env.create_workspace();
    REQUIRE(ws_id > 0);

    IndexingPipeline pipeline(*env.db);

    SECTION("Full indexing updates lifecycle and progress") {
        auto res = pipeline.run_indexing(ws_id, "full", true);
        REQUIRE(res.has_value());
        REQUIRE(res->status == "completed");
        REQUIRE(res->files_total == 2);
        REQUIRE(res->files_processed == 2);
        REQUIRE(res->files_skipped == 0);
        REQUIRE(res->workspace_revision.has_value());
        REQUIRE(*res->workspace_revision == 1);

        // Verify workspace status is ready
        auto ws = env.db->workspaces().get_by_id(ws_id);
        REQUIRE(ws.has_value());
        REQUIRE(ws->status == WorkspaceStatus::ready);
        REQUIRE(ws->revision == 1);

        // Verify index job in database
        auto job = env.db->jobs().get_by_id(res->job_id);
        REQUIRE(job.has_value());
        REQUIRE(job->status == "completed");
        REQUIRE(job->files_total == 2);
        REQUIRE(job->files_processed == 2);
        REQUIRE(job->files_skipped == 0);
        REQUIRE(job->started_at.has_value());
        REQUIRE(job->finished_at.has_value());
        REQUIRE(job->workspace_revision == 1);
    }
}

TEST_CASE("Per-file extraction flow and transactional replacement (D-08, D-09)",
          "[index][pipeline]") {
    PipelineTestWorkspace env;
    env.write_file("src/math.c", R"C(
#include <stdio.h>

int multiply(int x, int y) {
    return x * y;
}

int calculate(int a) {
    return multiply(a, 2);
}
)C");

    int64_t ws_id = env.create_workspace();
    IndexingPipeline pipeline(*env.db);

    auto res = pipeline.run_indexing(ws_id, "full", true);
    REQUIRE(res.has_value());
    REQUIRE(res->files_processed == 1);

    // Verify symbols extracted
    auto files = env.db->files().list_by_workspace(ws_id);
    REQUIRE(files.size() == 1);
    int64_t file_id = files[0].id;

    auto symbols = env.db->symbols().list_by_file(file_id);
    REQUIRE(symbols.size() >= 2);

    bool has_multiply = false;
    bool has_calculate = false;
    for (const auto& sym : symbols) {
        if (sym.name == "multiply") {
            has_multiply = true;
            REQUIRE(sym.kind == "function");
            REQUIRE(sym.is_definition);
        }
        if (sym.name == "calculate") {
            has_calculate = true;
            REQUIRE(sym.kind == "function");
        }
    }
    REQUIRE(has_multiply);
    REQUIRE(has_calculate);

    // Verify occurrences
    auto occurrences = env.db->occurrences().list_by_file(file_id);
    REQUIRE_FALSE(occurrences.empty());

    // Verify dependencies (include stdio.h)
    auto deps = env.db->dependencies().list_by_source_file(file_id);
    REQUIRE_FALSE(deps.empty());
}

TEST_CASE("Incremental indexing with unchanged, modified, renamed, and deleted files (D-10, D-13)",
          "[index][pipeline][incremental]") {
    PipelineTestWorkspace env;
    env.write_file("src/unchanged.c", "int u() { return 1; }\n");
    env.write_file("src/to_modify.c", "int m1() { return 10; }\n");
    env.write_file("src/to_delete.c", "int d() { return 20; }\n");

    int64_t ws_id = env.create_workspace();
    IndexingPipeline pipeline(*env.db);

    // 1. Initial full run
    auto res1 = pipeline.run_indexing(ws_id, "incremental", false);
    REQUIRE(res1.has_value());
    REQUIRE(res1->status == "completed");
    REQUIRE(res1->files_processed == 3);
    REQUIRE(res1->files_skipped == 0);
    REQUIRE(res1->workspace_revision == 1);

    auto files1 = env.db->files().list_by_workspace(ws_id, false);
    REQUIRE(files1.size() == 3);

    // 2. Incremental run with no changes -> all skipped!
    auto res2 = pipeline.run_indexing(ws_id, "incremental", false);
    REQUIRE(res2.has_value());
    REQUIRE(res2->status == "completed");
    REQUIRE(res2->files_processed == 0);
    REQUIRE(res2->files_skipped == 3);
    REQUIRE(res2->workspace_revision == 2);

    // 3. Modify one file
    // Wait briefly or write new content to change size/mtime
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    env.write_file("src/to_modify.c", "int m2() { return 999; }\nint m3() { return 1000; }\n");

    auto res3 = pipeline.run_indexing(ws_id, "incremental", false);
    REQUIRE(res3.has_value());
    REQUIRE(res3->files_processed == 1);
    REQUIRE(res3->files_skipped == 2);

    // Verify symbols of modified file were replaced
    auto mod_file = env.db->files().get_by_path(ws_id, "src/to_modify.c");
    REQUIRE(mod_file.has_value());
    auto mod_syms = env.db->symbols().list_by_file(mod_file->id);
    REQUIRE(mod_syms.size() == 2);
    bool has_m2 = false;
    bool has_m3 = false;
    for (const auto& s : mod_syms) {
        if (s.name == "m2")
            has_m2 = true;
        if (s.name == "m3")
            has_m3 = true;
    }
    REQUIRE(has_m2);
    REQUIRE(has_m3);

    // 4. Add a new file
    env.write_file("src/brand_new.c", "int brand_new() { return 42; }\n");
    auto res4 = pipeline.run_indexing(ws_id, "incremental", false);
    REQUIRE(res4.has_value());
    REQUIRE(res4->files_processed == 1);
    REQUIRE(res4->files_skipped == 3);

    // 5. Delete a file
    env.remove_file("src/to_delete.c");
    auto del_file_before = env.db->files().get_by_path(ws_id, "src/to_delete.c");
    REQUIRE(del_file_before.has_value());
    int64_t del_file_id = del_file_before->id;

    auto res5 = pipeline.run_indexing(ws_id, "incremental", false);
    REQUIRE(res5.has_value());

    // File should be marked deleted in DB and derived records cleaned up
    auto del_file_after = env.db->files().get_by_id(del_file_id);
    REQUIRE(del_file_after.has_value());
    REQUIRE(del_file_after->is_deleted);

    auto del_syms = env.db->symbols().list_by_file(del_file_id);
    REQUIRE(del_syms.empty());

    // 6. Rename a file (delete old, create new)
    env.remove_file("src/brand_new.c");
    env.write_file("src/renamed.c", "int brand_new() { return 42; }\n");

    auto res6 = pipeline.run_indexing(ws_id, "incremental", false);
    REQUIRE(res6.has_value());
    REQUIRE(res6->files_processed == 1);

    auto renamed_file = env.db->files().get_by_path(ws_id, "src/renamed.c");
    REQUIRE(renamed_file.has_value());
    REQUIRE_FALSE(renamed_file->is_deleted);
}

TEST_CASE("Pipeline cancellation and safe shutdown (D-11)", "[index][pipeline][cancellation]") {
    PipelineTestWorkspace env;
    for (int i = 0; i < 20; ++i) {
        env.write_file("src/file_" + std::to_string(i) + ".c", "int func_" + std::to_string(i) +
                                                                   "() { return " +
                                                                   std::to_string(i) + "; }\n");
    }

    int64_t ws_id = env.create_workspace();
    IndexingPipeline pipeline(*env.db);

    std::stop_source stop_source;

    // Start indexing in a background thread and cancel almost immediately
    std::future<Result<IndexResult>> future = std::async(std::launch::async, [&] {
        return pipeline.run_indexing(ws_id, "full", true, stop_source.get_token());
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    stop_source.request_stop();

    auto res = future.get();
    REQUIRE(res.has_value());

    // If cancellation was observed, status should be cancelled
    if (res->status == "cancelled") {
        auto ws = env.db->workspaces().get_by_id(ws_id);
        REQUIRE(ws.has_value());
        REQUIRE(ws->status == WorkspaceStatus::idle);

        auto job = env.db->jobs().get_by_id(res->job_id);
        REQUIRE(job.has_value());
        REQUIRE(job->status == "cancelled");
        REQUIRE(job->workspace_revision == std::nullopt);
    }
}

TEST_CASE("Diagnostics for malformed and invalid files (D-12)", "[index][pipeline][diagnostics]") {
    PipelineTestWorkspace env;
    // Malformed C file with syntax error
    env.write_file("src/malformed.c", "int broken( {\n");
    env.write_file("src/valid.c", "int valid() { return 1; }\n");

    int64_t ws_id = env.create_workspace();
    IndexingPipeline pipeline(*env.db);

    auto res = pipeline.run_indexing(ws_id, "full", true);
    REQUIRE(res.has_value());
    REQUIRE(res->status == "completed");
    REQUIRE(res->files_processed == 2);

    // Verify diagnostics recorded in database
    auto diags = env.db->diagnostics().list_by_workspace(ws_id);
    REQUIRE_FALSE(diags.empty());

    bool has_parser_diag = false;
    for (const auto& d : diags) {
        if (d.source == "parser") {
            has_parser_diag = true;
            REQUIRE(d.severity == "error");
        }
    }
    REQUIRE(has_parser_diag);

    // Valid file still indexed despite syntax error in malformed file
    auto valid_file = env.db->files().get_by_path(ws_id, "src/valid.c");
    REQUIRE(valid_file.has_value());
    auto valid_syms = env.db->symbols().list_by_file(valid_file->id);
    REQUIRE(valid_syms.size() == 1);
    REQUIRE(valid_syms[0].name == "valid");
}

TEST_CASE("Concurrency tests for one writer and multiple readers (D-14)",
          "[index][pipeline][concurrency]") {
    PipelineTestWorkspace env;
    for (int i = 0; i < 30; ++i) {
        env.write_file("src/module_" + std::to_string(i) + ".c", "int calc_" + std::to_string(i) +
                                                                     "() { return " +
                                                                     std::to_string(i) + "; }\n");
    }

    int64_t ws_id = env.create_workspace();
    IndexingPipeline pipeline(*env.db);

    std::atomic<bool> writer_done{false};
    std::atomic<int> reader_success_count{0};
    std::atomic<int> reader_error_count{0};

    // Spawn 4 reader threads
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&, r] {
            // Open a separate SQLite connection in each reader thread (best practice for WAL mode)
            auto reader_db = Database::open(env.db_file.string(), false);
            while (!writer_done.load(std::memory_order_relaxed)) {
                try {
                    auto ws = reader_db->workspaces().get_by_id(ws_id);
                    auto files = reader_db->files().list_by_workspace(ws_id);
                    auto jobs = reader_db->jobs().list_by_workspace(ws_id);
                    if (ws.has_value()) {
                        reader_success_count++;
                    }
                } catch (const std::exception& ex) {
                    reader_error_count++;
                }
                std::this_thread::yield();
            }
        });
    }

    // Run writer
    auto res = pipeline.run_indexing(ws_id, "full", true);
    REQUIRE(res.has_value());
    REQUIRE(res->status == "completed");
    REQUIRE(res->files_processed == 30);

    writer_done.store(true);
    for (auto& t : readers) {
        t.join();
    }

    REQUIRE(reader_error_count == 0);
    REQUIRE(reader_success_count > 0);

    // Final database consistency check
    auto final_files = env.db->files().list_by_workspace(ws_id);
    REQUIRE(final_files.size() == 30);
    auto final_symbols = env.db->symbols().find_by_name(ws_id, "calc_0");
    REQUIRE_FALSE(final_symbols.empty());
}

TEST_CASE("Serialized indexing jobs sharing same database connection (Finding 1)",
          "[index][pipeline][concurrency]") {
    PipelineTestWorkspace env;
    for (int i = 0; i < 10; ++i) {
        env.write_file("ws1/file_" + std::to_string(i) + ".c", "int func_ws1_" + std::to_string(i) +
                                                                   "() { return " +
                                                                   std::to_string(i) + "; }\n");
        env.write_file("ws2/file_" + std::to_string(i) + ".c", "int func_ws2_" + std::to_string(i) +
                                                                   "() { return " +
                                                                   std::to_string(i) + "; }\n");
    }

    Workspace ws1{.root_path = (env.root / "ws1").string(), .name = "WS 1"};
    int64_t ws1_id = env.db->workspaces().create(ws1);
    Workspace ws2{.root_path = (env.root / "ws2").string(), .name = "WS 2"};
    int64_t ws2_id = env.db->workspaces().create(ws2);

    IndexingPipeline pipeline(*env.db);

    // Run both indexing jobs concurrently on the same Database connection
    auto fut1 =
        std::async(std::launch::async, [&] { return pipeline.run_indexing(ws1_id, "full", true); });
    auto fut2 =
        std::async(std::launch::async, [&] { return pipeline.run_indexing(ws2_id, "full", true); });

    auto res1 = fut1.get();
    auto res2 = fut2.get();

    REQUIRE(res1.has_value());
    REQUIRE(res1->status == "completed");
    REQUIRE(res1->files_processed == 10);

    REQUIRE(res2.has_value());
    REQUIRE(res2->status == "completed");
    REQUIRE(res2->files_processed == 10);

    auto ws1_rec = env.db->workspaces().get_by_id(ws1_id);
    REQUIRE(ws1_rec->status == WorkspaceStatus::ready);
    auto ws2_rec = env.db->workspaces().get_by_id(ws2_id);
    REQUIRE(ws2_rec->status == WorkspaceStatus::ready);
}

TEST_CASE("Capture failure does not replace index or insert empty file record (Finding 2)",
          "[index][pipeline][capture]") {
    PipelineTestWorkspace env;
    env.write_file("src/valid.c", "int existing() { return 10; }\n");

    int64_t ws_id = env.create_workspace();
    IndexingPipeline pipeline(*env.db);

    // Initial indexing
    auto res1 = pipeline.run_indexing(ws_id, "full", true);
    REQUIRE(res1.has_value());
    REQUIRE(res1->files_processed == 1);

    auto initial_file = env.db->files().get_by_path(ws_id, "src/valid.c");
    REQUIRE(initial_file.has_value());
    auto initial_syms = env.db->symbols().list_by_file(initial_file->id);
    REQUIRE(initial_syms.size() == 1);
    REQUIRE(initial_syms[0].name == "existing");

    // Make the file unreadable (permission 000)
    auto file_path = env.root / "src/valid.c";
    std::error_code ec;
    fs::permissions(file_path, fs::perms::none, fs::perm_options::replace, ec);

    // Force reindex - file capture will fail
    auto res2 = pipeline.run_indexing(ws_id, "full", true);
    REQUIRE(res2.has_value());
    REQUIRE(res2->error_count > 0);

    // Restore permissions for cleanup
    fs::permissions(file_path, fs::perms::all, fs::perm_options::replace, ec);

    // Existing file was NOT purged by replace_file_index!
    auto existing_file = env.db->files().get_by_path(ws_id, "src/valid.c");
    REQUIRE(existing_file.has_value());
    auto existing_syms = env.db->symbols().list_by_file(existing_file->id);
    REQUIRE(existing_syms.size() == 1);
    REQUIRE(existing_syms[0].name == "existing");

    // Diagnostic was recorded
    auto diags = env.db->diagnostics().list_by_workspace(ws_id);
    bool has_read_error = false;
    for (const auto& d : diags) {
        if (d.code == "read_error") {
            has_read_error = true;
        }
    }
    REQUIRE(has_read_error);
}

TEST_CASE("Reject zero queue capacity and zero queue max bytes (Findings 5, 8)",
          "[index][pipeline][options]") {
    PipelineTestWorkspace env;
    env.write_file("src/main.c", "int main() { return 0; }\n");
    int64_t ws_id = env.create_workspace();

    SECTION("queue_capacity == 0 returns invalid_argument") {
        IndexerOptions opts;
        opts.queue_capacity = 0;
        IndexingPipeline pipeline(*env.db, opts);
        auto res = pipeline.run_indexing(ws_id, "full", true);
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().code == ErrorCode::invalid_argument);
    }

    SECTION("queue_max_bytes == 0 returns invalid_argument") {
        IndexerOptions opts;
        opts.queue_max_bytes = 0;
        IndexingPipeline pipeline(*env.db, opts);
        auto res = pipeline.run_indexing(ws_id, "full", true);
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().code == ErrorCode::invalid_argument);
    }
}

TEST_CASE("BOM offset is included in persisted byte ranges (Finding 6)", "[index][pipeline][bom]") {
    PipelineTestWorkspace env;
    // UTF-8 BOM (\xef\xbb\xbf) followed by C code
    std::string bom_content = "\xef\xbb\xbf"
                              "int hello() { return 1; }\n";
    env.write_file("src/bom.c", bom_content);

    int64_t ws_id = env.create_workspace();
    IndexingPipeline pipeline(*env.db);

    auto res = pipeline.run_indexing(ws_id, "full", true);
    REQUIRE(res.has_value());
    REQUIRE(res->files_processed == 1);

    auto file = env.db->files().get_by_path(ws_id, "src/bom.c");
    REQUIRE(file.has_value());

    auto syms = env.db->symbols().list_by_file(file->id);
    REQUIRE(syms.size() == 1);
    REQUIRE(syms[0].name == "hello");
    // Index same code without BOM for comparison
    env.write_file("src/nobom.c", "int hello() { return 1; }\n");

    auto res2 = pipeline.run_indexing(ws_id, "incremental", false);
    REQUIRE(res2.has_value());

    auto no_bom_file = env.db->files().get_by_path(ws_id, "src/nobom.c");
    REQUIRE(no_bom_file.has_value());
    auto no_bom_syms = env.db->symbols().list_by_file(no_bom_file->id);
    REQUIRE(no_bom_syms.size() == 1);

    // Verify non-BOM is at byte 4 ("int " is 4 bytes), and BOM is at byte 4 + 3 = 7
    REQUIRE(no_bom_syms[0].range.start_byte == 4);
    REQUIRE(syms[0].range.start_byte == 7);
    REQUIRE(syms[0].range.start_byte == no_bom_syms[0].range.start_byte + 3);
}

TEST_CASE("H1-06: Indexing pipeline integrates compile_commands.json context",
          "[index][pipeline][compile_commands]") {
    PipelineTestWorkspace env;

    env.write_file("include/mylib.h", "int helper(void);\n");
    env.write_file("src/main.c", R"C(
#include "mylib.h"

int main() {
    int v = BUILD_VERSION;
    return v;
}
)C");

    // Create compile_commands.json in workspace root
    std::string cdb_json = R"json([
  {
    "directory": ".",
    "file": "src/main.c",
    "arguments": [
      "gcc",
      "-Iinclude",
      "-DBUILD_VERSION=42",
      "-std=c11",
      "-c",
      "src/main.c"
    ]
  }
])json";
    env.write_file("compile_commands.json", cdb_json);

    int64_t ws_id = env.create_workspace("CDB Workspace");
    REQUIRE(ws_id > 0);

    IndexingPipeline pipeline(*env.db);
    auto res = pipeline.run_indexing(ws_id, "full", true);
    REQUIRE(res.has_value());
    REQUIRE(res->status == "completed");
    REQUIRE(res->files_processed == 3);

    // 1. Verify BUILD_VERSION macro symbol was extracted and persisted
    auto syms = env.db->symbols().list_by_workspace(ws_id);
    bool found_macro = false;
    int64_t macro_sym_id = 0;
    for (const auto& s : syms) {
        if (s.name == "BUILD_VERSION") {
            found_macro = true;
            macro_sym_id = s.id;
            CHECK(s.kind == "macro");
            CHECK(s.is_definition);
        }
    }
    REQUIRE(found_macro);
    REQUIRE(macro_sym_id > 0);

    // 2. Verify file dependency on mylib.h was resolved
    auto deps = env.db->dependencies().list_by_workspace(ws_id);
    bool found_resolved_dep = false;
    for (const auto& dep : deps) {
        if (dep.raw_name == "mylib.h") {
            found_resolved_dep = true;
            CHECK(dep.resolution == "resolved");
            CHECK(dep.target_file_id.has_value());
        }
    }
    REQUIRE(found_resolved_dep);

    // 3. Verify reference to BUILD_VERSION links to the macro symbol
    auto refs = env.db->references().list_by_workspace(ws_id);
    bool found_macro_ref = false;
    for (const auto& r : refs) {
        if (r.name == "BUILD_VERSION") {
            found_macro_ref = true;
            CHECK(r.target_symbol_id == macro_sym_id);
        }
    }
    REQUIRE(found_macro_ref);
}
