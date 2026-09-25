#include "codelenses/treesitter/grammars.hpp"

extern "C" {
const TSLanguage* tree_sitter_c(void);
const TSLanguage* tree_sitter_cpp(void);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) const TSLanguage* tree_sitter_c_sharp(void) {
    return nullptr;
}
__attribute__((weak)) const TSLanguage* tree_sitter_python(void) {
    return nullptr;
}
__attribute__((weak)) const TSLanguage* tree_sitter_typescript(void) {
    return nullptr;
}
__attribute__((weak)) const TSLanguage* tree_sitter_tsx(void) {
    return nullptr;
}
__attribute__((weak)) const TSLanguage* tree_sitter_javascript(void) {
    return nullptr;
}
__attribute__((weak)) const TSLanguage* tree_sitter_go(void) {
    return nullptr;
}
__attribute__((weak)) const TSLanguage* tree_sitter_java(void) {
    return nullptr;
}
__attribute__((weak)) const TSLanguage* tree_sitter_bash(void) {
    return nullptr;
}
#else
const TSLanguage* tree_sitter_c_sharp(void);
const TSLanguage* tree_sitter_python(void);
const TSLanguage* tree_sitter_typescript(void);
const TSLanguage* tree_sitter_tsx(void);
const TSLanguage* tree_sitter_javascript(void);
const TSLanguage* tree_sitter_go(void);
const TSLanguage* tree_sitter_java(void);
const TSLanguage* tree_sitter_bash(void);
#endif
}

namespace codelenses::treesitter {

const TSLanguage* grammar_for_language(Language lang) noexcept {
    switch (lang) {
    case Language::c:
        return tree_sitter_c();
    case Language::cpp:
        return tree_sitter_cpp();
    case Language::csharp:
        return tree_sitter_c_sharp();
    case Language::python:
        return tree_sitter_python();
    case Language::typescript:
        return tree_sitter_typescript();
    case Language::javascript:
        return tree_sitter_javascript();
    case Language::go:
        return tree_sitter_go();
    case Language::java:
        return tree_sitter_java();
    case Language::shell:
    case Language::bash:
        return tree_sitter_bash();
    case Language::unknown:
        return nullptr;
    }
    return nullptr;
}

bool has_grammar_for_language(Language lang) noexcept {
    return grammar_for_language(lang) != nullptr;
}

const TSLanguage* grammar_for_tsx() noexcept {
    return tree_sitter_tsx();
}

std::string_view grammar_version(Language lang) noexcept {
    switch (lang) {
    case Language::c:
        return "0.24.2";
    case Language::cpp:
        return "0.23.4";
    case Language::csharp:
        return "0.23.1";
    case Language::python:
        return "0.23.6";
    case Language::typescript:
        return "0.23.2";
    case Language::javascript:
        return "0.23.1";
    case Language::go:
        return "0.23.4";
    case Language::java:
        return "0.23.5";
    case Language::shell:
    case Language::bash:
        return "0.23.3";
    case Language::unknown:
        return "";
    }
    return "";
}

} // namespace codelenses::treesitter
