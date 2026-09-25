#include "codelenses/filesystem/file_capture.hpp"
#include "codelenses/filesystem/path.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

#include "codelenses/utf8.hpp"

namespace codelenses::filesystem {
namespace {

using Word = std::uint32_t;

constexpr std::array<Word, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

[[nodiscard]] bool same_timestamp(const timespec& left, const timespec& right) noexcept {
    return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
}

[[nodiscard]] bool same_file(const struct stat& before, const struct stat& after) noexcept {
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
           before.st_mode == after.st_mode && before.st_nlink == after.st_nlink &&
           before.st_size == after.st_size && same_timestamp(before.st_mtim, after.st_mtim) &&
           same_timestamp(before.st_ctim, after.st_ctim);
}

void compress_block(std::array<Word, 8>& state,
                    std::span<const std::byte, 64> block) noexcept {
    std::array<Word, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        const auto offset = index * 4U;
        words[index] = (std::to_integer<Word>(block[offset]) << 24U) |
                       (std::to_integer<Word>(block[offset + 1U]) << 16U) |
                       (std::to_integer<Word>(block[offset + 2U]) << 8U) |
                       std::to_integer<Word>(block[offset + 3U]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
        const Word s0 = std::rotr(words[index - 15U], 7) ^
                        std::rotr(words[index - 15U], 18) ^
                        (words[index - 15U] >> 3U);
        const Word s1 = std::rotr(words[index - 2U], 17) ^
                        std::rotr(words[index - 2U], 19) ^
                        (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state;
    for (std::size_t index = 0; index < words.size(); ++index) {
        const Word choice = (e & f) ^ (~e & g);
        const Word majority = (a & b) ^ (a & c) ^ (b & c);
        const Word sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const Word sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const Word temp1 = h + sum1 + choice + kRoundConstants[index] + words[index];
        const Word temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace

Hash32 sha256(std::span<const std::byte> bytes) noexcept {
    std::array<Word, 8> state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                              0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::size_t offset = 0;
    while (bytes.size() - offset >= 64U) {
        compress_block(state, std::span<const std::byte, 64>(bytes.data() + offset, 64));
        offset += 64U;
    }
    std::array<std::byte, 128> tail{};
    const auto remaining = bytes.size() - offset;
    for (std::size_t index = 0; index < remaining; ++index)
        tail[index] = bytes[offset + index];
    tail[remaining] = std::byte{0x80};
    const std::size_t tail_size = remaining < 56U ? 64U : 128U;
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
    for (std::size_t index = 0; index < 8; ++index) {
        tail[tail_size - 1U - index] =
            static_cast<std::byte>((bit_length >> (index * 8U)) & 0xffU);
    }
    compress_block(state, std::span<const std::byte, 64>(tail.data(), 64));
    if (tail_size == 128U) {
        compress_block(state, std::span<const std::byte, 64>(tail.data() + 64, 64));
    }

    Hash32 result{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        result[index * 4U] = static_cast<std::byte>((state[index] >> 24U) & 0xffU);
        result[index * 4U + 1U] = static_cast<std::byte>((state[index] >> 16U) & 0xffU);
        result[index * 4U + 2U] = static_cast<std::byte>((state[index] >> 8U) & 0xffU);
        result[index * 4U + 3U] = static_cast<std::byte>(state[index] & 0xffU);
    }
    return result;
}

std::string to_hex(const Hash32& hash) {
    constexpr char hex_chars[] = "0123456789abcdef";
    std::string s;
    s.resize(64);
    for (std::size_t i = 0; i < hash.size(); ++i) {
        const auto val = std::to_integer<std::uint8_t>(hash[i]);
        s[i * 2] = hex_chars[val >> 4];
        s[i * 2 + 1] = hex_chars[val & 0x0f];
    }
    return s;
}

std::string sha256_hex(std::span<const std::byte> bytes) noexcept {
    return to_hex(sha256(bytes));
}

bool is_binary_buffer(std::span<const std::byte> bytes) noexcept {
    if (bytes.empty()) {
        return false;
    }
    const auto inspect_len = std::min(bytes.size(), std::size_t{8192});
    for (std::size_t i = 0; i < inspect_len; ++i) {
        if (bytes[i] == std::byte{0}) {
            return true;
        }
    }
    return !is_valid_utf8(bytes);
}

Result<CapturedFile> capture_file(const std::filesystem::path& path, std::size_t max_bytes,
                                  const std::optional<std::filesystem::path>& workspace_root) {
    if (max_bytes == 0) {
        return unexpected_result<CapturedFile>(ErrorCode::invalid_argument,
                                               "file capture size limit must be positive");
    }

    if (workspace_root) {
        std::error_code ec;
        auto canonical_target = std::filesystem::canonical(path, ec);
        if (ec || !is_contained_in(*workspace_root, canonical_target)) {
            return unexpected_result<CapturedFile>(
                ErrorCode::invalid_argument, "file path escapes workspace root", path.string());
        }
    }

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return unexpected_result<CapturedFile>(
            ErrorCode::not_found, "cannot open file: " + std::string(std::strerror(errno)),
            path.string());
    }

    struct FdGuard {
        int d;
        ~FdGuard() {
            if (d >= 0)
                ::close(d);
        }
    } guard{fd};

    if (workspace_root) {
        std::error_code ec;
        char fd_proc[64];
        std::snprintf(fd_proc, sizeof(fd_proc), "/proc/self/fd/%d", fd);
        auto fd_target = std::filesystem::canonical(fd_proc, ec);
        if (!ec && !is_contained_in(*workspace_root, fd_target)) {
            return unexpected_result<CapturedFile>(ErrorCode::invalid_argument,
                                                   "opened file descriptor escapes workspace root",
                                                   path.string());
        }
    }

    struct stat before {};
    if (::fstat(fd, &before) != 0) {
        return unexpected_result<CapturedFile>(
            ErrorCode::failed, "cannot stat file: " + std::string(std::strerror(errno)),
            path.string());
    }

    if (!S_ISREG(before.st_mode)) {
        return unexpected_result<CapturedFile>(
            ErrorCode::invalid_argument, "path is not a regular file", path.string());
    }

    if (before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > max_bytes) {
        return unexpected_result<CapturedFile>(
            ErrorCode::out_of_range, "file size exceeds maximum allowed limit", path.string());
    }

    std::vector<std::byte> bytes;
    bytes.reserve(static_cast<std::size_t>(before.st_size));
    std::array<std::byte, 64U * 1024U> buffer{};

    while (true) {
        const auto remaining = max_bytes - bytes.size();
        const auto to_read = std::min(buffer.size(), remaining + 1U);
        const auto count = ::read(fd, buffer.data(), to_read);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return unexpected_result<CapturedFile>(
                ErrorCode::failed, "cannot read file: " + std::string(std::strerror(errno)),
                path.string());
        }
        if (count == 0)
            break;
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
        if (bytes.size() > max_bytes) {
            return unexpected_result<CapturedFile>(
                ErrorCode::out_of_range, "file exceeds maximum allowed size", path.string());
        }
    }

    struct stat after_fd {};
    struct stat after_path {};
    if (::fstat(fd, &after_fd) != 0 || ::stat(path.c_str(), &after_path) != 0) {
        return unexpected_result<CapturedFile>(
            ErrorCode::conflict, "file metadata changed while reading", path.string());
    }

    if (!same_file(before, after_fd) || !same_file(before, after_path) ||
        static_cast<std::uint64_t>(after_fd.st_size) != bytes.size()) {
        return unexpected_result<CapturedFile>(
            ErrorCode::conflict, "file content changed while reading", path.string());
    }

    if (workspace_root) {
        std::error_code ec;
        auto canonical_target = std::filesystem::canonical(path, ec);
        if (ec || !is_contained_in(*workspace_root, canonical_target)) {
            return unexpected_result<CapturedFile>(
                ErrorCode::conflict, "file path escaped workspace root during read", path.string());
        }
    }

    constexpr std::array<std::byte, 3> kUtf8Bom{std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}};
    const bool has_bom = bytes.size() >= kUtf8Bom.size() &&
                         std::equal(kUtf8Bom.begin(), kUtf8Bom.end(), bytes.begin());

    const bool is_binary = is_binary_buffer(bytes);
    const bool valid_utf8 = is_valid_utf8(bytes);
    const auto hash_hex = sha256_hex(bytes);

    int64_t mod_ns = 0;
    if (after_fd.st_mtim.tv_sec >= 0 && after_fd.st_mtim.tv_nsec >= 0) {
        mod_ns = static_cast<int64_t>(after_fd.st_mtim.tv_sec) * 1'000'000'000LL +
                 static_cast<int64_t>(after_fd.st_mtim.tv_nsec);
    }

    return CapturedFile{
        .path = path,
        .bytes = std::move(bytes),
        .content_hash = hash_hex,
        .byte_size = static_cast<uint64_t>(after_fd.st_size),
        .modified_ns = mod_ns,
        .is_binary = is_binary,
        .is_valid_utf8 = valid_utf8,
        .bom_offset = has_bom ? 3U : 0U,
    };
}

} // namespace codelenses::filesystem
