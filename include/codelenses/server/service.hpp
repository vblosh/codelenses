#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "codelenses/db/database.hpp"
#include "codelenses/index/indexer.hpp"
#include "codelenses/server/dto.hpp"
#include "codelenses/server/error.hpp"
#include "codelenses/server/server_config.hpp"

namespace codelenses::server {

class ApiService {
public:
    ApiService(Database& db, index::IndexingPipeline& pipeline,
               WorkspacePolicy policy = WorkspacePolicy{});
    ~ApiService();

    ApiService(const ApiService&) = delete;
    ApiService& operator=(const ApiService&) = delete;

    // Workspace CRUD (F-04)
    WorkspaceDto create_workspace(const CreateWorkspaceRequest& req);
    WorkspaceDto get_workspace(int64_t id);
    std::vector<WorkspaceDto> list_workspaces();
    WorkspaceDto update_workspace(int64_t id, const UpdateWorkspaceRequest& req);
    void delete_workspace(int64_t id);

    // Indexing, Status & Jobs (F-05)
    JobDto trigger_indexing(int64_t workspace_id, const IndexJobRequest& req);
    WorkspaceStatusDto get_workspace_status(int64_t workspace_id);
    JobDto get_job(int64_t job_id);
    JobDto cancel_job(int64_t job_id);

    // Tree, File Metadata & Range Content (F-06, F-10)
    WorkspaceTreeDto get_tree(int64_t workspace_id, const std::string& path);
    FileMetadataDto get_file(int64_t workspace_id, int64_t file_id);
    FileContentDto get_file_content(int64_t workspace_id, int64_t file_id,
                                    std::optional<int64_t> start_line = std::nullopt,
                                    std::optional<int64_t> end_line = std::nullopt,
                                    std::optional<int64_t> start_byte = std::nullopt,
                                    std::optional<int64_t> end_byte = std::nullopt);

    // Highlights, Symbols & Outline (F-07)
    HighlightResponseDto get_file_highlights(int64_t workspace_id, int64_t file_id);
    std::vector<SymbolDto> get_file_symbols(int64_t workspace_id, int64_t file_id);
    FileOutlineDto get_file_outline(int64_t workspace_id, int64_t file_id);
    std::vector<OccurrenceDto>
    get_file_occurrences(int64_t workspace_id, int64_t file_id,
                         std::optional<std::string> kind = std::nullopt,
                         std::optional<int64_t> start_byte = std::nullopt,
                         std::optional<int64_t> end_byte = std::nullopt);

    // Workspace Symbols & Detail (F-07, F-08)
    PaginatedResultDto<SymbolDto> list_symbols(int64_t workspace_id,
                                               std::optional<std::string> query = std::nullopt,
                                               std::optional<std::string> kind = std::nullopt,
                                               std::optional<std::string> language = std::nullopt,
                                               std::optional<int64_t> file_id = std::nullopt,
                                               int64_t limit = 50, int64_t offset = 0);
    SymbolDetailDto get_symbol_detail(int64_t workspace_id, int64_t symbol_id);
    PaginatedResultDto<ReferencerDto> get_symbol_references(int64_t workspace_id, int64_t symbol_id,
                                                            int64_t limit = 50, int64_t offset = 0);
    std::vector<SymbolDto> get_symbol_definitions(int64_t workspace_id, int64_t symbol_id);
    std::vector<CallerCalleeDto> get_symbol_callers(int64_t workspace_id, int64_t symbol_id);
    std::vector<CallerCalleeDto> get_symbol_callees(int64_t workspace_id, int64_t symbol_id);
    SymbolGraphDto get_symbol_graph(int64_t workspace_id, int64_t symbol_id, int depth = 1,
                                    size_t max_nodes = 50, size_t max_edges = 100,
                                    const std::vector<std::string>& kinds = {});

    // Search (F-09)
    PaginatedResultDto<SourceSearchHitDto> search_source(int64_t workspace_id,
                                                         const std::string& query,
                                                         int64_t limit = 50, int64_t offset = 0);
    PaginatedResultDto<SymbolSearchHitDto> search_symbols(int64_t workspace_id,
                                                          const std::string& query,
                                                          int64_t limit = 50, int64_t offset = 0);

    // Graceful Shutdown & Cancellation (F-13)
    void shutdown();

private:
    Workspace require_workspace(int64_t id);
    FileRecord require_file(int64_t workspace_id, int64_t file_id);
    Symbol require_symbol(int64_t workspace_id, int64_t symbol_id);

    Database& db_;
    index::IndexingPipeline& pipeline_;
    WorkspacePolicy policy_;

    std::mutex threads_mutex_;
    std::vector<std::thread> background_threads_;
    std::unordered_set<int64_t> reserved_workspaces_;
    std::unordered_map<int64_t, std::shared_ptr<std::stop_source>> active_job_stops_;
    std::atomic<bool> shutting_down_{false};
};

} // namespace codelenses::server
