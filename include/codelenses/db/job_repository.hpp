#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "codelenses/domain/job.hpp"

namespace codelenses {

class Connection;

class JobRepository {
public:
    explicit JobRepository(Connection& conn);

    int64_t create(const IndexJob& job);
    [[nodiscard]] std::optional<IndexJob> get_by_id(int64_t id);
    bool update_status(int64_t id, const std::string& status,
                       const std::optional<std::string>& error_message = std::nullopt);
    bool update_progress(int64_t id, int64_t processed, int64_t skipped, int64_t errors,
                         int64_t warnings);
    bool finish_job(int64_t id, const std::string& status, const std::optional<int64_t>& revision,
                    const std::optional<std::string>& error_message = std::nullopt);
    [[nodiscard]] std::vector<IndexJob> list_by_workspace(int64_t workspace_id, int64_t limit = 20);

private:
    Connection& conn_;
};

} // namespace codelenses
