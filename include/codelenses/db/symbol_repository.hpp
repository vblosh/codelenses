#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "codelenses/domain/symbol.hpp"

namespace codelenses {

class Connection;

class SymbolRepository {
public:
    explicit SymbolRepository(Connection& conn);

    int64_t insert(const Symbol& symbol);
    std::vector<int64_t> insert_batch(const std::vector<Symbol>& symbols);
    [[nodiscard]] std::optional<Symbol> get_by_id(int64_t id);
    [[nodiscard]] std::vector<Symbol> get_by_key(int64_t workspace_id,
                                                 const std::string& symbol_key);
    [[nodiscard]] std::vector<Symbol> list_by_file(int64_t file_id);
    [[nodiscard]] std::optional<Symbol> find_at_offset(int64_t file_id, int64_t byte_offset);
    [[nodiscard]] std::vector<Symbol> find_by_name(int64_t workspace_id, const std::string& name);
    [[nodiscard]] std::vector<Symbol> find_by_qualified_name(int64_t workspace_id,
                                                             const std::string& qualified_name);
    [[nodiscard]] std::vector<Symbol> list_by_workspace(int64_t workspace_id);
    [[nodiscard]] int64_t count_by_workspace(int64_t workspace_id);
    bool delete_by_file(int64_t file_id);

private:
    Connection& conn_;
};

} // namespace codelenses
