#include <algorithm>
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

TEST_CASE("C# target framework participates in library profile identity", "[library][fingerprint]") {
    LibraryProfile profile{
        .language = "csharp",
        .target_framework = "net8.0",
        .source_roots = {"/local/reference-source"},
    };
    const auto net8_fingerprint = compute_library_fingerprint(profile);
    profile.target_framework = "net9.0";
    CHECK(compute_library_fingerprint(profile) != net8_fingerprint);
}

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

TEST_CASE("C# library profiles resolve imports across authorized source roots",
          "[library][csharp][e2e]") {
    TempTestDir base("csharp_lib_e2e");
    auto db = Database::open((base.path / "test.db").string(), true);
    REQUIRE(db != nullptr);

    const auto root_one = base.path / "sources" / "models";
    const auto root_two = base.path / "sources" / "contracts";
    fs::create_directories(root_one);
    fs::create_directories(root_two);
    std::ofstream(root_one / "Widget.cs") << R"CS(
namespace Example.Models;
public class Widget { }
)CS";
    std::ofstream(root_two / "IService.cs") << R"CS(
namespace Example.Contracts;
public interface IService { }
public class BaseService { }
)CS";
    std::ofstream(root_one / "MathOps.cs") << R"CS(
namespace Example.Models;
public static class MathOps { public static int Square(int value) => value * value; }
public class Container<T> { }
)CS";

    const auto project_root = base.path / "project";
    fs::create_directories(project_root);
    std::ofstream(project_root / "Program.cs") << R"CS(
using Example.Models;
using Models = Example.Models;
using static Example.Models.MathOps;
namespace Client;
public class App : Example.Contracts.BaseService, Example.Contracts.IService {
    Widget _imported;
    Models.Widget _aliased;
    Example.Models.Widget _qualified;
    Container<string> _generic;
    System.String _framework_string;
    public int GetArea() => Square(4);
}
)CS";
    std::ofstream(project_root / "NoImport.cs") << R"CS(
namespace Other;
public class NoImport { Widget _unimported; }
public class GlobalUse { IService _service; }
)CS";
    std::ofstream(project_root / "GlobalUsings.cs") << R"CS(
