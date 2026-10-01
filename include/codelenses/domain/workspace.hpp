#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace codelenses {

enum class WorkspaceStatus {
    idle,
    indexing,
    ready,
    error,
};

[[nodiscard]] inline std::string to_string(WorkspaceStatus status) {
    switch (status) {
    case WorkspaceStatus::idle:
        return "idle";
    case WorkspaceStatus::indexing:
        return "indexing";
    case WorkspaceStatus::ready:
        return "ready";
    case WorkspaceStatus::error:
        return "error";
    }
    return "idle";
}

[[nodiscard]] inline WorkspaceStatus workspace_status_from_string(std::string_view str) {
    if (str == "indexing") {
        return WorkspaceStatus::indexing;
    }
    if (str == "ready") {
        return WorkspaceStatus::ready;
    }
    if (str == "error") {
        return WorkspaceStatus::error;
    }
    return WorkspaceStatus::idle;
}

struct Workspace {
    int64_t id = 0;
    std::string root_path = "";
    std::string name = "";
    std::vector<std::string> include_patterns = {};
    std::vector<std::string> exclude_patterns = {};
    std::vector<std::string> default_ignores = {};
    std::optional<std::string> compile_commands_path = std::nullopt;
    std::optional<std::string> default_compile_command = std::nullopt;
    std::string created_at = "";
    std::string updated_at = "";
    int64_t revision = 0;
    WorkspaceStatus status = WorkspaceStatus::idle;
    std::optional<std::string> last_error = std::nullopt;

    bool operator==(const Workspace& other) const = default;
};

} // namespace codelenses
