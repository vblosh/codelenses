#include "codelenses/server/service.hpp"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

#include "codelenses/adapters/registry.hpp"
#include "codelenses/filesystem/discovery.hpp"
#include "codelenses/filesystem/file_capture.hpp"
#include "codelenses/filesystem/path.hpp"
#include "codelenses/parser/reference_adapter.hpp"
#include "codelenses/resolver/compile_commands.hpp"
#include "codelenses/treesitter/grammars.hpp"
#include "codelenses/treesitter/parser.hpp"

namespace fs = std::filesystem;

namespace codelenses::server {

namespace {

struct LineSpan {
    int64_t start_byte{0};
    int64_t content_end_byte{0};
    int64_t line_end_byte{0};
};

std::vector<LineSpan> compute_line_spans(std::string_view text) {
    std::vector<LineSpan> spans;
    if (text.empty()) {
        return spans;
    }

    size_t pos = 0;
    while (pos < text.size()) {
        size_t start = pos;
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            size_t content_end = text.size();
            if (content_end > start && text[content_end - 1] == '\r') {
                content_end--;
            }
            spans.push_back(LineSpan{
                .start_byte = static_cast<int64_t>(start),
                .content_end_byte = static_cast<int64_t>(content_end),
                .line_end_byte = static_cast<int64_t>(text.size()),
            });
            break;
        } else {
            size_t content_end = nl;
            if (content_end > start && text[content_end - 1] == '\r') {
                content_end--;
            }
            spans.push_back(LineSpan{
                .start_byte = static_cast<int64_t>(start),
                .content_end_byte = static_cast<int64_t>(content_end),
                .line_end_byte = static_cast<int64_t>(nl + 1),
            });
            pos = nl + 1;
        }
    }

    if (!text.empty() && text.back() == '\n') {
        spans.push_back(LineSpan{
            .start_byte = static_cast<int64_t>(text.size()),
            .content_end_byte = static_cast<int64_t>(text.size()),
            .line_end_byte = static_cast<int64_t>(text.size()),
        });
    }

    return spans;
}

bool matches_any_ignore(const std::string& name, const std::vector<std::string>& patterns) {
    for (const auto& pat : patterns) {
        if (name == pat)
            return true;
        if (pat.starts_with('*') && name.ends_with(pat.substr(1)))
            return true;
    }
    return false;
}

} // namespace

ApiService::ApiService(Database& db, index::IndexingPipeline& pipeline, WorkspacePolicy policy)
    : db_(db), pipeline_(pipeline), policy_(std::move(policy)) {}

ApiService::~ApiService() {
    shutdown();
}

void ApiService::shutdown() {
    if (shutting_down_.exchange(true)) {
        return;
    }

    try {
        auto workspaces = db_.workspaces().list_all();
        for (const auto& ws : workspaces) {
            static_cast<void>(pipeline_.cancel_workspace(ws.id));
        }
    } catch (...) {
        // Ignore errors during database access on shutdown
    }

    std::vector<std::thread> threads_to_join;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        for (auto& [jid, stop_src] : active_job_stops_) {
            if (stop_src) {
                stop_src->request_stop();
            }
        }
        threads_to_join = std::move(background_threads_);
    }

    for (auto& th : threads_to_join) {
        if (th.joinable()) {
            th.join();
        }
    }
}

Workspace ApiService::require_workspace(int64_t id) {
    if (id <= 0) {
        throw ApiError::bad_request("invalid_id", "Workspace ID must be positive");
    }
    auto ws = db_.workspaces().get_by_id(id);
    if (!ws.has_value()) {
        throw ApiError::not_found("workspace_not_found",
                                  "Workspace " + std::to_string(id) + " not found");
    }
    return *ws;
}

FileRecord ApiService::require_file(int64_t workspace_id, int64_t file_id) {
    require_workspace(workspace_id);
    if (file_id <= 0) {
        throw ApiError::bad_request("invalid_id", "File ID must be positive");
    }
    auto f = db_.files().get_by_id(file_id);
    if (!f.has_value() || f->workspace_id != workspace_id) {
        throw ApiError::not_found("file_not_found", "File " + std::to_string(file_id) +
                                                        " not found in workspace " +
                                                        std::to_string(workspace_id));
    }
    return *f;
}

Symbol ApiService::require_symbol(int64_t workspace_id, int64_t symbol_id) {
    require_workspace(workspace_id);
    if (symbol_id <= 0) {
        throw ApiError::bad_request("invalid_id", "Symbol ID must be positive");
    }
    auto s = db_.symbols().get_by_id(symbol_id);
    if (!s.has_value() || s->workspace_id != workspace_id) {
        throw ApiError::not_found("symbol_not_found", "Symbol " + std::to_string(symbol_id) +
                                                          " not found in workspace " +
                                                          std::to_string(workspace_id));
    }
    return *s;
}

// ==========================================
// Workspace CRUD (F-04)
// ==========================================
WorkspaceDto ApiService::create_workspace(const CreateWorkspaceRequest& req) {
    if (req.root_path.empty()) {
        throw ApiError::bad_request("missing_field", "Field 'rootPath' is required");
    }

    std::error_code ec;
    auto sym_status = fs::symlink_status(req.root_path, ec);
    if (!ec && fs::is_symlink(sym_status) && !policy_.allow_external_symlinks) {
        throw ApiError::forbidden(
            "forbidden_workspace_root",
            "Workspace root cannot be a symlink when allow_external_symlinks is disabled");
    }

    auto canonical_root = filesystem::canonicalize_workspace_root(req.root_path);
    if (!canonical_root.has_value()) {
        throw ApiError::bad_request("invalid_workspace_root",
                                    "Workspace root path does not exist or is not a directory: " +
                                        req.root_path);
    }

    std::string reason;
    if (!policy_.is_allowed_root(*canonical_root, &reason)) {
        throw ApiError::forbidden("forbidden_workspace_root", reason);
    }

    std::string root_str = canonical_root->string();
    auto existing = db_.workspaces().get_by_root_path(root_str);
    if (existing.has_value()) {
        return workspace_to_dto(*existing);
    }

    std::string name = req.name;
    if (name.empty()) {
        name = canonical_root->filename().string();
        if (name.empty()) {
            name = "workspace";
        }
    }

    std::vector<std::string> default_ignores = req.default_ignores;
    if (default_ignores.empty()) {
        default_ignores = policy_.default_ignores;
    }

    Workspace ws{
        .root_path = root_str,
        .name = std::move(name),
        .include_patterns = req.include_patterns,
        .exclude_patterns = req.exclude_patterns,
        .default_ignores = std::move(default_ignores),
        .compile_commands_path = req.compile_commands_path,
        .default_compile_command = req.default_compile_command,
        .status = WorkspaceStatus::idle,
    };

    int64_t id = db_.workspaces().create(ws);
    auto created = db_.workspaces().get_by_id(id);
    if (!created.has_value()) {
        throw ApiError::internal_error("Failed to retrieve created workspace");
    }
    return workspace_to_dto(*created);
}

