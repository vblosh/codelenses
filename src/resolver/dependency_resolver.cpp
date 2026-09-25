#include "codelenses/resolver/dependency_resolver.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace codelenses::resolver {

namespace {

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() &&
           (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string_view strip_enclosures(std::string_view s) noexcept {
    s = trim(s);
    if (s.size() >= 2) {
        if ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'') ||
            (s.front() == '<' && s.back() == '>')) {
            s.remove_prefix(1);
            s.remove_suffix(1);
        }
    }
    return s;
}

bool has_shell_dynamic_expansion(std::string_view s) noexcept {
    for (char c : s) {
        if (c == '$' || c == '{' || c == '}' || c == '`') {
            return true;
        }
    }
    return false;
}

std::string dotted_to_slash(std::string_view dotted) {
    std::string result(dotted);
    for (char& c : result) {
        if (c == '.') {
            c = '/';
        }
    }
    return result;
}

std::string clean_ts_specifier(std::string_view raw) {
    raw = trim(raw);
    auto from_pos = raw.find(" from ");
    if (from_pos != std::string_view::npos) {
        raw = trim(raw.substr(from_pos + 6));
    } else if (raw.starts_with("import ")) {
        raw = trim(raw.substr(7));
    } else if (raw.starts_with("export ") && raw.find(" from ") != std::string_view::npos) {
        raw = trim(raw.substr(raw.find(" from ") + 6));
    }
    if (raw.starts_with("require(") && raw.ends_with(')')) {
        raw = trim(raw.substr(8, raw.size() - 9));
    }
    return std::string(strip_enclosures(raw));
}

} // namespace

void DependencyResolver::set_workspace_root(std::filesystem::path root) {
    workspace_root_ = root.lexically_normal();
}

void DependencyResolver::register_file(int64_t file_id, std::filesystem::path rel_path,
                                       Language lang) {
    auto norm = rel_path.lexically_normal();
    std::string key = norm.generic_string();
    files_by_rel_path_[key] = RegisteredFile{
        .file_id = file_id,
        .relative_path = norm,
        .language = lang,
    };
    file_id_to_rel_path_[file_id] = key;
}

void DependencyResolver::unregister_file(const std::filesystem::path& rel_path) {
    auto norm = rel_path.lexically_normal();
    std::string key = norm.generic_string();
    auto it = files_by_rel_path_.find(key);
    if (it != files_by_rel_path_.end()) {
        file_id_to_rel_path_.erase(it->second.file_id);
        files_by_rel_path_.erase(it);
    }
}

void DependencyResolver::clear_files() {
    files_by_rel_path_.clear();
    file_id_to_rel_path_.clear();
}

void DependencyResolver::set_compilation_database(const CompilationDatabase& cdb) {
    compilation_db_ = cdb;
}

void DependencyResolver::set_include_directories(std::filesystem::path file_path,
                                                 std::vector<std::filesystem::path> quote_dirs,
                                                 std::vector<std::filesystem::path> system_dirs) {
    if (file_path.empty()) {
        default_include_dirs_ = IncludeDirs{
            .quote_dirs = std::move(quote_dirs),
            .system_dirs = std::move(system_dirs),
        };
    } else {
        auto key = file_path.lexically_normal().generic_string();
        file_include_dirs_[key] = IncludeDirs{
            .quote_dirs = std::move(quote_dirs),
            .system_dirs = std::move(system_dirs),
        };
    }
}

void DependencyResolver::set_tsconfig_paths(
    std::filesystem::path base_url,
    std::unordered_map<std::string, std::vector<std::string>> paths) {
    ts_base_url_ = base_url.lexically_normal();
    ts_paths_ = std::move(paths);
}

void DependencyResolver::set_go_module(std::string module_path) {
    go_module_ = std::move(module_path);
}

std::filesystem::path DependencyResolver::to_absolute(const std::filesystem::path& p) const {
    if (p.is_absolute() || workspace_root_.empty()) {
        return p.lexically_normal();
    }
    return (workspace_root_ / p).lexically_normal();
}

std::filesystem::path DependencyResolver::to_relative(const std::filesystem::path& p) const {
    if (!workspace_root_.empty()) {
        std::error_code ec;
        auto rel = std::filesystem::relative(p, workspace_root_, ec);
        if (!ec && !rel.empty() && !rel.generic_string().starts_with("..")) {
            return rel.lexically_normal();
        }
    }
    return p.lexically_normal();
}

