#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "codelenses/hash.hpp"
#include "codelenses/result.hpp"

namespace codelenses::filesystem {

// Computes SHA-256 hash of byte buffer.
[[nodiscard]] Hash32 sha256(std::span<const std::byte> bytes) noexcept;

// Converts 32-byte hash to 64-char lowercase hexadecimal string.
[[nodiscard]] std::string to_hex(const Hash32& hash);

// Computes SHA-256 and returns lowercase hex string directly.
[[nodiscard]] std::string sha256_hex(std::span<const std::byte> bytes) noexcept;

// Checks whether byte buffer contains binary data (null bytes or invalid UTF-8).
[[nodiscard]] bool is_binary_buffer(std::span<const std::byte> bytes) noexcept;

struct CapturedFile {
    std::filesystem::path path;
    std::vector<std::byte> bytes;
    std::string content_hash; // 64-char lowercase hex
    uint64_t byte_size = 0;
    int64_t modified_ns = 0;
    bool is_binary = false;
    bool is_valid_utf8 = true;
    size_t bom_offset = 0;

    [[nodiscard]] std::string_view as_string_view() const noexcept {
        if (bom_offset >= bytes.size()) {
            return {};
        }
        return std::string_view(reinterpret_cast<const char*>(bytes.data() + bom_offset),
                                bytes.size() - bom_offset);
    }

    [[nodiscard]] std::span<const std::byte> content_bytes() const noexcept {
        if (bom_offset >= bytes.size()) {
            return {};
        }
        return std::span<const std::byte>(bytes).subspan(bom_offset);
    }
};

// Safely reads file content, verifies file size limits, ensures no concurrent modification
// during read via stat comparison, and computes content hash & binary/UTF-8 status.
// If workspace_root is provided, verifies that opened file is strictly contained within workspace_root.
[[nodiscard]] Result<CapturedFile>
capture_file(const std::filesystem::path& path,
             std::size_t max_bytes = 32U * 1024U * 1024U,
             const std::optional<std::filesystem::path>& workspace_root = std::nullopt);

} // namespace codelenses::filesystem
