#include "codelenses/adapters/registry.hpp"

#include "codelenses/adapters/c_adapter.hpp"
#include "codelenses/adapters/cpp_adapter.hpp"
#include "codelenses/adapters/csharp_adapter.hpp"
#include "codelenses/adapters/go_adapter.hpp"
#include "codelenses/adapters/java_adapter.hpp"
#include "codelenses/adapters/js_adapter.hpp"
#include "codelenses/adapters/python_adapter.hpp"
#include "codelenses/adapters/shell_adapter.hpp"
#include "codelenses/adapters/ts_adapter.hpp"

namespace codelenses::adapters {

void AdapterRegistry::register_adapter(std::unique_ptr<LanguageAdapter> adapter) {
    if (!adapter)
        return;
    std::lock_guard lock(mutex_);
    const Language lang = adapter->language();
    adapters_[lang] = std::move(adapter);
}

LanguageAdapter* AdapterRegistry::get_adapter(Language lang) const noexcept {
    std::lock_guard lock(mutex_);
    const auto it = adapters_.find(lang);
    if (it != adapters_.end()) {
        return it->second.get();
    }
    return nullptr;
}

LanguageAdapter*
AdapterRegistry::get_adapter_for_path(const std::filesystem::path& path) const noexcept {
    const Language lang = language_from_path(path);
    return get_adapter(lang);
}

bool AdapterRegistry::has_adapter(Language lang) const noexcept {
    std::lock_guard lock(mutex_);
    return adapters_.contains(lang);
}

AdapterRegistry& default_adapter_registry() {
    static AdapterRegistry registry;
    static const bool initialized = [&]() {
        registry.register_adapter(std::make_unique<CAdapter>());
        registry.register_adapter(std::make_unique<CppAdapter>());
        registry.register_adapter(std::make_unique<CSharpAdapter>());
        registry.register_adapter(std::make_unique<PythonAdapter>());
        registry.register_adapter(std::make_unique<TypeScriptAdapter>(Language::typescript));
        registry.register_adapter(std::make_unique<JavaScriptAdapter>());
        registry.register_adapter(std::make_unique<GoAdapter>());
        registry.register_adapter(std::make_unique<JavaAdapter>());
        registry.register_adapter(std::make_unique<ShellAdapter>(Language::shell));
        registry.register_adapter(std::make_unique<ShellAdapter>(Language::bash));
        return true;
    }();
    (void)initialized;
    return registry;
}

} // namespace codelenses::adapters
