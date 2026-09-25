#pragma once

#include <cstdint>
#include <limits>

#include "codelenses/domain/range.hpp"
#include "codelenses/result.hpp"

namespace codelenses {

// Byte ranges are half-open [start, end) offsets into the exact UTF-8 bytes
// captured from a file.  Keeping this separate from display coordinates avoids
// accidentally using UTF-16 columns or one-based lines as byte offsets.
struct ByteRange {
    std::uint64_t start{};
    std::uint64_t end{};

    [[nodiscard]] static constexpr Result<ByteRange> from_bounds(std::uint64_t start_offset,
                                                                 std::uint64_t end_offset) {
        if (end_offset < start_offset) {
            return unexpected_result<ByteRange>(ErrorCode::invalid_range,
                                                "byte range end precedes start");
        }
        return ByteRange{start_offset, end_offset};
    }

    [[nodiscard]] static constexpr Result<ByteRange> from_offset_and_length(std::uint64_t offset,
                                                                            std::uint64_t length) {
        if (length > std::numeric_limits<std::uint64_t>::max() - offset) {
            return unexpected_result<ByteRange>(ErrorCode::out_of_range,
                                                "byte range exceeds representable offsets");
        }
        return ByteRange{offset, offset + length};
    }

    [[nodiscard]] static constexpr ByteRange empty_at(std::uint64_t offset) {
        return ByteRange{offset, offset};
    }

    [[nodiscard]] constexpr bool valid() const noexcept { return end >= start; }
    [[nodiscard]] constexpr std::uint64_t size() const noexcept { return end - start; }
    [[nodiscard]] constexpr bool contains(std::uint64_t offset) const noexcept {
        return valid() && start <= offset && offset < end;
    }
    friend constexpr auto operator<=>(const ByteRange&, const ByteRange&) = default;
};

struct DisplayPosition {
    // Display coordinates intentionally remain one-based to match editor and
    // protocol conventions used by the prototype and browser clients.
    std::uint32_t line{1};
    std::uint32_t column{1};

    [[nodiscard]] constexpr bool valid() const noexcept { return line >= 1 && column >= 1; }
    friend constexpr auto operator<=>(const DisplayPosition&, const DisplayPosition&) = default;
};

struct DisplayRange {
    // Keep the prototype's public field names so existing aggregate construction
    // and database DTOs remain source-compatible.
    std::uint32_t start_line{1};
    std::uint32_t start_column{1};
    std::uint32_t end_line{1};
    std::uint32_t end_column{1};

    [[nodiscard]] static constexpr Result<DisplayRange>
    from_positions(DisplayPosition start_position, DisplayPosition end_position) {
        if (!start_position.valid() || !end_position.valid()) {
            return unexpected_result<DisplayRange>(ErrorCode::invalid_range,
                                                   "display positions are one-based");
        }
        if (end_position < start_position) {
            return unexpected_result<DisplayRange>(ErrorCode::invalid_range,
                                                   "display range end precedes start");
        }
        return DisplayRange{start_position.line, start_position.column, end_position.line,
                            end_position.column};
    }

    [[nodiscard]] static constexpr Result<DisplayRange> from_coordinates(std::uint32_t start_line,
                                                                         std::uint32_t start_column,
                                                                         std::uint32_t end_line,
                                                                         std::uint32_t end_column) {
        return from_positions(DisplayPosition{start_line, start_column},
                              DisplayPosition{end_line, end_column});
    }

    [[nodiscard]] constexpr DisplayPosition start() const noexcept {
        return DisplayPosition{start_line, start_column};
    }
    [[nodiscard]] constexpr DisplayPosition end() const noexcept {
        return DisplayPosition{end_line, end_column};
    }
    [[nodiscard]] constexpr bool valid() const noexcept {
        return start().valid() && end().valid() && end() >= start();
    }
    friend constexpr auto operator<=>(const DisplayRange&, const DisplayRange&) = default;
};

// Zero-based offsets in UTF-16 code units used by browser clients and
// CodeMirror selection models.
struct Utf16Position {
    std::uint64_t code_unit{0};

    friend constexpr auto operator<=>(const Utf16Position&, const Utf16Position&) = default;
};

struct Utf16Range {
    std::uint64_t start{0};
    std::uint64_t end{0};

    [[nodiscard]] static constexpr Result<Utf16Range> from_bounds(std::uint64_t start_code_unit,
                                                                  std::uint64_t end_code_unit) {
        if (end_code_unit < start_code_unit) {
            return unexpected_result<Utf16Range>(ErrorCode::invalid_range,
                                                 "utf-16 range end precedes start");
        }
        return Utf16Range{start_code_unit, end_code_unit};
    }

    [[nodiscard]] static constexpr Result<Utf16Range> from_offset_and_length(std::uint64_t offset,
                                                                             std::uint64_t length) {
        if (length > std::numeric_limits<std::uint64_t>::max() - offset) {
            return unexpected_result<Utf16Range>(ErrorCode::out_of_range,
                                                 "utf-16 range exceeds representable offsets");
        }
        return Utf16Range{offset, offset + length};
    }

    [[nodiscard]] static constexpr Utf16Range empty_at(std::uint64_t offset) {
        return Utf16Range{offset, offset};
    }

    [[nodiscard]] constexpr bool valid() const noexcept { return end >= start; }
    [[nodiscard]] constexpr std::uint64_t size() const noexcept { return end - start; }
    [[nodiscard]] constexpr bool contains(std::uint64_t offset) const noexcept {
        return valid() && start <= offset && offset < end;
    }
    friend constexpr auto operator<=>(const Utf16Range&, const Utf16Range&) = default;
};

} // namespace codelenses
