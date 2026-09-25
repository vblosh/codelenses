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
    bool delete_by_file(int64_t file_id);

private:
    Connection& conn_;
};

} // namespace codelenses
