#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "codelenses/db/database.hpp"
#include "codelenses/filesystem/discovery.hpp"
#include "codelenses/filesystem/path.hpp"
#include "codelenses/index/indexer.hpp"
#include "codelenses/server/service.hpp"
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace codelenses;

namespace {

struct SampleWorkspaceTestEnv {
    fs::path sample_root;
    fs::path db_file;
    std::unique_ptr<Database> db;

    SampleWorkspaceTestEnv() {
#ifdef CODELENSES_SOURCE_DIR
        sample_root = fs::path(CODELENSES_SOURCE_DIR) / "sample_workspace";
#else
        sample_root = fs::current_path() / "sample_workspace";
#endif
        auto pid = std::to_string(::getpid());
        auto now = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        db_file = fs::temp_directory_path() / ("codelenses_test_sample_" + pid + "_" + now + ".db");

        fs::remove(db_file);
        db = Database::open(db_file.string(), true);
    }

    ~SampleWorkspaceTestEnv() {
        db.reset();
        std::error_code ec;
        fs::remove(db_file, ec);
        fs::remove(db_file.string() + "-wal", ec);
        fs::remove(db_file.string() + "-shm", ec);
    }

    int64_t create_workspace(const std::string& name = "Sample Polyglot Workspace") {
        Workspace ws{
            .root_path = sample_root.string(),
            .name = name,
        };
        return db->workspaces().create(ws);
    }
};

} // namespace

TEST_CASE("Sample workspace file discovery covers all target languages (I-01)",
          "[sample_workspace][discovery]") {
    SampleWorkspaceTestEnv env;
    REQUIRE(fs::exists(env.sample_root));
    REQUIRE(fs::is_directory(env.sample_root));

    filesystem::DiscoveryOptions disc_opts;
    disc_opts.respect_ignore_files = true;
    disc_opts.default_ignores = {".git", "node_modules", "dist", "build"};

    filesystem::FileDiscovery discovery(disc_opts);
    auto res = discovery.discover(env.sample_root);
    REQUIRE(res.has_value());

    const auto& files = *res;
    REQUIRE_FALSE(files.empty());

    std::set<Language> detected_languages;
    std::set<std::string> relative_paths;
    bool found_binary = false;
    bool found_ignored = false;

    for (const auto& f : files) {
        detected_languages.insert(f.language);
        relative_paths.insert(f.relative_path);

        if (f.is_binary) {
            found_binary = true;
            CHECK(f.relative_path == "assets/binary.dat");
        }

        if (f.relative_path.find("ignored_dir") != std::string::npos) {
            found_ignored = true;
        }
    }

    // 1. Verify ignore rules excluded ignored_dir/
    CHECK_FALSE(found_ignored);

    // 2. Verify binary detection detected assets/binary.dat
    CHECK(found_binary);

    // 3. Verify all target languages from requirements FR-5 are detected:
    // C, C++, C#, Python, TypeScript, JavaScript, Go, Java, Shell/Bash
    CHECK(detected_languages.contains(Language::c));
    CHECK(detected_languages.contains(Language::cpp));
    CHECK(detected_languages.contains(Language::csharp));
    CHECK(detected_languages.contains(Language::python));
    CHECK(detected_languages.contains(Language::typescript));
    CHECK(detected_languages.contains(Language::javascript));
    CHECK(detected_languages.contains(Language::go));
    CHECK(detected_languages.contains(Language::java));
    CHECK((detected_languages.contains(Language::shell) ||
           detected_languages.contains(Language::bash)));

    // Unknown/data file
    CHECK(detected_languages.contains(Language::unknown));

    // Verify key specific files were discovered
    CHECK(relative_paths.contains("c/include/common.h"));
    CHECK(relative_paths.contains("c/src/processor.c"));
    CHECK(relative_paths.contains("cpp/include/filter.hpp"));
    CHECK(relative_paths.contains("cpp/src/pipeline.cpp"));
    CHECK(relative_paths.contains("csharp/Models/ISensor.cs"));
    CHECK(relative_paths.contains("csharp/Models/TemperatureSensor.cs"));
    CHECK(relative_paths.contains("python/telemetry/models.py"));
    CHECK(relative_paths.contains("python/telemetry/processor.py"));
    CHECK(relative_paths.contains("typescript/src/models.ts"));
    CHECK(relative_paths.contains("typescript/src/components/UserBadge.tsx"));
    CHECK(relative_paths.contains("javascript/src/helpers.js"));
    CHECK(relative_paths.contains("javascript/src/Notification.jsx"));
    CHECK(relative_paths.contains("go/pkg/queue/task.go"));
    CHECK(relative_paths.contains("go/pkg/queue/dispatcher.go"));
    CHECK(relative_paths.contains("java/com/example/billing/Invoice.java"));
    CHECK(relative_paths.contains("shell/build.sh"));
    CHECK(relative_paths.contains("shell/healthcheck"));
    CHECK(relative_paths.contains("compile_commands.json"));
    CHECK(relative_paths.contains("config/settings.json"));
    CHECK(relative_paths.contains("README.md"));
}

