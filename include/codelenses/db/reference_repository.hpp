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
    [[nodiscard]] std::vector<ReferencerResult> find_referencers(int64_t workspace_id,
                                                                 int64_t symbol_id,
                                                                 int64_t limit = 100,
                                                                 int64_t offset = 0);
    [[nodiscard]] std::vector<CallerCalleeResult> find_callers(int64_t symbol_id);
    [[nodiscard]] std::vector<CallerCalleeResult> find_callees(int64_t symbol_id);
    bool delete_by_file(int64_t file_id);

private:
    Connection& conn_;
};

} // namespace codelenses
