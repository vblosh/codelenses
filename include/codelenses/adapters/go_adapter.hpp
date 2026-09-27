#pragma once

#include "codelenses/adapters/adapter.hpp"

namespace codelenses::adapters {

class GoAdapter final : public LanguageAdapter {
public:
    using LanguageAdapter::parse;

    GoAdapter();
    ~GoAdapter() override = default;

    [[nodiscard]] Language language() const noexcept override { return Language::go; }
    [[nodiscard]] std::string_view name() const noexcept override { return "GoAdapter"; }
    [[nodiscard]] const LanguageCapabilities& capabilities() const noexcept override;

    [[nodiscard]] Result<AdapterResult> parse(std::string_view source,
                                              const std::filesystem::path& file_path = {},
                                              const std::stop_token& stop_token = {}) override;

    [[nodiscard]] Result<std::vector<HighlightToken>>
    highlight(std::string_view source, const treesitter::Tree& tree) override;
    [[nodiscard]] Result<std::vector<HighlightToken>> highlight(std::string_view source);

    [[nodiscard]] static std::string_view highlighting_query() noexcept;

private:
    LanguageCapabilities capabilities_{};
};

} // namespace codelenses::adapters