global using Example.Contracts;
)CS";

    IndexingPipeline pipeline(*db);
    ApiService service(*db, pipeline);
    auto wait_for_job = [&](int64_t job_id) {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
            auto job = service.get_job(job_id);
            if (job.status == "completed" || job.status == "failed" || job.status == "cancelled") {
                REQUIRE(job.status == "completed");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        FAIL("Timed out waiting for C# indexing job " << job_id);
    };

    CreateLibraryRequest library_request{
        .name = "Example source",
        .language = "cs",
        .provider = "custom",
        .language_standard = std::nullopt,
        .target_framework = "net8.0",
        .source_roots = {root_one.string(), root_two.string()},
        .default_include_roots = {},
        .defines = {},
        .include_patterns = {},
        .exclude_patterns = {},
    };
    auto missing_framework_request = library_request;
    missing_framework_request.target_framework.reset();
    CHECK_THROWS_AS(service.create_library(missing_framework_request), ApiError);
    auto library = service.create_library(library_request);
    REQUIRE(library.language == "csharp");
    REQUIRE(library.target_framework == "net8.0");
    REQUIRE(library.source_roots.size() == 2);
    auto source_roots = db->libraries().list_source_roots(library.id);
    REQUIRE(source_roots.size() == 2);
    const std::vector<int64_t> root_ids{source_roots[0].id, source_roots[1].id};
    auto library_job = service.trigger_library_index(library.id, IndexJobRequest{});
    wait_for_job(library_job.id);

    auto project = service.create_workspace(CreateWorkspaceRequest{
        .root_path = project_root.string(),
        .name = "C# consumer",
        .include_patterns = {},
        .exclude_patterns = {},
        .default_ignores = {},
    });
    service.attach_library(project.id, AttachLibraryRequest{.profile_id = library.id});
    auto project_job = service.trigger_indexing(
        project.id, IndexJobRequest{.job_type = "full", .force_full = true});
    wait_for_job(project_job.id);

    auto library_files = db->files().list_by_workspace(library.workspace_id, false);
    REQUIRE(library_files.size() == 3);
    CHECK(std::all_of(library_files.begin(), library_files.end(), [](const auto& file) {
        return file.relative_path.starts_with("root-");
    }));

    auto library_file = std::find_if(library_files.begin(), library_files.end(), [](const auto& file) {
        return file.name == "IService.cs";
    });
    REQUIRE(library_file != library_files.end());
    auto file_dto = service.get_file(project.id, library_file->id);
    CHECK(file_dto.origin_metadata.origin == "library");
    CHECK(file_dto.origin_metadata.library_profile_id == library.id);
    CHECK(file_dto.origin_metadata.target_framework == "net8.0");
    CHECK(service.get_file_content(project.id, library_file->id).content.find("interface IService") !=
          std::string::npos);

    auto occurrences = db->occurrences().list_by_workspace(project.id);
    auto find_occurrence = [&](const std::string& file_name, const std::string& name) {
        return std::find_if(occurrences.begin(), occurrences.end(), [&](const Occurrence& occ) {
            auto file = db->files().get_by_id(occ.file_id);
            return file && fs::path(file->relative_path).filename() == file_name &&
                   occ.name == name && occ.occurrence_kind == "reference";
        });
    };
    auto imported = find_occurrence("Program.cs", "Widget");
    REQUIRE(imported != occurrences.end());
    CHECK(imported->resolution == "resolved");
    auto aliased = find_occurrence("Program.cs", "Models.Widget");
    REQUIRE(aliased != occurrences.end());
    CHECK(aliased->resolution == "resolved");
    auto qualified = find_occurrence("Program.cs", "Example.Models.Widget");
    REQUIRE(qualified != occurrences.end());
    CHECK(qualified->resolution == "resolved");
    auto unimported = find_occurrence("NoImport.cs", "Widget");
    REQUIRE(unimported != occurrences.end());
    CHECK(unimported->resolution == "unresolved");
    auto global_imported = find_occurrence("NoImport.cs", "IService");
    REQUIRE(global_imported != occurrences.end());
    CHECK(global_imported->resolution == "resolved");

    auto generic = find_occurrence("Program.cs", "Container<string>");
    REQUIRE(generic != occurrences.end());
    CHECK(generic->resolution == "resolved");
    auto missing_framework_symbol = find_occurrence("Program.cs", "System.String");
    REQUIRE(missing_framework_symbol != occurrences.end());
    CHECK(missing_framework_symbol->resolution == "unresolved");

    auto base_type = std::find_if(occurrences.begin(), occurrences.end(), [](const Occurrence& occ) {
        return occ.name == "Example.Contracts.BaseService" &&
               occ.occurrence_kind == "inheritance";
    });
    REQUIRE(base_type != occurrences.end());
    CHECK(base_type->resolution == "resolved");
    auto interface_type = std::find_if(occurrences.begin(), occurrences.end(), [](const Occurrence& occ) {
        return occ.name == "Example.Contracts.IService" &&
               occ.occurrence_kind == "implementation";
    });
    REQUIRE(interface_type != occurrences.end());
    CHECK(interface_type->resolution == "resolved");

    auto project_symbols = db->symbols().list_by_workspace(project.id);
    auto app_symbol = std::find_if(project_symbols.begin(), project_symbols.end(),
                                   [](const Symbol& symbol) { return symbol.name == "App"; });
    REQUIRE(app_symbol != project_symbols.end());
    auto app_graph = service.get_symbol_graph(project.id, app_symbol->id, 1, 50, 100, {});
    REQUIRE(base_type->symbol_id.has_value());
    REQUIRE(interface_type->symbol_id.has_value());
    CHECK(std::any_of(app_graph.edges.begin(), app_graph.edges.end(), [&](const auto& edge) {
        return edge.relation_kind == "inherits" &&
               edge.target_symbol_id == *base_type->symbol_id;
    }));
    CHECK(std::any_of(app_graph.edges.begin(), app_graph.edges.end(), [&](const auto& edge) {
        return edge.relation_kind == "implements" &&
               edge.target_symbol_id == *interface_type->symbol_id;
    }));

    auto static_call = find_occurrence("Program.cs", "Square");
    REQUIRE(static_call != occurrences.end());
    CHECK(static_call->resolution == "resolved");
    auto library_symbols = db->symbols().list_by_workspace(library.workspace_id);
    auto square = std::find_if(library_symbols.begin(), library_symbols.end(), [](const Symbol& symbol) {
        return symbol.name == "Square";
    });
    REQUIRE(square != library_symbols.end());
    auto callers = service.get_symbol_callers(project.id, square->id);
    REQUIRE(callers.size() == 1);
    CHECK(callers[0].name == "GetArea");

    auto same_roots_reindex = service.trigger_library_index(library.id, IndexJobRequest{});
    wait_for_job(same_roots_reindex.id);
    const auto roots_after_reindex = db->libraries().list_source_roots(library.id);
    REQUIRE(roots_after_reindex.size() == root_ids.size());
    CHECK(roots_after_reindex[0].id == root_ids[0]);
    CHECK(roots_after_reindex[1].id == root_ids[1]);

    auto conflicting_request = library_request;
    conflicting_request.name = "Example source net9";
    conflicting_request.target_framework = "net9.0";
    auto conflicting_library = service.create_library(conflicting_request);
    auto conflicting_job = service.trigger_library_index(conflicting_library.id, IndexJobRequest{});
    wait_for_job(conflicting_job.id);
    service.attach_library(project.id, AttachLibraryRequest{.profile_id = conflicting_library.id});
    auto resolve_job = service.trigger_indexing(
        project.id, IndexJobRequest{.job_type = "incremental", .force_full = false});
    wait_for_job(resolve_job.id);

    occurrences = db->occurrences().list_by_workspace(project.id);
    imported = find_occurrence("Program.cs", "Widget");
    REQUIRE(imported != occurrences.end());
    CHECK(imported->resolution == "ambiguous");

    auto search = service.search_symbols(project.id, "IService", 20, 0);
    auto service_hit = std::find_if(search.items.begin(), search.items.end(), [](const auto& hit) {
        return hit.origin_metadata.origin == "library" && hit.name == "IService";
    });
    REQUIRE(service_hit != search.items.end());
    CHECK((service_hit->origin_metadata.target_framework == "net8.0" ||
           service_hit->origin_metadata.target_framework == "net9.0"));

    service.detach_library(project.id, library.id);
    CHECK_THROWS_AS(service.get_file(project.id, library_file->id), ApiError);
}
