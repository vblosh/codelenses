#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codelenses/range.hpp"
#include <nlohmann/json.hpp>

namespace codelenses {

namespace adapters {

struct HighlightToken {
    uint32_t line = 0;         // 0-indexed line
    uint32_t start_column = 0; // 0-indexed column
    uint32_t length = 0;
    uint32_t token_type = 0;      // index in legend
    uint32_t token_modifiers = 0; // bitmask
    ByteRange byte_range{};
    DisplayRange display_range{};

    bool operator==(const HighlightToken& other) const = default;
};

} // namespace adapters

class HighlightLegend {
public:
    static inline const std::vector<std::string> kDefaultTokenTypes = {
        "type",      "class",    "enum",     "interface",  "struct",  "typeParameter",
        "parameter", "variable", "property", "enumMember", "event",   "function",
        "method",    "macro",    "keyword",  "modifier",   "comment", "string",
        "number",    "regexp",   "operator", "decorator",  "label",
    };

    static inline const std::vector<std::string> kDefaultTokenModifiers = {
        "declaration", "definition", "readonly",     "static",        "deprecated",
        "abstract",    "async",      "modification", "documentation", "defaultLibrary",
    };

    HighlightLegend()
        : token_types_(kDefaultTokenTypes), token_modifiers_(kDefaultTokenModifiers) {}

    HighlightLegend(std::vector<std::string> types, std::vector<std::string> modifiers)
        : token_types_(std::move(types)), token_modifiers_(std::move(modifiers)) {}

    [[nodiscard]] const std::vector<std::string>& token_types() const noexcept {
        return token_types_;
    }

    [[nodiscard]] const std::vector<std::string>& token_modifiers() const noexcept {
        return token_modifiers_;
    }

    [[nodiscard]] std::optional<uint32_t> token_type_index(std::string_view type) const noexcept {
        for (size_t i = 0; i < token_types_.size(); ++i) {
            if (token_types_[i] == type) {
                return static_cast<uint32_t>(i);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string_view> token_type_name(uint32_t index) const noexcept {
        if (index < token_types_.size()) {
            return token_types_[index];
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<uint32_t>
    token_modifier_bit(std::string_view modifier) const noexcept {
        for (size_t i = 0; i < token_modifiers_.size(); ++i) {
            if (token_modifiers_[i] == modifier) {
                return static_cast<uint32_t>(1u << i);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] uint32_t
    encode_modifiers(const std::vector<std::string>& modifiers) const noexcept {
        uint32_t mask = 0;
        for (const auto& mod : modifiers) {
            if (auto bit = token_modifier_bit(mod)) {
                mask |= *bit;
            }
        }
        return mask;
    }

    [[nodiscard]] std::vector<std::string> decode_modifiers(uint32_t bitmask) const {
        std::vector<std::string> result;
        for (size_t i = 0; i < token_modifiers_.size(); ++i) {
            if (bitmask & (1u << i)) {
                result.push_back(token_modifiers_[i]);
            }
        }
        return result;
    }

    [[nodiscard]] nlohmann::json to_json() const {
        return nlohmann::json{
            {"tokenTypes", token_types_},
            {"tokenModifiers", token_modifiers_},
        };
    }

    static HighlightLegend from_json(const nlohmann::json& j) {
        std::vector<std::string> types;
        std::vector<std::string> modifiers;
        if (j.contains("tokenTypes") && j["tokenTypes"].is_array()) {
            types = j["tokenTypes"].get<std::vector<std::string>>();
        } else {
            types = kDefaultTokenTypes;
        }
        if (j.contains("tokenModifiers") && j["tokenModifiers"].is_array()) {
            modifiers = j["tokenModifiers"].get<std::vector<std::string>>();
        } else {
            modifiers = kDefaultTokenModifiers;
        }
        return HighlightLegend(std::move(types), std::move(modifiers));
    }

    static const HighlightLegend& default_legend() {
        static const HighlightLegend legend;
        return legend;
    }

private:
    std::vector<std::string> token_types_;
    std::vector<std::string> token_modifiers_;
};

namespace adapters {

inline void to_json(nlohmann::json& j, const HighlightToken& token) {
    j = nlohmann::json::object({
        {"line", token.line},
        {"startColumn", token.start_column},
        {"length", token.length},
        {"tokenType", token.token_type},
        {"tokenModifiers", token.token_modifiers},
    });
}

inline void from_json(const nlohmann::json& j, HighlightToken& token) {
    if (j.contains("line"))
        token.line = j["line"].get<uint32_t>();
    if (j.contains("startColumn"))
        token.start_column = j["startColumn"].get<uint32_t>();
    if (j.contains("length"))
        token.length = j["length"].get<uint32_t>();
    if (j.contains("tokenType"))
        token.token_type = j["tokenType"].get<uint32_t>();
    if (j.contains("tokenModifiers"))
        token.token_modifiers = j["tokenModifiers"].get<uint32_t>();
}

} // namespace adapters

} // namespace codelenses