const DependencyResolver::RegisteredFile*
DependencyResolver::find_file(const std::filesystem::path& p) const {
    if (p.empty())
        return nullptr;
    auto rel = to_relative(p);
    auto it = files_by_rel_path_.find(rel.generic_string());
    if (it != files_by_rel_path_.end()) {
        return &it->second;
    }
    return nullptr;
}

bool DependencyResolver::is_directory_in_workspace(const std::filesystem::path& dir) const {
    if (dir.empty())
        return false;
    auto rel_dir = to_relative(dir).generic_string();
    if (!rel_dir.ends_with('/')) {
        rel_dir += '/';
    }
    for (const auto& [path_str, _] : files_by_rel_path_) {
        if (path_str.starts_with(rel_dir)) {
            return true;
        }
    }
    return false;
}

CandidateTarget DependencyResolver::resolve_dependency(Language lang,
                                                       const std::filesystem::path& source_file,
                                                       std::string_view raw_name) const {
    auto candidates = resolve_dependency_candidates(lang, source_file, raw_name);
    if (!candidates.empty()) {
        return candidates.front();
    }
    return CandidateTarget{
        .target_symbol_id = std::nullopt,
        .target_file_path = std::nullopt,
        .rank = 0,
        .confidence = 0.0,
        .reason = "unresolved",
    };
}

std::vector<CandidateTarget> DependencyResolver::resolve_dependency_candidates(
    Language lang, const std::filesystem::path& source_file, std::string_view raw_name) const {
    switch (lang) {
    case Language::c:
    case Language::cpp:
        return resolve_c_cpp(source_file, raw_name);
    case Language::python:
        return resolve_python(source_file, raw_name);
    case Language::typescript:
    case Language::javascript:
        return resolve_typescript(source_file, raw_name);
    case Language::go:
        return resolve_go(source_file, raw_name);
    case Language::java:
        return resolve_java(source_file, raw_name);
    case Language::shell:
    case Language::bash:
        return resolve_shell(source_file, raw_name);
    default:
        break;
    }

    // Default generic path fallback
    std::string_view clean = strip_enclosures(raw_name);
    std::filesystem::path candidate_path = clean;
    if (const auto* rf = find_file(candidate_path)) {
        return {CandidateTarget{
            .target_symbol_id = std::nullopt,
            .target_symbol_key = std::nullopt,
            .target_file_path = rf->relative_path.generic_string(),
            .rank = 1,
            .confidence = 1.0,
            .reason = "path",
        }};
    }

    return {CandidateTarget{
        .target_symbol_id = std::nullopt,
        .target_symbol_key = std::nullopt,
        .target_file_path = std::nullopt,
        .rank = 0,
        .confidence = 0.0,
        .reason = "unresolved",
    }};
}

