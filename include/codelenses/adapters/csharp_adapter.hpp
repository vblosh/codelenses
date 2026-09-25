#pragma once

#include "codelenses/adapters/adapter.hpp"

namespace codelenses::adapters {

class CSharpAdapter final : public LanguageAdapter {
public:
    CSharpAdapter();
    ~CSharpAdapter() override = default;

    [[nodiscard]] Language language() const noexcept override { return Language::csharp; }
    [[nodiscard]] std::string_view name() const noexcept override { return "CSharpAdapter"; }
    [[nodiscard]] const LanguageCapabilities& capabilities() const noexcept override;

    [[nodiscard]] Result<AdapterResult> parse(std::string_view source,
                                              const std::filesystem::path& file_path = {},
                                              const std::stop_token& stop_token = {}) override;

private:
    LanguageCapabilities capabilities_{};
};

} // namespace codelenses::adapters
