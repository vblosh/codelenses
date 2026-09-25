#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "codelenses/domain/range.hpp"
#include "codelenses/range.hpp"

namespace codelenses {

struct Point {
    uint32_t line = 0;
    uint32_t column = 0;

    bool operator==(const Point& other) const = default;
};

class CoordinateConverter {
public:
    explicit CoordinateConverter(std::string_view source) : source_(source) {
        line_starts_.push_back(0);
        for (size_t i = 0; i < source.size(); ++i) {
            if (source[i] == '\n') {
                line_starts_.push_back(i + 1);
            }
        }
    }

    [[nodiscard]] size_t line_count() const noexcept { return line_starts_.size(); }

    [[nodiscard]] size_t source_size() const noexcept { return source_.size(); }

    [[nodiscard]] size_t line_start_offset(size_t line_index) const noexcept {
        if (line_index >= line_starts_.size()) {
            return source_.size();
        }
        return line_starts_[line_index];
    }

    [[nodiscard]] size_t line_end_offset(size_t line_index) const noexcept {
        if (line_index >= line_starts_.size()) {
            return source_.size();
        }
        if (line_index + 1 < line_starts_.size()) {
            size_t next_start = line_starts_[line_index + 1];
            if (next_start > 0 && source_[next_start - 1] == '\n') {
                --next_start;
                if (next_start > line_starts_[line_index] && source_[next_start - 1] == '\r') {
                    --next_start;
                }
            }
            return next_start;
        }
        return source_.size();
    }

    [[nodiscard]] std::string_view line_text(size_t line_index) const noexcept {
        if (line_index >= line_starts_.size()) {
            return "";
        }
        const size_t start = line_starts_[line_index];
        const size_t end = line_end_offset(line_index);
        if (start >= source_.size() || end < start) {
            return "";
        }
        return source_.substr(start, end - start);
    }

    [[nodiscard]] Point byte_to_point(size_t byte_offset) const noexcept {
        if (source_.empty() || byte_offset == 0) {
            return Point{0, 0};
        }
        if (byte_offset > source_.size()) {
            byte_offset = source_.size();
        }

        auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), byte_offset);
        size_t line_idx = static_cast<size_t>(std::distance(line_starts_.begin(), it) - 1);
        size_t col = byte_offset - line_starts_[line_idx];

        return Point{static_cast<uint32_t>(line_idx), static_cast<uint32_t>(col)};
    }

    [[nodiscard]] size_t point_to_byte(Point point) const noexcept {
        if (point.line >= line_starts_.size()) {
            return source_.size();
        }
        size_t line_start = line_starts_[point.line];
        size_t next_line_start =
            (point.line + 1 < line_starts_.size()) ? line_starts_[point.line + 1] : source_.size();
        size_t offset = line_start + point.column;
        return std::min(offset, next_line_start);
    }

    [[nodiscard]] codelenses::SourceRange
    byte_range_to_source_range(ByteRange range) const noexcept {
        Point start = byte_to_point(range.start);
        Point end = byte_to_point(range.end);
        return codelenses::SourceRange{
            .start_byte = static_cast<int64_t>(range.start),
            .end_byte = static_cast<int64_t>(range.end),
            .start_line = static_cast<int64_t>(start.line),
            .start_column = static_cast<int64_t>(start.column),
            .end_line = static_cast<int64_t>(end.line),
            .end_column = static_cast<int64_t>(end.column),
        };
    }

    [[nodiscard]] size_t byte_to_codepoint_column(size_t line_index,
                                                  size_t byte_col) const noexcept {
        if (line_index >= line_starts_.size())
            return 0;
        size_t start = line_starts_[line_index];
        size_t max_bytes = source_.size() - start;
        size_t limit = std::min(byte_col, max_bytes);

        size_t codepoints = 0;
        size_t i = 0;
        while (i < limit) {
            unsigned char c = static_cast<unsigned char>(source_[start + i]);
            if ((c & 0x80) == 0) {
                i += 1;
            } else if ((c & 0xE0) == 0xC0) {
                i += 2;
            } else if ((c & 0xF0) == 0xE0) {
                i += 3;
            } else if ((c & 0xF8) == 0xF0) {
                i += 4;
            } else {
                i += 1;
            }
            ++codepoints;
        }
        return codepoints;
    }

    [[nodiscard]] size_t codepoint_to_byte_column(size_t line_index,
                                                  size_t codepoint_col) const noexcept {
        if (line_index >= line_starts_.size())
            return 0;
        size_t start = line_starts_[line_index];
        size_t max_bytes = source_.size() - start;

        size_t codepoints = 0;
        size_t i = 0;
        while (i < max_bytes && codepoints < codepoint_col) {
            unsigned char c = static_cast<unsigned char>(source_[start + i]);
            if ((c & 0x80) == 0) {
                i += 1;
            } else if ((c & 0xE0) == 0xC0) {
                i += 2;
            } else if ((c & 0xF0) == 0xE0) {
                i += 3;
            } else if ((c & 0xF8) == 0xF0) {
                i += 4;
            } else {
                i += 1;
            }
            ++codepoints;
        }
        return i;
    }

    [[nodiscard]] size_t byte_to_utf16_column(size_t line_index, size_t byte_col) const noexcept {
        if (line_index >= line_starts_.size())
            return 0;
        size_t start = line_starts_[line_index];
        size_t max_bytes = source_.size() - start;
        size_t limit = std::min(byte_col, max_bytes);

        size_t utf16_units = 0;
        size_t i = 0;
        while (i < limit) {
            unsigned char c = static_cast<unsigned char>(source_[start + i]);
            if ((c & 0x80) == 0) {
                i += 1;
                utf16_units += 1;
            } else if ((c & 0xE0) == 0xC0) {
                i += 2;
                utf16_units += 1;
            } else if ((c & 0xF0) == 0xE0) {
                i += 3;
                utf16_units += 1;
            } else if ((c & 0xF8) == 0xF0) {
                i += 4;
                utf16_units += 2; // Surrogate pair in UTF-16
            } else {
                i += 1;
                utf16_units += 1;
            }
        }
        return utf16_units;
    }

    [[nodiscard]] size_t utf16_to_byte_column(size_t line_index, size_t utf16_col) const noexcept {
        if (line_index >= line_starts_.size())
            return 0;
        size_t start = line_starts_[line_index];
        size_t max_bytes = source_.size() - start;

        size_t utf16_units = 0;
        size_t i = 0;
        while (i < max_bytes && utf16_units < utf16_col) {
            unsigned char c = static_cast<unsigned char>(source_[start + i]);
            if ((c & 0x80) == 0) {
                i += 1;
                utf16_units += 1;
            } else if ((c & 0xE0) == 0xC0) {
                i += 2;
                utf16_units += 1;
            } else if ((c & 0xF0) == 0xE0) {
                i += 3;
                utf16_units += 1;
            } else if ((c & 0xF8) == 0xF0) {
                i += 4;
                utf16_units += 2;
            } else {
                i += 1;
                utf16_units += 1;
            }
        }
        return i;
    }

private:
    std::string_view source_;
    std::vector<size_t> line_starts_;
};

} // namespace codelenses
