#include <algorithm>
#include <cctype>
#include <string>

#include "codelenses/diagnostic.hpp"
#include "codelenses/language.hpp"
#include "codelenses/model.hpp"
#include "codelenses/status.hpp"

namespace codelenses {
namespace {

[[nodiscard]] std::string lowercase(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

template <typename Enum>
[[nodiscard]] Result<Enum> invalid_enum(std::string_view kind, std::string_view value) {
    return unexpected_result<Enum>(ErrorCode::invalid_argument,
                                   std::string(kind) +
                                       " has an unknown value: " + std::string(value));
}

[[nodiscard]] bool valid_severity(DiagnosticSeverity value) noexcept {
    switch (value) {
    case DiagnosticSeverity::info:
    case DiagnosticSeverity::warning:
    case DiagnosticSeverity::error:
    case DiagnosticSeverity::fatal:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_language(Language value) noexcept {
    switch (value) {
    case Language::c:
    case Language::cpp:
    case Language::csharp:
    case Language::python:
    case Language::typescript:
    case Language::javascript:
    case Language::go:
    case Language::java:
    case Language::shell:
    case Language::bash:
    case Language::unknown:
        return true;
    }
    return false;
}

} // namespace

Language language_from_extension(std::string_view extension) {
    std::string normalized = lowercase(extension);
    if (!normalized.empty() && normalized.front() != '.')
        normalized.insert(0, 1, '.');
    if (normalized == ".c")
        return Language::c;
    if (normalized == ".h")
        return Language::c;
    if (normalized == ".cc" || normalized == ".cpp" || normalized == ".cxx" ||
        normalized == ".hpp" || normalized == ".hh" || normalized == ".hxx") {
        return Language::cpp;
    }
    if (normalized == ".cs")
        return Language::csharp;
    if (normalized == ".py" || normalized == ".pyi")
        return Language::python;
    if (normalized == ".ts" || normalized == ".tsx")
        return Language::typescript;
    if (normalized == ".js" || normalized == ".jsx" || normalized == ".mjs" ||
        normalized == ".cjs") {
        return Language::javascript;
    }
    if (normalized == ".go")
        return Language::go;
    if (normalized == ".java")
        return Language::java;
    if (normalized == ".sh" || normalized == ".command")
        return Language::shell;
    if (normalized == ".bash")
        return Language::bash;
    return Language::unknown;
}

Language language_from_path(const std::filesystem::path& path) {
    return language_from_extension(path.extension().string());
}

Result<Language> language_from_string(std::string_view name) {
    const auto normalized = lowercase(name);
    if (normalized == "c")
        return Language::c;
    if (normalized == "c++" || normalized == "cpp" || normalized == "cxx")
        return Language::cpp;
    if (normalized == "c#" || normalized == "csharp")
        return Language::csharp;
    if (normalized == "python" || normalized == "py")
        return Language::python;
    if (normalized == "typescript" || normalized == "ts" || normalized == "tsx")
        return Language::typescript;
    if (normalized == "javascript" || normalized == "js" || normalized == "jsx")
        return Language::javascript;
    if (normalized == "go")
        return Language::go;
    if (normalized == "java")
        return Language::java;
    if (normalized == "shell" || normalized == "sh" || normalized == "posix-shell")
        return Language::shell;
    if (normalized == "bash")
        return Language::bash;
    if (normalized == "unknown")
        return Language::unknown;
    return invalid_enum<Language>("language", name);
}

std::string_view to_string(Language value) noexcept {
    switch (value) {
    case Language::c:
        return "C";
    case Language::cpp:
        return "C++";
    case Language::csharp:
        return "C#";
    case Language::python:
        return "Python";
    case Language::typescript:
        return "TypeScript";
    case Language::javascript:
        return "JavaScript";
    case Language::go:
        return "Go";
    case Language::java:
        return "Java";
    case Language::shell:
        return "POSIX shell";
    case Language::bash:
        return "Bash";
    case Language::unknown:
        return "Unknown";
    }
    return "Unknown";
}

std::string_view to_string(AnalysisStatus value) noexcept {
    switch (value) {
    case AnalysisStatus::unknown:
        return "unknown";
    case AnalysisStatus::pending:
        return "pending";
    case AnalysisStatus::indexing:
        return "indexing";
    case AnalysisStatus::parsing:
        return "parsing";
    case AnalysisStatus::resolving:
        return "resolving";
    case AnalysisStatus::complete:
        return "complete";
    case AnalysisStatus::failed:
        return "failed";
    case AnalysisStatus::cancelled:
        return "cancelled";
    case AnalysisStatus::degraded:
        return "degraded";
    case AnalysisStatus::stale:
        return "stale";
    case AnalysisStatus::unsupported:
        return "unsupported";
    }
    return "unknown";
}

std::string_view to_string(JobStatus value) noexcept {
    switch (value) {
    case JobStatus::discovered:
        return "discovered";
    case JobStatus::queued:
        return "queued";
    case JobStatus::running:
        return "running";
    case JobStatus::completed:
        return "completed";
    case JobStatus::failed:
        return "failed";
    case JobStatus::pending_resolution:
        return "pending_resolution";
    case JobStatus::cancelled:
        return "cancelled";
    case JobStatus::degraded:
        return "degraded";
    }
    return "failed";
}

Result<AnalysisStatus> analysis_status_from_string(std::string_view value) {
    const auto normalized = lowercase(value);
    for (const auto candidate :
         {AnalysisStatus::unknown, AnalysisStatus::pending, AnalysisStatus::indexing,
          AnalysisStatus::parsing, AnalysisStatus::resolving, AnalysisStatus::complete,
          AnalysisStatus::failed, AnalysisStatus::cancelled, AnalysisStatus::degraded,
          AnalysisStatus::stale, AnalysisStatus::unsupported}) {
        if (to_string(candidate) == normalized)
            return candidate;
    }
    return invalid_enum<AnalysisStatus>("analysis status", value);
}

Result<JobStatus> job_status_from_string(std::string_view value) {
    const auto normalized = lowercase(value);
    for (const auto candidate :
         {JobStatus::discovered, JobStatus::queued, JobStatus::running, JobStatus::completed,
          JobStatus::failed, JobStatus::pending_resolution, JobStatus::cancelled,
          JobStatus::degraded}) {
        if (to_string(candidate) == normalized)
            return candidate;
    }
    return invalid_enum<JobStatus>("job status", value);
}

std::string_view to_string(Freshness value) noexcept {
    switch (value) {
    case Freshness::current:
        return "current";
    case Freshness::stale:
        return "stale";
    }
    return "current";
}

Result<Freshness> freshness_from_string(std::string_view value) {
    const auto normalized = lowercase(value);
    if (normalized == "current")
        return Freshness::current;
    if (normalized == "stale")
        return Freshness::stale;
    return invalid_enum<Freshness>("freshness", value);
}

std::string_view to_string(DiagnosticSeverity value) noexcept {
    switch (value) {
    case DiagnosticSeverity::info:
        return "info";
    case DiagnosticSeverity::warning:
        return "warning";
    case DiagnosticSeverity::error:
        return "error";
    case DiagnosticSeverity::fatal:
        return "fatal";
    }
    return "error";
}

Result<DiagnosticSeverity> diagnostic_severity_from_string(std::string_view value) {
    const auto normalized = lowercase(value);
    if (normalized == "info")
        return DiagnosticSeverity::info;
    if (normalized == "warning" || normalized == "warn")
        return DiagnosticSeverity::warning;
    if (normalized == "error")
        return DiagnosticSeverity::error;
    if (normalized == "fatal")
        return DiagnosticSeverity::fatal;
    return invalid_enum<DiagnosticSeverity>("diagnostic severity", value);
}

Result<void> SourceDiagnostic::validate() const {
    if (!valid_severity(severity)) {
        return std::unexpected<Error>(
            make_error(ErrorCode::invalid_argument, "diagnostic severity is invalid"));
    }
    if (code.empty()) {
        return std::unexpected<Error>(
            make_error(ErrorCode::empty_value, "diagnostic code must not be empty"));
    }
    if (message.empty() || message.find_first_not_of(" \t\r\n") == std::string::npos) {
        return std::unexpected<Error>(
            make_error(ErrorCode::empty_value, "diagnostic message must not be empty"));
    }
    if (!valid_language(language)) {
        return std::unexpected<Error>(
            make_error(ErrorCode::invalid_argument, "diagnostic language is invalid"));
    }
    if (byte_range && !byte_range->valid()) {
        return std::unexpected<Error>(
            make_error(ErrorCode::invalid_range, "diagnostic byte range is invalid"));
    }
    if (display_range && !display_range->valid()) {
        return std::unexpected<Error>(
            make_error(ErrorCode::invalid_range, "diagnostic display range is invalid"));
    }
    return {};
}

Result<SourceDiagnostic> SourceDiagnostic::create(DiagnosticSeverity severity, std::string code,
                                                  std::string message, std::filesystem::path path,
                                                  std::optional<ByteRange> byte_range,
                                                  std::optional<DisplayRange> display_range,
                                                  Language language) {
    SourceDiagnostic diagnostic{severity,        std::move(code),       std::move(message),
                                std::move(path), std::move(byte_range), std::move(display_range),
                                language};
    if (auto valid = diagnostic.validate(); !valid) {
        return std::unexpected<Error>(std::move(valid.error()));
    }
    return diagnostic;
}

std::string_view to_string(NodeKind value) noexcept {
    switch (value) {
    case NodeKind::file:
        return "file";
    case NodeKind::namespace_:
        return "namespace";
    case NodeKind::class_:
        return "class";
    case NodeKind::struct_:
        return "struct";
    case NodeKind::interface_:
        return "interface";
    case NodeKind::function:
        return "function";
    case NodeKind::method:
        return "method";
    case NodeKind::variable:
        return "variable";
    case NodeKind::module:
        return "module";
    case NodeKind::package:
        return "package";
    }
    return "unknown";
}

} // namespace codelenses