std::vector<CandidateTarget>
DependencyResolver::resolve_c_cpp(const std::filesystem::path& source_file,
                                  std::string_view raw_name) const {
    std::string_view trimmed = trim(raw_name);
    if (trimmed.empty()) {
        return {};
    }
    bool is_quote = (trimmed.front() == '"' && trimmed.back() == '"');
    std::string_view clean = strip_enclosures(trimmed);

    if (clean.empty()) {
        return {};
    }

    std::filesystem::path header_path(clean);
    std::vector<std::filesystem::path> search_dirs;

    // 1. For quote includes: directory of the source file
    if (is_quote) {
        search_dirs.push_back(source_file.parent_path());
    }

    // 2. Check compilation database for include directories of this file
    if (const auto* cmd = compilation_db_.find_for_file(source_file)) {
        for (const auto& inc : cmd->include_dirs) {
            search_dirs.push_back(inc);
        }
    }

    // 3. Check explicit include directories for this file
    auto file_key = source_file.lexically_normal().generic_string();
    auto it = file_include_dirs_.find(file_key);
    if (it != file_include_dirs_.end()) {
        if (is_quote) {
            for (const auto& q : it->second.quote_dirs) {
                search_dirs.push_back(q);
            }
        }
        for (const auto& s : it->second.system_dirs) {
            search_dirs.push_back(s);
        }
    }

    // 4. Check default include directories
    if (is_quote) {
        for (const auto& q : default_include_dirs_.quote_dirs) {
            search_dirs.push_back(q);
        }
    }
    for (const auto& s : default_include_dirs_.system_dirs) {
        search_dirs.push_back(s);
    }

    // 5. Conventional workspace include directories
    std::vector<std::filesystem::path> conventional_dirs = {"include", "inc"};
    for (const auto& conv : conventional_dirs) {
        if (is_directory_in_workspace(conv)) {
            search_dirs.push_back(conv);
        }
    }

    // 6. Try each search directory
    for (const auto& dir : search_dirs) {
        std::filesystem::path candidate = (dir / header_path).lexically_normal();
        if (const auto* rf = find_file(candidate)) {
            return {CandidateTarget{
                .target_symbol_id = std::nullopt,
                .target_symbol_key = std::nullopt,
                .target_file_path = rf->relative_path.generic_string(),
                .rank = 1,
                .confidence = 1.0,
                .reason = "include",
            }};
        }
    }

    // 7. Direct relative path check
    if (const auto* rf = find_file(header_path)) {
        return {CandidateTarget{
            .target_symbol_id = std::nullopt,
            .target_symbol_key = std::nullopt,
            .target_file_path = rf->relative_path.generic_string(),
            .rank = 1,
            .confidence = 1.0,
            .reason = "include",
        }};
    }

    // 8. Unique filename suffix fallback
    std::string suffix = "/" + header_path.generic_string();
    const RegisteredFile* unique_match = nullptr;
    size_t match_count = 0;
    for (const auto& [path_str, rf] : files_by_rel_path_) {
        if (path_str == header_path.generic_string() || path_str.ends_with(suffix)) {
            unique_match = &rf;
            match_count++;
        }
    }
    if (match_count == 1 && unique_match != nullptr) {
        return {CandidateTarget{
            .target_symbol_id = std::nullopt,
            .target_symbol_key = std::nullopt,
            .target_file_path = unique_match->relative_path.generic_string(),
            .rank = 2,
            .confidence = 0.8,
            .reason = "include",
        }};
    }

    // External header (system headers like <iostream>, <stdio.h>, etc.)
    return {CandidateTarget{
        .target_symbol_id = std::nullopt,
        .target_symbol_key = std::nullopt,
        .target_file_path = std::nullopt,
        .rank = 0,
        .confidence = 0.0,
        .reason = "external_or_missing",
    }};
}

std::vector<CandidateTarget>
DependencyResolver::resolve_python(const std::filesystem::path& source_file,
                                   std::string_view raw_name) const {
    std::string_view clean = strip_enclosures(raw_name);
    if (clean.empty()) {
        return {};
    }

    // Handle relative imports with leading dots
    std::size_t leading_dots = 0;
    while (leading_dots < clean.size() && clean[leading_dots] == '.') {
        leading_dots++;
    }

    if (leading_dots > 0) {
        std::string_view rest = clean.substr(leading_dots);
        std::filesystem::path base_dir = source_file.parent_path();
        for (std::size_t i = 1; i < leading_dots; ++i) {
            base_dir = base_dir.parent_path();
        }

        std::string rel_module_path = dotted_to_slash(rest);
        std::vector<std::filesystem::path> tries;
        if (!rel_module_path.empty()) {
            tries.push_back(base_dir / (rel_module_path + ".py"));
            tries.push_back(base_dir / rel_module_path / "__init__.py");
        } else {
            tries.push_back(base_dir / "__init__.py");
        }

        for (const auto& candidate : tries) {
            if (const auto* rf = find_file(candidate)) {
                return {CandidateTarget{
                    .target_symbol_id = std::nullopt,
                    .target_symbol_key = std::nullopt,
                    .target_file_path = rf->relative_path.generic_string(),
                    .rank = 1,
                    .confidence = 1.0,
                    .reason = "import",
                }};
            }
        }
    } else {
        // Absolute import or sibling import in same directory
        std::string module_path = dotted_to_slash(clean);
        std::vector<std::filesystem::path> tries;

        // Try relative to source file directory first (Python relative sibling)
        tries.push_back(source_file.parent_path() / (module_path + ".py"));
        tries.push_back(source_file.parent_path() / module_path / "__init__.py");

        // Try from workspace root
        tries.push_back(std::filesystem::path(module_path + ".py"));
        tries.push_back(std::filesystem::path(module_path) / "__init__.py");

        // Try under src/
        tries.push_back(std::filesystem::path("src") / (module_path + ".py"));
        tries.push_back(std::filesystem::path("src") / module_path / "__init__.py");

        for (const auto& candidate : tries) {
            if (const auto* rf = find_file(candidate)) {
                return {CandidateTarget{
                    .target_symbol_id = std::nullopt,
                    .target_symbol_key = std::nullopt,
                    .target_file_path = rf->relative_path.generic_string(),
                    .rank = 1,
                    .confidence = 1.0,
                    .reason = "import",
                }};
            }
        }
    }

    return {CandidateTarget{
        .target_symbol_id = std::nullopt,
        .target_symbol_key = std::nullopt,
        .target_file_path = std::nullopt,
        .rank = 0,
        .confidence = 0.0,
        .reason = "external_or_missing",
    }};
}

