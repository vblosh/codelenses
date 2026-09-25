#pragma once

#include <cstdint>
#include <string>

namespace codelenses {

struct FileContent {
    int64_t file_id = 0;
    int64_t workspace_id = 0;
    std::string content = "";
    std::string content_hash = "";
    std::string updated_at = "";

    bool operator==(const FileContent& other) const = default;
};

} // namespace codelenses
