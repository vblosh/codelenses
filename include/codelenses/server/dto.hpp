#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "codelenses/domain/diagnostic.hpp"
#include "codelenses/domain/file.hpp"
#include "codelenses/domain/job.hpp"
#include "codelenses/domain/occurrence.hpp"
#include "codelenses/domain/range.hpp"
#include "codelenses/domain/reference.hpp"
#include "codelenses/domain/relation.hpp"
#include "codelenses/domain/symbol.hpp"
#include "codelenses/domain/workspace.hpp"
#include "codelenses/parser/highlight.hpp"
#include <nlohmann/json.hpp>

namespace codelenses::server {

// ==========================================
// Generic Paginated Response Envelope
// ==========================================
template <typename T>
struct PaginatedResultDto {
    std::vector<T> items;
    int64_t total{0};
    int64_t limit{50};
    int64_t offset{0};
    bool has_more{false};
};

template <typename T>
void to_json(nlohmann::json& j, const PaginatedResultDto<T>& p) {
    j = nlohmann::json{
        {"items", p.items},   {"total", p.total},      {"limit", p.limit},
        {"offset", p.offset}, {"hasMore", p.has_more},
    };
}

// ==========================================
// Workspace DTOs (F-04)
// ==========================================
struct CreateWorkspaceRequest {
    std::string root_path;
    std::string name;
    std::vector<std::string> include_patterns;
    std::vector<std::string> exclude_patterns;
    std::vector<std::string> default_ignores;
    std::optional<std::string> compile_commands_path{std::nullopt};
};

inline void from_json(const nlohmann::json& j, CreateWorkspaceRequest& req) {
    if (j.contains("rootPath"))
        req.root_path = j["rootPath"].get<std::string>();
    else if (j.contains("root_path"))
        req.root_path = j["root_path"].get<std::string>();

    if (j.contains("name"))
        req.name = j["name"].get<std::string>();

    if (j.contains("includePatterns"))
        req.include_patterns = j["includePatterns"].get<std::vector<std::string>>();
    else if (j.contains("include_patterns"))
        req.include_patterns = j["include_patterns"].get<std::vector<std::string>>();

    if (j.contains("excludePatterns"))
        req.exclude_patterns = j["excludePatterns"].get<std::vector<std::string>>();
    else if (j.contains("exclude_patterns"))
        req.exclude_patterns = j["exclude_patterns"].get<std::vector<std::string>>();

    if (j.contains("defaultIgnores"))
        req.default_ignores = j["defaultIgnores"].get<std::vector<std::string>>();
    else if (j.contains("default_ignores"))
        req.default_ignores = j["default_ignores"].get<std::vector<std::string>>();

    if (j.contains("compileCommandsPath") && !j["compileCommandsPath"].is_null())
        req.compile_commands_path = j["compileCommandsPath"].get<std::string>();
    else if (j.contains("compile_commands_path") && !j["compile_commands_path"].is_null())
        req.compile_commands_path = j["compile_commands_path"].get<std::string>();
}

struct UpdateWorkspaceRequest {
    std::optional<std::string> name{std::nullopt};
    std::optional<std::vector<std::string>> include_patterns{std::nullopt};
    std::optional<std::vector<std::string>> exclude_patterns{std::nullopt};
    std::optional<std::string> compile_commands_path{std::nullopt};
};

inline void from_json(const nlohmann::json& j, UpdateWorkspaceRequest& req) {
    if (j.contains("name") && !j["name"].is_null())
        req.name = j["name"].get<std::string>();

    if (j.contains("includePatterns") && !j["includePatterns"].is_null())
        req.include_patterns = j["includePatterns"].get<std::vector<std::string>>();
    else if (j.contains("include_patterns") && !j["include_patterns"].is_null())
        req.include_patterns = j["include_patterns"].get<std::vector<std::string>>();

    if (j.contains("excludePatterns") && !j["excludePatterns"].is_null())
        req.exclude_patterns = j["excludePatterns"].get<std::vector<std::string>>();
    else if (j.contains("exclude_patterns") && !j["exclude_patterns"].is_null())
        req.exclude_patterns = j["exclude_patterns"].get<std::vector<std::string>>();

    if (j.contains("compileCommandsPath") && !j["compileCommandsPath"].is_null())
        req.compile_commands_path = j["compileCommandsPath"].get<std::string>();
    else if (j.contains("compile_commands_path") && !j["compile_commands_path"].is_null())
        req.compile_commands_path = j["compile_commands_path"].get<std::string>();
}

struct WorkspaceDto {
    int64_t id{0};
    std::string root_path;
    std::string name;
    std::vector<std::string> include_patterns;
    std::vector<std::string> exclude_patterns;
    std::vector<std::string> default_ignores;
    std::optional<std::string> compile_commands_path;
    int64_t revision{0};
    std::string status{"idle"};
    std::optional<std::string> last_error;
    std::string created_at;
    std::string updated_at;
};

inline void to_json(nlohmann::json& j, const WorkspaceDto& w) {
    j = nlohmann::json{
        {"id", w.id},
        {"rootPath", w.root_path},
        {"name", w.name},
        {"includePatterns", w.include_patterns},
        {"excludePatterns", w.exclude_patterns},
        {"defaultIgnores", w.default_ignores},
        {"revision", w.revision},
        {"status", w.status},
        {"createdAt", w.created_at},
        {"updatedAt", w.updated_at},
    };
    if (w.compile_commands_path.has_value())
        j["compileCommandsPath"] = *w.compile_commands_path;
    else
        j["compileCommandsPath"] = nullptr;

    if (w.last_error.has_value())
        j["lastError"] = *w.last_error;
    else
        j["lastError"] = nullptr;
}

inline WorkspaceDto workspace_to_dto(const Workspace& ws) {
    return WorkspaceDto{
        .id = ws.id,
        .root_path = ws.root_path,
        .name = ws.name,
        .include_patterns = ws.include_patterns,
        .exclude_patterns = ws.exclude_patterns,
        .default_ignores = ws.default_ignores,
        .compile_commands_path = ws.compile_commands_path,
        .revision = ws.revision,
        .status = to_string(ws.status),
        .last_error = ws.last_error,
        .created_at = ws.created_at,
        .updated_at = ws.updated_at,
    };
}

// ==========================================
// Job & Status DTOs (F-05)
// ==========================================
struct IndexJobRequest {
    std::string job_type{"incremental"};
    bool force_full{false};
};

inline void from_json(const nlohmann::json& j, IndexJobRequest& req) {
    if (j.contains("jobType"))
        req.job_type = j["jobType"].get<std::string>();
    else if (j.contains("job_type"))
        req.job_type = j["job_type"].get<std::string>();

    if (j.contains("forceFull"))
        req.force_full = j["forceFull"].get<bool>();
    else if (j.contains("force_full"))
        req.force_full = j["force_full"].get<bool>();
}

struct JobDto {
    int64_t id{0};
    int64_t workspace_id{0};
    std::string job_type{"incremental"};
    std::string status{"queued"};
    std::string queued_at;
    std::optional<std::string> started_at;
    std::optional<std::string> finished_at;
    int64_t files_total{0};
    int64_t files_processed{0};
    int64_t files_skipped{0};
    int64_t error_count{0};
    int64_t warning_count{0};
    std::optional<int64_t> workspace_revision;
    std::optional<std::string> error_message;
};

inline void to_json(nlohmann::json& j, const JobDto& job) {
    j = nlohmann::json{
        {"id", job.id},
        {"workspaceId", job.workspace_id},
        {"jobType", job.job_type},
        {"status", job.status},
        {"queuedAt", job.queued_at},
        {"startedAt", job.started_at ? nlohmann::json(*job.started_at) : nullptr},
        {"finishedAt", job.finished_at ? nlohmann::json(*job.finished_at) : nullptr},
        {"filesTotal", job.files_total},
        {"filesProcessed", job.files_processed},
        {"filesSkipped", job.files_skipped},
        {"errorCount", job.error_count},
        {"warningCount", job.warning_count},
        {"workspaceRevision",
         job.workspace_revision ? nlohmann::json(*job.workspace_revision) : nullptr},
        {"errorMessage", job.error_message ? nlohmann::json(*job.error_message) : nullptr},
    };
}

inline JobDto job_to_dto(const IndexJob& j) {
    return JobDto{
        .id = j.id,
        .workspace_id = j.workspace_id,
        .job_type = j.job_type,
        .status = j.status,
        .queued_at = j.queued_at,
        .started_at = j.started_at,
        .finished_at = j.finished_at,
        .files_total = j.files_total,
        .files_processed = j.files_processed,
        .files_skipped = j.files_skipped,
        .error_count = j.error_count,
        .warning_count = j.warning_count,
        .workspace_revision = j.workspace_revision,
        .error_message = j.error_message,
    };
}

struct DiagnosticCountsDto {
    int64_t total{0};
    int64_t errors{0};
    int64_t warnings{0};
    int64_t info{0};
};

inline void to_json(nlohmann::json& j, const DiagnosticCountsDto& c) {
    j = nlohmann::json{
        {"total", c.total},
        {"errors", c.errors},
        {"warnings", c.warnings},
        {"info", c.info},
    };
}

struct DiagnosticDto {
    int64_t id{0};
    int64_t workspace_id{0};
    std::optional<int64_t> file_id{std::nullopt};
    std::optional<std::string> relative_path{std::nullopt};
    std::string severity{"error"};
    std::string source;
    std::string code;
    std::string message;
    std::optional<int64_t> line{std::nullopt};
    std::optional<int64_t> column{std::nullopt};
    std::optional<int64_t> end_line{std::nullopt};
    std::optional<int64_t> end_column{std::nullopt};
    std::string created_at;
};

inline void to_json(nlohmann::json& j, const DiagnosticDto& d) {
    j = nlohmann::json{
        {"id", d.id},
        {"workspaceId", d.workspace_id},
        {"severity", d.severity},
        {"source", d.source},
        {"code", d.code},
        {"message", d.message},
        {"createdAt", d.created_at},
    };
    if (d.file_id.has_value()) {
        j["fileId"] = *d.file_id;
    } else {
        j["fileId"] = nullptr;
    }
    if (d.relative_path.has_value()) {
        j["relativePath"] = *d.relative_path;
    } else {
        j["relativePath"] = nullptr;
    }
    if (d.line.has_value()) {
        j["line"] = *d.line;
    } else {
        j["line"] = nullptr;
    }
    if (d.column.has_value()) {
        j["column"] = *d.column;
    } else {
        j["column"] = nullptr;
    }
    if (d.end_line.has_value()) {
        j["endLine"] = *d.end_line;
    } else {
        j["endLine"] = nullptr;
    }
    if (d.end_column.has_value()) {
        j["endColumn"] = *d.end_column;
    } else {
        j["endColumn"] = nullptr;
    }
}

struct WorkspaceStatusDto {
    int64_t workspace_id{0};
    std::string status{"idle"};
    int64_t revision{0};
    std::optional<JobDto> latest_job{std::nullopt};
    int64_t file_count{0};
    int64_t symbol_count{0};
    DiagnosticCountsDto diagnostic_counts;
};

inline void to_json(nlohmann::json& j, const WorkspaceStatusDto& s) {
    j = nlohmann::json{
        {"workspaceId", s.workspace_id},
        {"status", s.status},
        {"revision", s.revision},
        {"latestJob", s.latest_job.has_value() ? nlohmann::json(*s.latest_job) : nullptr},
        {"fileCount", s.file_count},
        {"symbolCount", s.symbol_count},
        {"diagnosticCounts", s.diagnostic_counts},
    };
}

// ==========================================
// Tree, File Metadata & Range Content (F-06)
// ==========================================
struct TreeNodeDto {
    std::string name;
    std::string path;
    std::string type; // "directory" or "file"
    std::optional<int64_t> file_id{std::nullopt};
    int64_t size_bytes{0};
    bool is_binary{false};
    std::string language{"unknown"};
};

inline void to_json(nlohmann::json& j, const TreeNodeDto& n) {
    j = nlohmann::json{
        {"name", n.name},
        {"path", n.path},
        {"type", n.type},
        {"fileId", n.file_id ? nlohmann::json(*n.file_id) : nullptr},
        {"sizeBytes", n.size_bytes},
        {"isBinary", n.is_binary},
        {"language", n.language},
    };
}

struct WorkspaceTreeDto {
    int64_t workspace_id{0};
    std::string path;
    std::vector<TreeNodeDto> entries;
};

inline void to_json(nlohmann::json& j, const WorkspaceTreeDto& t) {
    j = nlohmann::json{
        {"workspaceId", t.workspace_id},
        {"path", t.path},
        {"entries", t.entries},
    };
}

struct FileMetadataDto {
    int64_t id{0};
    int64_t workspace_id{0};
    std::string path;
    std::string relative_path;
    std::string name;
    std::optional<std::string> extension;
    std::string language;
    std::string encoding;
    int64_t size_bytes{0};
    int64_t modified_ns{0};
    std::optional<std::string> content_hash;
    bool is_binary{false};
    bool is_generated{false};
    bool is_deleted{false};
    std::optional<std::string> indexed_at;
    std::string created_at;
    std::string updated_at;
};

inline void to_json(nlohmann::json& j, const FileMetadataDto& f) {
    j = nlohmann::json{
        {"id", f.id},
        {"workspaceId", f.workspace_id},
        {"path", f.path},
        {"relativePath", f.relative_path},
        {"name", f.name},
        {"extension", f.extension ? nlohmann::json(*f.extension) : nullptr},
        {"language", f.language},
        {"encoding", f.encoding},
        {"sizeBytes", f.size_bytes},
        {"modifiedNs", f.modified_ns},
        {"contentHash", f.content_hash ? nlohmann::json(*f.content_hash) : nullptr},
        {"isBinary", f.is_binary},
        {"isGenerated", f.is_generated},
        {"isDeleted", f.is_deleted},
        {"indexedAt", f.indexed_at ? nlohmann::json(*f.indexed_at) : nullptr},
        {"createdAt", f.created_at},
        {"updatedAt", f.updated_at},
    };
}

inline FileMetadataDto file_to_dto(const FileRecord& f) {
    return FileMetadataDto{
        .id = f.id,
        .workspace_id = f.workspace_id,
        .path = f.path,
        .relative_path = f.relative_path,
        .name = f.name,
        .extension = f.extension,
        .language = f.language,
        .encoding = f.encoding,
        .size_bytes = f.size_bytes,
        .modified_ns = f.modified_ns,
        .content_hash = f.content_hash,
        .is_binary = f.is_binary,
        .is_generated = f.is_generated,
        .is_deleted = f.is_deleted,
        .indexed_at = f.indexed_at,
        .created_at = f.created_at,
        .updated_at = f.updated_at,
    };
}

struct FileContentDto {
    int64_t file_id{0};
    std::string path;
    std::string content;
    int64_t total_size_bytes{0};
    int64_t total_lines{0};
    int64_t start_line{0};
    int64_t end_line{0};
    int64_t start_byte{0};
    int64_t end_byte{0};
    bool is_binary{false};
    std::string content_hash;
};

inline void to_json(nlohmann::json& j, const FileContentDto& c) {
    j = nlohmann::json{
        {"fileId", c.file_id},           {"path", c.path},
        {"content", c.content},          {"totalSizeBytes", c.total_size_bytes},
        {"totalLines", c.total_lines},   {"startLine", c.start_line},
        {"endLine", c.end_line},         {"startByte", c.start_byte},
        {"endByte", c.end_byte},         {"isBinary", c.is_binary},
        {"contentHash", c.content_hash},
    };
}

// ==========================================
// Highlights, Symbols & Outline DTOs (F-07)
// ==========================================
struct HighlightResponseDto {
    int64_t file_id{0};
    nlohmann::json legend;
    std::vector<adapters::HighlightToken> tokens;
};

inline void to_json(nlohmann::json& j, const HighlightResponseDto& h) {
    j = nlohmann::json{
        {"fileId", h.file_id},
        {"legend", h.legend},
        {"tokens", h.tokens},
    };
}

struct PositionDto {
    int64_t line{0};
    int64_t column{0};
    int64_t byte{0};
};

inline void to_json(nlohmann::json& j, const PositionDto& p) {
    j = nlohmann::json{
        {"line", p.line},
        {"column", p.column},
        {"byte", p.byte},
    };
}

struct RangeDto {
    PositionDto start;
    PositionDto end;
};

inline void to_json(nlohmann::json& j, const RangeDto& r) {
    j = nlohmann::json{
        {"start", r.start},
        {"end", r.end},
    };
}

inline RangeDto source_range_to_dto(const SourceRange& r) {
    return RangeDto{
        .start = PositionDto{.line = r.start_line, .column = r.start_column, .byte = r.start_byte},
        .end = PositionDto{.line = r.end_line, .column = r.end_column, .byte = r.end_byte},
    };
}

struct SymbolDto {
    int64_t id{0};
    int64_t workspace_id{0};
    int64_t file_id{0};
    std::string symbol_key;
    std::string name;
    std::optional<std::string> qualified_name;
    std::optional<std::string> display_name;
    std::string kind;
    std::string language;
    std::optional<std::string> signature;
    std::optional<std::string> documentation;
    std::optional<std::string> container_name;
    std::optional<int64_t> scope_symbol_id;
    std::optional<std::string> visibility;
    bool is_definition{false};
    bool is_declaration{false};
    RangeDto range;
    std::string created_at;
};

inline void to_json(nlohmann::json& j, const SymbolDto& s) {
    j = nlohmann::json{
        {"id", s.id},
        {"workspaceId", s.workspace_id},
        {"fileId", s.file_id},
        {"symbolKey", s.symbol_key},
        {"name", s.name},
        {"qualifiedName", s.qualified_name ? nlohmann::json(*s.qualified_name) : nullptr},
        {"displayName", s.display_name ? nlohmann::json(*s.display_name) : nullptr},
        {"kind", s.kind},
        {"language", s.language},
        {"signature", s.signature ? nlohmann::json(*s.signature) : nullptr},
        {"documentation", s.documentation ? nlohmann::json(*s.documentation) : nullptr},
        {"containerName", s.container_name ? nlohmann::json(*s.container_name) : nullptr},
        {"scopeSymbolId", s.scope_symbol_id ? nlohmann::json(*s.scope_symbol_id) : nullptr},
        {"visibility", s.visibility ? nlohmann::json(*s.visibility) : nullptr},
        {"isDefinition", s.is_definition},
        {"isDeclaration", s.is_declaration},
        {"range", s.range},
        {"createdAt", s.created_at},
    };
}

inline SymbolDto symbol_to_dto(const Symbol& s) {
    return SymbolDto{
        .id = s.id,
        .workspace_id = s.workspace_id,
        .file_id = s.file_id,
        .symbol_key = s.symbol_key,
        .name = s.name,
        .qualified_name = s.qualified_name,
        .display_name = s.display_name,
        .kind = s.kind,
        .language = s.language,
        .signature = s.signature,
        .documentation = s.documentation,
        .container_name = s.container_name,
        .scope_symbol_id = s.scope_symbol_id,
        .visibility = s.visibility,
        .is_definition = s.is_definition,
        .is_declaration = s.is_declaration,
        .range = source_range_to_dto(s.range),
        .created_at = s.created_at,
    };
}

struct SymbolOutlineNodeDto {
    int64_t id{0};
    std::string name;
    std::optional<std::string> qualified_name;
    std::string kind;
    std::optional<std::string> signature;
    std::optional<int64_t> scope_symbol_id;
    RangeDto range;
    std::vector<SymbolOutlineNodeDto> children;
};

inline void to_json(nlohmann::json& j, const SymbolOutlineNodeDto& n) {
    j = nlohmann::json{
        {"id", n.id},
        {"name", n.name},
        {"qualifiedName", n.qualified_name ? nlohmann::json(*n.qualified_name) : nullptr},
        {"kind", n.kind},
        {"signature", n.signature ? nlohmann::json(*n.signature) : nullptr},
        {"scopeSymbolId", n.scope_symbol_id ? nlohmann::json(*n.scope_symbol_id) : nullptr},
        {"range", n.range},
        {"children", n.children},
    };
}

struct FileOutlineDto {
    int64_t file_id{0};
    std::vector<SymbolOutlineNodeDto> outline;
};

inline void to_json(nlohmann::json& j, const FileOutlineDto& o) {
    j = nlohmann::json{
        {"fileId", o.file_id},
        {"outline", o.outline},
    };
}

struct OccurrenceDto {
    int64_t id{0};
    int64_t workspace_id{0};
    int64_t file_id{0};
    std::optional<int64_t> symbol_id;
    std::string occurrence_kind;
    std::string name;
    RangeDto range;
    double confidence{1.0};
    std::string resolution;
};

inline void to_json(nlohmann::json& j, const OccurrenceDto& o) {
    j = nlohmann::json{
        {"id", o.id},
        {"workspaceId", o.workspace_id},
        {"fileId", o.file_id},
        {"symbolId", o.symbol_id ? nlohmann::json(*o.symbol_id) : nullptr},
        {"occurrenceKind", o.occurrence_kind},
        {"name", o.name},
        {"range", o.range},
        {"confidence", o.confidence},
        {"resolution", o.resolution},
    };
}

inline OccurrenceDto occurrence_to_dto(const Occurrence& occ) {
    return OccurrenceDto{
        .id = occ.id,
        .workspace_id = occ.workspace_id,
        .file_id = occ.file_id,
        .symbol_id = occ.symbol_id,
        .occurrence_kind = occ.occurrence_kind,
        .name = occ.name,
        .range = source_range_to_dto(occ.range),
        .confidence = occ.confidence,
        .resolution = occ.resolution,
    };
}

// ==========================================
// Navigation & Graph DTOs (F-08)
// ==========================================
struct ReferencerDto {
    int64_t id{0};
    std::string reference_kind;
    std::string name;
    std::string resolution;
    double confidence{1.0};
    RangeDto range;
    int64_t file_id{0};
    std::string relative_path;
    std::optional<int64_t> containing_symbol_id;
    std::optional<std::string> containing_symbol_name;
    std::optional<std::string> containing_qualified_name;
};

inline void to_json(nlohmann::json& j, const ReferencerDto& r) {
    j = nlohmann::json{
        {"id", r.id},
        {"referenceKind", r.reference_kind},
        {"name", r.name},
        {"resolution", r.resolution},
        {"confidence", r.confidence},
        {"range", r.range},
        {"fileId", r.file_id},
        {"relativePath", r.relative_path},
        {"containingSymbolId",
         r.containing_symbol_id ? nlohmann::json(*r.containing_symbol_id) : nullptr},
        {"containingSymbolName",
         r.containing_symbol_name ? nlohmann::json(*r.containing_symbol_name) : nullptr},
        {"containingQualifiedName",
         r.containing_qualified_name ? nlohmann::json(*r.containing_qualified_name) : nullptr},
    };
}

inline ReferencerDto referencer_to_dto(const ReferencerResult& r) {
    return ReferencerDto{
        .id = r.id,
        .reference_kind = r.reference_kind,
        .name = r.name,
        .resolution = r.resolution,
        .confidence = r.confidence,
        .range =
            RangeDto{
                .start = PositionDto{.line = r.start_line, .column = r.start_column, .byte = 0},
                .end = PositionDto{.line = r.end_line, .column = r.end_column, .byte = 0},
            },
        .file_id = r.file_id,
        .relative_path = r.relative_path,
        .containing_symbol_id = r.containing_symbol_id,
        .containing_symbol_name = r.containing_symbol_name,
        .containing_qualified_name = r.containing_qualified_name,
    };
}

struct CallerCalleeDto {
    std::optional<int64_t> symbol_id;
    std::string name;
    std::optional<std::string> qualified_name;
    int64_t file_id{0};
    std::string relative_path;
};

inline void to_json(nlohmann::json& j, const CallerCalleeDto& c) {
    j = nlohmann::json{
        {"symbolId", c.symbol_id ? nlohmann::json(*c.symbol_id) : nullptr},
        {"name", c.name},
        {"qualifiedName", c.qualified_name ? nlohmann::json(*c.qualified_name) : nullptr},
        {"fileId", c.file_id},
        {"relativePath", c.relative_path},
    };
}

struct SymbolDetailDto {
    SymbolDto symbol;
    FileMetadataDto file;
    std::vector<SymbolDto> declarations;
    int64_t callers_count{0};
    int64_t callees_count{0};
    int64_t referencers_count{0};
};

inline void to_json(nlohmann::json& j, const SymbolDetailDto& d) {
    j = nlohmann::json{
        {"symbol", d.symbol},
        {"file", d.file},
        {"declarations", d.declarations},
        {"callersCount", d.callers_count},
        {"calleesCount", d.callees_count},
        {"referencersCount", d.referencers_count},
    };
}

struct GraphNodeDto {
    int64_t id{0};
    std::string name;
    std::optional<std::string> qualified_name;
    std::string kind;
    int64_t file_id{0};
    std::string relative_path;
};

inline void to_json(nlohmann::json& j, const GraphNodeDto& n) {
    j = nlohmann::json{
        {"id", n.id},
        {"name", n.name},
        {"qualifiedName", n.qualified_name ? nlohmann::json(*n.qualified_name) : nullptr},
        {"kind", n.kind},
        {"fileId", n.file_id},
        {"relativePath", n.relative_path},
    };
}

struct GraphEdgeDto {
    int64_t source_symbol_id{0};
    std::optional<int64_t> target_symbol_id;
    std::string relation_kind;
    std::string resolution{"resolved"};
    double confidence{1.0};
};

inline void to_json(nlohmann::json& j, const GraphEdgeDto& e) {
    j = nlohmann::json{
        {"sourceSymbolId", e.source_symbol_id},
        {"targetSymbolId", e.target_symbol_id ? nlohmann::json(*e.target_symbol_id) : nullptr},
        {"relationKind", e.relation_kind},
        {"resolution", e.resolution},
        {"confidence", e.confidence},
    };
}

struct SymbolGraphDto {
    int64_t root_symbol_id{0};
    std::vector<GraphNodeDto> nodes;
    std::vector<GraphEdgeDto> edges;
    bool truncated{false};
};

inline void to_json(nlohmann::json& j, const SymbolGraphDto& g) {
    j = nlohmann::json{
        {"rootSymbolId", g.root_symbol_id},
        {"nodes", g.nodes},
        {"edges", g.edges},
        {"truncated", g.truncated},
    };
}

// ==========================================
// Search DTOs (F-09)
// ==========================================
struct SourceSearchHitDto {
    int64_t file_id{0};
    std::string relative_path;
    std::string snippet;
    double rank{0.0};
};

inline void to_json(nlohmann::json& j, const SourceSearchHitDto& h) {
    j = nlohmann::json{
        {"fileId", h.file_id},
        {"relativePath", h.relative_path},
        {"snippet", h.snippet},
        {"rank", h.rank},
    };
}

struct SymbolSearchHitDto {
    int64_t id{0};
    int64_t file_id{0};
    std::string relative_path;
    std::string name;
    std::optional<std::string> qualified_name;
    std::string kind;
    double rank{0.0};
};

inline void to_json(nlohmann::json& j, const SymbolSearchHitDto& h) {
    j = nlohmann::json{
        {"id", h.id},
        {"fileId", h.file_id},
        {"relativePath", h.relative_path},
        {"name", h.name},
        {"qualifiedName", h.qualified_name ? nlohmann::json(*h.qualified_name) : nullptr},
        {"kind", h.kind},
        {"rank", h.rank},
    };
}

} // namespace codelenses::server
