#pragma once

#include <cstdint>

namespace codelenses {

struct SourceRange {
    int64_t start_byte = 0;
    int64_t end_byte = 0;
    int64_t start_line = 0;
    int64_t start_column = 0;
    int64_t end_line = 0;
    int64_t end_column = 0;

    bool operator==(const SourceRange& other) const = default;
};

} // namespace codelenses
