#pragma once

#include <string_view>

#include "codelenses/language.hpp"
#include <tree_sitter/api.h>

namespace codelenses::treesitter {

// Returns the pinned Tree-sitter grammar for the given language enum,
// or nullptr if the language is unknown or unsupported.
[[nodiscard]] const TSLanguage* grammar_for_language(Language lang) noexcept;

// Returns true if a pinned grammar is available for the given language.
[[nodiscard]] bool has_grammar_for_language(Language lang) noexcept;

// Returns the pinned Tree-sitter grammar for TSX.
[[nodiscard]] const TSLanguage* grammar_for_tsx() noexcept;

// Returns the pinned upstream release version for the given language grammar.
[[nodiscard]] std::string_view grammar_version(Language lang) noexcept;

} // namespace codelenses::treesitter
