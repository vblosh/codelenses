#pragma once

#include <algorithm>
#include <cctype>
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
    double confidence{1.0};
    std::optional<std::string> metadata_json{std::nullopt};

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

enum class IncludeCategory {
    quoted,            // -iquote
    standard,          // -I
    system,            // -isystem
    default_toolchain, // explicitly configured default toolchain search roots
    after,             // -idirafter
};

[[nodiscard]] constexpr std::string_view to_string(IncludeCategory cat) noexcept {
    switch (cat) {
    case IncludeCategory::quoted:
        return "quoted";
    case IncludeCategory::standard:
        return "standard";
    case IncludeCategory::system:
        return "system";
    case IncludeCategory::default_toolchain:
        return "default_toolchain";
    case IncludeCategory::after:
        return "after";
    }
    return "standard";
}

enum class IncludeOrigin {
    compile_command,
    configured_profile,
};

enum class RootRole {
    unspecified,
    c_runtime,
    cpp_library,
    compiler_headers,
    platform_headers,
};

struct IncludeSearchEntry {
    std::filesystem::path directory{};
    IncludeCategory category{IncludeCategory::standard};
    std::size_t original_position{0};
    IncludeOrigin origin{IncludeOrigin::compile_command};
    RootRole role{RootRole::unspecified};

    friend bool operator==(const IncludeSearchEntry&, const IncludeSearchEntry&) = default;
};

struct CompileCommandContext {
    std::filesystem::path directory{};
    std::filesystem::path file{};
    std::optional<std::filesystem::path> output{std::nullopt};
    std::vector<std::string> arguments{};
    std::vector<std::filesystem::path> include_dirs{};
    std::vector<IncludeSearchEntry> search_entries{};
    std::optional<std::filesystem::path> sysroot{std::nullopt};
    bool nostdinc{false};
    bool nostdincxx{false};
    bool conservative_preproc{false};
    std::vector<std::string> defines{};
    std::optional<std::string> language_standard{std::nullopt};
    bool parsed_from_arguments{false};

    friend bool operator==(const CompileCommandContext&, const CompileCommandContext&) = default;

    [[nodiscard]] bool is_cpp() const {
        if (language_standard.has_value()) {
            std::string std_str = *language_standard;
            if (std_str.starts_with("-std=")) std_str = std_str.substr(5);
            else if (std_str.starts_with("--std=")) std_str = std_str.substr(6);
            std::transform(std_str.begin(), std_str.end(), std_str.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (std_str.starts_with("c++") || std_str.starts_with("gnu++") ||
                std_str.starts_with("iso14882")) {
                return true;
            }
        }
        for (const auto& arg : arguments) {
            if (arg == "-xc++" || arg == "-xc++-header") return true;
        }
        if (!arguments.empty()) {
            auto compiler = std::filesystem::path(arguments.front()).stem().string();
            std::transform(compiler.begin(), compiler.end(), compiler.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (compiler.ends_with("++") || compiler == "c++" || compiler == "clang++" ||
                compiler == "g++" || compiler == "cl" || compiler == "cl.exe") {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool is_c() const {
        if (is_cpp()) return false;
        if (language_standard.has_value()) {
            std::string std_str = *language_standard;
            if (std_str.starts_with("-std=")) std_str = std_str.substr(5);
            else if (std_str.starts_with("--std=")) std_str = std_str.substr(6);
            std::transform(std_str.begin(), std_str.end(), std_str.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (std_str.starts_with("c") || std_str.starts_with("gnu") ||
                std_str.starts_with("iso9899")) {
                return true;
            }
        }
        for (const auto& arg : arguments) {
            if (arg == "-xc" || arg == "-xc-header") return true;
        }
        if (!arguments.empty()) {
            auto compiler = std::filesystem::path(arguments.front()).stem().string();
            std::transform(compiler.begin(), compiler.end(), compiler.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (compiler == "gcc" || compiler == "clang") {
                return true;
            }
        }
        return false;
    }

    // Computes deterministic ordered search directories according to standard GCC/Clang semantics.
    // For quoted includes:
    // 1. Including file's directory
    // 2. -iquote entries
    // 3. -I entries
    // 4. -isystem entries
    // 5. Default toolchain entries (unless disabled by nostdinc, or nostdincxx for C++)
    // 6. -idirafter entries
    // For angle includes: omit 1 and 2.
    // Preserves order within categories; deduplicates directories (first seen wins).
    [[nodiscard]] std::vector<std::filesystem::path> get_ordered_include_paths(
        bool is_quote,
        const std::filesystem::path& source_file_dir = {},
        const std::vector<std::filesystem::path>& default_toolchain_dirs = {},
        bool is_cpp = true) const {
        std::vector<std::filesystem::path> ordered;
        std::vector<std::string> seen;

        auto add_dir = [&](const std::filesystem::path& p) {
            if (p.empty())
                return;
            auto norm = p.lexically_normal();
            std::string key = norm.generic_string();
            for (const auto& s : seen) {
                if (s == key)
                    return;
            }
            seen.push_back(key);
            ordered.push_back(norm);
        };

        // 1. Including file's directory (quote only)
        if (is_quote && !source_file_dir.empty()) {
            add_dir(source_file_dir);
        }

        // 2. -iquote entries (quote only)
        if (is_quote) {
            for (const auto& entry : search_entries) {
                if (entry.category == IncludeCategory::quoted) {
                    add_dir(entry.directory);
                }
            }
        }

        // 3. -I entries
        for (const auto& entry : search_entries) {
            if (entry.category == IncludeCategory::standard) {
                add_dir(entry.directory);
            }
        }

        // 4. -isystem entries
        for (const auto& entry : search_entries) {
            if (entry.category == IncludeCategory::system) {
                add_dir(entry.directory);
            }
        }

        // 5. Explicitly configured default toolchain directories
        bool suppress_default = nostdinc;
        if (!suppress_default) {
            for (const auto& entry : search_entries) {
                if (entry.category == IncludeCategory::default_toolchain) {
                    if (is_cpp && nostdincxx && entry.role == RootRole::cpp_library) {
                        continue;
                    }
                    add_dir(entry.directory);
                }
            }
            for (const auto& d : default_toolchain_dirs) {
                add_dir(d);
            }
        }

        // 6. -idirafter entries
        for (const auto& entry : search_entries) {
            if (entry.category == IncludeCategory::after) {
                add_dir(entry.directory);
            }
        }

        return ordered;
    }

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
                std::string_view def_sv(*it);
                auto eq = def_sv.find('=');
                std::string_view def_name =
                    (eq != std::string_view::npos) ? def_sv.substr(0, eq) : def_sv;
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
                std::string_view def_sv(*it);
                auto eq = def_sv.find('=');
                std::string_view def_name =
                    (eq != std::string_view::npos) ? def_sv.substr(0, eq) : def_sv;
                if (def_name == name) {
                    if (eq != std::string_view::npos) {
                        return std::string(def_sv.substr(eq + 1));
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
