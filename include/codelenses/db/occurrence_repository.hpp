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
    [[nodiscard]] std::vector<Occurrence> list_by_workspace(int64_t workspace_id);
    [[nodiscard]] std::vector<Occurrence> list_by_symbol(int64_t symbol_id);
    [[nodiscard]] std::vector<Occurrence> find_at_range(int64_t file_id, int64_t start_byte,
                                                        int64_t end_byte);
    bool update_resolution(int64_t id, std::optional<int64_t> symbol_id,
                           const std::string& resolution, double confidence,
                           const std::optional<std::string>& metadata_json = std::nullopt);
    bool delete_by_file(int64_t file_id);

private:
    Connection& conn_;
};

} // namespace codelenses