WorkspaceDto ApiService::get_workspace(int64_t id) {
    auto ws = require_workspace(id);
    return workspace_to_dto(ws);
}

std::vector<WorkspaceDto> ApiService::list_workspaces() {
    auto all = db_.workspaces().list_all();
    std::vector<WorkspaceDto> dtos;
    dtos.reserve(all.size());
    for (const auto& w : all) {
        dtos.push_back(workspace_to_dto(w));
    }
    return dtos;
}

WorkspaceDto ApiService::update_workspace(int64_t id, const UpdateWorkspaceRequest& req) {
    auto ws = require_workspace(id);

    if (req.name.has_value()) {
        ws.name = *req.name;
    }
    if (req.include_patterns.has_value()) {
        ws.include_patterns = *req.include_patterns;
    }
    if (req.exclude_patterns.has_value()) {
        ws.exclude_patterns = *req.exclude_patterns;
    }
    if (req.compile_commands_path.has_value()) {
        ws.compile_commands_path = *req.compile_commands_path;
        invalidate_cdb_cache(id);
    }
    if (req.default_compile_command.has_value()) {
        ws.default_compile_command = *req.default_compile_command;
    }

    db_.workspaces().update(ws);
    auto updated = db_.workspaces().get_by_id(id);
    return workspace_to_dto(*updated);
}

void ApiService::delete_workspace(int64_t id) {
    require_workspace(id);
    invalidate_cdb_cache(id);
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        reserved_workspaces_.erase(id);
    }
    if (pipeline_.is_indexing(id)) {
        static_cast<void>(pipeline_.cancel_workspace(id));
    }
    db_.workspaces().delete_by_id(id);
}

// ==========================================
// Indexing, Status & Jobs (F-05)
// ==========================================
JobDto ApiService::trigger_indexing(int64_t workspace_id, const IndexJobRequest& req) {
    require_workspace(workspace_id);

    std::unique_lock<std::mutex> lock(threads_mutex_);
    if (shutting_down_.load()) {
        throw ApiError::conflict("shutting_down", "Server is shutting down");
    }

    if (pipeline_.is_indexing(workspace_id) || reserved_workspaces_.contains(workspace_id)) {
        throw ApiError::conflict("indexing_in_progress",
                                 "An indexing job is already active for workspace " +
                                     std::to_string(workspace_id));
    }

    reserved_workspaces_.insert(workspace_id);

    IndexJob job{
        .workspace_id = workspace_id,
        .job_type = req.job_type.empty() ? "incremental" : req.job_type,
        .status = "queued",
        .requested_mode = req.force_full ? std::optional<std::string>("full") : std::nullopt,
    };
    int64_t job_id = db_.jobs().create(job);

    auto stop_source = std::make_shared<std::stop_source>();
    active_job_stops_[job_id] = stop_source;

    std::string job_type = job.job_type;
    bool force_full = req.force_full;

    background_threads_.emplace_back([this, workspace_id, job_type, force_full, job_id,
                                      stop_source]() {
        struct ThreadCleanup {
            ApiService& self;
            int64_t ws_id;
            int64_t j_id;
            ~ThreadCleanup() {
                std::lock_guard<std::mutex> lk(self.threads_mutex_);
                self.reserved_workspaces_.erase(ws_id);
                self.active_job_stops_.erase(j_id);
            }
        } cleanup{*this, workspace_id, job_id};

        auto res = pipeline_.run_indexing(workspace_id, job_type, force_full,
                                          stop_source->get_token(), job_id);
        if (!res) {
            auto current = db_.jobs().get_by_id(job_id);
            if (current.has_value() &&
                (current->status == "queued" || current->status == "running")) {
                std::string st = (res.error().code == ErrorCode::cancelled) ? "canceled" : "failed";
                db_.jobs().finish_job(job_id, st, std::nullopt, res.error().message);
            }
        }
    });

    lock.unlock();

    auto created_job = db_.jobs().get_by_id(job_id);
    if (!created_job.has_value()) {
        throw ApiError::internal_error("Failed to retrieve created indexing job");
    }
    return job_to_dto(*created_job);
}

WorkspaceStatusDto ApiService::get_workspace_status(int64_t workspace_id) {
    auto ws = require_workspace(workspace_id);

    std::optional<JobDto> latest_job = std::nullopt;
    auto recent_jobs = db_.jobs().list_by_workspace(workspace_id, 1);
    if (!recent_jobs.empty()) {
        latest_job = job_to_dto(recent_jobs[0]);
    }

    auto files = db_.files().list_by_workspace(workspace_id, false);
    auto symbols = db_.symbols().list_by_workspace(workspace_id);

    DiagnosticCountsDto diag_counts;
    auto diags = db_.diagnostics().list_by_workspace(workspace_id, std::nullopt, 100000, 0);
    diag_counts.total = static_cast<int64_t>(diags.size());
    for (const auto& d : diags) {
        if (d.severity == "error") {
            diag_counts.errors++;
        } else if (d.severity == "warning") {
            diag_counts.warnings++;
        } else {
            diag_counts.info++;
        }
    }

    std::string status_str = to_string(ws.status);
    bool is_indexing_active = false;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        is_indexing_active =
            pipeline_.is_indexing(workspace_id) || reserved_workspaces_.contains(workspace_id);
    }
    if (is_indexing_active) {
        status_str = "indexing";
    }

    return WorkspaceStatusDto{
        .workspace_id = ws.id,
        .status = std::move(status_str),
        .revision = ws.revision,
        .latest_job = std::move(latest_job),
        .file_count = static_cast<int64_t>(files.size()),
        .symbol_count = static_cast<int64_t>(symbols.size()),
        .diagnostic_counts = diag_counts,
    };
}

