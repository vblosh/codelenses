#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "codelenses/domain/range.hpp"

namespace codelenses {

struct Symbol {
    int64_t id = 0;
    int64_t workspace_id = 0;
    int64_t file_id = 0;
    std::string symbol_key = "";
    std::string name = "";
    std::optional<std::string> qualified_name = std::nullopt;
    std::optional<std::string> display_name = std::nullopt;
    std::string kind = "";
    std::string language = "";
    std::optional<std::string> signature = std::nullopt;
    std::optional<std::string> documentation = std::nullopt;
    std::optional<std::string> container_name = std::nullopt;
    std::optional<int64_t> scope_symbol_id = std::nullopt;
    std::optional<std::string> visibility = std::nullopt;
    bool is_definition = false;
    bool is_declaration = false;
    bool is_generated = false;
    SourceRange range = {};
    std::optional<int64_t> selection_start_byte = std::nullopt;
    std::optional<int64_t> selection_end_byte = std::nullopt;
    std::optional<std::string> signature_hash = std::nullopt;
    std::optional<std::string> parser_version = std::nullopt;
    std::string created_at = "";
    std::string updated_at = "";

    bool operator==(const Symbol& other) const = default;
};

} // namespace codelenses
