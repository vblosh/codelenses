#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "codelenses/language.hpp"
#include "codelenses/range.hpp"

namespace codelenses {

enum class DiagnosticSeverity { info, warning, error, fatal };

[[nodiscard]] std::string_view to_string(DiagnosticSeverity value) noexcept;
[[nodiscard]] Result<DiagnosticSeverity> diagnostic_severity_from_string(std::string_view value);

// Diagnostics own all text and paths.  They can therefore safely be queued,
// persisted, or returned by an API after the parser's input buffer dies.
struct Diagnostic {
    DiagnosticSeverity severity{DiagnosticSeverity::error};
    std::string code;
    std::string message;
    std::filesystem::path path;
    std::optional<ByteRange> byte_range;
    std::optional<DisplayRange> display_range;
    Language language{Language::unknown};

    [[nodiscard]] Result<void> validate() const;

    [[nodiscard]] static Result<Diagnostic>
    create(DiagnosticSeverity severity, std::string code, std::string message,
           std::filesystem::path path = {}, std::optional<ByteRange> byte_range = {},
           std::optional<DisplayRange> display_range = {}, Language language = Language::unknown);
};

} // namespace codelenses