JobDto ApiService::get_job(int64_t job_id) {
    if (job_id <= 0) {
        throw ApiError::bad_request("invalid_id", "Job ID must be positive");
    }
    auto j = db_.jobs().get_by_id(job_id);
    if (!j.has_value()) {
        throw ApiError::not_found("job_not_found", "Job " + std::to_string(job_id) + " not found");
    }
    return job_to_dto(*j);
}

JobDto ApiService::cancel_job(int64_t job_id) {
    auto j = get_job(job_id);
    if (j.status == "running" || j.status == "queued") {
        bool cancelled_in_pipeline = pipeline_.cancel_job(job_id).has_value();
        {
            std::lock_guard<std::mutex> lock(threads_mutex_);
            auto it = active_job_stops_.find(job_id);
            if (it != active_job_stops_.end() && it->second) {
                it->second->request_stop();
            }
        }
        auto current = db_.jobs().get_by_id(job_id);
        if (current.has_value() && (current->status == "queued" || !cancelled_in_pipeline)) {
            db_.jobs().finish_job(job_id, "canceled", std::nullopt, "Canceled by user");
        }
    }
    return get_job(job_id);
}

// ==========================================
// Tree, File Metadata & Range Content (F-06, F-10)
// ==========================================
WorkspaceTreeDto ApiService::get_tree(int64_t workspace_id, const std::string& path) {
    auto ws = require_workspace(workspace_id);

    auto canonical_root = filesystem::canonicalize_workspace_root(ws.root_path);
    if (!canonical_root.has_value()) {
        throw ApiError::internal_error("Cannot resolve workspace root");
    }

    std::string rel_path = path;
    if (rel_path.empty() || rel_path == "/") {
        rel_path = ".";
    }

    auto target_path = filesystem::resolve_workspace_path(*canonical_root, rel_path);
    if (!target_path.has_value()) {
        throw ApiError::bad_request("path_traversal",
                                    "Path traverses outside workspace root: " + path);
    }

    std::error_code ec;
    if (!fs::exists(*target_path, ec) || !fs::is_directory(*target_path, ec)) {
        throw ApiError::not_found("directory_not_found", "Directory does not exist: " + path);
    }

    std::vector<TreeNodeDto> entries;

    for (const auto& entry : fs::directory_iterator(*target_path, ec)) {
        std::string filename = entry.path().filename().string();
        if (matches_any_ignore(filename, ws.default_ignores)) {
            continue;
        }

        std::string entry_rel = filesystem::to_workspace_relative(*canonical_root, entry.path());
        bool is_dir = entry.is_directory(ec);

        if (is_dir) {
            entries.push_back(TreeNodeDto{
                .name = filename,
                .path = entry_rel,
                .type = "directory",
                .file_id = std::nullopt,
                .size_bytes = 0,
                .is_binary = false,
                .language = "",
            });
        } else if (entry.is_regular_file(ec)) {
            auto file_rec = db_.files().get_by_path(workspace_id, entry_rel);
            if (file_rec.has_value()) {
                entries.push_back(TreeNodeDto{
                    .name = filename,
                    .path = entry_rel,
                    .type = "file",
                    .file_id = file_rec->id,
                    .size_bytes = file_rec->size_bytes,
                    .is_binary = file_rec->is_binary,
                    .language = file_rec->language,
                });
            } else {
                int64_t size_bytes = static_cast<int64_t>(entry.file_size(ec));
                entries.push_back(TreeNodeDto{
                    .name = filename,
                    .path = entry_rel,
                    .type = "file",
                    .file_id = std::nullopt,
                    .size_bytes = size_bytes,
                    .is_binary = false,
                    .language = "unknown",
                });
            }
        }
    }

    std::sort(entries.begin(), entries.end(), [](const TreeNodeDto& a, const TreeNodeDto& b) {
        if (a.type != b.type) {
            return a.type == "directory";
        }
        return a.name < b.name;
    });

    std::string display_path = (rel_path == ".") ? "" : rel_path;
    return WorkspaceTreeDto{
        .workspace_id = ws.id,
        .path = std::move(display_path),
        .entries = std::move(entries),
    };
}

FileMetadataDto ApiService::get_file(int64_t workspace_id, int64_t file_id) {
    auto f = require_file(workspace_id, file_id);
    return file_to_dto(f);
}

