#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "codelenses/language.hpp"
#include "codelenses/result.hpp"

namespace codelenses::filesystem {

enum class SymlinkPolicy {
    follow_safe, // Follow symlinks if they stay within workspace root and don't loop
    ignore,      // Do not follow symlinks
};

struct DiscoveredFile {
    std::string relative_path; // normalized with '/'
    std::filesystem::path absolute_path;
    Language language{Language::unknown};
    uint64_t file_size{0};
    bool is_binary{false};

    friend bool operator==(const DiscoveredFile&, const DiscoveredFile&) = default;
};

struct DiscoveryOptions {
    std::vector<std::string> include_patterns{}; // if empty, include all
    std::vector<std::string> exclude_patterns{};
    std::vector<std::string> default_ignores{
        ".git",   "node_modules", "build",       "dist",   "target",
        "vendor", ".codelens",    ".codelenses", ".cache",
    };
    std::unordered_map<std::string, Language> extension_overrides{};
    std::unordered_map<std::string, Language> path_overrides{};
    std::optional<Language> ambiguous_header_mode{std::nullopt};
    bool respect_ignore_files{true}; // parse .gitignore and .codelensignore
    size_t max_file_size_bytes{32U * 1024U * 1024U};
    size_t max_discovered_files{1000000U};
    size_t max_discovery_depth{64U};
    SymlinkPolicy symlink_policy{SymlinkPolicy::follow_safe};

    friend bool operator==(const DiscoveryOptions&, const DiscoveryOptions&) = default;
};

class FileDiscovery {
public:
    explicit FileDiscovery(DiscoveryOptions options = {});

    // Recursively discovers all files matching inclusion/exclusion rules under workspace_root
    [[nodiscard]] Result<std::vector<DiscoveredFile>>
    discover(const std::filesystem::path& workspace_root);

    // Parses a shebang line (e.g., "#!/bin/bash" or "#!/usr/bin/env python3")
    [[nodiscard]] static std::optional<Language> parse_shebang_line(std::string_view line);

    // Checks whether the first line of an extensionless or unknown file contains a supported
    // shebang
    [[nodiscard]] static std::optional<Language>
    detect_shebang_language(const std::filesystem::path& file_path);

    // Matches a glob pattern against a relative path (supports *, **, ?)
    [[nodiscard]] static bool match_glob(std::string_view pattern, std::string_view path);

    [[nodiscard]] const DiscoveryOptions& options() const noexcept { return options_; }

private:
    DiscoveryOptions options_;
};

} // namespace codelenses::filesystem
