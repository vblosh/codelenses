#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

#include "codelenses/adapters/registry.hpp"
#include "codelenses/db/database.hpp"
#include "codelenses/domain/job.hpp"
#include "codelenses/filesystem/discovery.hpp"
#include "codelenses/result.hpp"

namespace codelenses::index {

struct IndexerOptions {
    std::size_t worker_threads{0}; // 0 = hardware concurrency
    std::size_t queue_capacity{128};
    std::size_t queue_max_bytes{64U * 1024U * 1024U};
    std::size_t max_file_size_bytes{32U * 1024U * 1024U};
    adapters::AdapterRegistry* registry{nullptr}; // defaults to default_adapter_registry()
};

struct IndexResult {
    int64_t job_id{0};
    std::string status{"completed"};
    int64_t files_total{0};
    int64_t files_processed{0};
    int64_t files_skipped{0};
    int64_t error_count{0};
    int64_t warning_count{0};
    std::optional<int64_t> workspace_revision{std::nullopt};
    std::optional<std::string> error_message{std::nullopt};
};

class IndexingPipeline {
public:
    explicit IndexingPipeline(Database& db, IndexerOptions options = {});
    ~IndexingPipeline();

    IndexingPipeline(const IndexingPipeline&) = delete;
    IndexingPipeline& operator=(const IndexingPipeline&) = delete;

    // Executes an indexing job on a workspace. Returns completed job result or error.
    [[nodiscard]] Result<IndexResult> run_indexing(int64_t workspace_id,
                                                   const std::string& job_type = "incremental",
                                                   bool force_full = false,
                                                   std::stop_token stop = {});

    // Requests cooperative cancellation of an active job by workspace ID.
    [[nodiscard]] Result<void> cancel_workspace(int64_t workspace_id);

    // Requests cooperative cancellation by job ID.
    [[nodiscard]] Result<void> cancel_job(int64_t job_id);

    // Checks if an indexing job is currently active for a workspace.
    [[nodiscard]] bool is_indexing(int64_t workspace_id) const;

private:
    Database& db_;
    IndexerOptions options_;
    adapters::AdapterRegistry& registry_;

    mutable std::mutex jobs_mutex_;
    std::unordered_map<int64_t, std::shared_ptr<std::stop_source>> active_workspace_jobs_;
    std::unordered_map<int64_t, int64_t> job_to_workspace_map_;
};

} // namespace codelenses::index
