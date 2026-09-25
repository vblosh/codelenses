#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace codelenses {

struct SymbolRelation {
    int64_t id = 0;
    int64_t workspace_id = 0;
    int64_t source_symbol_id = 0;
    std::optional<int64_t> target_symbol_id = std::nullopt;
    std::string relation_kind = ""; // contains, calls, imports, includes, inherits, implements,
                                    // overrides, instantiates, throws, returns, type_of
    std::optional<std::string> target_name = std::nullopt;
    std::string resolution = "unresolved"; // resolved, unresolved, ambiguous, external
    double confidence = 1.0;
    std::optional<std::string> metadata_json = std::nullopt;

    bool operator==(const SymbolRelation& other) const = default;
};

} // namespace codelenses
