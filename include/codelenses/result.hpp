#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace codelenses {

// Error is the value carried by every recoverable domain failure.  Its text is
// owned deliberately: errors routinely cross worker, storage, and HTTP
// boundaries, so keeping a string_view here would make their lifetime
// dependent on an implementation detail at the call site.
enum class ErrorCode {
    invalid_argument,
    empty_value,
    invalid_identifier,
    invalid_range,
    out_of_range,
    unsupported,
    not_found,
    conflict,
    cancelled,
    failed,
};

struct Error {
    ErrorCode code{ErrorCode::failed};
    std::string message;
    std::string context;
    std::optional<std::uint64_t> expected_revision;
    std::optional<std::uint64_t> current_revision;

    friend bool operator==(const Error&, const Error&) = default;
};

using DomainError = Error;

template <typename T, typename E = Error>
using Result = std::expected<T, E>;

using VoidResult = Result<void>;

[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::invalid_argument:
        return "invalid_argument";
    case ErrorCode::empty_value:
        return "empty_value";
    case ErrorCode::invalid_identifier:
        return "invalid_identifier";
    case ErrorCode::invalid_range:
        return "invalid_range";
    case ErrorCode::out_of_range:
        return "out_of_range";
    case ErrorCode::unsupported:
        return "unsupported";
    case ErrorCode::not_found:
        return "not_found";
    case ErrorCode::conflict:
        return "conflict";
    case ErrorCode::cancelled:
        return "cancelled";
    case ErrorCode::failed:
        return "failed";
    }
    return "failed";
}

[[nodiscard]] inline Error make_error(ErrorCode code, std::string message,
                                      std::string context = {}) {
    return Error{code, std::move(message), std::move(context), std::nullopt, std::nullopt};
}

template <typename T>
[[nodiscard]] inline Result<T> unexpected_result(ErrorCode code, std::string message,
                                                 std::string context = {}) {
    return std::unexpected<Error>(make_error(code, std::move(message), std::move(context)));
}

} // namespace codelenses
