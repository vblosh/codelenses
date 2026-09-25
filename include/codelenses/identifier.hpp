#pragma once

#include <charconv>
#include <compare>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include "codelenses/result.hpp"

namespace codelenses {

// The tag prevents accidentally passing (for example) a FileId where a
// WorkspaceId is required.  The representation stays a small unsigned value
// so it can be copied into a SQLite binding or a wire DTO without ownership
// surprises.
template <typename Tag, typename Rep = std::uint64_t>
class StrongIdentifier {
    static_assert(std::is_integral_v<Rep> && std::is_unsigned_v<Rep>,
                  "StrongIdentifier requires an unsigned integral representation");

public:
    using value_type = Rep;

    [[nodiscard]] static constexpr Result<StrongIdentifier> from_value(Rep value) {
        if (value == 0) {
            return unexpected_result<StrongIdentifier>(ErrorCode::invalid_identifier,
                                                       "identifier must be non-zero");
        }
        return StrongIdentifier(value, private_tag{});
    }

    [[nodiscard]] static Result<StrongIdentifier> parse(std::string_view text) {
        if (text.empty()) {
            return unexpected_result<StrongIdentifier>(ErrorCode::empty_value,
                                                       "identifier text is empty");
        }
        Rep value{};
        const auto* first = text.data();
        const auto* last = first + text.size();
        const auto parsed = std::from_chars(first, last, value, 10);
        if (parsed.ec == std::errc::invalid_argument || parsed.ptr != last) {
            return unexpected_result<StrongIdentifier>(
                ErrorCode::invalid_identifier, "identifier must be an unsigned decimal value");
        }
        if (parsed.ec == std::errc::result_out_of_range) {
            return unexpected_result<StrongIdentifier>(ErrorCode::out_of_range,
                                                       "identifier is too large");
        }
        return from_value(value);
    }

    // This constructor is intentionally private in normal use.  Storage
    // adapters can use from_value/parse and cannot accidentally create zero.
    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_ != 0; }
    [[nodiscard]] std::string str() const { return std::to_string(value_); }

    friend constexpr auto operator<=>(const StrongIdentifier&, const StrongIdentifier&) = default;
    friend constexpr bool operator==(const StrongIdentifier&, const StrongIdentifier&) = default;

private:
    struct private_tag {};
    constexpr StrongIdentifier(Rep value, private_tag) : value_(value) {}
    Rep value_{};
};

struct IdentifierTag;
struct WorkspaceIdTag;
struct RevisionIdTag;
struct JobIdTag;
struct FileIdTag;
struct FileGenerationIdTag;
struct SymbolIdTag;
struct DeclarationIdTag;
struct OccurrenceIdTag;

using Identifier = StrongIdentifier<IdentifierTag>;
using WorkspaceId = StrongIdentifier<WorkspaceIdTag>;
using RevisionId = StrongIdentifier<RevisionIdTag>;
using JobId = StrongIdentifier<JobIdTag>;
using FileId = StrongIdentifier<FileIdTag>;
using FileGenerationId = StrongIdentifier<FileGenerationIdTag>;
using SymbolId = StrongIdentifier<SymbolIdTag>;
using DeclarationId = StrongIdentifier<DeclarationIdTag>;
using OccurrenceId = StrongIdentifier<OccurrenceIdTag>;

// A shorter spelling is useful in generic code while preserving the explicit
// StrongIdentifier spelling for public APIs and documentation.
template <typename Tag, typename Rep = std::uint64_t>
using StrongId = StrongIdentifier<Tag, Rep>;

} // namespace codelenses

namespace std {

template <typename Tag, typename Rep>
struct hash<codelenses::StrongIdentifier<Tag, Rep>> {
    [[nodiscard]] size_t
    operator()(const codelenses::StrongIdentifier<Tag, Rep>& id) const noexcept {
        return std::hash<Rep>{}(id.value());
    }
};

} // namespace std
