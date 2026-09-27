#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "codelenses/adapters/adapter.hpp"
#include "codelenses/adapters/cpp_adapter.hpp"
#include "codelenses/adapters/registry.hpp"
#include "codelenses/db/database.hpp"
#include "codelenses/domain/library.hpp"
#include "codelenses/filesystem/discovery.hpp"
#include "codelenses/index/indexer.hpp"
#include "codelenses/resolver/compile_commands.hpp"
#include "codelenses/resolver/dependency_resolver.hpp"
#include "codelenses/resolver/resolver.hpp"
#include "codelenses/resolver/symbol_resolver.hpp"
#include "codelenses/server/error.hpp"
#include "codelenses/server/service.hpp"
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using namespace codelenses;
using namespace codelenses::adapters;
using namespace codelenses::filesystem;
using namespace codelenses::index;
using namespace codelenses::resolver;
using namespace codelenses::server;

namespace {

struct TempTestDir {
    fs::path path;

    TempTestDir(const std::string& prefix) {
        auto now = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        path = fs::temp_directory_path() / (prefix + "_" + now);
        fs::remove_all(path);
        fs::create_directories(path);
    }

    ~TempTestDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    void write(const fs::path& rel, const std::string& content) {
        auto full = path / rel;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out << content;
    }
};

} // namespace

TEST_CASE("Compiler include ordering and suppression flags", "[library][compile_commands]") {
    CompileCommandContext ctx;
    ctx.search_entries = {
        IncludeSearchEntry{
            .directory = "/usr/include/after",
            .category = IncludeCategory::after,
            .origin = IncludeOrigin::compile_command,
        },
        IncludeSearchEntry{
            .directory = "/custom/include",
            .category = IncludeCategory::standard,
            .origin = IncludeOrigin::compile_command,
        },
        IncludeSearchEntry{
            .directory = "/local/quote",
            .category = IncludeCategory::quoted,
            .origin = IncludeOrigin::compile_command,
        },
        IncludeSearchEntry{
            .directory = "/system/sys_inc",
            .category = IncludeCategory::system,
            .origin = IncludeOrigin::compile_command,
        },
        IncludeSearchEntry{
            .directory = "/usr/include/c++/v1",
            .category = IncludeCategory::default_toolchain,
            .origin = IncludeOrigin::configured_profile,
            .role = RootRole::cpp_library,
        },
        IncludeSearchEntry{
            .directory = "/custom/include", // Duplicate: should be deduplicated
            .category = IncludeCategory::standard,
            .origin = IncludeOrigin::compile_command,
        },
    };

    std::vector<fs::path> default_dirs = {"/usr/include"};

    SECTION("Quoted include ordering") {
        auto paths = ctx.get_ordered_include_paths(true, "/source/dir", default_dirs, true);
        REQUIRE(paths.size() == 7);
        CHECK(paths[0] == fs::path("/source/dir"));
        CHECK(paths[1] == fs::path("/local/quote"));
        CHECK(paths[2] == fs::path("/custom/include"));
        CHECK(paths[3] == fs::path("/system/sys_inc"));
        CHECK(paths[4] == fs::path("/usr/include/c++/v1"));
        CHECK(paths[5] == fs::path("/usr/include"));
        CHECK(paths[6] == fs::path("/usr/include/after"));
    }

    SECTION("Angle include ordering omits source dir and quote entries") {
        auto paths = ctx.get_ordered_include_paths(false, "/source/dir", default_dirs, true);
        REQUIRE(paths.size() == 5);
        CHECK(paths[0] == fs::path("/custom/include"));
        CHECK(paths[1] == fs::path("/system/sys_inc"));
        CHECK(paths[2] == fs::path("/usr/include/c++/v1"));
        CHECK(paths[3] == fs::path("/usr/include"));
        CHECK(paths[4] == fs::path("/usr/include/after"));
    }

    SECTION("-nostdinc suppresses all default toolchain include directories") {
        ctx.nostdinc = true;
        auto paths = ctx.get_ordered_include_paths(false, "", default_dirs, true);
        REQUIRE(paths.size() == 3);
        CHECK(paths[0] == fs::path("/custom/include"));
        CHECK(paths[1] == fs::path("/system/sys_inc"));
        CHECK(paths[2] == fs::path("/usr/include/after"));
    }

    SECTION("-nostdinc++ suppresses only C++ standard library directories, keeping C runtime headers") {
        ctx.nostdincxx = true;
        auto paths = ctx.get_ordered_include_paths(false, "", default_dirs, true);
        REQUIRE(paths.size() == 4);
        CHECK(paths[0] == fs::path("/custom/include"));
        CHECK(paths[1] == fs::path("/system/sys_inc"));
        CHECK(paths[2] == fs::path("/usr/include"));
        CHECK(paths[3] == fs::path("/usr/include/after"));
    }
}