FileContentDto ApiService::get_file_content(int64_t workspace_id, int64_t file_id,
                                            std::optional<int64_t> start_line,
                                            std::optional<int64_t> end_line,
                                            std::optional<int64_t> start_byte,
                                            std::optional<int64_t> end_byte) {
    auto ws = require_workspace(workspace_id);
    auto f = require_file(workspace_id, file_id);

    auto captured = filesystem::capture_file(f.path, policy_.max_file_size_bytes, ws.root_path);
    if (!captured.has_value()) {
        throw ApiError::not_found("file_read_error",
                                  "Failed to read file from disk: " + f.relative_path);
    }

    if (captured->is_binary) {
        return FileContentDto{
            .file_id = f.id,
            .path = f.relative_path,
            .content = "",
            .total_size_bytes = static_cast<int64_t>(captured->byte_size),
            .total_lines = 0,
            .start_line = 0,
            .end_line = 0,
            .start_byte = 0,
            .end_byte = 0,
            .is_binary = true,
            .content_hash = captured->content_hash,
        };
    }

    std::string_view full_text = captured->as_string_view();
    int64_t total_size = static_cast<int64_t>(full_text.size());
    auto spans = compute_line_spans(full_text);
    int64_t total_lines = static_cast<int64_t>(spans.size());

    if (start_byte.has_value() || end_byte.has_value()) {
        int64_t sb = std::clamp(start_byte.value_or(0), 0L, total_size);
        int64_t eb = std::clamp(end_byte.value_or(total_size), sb, total_size);
        std::string sliced(full_text.substr(static_cast<size_t>(sb), static_cast<size_t>(eb - sb)));
        auto sliced_spans = compute_line_spans(sliced);
        int64_t sl_count = static_cast<int64_t>(sliced_spans.size());
        return FileContentDto{
            .file_id = f.id,
            .path = f.relative_path,
            .content = std::move(sliced),
            .total_size_bytes = total_size,
            .total_lines = sl_count,
            .start_line = 0,
            .end_line = sl_count > 0 ? sl_count - 1 : 0,
            .start_byte = sb,
            .end_byte = eb,
            .is_binary = false,
            .content_hash = captured->content_hash,
        };
    }

    if (!start_line.has_value() && !end_line.has_value()) {
        return FileContentDto{
            .file_id = f.id,
            .path = f.relative_path,
            .content = std::string(full_text),
            .total_size_bytes = total_size,
            .total_lines = total_lines,
            .start_line = 0,
            .end_line = total_lines > 0 ? total_lines - 1 : 0,
            .start_byte = 0,
            .end_byte = total_size,
            .is_binary = false,
            .content_hash = captured->content_hash,
        };
    }

    int64_t sl = std::clamp(start_line.value_or(0), 0L, std::max(0L, total_lines - 1));
    int64_t el = std::clamp(end_line.value_or(total_lines - 1), sl, std::max(0L, total_lines - 1));

    int64_t actual_start_byte = 0;
    int64_t actual_end_byte = 0;
    std::string sliced_content;

    if (!spans.empty()) {
        actual_start_byte = spans[static_cast<size_t>(sl)].start_byte;
        actual_end_byte = spans[static_cast<size_t>(el)].content_end_byte;
        if (actual_end_byte >= actual_start_byte) {
            sliced_content = std::string(
                full_text.substr(static_cast<size_t>(actual_start_byte),
                                 static_cast<size_t>(actual_end_byte - actual_start_byte)));
        }
    }

    return FileContentDto{
        .file_id = f.id,
        .path = f.relative_path,
        .content = std::move(sliced_content),
        .total_size_bytes = total_size,
        .total_lines = total_lines,
        .start_line = sl,
        .end_line = el,
        .start_byte = actual_start_byte,
        .end_byte = actual_end_byte,
        .is_binary = false,
        .content_hash = captured->content_hash,
    };
}

void ApiService::invalidate_cdb_cache(int64_t workspace_id) {
    std::lock_guard<std::mutex> lock(cdb_cache_mutex_);
    cdb_cache_.erase(workspace_id);
}

std::shared_ptr<const resolver::CompilationDatabase>
ApiService::get_or_load_cdb(const Workspace& ws, std::filesystem::path* out_effective_path,
                            bool* out_is_auto_detected) {
    std::lock_guard<std::mutex> lock(cdb_cache_mutex_);
    auto it = cdb_cache_.find(ws.id);
    if (it != cdb_cache_.end()) {
        if (out_effective_path != nullptr) {
            *out_effective_path = it->second.second;
        }
        if (out_is_auto_detected != nullptr) {
            *out_is_auto_detected =
                (!ws.compile_commands_path.has_value() || ws.compile_commands_path->empty());
        }
        return it->second.first;
    }

    std::error_code ec;
    std::filesystem::path canonical_root = std::filesystem::canonical(ws.root_path, ec);
    if (ec) {
        canonical_root = ws.root_path;
    }

    std::filesystem::path cdb_path;
    bool is_auto = false;

    if (ws.compile_commands_path.has_value() && !ws.compile_commands_path->empty()) {
        cdb_path = *ws.compile_commands_path;
        if (cdb_path.is_relative()) {
            cdb_path = canonical_root / cdb_path;
        }
        is_auto = false;
    } else {
        std::vector<std::filesystem::path> candidates = {
            canonical_root / "compile_commands.json",
            canonical_root / "build" / "compile_commands.json",
        };
        for (const auto& c : candidates) {
            if (std::filesystem::exists(c)) {
                cdb_path = c;
                is_auto = true;
                break;
            }
        }
    }

    if (out_effective_path != nullptr) {
        *out_effective_path = cdb_path;
    }
    if (out_is_auto_detected != nullptr) {
        *out_is_auto_detected = is_auto;
    }

    if (!cdb_path.empty() && std::filesystem::exists(cdb_path)) {
        auto loaded = resolver::CompilationDatabase::load_file(cdb_path, canonical_root);
        if (loaded) {
            auto comp_db =
                std::make_shared<const resolver::CompilationDatabase>(std::move(*loaded));
            cdb_cache_[ws.id] = {comp_db, cdb_path};
            return comp_db;
        }
    }

    return nullptr;
}

FileCompileCommandResponseDto ApiService::get_file_compile_command(int64_t workspace_id,
                                                                   int64_t file_id) {
    auto ws = require_workspace(workspace_id);
    auto f = require_file(workspace_id, file_id);

    std::filesystem::path effective_path;
    bool is_auto = false;
    auto cdb = get_or_load_cdb(ws, &effective_path, &is_auto);

    FileCompileCommandResponseDto res{
        .file_id = file_id,
        .has_compile_command = false,
        .is_workspace_default = false,
        .database_path = effective_path.empty() ? std::nullopt : std::make_optional(effective_path.string()),
        .is_auto_detected = is_auto,
        .compile_command = std::nullopt,
    };

    const resolver::CompileCommand* cmd = nullptr;
    if (cdb != nullptr) {
        cmd = cdb->find_for_file(f.relative_path);
        if (cmd == nullptr) {
            cmd = cdb->find_for_file(f.path);
        }
    }

    std::optional<resolver::CompileCommand> fallback_cmd;
    if (cmd == nullptr && ws.default_compile_command.has_value() && !ws.default_compile_command->empty()) {
        auto parsed = resolver::CompilationDatabase::parse_command_string(
            *ws.default_compile_command, ws.root_path, f.relative_path, ws.root_path);
        if (parsed) {
            fallback_cmd = std::move(*parsed);
            cmd = &*fallback_cmd;
            res.is_workspace_default = true;
        }
    }

    if (cmd != nullptr) {
        res.has_compile_command = true;
        std::vector<std::string> inc_dirs;
        inc_dirs.reserve(cmd->include_dirs.size());
        for (const auto& d : cmd->include_dirs) {
            inc_dirs.push_back(d.string());
        }

        res.compile_command = CompileCommandDto{
            .directory = cmd->directory.string(),
            .file = cmd->file.string(),
            .output = cmd->output.has_value() ? std::make_optional(cmd->output->string())
                                              : std::nullopt,
            .arguments = cmd->arguments,
            .include_dirs = std::move(inc_dirs),
            .defines = cmd->defines,
            .language_standard = cmd->language_standard,
        };
    }

    return res;
}

