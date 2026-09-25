#pragma once

#include "codelenses/adapters/adapter.hpp"

namespace codelenses::adapters {

class CAdapter final : public LanguageAdapter {
public:
    using LanguageAdapter::parse;

    CAdapter();
    ~CAdapter() override = default;

    [[nodiscard]] Language language() const noexcept override { return Language::c; }
    [[nodiscard]] std::string_view name() const noexcept override { return "CAdapter"; }
    [[nodiscard]] const LanguageCapabilities& capabilities() const noexcept override;

    // Configures a default compile-command context on this adapter instance
    void set_compile_command_context(CompileCommandContext context);
    void clear_compile_command_context() noexcept;
    [[nodiscard]] const std::optional<CompileCommandContext>&
    compile_command_context() const noexcept;

    [[nodiscard]] Result<AdapterResult> parse(std::string_view source,
                                              const std::filesystem::path& file_path = {},
                                              const std::stop_token& stop_token = {}) override;

    [[nodiscard]] Result<AdapterResult> parse(std::string_view source,
                                              const std::filesystem::path& file_path,
                                              const CompileCommandContext& context,
                                              const std::stop_token& stop_token = {}) override;

    [[nodiscard]] Result<std::vector<HighlightToken>>
    highlight(std::string_view source, const treesitter::Tree& tree) override;

    [[nodiscard]] Result<std::vector<HighlightToken>> highlight(std::string_view source);

    [[nodiscard]] static std::string_view highlighting_query() noexcept;

private:
    LanguageCapabilities capabilities_{};
    std::optional<CompileCommandContext> compile_context_{std::nullopt};
};

} // namespace codelenses::adapters
