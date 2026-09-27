#include "codelenses/filesystem/discovery.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>

#include "codelenses/filesystem/path.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace codelenses::filesystem {
namespace {

std::string_view trim(std::string_view str) {
    while (!str.empty() && (str.front() == ' ' || str.front() == '\t' || str.front() == '\r')) {
        str.remove_prefix(1);
    }
    while (!str.empty() && (str.back() == ' ' || str.back() == '\t' || str.back() == '\r')) {
        str.remove_suffix(1);
    }
    return str;
}

std::string glob_to_regex(std::string_view pattern, bool anchored) {
    std::string regex_str;
    if (anchored || pattern.starts_with('/')) {
        regex_str = "^";
        if (pattern.starts_with('/')) {
            pattern.remove_prefix(1);
        }
    } else {
        regex_str = "(^|.*/)";
    }

    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '*' && i + 1 < pattern.size() && pattern[i + 1] == '*') {
            // Double star '**'
            ++i;
            if (i + 1 < pattern.size() && pattern[i + 1] == '/') {
                ++i;
                regex_str += "(.*/)?";
            } else {
                regex_str += ".*";
            }
        } else if (c == '*') {
            regex_str += "[^/]*";
        } else if (c == '?') {
            regex_str += "[^/]";
        } else if (c == '.' || c == '+' || c == '(' || c == ')' || c == '{' || c == '}' ||
                   c == '^' || c == '$' || c == '|' || c == '[' || c == ']') {
            regex_str += '\\';
            regex_str += c;
        } else {
            regex_str += c;
        }
    }
    regex_str += "$";
    return regex_str;
}

struct IgnoreRule {
    std::regex regex;
    std::filesystem::path base_dir;
    bool is_negation{false};
    bool directory_only{false};
};