WorkspaceCompileCommandsSummaryDto ApiService::get_workspace_compile_commands(int64_t workspace_id) {
    auto ws = require_workspace(workspace_id);
    std::filesystem::path effective_path;
    bool is_auto = false;
    auto cdb = get_or_load_cdb(ws, &effective_path, &is_auto);

    bool exists = !effective_path.empty() && std::filesystem::exists(effective_path);
    size_t total = cdb != nullptr ? cdb->size() : 0;

    return WorkspaceCompileCommandsSummaryDto{
        .configured_path = ws.compile_commands_path,
        .effective_path =
            effective_path.empty() ? std::nullopt : std::make_optional(effective_path.string()),
        .exists = exists,
        .is_auto_detected = is_auto,
        .total_commands = total,
        .default_compile_command = ws.default_compile_command,
    };
}

// ==========================================
// Highlights, Symbols & Outline (F-07)
// ==========================================
HighlightResponseDto ApiService::get_file_highlights(int64_t workspace_id, int64_t file_id) {
    auto ws = require_workspace(workspace_id);
    auto f = require_file(workspace_id, file_id);

    const auto& legend = HighlightLegend::default_legend();
    std::vector<adapters::HighlightToken> tokens;

    auto captured = filesystem::capture_file(f.path, policy_.max_file_size_bytes, ws.root_path);
    if (captured.has_value() && !captured->is_binary) {
        auto lang_res = language_from_string(f.language);
        const auto* grammar =
            lang_res.has_value() ? treesitter::grammar_for_language(*lang_res) : nullptr;
        if (grammar != nullptr) {
            treesitter::Parser parser;
            if (parser.set_language(grammar).has_value()) {
                auto tree_res = parser.parse_string(captured->as_string_view());
                if (tree_res.has_value()) {
                    auto* adapter = adapters::default_adapter_registry().get_adapter(*lang_res);
                    if (adapter != nullptr) {
                        auto res = adapter->highlight(captured->as_string_view(), *tree_res);
                        if (res.has_value() && !res->empty()) {
                            tokens = std::move(*res);
                        }
                    }
                    if (tokens.empty()) {
                        adapters::ReferenceAdapter ref_adapter;
                        auto res = ref_adapter.highlight(captured->as_string_view(), *tree_res);
                        if (res.has_value()) {
                            tokens = std::move(*res);
                        }
                    }
                }
            }
        }
    }

    if (tokens.empty()) {
        auto syms = db_.symbols().list_by_file(file_id);
        for (const auto& s : syms) {
            std::string_view kind_for_legend = s.kind;
            if (s.kind == "type_alias") {
                kind_for_legend = "type";
            } else if (s.kind == "field") {
                kind_for_legend = "property";
            }
            uint32_t type_idx = legend.token_type_index(kind_for_legend)
                                    .value_or(legend.token_type_index("variable").value_or(0));
            uint32_t modifiers = s.is_definition ? legend.encode_modifiers({"definition"})
                                                 : legend.encode_modifiers({"declaration"});
            uint32_t len = static_cast<uint32_t>(s.name.size());
            tokens.push_back(adapters::HighlightToken{
                .line = static_cast<uint32_t>(s.range.start_line),
                .start_column = static_cast<uint32_t>(s.range.start_column),
                .length = len,
                .token_type = type_idx,
                .token_modifiers = modifiers,
                .byte_range =
                    ByteRange{
                        .start = static_cast<uint32_t>(s.range.start_byte),
                        .end = static_cast<uint32_t>(s.range.end_byte),
                    },
                .display_range =
                    DisplayRange{
                        .start_line = static_cast<uint32_t>(s.range.start_line),
                        .start_column = static_cast<uint32_t>(s.range.start_column),
                        .end_line = static_cast<uint32_t>(s.range.end_line),
                        .end_column = static_cast<uint32_t>(s.range.end_column),
                    },
            });
        }
    }

    std::sort(tokens.begin(), tokens.end(),
              [](const adapters::HighlightToken& a, const adapters::HighlightToken& b) {
                  if (a.line != b.line) {
                      return a.line < b.line;
                  }
                  return a.start_column < b.start_column;
              });

    return HighlightResponseDto{
        .file_id = f.id,
        .legend = legend.to_json(),
        .tokens = std::move(tokens),
    };
}

std::vector<SymbolDto> ApiService::get_file_symbols(int64_t workspace_id, int64_t file_id) {
    require_file(workspace_id, file_id);
    auto syms = db_.symbols().list_by_file(file_id);
    std::vector<SymbolDto> dtos;
    dtos.reserve(syms.size());
    for (const auto& s : syms) {
        dtos.push_back(symbol_to_dto(s));
    }
    return dtos;
}

FileOutlineDto ApiService::get_file_outline(int64_t workspace_id, int64_t file_id) {
    require_file(workspace_id, file_id);
    auto syms = db_.symbols().list_by_file(file_id);

    std::vector<SymbolOutlineNodeDto> nodes;
    nodes.reserve(syms.size());
    for (const auto& s : syms) {
        nodes.push_back(SymbolOutlineNodeDto{
            .id = s.id,
            .name = s.name,
            .qualified_name = s.qualified_name,
            .kind = s.kind,
            .signature = s.signature,
            .scope_symbol_id = s.scope_symbol_id,
            .range = source_range_to_dto(s.range),
            .children = {},
        });
    }

    std::unordered_map<int64_t, size_t> id_to_index;
    for (size_t i = 0; i < nodes.size(); ++i) {
        id_to_index[nodes[i].id] = i;
    }

    std::vector<SymbolOutlineNodeDto> root_nodes;
    std::vector<bool> is_child(nodes.size(), false);

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].scope_symbol_id.has_value()) {
            auto it = id_to_index.find(*nodes[i].scope_symbol_id);
            if (it != id_to_index.end() && it->second != i) {
                is_child[i] = true;
            }
        }
    }

    // Build hierarchical tree iteratively
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!is_child[i]) {
            root_nodes.push_back(nodes[i]);
        }
    }

    std::function<void(SymbolOutlineNodeDto&)> populate_children =
        [&](SymbolOutlineNodeDto& parent) {
            for (size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i].scope_symbol_id.has_value() &&
                    *nodes[i].scope_symbol_id == parent.id) {
                    parent.children.push_back(nodes[i]);
                    populate_children(parent.children.back());
                }
            }
        };

    for (auto& root : root_nodes) {
        populate_children(root);
    }

    return FileOutlineDto{
        .file_id = file_id,
        .outline = std::move(root_nodes),
    };
}