TEST_CASE("Conservative preprocessor preserves declarations in unknown conditions",
          "[library][preprocessor]") {
    CppAdapter adapter;

    const std::string source = R"(
#ifdef _UNKNOWN_FEATURE_FLAG
void feature_enabled_decl();
#else
void feature_fallback_decl();
#endif

#if _UNKNOWN_VERSION >= 3
void version_3_decl();
#elif _UNKNOWN_VERSION == 2
void version_2_decl();
#else
void version_fallback_decl();
#endif
)";

    SECTION("Default non-conservative mode skips unselected branches") {
        CompileCommandContext normal_ctx;
        normal_ctx.conservative_preproc = false;

        auto res = adapter.parse(source, "test.hpp", normal_ctx);
        REQUIRE(res.has_value());

        std::vector<std::string> names;
        for (const auto& s : res->symbols) {
            names.push_back(s.name);
        }

        // Unknown macros evaluate to false/0 in standard preprocessor
        CHECK(std::find(names.begin(), names.end(), "feature_fallback_decl") != names.end());
        CHECK(std::find(names.begin(), names.end(), "feature_enabled_decl") == names.end());
        CHECK(std::find(names.begin(), names.end(), "version_fallback_decl") != names.end());
        CHECK(std::find(names.begin(), names.end(), "version_3_decl") == names.end());
    }

    SECTION("Conservative mode retains declarations from all potentially applicable branches") {
        CompileCommandContext conservative_ctx;
        conservative_ctx.conservative_preproc = true;

        auto res = adapter.parse(source, "test.hpp", conservative_ctx);
        REQUIRE(res.has_value());

        std::vector<std::string> names;
        for (const auto& s : res->symbols) {
            names.push_back(s.name);
        }

        CHECK(std::find(names.begin(), names.end(), "feature_enabled_decl") != names.end());
        CHECK(std::find(names.begin(), names.end(), "feature_fallback_decl") != names.end());
        CHECK(std::find(names.begin(), names.end(), "version_3_decl") != names.end());
        CHECK(std::find(names.begin(), names.end(), "version_2_decl") != names.end());
        CHECK(std::find(names.begin(), names.end(), "version_fallback_decl") != names.end());
    }
}

TEST_CASE("Discovery of extensionless C++ library headers and fragments", "[library][discovery]") {
    TempTestDir sdk("sdk_discovery");
    sdk.write("vector", "#pragma once\nnamespace std { class vector {}; }\n");
    sdk.write("string", "#pragma once\nnamespace std { class string {}; }\n");
    sdk.write("bits/stl_vector.h", "#pragma once\n");
    sdk.write("bits/vector.tcc", "#pragma once\n");
    sdk.write("custom.inc", "#pragma once\n");
    sdk.write("README.txt", "documentation\n");

    DiscoveryOptions opts;
    opts.allow_extensionless_headers = true;
    opts.extensionless_language = Language::cpp;
    opts.extension_overrides[".tcc"] = Language::cpp;
    opts.extension_overrides[".inc"] = Language::cpp;
    opts.ambiguous_header_mode = Language::cpp;

    FileDiscovery disc(opts);
    auto res = disc.discover(sdk.path);
    REQUIRE(res.has_value());

    std::unordered_map<std::string, Language> found;
    for (const auto& f : *res) {
        found[f.relative_path] = f.language;
    }

    CHECK(found["vector"] == Language::cpp);
    CHECK(found["string"] == Language::cpp);
    CHECK(found["bits/stl_vector.h"] == Language::cpp);
    CHECK(found["bits/vector.tcc"] == Language::cpp);
    CHECK(found["custom.inc"] == Language::cpp);
    CHECK(found["README.txt"] == Language::unknown);
}

TEST_CASE("End-to-end library indexing, transitive include closure, and caller isolation",
          "[library][e2e]") {
    TempTestDir base("lib_e2e");
    auto db_file = base.path / "test.db";
    auto db = Database::open(db_file.string(), true);
    REQUIRE(db != nullptr);

    // Setup synthetic SDK library
    auto sdk_root = base.path / "synthetic_sdk";
    fs::create_directories(sdk_root / "bits");

    std::ofstream(sdk_root / "vector") << R"(#pragma once
#include <bits/stl_vector.h>
)";
    std::ofstream(sdk_root / "bits/stl_vector.h") << R"(#pragma once
namespace std {
class vector {
public:
    int size() const;
};
}
)";
    std::ofstream(sdk_root / "log.h") << R"(#pragma once
void sdk_log(const char* msg);
)";

    // Setup Project A (includes <vector> and <log.h>)
    auto projA_root = base.path / "project_a";
    fs::create_directories(projA_root);
    std::ofstream(projA_root / "main.cpp") << R"(#include <vector>
#include <log.h>

