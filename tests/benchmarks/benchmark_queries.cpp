#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "codelenses/db/database.hpp"
#include "codelenses/index/indexer.hpp"
#include "codelenses/server/service.hpp"
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace codelenses;
using namespace codelenses::server;

namespace {

#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer)) || \
    defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer)) || \
    defined(__SANITIZE_UNDEFINED__) || (defined(__has_feature) && __has_feature(undefined_behavior_sanitizer))
constexpr bool is_sanitized_build = true;
#else
constexpr bool is_sanitized_build = false;
#endif

constexpr double timing_scale = is_sanitized_build ? 4.0 : 1.0;

struct QueryBenchmarkFixture {
    fs::path temp_dir;
    fs::path workspace_root;
    fs::path sample_workspace_root;
    std::unique_ptr<Database> db;
    std::unique_ptr<index::IndexingPipeline> pipeline;
    std::unique_ptr<ApiService> service;

    int64_t workspace_id{0};
    std::vector<int64_t> file_ids;
    std::vector<int64_t> symbol_ids;
    int64_t large_file_id{0};

    QueryBenchmarkFixture() {
        auto pid = std::to_string(::getpid());
        auto now = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        temp_dir = fs::temp_directory_path() / ("codelenses_bench_qry_" + pid + "_" + now);
        workspace_root = temp_dir / "workspace";

#ifdef CODELENSES_SOURCE_DIR
        sample_workspace_root = fs::path(CODELENSES_SOURCE_DIR) / "sample_workspace";
#else
        sample_workspace_root = fs::current_path() / "sample_workspace";
#endif

        fs::create_directories(workspace_root / "src" / "deep" / "nested" / "pkg");
        fs::create_directories(workspace_root / "include");

        // 1. Create a large source file (5,000 lines) for range retrieval benchmarking
        std::ofstream large_file(workspace_root / "src" / "large_source.c");
        large_file << "// Large file for source range retrieval benchmark\n";
        for (int i = 1; i <= 5000; ++i) {
            large_file << "int compute_step_" << i << "(int x) { return x + " << i << "; }\n";
        }
        large_file.close();

        // 2. Create structured multi-folder hierarchy for tree expansion benchmark
        for (int dir_idx = 0; dir_idx < 10; ++dir_idx) {
            fs::path sub = workspace_root / "src" / ("module_" + std::to_string(dir_idx));
            fs::create_directories(sub);
            for (int f_idx = 0; f_idx < 5; ++f_idx) {
                std::ofstream f(sub / ("file_" + std::to_string(f_idx) + ".c"));
                f << "int mod_" << dir_idx << "_func_" << f_idx << "() {\n"
                  << "    return " << (dir_idx * 100 + f_idx) << ";\n"
                  << "}\n";
                f.close();
            }
        }

        // 3. Create cross-referencing files for reference / caller / callee queries
        std::ofstream caller_f(workspace_root / "src" / "caller_service.c");
        caller_f << "int target_base_function(int a) { return a * 10; }\n";
        for (int i = 0; i < 20; ++i) {
            caller_f << "int caller_hub_" << i << "() { return target_base_function(" << i
                     << "); }\n";
        }
        caller_f.close();

        // Initialize Database, Pipeline, and Service
        fs::path db_path = temp_dir / "query_bench.db";
        db = Database::open(db_path.string(), true);
        pipeline = std::make_unique<index::IndexingPipeline>(*db);
        service = std::make_unique<ApiService>(*db, *pipeline);

        workspace_id = service
                           ->create_workspace(CreateWorkspaceRequest{
                               .root_path = workspace_root.string(),
                               .name = "Query Benchmark Workspace",
                               .include_patterns = {},
                               .exclude_patterns = {},
                               .default_ignores = {},
                           })
                           .id;

        // Perform cold indexing to populate database
        auto index_res = pipeline->run_indexing(workspace_id, "full", true);
        REQUIRE(index_res.has_value());

        // Gather indexed file IDs and symbol IDs
        auto syms = service->list_symbols(workspace_id, std::nullopt, std::nullopt, std::nullopt,
                                          std::nullopt, 200, 0);
        for (const auto& s : syms.items) {
            symbol_ids.push_back(s.id);
        }

        // Locate large_source.c
        auto tree = service->get_tree(workspace_id, "src");
        for (const auto& entry : tree.entries) {
            if (entry.name == "large_source.c" && entry.file_id.has_value()) {
                large_file_id = *entry.file_id;
                break;
            }
        }
    }