std::vector<OccurrenceDto> ApiService::get_file_occurrences(int64_t workspace_id, int64_t file_id,
                                                            std::optional<std::string> kind,
                                                            std::optional<int64_t> start_byte,
                                                            std::optional<int64_t> end_byte) {
    require_file(workspace_id, file_id);
    std::vector<Occurrence> occs;
    if (start_byte.has_value() && end_byte.has_value()) {
        occs = db_.occurrences().find_at_range(file_id, *start_byte, *end_byte);
    } else {
        occs = db_.occurrences().list_by_file(file_id);
    }

    std::vector<OccurrenceDto> dtos;
    for (const auto& o : occs) {
        if (kind.has_value() && !kind->empty() && o.occurrence_kind != *kind) {
            continue;
        }
        dtos.push_back(occurrence_to_dto(o));
    }
    return dtos;
}

// ==========================================
// Workspace Symbols & Detail (F-07, F-08)
// ==========================================
PaginatedResultDto<SymbolDto>
ApiService::list_symbols(int64_t workspace_id, std::optional<std::string> query,
                         std::optional<std::string> kind, std::optional<std::string> language,
                         std::optional<int64_t> file_id, int64_t limit, int64_t offset) {
    require_workspace(workspace_id);
    if (file_id.has_value()) {
        require_file(workspace_id, *file_id);
    }
    int64_t eff_limit = std::clamp(limit, 1L, static_cast<int64_t>(policy_.max_page_size));
    int64_t eff_offset = std::max(0L, offset);

    std::vector<Symbol> candidates;
    if (query.has_value() && !query->empty()) {
        auto fts_hits = db_.fts().search_symbols(workspace_id, *query, 1000, 0);
        for (const auto& hit : fts_hits) {
            auto sym = db_.symbols().get_by_id(hit.id);
            if (sym.has_value() && sym->workspace_id == workspace_id) {
                candidates.push_back(std::move(*sym));
            }
        }
        if (candidates.empty()) {
            candidates = db_.symbols().find_by_name(workspace_id, *query);
        }
    } else if (file_id.has_value()) {
        candidates = db_.symbols().list_by_file(*file_id);
    } else {
        candidates = db_.symbols().list_by_workspace(workspace_id);
    }

    std::vector<SymbolDto> filtered;
    for (const auto& s : candidates) {
        if (kind.has_value() && !kind->empty() && s.kind != *kind) {
            continue;
        }
        if (language.has_value() && !language->empty() && s.language != *language) {
            continue;
        }
        if (file_id.has_value() && s.file_id != *file_id) {
            continue;
        }
        filtered.push_back(symbol_to_dto(s));
    }

    int64_t total = static_cast<int64_t>(filtered.size());
    std::vector<SymbolDto> paged;
    if (eff_offset < total) {
        int64_t end = std::min(total, eff_offset + eff_limit);
        paged.assign(filtered.begin() + static_cast<std::ptrdiff_t>(eff_offset),
                     filtered.begin() + static_cast<std::ptrdiff_t>(end));
    }

    bool has_more = (eff_offset + eff_limit) < total;

    return PaginatedResultDto<SymbolDto>{
        .items = std::move(paged),
        .total = total,
        .limit = eff_limit,
        .offset = eff_offset,
        .has_more = has_more,
    };
}

SymbolDetailDto ApiService::get_symbol_detail(int64_t workspace_id, int64_t symbol_id) {
    auto sym = require_symbol(workspace_id, symbol_id);
    auto file = require_file(workspace_id, sym.file_id);

    auto same_key_symbols = db_.symbols().get_by_key(workspace_id, sym.symbol_key);
    std::vector<SymbolDto> declarations;
    declarations.reserve(same_key_symbols.size());
    for (const auto& s : same_key_symbols) {
        declarations.push_back(symbol_to_dto(s));
    }

    auto callers = db_.references().find_callers(symbol_id);
    auto callees = db_.references().find_callees(symbol_id);
    auto referencers = db_.references().find_referencers(workspace_id, symbol_id, 10000, 0);

    return SymbolDetailDto{
        .symbol = symbol_to_dto(sym),
        .file = file_to_dto(file),
        .declarations = std::move(declarations),
        .callers_count = static_cast<int64_t>(callers.size()),
        .callees_count = static_cast<int64_t>(callees.size()),
        .referencers_count = static_cast<int64_t>(referencers.size()),
    };
}

PaginatedResultDto<ReferencerDto> ApiService::get_symbol_references(int64_t workspace_id,
                                                                    int64_t symbol_id,
                                                                    int64_t limit, int64_t offset) {
    require_symbol(workspace_id, symbol_id);
    int64_t eff_limit = std::clamp(limit, 1L, static_cast<int64_t>(policy_.max_page_size));
    int64_t eff_offset = std::max(0L, offset);

    auto all_refs = db_.references().find_referencers(workspace_id, symbol_id, 100000, 0);
    int64_t total = static_cast<int64_t>(all_refs.size());

    std::vector<ReferencerDto> items;
    if (eff_offset < total) {
        int64_t end = std::min(total, eff_offset + eff_limit);
        for (int64_t i = eff_offset; i < end; ++i) {
            items.push_back(referencer_to_dto(all_refs[static_cast<size_t>(i)]));
        }
    }

    bool has_more = (eff_offset + eff_limit) < total;

    return PaginatedResultDto<ReferencerDto>{
        .items = std::move(items),
        .total = total,
        .limit = eff_limit,
        .offset = eff_offset,
        .has_more = has_more,
    };
}

