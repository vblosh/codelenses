#include "codelenses/adapters/adapter.hpp"

#include <vector>

namespace codelenses::adapters {

namespace {

void walk_error_nodes(const treesitter::Node& node, Language lang,
                      std::vector<ParseDiagnostic>& out) {
    if (node.is_null())
        return;

    if (node.is_error()) {
        ParseDiagnostic diag{
            .severity = DiagnosticSeverity::error,
            .language = lang,
            .code = "syntax.error",
            .message = "Syntax error near '" + std::string(node.type()) + "'",
            .byte_range = node.byte_range(),
            .display_range = node.display_range(),
        };
        out.push_back(std::move(diag));
        // Don't recurse into children of an error node to avoid duplicate diagnostics for fragments
        return;
    }

    if (node.is_missing()) {
        ParseDiagnostic diag{
            .severity = DiagnosticSeverity::error,
            .language = lang,
            .code = "syntax.missing",
            .message = "Missing expected syntax: " + std::string(node.type()),
            .byte_range = node.byte_range(),
            .display_range = node.display_range(),
        };
        out.push_back(std::move(diag));
        return;
    }

    if (node.has_error()) {
        const uint32_t count = node.child_count();
        for (uint32_t i = 0; i < count; ++i) {
            walk_error_nodes(node.child(i), lang, out);
        }
    }
}

} // namespace

std::vector<ParseDiagnostic> collect_syntax_errors(const treesitter::Node& root, Language lang) {
    std::vector<ParseDiagnostic> diags;
    if (!root.is_null() && root.has_error()) {
        walk_error_nodes(root, lang, diags);
    }
    return diags;
}

} // namespace codelenses::adapters
