#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "codelenses/language.hpp"
#include "codelenses/model.hpp"

namespace codelenses::resolver {

// Normalizes a function/method signature by extracting parameter types and stripping parameter
// names.
[[nodiscard]] std::string normalize_signature(std::string_view signature);

// Generates a deterministic, stable symbol key across re-indexing.
// Format: <lang>:<rel_path>#<kind>#<qualified_name>[#<norm_sig>][@<start_byte>]
[[nodiscard]] std::string
generate_symbol_key(std::string_view language, std::string_view rel_path, std::string_view kind,
                    std::string_view qualified_name, std::string_view signature = "",
                    std::optional<int64_t> disambiguator_byte = std::nullopt);

[[nodiscard]] std::string
generate_symbol_key(Language language, std::string_view rel_path, NodeKind kind,
                    std::string_view qualified_name, std::string_view signature = "",
                    std::optional<int64_t> disambiguator_byte = std::nullopt);

} // namespace codelenses::resolver
