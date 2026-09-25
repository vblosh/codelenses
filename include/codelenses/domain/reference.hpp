#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "codelenses/domain/range.hpp"

namespace codelenses {

struct ReferenceOccurrence {
    int64_t id = 0;
    int64_t workspace_id = 0;
    int64_t source_file_id = 0;
    std::optional<int64_t> source_symbol_id = std::nullopt;
    std::optional<int64_t> target_symbol_id = std::nullopt;
    std::string name = "";
    std::string reference_kind = "";
    SourceRange range = {};
    std::string resolution = "unresolved"; // resolved, unresolved, ambiguous, external
    double confidence = 1.0;
    std::optional<std::string> metadata_json = std::nullopt;

    bool operator==(const ReferenceOccurrence& other) const = default;
};

struct ReferencerResult {
    int64_t id = 0;
    std::string reference_kind = "";
    std::string name = "";
    std::string resolution = "unresolved";
    double confidence = 1.0;
    int64_t start_line = 0;
    int64_t start_column = 0;
    int64_t end_line = 0;
    int64_t end_column = 0;
    int64_t file_id = 0;
    std::string relative_path = "";
    std::optional<int64_t> containing_symbol_id = std::nullopt;
    std::optional<std::string> containing_symbol_name = std::nullopt;
    std::optional<std::string> containing_qualified_name = std::nullopt;

    bool operator==(const ReferencerResult& other) const = default;
};

struct CallerCalleeResult {
    std::optional<int64_t> symbol_id = std::nullopt;
    std::string name = "";
    std::optional<std::string> qualified_name = std::nullopt;
    int64_t file_id = 0;

    bool operator==(const CallerCalleeResult& other) const = default;
};

} // namespace codelenses
