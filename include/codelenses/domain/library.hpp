#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace codelenses {

// A library index is an explicitly configured, read-only set of source roots
// (e.g. a C++ toolchain include tree, GOROOT/src, extracted JDK sources)
// attached to one or more project workspaces.
struct LibraryProfile {
    int64_t id = 0;
    int64_t workspace_id = 0; // backing library-kind workspace
    std::string name = "";
    std::string language = ""; // primary language: c, cpp, go, python, java, ...
    std::string provider = ""; // e.g. "toolchain", "goroot", "jdk", "typescript-lib"
    std::optional<std::string> sdk_version = std::nullopt;
    std::optional<std::string> target_environment = std::nullopt;
    std::optional<std::string> language_standard = std::nullopt; // e.g. "c17", "c++20"
    std::optional<std::string> target_framework = std::nullopt; // e.g. "net8.0" (C# identity)
    std::optional<std::string> sysroot = std::nullopt;
    std::vector<std::string> source_roots = {}; // authorized source directories
    std::vector<std::string> default_include_roots = {}; // ordered default search roots
    std::vector<std::string> defines = {}; // explicit defines/undefines (-D, -U)
    std::vector<std::string> include_patterns = {};
    std::vector<std::string> exclude_patterns = {};
    std::string fingerprint = ""; // configuration fingerprint for caching/invalidation
    std::string created_at = "";
    std::string updated_at = "";

    bool operator==(const LibraryProfile& other) const = default;
};

struct LibrarySourceRoot {
    int64_t id = 0;
    int64_t profile_id = 0;
    int64_t ordinal = 0;
    std::string path;

    bool operator==(const LibrarySourceRoot& other) const = default;
};

struct WorkspaceLibrary {
    int64_t workspace_id = 0;    // consuming project workspace
    int64_t profile_id = 0;      // attached library profile
    std::string attached_at = "";

    bool operator==(const WorkspaceLibrary& other) const = default;
};

// Computes a deterministic configuration fingerprint for cache invalidation.
[[nodiscard]] std::string compute_library_fingerprint(const LibraryProfile& profile);

} // namespace codelenses
