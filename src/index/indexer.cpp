#include "codelenses/index/indexer.hpp"

#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>

#include "codelenses/db/statement.hpp"
#include "codelenses/db/transaction.hpp"
#include "codelenses/filesystem/file_capture.hpp"
#include "codelenses/filesystem/path.hpp"
#include "codelenses/index/bounded_queue.hpp"
#include "codelenses/index/incremental_planner.hpp"
#include "codelenses/index/thread_pool.hpp"

namespace codelenses::index {
namespace {

struct ExtractionResult {
    PlannedFile planned;
    FileIndexData index_data;
    std::vector<Diagnostic> diagnostics;
    bool success{true};
    int64_t errors{0};
    int64_t warnings{0};
};

std::string make_symbol_key(std::string_view rel_path, NodeKind kind, std::string_view name,
                            std::string_view signature, int64_t start_byte) {
    std::string key(rel_path);
    key += "#";
    key += to_string(kind);
    key += "#";
    key += name;
    if (!signature.empty()) {
        key += "#";
        key += signature;
    }
    key += "@";
    key += std::to_string(start_byte);
    return key;
}

std::string fact_kind_to_occurrence_kind(worker::FactKind kind) {
    switch (kind) {
    case worker::FactKind::symbol:
        return "definition";
    case worker::FactKind::declaration:
        return "declaration";
    case worker::FactKind::scope:
        return "definition";
    case worker::FactKind::reference:
        return "reference";
    case worker::FactKind::call:
        return "reference";
    case worker::FactKind::inheritance:
        return "reference";
    case worker::FactKind::implementation:
        return "implementation";
    case worker::FactKind::include:
        return "include";
    case worker::FactKind::import:
        return "import";
    }
    return "reference";
}

ExtractionResult extract_file(int64_t workspace_id, int64_t job_id, const PlannedFile& planned,
                              adapters::AdapterRegistry& registry, std::size_t max_file_size,
                              const std::stop_token& stop_token) {
    ExtractionResult result;
    result.planned = planned;

    if (stop_token.stop_requested()) {
        result.success = false;
        return result;
    }

    auto capture_res = filesystem::capture_file(planned.absolute_path, max_file_size);
    if (!capture_res) {
        result.success = false;
        result.errors++;
        Diagnostic diag{
            .workspace_id = workspace_id,
            .job_id = job_id,
            .severity = "error",
            .source = "filesystem",
            .code = "read_error",
            .message = capture_res.error().message,
        };
        result.diagnostics.push_back(std::move(diag));
        return result;
    }

    const auto& captured = *capture_res;
    result.index_data.size_bytes = static_cast<int64_t>(captured.byte_size);
    result.index_data.modified_ns = captured.modified_ns;
    result.index_data.content_hash = captured.content_hash;
    result.index_data.is_binary = captured.is_binary;
    result.index_data.last_index_job_id = job_id;

    if (captured.is_binary || planned.language == Language::unknown) {
        result.index_data.language = "unknown";
        return result;
    }

    result.index_data.language = std::string(to_string(planned.language));

    auto* adapter = registry.get_adapter(planned.language);
    if (!adapter) {
        adapter = registry.get_adapter_for_path(planned.absolute_path);
    }

    if (!adapter) {
        // No adapter registered for this language, keep as unparsed known file
        return result;
    }

    auto parse_res = adapter->parse(captured.as_string_view(), planned.absolute_path, stop_token);
    if (!parse_res) {
        result.errors++;
        Diagnostic diag{
            .workspace_id = workspace_id,
            .job_id = job_id,
            .severity = "error",
            .source = "adapter",
            .code = "adapter_error",
            .message = parse_res.error().message,
        };
        result.diagnostics.push_back(std::move(diag));
        return result;
    }

    const auto& adapter_res = *parse_res;

    // 1. Process symbols
    result.index_data.symbols.reserve(adapter_res.symbols.size());
    for (size_t i = 0; i < adapter_res.symbols.size(); ++i) {
        const auto& s = adapter_res.symbols[i];
        Symbol sym{
            .id = static_cast<int64_t>(i + 1), // temporary 1-based ID for within-file remapping
            .workspace_id = workspace_id,
            .symbol_key = make_symbol_key(planned.relative_path, s.kind,
                                          s.qualified_name.empty() ? s.name : s.qualified_name,
                                          s.signature, static_cast<int64_t>(s.range.start)),
            .name = s.name,
            .qualified_name = s.qualified_name.empty() ? std::nullopt
                                                       : std::make_optional(s.qualified_name),
            .kind = std::string(to_string(s.kind)),
            .language = result.index_data.language,
            .signature = s.signature.empty() ? std::nullopt : std::make_optional(s.signature),
            .container_name = s.enclosing_scope,
            .is_definition = true,
            .range =
                SourceRange{
                    .start_byte = static_cast<int64_t>(s.range.start),
                    .end_byte = static_cast<int64_t>(s.range.end),
                    .start_line = static_cast<int64_t>(s.display_range.start_line),
                    .start_column = static_cast<int64_t>(s.display_range.start_column),
                    .end_line = static_cast<int64_t>(s.display_range.end_line),
                    .end_column = static_cast<int64_t>(s.display_range.end_column),
                },
        };
        result.index_data.symbols.push_back(std::move(sym));
    }

    // 2. Process occurrences
    result.index_data.occurrences.reserve(adapter_res.occurrences.size());
    for (const auto& occ : adapter_res.occurrences) {
        Occurrence db_occ{
            .workspace_id = workspace_id,
            .occurrence_kind = fact_kind_to_occurrence_kind(occ.kind),
            .name = occ.written_name,
            .range =
                SourceRange{
                    .start_byte = static_cast<int64_t>(occ.range.start),
                    .end_byte = static_cast<int64_t>(occ.range.end),
                    .start_line = static_cast<int64_t>(occ.display_range.start_line),
                    .start_column = static_cast<int64_t>(occ.display_range.start_column),
                    .end_line = static_cast<int64_t>(occ.display_range.end_line),
                    .end_column = static_cast<int64_t>(occ.display_range.end_column),
                },
            .confidence = 1.0,
            .resolution = "unresolved",
        };

        // If occurrence matches a symbol declared in the same file, link it
        for (const auto& sym : result.index_data.symbols) {
            if (sym.name == occ.written_name) {
                db_occ.symbol_id = sym.id;
                db_occ.resolution = "resolved";
                break;
            }
        }

        // Create references for calls and references
        if (occ.kind == worker::FactKind::call || occ.kind == worker::FactKind::reference) {
            ReferenceOccurrence ref{
                .workspace_id = workspace_id,
                .target_symbol_id = db_occ.symbol_id,
                .name = occ.written_name,
                .reference_kind = (occ.kind == worker::FactKind::call) ? "call" : "reference",
                .range = db_occ.range,
                .resolution = db_occ.resolution,
                .confidence = 1.0,
            };
            result.index_data.references.push_back(std::move(ref));
        }

        result.index_data.occurrences.push_back(std::move(db_occ));

        // Create file dependencies for includes/imports
        if (occ.kind == worker::FactKind::include || occ.kind == worker::FactKind::import) {
            FileDependency dep{
                .workspace_id = workspace_id,
                .dependency_kind = (occ.kind == worker::FactKind::include) ? "include" : "import",
                .raw_name = occ.written_name,
                .resolution = "unresolved",
                .start_byte = static_cast<int64_t>(occ.range.start),
                .end_byte = static_cast<int64_t>(occ.range.end),
            };
            result.index_data.dependencies.push_back(std::move(dep));
        }
    }

    // 3. Process diagnostics
    for (const auto& d : adapter_res.diagnostics) {
        std::string severity = "error";
        if (d.severity == DiagnosticSeverity::info) {
            severity = "info";
        } else if (d.severity == DiagnosticSeverity::warning) {
            severity = "warning";
            result.warnings++;
        } else {
            severity = "error";
            result.errors++;
        }

        Diagnostic diag{
            .workspace_id = workspace_id,
            .job_id = job_id,
            .severity = severity,
            .source = "parser",
            .code = d.code.empty() ? "syntax_error" : d.code,
            .message = d.message.empty() ? "Syntax error" : d.message,
            .start_byte = static_cast<int64_t>(d.byte_range.start),
            .end_byte = static_cast<int64_t>(d.byte_range.end),
            .start_line = static_cast<int64_t>(d.display_range.start_line),
            .start_column = static_cast<int64_t>(d.display_range.start_column),
            .end_line = static_cast<int64_t>(d.display_range.end_line),
            .end_column = static_cast<int64_t>(d.display_range.end_column),
        };
        result.diagnostics.push_back(std::move(diag));
    }

    return result;
}

} // namespace

IndexingPipeline::IndexingPipeline(Database& db, IndexerOptions options)
    : db_(db), options_(options),
      registry_(options.registry ? *options.registry : adapters::default_adapter_registry()) {}

IndexingPipeline::~IndexingPipeline() {
    std::lock_guard lock(jobs_mutex_);
    for (auto& [_, stop_source] : active_workspace_jobs_) {
        if (stop_source) {
            stop_source->request_stop();
        }
    }
}

Result<void> IndexingPipeline::cancel_workspace(int64_t workspace_id) {
    std::lock_guard lock(jobs_mutex_);
    auto it = active_workspace_jobs_.find(workspace_id);
    if (it != active_workspace_jobs_.end() && it->second) {
        it->second->request_stop();
        return {};
    }
    return unexpected_result<void>(ErrorCode::not_found, "no active indexing job for workspace");
}

Result<void> IndexingPipeline::cancel_job(int64_t job_id) {
    std::lock_guard lock(jobs_mutex_);
    auto it = job_to_workspace_map_.find(job_id);
    if (it != job_to_workspace_map_.end()) {
        auto ws_it = active_workspace_jobs_.find(it->second);
        if (ws_it != active_workspace_jobs_.end() && ws_it->second) {
            ws_it->second->request_stop();
            return {};
        }
    }
    return unexpected_result<void>(ErrorCode::not_found, "no active job found with ID");
}

bool IndexingPipeline::is_indexing(int64_t workspace_id) const {
    std::lock_guard lock(jobs_mutex_);
    return active_workspace_jobs_.contains(workspace_id);
}

Result<IndexResult> IndexingPipeline::run_indexing(int64_t workspace_id,
                                                   const std::string& job_type, bool force_full,
                                                   std::stop_token stop) {
    auto ws = db_.workspaces().get_by_id(workspace_id);
    if (!ws) {
        return unexpected_result<IndexResult>(ErrorCode::not_found, "workspace not found");
    }

    auto stop_source = std::make_shared<std::stop_source>();
    std::stop_callback stop_cb(stop, [stop_source]() { stop_source->request_stop(); });

    int64_t job_id = 0;
    {
        std::lock_guard lock(jobs_mutex_);
        if (active_workspace_jobs_.contains(workspace_id)) {
            return unexpected_result<IndexResult>(
                ErrorCode::conflict, "an indexing job is already active for this workspace");
        }

        IndexJob job{
            .workspace_id = workspace_id,
            .job_type = job_type,
            .status = "running",
            .requested_mode = (force_full ? "full" : "incremental"),
        };
        job_id = db_.jobs().create(job);
        db_.jobs().update_status(job_id, "running");

        active_workspace_jobs_[workspace_id] = stop_source;
        job_to_workspace_map_[job_id] = workspace_id;
    }

    struct JobCleanup {
        IndexingPipeline& self;
        int64_t ws_id;
        int64_t j_id;
        ~JobCleanup() {
            std::lock_guard lock(self.jobs_mutex_);
            self.active_workspace_jobs_.erase(ws_id);
            self.job_to_workspace_map_.erase(j_id);
        }
    } cleanup{*this, workspace_id, job_id};

    auto job_stop = stop_source->get_token();

    // Set workspace status to indexing
    ws->status = WorkspaceStatus::indexing;
    db_.workspaces().update(*ws);

    auto canonical_root_res = filesystem::canonicalize_workspace_root(ws->root_path);
    if (!canonical_root_res) {
        db_.jobs().finish_job(job_id, "failed", std::nullopt, canonical_root_res.error().message);
        ws->status = WorkspaceStatus::error;
        ws->last_error = canonical_root_res.error().message;
        db_.workspaces().update(*ws);
        return std::unexpected(canonical_root_res.error());
    }
    const auto canonical_root = *canonical_root_res;

    // File discovery
    filesystem::DiscoveryOptions disc_opts;
    disc_opts.include_patterns = ws->include_patterns;
    disc_opts.exclude_patterns = ws->exclude_patterns;
    disc_opts.default_ignores = ws->default_ignores;
    disc_opts.max_file_size_bytes = options_.max_file_size_bytes;

    filesystem::FileDiscovery discovery(disc_opts);
    auto disc_res = discovery.discover(canonical_root);
    if (!disc_res) {
        db_.jobs().finish_job(job_id, "failed", std::nullopt, disc_res.error().message);
        ws->status = WorkspaceStatus::error;
        ws->last_error = disc_res.error().message;
        db_.workspaces().update(*ws);
        return std::unexpected(disc_res.error());
    }
    const auto& discovered = *disc_res;

    // Incremental planning
    auto db_states = db_.files().get_file_states(workspace_id);
    auto plan = plan_indexing(workspace_id, discovered, db_states,
                              force_full || (job_type == "full"));

    int64_t files_total =
        static_cast<int64_t>(plan.files_to_process.size() + plan.files_to_skip.size());
    int64_t files_skipped = static_cast<int64_t>(plan.files_to_skip.size());
    int64_t files_processed = 0;
    int64_t error_count = 0;
    int64_t warning_count = 0;

    // Update job files_total
    {
        Statement update_total(db_.connection().handle(),
                               "UPDATE index_job SET files_total = ? WHERE id = ?;");
        update_total.bind_int64(1, files_total);
        update_total.bind_int64(2, job_id);
        update_total.execute();
    }

    if (job_stop.stop_requested()) {
        db_.jobs().finish_job(job_id, "cancelled", std::nullopt, "Indexing job was cancelled");
        ws->status = WorkspaceStatus::idle;
        db_.workspaces().update(*ws);
        return IndexResult{
            .job_id = job_id,
            .status = "cancelled",
            .files_total = files_total,
            .files_processed = 0,
            .files_skipped = files_skipped,
            .error_count = 0,
            .warning_count = 0,
            .workspace_revision = std::nullopt,
            .error_message = "Indexing job was cancelled",
        };
    }

    // Parallel parsing via ThreadPool and bounded batches
    ThreadPool pool(options_.worker_threads);

    std::size_t batch_size = options_.queue_capacity;
    std::size_t idx = 0;
    bool cancelled = false;

    while (idx < plan.files_to_process.size()) {
        if (job_stop.stop_requested()) {
            cancelled = true;
            break;
        }

        std::size_t current_batch =
            std::min(batch_size, plan.files_to_process.size() - idx);
        std::vector<std::future<ExtractionResult>> futures;
        futures.reserve(current_batch);

        for (std::size_t i = 0; i < current_batch; ++i) {
            const auto& planned = plan.files_to_process[idx + i];
            futures.push_back(pool.submit([this, workspace_id, job_id, planned, job_stop] {
                return extract_file(workspace_id, job_id, planned, registry_,
                                    options_.max_file_size_bytes, job_stop);
            }));
        }

        for (auto& fut : futures) {
            auto ext_res = fut.get();
            if (job_stop.stop_requested()) {
                cancelled = true;
                break;
            }

            error_count += ext_res.errors;
            warning_count += ext_res.warnings;

            // Database persistence (single writer thread)
            int64_t file_id = ext_res.planned.file_id;
            if (file_id == 0) {
                // Insert new file record
                FileRecord record{
                    .workspace_id = workspace_id,
                    .path = ext_res.planned.absolute_path.string(),
                    .relative_path = ext_res.planned.relative_path,
                    .name = ext_res.planned.absolute_path.filename().string(),
                    .extension = ext_res.planned.absolute_path.extension().string(),
                    .language = ext_res.index_data.language,
                    .encoding = "utf-8",
                    .size_bytes = ext_res.index_data.size_bytes,
                    .modified_ns = ext_res.index_data.modified_ns,
                    .content_hash = ext_res.index_data.content_hash,
                    .is_binary = ext_res.index_data.is_binary,
                    .last_index_job_id = job_id,
                };
                file_id = db_.files().insert(record);
                ext_res.planned.file_id = file_id;
                plan.keep_file_ids.push_back(file_id);
            }

            if (file_id > 0) {
                // Assign file_id to all symbols and occurrences
                for (auto& sym : ext_res.index_data.symbols) {
                    sym.file_id = file_id;
                }
                for (auto& occ : ext_res.index_data.occurrences) {
                    occ.file_id = file_id;
                }
                for (auto& ref : ext_res.index_data.references) {
                    ref.source_file_id = file_id;
                }
                for (auto& dep : ext_res.index_data.dependencies) {
                    dep.source_file_id = file_id;
                }
                for (auto& diag : ext_res.diagnostics) {
                    diag.file_id = file_id;
                }

                db_.replace_file_index(file_id, ext_res.index_data);
                if (!ext_res.diagnostics.empty()) {
                    db_.diagnostics().insert_batch(ext_res.diagnostics);
                }
                files_processed++;
            }
        }

        if (cancelled) {
            break;
        }

        idx += current_batch;
        db_.jobs().update_progress(job_id, files_processed, files_skipped, error_count,
                                   warning_count);
    }

    pool.shutdown();

    if (cancelled || job_stop.stop_requested()) {
        db_.jobs().finish_job(job_id, "cancelled", std::nullopt, "Indexing job was cancelled");
        ws->status = WorkspaceStatus::idle;
        db_.workspaces().update(*ws);
        return IndexResult{
            .job_id = job_id,
            .status = "cancelled",
            .files_total = files_total,
            .files_processed = files_processed,
            .files_skipped = files_skipped,
            .error_count = error_count,
            .warning_count = warning_count,
            .workspace_revision = std::nullopt,
            .error_message = "Indexing job was cancelled",
        };
    }

    // Cleanup stale records for deleted files (D-10)
    db_.files().mark_missing_as_deleted(workspace_id, plan.keep_file_ids);
    db_.files().cleanup_deleted_files_derived_data(workspace_id);

    // Job completion and revision increment (D-06)
    int64_t new_revision = db_.workspaces().increment_revision(workspace_id);
    db_.jobs().finish_job(job_id, "completed", new_revision);
    db_.jobs().update_progress(job_id, files_processed, files_skipped, error_count, warning_count);

    ws->status = WorkspaceStatus::ready;
    ws->revision = new_revision;
    ws->last_error = std::nullopt;
    db_.workspaces().update(*ws);

    return IndexResult{
        .job_id = job_id,
        .status = "completed",
        .files_total = files_total,
        .files_processed = files_processed,
        .files_skipped = files_skipped,
        .error_count = error_count,
        .warning_count = warning_count,
        .workspace_revision = new_revision,
    };
}

} // namespace codelenses::index