TEST_CASE("Sample workspace end-to-end indexing pipeline (I-01)", "[sample_workspace][indexing]") {
    SampleWorkspaceTestEnv env;
    int64_t ws_id = env.create_workspace();
    REQUIRE(ws_id > 0);

    index::IndexingPipeline pipeline(*env.db);

    SECTION("Full indexing succeeds and indexes all languages without failure") {
        auto res = pipeline.run_indexing(ws_id, "full", true);
        REQUIRE(res.has_value());
        REQUIRE(res->status == "completed");
        REQUIRE(res->files_total > 20);
        REQUIRE(res->files_processed == res->files_total);
        REQUIRE(res->files_skipped == 0);

        // Verify workspace status is ready
        auto ws = env.db->workspaces().get_by_id(ws_id);
        REQUIRE(ws.has_value());
        REQUIRE(ws->status == WorkspaceStatus::ready);
        REQUIRE(ws->revision == 1);

        // Verify all discovered files have database records
        auto file_records = env.db->files().list_by_workspace(ws_id);
        REQUIRE(file_records.size() == static_cast<size_t>(res->files_total));

        // Verify binary file record
        auto bin_record = env.db->files().get_by_path(ws_id, "assets/binary.dat");
        REQUIRE(bin_record.has_value());
        CHECK(bin_record->is_binary);

        // Verify C symbols
        auto init_metric_syms = env.db->symbols().find_by_name(ws_id, "init_metric");
        CHECK_FALSE(init_metric_syms.empty());
        auto metric_rec_syms = env.db->symbols().find_by_name(ws_id, "MetricRecord");
        CHECK_FALSE(metric_rec_syms.empty());
        auto buffer_size_syms = env.db->symbols().find_by_name(ws_id, "BUFFER_SIZE");
        CHECK_FALSE(buffer_size_syms.empty());

        // Verify C++ symbols
        auto filter_syms = env.db->symbols().find_by_name(ws_id, "ThresholdFilter");
        CHECK_FALSE(filter_syms.empty());
        auto base_filter_syms = env.db->symbols().find_by_name(ws_id, "BaseFilter");
        CHECK_FALSE(base_filter_syms.empty());
        auto pipeline_syms = env.db->symbols().find_by_name(ws_id, "MetricPipeline");
        CHECK_FALSE(pipeline_syms.empty());
        auto circ_buffer_syms = env.db->symbols().find_by_name(ws_id, "CircularBuffer");
        CHECK_FALSE(circ_buffer_syms.empty());

        // Verify built relations (calls, imports)
        auto all_relations = env.db->relations().list_by_workspace(ws_id);
        CHECK_FALSE(all_relations.empty());
        bool found_calls_relation = false;
        bool found_imports_relation = false;
        for (const auto& rel : all_relations) {
            if (rel.relation_kind == "calls") {
                found_calls_relation = true;
            }
            if (rel.relation_kind == "imports") {
                found_imports_relation = true;
            }
        }
        CHECK(found_calls_relation);
        CHECK(found_imports_relation);

        // Verify occurrences (including base type references for C++ and C#)
        auto all_occurrences = env.db->occurrences().list_by_workspace(ws_id);
        CHECK_FALSE(all_occurrences.empty());
        bool found_base_filter_occ = false;
        bool found_isensor_occ = false;
        for (const auto& occ : all_occurrences) {
            if (occ.name == "BaseFilter") {
                found_base_filter_occ = true;
            }
            if (occ.name == "ISensor") {
                found_isensor_occ = true;
            }
        }
        CHECK(found_base_filter_occ);
        CHECK(found_isensor_occ);

        // Verify file dependencies (e.g. C/C++ includes, Python imports)
        auto all_deps = env.db->dependencies().list_by_workspace(ws_id);
        CHECK_FALSE(all_deps.empty());
        bool found_include_dep = false;
        for (const auto& dep : all_deps) {
            if (dep.raw_name == "common.h" || dep.raw_name == "math_utils.h" ||
                dep.raw_name == "filter.hpp") {
                found_include_dep = true;
                break;
            }
        }
        CHECK(found_include_dep);

        // Verify C# symbols
        auto isensor_syms = env.db->symbols().find_by_name(ws_id, "ISensor");
        CHECK_FALSE(isensor_syms.empty());
        auto temp_sensor_syms = env.db->symbols().find_by_name(ws_id, "TemperatureSensor");
        CHECK_FALSE(temp_sensor_syms.empty());
        auto sensor_monitor_syms = env.db->symbols().find_by_name(ws_id, "SensorMonitor");
        CHECK_FALSE(sensor_monitor_syms.empty());

        // Verify Python symbols
        auto telemetry_event_syms = env.db->symbols().find_by_name(ws_id, "TelemetryEvent");
        CHECK_FALSE(telemetry_event_syms.empty());
        auto processor_syms = env.db->symbols().find_by_name(ws_id, "EventProcessor");
        CHECK_FALSE(processor_syms.empty());
        auto timed_op_syms = env.db->symbols().find_by_name(ws_id, "timed_operation");
        CHECK_FALSE(timed_op_syms.empty());
        auto aggregate_syms = env.db->symbols().find_by_name(ws_id, "aggregate_events");
        CHECK_FALSE(aggregate_syms.empty());

        // Verify diagnostics (clean when all grammars are active)
        auto diagnostics = env.db->diagnostics().list_by_workspace(ws_id);
        CHECK(diagnostics.empty());
    }

    SECTION("Incremental indexing skips unchanged files") {
        auto first_res = pipeline.run_indexing(ws_id, "full", true);
        REQUIRE(first_res.has_value());
        REQUIRE(first_res->status == "completed");
        auto total_files = first_res->files_total;

        auto second_res = pipeline.run_indexing(ws_id, "incremental", false);
        REQUIRE(second_res.has_value());
        REQUIRE(second_res->status == "completed");
        CHECK(second_res->files_total == total_files);
        CHECK(second_res->files_processed == 0);
        CHECK(second_res->files_skipped == total_files);
    }
}

