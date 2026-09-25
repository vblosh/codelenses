#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "codelenses/domain/range.hpp"

namespace codelenses {

struct Occurrence {
    int64_t id = 0;
    int64_t workspace_id = 0;
    int64_t file_id = 0;
    std::optional<int64_t> symbol_id = std::nullopt;
    std::string occurrence_kind =
        ""; // definition, declaration, reference, implementation, override, import, include
    std::string name = "";
    SourceRange range = {};
    double confidence = 1.0;
    std::string resolution = "unresolved"; // resolved, unresolved, ambiguous, external
    std::optional<std::string> metadata_json = std::nullopt;

    bool operator==(const Occurrence& other) const = default;
};

} // namespace codelenses
