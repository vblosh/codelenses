#include "codelenses/resolver/symbol_key.hpp"

#include <cctype>
#include <vector>

namespace codelenses::resolver {

std::string normalize_signature(std::string_view sig) {
    auto open_paren = sig.find('(');
    auto close_paren = sig.rfind(')');
    if (open_paren == std::string_view::npos || close_paren == std::string_view::npos ||
        close_paren <= open_paren) {
        // No parenthesized parameter list, strip redundant whitespace
        std::string result;
        bool in_space = false;
        for (char c : sig) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!in_space && !result.empty()) {
                    result += ' ';
                    in_space = true;
                }
            } else {
                result += c;
                in_space = false;
            }
        }
        return result;
    }

    std::string_view params_str = sig.substr(open_paren + 1, close_paren - open_paren - 1);
    std::vector<std::string> param_types;
    std::size_t start = 0;
    int template_depth = 0;

    for (std::size_t i = 0; i <= params_str.size(); ++i) {
        if (i == params_str.size() || (params_str[i] == ',' && template_depth == 0)) {
            std::string_view p = params_str.substr(start, i - start);
            while (!p.empty() && std::isspace(static_cast<unsigned char>(p.front())))
                p.remove_prefix(1);
            while (!p.empty() && std::isspace(static_cast<unsigned char>(p.back())))
                p.remove_suffix(1);

            if (!p.empty() && p != "void") {
                // If there is a space, see if the last token is a parameter name (an identifier)
                // rather than part of a multi-word type like 'unsigned int', 'const char', etc.
                auto last_space = p.rfind(' ');
                if (last_space != std::string_view::npos) {
                    std::string_view last_word = p.substr(last_space + 1);
                    static constexpr std::string_view kKeywords[] = {
                        "int",    "char", "short", "long",   "float",
                        "double", "bool", "void",  "signed", "unsigned"};
                    bool is_keyword = false;
                    for (auto kw : kKeywords) {
                        if (last_word == kw) {
                            is_keyword = true;
                            break;
                        }
                    }

                    if (!is_keyword && !last_word.empty() &&
                        (std::isalpha(static_cast<unsigned char>(last_word[0])) ||
                         last_word[0] == '_')) {
                        bool is_ident = true;
                        for (char c : last_word) {
                            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                                is_ident = false;
                                break;
                            }
                        }
                        if (is_ident) {
                            p = p.substr(0, last_space);
                            while (!p.empty() && std::isspace(static_cast<unsigned char>(p.back())))
                                p.remove_suffix(1);
                        }
                    }
                }

                std::string norm_type;
                bool in_space = false;
                for (char c : p) {
                    if (std::isspace(static_cast<unsigned char>(c))) {
                        if (!in_space) {
                            norm_type += ' ';
                            in_space = true;
                        }
                    } else {
                        norm_type += c;
                        in_space = false;
                    }
                }
                param_types.push_back(std::move(norm_type));
            }
            start = i + 1;
        } else if (params_str[i] == '<') {
            template_depth++;
        } else if (params_str[i] == '>') {
            if (template_depth > 0)
                template_depth--;
        }
    }

    std::string result = "(";
    for (std::size_t i = 0; i < param_types.size(); ++i) {
        if (i > 0)
            result += ", ";
        result += param_types[i];
    }
    result += ")";
    return result;
}

std::string generate_symbol_key(std::string_view language, std::string_view rel_path,
                                std::string_view kind, std::string_view qualified_name,
                                std::string_view signature,
                                std::optional<int64_t> disambiguator_byte) {
    std::string key;
    key.reserve(language.size() + rel_path.size() + kind.size() + qualified_name.size() + 32);

    key += language;
    key += ":";
    key += rel_path;
    key += "#";
    key += kind;
    key += "#";
    key += qualified_name;

    if (!signature.empty()) {
        std::string norm_sig = normalize_signature(signature);
        if (!norm_sig.empty()) {
            key += "#";
            key += norm_sig;
        }
    }

    if (disambiguator_byte.has_value()) {
        key += "@";
        key += std::to_string(*disambiguator_byte);
    }

    return key;
}

std::string generate_symbol_key(Language language, std::string_view rel_path, NodeKind kind,
                                std::string_view qualified_name, std::string_view signature,
                                std::optional<int64_t> disambiguator_byte) {
    return generate_symbol_key(to_string(language), rel_path, to_string(kind), qualified_name,
                               signature, disambiguator_byte);
}

} // namespace codelenses::resolver
