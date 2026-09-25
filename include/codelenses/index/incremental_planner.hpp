#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "codelenses/domain/file.hpp"
#include "codelenses/filesystem/discovery.hpp"
#include "codelenses/language.hpp"

namespace codelenses::index {

enum class PlannedAction {
    skip,   // File is unchanged, skip parsing and replacement
    parse,  // File is new or modified, re-parse and replace
    remove, // File has been deleted from filesystem
};

struct PlannedFile {
    std::string relative_path;
    std::filesystem::path absolute_path;
    int64_t file_id{0};
    Language language{Language::unknown};
    PlannedAction action{PlannedAction::parse};
    std::string reason;
    uint64_t file_size{0};
    bool is_binary{false};
};

struct PlanResult {
    std::vector<PlannedFile> files_to_process;
    std::vector<PlannedFile> files_to_skip;
    std::vector<int64_t> file_ids_to_remove;
    std::vector<int64_t> keep_file_ids;
};

// Compares discovered files with existing file states in SQLite.
// Classifies each file into skip (unchanged) vs parse (added/modified) vs remove (deleted).
[[nodiscard]] PlanResult plan_indexing(int64_t workspace_id,
                                       const std::vector<filesystem::DiscoveredFile>& discovered,
                                       const std::vector<FileStateItem>& db_states,
                                       bool force_full = false);

} // namespace codelenses::index
