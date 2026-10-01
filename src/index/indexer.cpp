#include "codelenses/index/indexer.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>
#include <unordered_set>

#include "codelenses/db/statement.hpp"
#include "codelenses/db/transaction.hpp"
#include "codelenses/filesystem/file_capture.hpp"
#include "codelenses/filesystem/path.hpp"
#include "codelenses/index/bounded_queue.hpp"
#include "codelenses/index/incremental_planner.hpp"
#include "codelenses/index/thread_pool.hpp"
#include "codelenses/resolver/compile_commands.hpp"
#include "codelenses/resolver/resolver.hpp"
#include "codelenses/resolver/symbol_key.hpp"
#include <nlohmann/json.hpp>

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
    return resolver::generate_symbol_key("unknown", rel_path, to_string(kind), name, signature,
                                         start_byte);
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
        return "inheritance";
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
                              const std::stop_token& stop_token,
                              const std::filesystem::path& workspace_root,
                              const resolver::CompilationDatabase* cdb = nullptr,
                              const resolver::CompileCommand* default_cmd = nullptr) {
    ExtractionResult result;
    result.planned = planned;

    if (stop_token.stop_requested()) {
        result.success = false;
        return result;
    }

    const auto& capture_root = planned.source_root.empty() ? workspace_root : planned.source_root;
    auto capture_res = filesystem::capture_file(planned.absolute_path, max_file_size, capture_root);
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
    const auto bom_offset = captured.bom_offset;
    result.index_data.size_bytes = static_cast<int64_t>(captured.byte_size);
    result.index_data.modified_ns = captured.modified_ns;
    result.index_data.content_hash = captured.content_hash;
    result.index_data.is_binary = captured.is_binary;
    result.index_data.last_index_job_id = job_id;

    Language effective_language = planned.language;

    const resolver::CompileCommand* cmd = nullptr;
    if (cdb != nullptr) {
        cmd = cdb->find_for_file(planned.relative_path);
        if (!cmd) {
            cmd = cdb->find_for_file(planned.absolute_path);
        }
    }
    if (cmd == nullptr && default_cmd != nullptr) {
        cmd = default_cmd;
    }

    if (effective_language == Language::c && planned.absolute_path.extension() == ".h") {
        if (cmd != nullptr && cmd->is_cpp()) {
            effective_language = Language::cpp;
        } else if (filesystem::FileDiscovery::looks_like_cpp_content(captured.as_string_view())) {
            effective_language = Language::cpp;
        }
    }

    if (captured.is_binary || effective_language == Language::unknown) {
        result.index_data.language = "unknown";
        return result;
    }

    result.index_data.language = std::string(to_string(effective_language));

    auto* adapter = registry.get_adapter(effective_language);
    if (!adapter) {
        adapter = registry.get_adapter_for_path(planned.absolute_path);
    }

    if (!adapter) {
        // No adapter registered for this language, keep as unparsed known file
        return result;
    }

    Result<adapters::AdapterResult> parse_res;
    if (cmd != nullptr) {
        parse_res =
            adapter->parse(captured.as_string_view(), planned.absolute_path, *cmd, stop_token);
    } else {
        parse_res = adapter->parse(captured.as_string_view(), planned.absolute_path, stop_token);
    }

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
    struct DeclKey {
        uint64_t start;
        uint64_t end;
        std::string_view name;

        bool operator==(const DeclKey& o) const noexcept {
            return start == o.start && end == o.end && name == o.name;
        }
    };
    struct DeclKeyHash {
        size_t operator()(const DeclKey& k) const noexcept {
            size_t h1 = std::hash<uint64_t>{}(k.start);
            size_t h2 = std::hash<uint64_t>{}(k.end);
            size_t h3 = std::hash<std::string_view>{}(k.name);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };
    std::unordered_map<DeclKey, bool, DeclKeyHash> decl_defs;
    decl_defs.reserve(adapter_res.declarations.size());
    for (const auto& d : adapter_res.declarations) {
        decl_defs[{d.range.start, d.range.end, d.symbol_name}] = d.is_definition;
    }

    result.index_data.symbols.reserve(adapter_res.symbols.size());
    for (size_t i = 0; i < adapter_res.symbols.size(); ++i) {
        const auto& s = adapter_res.symbols[i];
        const auto sym_start = static_cast<int64_t>(s.range.start + bom_offset);
        const auto sym_end = static_cast<int64_t>(s.range.end + bom_offset);
        bool is_def = true;
        auto dit = decl_defs.find({s.range.start, s.range.end, s.name});
        if (dit != decl_defs.end()) {
            is_def = dit->second;
        }
        Symbol sym{
            .id = static_cast<int64_t>(i + 1), // temporary 1-based ID for within-file remapping
            .workspace_id = workspace_id,
            .symbol_key = make_symbol_key(planned.relative_path, s.kind,
                                          s.qualified_name.empty() ? s.name : s.qualified_name,
                                          s.signature, sym_start),
            .name = s.name,
            .qualified_name =
                s.qualified_name.empty() ? std::nullopt : std::make_optional(s.qualified_name),
            .kind = std::string(to_string(s.kind)),
            .language = result.index_data.language,
            .signature = s.signature.empty() ? std::nullopt : std::make_optional(s.signature),
            .container_name = s.enclosing_scope,
            .is_definition = is_def,
            .range =
                SourceRange{
                    .start_byte = sym_start,
                    .end_byte = sym_end,
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
        const auto occ_start = static_cast<int64_t>(occ.range.start + bom_offset);
        const auto occ_end = static_cast<int64_t>(occ.range.end + bom_offset);
        auto occurrence_metadata = occ.metadata_json;
        if (effective_language == Language::csharp && occ.enclosing_scope.has_value()) {
            nlohmann::json metadata = nlohmann::json::object();
            if (occurrence_metadata.has_value()) {
                try {
                    auto parsed = nlohmann::json::parse(*occurrence_metadata);
                    if (parsed.is_object())
                        metadata = std::move(parsed);
                } catch (...) {
                }
            }
            metadata["enclosingScope"] = *occ.enclosing_scope;
            occurrence_metadata = metadata.dump();
        }
        Occurrence db_occ{
            .workspace_id = workspace_id,
            .occurrence_kind = fact_kind_to_occurrence_kind(occ.kind),
            .name = occ.written_name,
            .range =
                SourceRange{
                    .start_byte = occ_start,
                    .end_byte = occ_end,
                    .start_line = static_cast<int64_t>(occ.display_range.start_line),
                    .start_column = static_cast<int64_t>(occ.display_range.start_column),
                    .end_line = static_cast<int64_t>(occ.display_range.end_line),
                    .end_column = static_cast<int64_t>(occ.display_range.end_column),
                },
            .confidence = occ.confidence,
            .resolution = "unresolved",
            .metadata_json = occurrence_metadata,
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
                .confidence = occ.confidence,
                .metadata_json = occurrence_metadata,
            };

            // Link source_symbol_id if occurrence has enclosing_scope
            if (occ.enclosing_scope.has_value() && !occ.enclosing_scope->empty()) {
                for (const auto& sym : result.index_data.symbols) {
                    if (sym.name == *occ.enclosing_scope ||
                        (sym.qualified_name.has_value() &&
                         *sym.qualified_name == *occ.enclosing_scope)) {
                        ref.source_symbol_id = sym.id;
                        break;
                    }
                }
            }

            // Fallback to geometric containment if not matched by scope
            if (!ref.source_symbol_id.has_value()) {
                int64_t smallest_len = std::numeric_limits<int64_t>::max();
                for (const auto& sym : result.index_data.symbols) {
                    if (sym.range.start_byte <= db_occ.range.start_byte &&
                        db_occ.range.end_byte <= sym.range.end_byte) {
                        int64_t len = sym.range.end_byte - sym.range.start_byte;
                        if (len < smallest_len) {
                            smallest_len = len;
                            ref.source_symbol_id = sym.id;
                        }
                    }
                }
            }

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
                .start_byte = occ_start,
                .end_byte = occ_end,
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
            .start_byte = static_cast<int64_t>(d.byte_range.start + bom_offset),
            .end_byte = static_cast<int64_t>(d.byte_range.end + bom_offset),
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
                                                   std::stop_token stop,
                                                   std::optional<int64_t> job_id_override) {
    if (options_.queue_capacity == 0) {
        if (job_id_override.has_value()) {
            db_.jobs().finish_job(*job_id_override, "failed", std::nullopt,
                                  "queue_capacity must be greater than zero");
        }
        return unexpected_result<IndexResult>(ErrorCode::invalid_argument,
                                              "queue_capacity must be greater than zero");
    }
    if (options_.queue_max_bytes == 0) {
        if (job_id_override.has_value()) {
            db_.jobs().finish_job(*job_id_override, "failed", std::nullopt,
                                  "queue_max_bytes must be greater than zero");
        }
        return unexpected_result<IndexResult>(ErrorCode::invalid_argument,
                                              "queue_max_bytes must be greater than zero");
    }

    auto ws = db_.workspaces().get_by_id(workspace_id);
    if (!ws) {
        if (job_id_override.has_value()) {
            db_.jobs().finish_job(*job_id_override, "failed", std::nullopt, "workspace not found");
        }
        return unexpected_result<IndexResult>(ErrorCode::not_found, "workspace not found");
    }

    auto stop_source = std::make_shared<std::stop_source>();
    std::stop_callback stop_cb(stop, [stop_source]() { stop_source->request_stop(); });

    {
        std::lock_guard lock(jobs_mutex_);
        if (active_workspace_jobs_.contains(workspace_id)) {
            if (job_id_override.has_value()) {
                db_.jobs().finish_job(*job_id_override, "failed", std::nullopt,
                                      "an indexing job is already active for this workspace");
            }
            return unexpected_result<IndexResult>(
                ErrorCode::conflict, "an indexing job is already active for this workspace");
        }
        active_workspace_jobs_[workspace_id] = stop_source;
        if (job_id_override.has_value()) {
            job_to_workspace_map_[*job_id_override] = workspace_id;
        }
    }

    auto job_stop = stop_source->get_token();

    // Serialize indexing jobs that share the database connection
    auto coord = db_.indexing_coordinator();
    std::unique_lock<std::mutex> coord_lock(coord->mutex);

    std::stop_callback coord_stop_cb(job_stop, [&]() { coord->cv.notify_all(); });
    coord->cv.wait(coord_lock, [&] { return !coord->job_running || job_stop.stop_requested(); });

    if (job_stop.stop_requested()) {
        {
            std::lock_guard lock(jobs_mutex_);
            active_workspace_jobs_.erase(workspace_id);
            if (job_id_override.has_value()) {
                job_to_workspace_map_.erase(*job_id_override);
            }
        }
        if (job_id_override.has_value()) {
            db_.jobs().finish_job(*job_id_override, "canceled", std::nullopt,
                                  "indexing job was cancelled");
        }
        return unexpected_result<IndexResult>(ErrorCode::cancelled, "indexing job was cancelled");
    }

    coord->job_running = true;
    coord_lock.unlock();

    struct JobCleanup {
        IndexingPipeline& self;
        int64_t ws_id;
        int64_t j_id;
        std::shared_ptr<IndexingCoordinator> coord;
        ~JobCleanup() {
            {
                std::lock_guard lock(self.jobs_mutex_);
                self.active_workspace_jobs_.erase(ws_id);
                if (j_id > 0) {
                    self.job_to_workspace_map_.erase(j_id);
                }
            }
            if (coord) {
                std::lock_guard lock(coord->mutex);
                coord->job_running = false;
                coord->cv.notify_all();
            }
        }
    } cleanup{*this, workspace_id, job_id_override.value_or(0), coord};

    // A queued job may have waited while its workspace was edited or deleted.
    ws = db_.workspaces().get_by_id(workspace_id);
    if (!ws)
        return unexpected_result<IndexResult>(ErrorCode::not_found, "workspace not found");

    int64_t job_id = 0;
    if (job_id_override.has_value()) {
        job_id = *job_id_override;
        db_.jobs().update_status(job_id, "running");
    } else {
        IndexJob job{
            .workspace_id = workspace_id,
            .job_type = job_type,
            .status = "running",
            .requested_mode = (force_full ? "full" : "incremental"),
        };
        job_id = db_.jobs().create(job);
        db_.jobs().update_status(job_id, "running");
    }
    cleanup.j_id = job_id;
    {
        std::lock_guard lock(jobs_mutex_);
        job_to_workspace_map_[job_id] = workspace_id;
    }

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

    // Load compilation database if configured or present in workspace
    std::shared_ptr<const resolver::CompilationDatabase> comp_db;
    std::filesystem::path cdb_path;
    if (ws->compile_commands_path.has_value() && !ws->compile_commands_path->empty()) {
        cdb_path = *ws->compile_commands_path;
        if (cdb_path.is_relative()) {
            cdb_path = canonical_root / cdb_path;
        }
    } else {
        std::vector<std::filesystem::path> candidates = {
            canonical_root / "compile_commands.json",
            canonical_root / "build" / "compile_commands.json",
        };
        for (const auto& c : candidates) {
            if (std::filesystem::exists(c)) {
                cdb_path = c;
                break;
            }
        }
    }

    if (!cdb_path.empty() && std::filesystem::exists(cdb_path)) {
        auto loaded = resolver::CompilationDatabase::load_file(cdb_path, canonical_root);
        if (loaded) {
            comp_db = std::make_shared<const resolver::CompilationDatabase>(std::move(*loaded));
        }
    }

    const auto* raw_cdb = comp_db.get();

    std::shared_ptr<const resolver::CompileCommand> default_cmd;
    if (ws->default_compile_command.has_value() && !ws->default_compile_command->empty()) {
        auto parsed = resolver::CompilationDatabase::parse_command_string(
            *ws->default_compile_command, canonical_root, "", canonical_root);
        if (parsed) {
            default_cmd = std::make_shared<const resolver::CompileCommand>(std::move(*parsed));
        }
    }

    // File discovery
    filesystem::DiscoveryOptions disc_opts;
    disc_opts.include_patterns = ws->include_patterns;
    disc_opts.exclude_patterns = ws->exclude_patterns;
    disc_opts.default_ignores = ws->default_ignores;
    disc_opts.max_file_size_bytes = options_.max_file_size_bytes;

    auto lib_profile = db_.workspace_settings().get_profile_by_workspace(workspace_id);
    if (lib_profile) {
        if (!lib_profile->include_patterns.empty()) {
            disc_opts.include_patterns = lib_profile->include_patterns;
        }
        if (!lib_profile->exclude_patterns.empty()) {
            disc_opts.exclude_patterns = lib_profile->exclude_patterns;
        }
        if (lib_profile->language == "cpp") {
            disc_opts.allow_extensionless_headers = true;
            disc_opts.extensionless_language = Language::cpp;
            disc_opts.extension_overrides[".tcc"] = Language::cpp;
            disc_opts.extension_overrides[".inc"] = Language::cpp;
            disc_opts.ambiguous_header_mode = Language::cpp;
        } else if (lib_profile->language == "c") {
            disc_opts.extension_overrides[".inc"] = Language::c;
            disc_opts.ambiguous_header_mode = Language::c;
        }
    }
    if (!disc_opts.ambiguous_header_mode.has_value()) {
        if (default_cmd != nullptr) {
            if (default_cmd->is_cpp()) {
                disc_opts.ambiguous_header_mode = Language::cpp;
            } else if (default_cmd->is_c()) {
                disc_opts.ambiguous_header_mode = Language::c;
            }
        }
        if (!disc_opts.ambiguous_header_mode.has_value() && raw_cdb != nullptr &&
            !raw_cdb->empty()) {
            for (const auto& entry : raw_cdb->entries()) {
                if (entry.is_cpp()) {
                    disc_opts.ambiguous_header_mode = Language::cpp;
                    break;
                }
            }
        }
    }

    if (lib_profile.has_value()) {
        adapters::CompileCommandContext lib_cmd;
        if (default_cmd != nullptr) {
            lib_cmd = *default_cmd;
        }
        lib_cmd.conservative_preproc = true;
        if (!lib_cmd.language_standard.has_value() && lib_profile->language_standard.has_value()) {
            lib_cmd.language_standard = lib_profile->language_standard;
        }
        if (lib_profile->sysroot.has_value() && !lib_profile->sysroot->empty()) {
            lib_cmd.sysroot = std::filesystem::path(*lib_profile->sysroot);
        }
        for (const auto& d : lib_profile->defines) {
            lib_cmd.defines.push_back(d);
        }
        for (const auto& r : lib_profile->default_include_roots) {
            lib_cmd.search_entries.push_back(adapters::IncludeSearchEntry{
                .directory = std::filesystem::path(r),
                .category = adapters::IncludeCategory::default_toolchain,
                .origin = adapters::IncludeOrigin::configured_profile,
                .role = (lib_profile->language == "cpp") ? adapters::RootRole::cpp_library
                                                         : adapters::RootRole::c_runtime,
            });
        }
        default_cmd = std::make_shared<const resolver::CompileCommand>(std::move(lib_cmd));
    }
    const auto* raw_default_cmd = default_cmd.get();

    filesystem::FileDiscovery discovery(disc_opts);
    std::vector<filesystem::DiscoveredFile> discovered;
    if (lib_profile.has_value()) {
        auto roots = db_.workspace_settings().list_source_roots(lib_profile->id);
        if (roots.empty()) {
            roots.push_back(WorkspaceSourceRoot{
                .id = 0,
                .profile_id = lib_profile->id,
                .ordinal = 0,
                .path = canonical_root.string(),
            });
        }
        std::unordered_set<std::string> seen_canonical_files;
        const bool multiple_roots = roots.size() > 1;
        for (const auto& source_root : roots) {
            std::filesystem::path source_path(source_root.path);
            if (source_path.is_relative())
                source_path = canonical_root / source_path;
            auto root_result = filesystem::canonicalize_workspace_root(source_path);
            if (!root_result) {
                auto error = root_result.error();
                db_.jobs().finish_job(job_id, "failed", std::nullopt, error.message);
                ws->status = WorkspaceStatus::error;
                ws->last_error = error.message;
                db_.workspaces().update(*ws);
                return std::unexpected(error);
            }
            auto root_files = discovery.discover(*root_result);
            if (!root_files) {
                auto error = root_files.error();
                db_.jobs().finish_job(job_id, "failed", std::nullopt, error.message);
                ws->status = WorkspaceStatus::error;
                ws->last_error = error.message;
                db_.workspaces().update(*ws);
                return std::unexpected(error);
            }
            for (auto file : *root_files) {
                if (lib_profile->language == "csharp") {
                    auto extension = file.absolute_path.extension().string();
                    std::transform(
                        extension.begin(), extension.end(), extension.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (extension != ".cs")
                        continue;
                }
                std::error_code canonical_ec;
                auto canonical_file =
                    std::filesystem::weakly_canonical(file.absolute_path, canonical_ec);
                const std::string canonical_key =
                    canonical_ec ? file.absolute_path.lexically_normal().string()
                                 : canonical_file.string();
                if (!seen_canonical_files.insert(canonical_key).second) {
                    continue;
                }
                if (multiple_roots || lib_profile->language == "csharp") {
                    file.relative_path =
                        "root-" + std::to_string(source_root.id) + "/" + file.relative_path;
                }
                discovered.push_back(std::move(file));
            }
        }
        std::sort(discovered.begin(), discovered.end(),
                  [](const auto& a, const auto& b) { return a.relative_path < b.relative_path; });
    } else {
        auto disc_res = discovery.discover(canonical_root);
        if (!disc_res) {
            db_.jobs().finish_job(job_id, "failed", std::nullopt, disc_res.error().message);
            ws->status = WorkspaceStatus::error;
            ws->last_error = disc_res.error().message;
            db_.workspaces().update(*ws);
            return std::unexpected(disc_res.error());
        }
        discovered = std::move(*disc_res);
    }

    // Incremental planning
    auto db_states = db_.files().get_file_states(workspace_id);
    auto plan =
        plan_indexing(workspace_id, discovered, db_states, force_full || (job_type == "full"));

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

    std::size_t idx = 0;
    bool cancelled = false;

    while (idx < plan.files_to_process.size()) {
        if (job_stop.stop_requested()) {
            cancelled = true;
            break;
        }

        std::size_t current_batch = 0;
        std::size_t current_batch_bytes = 0;
        while (idx + current_batch < plan.files_to_process.size()) {
            const auto& planned = plan.files_to_process[idx + current_batch];
            if (current_batch > 0 && current_batch >= options_.queue_capacity) {
                break;
            }
            if (current_batch > 0 &&
                (current_batch_bytes + planned.file_size > options_.queue_max_bytes)) {
                break;
            }
            current_batch_bytes += planned.file_size;
            current_batch++;
        }
        std::vector<std::future<ExtractionResult>> futures;
        futures.reserve(current_batch);

        for (std::size_t i = 0; i < current_batch; ++i) {
            const auto& planned = plan.files_to_process[idx + i];
            futures.push_back(pool.submit([this, workspace_id, job_id, planned, job_stop,
                                           canonical_root, raw_cdb, raw_default_cmd] {
                return extract_file(workspace_id, job_id, planned, registry_,
                                    options_.max_file_size_bytes, job_stop, canonical_root, raw_cdb,
                                    raw_default_cmd);
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

            // If file capture/read failed, do NOT replace file index!
            if (!ext_res.success) {
                if (ext_res.planned.file_id > 0) {
                    plan.keep_file_ids.push_back(ext_res.planned.file_id);
                    for (auto& diag : ext_res.diagnostics) {
                        diag.file_id = ext_res.planned.file_id;
                    }
                }
                if (!ext_res.diagnostics.empty()) {
                    db_.diagnostics().insert_batch(ext_res.diagnostics);
                }
                continue;
            }

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

    // Workspace resolution and relationship builder pass (Section E)
    resolver::WorkspaceResolver workspace_resolver(db_);
    auto resolve_res = workspace_resolver.resolve_workspace(workspace_id, job_stop);
    if (!resolve_res && resolve_res.error().code == ErrorCode::cancelled) {
        db_.jobs().finish_job(job_id, "cancelled", std::nullopt,
                              "Indexing job was cancelled during resolution");
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
            .error_message = "Indexing job was cancelled during resolution",
        };
    }

    // Only direct consumers need a refresh; do not recursively follow cycles.
    if (resolve_res) {
        for (auto consumer_id : db_.workspaces().consumer_ids(workspace_id)) {
            if (job_stop.stop_requested())
                break;
            auto refreshed = workspace_resolver.resolve_workspace(consumer_id, job_stop);
            if (refreshed)
                db_.workspaces().increment_revision(consumer_id);
            else
                db_.workspaces().update_status(consumer_id, WorkspaceStatus::error,
                                               refreshed.error().message);
        }
    }

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
