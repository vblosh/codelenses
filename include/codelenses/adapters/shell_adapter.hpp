#pragma once

#include "codelenses/adapters/adapter.hpp"

namespace codelenses::adapters {

class ShellAdapter final : public LanguageAdapter {
public:
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

private:
    Language lang_{Language::shell};
    LanguageCapabilities capabilities_{};
};

} // namespace codelenses::adapters
