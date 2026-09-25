#include "codelenses/filesystem/path.hpp"

#include <algorithm>

namespace codelenses::filesystem {

std::string normalize_separators(std::string_view path) {
    std::string result(path);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

Result<std::filesystem::path> canonicalize_workspace_root(const std::filesystem::path& root) {
    if (root.empty()) {
        return unexpected_result<std::filesystem::path>(ErrorCode::invalid_argument,
                                                        "workspace root path cannot be empty");
    }

    std::error_code ec;
    const auto status = std::filesystem::status(root, ec);
    if (ec || !std::filesystem::exists(status)) {
        return unexpected_result<std::filesystem::path>(
            ErrorCode::not_found, "workspace root does not exist: " + root.string(), root.string());
    }

    if (!std::filesystem::is_directory(status)) {
        return unexpected_result<std::filesystem::path>(
            ErrorCode::invalid_argument,
            "workspace root path is not a directory: " + root.string(), root.string());
    }

    const auto canonical = std::filesystem::canonical(root, ec);
    if (ec) {
        return unexpected_result<std::filesystem::path>(
            ErrorCode::invalid_argument,
            "cannot canonicalize workspace root: " + ec.message(), root.string());
    }

    return canonical;
}

bool is_contained_in(const std::filesystem::path& base, const std::filesystem::path& target) {
    auto base_it = base.begin();
    auto target_it = target.begin();
    while (base_it != base.end() && target_it != target.end()) {
        if (*base_it != *target_it) {
            return false;
        }
        ++base_it;
        ++target_it;
    }
    return base_it == base.end();
}

Result<std::filesystem::path>
resolve_workspace_path(const std::filesystem::path& canonical_root,
                       const std::filesystem::path& rel_or_abs_path) {
    if (rel_or_abs_path.empty() || rel_or_abs_path == ".") {
        return canonical_root;
    }

    std::filesystem::path target;
    if (rel_or_abs_path.is_absolute()) {
        target = rel_or_abs_path;
    } else {
        target = canonical_root / rel_or_abs_path;
    }

    std::error_code ec;
    std::filesystem::path canonical_target;
    if (std::filesystem::exists(target, ec)) {
        canonical_target = std::filesystem::canonical(target, ec);
    } else {
        canonical_target = std::filesystem::weakly_canonical(target, ec);
    }

    if (ec) {
        return unexpected_result<std::filesystem::path>(
            ErrorCode::invalid_argument,
            "cannot resolve path: " + ec.message(), rel_or_abs_path.string());
    }

    if (!is_contained_in(canonical_root, canonical_target)) {
        return unexpected_result<std::filesystem::path>(
            ErrorCode::invalid_argument,
            "path escapes workspace root: " + rel_or_abs_path.string(), rel_or_abs_path.string());
    }

    return canonical_target;
}

std::string to_workspace_relative(const std::filesystem::path& root,
                                  const std::filesystem::path& path) {
    std::error_code ec;
    const auto rel = std::filesystem::relative(path, root, ec);
    if (ec || rel.empty()) {
        return normalize_separators(path.generic_string());
    }
    return normalize_separators(rel.generic_string());
}

} // namespace codelenses::filesystem
