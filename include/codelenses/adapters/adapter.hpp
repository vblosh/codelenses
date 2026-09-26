#pragma once

#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "codelenses/diagnostic.hpp"
#include "codelenses/language.hpp"
#include "codelenses/model.hpp"
#include "codelenses/parser/highlight.hpp"
#include "codelenses/range.hpp"
#include "codelenses/result.hpp"
#include "codelenses/treesitter/parser.hpp"
#include "codelenses/worker/protocol.hpp"

namespace codelenses::adapters {

enum class CapabilityStatus {
    supported,
    candidate_only,
    unavailable,
    deferred,
};

[[nodiscard]] constexpr std::string_view to_string(CapabilityStatus status) noexcept {
    switch (status) {
    case CapabilityStatus::supported:
        return "supported";
    case CapabilityStatus::candidate_only:
        return "candidate_only";
    case CapabilityStatus::unavailable:
        return "unavailable";
    case CapabilityStatus::deferred:
        return "deferred";
    }
    return "unavailable";
}

struct LanguageCapabilities {
    // Construct support
    CapabilityStatus functions{CapabilityStatus::unavailable};
    CapabilityStatus methods{CapabilityStatus::unavailable};
    CapabilityStatus classes{CapabilityStatus::unavailable};
    CapabilityStatus structs{CapabilityStatus::unavailable};
    CapabilityStatus interfaces{CapabilityStatus::unavailable};
    CapabilityStatus enums{CapabilityStatus::unavailable};
    CapabilityStatus records{CapabilityStatus::unavailable};
    CapabilityStatus namespaces{CapabilityStatus::unavailable};
    CapabilityStatus variables{CapabilityStatus::unavailable};
    CapabilityStatus modules{CapabilityStatus::unavailable};
    CapabilityStatus packages{CapabilityStatus::unavailable};
    CapabilityStatus templates{CapabilityStatus::unavailable};
    CapabilityStatus partial_types{CapabilityStatus::unavailable};

    // Relationship support
    CapabilityStatus containment{CapabilityStatus::unavailable};
    CapabilityStatus calls{CapabilityStatus::unavailable};
    CapabilityStatus references{CapabilityStatus::unavailable};
    CapabilityStatus inheritance{CapabilityStatus::unavailable};
    CapabilityStatus implementation{CapabilityStatus::unavailable};
    CapabilityStatus imports{CapabilityStatus::unavailable};
    CapabilityStatus includes{CapabilityStatus::unavailable};

    // Deferred framework / semantic analyses
    CapabilityStatus api_endpoints{CapabilityStatus::deferred};
    CapabilityStatus database_tables{CapabilityStatus::deferred};
    CapabilityStatus test_declarations{CapabilityStatus::deferred};
    CapabilityStatus override_analysis{CapabilityStatus::deferred};
    CapabilityStatus instantiation_analysis{CapabilityStatus::deferred};
    CapabilityStatus reads_writes{CapabilityStatus::deferred};
    CapabilityStatus build_model{CapabilityStatus::deferred};
    CapabilityStatus semantic_only_analysis{CapabilityStatus::deferred};
};

struct SymbolFact {
    std::string name;
    std::string qualified_name;
    NodeKind kind{NodeKind::function};
    ByteRange range{};
    DisplayRange display_range{};
    std::optional<std::string> enclosing_scope;
    std::string signature;

    friend bool operator==(const SymbolFact&, const SymbolFact&) = default;
};

struct DeclarationFact {
    std::string symbol_name;
    std::string qualified_name;
    NodeKind kind{NodeKind::function};
    ByteRange range{};
    DisplayRange display_range{};
    std::optional<std::string> enclosing_scope;
    bool is_definition{true};

    friend bool operator==(const DeclarationFact&, const DeclarationFact&) = default;
};

struct OccurrenceFact {
    worker::FactKind kind{worker::FactKind::reference};
    std::string written_name;
    ByteRange range{};
    DisplayRange display_range{};
    std::optional<std::string> enclosing_scope;
    std::vector<std::string> candidate_targets;

    friend bool operator==(const OccurrenceFact&, const OccurrenceFact&) = default;
};

struct ParseDiagnostic {
    DiagnosticSeverity severity{DiagnosticSeverity::error};
    Language language{Language::unknown};
    std::string code{"syntax_error"};
    std::string message;
    ByteRange byte_range{};
    DisplayRange display_range{};

