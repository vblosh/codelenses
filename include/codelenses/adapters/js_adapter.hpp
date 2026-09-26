#pragma once
 
#include "codelenses/adapters/ts_adapter.hpp"

namespace codelenses::adapters {

class JavaScriptAdapter final : public TypeScriptAdapter {
public:
    explicit JavaScriptAdapter() : TypeScriptAdapter(Language::javascript) {}
    ~JavaScriptAdapter() override = default;

    [[nodiscard]] std::string_view name() const noexcept override {
        return "JavaScriptAdapter";
    }
};

} // namespace codelenses::adapters