TEST_CASE("Sample workspace ApiService tree, content, and compile-commands (I-01)",
          "[sample_workspace][service]") {
    SampleWorkspaceTestEnv env;
    int64_t ws_id = env.create_workspace();
    REQUIRE(ws_id > 0);

    index::IndexingPipeline pipeline(*env.db);
    auto index_res = pipeline.run_indexing(ws_id, "full", true);
    REQUIRE(index_res.has_value());
    REQUIRE(index_res->status == "completed");

    server::ApiService service(*env.db, pipeline);

    SECTION("Root tree returns multi-language directories") {
        auto tree = service.get_tree(ws_id, "");
        CHECK(tree.workspace_id == ws_id);
        CHECK(tree.path.empty());

        std::set<std::string> folder_names;
        std::set<std::string> file_names;
        for (const auto& item : tree.entries) {
            if (item.type == "directory") {
                folder_names.insert(item.name);
            } else {
                file_names.insert(item.name);
            }
        }

        CHECK(folder_names.contains("c"));
        CHECK(folder_names.contains("cpp"));
        CHECK(folder_names.contains("csharp"));
        CHECK(folder_names.contains("python"));
        CHECK(folder_names.contains("typescript"));
        CHECK(folder_names.contains("javascript"));
        CHECK(folder_names.contains("go"));
        CHECK(folder_names.contains("java"));
        CHECK(folder_names.contains("shell"));
        CHECK(folder_names.contains("assets"));
        CHECK(folder_names.contains("config"));

        CHECK(file_names.contains("README.md"));
        CHECK(file_names.contains("compile_commands.json"));
    }

    SECTION("Source content retrieval succeeds for sample files") {
        auto file_rec = env.db->files().get_by_path(ws_id, "c/include/common.h");
        REQUIRE(file_rec.has_value());

        auto content = service.get_file_content(ws_id, file_rec->id);
        CHECK(content.file_id == file_rec->id);
        CHECK(content.content.find("SAMPLE_COMMON_H") != std::string::npos);
        CHECK(content.content.find("MetricRecord") != std::string::npos);
        CHECK_FALSE(content.is_binary);
    }

    SECTION("Compile commands summary reflects workspace compile_commands.json") {
        auto cc_summary = service.get_workspace_compile_commands(ws_id);
        CHECK(cc_summary.exists);
        CHECK(cc_summary.total_commands >= 3);
    }

    SECTION("Symbol search returns unresolved calls") {
        auto file_rec = env.db->files().get_by_path(ws_id, "c/include/common.h");
        REQUIRE(file_rec.has_value());

        env.db->references().insert(ReferenceOccurrence{
            .workspace_id = ws_id,
            .source_file_id = file_rec->id,
            .name = "unresolved_metric_hook",
            .reference_kind = "call",
            .range = {.start_byte = 10, .end_byte = 32, .start_line = 15, .start_column = 4, .end_line = 15, .end_column = 26},
            .resolution = "unresolved",
            .confidence = 0.0,
        });

        auto search_res = service.search_symbols(ws_id, "unresolved_metric_hook", 10, 0);
        REQUIRE(search_res.total >= 1);
        bool found = false;
        for (const auto& item : search_res.items) {
            if (item.name == "unresolved_metric_hook") {
                found = true;
                CHECK(item.kind == "unresolved_call");
                CHECK(item.file_id == file_rec->id);
                CHECK(item.line.has_value());
                CHECK(*item.line == 15);
            }
        }
        CHECK(found);
    }
}
