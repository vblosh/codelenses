#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "codelenses/db/database.hpp"
#include "codelenses/index/indexer.hpp"
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace codelenses;
using namespace codelenses::index;

namespace {

struct IndexingBenchmarkFixture {
    fs::path temp_dir;
    fs::path workspace_root;
    fs::path sample_workspace_root;
    int num_synthetic_files{50};

    IndexingBenchmarkFixture() {
        auto pid = std::to_string(::getpid());
        auto now = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        temp_dir = fs::temp_directory_path() / ("codelenses_bench_idx_" + pid + "_" + now);
        workspace_root = temp_dir / "workspace";
        fs::create_directories(workspace_root / "src");
        fs::create_directories(workspace_root / "include");

#ifdef CODELENSES_SOURCE_DIR
        sample_workspace_root = fs::path(CODELENSES_SOURCE_DIR) / "sample_workspace";
#else
        sample_workspace_root = fs::current_path() / "sample_workspace";
#endif

        // Generate synthetic polyglot files for repeatable scaling tests
        for (int i = 0; i < num_synthetic_files; ++i) {
            std::ofstream f(workspace_root / "src" / ("service_" + std::to_string(i) + ".c"));
            f << "#include <stdio.h>\n"
              << "int handler_" << i << "(int code) {\n"
              << "    int result = code * 2 + " << i << ";\n"
              << "    return result;\n"
              << "}\n"
              << "int entrypoint_" << i << "() {\n"
              << "    return handler_" << i << "(" << i << ");\n"
              << "}\n";
            f.close();
        }
    }

    ~IndexingBenchmarkFixture() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    std::unique_ptr<Database> create_temp_db(const std::string& name) {
        fs::path db_file = temp_dir / (name + ".db");
        fs::remove(db_file);
        return Database::open(db_file.string(), true);
    }
};

} // namespace

