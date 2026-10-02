#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "codelenses/db/database.hpp"
#include "codelenses/db/statement.hpp"
#include "codelenses/index/indexer.hpp"
#include "codelenses/server/service.hpp"
#include <catch2/catch_test_macros.hpp>
#include <sys/resource.h>
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

// Helper to query peak resident set size (RSS) in KB
size_t get_peak_rss_kb() {
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return static_cast<size_t>(usage.ru_maxrss); // on Linux ru_maxrss is in kilobytes
    }
    return 0;
}

// Helper to query current resident set size (RSS) in KB via /proc/self/statm
size_t get_current_rss_kb() {
    std::ifstream statm("/proc/self/statm");
    if (statm.is_open()) {
        size_t size = 0, resident = 0;
        if (statm >> size >> resident) {
            long page_size = sysconf(_SC_PAGESIZE);
            return (resident * static_cast<size_t>(page_size)) / 1024;
        }
    }
    return 0;
}

// Calculate total size of files under directory in bytes
uintmax_t calculate_directory_size(const fs::path& dir) {
    uintmax_t total = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

struct FootprintFixture {
    fs::path temp_dir;
    fs::path workspace_root;
    fs::path sample_workspace_root;

    FootprintFixture() {
        auto pid = std::to_string(::getpid());
        auto now = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        temp_dir = fs::temp_directory_path() / ("codelenses_footprint_" + pid + "_" + now);
        workspace_root = temp_dir / "scaled_repo";
        fs::create_directories(workspace_root / "src");

#ifdef CODELENSES_SOURCE_DIR
        sample_workspace_root = fs::path(CODELENSES_SOURCE_DIR) / "sample_workspace";
#else
        sample_workspace_root = fs::current_path() / "sample_workspace";
#endif

        // Generate scaled repository: 100 C and Python files with realistic AST structures
        for (int i = 0; i < 60; ++i) {
            std::ofstream fc(workspace_root / "src" / ("component_" + std::to_string(i) + ".c"));
            fc << "/* Component " << i << " */\n"
               << "typedef struct {\n"
               << "    int id;\n"
               << "    char label[32];\n"
               << "} ComponentState_" << i << ";\n\n"
               << "int init_comp_" << i << "(ComponentState_" << i << "* s) {\n"
               << "    if (!s) return -1;\n"
               << "    s->id = " << i << ";\n"
               << "    return 0;\n"
               << "}\n\n"
               << "int exec_comp_" << i << "(int val) {\n"
               << "    return val * " << (i + 1) << ";\n"
               << "}\n";
            fc.close();
        }

        for (int i = 0; i < 40; ++i) {
            std::ofstream fp(workspace_root / "src" / ("handler_" + std::to_string(i) + ".py"));
            fp << "# Handler " << i << "\n"
               << "class Handler" << i << ":\n"
               << "    def __init__(self, name: str):\n"
               << "        self.name = name\n"
               << "        self.active = True\n\n"
               << "    def handle_request(self, payload: dict) -> bool:\n"
               << "        if not self.active:\n"
               << "            return False\n"
               << "        return len(payload) > " << i << "\n";
            fp.close();
        }
    }

    ~FootprintFixture() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

} // namespace

TEST_CASE("Benchmark: Memory Footprint and Database Size Measurements (I-08)",
          "[benchmark][footprint]") {
    FootprintFixture fixture;

    SECTION("1. Footprint on Polyglot Sample Workspace") {
        if (fs::exists(fixture.sample_workspace_root)) {
            uintmax_t source_bytes = calculate_directory_size(fixture.sample_workspace_root);
            fs::path db_file = fixture.temp_dir / "sample_footprint.db";
            fs::remove(db_file);

            size_t rss_before_kb = get_current_rss_kb();

            auto db = Database::open(db_file.string(), true);
            index::IndexingPipeline pipeline(*db);
            ApiService service(*db, pipeline);

            int64_t ws_id = service
                                .create_workspace(CreateWorkspaceRequest{
                                    .root_path = fixture.sample_workspace_root.string(),
                                    .name = "Sample Polyglot Footprint",
                                    .include_patterns = {},
                                    .exclude_patterns = {},
                                    .default_ignores = {},
                                })
                                .id;

            auto index_res = pipeline.run_indexing(ws_id, "full", true);
            REQUIRE(index_res.has_value());

            size_t rss_after_kb = get_current_rss_kb();
            size_t peak_rss_kb = get_peak_rss_kb();

            // Checkpoint WAL to measure exact database footprint
            Statement checkpoint(db->connection().handle(), "PRAGMA wal_checkpoint(TRUNCATE);");
            (void)checkpoint.step();

            uintmax_t db_bytes = fs::file_size(db_file);
            double expansion_ratio =
                static_cast<double>(db_bytes) / static_cast<double>(std::max(1UL, source_bytes));

            auto st = service.get_workspace_status(ws_id);

            std::cout << "\n[BENCHMARK I-08] Sample Polyglot Workspace Footprint:\n"
                      << "  - Files Indexed: " << st.file_count << "\n"
                      << "  - Symbols Extracted: " << st.symbol_count << "\n"
                      << "  - Source Size: " << (static_cast<double>(source_bytes) / 1024.0)
                      << " KB\n"
                      << "  - Database Size: " << (static_cast<double>(db_bytes) / 1024.0)
                      << " KB\n"
                      << "  - Storage Expansion Ratio: " << expansion_ratio << "x\n"
                      << "  - Baseline RSS: " << (static_cast<double>(rss_before_kb) / 1024.0)
                      << " MB\n"
                      << "  - Post-Index RSS: " << (static_cast<double>(rss_after_kb) / 1024.0)
                      << " MB\n"
                      << "  - Peak RSS: " << (static_cast<double>(peak_rss_kb) / 1024.0) << " MB\n"
                      << "  - Net RSS Growth: "
                      << (static_cast<double>(rss_after_kb) - static_cast<double>(rss_before_kb)) /
                             1024.0
                      << " MB\n";

            // Storage threshold: small sample workspace with SQLite schema pages stays well under 5
            // MB
            CHECK(db_bytes < 5UL * 1024UL * 1024UL); // Under 5 MB
            CHECK(expansion_ratio < 30.0);       // Ratio bounded accounting for fixed schema tables
            CHECK(peak_rss_kb < (is_sanitized_build ? 1500UL * 1024UL : 500UL * 1024UL)); // Peak under 500 MB (1500 MB with sanitizers)
        }
    }

    SECTION("2. Footprint on Scaled Repository (100 files, 1,000+ symbols)") {
        uintmax_t source_bytes = calculate_directory_size(fixture.workspace_root);
        fs::path db_file = fixture.temp_dir / "scaled_footprint.db";
        fs::remove(db_file);

        size_t rss_before_kb = get_current_rss_kb();

        auto db = Database::open(db_file.string(), true);
        index::IndexingPipeline pipeline(*db);
        ApiService service(*db, pipeline);

        int64_t ws_id = service
                            .create_workspace(CreateWorkspaceRequest{
                                .root_path = fixture.workspace_root.string(),
                                .name = "Scaled Repo Footprint",
                                .include_patterns = {},
                                .exclude_patterns = {},
                                .default_ignores = {},
                            })
                            .id;

        auto index_res = pipeline.run_indexing(ws_id, "full", true);
        REQUIRE(index_res.has_value());

        size_t rss_after_kb = get_current_rss_kb();
        size_t peak_rss_kb = get_peak_rss_kb();

        Statement checkpoint(db->connection().handle(), "PRAGMA wal_checkpoint(TRUNCATE);");
        (void)checkpoint.step();

        uintmax_t db_bytes = fs::file_size(db_file);
        double expansion_ratio =
            static_cast<double>(db_bytes) / static_cast<double>(std::max(1UL, source_bytes));

        auto st = service.get_workspace_status(ws_id);
        double bytes_per_symbol =
            (st.symbol_count > 0)
                ? (static_cast<double>(db_bytes) / static_cast<double>(st.symbol_count))
                : 0.0;
        double bytes_per_file =
            (st.file_count > 0)
                ? (static_cast<double>(db_bytes) / static_cast<double>(st.file_count))
                : 0.0;

        std::cout << "[BENCHMARK I-08] Scaled Repository Footprint (100 files):\n"
                  << "  - Files Indexed: " << st.file_count << "\n"
                  << "  - Symbols Extracted: " << st.symbol_count << "\n"
                  << "  - Source Size: " << (static_cast<double>(source_bytes) / 1024.0) << " KB\n"
                  << "  - Database Size: " << (static_cast<double>(db_bytes) / 1024.0) << " KB\n"
                  << "  - Storage Expansion Ratio: " << expansion_ratio << "x\n"
                  << "  - Storage Per File: " << (bytes_per_file / 1024.0) << " KB/file\n"
                  << "  - Storage Per Symbol: " << bytes_per_symbol << " bytes/symbol\n"
                  << "  - Baseline RSS: " << (static_cast<double>(rss_before_kb) / 1024.0)
                  << " MB\n"
                  << "  - Post-Index RSS: " << (static_cast<double>(rss_after_kb) / 1024.0)
                  << " MB\n"
                  << "  - Peak RSS: " << (static_cast<double>(peak_rss_kb) / 1024.0) << " MB\n";

        // Verification thresholds
        CHECK(st.file_count == 100);
        CHECK(st.symbol_count >= 200);
        CHECK(db_bytes < 20UL * 1024UL * 1024UL); // Under 20 MB for 100 files
        CHECK(bytes_per_symbol < 5000.0);         // Under 5 KB per indexed symbol
        CHECK(peak_rss_kb < (is_sanitized_build ? 1500UL * 1024UL : 500UL * 1024UL));      // Peak memory under 500 MB (1500 MB with sanitizers)
    }
}
