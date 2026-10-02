#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "codelenses/domain/file.hpp"

namespace codelenses {

class Connection;

class FileRepository {
public:
    explicit FileRepository(Connection& conn);

    int64_t insert(const FileRecord& file);
    bool update(const FileRecord& file);
    [[nodiscard]] std::optional<FileRecord> get_by_id(int64_t id);
    [[nodiscard]] std::optional<FileRecord> get_by_path(int64_t workspace_id,
                                                        const std::string& relative_path);
    [[nodiscard]] std::vector<FileRecord> list_by_workspace(int64_t workspace_id,
                                                            bool include_deleted = false);
    [[nodiscard]] int64_t count_by_workspace(int64_t workspace_id);
    [[nodiscard]] std::vector<FileLanguageCount> count_by_language(int64_t workspace_id);
    [[nodiscard]] std::vector<FileStateItem> get_file_states(int64_t workspace_id);

    bool mark_deleted(int64_t file_id);
    int64_t mark_missing_as_deleted(int64_t workspace_id,
                                    const std::vector<int64_t>& keep_file_ids);
    int64_t cleanup_deleted_files_derived_data(int64_t workspace_id);
    int64_t delete_tombstones(int64_t workspace_id);
    bool delete_by_id(int64_t id);

private:
    Connection& conn_;
};

} // namespace codelenses
