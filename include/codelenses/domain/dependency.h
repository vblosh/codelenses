#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace codelenses {

struct FileDependency {
    int64_t id = 0;
    int64_t workspace_id = 0;
    int64_t source_file_id = 0;
    std::optional<int64_t> target_file_id = std::nullopt;
    std::string dependency_kind = ""; // import, include, using, etc.
    std::string raw_name = "";
    std::optional<std::string> resolved_path = std::nullopt;
    std::string resolution = "unresolved"; // resolved, unresolved, external
    std::optional<int64_t> start_byte = std::nullopt;
    std::optional<int64_t> end_byte = std::nullopt;
    std::optional<std::string> metadata_json = std::nullopt;

    bool operator==(const FileDependency& other) const = default;
};

} // namespace codelenses