void app_run() {
    std::vector v;
    sdk_log("Hello from A");
}
)";

    // Setup Project B (calls sdk_log, does NOT include <vector> but tries to use std::vector)
    auto projB_root = base.path / "project_b";
    fs::create_directories(projB_root);
    std::ofstream(projB_root / "worker.cpp") << R"(#include <log.h>

void worker_run() {
    sdk_log("Hello from B");
    std::vector unincluded_vec;
}
)";

    IndexingPipeline pipeline(*db);
    ApiService service(*db, pipeline);

    // 1. Create Library profile
    CreateLibraryRequest lib_req{
        .name = "SyntheticStdLib",
        .language = "cpp",
        .provider = "toolchain",
        .sdk_version = std::nullopt,
        .target_environment = std::nullopt,
        .language_standard = "c++20",
        .sysroot = std::nullopt,
        .source_roots = {sdk_root.string()},
        .default_include_roots = {sdk_root.string()},
        .defines = {},
        .include_patterns = {},
        .exclude_patterns = {},
    };
    auto lib_dto = service.create_library(lib_req);
    REQUIRE(lib_dto.id > 0);

    auto wait_for_job = [&](int64_t job_id,
                            std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < timeout) {
            auto st = service.get_job(job_id);
            if (st.status == "completed" || st.status == "failed" || st.status == "cancelled") {
                REQUIRE(st.status == "completed");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        FAIL("Timed out waiting for job " << job_id);
    };

    // Index the library
    auto lib_job = service.trigger_library_index(lib_dto.id, IndexJobRequest{});
    REQUIRE(lib_job.id > 0);
    wait_for_job(lib_job.id);

    // 2. Create Project A and attach Library
    CreateWorkspaceRequest reqA;
    reqA.root_path = projA_root.string();
    reqA.name = "ProjectA";
    auto projA = service.create_workspace(reqA);
    service.attach_library(projA.id, AttachLibraryRequest{.profile_id = lib_dto.id});

    auto jobA = service.trigger_indexing(
        projA.id, IndexJobRequest{.job_type = "full", .force_full = true});
    wait_for_job(jobA.id);

    // 3. Create Project B and attach Library
    CreateWorkspaceRequest reqB;
    reqB.root_path = projB_root.string();
    reqB.name = "ProjectB";
    auto projB = service.create_workspace(reqB);
    service.attach_library(projB.id, AttachLibraryRequest{.profile_id = lib_dto.id});

    auto jobB = service.trigger_indexing(
        projB.id, IndexJobRequest{.job_type = "full", .force_full = true});
    wait_for_job(jobB.id);

    SECTION("Project A resolves std::vector via transitive include closure") {
        auto occs = db->occurrences().list_by_workspace(projA.id);
        bool found_vector_resolved = false;
        for (const auto& occ : occs) {
            if (occ.name == "std::vector") {
                CHECK(occ.resolution == "resolved");
                CHECK(occ.symbol_id.has_value());
                found_vector_resolved = true;
            }
        }
        CHECK(found_vector_resolved);
    }

    SECTION("Project B cannot resolve std::vector without including <vector>") {
        auto occs = db->occurrences().list_by_workspace(projB.id);
        bool found_vector_unresolved = false;
        for (const auto& occ : occs) {
            if (occ.name == "std::vector") {
                CHECK(occ.resolution == "unresolved");
                CHECK(!occ.symbol_id.has_value());
                found_vector_unresolved = true;
            }
        }
        CHECK(found_vector_unresolved);
    }

    SECTION("Caller isolation across projects sharing the library") {
        // Find sdk_log symbol ID in the library
        auto lib_symbols = db->symbols().list_by_workspace(lib_dto.workspace_id);
        auto log_sym_it = std::find_if(lib_symbols.begin(), lib_symbols.end(),
                                       [](const Symbol& s) { return s.name == "sdk_log"; });
        REQUIRE(log_sym_it != lib_symbols.end());
        int64_t log_sym_id = log_sym_it->id;

        // Query callers from Project A context
        auto callers_a = service.get_symbol_callers(projA.id, log_sym_id);
        REQUIRE(callers_a.size() == 1);
        CHECK(callers_a[0].name == "app_run");

        // Query callers from Project B context
        auto callers_b = service.get_symbol_callers(projB.id, log_sym_id);
        REQUIRE(callers_b.size() == 1);
        CHECK(callers_b[0].name == "worker_run");
    }

    SECTION("Detaching library immediately revokes access to its files and symbols") {
        auto lib_files = db->files().list_by_workspace(lib_dto.workspace_id, false);
        REQUIRE(!lib_files.empty());
        int64_t lib_file_id = lib_files[0].id;

        // Attached: Project A can access the library file metadata
        REQUIRE_NOTHROW(service.get_file(projA.id, lib_file_id));

        // Detach library from Project A
        service.detach_library(projA.id, lib_dto.id);

        // Detached: Project A cannot access the library file
        REQUIRE_THROWS_AS(service.get_file(projA.id, lib_file_id), ApiError);
    }
}
