#pragma once

#include "codelenses/adapters/adapter.hpp"

namespace codelenses::adapters {

class ShellAdapter final : public LanguageAdapter {
public:
    using LanguageAdapter::parse;

    explicit ShellAdapter(Language lang = Language::shell);
    ~ShellAdapter() override = default;

    [[nodiscard]] Language language() const noexcept override { return lang_; }
    [[nodiscard]] std::string_view name() const noexcept override {
        return lang_ == Language::bash ? "BashAdapter" : "ShellAdapter";
    }
    [[nodiscard]] const LanguageCapabilities& capabilities() const noexcept override;

    [[nodiscard]] Result<AdapterResult> parse(std::string_view source,
                                              const std::filesystem::path& file_path = {},
                                              const std::stop_token& stop_token = {}) override;

    [[nodiscard]] Result<std::vector<HighlightToken>>
    highlight(std::string_view source, const treesitter::Tree& tree) override;

    [[nodiscard]] Result<std::vector<HighlightToken>> highlight(std::string_view source);

    [[nodiscard]] static std::string_view highlighting_query() noexcept;

private:
    Language lang_{Language::shell};
    LanguageCapabilities capabilities_{};
};

} // namespace codelenses::adapters
