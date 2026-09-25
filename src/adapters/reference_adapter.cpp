#include "codelenses/parser/reference_adapter.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

#include "codelenses/adapters/c_adapter.hpp"
#include "codelenses/parser/coordinate_converter.hpp"
#include "codelenses/parser/highlight.hpp"
#include "codelenses/treesitter/grammars.hpp"
#include "codelenses/treesitter/parser.hpp"

namespace codelenses::adapters {

namespace {

void extract_highlights_from_node(const treesitter::Node& node, std::string_view source,
                                  const CoordinateConverter& converter,
                                  const HighlightLegend& legend, std::vector<HighlightToken>& out) {
    if (node.is_null())
        return;

    const std::string_view type = node.type();

    std::optional<uint32_t> token_type;
    uint32_t modifiers = 0;

    if (type == "primitive_type" || type == "type_identifier" || type == "sized_type_specifier") {
        token_type = legend.token_type_index("type");
    } else if (type == "identifier") {
        treesitter::Node p = node.parent();
        if (!p.is_null()) {
            std::string_view pt = p.type();
            if (pt == "function_declarator") {
                token_type = legend.token_type_index("function");
                modifiers = legend.encode_modifiers({"definition"});
            } else if (pt == "call_expression") {
                token_type = legend.token_type_index("function");
            } else {
                token_type = legend.token_type_index("variable");
            }
        }
    } else if (type == "number_literal") {
        token_type = legend.token_type_index("number");
    } else if (type == "string_literal" || type == "char_literal" || type == "system_lib_string") {
        token_type = legend.token_type_index("string");
    } else if (type == "comment") {
        token_type = legend.token_type_index("comment");
    } else if (type == "if" || type == "else" || type == "for" || type == "while" ||
               type == "return" || type == "struct" || type == "enum" || type == "typedef" ||
               type == "#include" || type == "#define") {
        token_type = legend.token_type_index("keyword");
    }

    if (token_type.has_value()) {
        const uint32_t sb = node.start_byte();
        const uint32_t eb = node.end_byte();
        if (eb > sb && eb <= source.size()) {
            Point pt = converter.byte_to_point(sb);
            out.push_back(HighlightToken{
                .line = pt.line,
                .start_column = pt.column,
                .length = eb - sb,
                .token_type = *token_type,
                .token_modifiers = modifiers,
                .byte_range = node.byte_range(),
                .display_range = node.display_range(),
            });
        }
    }

    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        extract_highlights_from_node(node.child(i), source, converter, legend, out);
    }
}

} // namespace

ReferenceAdapter::ReferenceAdapter() {
    capabilities_ = {
        .functions = CapabilityStatus::supported,
        .methods = CapabilityStatus::unavailable,
        .classes = CapabilityStatus::unavailable,
        .structs = CapabilityStatus::supported,
        .interfaces = CapabilityStatus::unavailable,
        .enums = CapabilityStatus::supported,
        .records = CapabilityStatus::unavailable,
        .namespaces = CapabilityStatus::unavailable,
        .variables = CapabilityStatus::supported,
        .modules = CapabilityStatus::unavailable,
        .packages = CapabilityStatus::unavailable,
        .templates = CapabilityStatus::unavailable,
        .partial_types = CapabilityStatus::unavailable,
        .containment = CapabilityStatus::supported,
        .calls = CapabilityStatus::supported,
        .references = CapabilityStatus::supported,
        .inheritance = CapabilityStatus::unavailable,
        .implementation = CapabilityStatus::unavailable,
        .imports = CapabilityStatus::unavailable,
        .includes = CapabilityStatus::supported,
        .api_endpoints = CapabilityStatus::deferred,
        .database_tables = CapabilityStatus::deferred,
        .test_declarations = CapabilityStatus::deferred,
        .override_analysis = CapabilityStatus::deferred,
        .instantiation_analysis = CapabilityStatus::deferred,
    };
}

const LanguageCapabilities& ReferenceAdapter::capabilities() const noexcept {
    return capabilities_;
}

Result<AdapterResult> ReferenceAdapter::parse(std::string_view source,
                                              const std::filesystem::path& file_path,
                                              const std::stop_token& stop_token) {
    CAdapter c_adapter;
    return c_adapter.parse(source, file_path, stop_token);
}

Result<std::vector<HighlightToken>> ReferenceAdapter::highlight(std::string_view source,
                                                                const treesitter::Tree& tree) {
    std::vector<HighlightToken> tokens;
    CoordinateConverter converter(source);
    const auto& legend = HighlightLegend::default_legend();
    extract_highlights_from_node(tree.root_node(), source, converter, legend, tokens);
    return tokens;
}

} // namespace codelenses::adapters
