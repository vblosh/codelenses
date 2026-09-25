#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace codelenses {

struct Diagnostic {
    int64_t id = 0;
    int64_t workspace_id = 0;
    std::optional<int64_t> job_id = std::nullopt;
    std::optional<int64_t> file_id = std::nullopt;
    std::string severity = ""; // info, warning, error
    std::string source = "";   // filesystem, parser, adapter, resolver, database, indexer
    std::string code = "";
    std::string message = "";
    std::optional<int64_t> start_byte = std::nullopt;
    std::optional<int64_t> end_byte = std::nullopt;
    std::optional<int64_t> start_line = std::nullopt;
    std::optional<int64_t> start_column = std::nullopt;
    std::optional<int64_t> end_line = std::nullopt;
    std::optional<int64_t> end_column = std::nullopt;
    std::optional<std::string> metadata_json = std::nullopt;
    std::string created_at = "";

    bool operator==(const Diagnostic& other) const = default;
};

} // namespace codelenses
