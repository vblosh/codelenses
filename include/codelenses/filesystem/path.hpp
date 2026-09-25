#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "codelenses/result.hpp"

namespace codelenses::filesystem {

// Canonicalizes workspace root: verifies it exists and is a directory,
// resolves symlinks, and normalizes to generic path.
[[nodiscard]] Result<std::filesystem::path>
canonicalize_workspace_root(const std::filesystem::path& root);

// Checks if target path is strictly contained within base directory.
// Both paths must be canonical or comparable.
[[nodiscard]] bool is_contained_in(const std::filesystem::path& base,
                                   const std::filesystem::path& target);

// Resolves a relative or absolute path against workspace root and verifies
// that the resulting path remains safely contained within workspace root.
// Prevents path traversal and escaping symlinks.
[[nodiscard]] Result<std::filesystem::path>
resolve_workspace_path(const std::filesystem::path& canonical_root,
                       const std::filesystem::path& rel_or_abs_path);

// Converts a path to a normalized workspace-relative path with '/' separators.
[[nodiscard]] std::string to_workspace_relative(const std::filesystem::path& root,
                                                const std::filesystem::path& path);

// Normalizes path separators to '/'
[[nodiscard]] std::string normalize_separators(std::string_view path);

} // namespace codelenses::filesystem
