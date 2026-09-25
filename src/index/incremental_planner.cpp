#include "codelenses/index/incremental_planner.hpp"
#include "codelenses/filesystem/file_capture.hpp"

#include <sys/stat.h>

#include <unordered_map>
#include <unordered_set>

namespace codelenses::index {

PlanResult plan_indexing(int64_t /*workspace_id*/,
                         const std::vector<filesystem::DiscoveredFile>& discovered,
                         const std::vector<FileStateItem>& db_states, bool force_full) {
    PlanResult plan;

    std::unordered_map<std::string, const FileStateItem*> state_map;
    state_map.reserve(db_states.size());
    for (const auto& item : db_states) {
        state_map[item.relative_path] = &item;
    }

    std::unordered_set<std::string> discovered_paths;
    discovered_paths.reserve(discovered.size());

    for (const auto& file : discovered) {
        discovered_paths.insert(file.relative_path);

        auto it = state_map.find(file.relative_path);
        if (it == state_map.end() || it->second->is_deleted) {
            // New or previously deleted file
            PlannedFile pf{
                .relative_path = file.relative_path,
                .absolute_path = file.absolute_path,
                .file_id = (it != state_map.end()) ? it->second->id : 0,
                .language = file.language,
                .action = PlannedAction::parse,
                .reason = (it == state_map.end()) ? "new source" : "re-created source",
                .file_size = file.file_size,
                .is_binary = file.is_binary,
            };
            plan.files_to_process.push_back(std::move(pf));
            if (it != state_map.end()) {
                plan.keep_file_ids.push_back(it->second->id);
            }
        } else {
            const auto* existing = it->second;
            plan.keep_file_ids.push_back(existing->id);

            if (force_full) {
                PlannedFile pf{
                    .relative_path = file.relative_path,
                    .absolute_path = file.absolute_path,
                    .file_id = existing->id,
                    .language = file.language,
                    .action = PlannedAction::parse,
                    .reason = "full reindex requested",
                    .file_size = file.file_size,
                    .is_binary = file.is_binary,
                };
                plan.files_to_process.push_back(std::move(pf));
            } else {
                // Check filesystem mtime
                struct stat st {};
                int64_t current_mtime_ns = 0;
                if (::stat(file.absolute_path.c_str(), &st) == 0 && st.st_mtim.tv_sec >= 0 &&
                    st.st_mtim.tv_nsec >= 0) {
                    current_mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1'000'000'000LL +
                                       static_cast<int64_t>(st.st_mtim.tv_nsec);
                }

                if (existing->size_bytes == static_cast<int64_t>(file.file_size) &&
                    existing->modified_ns == current_mtime_ns) {
                    bool content_changed = false;
                    if (existing->content_hash.has_value()) {
                        auto capture_res = filesystem::capture_file(file.absolute_path);
                        if (capture_res && capture_res->content_hash != *existing->content_hash) {
                            content_changed = true;
                        }
                    }

                    if (!content_changed) {
                        PlannedFile pf{
                            .relative_path = file.relative_path,
                            .absolute_path = file.absolute_path,
                            .file_id = existing->id,
                            .language = file.language,
                            .action = PlannedAction::skip,
                            .reason = "source size and timestamp unchanged",
                            .file_size = file.file_size,
                            .is_binary = file.is_binary,
                        };
                        plan.files_to_skip.push_back(std::move(pf));
                    } else {
                        PlannedFile pf{
                            .relative_path = file.relative_path,
                            .absolute_path = file.absolute_path,
                            .file_id = existing->id,
                            .language = file.language,
                            .action = PlannedAction::parse,
                            .reason = "source content modified",
                            .file_size = file.file_size,
                            .is_binary = file.is_binary,
                        };
                        plan.files_to_process.push_back(std::move(pf));
                    }
                } else {
                    PlannedFile pf{
                        .relative_path = file.relative_path,
                        .absolute_path = file.absolute_path,
                        .file_id = existing->id,
                        .language = file.language,
                        .action = PlannedAction::parse,
                        .reason = "source metadata modified",
                        .file_size = file.file_size,
                        .is_binary = file.is_binary,
                    };
                    plan.files_to_process.push_back(std::move(pf));
                }
            }
        }
    }

    // Check for deleted files
    for (const auto& item : db_states) {
        if (!item.is_deleted && !discovered_paths.contains(item.relative_path)) {
            plan.file_ids_to_remove.push_back(item.id);
        }
    }

    return plan;
}

} // namespace codelenses::index
