#include "codelenses/resolver/resolver.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "codelenses/db/transaction.hpp"
#include "codelenses/resolver/dependency_resolver.hpp"
#include "codelenses/resolver/relationship_builder.hpp"
#include "codelenses/resolver/symbol_resolver.hpp"
#include <nlohmann/json.hpp>

namespace codelenses::resolver {

namespace {

void configure_tsconfig_paths(DependencyResolver& resolver, const std::filesystem::path& root) {
    auto tsconfig_path = root / "tsconfig.json";
    if (!std::filesystem::exists(tsconfig_path)) {
        return;
    }

    try {
        std::ifstream file(tsconfig_path);
        if (!file.is_open())
            return;

        nlohmann::json js;
        file >> js;

        if (!js.contains("compilerOptions") || !js["compilerOptions"].is_object()) {
            return;
        }

        const auto& opts = js["compilerOptions"];
        std::filesystem::path base_url;
        if (opts.contains("baseUrl") && opts["baseUrl"].is_string()) {
            base_url = opts["baseUrl"].get<std::string>();
            if (base_url.is_relative()) {
                base_url = (root / base_url).lexically_normal();
            }
        } else {
            base_url = root;
        }

        std::unordered_map<std::string, std::vector<std::string>> paths_map;
        if (opts.contains("paths") && opts["paths"].is_object()) {
            for (auto it = opts["paths"].begin(); it != opts["paths"].end(); ++it) {
                if (it.value().is_array()) {
                    std::vector<std::string> targets;
                    for (const auto& elem : it.value()) {
                        if (elem.is_string()) {
                            targets.push_back(elem.get<std::string>());
                        }
                    }
                    paths_map[it.key()] = std::move(targets);
                }
            }
        }

        resolver.set_tsconfig_paths(base_url, std::move(paths_map));
    } catch (...) {
        // Ignore malformed tsconfig gracefully
    }
}

void configure_go_module(DependencyResolver& resolver, const std::filesystem::path& root) {
    auto go_mod_path = root / "go.mod";
    if (!std::filesystem::exists(go_mod_path)) {
        return;
    }

    std::ifstream file(go_mod_path);
    if (!file.is_open())
        return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.starts_with("module ")) {
            std::string mod = line.substr(7);
            while (!mod.empty() && std::isspace(static_cast<unsigned char>(mod.back()))) {
                mod.pop_back();
            }
            while (!mod.empty() && std::isspace(static_cast<unsigned char>(mod.front()))) {
                mod.erase(0, 1);
            }
            if (!mod.empty()) {
                resolver.set_go_module(mod);
                break;
            }
        }
    }
}

void configure_compilation_database(DependencyResolver& resolver, const std::filesystem::path& root,
                                    const std::optional<std::string>& configured_path,
                                    const std::optional<std::string>& default_cmd_str) {
    std::filesystem::path cdb_path;
    if (configured_path.has_value() && !configured_path->empty()) {
        cdb_path = *configured_path;
        if (cdb_path.is_relative()) {
            cdb_path = root / cdb_path;
        }
    } else {
        std::vector<std::filesystem::path> candidates = {
            root / "compile_commands.json",
            root / "build" / "compile_commands.json",
        };
        for (const auto& c : candidates) {
            if (std::filesystem::exists(c)) {
                cdb_path = c;
                break;
            }
        }
    }

    if (!cdb_path.empty() && std::filesystem::exists(cdb_path)) {
        auto loaded = CompilationDatabase::load_file(cdb_path, root);
        if (loaded) {
            resolver.set_compilation_database(*loaded);
        }
    }

    if (default_cmd_str.has_value() && !default_cmd_str->empty()) {
        auto parsed = CompilationDatabase::parse_command_string(*default_cmd_str, root, "", root);
        if (parsed && !parsed->include_dirs.empty()) {
            resolver.set_include_directories("", parsed->include_dirs, parsed->include_dirs);
        }
    }
}

} // namespace

WorkspaceResolver::WorkspaceResolver(Database& db) : db_(db) {}

