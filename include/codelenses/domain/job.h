#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace codelenses {

struct IndexJob {
    int64_t id = 0;
    int64_t workspace_id = 0;
    std::string job_type = "";     // full, incremental, resolve, rebuild_search
    std::string status = "queued"; // queued, running, cancelling, cancelled, completed, failed
    std::optional<std::string> requested_mode = std::nullopt;
    std::string queued_at = "";
    std::optional<std::string> started_at = std::nullopt;
    std::optional<std::string> finished_at = std::nullopt;
    int64_t files_total = 0;
    int64_t files_processed = 0;
    int64_t files_skipped = 0;
    int64_t error_count = 0;
    int64_t warning_count = 0;
    std::optional<int64_t> workspace_revision = std::nullopt;
    std::optional<std::string> error_message = std::nullopt;

    bool operator==(const IndexJob& other) const = default;
};

} // namespace codelenses
