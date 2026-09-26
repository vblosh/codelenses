#include "codelenses/resolver/resolver.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

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

    configure_compilation_database(dep_resolver, root, ws->compile_commands_path, ws->default_compile_command);
    configure_tsconfig_paths(dep_resolver, root);
    configure_go_module(dep_resolver, root);

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

        if (target.target_file_path.has_value()) {
            auto it_tgt = file_id_by_path.find(*target.target_file_path);
            std::optional<int64_t> tgt_id = (it_tgt != file_id_by_path.end())
                                                ? std::make_optional(it_tgt->second)
                                                : std::nullopt;

            dep.target_file_id = tgt_id;
            dep.resolved_path = target.target_file_path;
            dep.resolution = "resolved";

            if (tgt_id.has_value()) {
                file_imports[dep.source_file_id].push_back(*tgt_id);
            }
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

    // 3. Setup SymbolResolver
    SymbolResolver symbol_resolver;
    auto symbols = db_.symbols().list_by_workspace(workspace_id);
    for (const auto& sym : symbols) {
        auto it_f = files_by_id.find(sym.file_id);
        std::string rel_path = (it_f != files_by_id.end()) ? it_f->second.relative_path : "";
        symbol_resolver.add_symbol(sym, rel_path);
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
        const auto& imported = file_imports[occ.file_id];

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
        const auto& imported = file_imports[ref.source_file_id];

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
