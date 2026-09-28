#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace codelenses {

struct FileRecord {
    int64_t id = 0;
    int64_t workspace_id = 0;
    std::string path = "";
    std::string relative_path = "";
    std::string name = "";
    std::optional<std::string> extension = std::nullopt;
    std::string language = "unknown";
    std::string encoding = "utf-8";
    int64_t size_bytes = 0;
    int64_t modified_ns = 0;
    std::optional<std::string> content_hash = std::nullopt;
    std::optional<std::string> parse_hash = std::nullopt;
    std::optional<std::string> language_version = std::nullopt;
    bool is_binary = false;
    bool is_generated = false;
    bool is_deleted = false;
    std::optional<int64_t> last_index_job_id = std::nullopt;
    std::optional<std::string> indexed_at = std::nullopt;
    std::string created_at = "";
    std::string updated_at = "";

    bool operator==(const FileRecord& other) const = default;
};

struct FileStateItem {
    int64_t id = 0;
    std::string relative_path = "";
    int64_t size_bytes = 0;
    int64_t modified_ns = 0;
    std::optional<std::string> content_hash = std::nullopt;
    bool is_deleted = false;
    std::string language = "";

    bool operator==(const FileStateItem& other) const = default;
};

} // namespace codelenses