std::optional<IgnoreRule> parse_ignore_line(std::string_view line_view,
                                            const std::filesystem::path& base_dir) {
    std::string line(trim(line_view));
    if (line.empty() || line.front() == '#') {
        return std::nullopt;
    }

    bool is_negation = false;
    if (line.front() == '!') {
        is_negation = true;
        line.erase(0, 1);
        line = trim(line);
        if (line.empty())
            return std::nullopt;
    }

    bool directory_only = false;
    if (line.back() == '/') {
        directory_only = true;
        line.pop_back();
    }

    bool anchored = false;
    if (line.front() == '/') {
        anchored = true;
        line.erase(0, 1);
    } else if (line.find('/') != std::string::npos) {
        anchored = true;
    }

    std::string regex_str = glob_to_regex(line, anchored);

    try {
        return IgnoreRule{
            .regex = std::regex(regex_str, std::regex::ECMAScript),
            .base_dir = base_dir,
            .is_negation = is_negation,
            .directory_only = directory_only,
        };
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<IgnoreRule> load_ignore_file(const std::filesystem::path& file_path,
                                         const std::filesystem::path& base_dir) {
    std::vector<IgnoreRule> rules;
    std::ifstream in(file_path);
    if (!in.is_open())
        return rules;

    std::string line;
    while (std::getline(in, line)) {
        auto rule = parse_ignore_line(line, base_dir);
        if (rule) {
            rules.push_back(std::move(*rule));
        }
    }
    return rules;
}

bool is_path_ignored(const std::string& path_str, bool is_dir,
                     const std::vector<IgnoreRule>& rules) {
    bool ignored = false;
    for (const auto& rule : rules) {
        if (rule.directory_only && !is_dir) {
            continue;
        }

        std::string test_str;
        if (rule.base_dir.empty()) {
            test_str = path_str;
        } else {
            const std::string base_str = rule.base_dir.generic_string();
            if (path_str.starts_with(base_str)) {
                if (path_str.size() == base_str.size()) {
                    test_str = "";
                } else if (path_str[base_str.size()] == '/') {
                    test_str = path_str.substr(base_str.size() + 1);
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }

        if (std::regex_match(test_str, rule.regex)) {
            ignored = !rule.is_negation;
        }
    }
    return ignored;
}

bool quick_check_binary(const std::filesystem::path& abs_path) {
    int fd = ::open(abs_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    char buf[1024];
    auto n = ::read(fd, buf, sizeof(buf));
    ::close(fd);

    if (n <= 0)
        return false;

    for (ssize_t i = 0; i < n; ++i) {
        if (buf[i] == '\0') {
            return true;
        }
    }
    return false;
}

} // namespace

FileDiscovery::FileDiscovery(DiscoveryOptions options) : options_(std::move(options)) {}

bool FileDiscovery::match_glob(std::string_view pattern, std::string_view path) {
    if (pattern.empty())
        return path.empty();
    if (pattern == "**" || pattern == "*")
        return true;

    try {
        std::string regex_str = glob_to_regex(pattern, pattern.find('/') != std::string_view::npos);
        std::regex re(regex_str, std::regex::ECMAScript);
        return std::regex_match(std::string(path), re);
    } catch (...) {
        return false;
    }
}

std::optional<Language> FileDiscovery::parse_shebang_line(std::string_view line) {
    line = trim(line);
    if (!line.starts_with("#!")) {
        return std::nullopt;
    }

    line.remove_prefix(2);
    line = trim(line);

    // Extract command and optional args
    const auto space_pos = line.find_first_of(" \t");
    std::string_view cmd = (space_pos != std::string_view::npos) ? line.substr(0, space_pos) : line;
    std::string_view rest = (space_pos != std::string_view::npos) ? line.substr(space_pos + 1) : "";

    // Check /usr/bin/env
    if (cmd == "/usr/bin/env" || cmd == "/bin/env" || cmd == "env") {
        rest = trim(rest);
        const auto arg_space = rest.find_first_of(" \t");
        const std::string_view env_arg =
            (arg_space != std::string_view::npos) ? rest.substr(0, arg_space) : rest;
        if (env_arg == "bash")
            return Language::bash;
        if (env_arg == "sh" || env_arg == "dash")
            return Language::shell;
        if (env_arg.starts_with("python"))
            return Language::python;
        if (env_arg == "node")
            return Language::javascript;
        return std::nullopt;
    }

    // Check direct path like /bin/bash or /bin/sh
    const auto slash_pos = cmd.find_last_of('/');
    const std::string_view basename =
        (slash_pos != std::string_view::npos) ? cmd.substr(slash_pos + 1) : cmd;
    if (basename == "bash")
        return Language::bash;
    if (basename == "sh" || basename == "dash")
        return Language::shell;
    if (basename.starts_with("python"))
        return Language::python;
    if (basename == "node")
        return Language::javascript;

    return std::nullopt;
}

std::optional<Language>
FileDiscovery::detect_shebang_language(const std::filesystem::path& file_path) {
    std::ifstream in(file_path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }

    char buf[256];
    in.read(buf, sizeof(buf));
    const auto bytes_read = in.gcount();
    if (bytes_read < 3) {
        return std::nullopt;
    }

    std::string_view view(buf, static_cast<std::size_t>(bytes_read));
    const auto nl_pos = view.find('\n');
    if (nl_pos != std::string_view::npos) {
        view = view.substr(0, nl_pos);
    }

    return parse_shebang_line(view);
}

Result<std::vector<DiscoveredFile>>
FileDiscovery::discover(const std::filesystem::path& workspace_root) {
    if (options_.max_discovered_files == 0) {
        return unexpected_result<std::vector<DiscoveredFile>>(
            ErrorCode::invalid_argument, "maximum discovered files must be positive");
    }

    auto root_result = canonicalize_workspace_root(workspace_root);
    if (!root_result) {
        return std::unexpected(root_result.error());
    }
    const auto canonical_root = *root_result;

    std::vector<DiscoveredFile> discovered;
    bool discovery_limit_exceeded = false;
    std::set<std::pair<dev_t, ino_t>> visited_dirs;
    std::vector<IgnoreRule> ignore_rules;
    std::error_code ec;

    struct stat root_st{};
    if (::stat(canonical_root.c_str(), &root_st) == 0) {
        visited_dirs.insert({root_st.st_dev, root_st.st_ino});
    }

    auto is_default_ignored = [this](std::string_view name) {
        for (const auto& ignore : options_.default_ignores) {
            if (name == ignore)
                return true;
        }
        return false;
    };

    auto matches_include = [this](std::string_view rel_path) {
        if (options_.include_patterns.empty())
            return true;
        for (const auto& pattern : options_.include_patterns) {
            if (match_glob(pattern, rel_path))
                return true;
        }
        return false;
    };

    auto matches_exclude = [this](std::string_view rel_path) {
        for (const auto& pattern : options_.exclude_patterns) {
            if (match_glob(pattern, rel_path))
                return true;
        }
        return false;
    };

    auto resolve_language = [this](const std::filesystem::path& rel_path,
                                   const std::filesystem::path& abs_path) -> Language {
        const std::string rel_str = normalize_separators(rel_path.generic_string());

        // 1. Path overrides
        if (const auto it = options_.path_overrides.find(rel_str);
            it != options_.path_overrides.end()) {
            return it->second;
        }

        // 2. Extension overrides
        const std::string ext = rel_path.extension().string();
        if (!ext.empty()) {
            if (const auto it = options_.extension_overrides.find(ext);
                it != options_.extension_overrides.end()) {
                return it->second;
            }
        }

        // 3. Ambiguous header handling (.h)
        if (ext == ".h") {
            if (options_.ambiguous_header_mode.has_value()) {
                return *options_.ambiguous_header_mode;
            }
            return Language::c;
        }

        // 4. Standard extension lookup
        const auto lang = language_from_path(rel_path);
        if (lang != Language::unknown) {
            return lang;
        }

        // 5. Extensionless / unknown shebang detection
        if (ext.empty() || lang == Language::unknown) {
            const auto shebang_lang = detect_shebang_language(abs_path);
            if (shebang_lang.has_value()) {
                return *shebang_lang;
            }
            if (ext.empty() && options_.allow_extensionless_headers &&
                options_.extensionless_language.has_value()) {
                return *options_.extensionless_language;
            }
        }

        return Language::unknown;
    };

    std::optional<Error> scan_error;

    auto traverse = [&](auto& self, const std::filesystem::path& current_dir,
                        const std::filesystem::path& current_rel, std::size_t depth) -> void {
        if (discovery_limit_exceeded || scan_error.has_value()) {
            return;
        }

        if (depth > options_.max_discovery_depth) {
            scan_error = make_error(ErrorCode::out_of_range,
                                    "directory scan exceeded maximum discovery depth (" +
                                        std::to_string(options_.max_discovery_depth) + ")",
                                    current_dir.string());
            return;
        }

        std::size_t rules_before_dir = ignore_rules.size();
        if (options_.respect_ignore_files) {
            const auto gitignore = current_dir / ".gitignore";
            if (std::filesystem::exists(gitignore, ec)) {
                auto new_rules = load_ignore_file(gitignore, current_rel);
                ignore_rules.insert(ignore_rules.end(), std::make_move_iterator(new_rules.begin()),
                                    std::make_move_iterator(new_rules.end()));
            }
            const auto codelensignore = current_dir / ".codelensignore";
            if (std::filesystem::exists(codelensignore, ec)) {
                auto new_rules = load_ignore_file(codelensignore, current_rel);
                ignore_rules.insert(ignore_rules.end(), std::make_move_iterator(new_rules.begin()),
                                    std::make_move_iterator(new_rules.end()));
            }
        }

        std::error_code iter_ec;
        auto it = std::filesystem::directory_iterator(
            current_dir, std::filesystem::directory_options::none, iter_ec);
        if (iter_ec) {
            scan_error =
                make_error(ErrorCode::failed, "failed to open directory: " + iter_ec.message(),
                           current_dir.string());
            return;
        }

        auto end = std::filesystem::directory_iterator();
        while (it != end && !scan_error.has_value() && !discovery_limit_exceeded) {
            auto process_entry = [&]() {
                const auto& entry = *it;
                const auto filename = entry.path().filename().string();
                if (is_default_ignored(filename)) {
                    return;
                }

                const auto rel_path =
                    current_rel.empty() ? std::filesystem::path(filename) : current_rel / filename;
                const std::string rel_str = normalize_separators(rel_path.generic_string());

                bool is_symlink = entry.is_symlink(ec);
                std::filesystem::path target_path = entry.path();

                if (is_symlink) {
                    if (options_.symlink_policy == SymlinkPolicy::ignore) {
                        return;
                    }

                    auto canonical_target = std::filesystem::canonical(entry.path(), ec);
                    if (ec) {
                        // Broken symlink
                        return;
                    }

                    // Verify symlink does not escape workspace root
                    if (!is_contained_in(canonical_root, canonical_target)) {
                        return;
                    }

                    target_path = canonical_target;
                }

                const auto status = std::filesystem::status(target_path, ec);
                if (ec)
                    return;

                if (std::filesystem::is_directory(status)) {
                    if (matches_exclude(rel_str)) {
                        return;
                    }
                    if (options_.respect_ignore_files &&
                        is_path_ignored(rel_str, true, ignore_rules)) {
                        return;
                    }

                    struct stat st{};
                    if (::stat(target_path.c_str(), &st) == 0) {
                        std::pair<dev_t, ino_t> dir_id{st.st_dev, st.st_ino};
                        if (visited_dirs.contains(dir_id)) {
                            return; // Cyclic symlink or already visited
                        }
                        visited_dirs.insert(dir_id);
                    }

                    self(self, target_path, rel_path, depth + 1);
                } else if (std::filesystem::is_regular_file(status)) {
                    if (matches_exclude(rel_str)) {
                        return;
                    }
                    if (options_.respect_ignore_files &&
                        is_path_ignored(rel_str, false, ignore_rules)) {
                        return;
                    }
                    if (!matches_include(rel_str)) {
                        return;
                    }

                    const auto fsize = std::filesystem::file_size(target_path, ec);
                    const auto observed_size = ec ? uint64_t{0} : static_cast<uint64_t>(fsize);
                    ec.clear();

                    const auto lang = resolve_language(rel_path, target_path);
                    const bool is_binary = quick_check_binary(target_path);

                    if (discovered.size() >= options_.max_discovered_files) {
                        discovery_limit_exceeded = true;
                        return;
                    }

                    discovered.push_back(DiscoveredFile{
                        .relative_path = rel_str,
                        .absolute_path = is_symlink ? target_path : entry.path(),
                        .language = is_binary ? Language::unknown : lang,
                        .file_size = observed_size,
                        .is_binary = is_binary,
                    });
                }
            };

            process_entry();
            it.increment(iter_ec);
            if (iter_ec) {
                scan_error = make_error(ErrorCode::failed,
                                        "failed to iterate directory: " + iter_ec.message(),
                                        current_dir.string());
                break;
            }
        }

        ignore_rules.resize(rules_before_dir);
    };

    traverse(traverse, canonical_root, "", 0);

    if (scan_error.has_value()) {
        return std::unexpected(*scan_error);
    }

    if (discovery_limit_exceeded) {
        return unexpected_result<std::vector<DiscoveredFile>>(
            ErrorCode::out_of_range,
            "workspace contains more files than the configured discovery limit",
            canonical_root.string());
    }

    std::ranges::sort(discovered, [](const DiscoveredFile& a, const DiscoveredFile& b) {
        return a.relative_path < b.relative_path;
    });

    return discovered;
}

} // namespace codelenses::filesystem