std::vector<SymbolDto> ApiService::get_symbol_definitions(int64_t workspace_id, int64_t symbol_id) {
    auto sym = require_symbol(workspace_id, symbol_id);
    auto defs = db_.symbols().get_by_key(workspace_id, sym.symbol_key);
    std::vector<SymbolDto> dtos;
    for (const auto& s : defs) {
        if (s.is_definition) {
            dtos.push_back(symbol_to_dto(s));
        }
    }
    if (dtos.empty()) {
        for (const auto& s : defs) {
            dtos.push_back(symbol_to_dto(s));
        }
    }
    return dtos;
}

std::vector<CallerCalleeDto> ApiService::get_symbol_callers(int64_t workspace_id,
                                                            int64_t symbol_id) {
    require_symbol(workspace_id, symbol_id);
    auto callers = db_.references().find_callers(symbol_id);
    std::vector<CallerCalleeDto> dtos;
    dtos.reserve(callers.size());
    for (const auto& c : callers) {
        std::string rel_path;
        if (auto f = db_.files().get_by_id(c.file_id)) {
            rel_path = f->relative_path;
        }
        dtos.push_back(CallerCalleeDto{
            .symbol_id = c.symbol_id,
            .name = c.name,
            .qualified_name = c.qualified_name,
            .file_id = c.file_id,
            .relative_path = std::move(rel_path),
        });
    }
    return dtos;
}

std::vector<CallerCalleeDto> ApiService::get_symbol_callees(int64_t workspace_id,
                                                            int64_t symbol_id) {
    require_symbol(workspace_id, symbol_id);
    auto callees = db_.references().find_callees(symbol_id);
    std::vector<CallerCalleeDto> dtos;
    dtos.reserve(callees.size());
    for (const auto& c : callees) {
        std::string rel_path;
        if (auto f = db_.files().get_by_id(c.file_id)) {
            rel_path = f->relative_path;
        }
        dtos.push_back(CallerCalleeDto{
            .symbol_id = c.symbol_id,
            .name = c.name,
            .qualified_name = c.qualified_name,
            .file_id = c.file_id,
            .relative_path = std::move(rel_path),
        });
    }
    return dtos;
}

SymbolGraphDto ApiService::get_symbol_graph(int64_t workspace_id, int64_t symbol_id, int depth,
                                            size_t max_nodes, size_t max_edges,
                                            const std::vector<std::string>& kinds) {
    auto root_sym = require_symbol(workspace_id, symbol_id);

    int eff_depth = std::clamp(depth, 1, 10);
    size_t eff_max_nodes = std::clamp(max_nodes, size_t{1}, size_t{500});
    size_t eff_max_edges = std::clamp(max_edges, size_t{1}, size_t{1000});

    std::vector<GraphNodeDto> nodes;
    std::vector<GraphEdgeDto> edges;
    std::unordered_set<int64_t> visited_nodes;
    bool truncated = false;

    std::string root_rel_path;
    if (auto f = db_.files().get_by_id(root_sym.file_id)) {
        root_rel_path = f->relative_path;
    }

    nodes.push_back(GraphNodeDto{
        .id = root_sym.id,
        .name = root_sym.name,
        .qualified_name = root_sym.qualified_name,
        .kind = root_sym.kind,
        .file_id = root_sym.file_id,
        .relative_path = std::move(root_rel_path),
    });
    visited_nodes.insert(root_sym.id);

    std::deque<std::pair<int64_t, int>> queue;
    queue.push_back({root_sym.id, 0});

    while (!queue.empty()) {
        auto [curr_id, curr_depth] = queue.front();
        queue.pop_front();

        if (curr_depth >= eff_depth) {
            continue;
        }

        // Outgoing relations
        auto out_rels = db_.relations().find_by_source_symbol(curr_id);
        for (const auto& rel : out_rels) {
            if (!rel.target_symbol_id.has_value())
                continue;

            if (!kinds.empty()) {
                if (std::find(kinds.begin(), kinds.end(), rel.relation_kind) == kinds.end()) {
                    continue;
                }
            }

            int64_t tid = *rel.target_symbol_id;

            if (edges.size() >= eff_max_edges) {
                truncated = true;
                break;
            }

            edges.push_back(GraphEdgeDto{
                .source_symbol_id = curr_id,
                .target_symbol_id = tid,
                .relation_kind = rel.relation_kind,
                .resolution = rel.resolution,
                .confidence = rel.confidence,
            });

            if (!visited_nodes.contains(tid)) {
                if (nodes.size() >= eff_max_nodes) {
                    truncated = true;
                    continue;
                }
                if (auto target_sym = db_.symbols().get_by_id(tid)) {
                    std::string target_rel;
                    if (auto tf = db_.files().get_by_id(target_sym->file_id)) {
                        target_rel = tf->relative_path;
                    }
                    nodes.push_back(GraphNodeDto{
                        .id = target_sym->id,
                        .name = target_sym->name,
                        .qualified_name = target_sym->qualified_name,
                        .kind = target_sym->kind,
                        .file_id = target_sym->file_id,
                        .relative_path = std::move(target_rel),
                    });
                    visited_nodes.insert(tid);
                    queue.push_back({tid, curr_depth + 1});
                }
            }
        }

        // Callers / callees
        if (kinds.empty() || std::find(kinds.begin(), kinds.end(), "calls") != kinds.end()) {
            auto callers = db_.references().find_callers(curr_id);
            for (const auto& c : callers) {
                if (!c.symbol_id.has_value())
                    continue;
                int64_t cid = *c.symbol_id;

                if (edges.size() >= eff_max_edges) {
                    truncated = true;
                    break;
                }

                edges.push_back(GraphEdgeDto{
                    .source_symbol_id = cid,
                    .target_symbol_id = curr_id,
                    .relation_kind = "calls",
                    .resolution = "resolved",
                    .confidence = 1.0,
                });

                if (!visited_nodes.contains(cid)) {
                    if (nodes.size() >= eff_max_nodes) {
                        truncated = true;
                        continue;
                    }
                    std::string caller_rel;
                    if (auto cf = db_.files().get_by_id(c.file_id)) {
                        caller_rel = cf->relative_path;
                    }
                    nodes.push_back(GraphNodeDto{
                        .id = cid,
                        .name = c.name,
                        .qualified_name = c.qualified_name,
                        .kind = "function",
                        .file_id = c.file_id,
                        .relative_path = std::move(caller_rel),
                    });
                    visited_nodes.insert(cid);
                    queue.push_back({cid, curr_depth + 1});
                }
            }
        }
    }

    return SymbolGraphDto{
        .root_symbol_id = root_sym.id,
        .nodes = std::move(nodes),
        .edges = std::move(edges),
        .truncated = truncated,
    };
}

