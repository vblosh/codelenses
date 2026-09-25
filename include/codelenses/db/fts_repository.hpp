#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "codelenses/domain/file_content.hpp"
#include "codelenses/domain/search.hpp"

namespace codelenses {

class Connection;

class FtsRepository {
public:
    explicit FtsRepository(Connection& conn);

    [[nodiscard]] std::vector<SymbolSearchResult> search_symbols(int64_t workspace_id,
                                                                 const std::string& query,
                                                                 int64_t limit = 50,
                                                                 int64_t offset = 0);

    void insert_or_update_file_content(const FileContent& content);
    void delete_file_content(int64_t file_id);

    [[nodiscard]] std::vector<FileSearchResult> search_files(int64_t workspace_id,
                                                             const std::string& query,
                                                             int64_t limit = 50,
                                                             int64_t offset = 0);

    void rebuild_symbol_index();
    void rebuild_file_index();

private:
    Connection& conn_;
};

} // namespace codelenses
