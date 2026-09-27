#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "codelenses/domain/reference.hpp"

namespace codelenses {

class Connection;

class ReferenceRepository {
public:
    explicit ReferenceRepository(Connection& conn);

    int64_t insert(const ReferenceOccurrence& ref);
    void insert_batch(const std::vector<ReferenceOccurrence>& references);
    [[nodiscard]] std::optional<ReferenceOccurrence> get_by_id(int64_t id);
    [[nodiscard]] std::vector<ReferenceOccurrence> list_by_source_file(int64_t source_file_id);
    [[nodiscard]] std::vector<ReferenceOccurrence> list_by_workspace(int64_t workspace_id);
    [[nodiscard]] std::vector<ReferencerResult> find_referencers(int64_t workspace_id,
                                                                 int64_t symbol_id,
                                                                 int64_t limit = 100,
                                                                 int64_t offset = 0);
    [[nodiscard]] std::vector<CallerCalleeResult> find_callers(int64_t symbol_id);
    [[nodiscard]] std::vector<CallerCalleeResult> find_callers(int64_t workspace_id,
                                                               int64_t symbol_id);
    [[nodiscard]] std::vector<CallerCalleeResult> find_callees(int64_t symbol_id);
    [[nodiscard]] std::vector<CallerCalleeResult> find_callees(int64_t workspace_id,
                                                               int64_t symbol_id);
    bool update_resolution(int64_t id, std::optional<int64_t> source_symbol_id,
                           std::optional<int64_t> target_symbol_id, const std::string& resolution,
                           double confidence,
                           const std::optional<std::string>& metadata_json = std::nullopt);
    bool delete_by_file(int64_t file_id);

private:
    Connection& conn_;
};

} // namespace codelenses
