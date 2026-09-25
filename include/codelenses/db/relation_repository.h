#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "codelenses/domain/relation.h"

namespace codelenses {

class Connection;

class RelationRepository {
public:
    explicit RelationRepository(Connection& conn);

    int64_t insert(const SymbolRelation& rel);
    void insert_batch(const std::vector<SymbolRelation>& relations);
    [[nodiscard]] std::vector<SymbolRelation>
    find_by_source_symbol(int64_t source_symbol_id,
                          const std::optional<std::string>& relation_kind = std::nullopt);
    [[nodiscard]] std::vector<SymbolRelation>
    find_by_target_symbol(int64_t target_symbol_id,
                          const std::optional<std::string>& relation_kind = std::nullopt);
    bool delete_by_file(int64_t file_id);

private:
    Connection& conn_;
};

} // namespace codelenses
