#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "codelenses/range.hpp"
#include "codelenses/result.hpp"

namespace codelenses {

[[nodiscard]] constexpr bool is_valid_utf8(std::span<const std::byte> bytes) noexcept {
    std::size_t index = 0;
    while (index < bytes.size()) {
        const auto first = std::to_integer<std::uint8_t>(bytes[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t count = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            count = 2;
            codepoint = first & 0x1fU;
            minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            count = 3;
            codepoint = first & 0x0fU;
            minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            count = 4;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (index + count > bytes.size())
            return false;
        for (std::size_t offset = 1; offset < count; ++offset) {
            const auto continuation = std::to_integer<std::uint8_t>(bytes[index + offset]);
            if ((continuation & 0xc0U) != 0x80U)
                return false;
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU))
            return false;
        index += count;
    }
    return true;
}

[[nodiscard]] inline Result<std::uint64_t>
utf8_byte_offset_to_utf16_code_units(std::span<const std::byte> bytes, std::uint64_t byte_offset) {
    if (byte_offset > bytes.size()) {
        return unexpected_result<std::uint64_t>(ErrorCode::out_of_range,
                                                "byte offset exceeds buffer size");
    }
    std::size_t index = 0;
    std::uint64_t code_units = 0;
    while (index < byte_offset) {
        const auto first = std::to_integer<std::uint8_t>(bytes[index]);
        if (first <= 0x7fU) {
            ++index;
            ++code_units;
            continue;
        }
        std::size_t count = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            count = 2;
            codepoint = first & 0x1fU;
            minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            count = 3;
            codepoint = first & 0x0fU;
            minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            count = 4;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return unexpected_result<std::uint64_t>(ErrorCode::invalid_argument,
                                                    "invalid utf-8 lead byte");
        }
        if (index + count > bytes.size()) {
            return unexpected_result<std::uint64_t>(ErrorCode::invalid_argument,
                                                    "truncated utf-8 sequence");
        }
        for (std::size_t offset = 1; offset < count; ++offset) {
            const auto continuation = std::to_integer<std::uint8_t>(bytes[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                return unexpected_result<std::uint64_t>(ErrorCode::invalid_argument,
                                                        "invalid utf-8 continuation byte");
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return unexpected_result<std::uint64_t>(ErrorCode::invalid_argument,
                                                    "invalid utf-8 codepoint");
        }
        index += count;
        if (index > byte_offset) {
            return unexpected_result<std::uint64_t>(ErrorCode::invalid_range,
                                                    "byte offset is not on a codepoint boundary");
        }
        code_units += (codepoint >= 0x10000U) ? 2U : 1U;
    }
    return code_units;
}

[[nodiscard]] inline Result<std::uint64_t>
utf8_byte_offset_to_utf16_code_units(std::string_view text, std::uint64_t byte_offset) {
    return utf8_byte_offset_to_utf16_code_units(std::as_bytes(std::span(text)), byte_offset);
}

[[nodiscard]] inline Result<std::uint64_t>
utf16_code_units_to_utf8_byte_offset(std::span<const std::byte> bytes,
                                     std::uint64_t target_code_units) {
    std::size_t index = 0;
    std::uint64_t current_units = 0;
    while (current_units < target_code_units) {
        if (index >= bytes.size()) {
            return unexpected_result<std::uint64_t>(ErrorCode::out_of_range,
                                                    "utf-16 code units exceed buffer size");
        }
        const auto first = std::to_integer<std::uint8_t>(bytes[index]);
        if (first <= 0x7fU) {
            ++index;
            ++current_units;
            continue;
        }
        std::size_t count = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            count = 2;
            codepoint = first & 0x1fU;
            minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            count = 3;
            codepoint = first & 0x0fU;
            minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            count = 4;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return unexpected_result<std::uint64_t>(ErrorCode::invalid_argument,
                                                    "invalid utf-8 lead byte");
        }
        if (index + count > bytes.size()) {
            return unexpected_result<std::uint64_t>(ErrorCode::invalid_argument,
                                                    "truncated utf-8 sequence");
        }
        for (std::size_t offset = 1; offset < count; ++offset) {
            const auto continuation = std::to_integer<std::uint8_t>(bytes[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                return unexpected_result<std::uint64_t>(ErrorCode::invalid_argument,
                                                        "invalid utf-8 continuation byte");
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return unexpected_result<std::uint64_t>(ErrorCode::invalid_argument,
                                                    "invalid utf-8 codepoint");
        }
        const std::uint64_t units_for_char = (codepoint >= 0x10000U) ? 2U : 1U;
        if (current_units + units_for_char > target_code_units) {
            return unexpected_result<std::uint64_t>(ErrorCode::invalid_range,
                                                    "target code units split a surrogate pair");
        }
        current_units += units_for_char;
        index += count;
    }
    return static_cast<std::uint64_t>(index);
}

[[nodiscard]] inline Result<std::uint64_t>
utf16_code_units_to_utf8_byte_offset(std::string_view text, std::uint64_t target_code_units) {
    return utf16_code_units_to_utf8_byte_offset(std::as_bytes(std::span(text)), target_code_units);
}

[[nodiscard]] inline Result<Utf16Range> utf8_byte_range_to_utf16(std::span<const std::byte> bytes,
                                                                 ByteRange byte_range) {
    if (!byte_range.valid()) {
        return unexpected_result<Utf16Range>(ErrorCode::invalid_range, "invalid byte range");
    }
    auto start = utf8_byte_offset_to_utf16_code_units(bytes, byte_range.start);
    if (!start)
        return std::unexpected(start.error());
    auto end = utf8_byte_offset_to_utf16_code_units(bytes, byte_range.end);
    if (!end)
        return std::unexpected(end.error());
    return Utf16Range{*start, *end};
}

[[nodiscard]] inline Result<Utf16Range> utf8_byte_range_to_utf16(std::string_view text,
                                                                 ByteRange byte_range) {
    return utf8_byte_range_to_utf16(std::as_bytes(std::span(text)), byte_range);
}

} // namespace codelenses
