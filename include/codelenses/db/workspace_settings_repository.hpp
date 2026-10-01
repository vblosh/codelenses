#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "codelenses/domain/workspace_index_settings.hpp"

namespace codelenses {

class Connection;

// Optional workspace indexing settings and their durable source roots.
class WorkspaceSettingsRepository {
public:
    explicit WorkspaceSettingsRepository(Connection& conn);

    int64_t create_profile(const WorkspaceIndexSettings& profile);
    [[nodiscard]] std::vector<WorkspaceSourceRoot> list_source_roots(int64_t profile_id);
    [[nodiscard]] std::optional<WorkspaceIndexSettings> get_profile(int64_t id);
    [[nodiscard]] std::optional<WorkspaceIndexSettings>
    get_profile_by_workspace(int64_t workspace_id);
    [[nodiscard]] std::vector<WorkspaceIndexSettings> list_profiles();
    bool update_profile(const WorkspaceIndexSettings& profile);
    bool delete_profile(int64_t id);

private:
    Connection& conn_;
};

} // namespace codelenses
