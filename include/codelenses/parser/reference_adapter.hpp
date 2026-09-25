#pragma once

#include <filesystem>
#include <stop_token>
#include <string_view>
#include <vector>

#include "codelenses/adapters/adapter.hpp"
#include "codelenses/parser/highlight.hpp"

namespace codelenses::adapters {

class ReferenceAdapter : public LanguageAdapter {
public:
    using LanguageAdapter::parse;

    ReferenceAdapter();

    [[nodiscard]] Language language() const noexcept override { return Language::c; }

    [[nodiscard]] std::string_view name() const noexcept override { return "c"; }
    [[nodiscard]] std::string_view language_id() const noexcept { return "c"; }

    [[nodiscard]] const LanguageCapabilities& capabilities() const noexcept override;

    [[nodiscard]] Result<AdapterResult>
    parse(std::string_view source, const std::filesystem::path& file_path = {},
          const std::stop_token& stop_token = std::stop_token{}) override;

    [[nodiscard]] Result<std::vector<HighlightToken>>
    highlight(std::string_view source, const treesitter::Tree& tree) override;

private:
    LanguageCapabilities capabilities_{};
};

} // namespace codelenses::adapters
