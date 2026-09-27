#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codelenses/domain/range.hpp"
#include "codelenses/language.hpp"
#include "codelenses/model.hpp"

namespace codelenses::resolver {

enum class Resolution : std::uint8_t {
    resolved = 1,
    ambiguous = 2,
    unresolved = 3,
    external = 4,
};

[[nodiscard]] constexpr std::string_view to_string(Resolution res) noexcept {
    switch (res) {
    case Resolution::resolved:
        return "resolved";
    case Resolution::ambiguous:
        return "ambiguous";
    case Resolution::unresolved:
        return "unresolved";
    case Resolution::external:
        return "external";
    }
    return "unresolved";
}

[[nodiscard]] inline Resolution resolution_from_string(std::string_view str) noexcept {
    if (str == "resolved")
        return Resolution::resolved;
    if (str == "ambiguous")
        return Resolution::ambiguous;
    if (str == "external")
        return Resolution::external;
    return Resolution::unresolved;
}

struct CandidateTarget {
    std::optional<int64_t> target_symbol_id{std::nullopt};
    std::optional<std::string> target_symbol_key{std::nullopt};
    std::optional<std::string> target_file_path{std::nullopt};
    int32_t rank{0};
    double confidence{1.0};
    std::string reason{};
    std::optional<int64_t> owner_workspace_id{std::nullopt};
    std::optional<int64_t> target_file_id{std::nullopt};

    friend bool operator==(const CandidateTarget&, const CandidateTarget&) = default;
};

struct ResolvedOccurrence {
    int64_t id{0};
    int64_t file_id{0};
    std::string file_path{};
    std::optional<int64_t> enclosing_symbol_id{std::nullopt};
    std::string written_name{};
    SourceRange range{};
    std::optional<std::string> enclosing_scope{std::nullopt};
    Resolution resolution{Resolution::unresolved};
    double confidence{1.0};
    std::string occurrence_kind{"reference"};
    std::vector<CandidateTarget> candidates{};

    friend bool operator==(const ResolvedOccurrence&, const ResolvedOccurrence&) = default;
};

struct SymbolCandidate {
    int64_t symbol_id{0};
    int64_t file_id{0};
    std::string file_path{};
    std::string symbol_key{};
    std::string name{};
    std::optional<std::string> qualified_name{std::nullopt};
    std::string kind{};
    std::string language{};
    std::optional<std::string> signature{std::nullopt};
    std::optional<std::string> enclosing_scope{std::nullopt};
    SourceRange range{};
    bool is_definition{true};
    int32_t scope_distance{0};
    int64_t owner_workspace_id{0}; // 0 = current/project, >0 = library

    friend bool operator==(const SymbolCandidate&, const SymbolCandidate&) = default;
};

[[nodiscard]] inline std::string_view scope_delimiter(Language lang) noexcept {
    switch (lang) {
    case Language::c:
    case Language::cpp:
        return "::";
    default:
        return ".";
    }
}

[[nodiscard]] inline std::vector<std::string>
get_scope_hierarchy(std::optional<std::string_view> enclosing_scope,
                    std::string_view delimiter = "::") {
    std::vector<std::string> scopes;
    if (!enclosing_scope || enclosing_scope->empty()) {
        scopes.push_back("");
        return scopes;
    }

    std::string_view current = *enclosing_scope;
    scopes.emplace_back(current);

    while (true) {
        auto last_pos = current.rfind(delimiter);
        if (last_pos == std::string_view::npos) {
            break;
        }
        current = current.substr(0, last_pos);
        scopes.emplace_back(current);
    }

    scopes.push_back(""); // global scope
    return scopes;
}

[[nodiscard]] inline bool languages_compatible(Language a, Language b) noexcept {
    if (a == b)
        return true;
    if ((a == Language::c && b == Language::cpp) || (a == Language::cpp && b == Language::c)) {
        return true;
    }
    if ((a == Language::shell && b == Language::bash) ||
        (a == Language::bash && b == Language::shell)) {
        return true;
    }
    if ((a == Language::typescript && b == Language::javascript) ||
        (a == Language::javascript && b == Language::typescript)) {
        return true;
    }
    return false;
}

} // namespace codelenses::resolver