std::vector<CandidateTarget>
DependencyResolver::resolve_typescript(const std::filesystem::path& source_file,
                                       std::string_view raw_name) const {
    std::string clean = clean_ts_specifier(raw_name);
    if (clean.empty()) {
        return {};
    }

    auto try_file_variations = [this](const std::filesystem::path& base) -> const RegisteredFile* {
        std::vector<std::filesystem::path> candidates = {
            base,
            base.string() + ".ts",
            base.string() + ".tsx",
            base.string() + ".d.ts",
            base.string() + ".js",
            base.string() + ".jsx",
            base / "index.ts",
            base / "index.tsx",
            base / "index.js",
            base / "index.jsx",
        };
        for (const auto& cand : candidates) {
            if (const auto* rf = find_file(cand)) {
                return rf;
            }
        }
        return nullptr;
    };

    // 1. Relative import (starts with . or ..)
    if (clean.starts_with("./") || clean.starts_with("../")) {
        std::filesystem::path resolved_base = source_file.parent_path() / clean;
        if (const auto* rf = try_file_variations(resolved_base)) {
            return {CandidateTarget{
                .target_symbol_id = std::nullopt,
                .target_symbol_key = std::nullopt,
                .target_file_path = rf->relative_path.generic_string(),
                .rank = 1,
                .confidence = 1.0,
                .reason = "import",
            }};
        }
    }

    // 2. Tsconfig path mappings
    for (const auto& [pattern, targets] : ts_paths_) {
        if (pattern.ends_with('*')) {
            std::string prefix = pattern.substr(0, pattern.size() - 1);
            if (clean.starts_with(prefix)) {
                std::string matched_suffix = clean.substr(prefix.size());
                for (const auto& target_template : targets) {
                    std::string substituted = target_template;
                    auto star_pos = substituted.find('*');
                    if (star_pos != std::string::npos) {
                        substituted.replace(star_pos, 1, matched_suffix);
                    }
                    std::filesystem::path cand_base = ts_base_url_.empty()
                                                          ? std::filesystem::path(substituted)
                                                          : (ts_base_url_ / substituted);
                    if (const auto* rf = try_file_variations(cand_base)) {
                        return {CandidateTarget{
                            .target_symbol_id = std::nullopt,
                            .target_symbol_key = std::nullopt,
                            .target_file_path = rf->relative_path.generic_string(),
                            .rank = 1,
                            .confidence = 1.0,
                            .reason = "import",
                        }};
                    }
                }
            }
        } else if (pattern == clean) {
            for (const auto& target : targets) {
                std::filesystem::path cand_base =
                    ts_base_url_.empty() ? std::filesystem::path(target) : (ts_base_url_ / target);
                if (const auto* rf = try_file_variations(cand_base)) {
                    return {CandidateTarget{
                        .target_symbol_id = std::nullopt,
                        .target_symbol_key = std::nullopt,
                        .target_file_path = rf->relative_path.generic_string(),
                        .rank = 1,
                        .confidence = 1.0,
                        .reason = "import",
                    }};
                }
            }
        }
    }

    // 3. Fallback: baseUrl relative import
    if (!ts_base_url_.empty()) {
        std::filesystem::path cand_base = ts_base_url_ / clean;
        if (const auto* rf = try_file_variations(cand_base)) {
            return {CandidateTarget{
                .target_symbol_id = std::nullopt,
                .target_symbol_key = std::nullopt,
                .target_file_path = rf->relative_path.generic_string(),
                .rank = 1,
                .confidence = 1.0,
                .reason = "import",
            }};
        }
    }

    // External npm package
    return {CandidateTarget{
        .target_symbol_id = std::nullopt,
        .target_symbol_key = std::nullopt,
        .target_file_path = std::nullopt,
        .rank = 0,
        .confidence = 0.0,
        .reason = "external_or_missing",
    }};
}