    friend bool operator==(const ParseDiagnostic&, const ParseDiagnostic&) = default;
};

struct CompileCommandContext {
    std::filesystem::path directory{};
    std::filesystem::path file{};
    std::optional<std::filesystem::path> output{std::nullopt};
    std::vector<std::string> arguments{};
    std::vector<std::filesystem::path> include_dirs{};
    std::vector<std::string> defines{};
    std::optional<std::string> language_standard{std::nullopt};
    bool parsed_from_arguments{false};

    friend bool operator==(const CompileCommandContext&, const CompileCommandContext&) = default;

    struct ActiveDefine {
        std::string name;
        std::string value;
        auto operator<=>(const ActiveDefine&) const = default;
    };

    [[nodiscard]] std::vector<ActiveDefine> active_defines() const {
        std::vector<ActiveDefine> result;
        for (const auto& def_arg : defines) {
            if (def_arg.starts_with("-U")) {
                std::string_view undef_name = std::string_view(def_arg).substr(2);
                std::erase_if(result, [&](const ActiveDefine& m) { return m.name == undef_name; });
            } else {
                auto eq = def_arg.find('=');
                std::string name = (eq != std::string::npos) ? def_arg.substr(0, eq) : def_arg;
                std::string val = (eq != std::string::npos) ? def_arg.substr(eq + 1) : "1";
                std::erase_if(result, [&](const ActiveDefine& m) { return m.name == name; });
                result.push_back(ActiveDefine{.name = std::move(name), .value = std::move(val)});
            }
        }
        return result;
    }

    [[nodiscard]] bool has_define(std::string_view name) const {
        for (auto it = defines.rbegin(); it != defines.rend(); ++it) {
            if (it->starts_with("-U")) {
                if (it->substr(2) == name) {
                    return false;
                }
            } else {
                auto eq = it->find('=');
                std::string_view def_name =
                    (eq != std::string::npos) ? it->substr(0, eq) : std::string_view(*it);
                if (def_name == name) {
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<std::string> get_define_value(std::string_view name) const {
        for (auto it = defines.rbegin(); it != defines.rend(); ++it) {
            if (it->starts_with("-U")) {
                if (it->substr(2) == name) {
                    return std::nullopt;
                }
            } else {
                auto eq = it->find('=');
                std::string_view def_name =
                    (eq != std::string::npos) ? it->substr(0, eq) : std::string_view(*it);
                if (def_name == name) {
                    if (eq != std::string::npos) {
                        return it->substr(eq + 1);
                    }
                    return "1";
                }
            }
        }
        return std::nullopt;
    }
};

struct AdapterResult {
    Language language{Language::unknown};
    worker::CompletionStatus status{worker::CompletionStatus::complete};
    std::vector<SymbolFact> symbols;
    std::vector<DeclarationFact> declarations;
    std::vector<OccurrenceFact> occurrences;
    std::vector<ParseDiagnostic> diagnostics;
    std::optional<CompileCommandContext> compile_command{std::nullopt};

    [[nodiscard]] bool has_errors() const noexcept {
        for (const auto& d : diagnostics) {
            if (d.severity == DiagnosticSeverity::error)
                return true;
        }
        return false;
    }
};

// Normalized contract that every language adapter implements.
class LanguageAdapter {
public:
    virtual ~LanguageAdapter() = default;

    [[nodiscard]] virtual Language language() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual const LanguageCapabilities& capabilities() const noexcept = 0;

    // Parses the given source buffer and produces normalized facts, occurrences, and diagnostics.
    // Must be thread-safe across distinct adapter instances and support cooperative cancellation.
    [[nodiscard]] virtual Result<AdapterResult> parse(std::string_view source,
                                                      const std::filesystem::path& file_path = {},
                                                      const std::stop_token& stop_token = {}) = 0;

    // Parses the given source buffer with optional compile-command context.
    // Default implementation delegates to parse(source, file_path, stop_token).
    [[nodiscard]] virtual Result<AdapterResult> parse(std::string_view source,
                                                      const std::filesystem::path& file_path,
                                                      const CompileCommandContext& context,
                                                      const std::stop_token& stop_token = {}) {
        (void)context;
        return parse(source, file_path, stop_token);
    }

    // Highlights the source buffer given a parsed tree. Default implementation returns empty.
    [[nodiscard]] virtual Result<std::vector<HighlightToken>>
    highlight(std::string_view source, const treesitter::Tree& tree) {
        (void)source;
        (void)tree;
        return std::vector<HighlightToken>{};
    }
};

// Traverses a Tree-sitter CST and collects all syntax error or missing nodes.
[[nodiscard]] std::vector<ParseDiagnostic> collect_syntax_errors(const treesitter::Node& root,
                                                                 Language lang);

} // namespace codelenses::adapters
