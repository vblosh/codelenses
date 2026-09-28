#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace codelenses {

struct SymbolSearchResult {
    int64_t id = 0;
    int64_t file_id = 0;
    std::string name;
    std::optional<std::string> qualified_name;
    std::string kind;
    double rank = 0.0;
    std::optional<int64_t> line = std::nullopt;

    bool operator==(const SymbolSearchResult& other) const = default;
};

struct FileSearchResult {
    int64_t file_id = 0;
    std::string snippet;
    double rank = 0.0;

    bool operator==(const FileSearchResult& other) const = default;
};

} // namespace codelenses
