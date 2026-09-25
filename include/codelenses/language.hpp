#pragma once

#include <filesystem>
#include <string_view>

#include "codelenses/result.hpp"

namespace codelenses {

enum class Language {
    c,
    cpp,
    csharp,
    python,
    typescript,
    javascript,
    go,
    java,
    shell,
    bash,
    unknown,
};

[[nodiscard]] Language language_from_path(const std::filesystem::path& path);
[[nodiscard]] Language language_from_extension(std::string_view extension);
[[nodiscard]] Result<Language> language_from_string(std::string_view name);
[[nodiscard]] inline Result<Language> parse_language(std::string_view name) {
    return language_from_string(name);
}
[[nodiscard]] std::string_view to_string(Language value) noexcept;

} // namespace codelenses
