#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "codelenses/adapters/adapter.hpp"
#include "codelenses/language.hpp"

namespace codelenses::adapters {

class AdapterRegistry {
public:
    AdapterRegistry() = default;
    ~AdapterRegistry() = default;

    AdapterRegistry(const AdapterRegistry&) = delete;
    AdapterRegistry& operator=(const AdapterRegistry&) = delete;

    // Registers a language adapter. Overwrites existing adapter for the same language.
    void register_adapter(std::unique_ptr<LanguageAdapter> adapter);

    // Looks up an adapter by language enum. Returns nullptr if not registered.
    [[nodiscard]] LanguageAdapter* get_adapter(Language lang) const noexcept;

    // Looks up an adapter by file path extension.
    [[nodiscard]] LanguageAdapter*
    get_adapter_for_path(const std::filesystem::path& path) const noexcept;

    // Returns true if an adapter is registered for the given language.
    [[nodiscard]] bool has_adapter(Language lang) const noexcept;

private:
    mutable std::mutex mutex_;
    std::unordered_map<Language, std::unique_ptr<LanguageAdapter>> adapters_;
};

// Returns the process-wide default adapter registry.
[[nodiscard]] AdapterRegistry& default_adapter_registry();

} // namespace codelenses::adapters
