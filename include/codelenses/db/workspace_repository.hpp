#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "codelenses/domain/workspace.hpp"

namespace codelenses {

class Connection;

class WorkspaceRepository {
public:
    explicit WorkspaceRepository(Connection& conn);

    int64_t create(const Workspace& ws);
    [[nodiscard]] std::optional<Workspace> get_by_id(int64_t id);
    [[nodiscard]] std::optional<Workspace> get_by_root_path(const std::string& root_path);
    [[nodiscard]] std::vector<Workspace> list_all();
    void link(int64_t workspace_id, int64_t target_id);
    void unlink(int64_t workspace_id, int64_t target_id);
    [[nodiscard]] bool is_linked(int64_t workspace_id, int64_t target_id);
    [[nodiscard]] std::vector<int64_t> linked_ids(int64_t workspace_id);
    [[nodiscard]] std::vector<int64_t> consumer_ids(int64_t target_id);
    bool update(const Workspace& ws);
    bool update_status(int64_t id, WorkspaceStatus status,
                       const std::optional<std::string>& last_error = std::nullopt);
    int64_t increment_revision(int64_t id);
    bool delete_by_id(int64_t id);

private:
    Connection& conn_;
};

} // namespace codelenses