// ==========================================
// Search (F-09)
// ==========================================
PaginatedResultDto<SourceSearchHitDto> ApiService::search_source(int64_t workspace_id,
                                                                 const std::string& query,
                                                                 int64_t limit, int64_t offset) {
    require_workspace(workspace_id);
    int64_t eff_limit = std::clamp(limit, 1L, static_cast<int64_t>(policy_.max_page_size));
    int64_t eff_offset = std::max(0L, offset);

    int64_t total = db_.fts().count_search_files(workspace_id, query);
    auto hits = db_.fts().search_files(workspace_id, query, eff_limit, eff_offset);

    std::vector<SourceSearchHitDto> items;
    items.reserve(hits.size());
    for (const auto& hit : hits) {
        std::string rel_path;
        if (auto f = db_.files().get_by_id(hit.file_id)) {
            rel_path = f->relative_path;
        }
        items.push_back(SourceSearchHitDto{
            .file_id = hit.file_id,
            .relative_path = std::move(rel_path),
            .snippet = hit.snippet,
            .rank = hit.rank,
        });
    }

    bool has_more = (eff_offset + static_cast<int64_t>(items.size())) < total;

    return PaginatedResultDto<SourceSearchHitDto>{
        .items = std::move(items),
        .total = total,
        .limit = eff_limit,
        .offset = eff_offset,
        .has_more = has_more,
    };
}

PaginatedResultDto<SymbolSearchHitDto> ApiService::search_symbols(int64_t workspace_id,
                                                                  const std::string& query,
                                                                  int64_t limit, int64_t offset) {
    require_workspace(workspace_id);
    int64_t eff_limit = std::clamp(limit, 1L, static_cast<int64_t>(policy_.max_page_size));
    int64_t eff_offset = std::max(0L, offset);

    int64_t total = db_.fts().count_search_symbols(workspace_id, query);
    std::vector<SymbolSearchHitDto> items;

    if (total > 0) {
        auto hits = db_.fts().search_symbols(workspace_id, query, eff_limit, eff_offset);
        items.reserve(hits.size());
        for (const auto& hit : hits) {
            std::string rel_path;
            if (auto f = db_.files().get_by_id(hit.file_id)) {
                rel_path = f->relative_path;
            }
            items.push_back(SymbolSearchHitDto{
                .id = hit.id,
                .file_id = hit.file_id,
                .relative_path = std::move(rel_path),
                .name = hit.name,
                .qualified_name = hit.qualified_name,
                .kind = hit.kind,
                .rank = hit.rank,
            });
        }
    } else {
        auto by_name = db_.symbols().find_by_name(workspace_id, query);
        total = static_cast<int64_t>(by_name.size());
        if (eff_offset < total) {
            int64_t end = std::min(total, eff_offset + eff_limit);
            for (int64_t i = eff_offset; i < end; ++i) {
                const auto& s = by_name[static_cast<size_t>(i)];
                std::string rel_path;
                if (auto f = db_.files().get_by_id(s.file_id)) {
                    rel_path = f->relative_path;
                }
                items.push_back(SymbolSearchHitDto{
                    .id = s.id,
                    .file_id = s.file_id,
                    .relative_path = std::move(rel_path),
                    .name = s.name,
                    .qualified_name = s.qualified_name,
                    .kind = s.kind,
                    .rank = 1.0,
                });
            }
        }
    }

    bool has_more = (eff_offset + static_cast<int64_t>(items.size())) < total;

    return PaginatedResultDto<SymbolSearchHitDto>{
        .items = std::move(items),
        .total = total,
        .limit = eff_limit,
        .offset = eff_offset,
        .has_more = has_more,
    };
}

std::vector<DiagnosticDto>
ApiService::get_workspace_diagnostics(int64_t workspace_id,
                                      const std::optional<std::string>& severity, int64_t limit,
                                      int64_t offset) {
    require_workspace(workspace_id);
    auto raw_diags = db_.diagnostics().list_by_workspace(workspace_id, severity, limit, offset);
    std::vector<DiagnosticDto> result;
    result.reserve(raw_diags.size());

    std::unordered_map<int64_t, std::string> file_paths;
    for (const auto& d : raw_diags) {
        std::optional<std::string> rel_path = std::nullopt;
        if (d.file_id.has_value()) {
            auto it = file_paths.find(*d.file_id);
            if (it != file_paths.end()) {
                rel_path = it->second;
            } else {
                auto f = db_.files().get_by_id(*d.file_id);
                if (f) {
                    file_paths[*d.file_id] = f->relative_path;
                    rel_path = f->relative_path;
                }
            }
        }
        result.push_back(DiagnosticDto{
            .id = d.id,
            .workspace_id = d.workspace_id,
            .file_id = d.file_id,
            .relative_path = std::move(rel_path),
            .severity = d.severity,
            .source = d.source,
            .code = d.code,
            .message = d.message,
            .line = d.start_line,
            .column = d.start_column,
            .end_line = d.end_line,
            .end_column = d.end_column,
            .created_at = d.created_at,
        });
    }
    return result;
}

std::vector<DiagnosticDto> ApiService::get_file_diagnostics(int64_t workspace_id, int64_t file_id) {
    auto f = require_file(workspace_id, file_id);
    auto raw_diags = db_.diagnostics().list_by_file(file_id);
    std::vector<DiagnosticDto> result;
    result.reserve(raw_diags.size());

    for (const auto& d : raw_diags) {
        result.push_back(DiagnosticDto{
            .id = d.id,
            .workspace_id = d.workspace_id,
            .file_id = d.file_id,
            .relative_path = f.relative_path,
            .severity = d.severity,
            .source = d.source,
            .code = d.code,
            .message = d.message,
            .line = d.start_line,
            .column = d.start_column,
            .end_line = d.end_line,
            .end_column = d.end_column,
            .created_at = d.created_at,
        });
    }
    return result;
}

} // namespace codelenses::server