namespace {

// Resolution context for one attached library. Each library gets its own DependencyResolver
// rooted at the library root, plus identity maps so results can be attributed to the owner.
struct LibraryScope {
    LibraryProfile profile;
    Workspace workspace;
    DependencyResolver dep_resolver;
    std::unordered_map<std::string, int64_t> file_id_by_rel_path;
    std::unordered_map<int64_t, FileRecord> files_by_id;
};

} // namespace

Result<ResolutionStats> WorkspaceResolver::resolve_workspace(int64_t workspace_id,
                                                             std::stop_token stop) {
    auto ws = db_.workspaces().get_by_id(workspace_id);
    if (!ws) {
        return unexpected_result<ResolutionStats>(ErrorCode::not_found, "workspace not found");
    }

    if (stop.stop_requested()) {
        return unexpected_result<ResolutionStats>(ErrorCode::cancelled, "resolution cancelled");
    }

    ResolutionStats stats;
    std::filesystem::path root(ws->root_path);

    // 1. Setup DependencyResolver
    DependencyResolver dep_resolver;
    dep_resolver.set_workspace_root(root);

    auto files = db_.files().list_by_workspace(workspace_id, false);
    std::unordered_map<int64_t, FileRecord> files_by_id;
    std::unordered_map<std::string, int64_t> file_id_by_path;

    for (const auto& f : files) {
        files_by_id[f.id] = f;
        file_id_by_path[f.relative_path] = f.id;
        auto lang = language_from_string(f.language).value_or(Language::unknown);
        dep_resolver.register_file(f.id, f.relative_path, lang);
    }

    configure_compilation_database(dep_resolver, root, ws->compile_commands_path,
                                   ws->default_compile_command);
    configure_tsconfig_paths(dep_resolver, root);
    configure_go_module(dep_resolver, root);

    if (ws->kind == codelenses::WorkspaceKind::library) {
        dep_resolver.set_allow_suffix_fallback(false);
        auto lib_prof = db_.libraries().get_profile_by_workspace(workspace_id);
        if (lib_prof.has_value()) {
            std::vector<std::filesystem::path> def_dirs;
            for (const auto& r : lib_prof->default_include_roots) {
                def_dirs.push_back(std::filesystem::path(r));
            }
            dep_resolver.set_include_directories({}, {}, def_dirs);
        }
    }

    // 1b. Attached libraries: build an isolated resolution scope per library so that project
    // files resolve first and library identities never collide with project paths.
    std::vector<LibraryScope> libraries;
    if (ws->kind != codelenses::WorkspaceKind::library) {
        for (const auto& profile : db_.libraries().list_attached(workspace_id)) {
            auto lib_ws = db_.workspaces().get_by_id(profile.workspace_id);
            if (!lib_ws.has_value()) {
                continue;
            }

            LibraryScope scope;
            scope.profile = profile;
            scope.workspace = *lib_ws;
            std::filesystem::path lib_root(lib_ws->root_path);
            scope.dep_resolver.set_workspace_root(lib_root);
            scope.dep_resolver.set_allow_suffix_fallback(false);

            std::vector<std::filesystem::path> lib_include_roots;
            for (const auto& r : profile.default_include_roots) {
                lib_include_roots.push_back(std::filesystem::path(r));
            }
            scope.dep_resolver.set_include_directories({}, {}, lib_include_roots);

            auto lib_files = db_.files().list_by_workspace(lib_ws->id, false);
            for (const auto& lf : lib_files) {
                scope.files_by_id[lf.id] = lf;
                scope.file_id_by_rel_path[lf.relative_path] = lf.id;
                auto lang = language_from_string(lf.language).value_or(Language::unknown);
                scope.dep_resolver.register_file(lf.id, lf.relative_path, lang);
            }
            libraries.push_back(std::move(scope));
        }
    }

    // 2. Resolve File Dependencies
    auto deps = db_.dependencies().list_by_workspace(workspace_id);
    std::unordered_map<int64_t, std::vector<int64_t>> file_imports;

    Transaction tx(db_.connection(), TransactionType::immediate);

    for (auto& dep : deps) {
        if (stop.stop_requested()) {
            tx.rollback();
            return unexpected_result<ResolutionStats>(ErrorCode::cancelled, "resolution cancelled");
        }

        auto it_f = files_by_id.find(dep.source_file_id);
        if (it_f == files_by_id.end()) {
            continue;
        }

        auto lang = language_from_string(it_f->second.language).value_or(Language::unknown);
        auto target =
            dep_resolver.resolve_dependency(lang, it_f->second.relative_path, dep.raw_name);

        std::optional<int64_t> project_tgt_id;
        if (target.target_file_path.has_value()) {
            auto it_tgt = file_id_by_path.find(*target.target_file_path);
            if (it_tgt != file_id_by_path.end()) {
                project_tgt_id = it_tgt->second;
            }
        }

        // If not resolved inside the project, try attached libraries in attachment order.
        // Targets keep their library file identity (origin), but resolution becomes "resolved".
        std::optional<int64_t> library_tgt_id;
        if (!project_tgt_id.has_value() && !libraries.empty()) {
            for (auto& lib : libraries) {
                auto lib_target =
                    lib.dep_resolver.resolve_dependency(lang, it_f->second.relative_path,
                                                        dep.raw_name);
                if (lib_target.target_file_path.has_value()) {
                    auto it_tgt = lib.file_id_by_rel_path.find(*lib_target.target_file_path);
                    if (it_tgt != lib.file_id_by_rel_path.end()) {
                        target = lib_target;
                        target.reason = "library:" + lib.profile.name;
                        library_tgt_id = it_tgt->second;
                        break;
                    }
                }
            }
        }

        std::optional<int64_t> tgt_id =
            project_tgt_id.has_value() ? project_tgt_id : library_tgt_id;

        if (tgt_id.has_value()) {
            dep.target_file_id = tgt_id;
            dep.resolved_path = target.target_file_path;
            dep.resolution = "resolved";

            file_imports[dep.source_file_id].push_back(*tgt_id);
            stats.dependencies_resolved++;
        } else if (target.target_file_path.has_value()) {
            // Path known but file not indexed in any registered owner scope.
            dep.target_file_id = std::nullopt;
            dep.resolved_path = target.target_file_path;
            dep.resolution = "resolved";
            stats.dependencies_resolved++;
        } else if (target.reason == "external" || target.reason == "external_or_missing") {
            dep.target_file_id = std::nullopt;
            dep.resolved_path = std::nullopt;
            dep.resolution = "external";
            stats.dependencies_external++;
        } else {
            dep.target_file_id = std::nullopt;
            dep.resolved_path = std::nullopt;
            dep.resolution = "unresolved";
        }

        db_.dependencies().update_resolution(dep.id, dep.target_file_id, dep.resolved_path,
                                             dep.resolution);
    }

    // Include resolved dependencies from attached libraries so transitive inclusions are reachable.
    for (const auto& lib : libraries) {
        auto lib_deps = db_.dependencies().list_by_workspace(lib.workspace.id);
        for (const auto& ld : lib_deps) {
            if (ld.target_file_id.has_value()) {
                file_imports[ld.source_file_id].push_back(*ld.target_file_id);
            }
        }
    }

    std::unordered_map<int64_t, std::vector<int64_t>> transitive_imports_cache;
    auto get_transitive_imports = [&](int64_t file_id) -> const std::vector<int64_t>& {
        auto it_cached = transitive_imports_cache.find(file_id);
        if (it_cached != transitive_imports_cache.end()) {
            return it_cached->second;
        }

        std::vector<int64_t> closure;
        std::unordered_set<int64_t> visited;
        visited.insert(file_id);

        std::vector<int64_t> current_level;
        auto it = file_imports.find(file_id);
        if (it != file_imports.end()) {
            for (int64_t dep_id : it->second) {
                if (visited.insert(dep_id).second) {
                    current_level.push_back(dep_id);
                    closure.push_back(dep_id);
                }
            }
        }

        size_t depth = 0;
        constexpr size_t kMaxDepth = 64;
        while (!current_level.empty() && depth < kMaxDepth) {
            std::vector<int64_t> next_level;
            for (int64_t cur : current_level) {
                auto cur_it = file_imports.find(cur);
                if (cur_it != file_imports.end()) {
                    for (int64_t next_id : cur_it->second) {
                        if (visited.insert(next_id).second) {
                            next_level.push_back(next_id);
                            closure.push_back(next_id);
                        }
                    }
                }
            }
            current_level = std::move(next_level);
            depth++;
        }

        auto [ins_it, _] = transitive_imports_cache.emplace(file_id, std::move(closure));
        return ins_it->second;
    };

    // 3. Setup SymbolResolver
    SymbolResolver symbol_resolver;
    auto symbols = db_.symbols().list_by_workspace(workspace_id);
    for (const auto& sym : symbols) {
        auto it_f = files_by_id.find(sym.file_id);
        std::string rel_path = (it_f != files_by_id.end()) ? it_f->second.relative_path : "";
        symbol_resolver.add_symbol(sym, rel_path);
    }

    // Add symbols from attached libraries (owner-scoped keys; project symbols keep priority).
    for (const auto& lib : libraries) {
        auto lib_symbols = db_.symbols().list_by_workspace(lib.workspace.id);
        for (const auto& sym : lib_symbols) {
            auto it_f = lib.files_by_id.find(sym.file_id);
            std::string rel_path =
                (it_f != lib.files_by_id.end()) ? it_f->second.relative_path : "";
            symbol_resolver.add_symbol(sym, rel_path, lib.workspace.id);
        }
    }

    // 4. Resolve Occurrences
    auto occurrences = db_.occurrences().list_by_workspace(workspace_id);
    for (auto& occ : occurrences) {
        if (stop.stop_requested()) {
            tx.rollback();
            return unexpected_result<ResolutionStats>(ErrorCode::cancelled, "resolution cancelled");
        }

        auto it_f = files_by_id.find(occ.file_id);
        if (it_f == files_by_id.end()) {
            continue;
        }

        auto lang = language_from_string(it_f->second.language).value_or(Language::unknown);
        const auto& imported = get_transitive_imports(occ.file_id);

        auto res =
            symbol_resolver.resolve_occurrence(occ, it_f->second.relative_path, lang, imported);

        std::optional<int64_t> resolved_sym_id = std::nullopt;
        if (res.resolution == Resolution::resolved && !res.candidates.empty()) {
            resolved_sym_id = res.candidates[0].target_symbol_id;
        }

        occ.symbol_id = resolved_sym_id;
        occ.resolution = std::string(to_string(res.resolution));
        occ.confidence = res.confidence;

        if (res.resolution == Resolution::resolved) {
            stats.occurrences_resolved++;
        } else if (res.resolution == Resolution::ambiguous) {
            stats.occurrences_ambiguous++;
        } else {
            stats.occurrences_unresolved++;
        }

        db_.occurrences().update_resolution(occ.id, occ.symbol_id, occ.resolution, occ.confidence);
    }

    // 5. Resolve References
    auto references = db_.references().list_by_workspace(workspace_id);
    for (auto& ref : references) {
        if (stop.stop_requested()) {
            tx.rollback();
            return unexpected_result<ResolutionStats>(ErrorCode::cancelled, "resolution cancelled");
        }

        auto it_f = files_by_id.find(ref.source_file_id);
        if (it_f == files_by_id.end()) {
            continue;
        }

        auto lang = language_from_string(it_f->second.language).value_or(Language::unknown);
        const auto& imported = get_transitive_imports(ref.source_file_id);

        auto res =
            symbol_resolver.resolve_reference(ref, it_f->second.relative_path, lang, imported);

        std::optional<int64_t> target_id = std::nullopt;
        if (res.resolution == Resolution::resolved && !res.candidates.empty()) {
            target_id = res.candidates[0].target_symbol_id;
        }

        if (res.enclosing_symbol_id.has_value()) {
            ref.source_symbol_id = res.enclosing_symbol_id;
        }
        ref.target_symbol_id = target_id;
        ref.resolution = std::string(to_string(res.resolution));
        ref.confidence = res.confidence;

        db_.references().update_resolution(ref.id, ref.source_symbol_id, ref.target_symbol_id,
                                           ref.resolution, ref.confidence);
    }

    // 6. Build Relationships
    RelationshipBuilder rel_builder;
    auto built_relations = rel_builder.build_relations(workspace_id, symbols, occurrences,
                                                       references, deps, symbol_resolver);

    db_.relations().delete_by_workspace(workspace_id);
    db_.relations().insert_batch(built_relations);
    stats.relations_built = static_cast<int64_t>(built_relations.size());

    tx.commit();

    return stats;
}

} // namespace codelenses::resolver
