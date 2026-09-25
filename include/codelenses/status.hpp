#pragma once

#include <string_view>

#include "codelenses/result.hpp"

namespace codelenses {

enum class AnalysisStatus {
    unknown,
    pending,
    indexing,
    parsing,
    resolving,
    complete,
    failed,
    cancelled,
    degraded,
    stale,
    unsupported,
};

enum class JobStatus {
    discovered,
    queued,
    running,
    completed,
    failed,
    pending_resolution,
    cancelled,
    degraded,
};

using AnalysisState = AnalysisStatus;
using JobState = JobStatus;

enum class Freshness {
    current,
    stale,
};

[[nodiscard]] std::string_view to_string(AnalysisStatus value) noexcept;
[[nodiscard]] std::string_view to_string(JobStatus value) noexcept;
[[nodiscard]] std::string_view to_string(Freshness value) noexcept;
[[nodiscard]] Result<AnalysisStatus> analysis_status_from_string(std::string_view value);
[[nodiscard]] Result<JobStatus> job_status_from_string(std::string_view value);
[[nodiscard]] Result<Freshness> freshness_from_string(std::string_view value);

[[nodiscard]] inline Result<AnalysisStatus> parse_analysis_status(std::string_view value) {
    return analysis_status_from_string(value);
}
[[nodiscard]] inline Result<JobStatus> parse_job_status(std::string_view value) {
    return job_status_from_string(value);
}
[[nodiscard]] inline Result<Freshness> parse_freshness(std::string_view value) {
    return freshness_from_string(value);
}

} // namespace codelenses
