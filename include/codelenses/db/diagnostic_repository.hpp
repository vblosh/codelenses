#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "codelenses/domain/diagnostic.hpp"

namespace codelenses {

class Connection;

class DiagnosticRepository {
public:
    explicit DiagnosticRepository(Connection& conn);

    int64_t insert(const Diagnostic& diag);
    void insert_batch(const std::vector<Diagnostic>& diagnostics);
    [[nodiscard]] std::vector<Diagnostic>
    list_by_workspace(int64_t workspace_id,
                      const std::optional<std::string>& severity = std::nullopt,
                      int64_t limit = 100, int64_t offset = 0);
    [[nodiscard]] DiagnosticCounts count_by_workspace(int64_t workspace_id);
    [[nodiscard]] std::vector<Diagnostic> list_by_file(int64_t file_id);
    [[nodiscard]] std::vector<Diagnostic> list_by_job(int64_t job_id);
    bool delete_by_file(int64_t file_id);

private:
    Connection& conn_;
};

} // namespace codelenses
