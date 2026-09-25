#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "codelenses/domain/occurrence.hpp"

namespace codelenses {

class Connection;

class OccurrenceRepository {
public:
    explicit OccurrenceRepository(Connection& conn);

    int64_t insert(const Occurrence& occ);
    void insert_batch(const std::vector<Occurrence>& occurrences);
    [[nodiscard]] std::optional<Occurrence> get_by_id(int64_t id);
    [[nodiscard]] std::vector<Occurrence> list_by_file(int64_t file_id);
    [[nodiscard]] std::vector<Occurrence> list_by_symbol(int64_t symbol_id);
    [[nodiscard]] std::vector<Occurrence> find_at_range(int64_t file_id, int64_t start_byte,
                                                        int64_t end_byte);
    bool delete_by_file(int64_t file_id);

private:
    Connection& conn_;
};

} // namespace codelenses
