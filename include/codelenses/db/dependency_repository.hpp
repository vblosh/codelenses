#pragma once

#include <cstdint>
#include <vector>

#include "codelenses/domain/dependency.hpp"

namespace codelenses {

class Connection;

class DependencyRepository {
public:
    explicit DependencyRepository(Connection& conn);

    int64_t insert(const FileDependency& dep);
    void insert_batch(const std::vector<FileDependency>& dependencies);
    [[nodiscard]] std::vector<FileDependency> list_by_source_file(int64_t file_id);
    [[nodiscard]] std::vector<FileDependency> list_by_target_file(int64_t file_id);
    [[nodiscard]] std::vector<FileDependency> list_by_workspace(int64_t workspace_id);
    bool update_resolution(int64_t id, std::optional<int64_t> target_file_id,
                           const std::optional<std::string>& resolved_path,
                           const std::string& resolution);
    bool delete_by_file(int64_t file_id);

private:
    Connection& conn_;
};

} // namespace codelenses