std::vector<CandidateTarget>
DependencyResolver::resolve_go(const std::filesystem::path& /*source_file*/,
                               std::string_view raw_name) const {
    std::string_view clean = strip_enclosures(raw_name);
    if (clean.empty()) {
        return {};
    }

    // If starts with Go module prefix
    if (!go_module_.empty() && clean.starts_with(go_module_)) {
        std::string_view subpath = clean.substr(go_module_.size());
        if (subpath.starts_with('/')) {
            subpath.remove_prefix(1);
        }

        std::filesystem::path candidate_dir(subpath);
        if (is_directory_in_workspace(candidate_dir)) {
            return {CandidateTarget{
                .target_symbol_id = std::nullopt,
                .target_symbol_key = std::nullopt,
                .target_file_path = candidate_dir.generic_string(),
                .rank = 1,
                .confidence = 1.0,
                .reason = "import",
            }};
        }
    }

    return {CandidateTarget{
        .target_symbol_id = std::nullopt,
        .target_symbol_key = std::nullopt,
        .target_file_path = std::nullopt,
        .rank = 0,
        .confidence = 0.0,
        .reason = "external_or_missing",
    }};
}

std::vector<CandidateTarget>
DependencyResolver::resolve_java(const std::filesystem::path& /*source_file*/,
                                 std::string_view raw_name) const {
    std::string_view clean = strip_enclosures(raw_name);
    if (clean.empty()) {
        return {};
    }

    // Wildcard import (com.example.model.*)
    if (clean.ends_with(".*")) {
        std::string_view pkg = clean.substr(0, clean.size() - 2);
        std::string dir_path = dotted_to_slash(pkg);

        std::vector<std::filesystem::path> tries = {
            std::filesystem::path(dir_path),
            std::filesystem::path("src") / dir_path,
        };
        for (const auto& t : tries) {
            if (is_directory_in_workspace(t)) {
                return {CandidateTarget{
                    .target_symbol_id = std::nullopt,
                    .target_symbol_key = std::nullopt,
                    .target_file_path = t.generic_string(),
                    .rank = 1,
                    .confidence = 1.0,
                    .reason = "import",
                }};
            }
        }
    } else {
        // Specific class import (com.example.model.User)
        std::string file_path = dotted_to_slash(clean) + ".java";
        std::vector<std::filesystem::path> tries = {
            std::filesystem::path(file_path),
            std::filesystem::path("src") / file_path,
        };
        for (const auto& t : tries) {
            if (const auto* rf = find_file(t)) {
                return {CandidateTarget{
                    .target_symbol_id = std::nullopt,
                    .target_symbol_key = std::nullopt,
                    .target_file_path = rf->relative_path.generic_string(),
                    .rank = 1,
                    .confidence = 1.0,
                    .reason = "import",
                }};
            }
        }
    }

    return {CandidateTarget{
        .target_symbol_id = std::nullopt,
        .target_symbol_key = std::nullopt,
        .target_file_path = std::nullopt,
        .rank = 0,
        .confidence = 0.0,
        .reason = "external_or_missing",
    }};
}

std::vector<CandidateTarget>
DependencyResolver::resolve_shell(const std::filesystem::path& source_file,
                                  std::string_view raw_name) const {
    std::string_view clean = strip_enclosures(raw_name);

    if (has_shell_dynamic_expansion(clean)) {
        return {CandidateTarget{
            .target_symbol_id = std::nullopt,
            .target_symbol_key = std::nullopt,
            .target_file_path = std::nullopt,
            .rank = 0,
            .confidence = 0.0,
            .reason = "dynamic_expansion",
        }};
    }

    std::filesystem::path candidate = clean;
    if (candidate.is_relative()) {
        std::filesystem::path from_source =
            (source_file.parent_path() / candidate).lexically_normal();
        if (const auto* rf = find_file(from_source)) {
            return {CandidateTarget{
                .target_symbol_id = std::nullopt,
                .target_symbol_key = std::nullopt,
                .target_file_path = rf->relative_path.generic_string(),
                .rank = 1,
                .confidence = 1.0,
                .reason = "source",
            }};
        }
    }

    if (const auto* rf = find_file(candidate)) {
        return {CandidateTarget{
            .target_symbol_id = std::nullopt,
            .target_symbol_key = std::nullopt,
            .target_file_path = rf->relative_path.generic_string(),
            .rank = 1,
            .confidence = 1.0,
            .reason = "source",
        }};
    }

    return {CandidateTarget{
        .target_symbol_id = std::nullopt,
        .target_symbol_key = std::nullopt,
        .target_file_path = std::nullopt,
        .rank = 0,
        .confidence = 0.0,
        .reason = "external_or_missing",
    }};
}

} // namespace codelenses::resolver
