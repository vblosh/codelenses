#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "codelenses/domain/occurrence.hpp"
#include "codelenses/domain/reference.hpp"
#include "codelenses/domain/symbol.hpp"
#include "codelenses/language.hpp"
#include "codelenses/resolver/types.hpp"

namespace codelenses::resolver {

class SymbolResolver {
public:
    SymbolResolver() = default;

    // Adds a symbol to the resolution index.
    void add_symbol(const SymbolCandidate& candidate);
    void add_symbol(const Symbol& symbol, const std::string& file_path);

    // Clears all indexed symbols.
    void clear();

    [[nodiscard]] std::size_t symbol_count() const noexcept { return symbols_.size(); }

    [[nodiscard]] const SymbolCandidate* find_symbol_by_id(int64_t id) const;
    [[nodiscard]] const SymbolCandidate* find_symbol_by_key(std::string_view key) const;
    [[nodiscard]] std::vector<const SymbolCandidate*>
    find_symbols_by_name(std::string_view name) const;
    [[nodiscard]] std::vector<const SymbolCandidate*>
    find_symbols_by_qualified_name(std::string_view qname) const;

    // Finds the smallest containing symbol for an occurrence range in a given file.
    [[nodiscard]] std::optional<int64_t> find_enclosing_symbol_id(int64_t file_id,
                                                                  const SourceRange& range) const;

    // Resolves an occurrence to candidate targets, determining resolution state and confidence.
    [[nodiscard]] ResolvedOccurrence
    resolve_occurrence(const Occurrence& occ, const std::string& file_path, Language lang,
                       const std::vector<int64_t>& imported_file_ids = {}) const;

    // Resolves a reference occurrence to candidate targets.
    [[nodiscard]] ResolvedOccurrence
    resolve_reference(const ReferenceOccurrence& ref, const std::string& file_path, Language lang,
                      const std::vector<int64_t>& imported_file_ids = {}) const;

private:
    std::vector<SymbolCandidate> symbols_;
    std::unordered_map<int64_t, std::size_t> symbol_id_to_index_;
    std::unordered_map<std::string, std::vector<std::size_t>> symbols_by_name_;
    std::unordered_map<std::string, std::vector<std::size_t>> symbols_by_qname_;
    std::unordered_map<int64_t, std::vector<std::size_t>> symbols_by_file_id_;
    std::unordered_map<std::string, std::size_t> symbol_by_key_;
};

} // namespace codelenses::resolver