TEST_CASE("Benchmark: Cold Indexing vs Incremental Indexing Separate Performance (I-06)",
          "[benchmark][indexing]") {
    IndexingBenchmarkFixture fixture;

    SECTION("1. Cold Indexing Performance and Throughput Thresholds") {
        auto db = fixture.create_temp_db("bench_cold");
        int64_t ws_id = db->workspaces().create(Workspace{
            .root_path = fixture.workspace_root.string(),
            .name = "Cold Benchmark Workspace",
        });
        REQUIRE(ws_id > 0);

        IndexingPipeline pipeline(*db);

        auto t0 = std::chrono::steady_clock::now();
        auto result = pipeline.run_indexing(ws_id, "full", true);
        auto t1 = std::chrono::steady_clock::now();

        REQUIRE(result.has_value());
        CHECK(result->status == "completed");
        CHECK(result->files_processed == fixture.num_synthetic_files);
        CHECK(result->files_skipped == 0);

        double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double files_per_sec = (fixture.num_synthetic_files / (elapsed_ms / 1000.0));

        std::cout << "\n[BENCHMARK I-06] Cold Indexing (" << fixture.num_synthetic_files
                  << " files):\n"
                  << "  - Duration: " << elapsed_ms << " ms\n"
                  << "  - Throughput: " << files_per_sec << " files/sec\n"
                  << "  - Total symbols: " << db->symbols().list_by_workspace(ws_id).size() << "\n";

        // Regression threshold: Cold indexing must process at least 15 files/second in debug build
        CHECK(elapsed_ms < 5000.0); // Must finish within 5 seconds
        CHECK(files_per_sec > 15.0);
    }

    SECTION("2. Incremental Indexing No-Op Thresholds and Speedup Ratio") {
        auto db = fixture.create_temp_db("bench_inc_noop");
        int64_t ws_id = db->workspaces().create(Workspace{
            .root_path = fixture.workspace_root.string(),
            .name = "Incremental No-Op Workspace",
        });
        REQUIRE(ws_id > 0);

        IndexingPipeline pipeline(*db);

        // Run cold index first
        auto cold_t0 = std::chrono::steady_clock::now();
        auto cold_res = pipeline.run_indexing(ws_id, "full", true);
        auto cold_t1 = std::chrono::steady_clock::now();
        REQUIRE(cold_res.has_value());
        double cold_ms = std::chrono::duration<double, std::milli>(cold_t1 - cold_t0).count();

        // Run incremental index immediately with NO changes
        auto inc_t0 = std::chrono::steady_clock::now();
        auto inc_res = pipeline.run_indexing(ws_id, "incremental", false);
        auto inc_t1 = std::chrono::steady_clock::now();
        REQUIRE(inc_res.has_value());
        double inc_ms = std::chrono::duration<double, std::milli>(inc_t1 - inc_t0).count();

        CHECK(inc_res->status == "completed");
        // Core invariant: 0 files re-parsed, 100% skipped
        CHECK(inc_res->files_processed == 0);
        CHECK(inc_res->files_skipped == fixture.num_synthetic_files);

        double speedup = (inc_ms > 0.0) ? (cold_ms / inc_ms) : 100.0;
        std::cout << "[BENCHMARK I-06] Incremental No-Op Indexing:\n"
                  << "  - Cold Duration: " << cold_ms << " ms\n"
                  << "  - Inc Duration: " << inc_ms << " ms\n"
                  << "  - Speedup Ratio: " << speedup << "x\n"
                  << "  - Files Skipped: " << inc_res->files_skipped << " / "
                  << fixture.num_synthetic_files << "\n";

        // Regression threshold: Incremental no-op should take < 300 ms and be substantially faster
        // than cold
        CHECK(inc_ms < 300.0);
        CHECK(speedup >= 1.5);
    }

    SECTION("3. Incremental Indexing with Single File Modified") {
        auto db = fixture.create_temp_db("bench_inc_mod");
        int64_t ws_id = db->workspaces().create(Workspace{
            .root_path = fixture.workspace_root.string(),
            .name = "Incremental Mod Workspace",
        });
        REQUIRE(ws_id > 0);

        IndexingPipeline pipeline(*db);
        REQUIRE(pipeline.run_indexing(ws_id, "full", true).has_value());

        // Modify exactly 1 file
        fs::path target_file = fixture.workspace_root / "src" / "service_0.c";
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Ensure mtime increases
        std::ofstream append_f(target_file, std::ios::app);
        append_f << "\nint added_helper() { return 999; }\n";
        append_f.close();

        auto t0 = std::chrono::steady_clock::now();
        auto inc_res = pipeline.run_indexing(ws_id, "incremental", false);
        auto t1 = std::chrono::steady_clock::now();
        REQUIRE(inc_res.has_value());

        double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "[BENCHMARK I-06] Incremental Single File Modified:\n"
                  << "  - Duration: " << elapsed_ms << " ms\n"
                  << "  - Files Processed: " << inc_res->files_processed << " (expected 1)\n"
                  << "  - Files Skipped: " << inc_res->files_skipped << "\n";

        // Exactly 1 file should be re-processed, 49 skipped
        CHECK(inc_res->files_processed == 1);
        CHECK(inc_res->files_skipped == fixture.num_synthetic_files - 1);
        CHECK(elapsed_ms < 350.0);
    }

    SECTION("4. Incremental Indexing with File Addition and Deletion") {
        auto db = fixture.create_temp_db("bench_inc_add_del");
        int64_t ws_id = db->workspaces().create(Workspace{
            .root_path = fixture.workspace_root.string(),
            .name = "Incremental Add/Del Workspace",
        });
        REQUIRE(ws_id > 0);

        IndexingPipeline pipeline(*db);
        REQUIRE(pipeline.run_indexing(ws_id, "full", true).has_value());

        // Add 1 file
        fs::path new_file = fixture.workspace_root / "src" / "brand_new.c";
        std::ofstream nf(new_file);
        nf << "int brand_new_function() { return 777; }\n";
        nf.close();

        auto add_res = pipeline.run_indexing(ws_id, "incremental", false);
        REQUIRE(add_res.has_value());
        CHECK(add_res->files_processed == 1);
        CHECK(add_res->files_skipped == fixture.num_synthetic_files);

        // Delete 1 file
        fs::remove(new_file);
        auto del_res = pipeline.run_indexing(ws_id, "incremental", false);
        REQUIRE(del_res.has_value());
        CHECK(del_res->files_processed == 0); // No parse needed, file deleted
        CHECK(del_res->files_skipped == fixture.num_synthetic_files);
    }

    SECTION("5. Cold vs Incremental Indexing on Real Sample Polyglot Workspace") {
        if (fs::exists(fixture.sample_workspace_root)) {
            auto db = fixture.create_temp_db("bench_sample_ws");
            int64_t ws_id = db->workspaces().create(Workspace{
                .root_path = fixture.sample_workspace_root.string(),
                .name = "Polyglot Sample Workspace Benchmark",
            });
            REQUIRE(ws_id > 0);

            IndexingPipeline pipeline(*db);

            // Cold pass
            auto cold_t0 = std::chrono::steady_clock::now();
            auto cold_res = pipeline.run_indexing(ws_id, "full", true);
            auto cold_t1 = std::chrono::steady_clock::now();
            REQUIRE(cold_res.has_value());
            double cold_ms = std::chrono::duration<double, std::milli>(cold_t1 - cold_t0).count();

            // Incremental no-op pass
            auto inc_t0 = std::chrono::steady_clock::now();
            auto inc_res = pipeline.run_indexing(ws_id, "incremental", false);
            auto inc_t1 = std::chrono::steady_clock::now();
            REQUIRE(inc_res.has_value());
            double inc_ms = std::chrono::duration<double, std::milli>(inc_t1 - inc_t0).count();

            std::cout << "[BENCHMARK I-06] Sample Workspace (" << cold_res->files_total
                      << " polyglot files):\n"
                      << "  - Cold Duration: " << cold_ms << " ms\n"
                      << "  - Incremental Duration: " << inc_ms << " ms\n"
                      << "  - Speedup: " << (cold_ms / std::max(0.1, inc_ms)) << "x\n";

            CHECK(cold_res->status == "completed");
            CHECK(inc_res->status == "completed");
            CHECK(inc_res->files_processed == 0);
            CHECK(inc_res->files_skipped == cold_res->files_total);
            CHECK(inc_ms < cold_ms);
        }
    }
}