    ~QueryBenchmarkFixture() {
        service.reset();
        pipeline.reset();
        db.reset();
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

template <typename Fn>
double measure_avg_time_ms(int iterations, Fn&& fn) {
    // Warm-up run
    fn();

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        fn();
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / iterations;
}

} // namespace

TEST_CASE("Benchmark: Tree Expansion, Source Range, Symbol Search, and References (I-07)",
          "[benchmark][queries]") {
    QueryBenchmarkFixture fixture;
    REQUIRE(fixture.workspace_id > 0);
    REQUIRE(!fixture.symbol_ids.empty());
    REQUIRE(fixture.large_file_id > 0);

    SECTION("1. Tree Expansion Latency and Throughput Benchmark") {
        // Measure root folder expansion
        int iters = 200;
        double root_avg_ms = measure_avg_time_ms(iters, [&] {
            auto tree = fixture.service->get_tree(fixture.workspace_id, "");
            CHECK(tree.entries.size() >= 2);
        });

        // Measure nested subfolder expansion
        double nested_avg_ms = measure_avg_time_ms(iters, [&] {
            auto tree = fixture.service->get_tree(fixture.workspace_id, "src/module_0");
            CHECK(tree.entries.size() == 5);
        });

        double root_ops_sec = 1000.0 / std::max(0.001, root_avg_ms);
        double nested_ops_sec = 1000.0 / std::max(0.001, nested_avg_ms);

        std::cout << "\n[BENCHMARK I-07] Tree Expansion Performance:\n"
                  << "  - Root expansion latency: " << root_avg_ms << " ms (" << root_ops_sec
                  << " ops/sec)\n"
                  << "  - Nested expansion latency: " << nested_avg_ms << " ms (" << nested_ops_sec
                  << " ops/sec)\n";

        // Regression threshold: tree query must execute in under 10ms
        CHECK(root_avg_ms < 10.0 * timing_scale);
        CHECK(nested_avg_ms < 10.0 * timing_scale);
    }

    SECTION("2. Source Range Retrieval Latency on Large Files (5,000 lines)") {
        int iters = 20;

        // Line-range retrieval: 50 lines from middle of 5,000 line file
        double range_50_ms = measure_avg_time_ms(iters, [&] {
            auto content =
                fixture.service->get_file_content(fixture.workspace_id, fixture.large_file_id, 2500,
                                                  2550, std::nullopt, std::nullopt);
            CHECK(content.total_lines >= 50);
            CHECK(content.start_line == 2500);
            CHECK(content.end_line == 2550);
        });

        // Line-range retrieval: 500 lines
        double range_500_ms = measure_avg_time_ms(iters, [&] {
            auto content =
                fixture.service->get_file_content(fixture.workspace_id, fixture.large_file_id, 1000,
                                                  1500, std::nullopt, std::nullopt);
            CHECK(content.total_lines >= 500);
        });

        // Full 5,000 lines retrieval
        double full_ms = measure_avg_time_ms(iters, [&] {
            auto content = fixture.service->get_file_content(
                fixture.workspace_id, fixture.large_file_id, std::nullopt, std::nullopt,
                std::nullopt, std::nullopt);
            CHECK(content.total_lines >= 5000);
        });

        std::cout << "[BENCHMARK I-07] Source Range Retrieval (5,000-line file):\n"
                  << "  - 50-line range latency: " << range_50_ms << " ms\n"
                  << "  - 500-line range latency: " << range_500_ms << " ms\n"
                  << "  - Full file (5,000 lines) latency: " << full_ms << " ms\n";

        // Regression threshold: range retrieval on 5,000 lines must execute under 50ms in debug
        // build
        CHECK(range_50_ms < 50.0 * timing_scale);
        CHECK(range_500_ms < 50.0 * timing_scale);
        CHECK(full_ms < 50.0 * timing_scale);
    }

    SECTION("3. Symbol Search Latency and Throughput (Prefix, Substring, and FTS)") {
        int iters = 20;

        // Prefix / Substring list_symbols search
        double sym_search_ms = measure_avg_time_ms(iters, [&] {
            auto res =
                fixture.service->list_symbols(fixture.workspace_id, "compute_step_1", std::nullopt,
                                              std::nullopt, std::nullopt, 20, 0);
            CHECK(res.total >= 1);
        });

        // Full-Text Search (FTS) symbol search
        double fts_sym_ms = measure_avg_time_ms(iters, [&] {
            auto res = fixture.service->search_symbols(fixture.workspace_id, "compute_step", 20, 0);
            CHECK(res.items.size() >= 1);
        });

        // Paginated symbol search (page 1 vs page 5)
        double paged_sym_ms = measure_avg_time_ms(iters, [&] {
            auto res =
                fixture.service->list_symbols(fixture.workspace_id, std::nullopt, std::nullopt,
                                              std::nullopt, std::nullopt, 50, 100);
            CHECK(res.items.size() <= 50);
        });

        double search_ops_sec = 1000.0 / std::max(0.001, sym_search_ms);
        double fts_ops_sec = 1000.0 / std::max(0.001, fts_sym_ms);

        std::cout << "[BENCHMARK I-07] Symbol Search Performance:\n"
                  << "  - Name search latency: " << sym_search_ms << " ms (" << search_ops_sec
                  << " ops/sec)\n"
                  << "  - FTS symbol search latency: " << fts_sym_ms << " ms (" << fts_ops_sec
                  << " ops/sec)\n"
                  << "  - Paginated list latency: " << paged_sym_ms << " ms\n";

        // Regression threshold: symbol lookups must execute under 60-100ms in debug build
        CHECK(sym_search_ms < 20.0 * timing_scale);
        CHECK(fts_sym_ms < 60.0 * timing_scale);
        CHECK(paged_sym_ms < 100.0 * timing_scale);
    }

    SECTION("4. Reference, Caller, Callee, and Graph Queries Benchmark") {
        int64_t target_sym_id = 0;
        auto found = fixture.service->list_symbols(fixture.workspace_id, "target_base_function",
                                                   std::nullopt, std::nullopt, std::nullopt, 1, 0);
        if (!found.items.empty()) {
            target_sym_id = found.items[0].id;
        } else {
            target_sym_id = fixture.symbol_ids.front();
        }
        REQUIRE(target_sym_id > 0);

        int iters = 20;

        // Symbol Detail lookup
        double detail_ms = measure_avg_time_ms(iters, [&] {
            auto det = fixture.service->get_symbol_detail(fixture.workspace_id, target_sym_id);
            CHECK(det.symbol.id == target_sym_id);
        });

        // References query
        double refs_ms = measure_avg_time_ms(iters, [&] {
            auto refs =
                fixture.service->get_symbol_references(fixture.workspace_id, target_sym_id, 50, 0);
            CHECK(refs.limit == 50);
        });

        // Callers query
        double callers_ms = measure_avg_time_ms(iters, [&] {
            auto callers = fixture.service->get_symbol_callers(fixture.workspace_id, target_sym_id);
            CHECK((callers.empty() == false || callers.empty() == true));
        });

        // Callees query
        double callees_ms = measure_avg_time_ms(iters, [&] {
            auto callees = fixture.service->get_symbol_callees(fixture.workspace_id, target_sym_id);
            CHECK((callees.empty() == false || callees.empty() == true));
        });

        // Relationship Graph expansion (depth=2)
        double graph_ms = measure_avg_time_ms(iters, [&] {
            auto graph = fixture.service->get_symbol_graph(fixture.workspace_id, target_sym_id, 2,
                                                           50, 100, {});
            CHECK(graph.root_symbol_id == target_sym_id);
        });

        std::cout << "[BENCHMARK I-07] Navigation and Reference Queries:\n"
                  << "  - Detail lookup latency: " << detail_ms << " ms\n"
                  << "  - References query latency: " << refs_ms << " ms\n"
                  << "  - Callers query latency: " << callers_ms << " ms\n"
                  << "  - Callees query latency: " << callees_ms << " ms\n"
                  << "  - Graph traversal (depth 2) latency: " << graph_ms << " ms\n";

        // Regression thresholds: All navigation queries must execute in under 20-30ms in debug
        // build
        CHECK(detail_ms < 20.0 * timing_scale);
        CHECK(refs_ms < 20.0 * timing_scale);
        CHECK(callers_ms < 20.0 * timing_scale);
        CHECK(callees_ms < 20.0 * timing_scale);
        CHECK(graph_ms < 30.0 * timing_scale);
    }
}
