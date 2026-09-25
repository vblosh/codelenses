#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "codelenses/diagnostic.hpp"
#include "codelenses/hash.hpp"
#include "codelenses/model.hpp"
#include "codelenses/range.hpp"
#include "codelenses/result.hpp"

namespace codelenses::worker {

// The protocol is deliberately small and self-contained.  A worker receives
// one complete frame at a time; it never interprets a path as a filesystem
// operation and it never receives a database handle or connection.
using Bytes = std::vector<std::byte>;
using Hash32 = ::codelenses::Hash32;
using ContentHash = Hash32;
using ContextFingerprint = Hash32;

inline constexpr std::uint32_t kFrameHeaderSize = 12;
inline constexpr std::uint8_t kProtocolVersion = 2;

enum class FrameType : std::uint8_t {
    request = 1,
    response = 2,
};

enum class CompletionStatus : std::uint8_t {
    complete = 1,
    failed = 2,
    cancelled = 3,
    degraded = 4,
};

enum class FactKind : std::uint8_t {
    symbol = 1,
    declaration = 2,
    scope = 3,
    reference = 4,
    call = 5,
    inheritance = 6,
    implementation = 7,
    include = 8,
    import = 9,
};

// All limits are applied before a variable-sized allocation.  max_frame_size
// includes the 12-byte frame header; max_field_size applies to each encoded
// string/blob field and max_content_size is an additional request-content
// bound.  Counts are bounded independently to avoid fact/diagnostic floods.
struct ProtocolLimits {
    std::size_t max_frame_size = 64U * 1024U * 1024U;
    std::size_t max_field_size = 32U * 1024U * 1024U;
    std::size_t max_content_size = 32U * 1024U * 1024U;
    std::size_t max_path_size = 4096U;
    std::size_t max_fact_count = 100000U;
    std::size_t max_diagnostic_count = 10000U;
    std::size_t max_candidate_target_count = 1000U;

    [[nodiscard]] Result<void> validate() const;
};

struct RequestIdentity {
    std::uint64_t request_id{};
    std::string workspace_relative_path;
    Language language{Language::unknown};
    ContextFingerprint context_fingerprint{};
    ContentHash content_hash{};

    friend bool operator==(const RequestIdentity&, const RequestIdentity&) = default;
};

struct ResourceLimits {
    std::uint64_t deadline_millis{};
    std::uint64_t memory_bytes{};
    std::uint32_t max_fact_count{};
    std::uint32_t max_diagnostic_count{};
};

struct Request {
    RequestIdentity identity;
    Bytes content;
    ResourceLimits resources;
};

struct Fact {
    FactKind kind{FactKind::symbol};
    ByteRange range{};
    std::string name;
    std::string qualified_name;
    NodeKind symbol_kind{NodeKind::function};
    DisplayRange display_range{};
    std::optional<std::string> enclosing_scope;
    bool is_definition{true};
    std::string signature;
    std::vector<std::string> candidate_targets;
};

struct Diagnostic {
    DiagnosticSeverity severity{DiagnosticSeverity::error};
    Language language{Language::unknown};
    std::string code;
    std::string message;
    std::optional<ByteRange> byte_range;
    std::optional<DisplayRange> display_range;
};

struct Response {
    RequestIdentity identity;
    CompletionStatus status{CompletionStatus::complete};
    std::uint64_t input_size{};
    std::vector<Fact> facts;
    std::vector<Diagnostic> diagnostics;
};

struct Frame {
    std::uint8_t version{kProtocolVersion};
    FrameType type{FrameType::request};
    Bytes payload;
};

[[nodiscard]] constexpr bool valid_frame_type(FrameType value) noexcept {
    return value == FrameType::request || value == FrameType::response;
}

[[nodiscard]] constexpr bool valid_completion_status(CompletionStatus value) noexcept {
    switch (value) {
    case CompletionStatus::complete:
    case CompletionStatus::failed:
    case CompletionStatus::cancelled:
    case CompletionStatus::degraded:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool valid_fact_kind(FactKind value) noexcept {
    switch (value) {
    case FactKind::symbol:
    case FactKind::declaration:
    case FactKind::scope:
    case FactKind::reference:
    case FactKind::call:
    case FactKind::inheritance:
    case FactKind::implementation:
    case FactKind::include:
    case FactKind::import:
        return true;
    }
    return false;
}

[[nodiscard]] Result<Frame> decode_frame(std::span<const std::byte> encoded,
                                         const ProtocolLimits& limits = {});
[[nodiscard]] Result<Bytes> encode_frame(const Frame& frame, const ProtocolLimits& limits = {});

[[nodiscard]] Result<Bytes> encode_request(const Request& request,
                                           const ProtocolLimits& limits = {});
[[nodiscard]] Result<Request> decode_request(std::span<const std::byte> encoded,
                                             const ProtocolLimits& limits = {});

[[nodiscard]] Result<Bytes> encode_response(const Response& response,
                                            const ProtocolLimits& limits = {});
[[nodiscard]] Result<Response> decode_response(std::span<const std::byte> encoded,
                                               const ProtocolLimits& limits = {});
[[nodiscard]] Result<Response> decode_response(std::span<const std::byte> encoded,
                                               const RequestIdentity& expected_identity,
                                               const ProtocolLimits& limits = {});
[[nodiscard]] Result<void> validate_response_identity(const RequestIdentity& expected,
                                                      const RequestIdentity& actual);

// These aliases make the direction explicit at process boundaries and keep
// call sites readable without exposing serialization implementation details.
[[nodiscard]] inline Result<Bytes> serialize_request(const Request& request,
                                                     const ProtocolLimits& limits = {}) {
    return encode_request(request, limits);
}
[[nodiscard]] inline Result<Request> deserialize_request(std::span<const std::byte> encoded,
                                                         const ProtocolLimits& limits = {}) {
    return decode_request(encoded, limits);
}
[[nodiscard]] inline Result<Bytes> serialize_response(const Response& response,
                                                      const ProtocolLimits& limits = {}) {
    return encode_response(response, limits);
}
[[nodiscard]] inline Result<Response> deserialize_response(std::span<const std::byte> encoded,
                                                           const ProtocolLimits& limits = {}) {
    return decode_response(encoded, limits);
}

} // namespace codelenses::worker
