#include "codelenses/adapters/csharp_adapter.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "codelenses/treesitter/grammars.hpp"
#include "codelenses/treesitter/parser.hpp"

namespace codelenses::adapters {

namespace {

struct ASTContext {
    std::string_view source;
    const std::stop_token& stop_token;
    AdapterResult& result;
    std::vector<std::string> scope_stack;
    std::unordered_set<uint32_t> handled_identifier_byte_starts;

    [[nodiscard]] std::optional<std::string> current_scope() const {
        if (scope_stack.empty())
            return std::nullopt;
        std::string full;
        for (size_t i = 0; i < scope_stack.size(); ++i) {
            if (i > 0)
                full += ".";
            full += scope_stack[i];
        }
        return full;
    }
};

bool has_modifier(treesitter::Node node, std::string_view modifier_name, std::string_view source) {
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "modifier" && ch.text(source) == modifier_name) {
            return true;
        }
    }
    return false;
}

void walk_node(treesitter::Node node, ASTContext& ctx);

void process_using_directive(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (name_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "identifier" || ch.type() == "qualified_name") {
                name_node = ch;
                break;
            }
        }
    }

    if (!name_node.is_null()) {
        const std::string name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::import,
            .written_name = name,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = {name},
        });
    }
}

void process_namespace(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");

    std::string ns_name;
    if (!name_node.is_null()) {
        ns_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + ns_name : ns_name;

        ctx.result.symbols.push_back(SymbolFact{
            .name = ns_name,
            .qualified_name = qname,
            .kind = NodeKind::namespace_,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = "namespace " + ns_name,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = ns_name,
            .qualified_name = qname,
            .kind = NodeKind::namespace_,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });
    }

    const bool file_scoped = (node.type() == "file_scoped_namespace_declaration");

    if (!ns_name.empty()) {
        ctx.scope_stack.push_back(ns_name);
    }

    if (!body_node.is_null()) {
        walk_node(body_node, ctx);
    } else if (file_scoped) {
        // In file-scoped namespace, siblings after this node in translation_unit are part of the
        // namespace But within the node itself, walk any declarations
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch != name_node) {
                walk_node(ch, ctx);
            }
        }
    }

    if (!file_scoped && !ns_name.empty()) {
        ctx.scope_stack.pop_back();
    }
}

void process_type_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");

    const auto type_str = node.type();
    NodeKind kind = NodeKind::class_;
    if (type_str == "struct_declaration")
        kind = NodeKind::struct_;
    else if (type_str == "interface_declaration")
        kind = NodeKind::interface_;
    else if (type_str == "record_declaration" || type_str == "record_struct_declaration")
        kind = NodeKind::class_;
    else if (type_str == "enum_declaration")
        kind = NodeKind::struct_;

    const bool is_partial = has_modifier(node, "partial", ctx.source);
    std::string name;
    if (!name_node.is_null()) {
        name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + name : name;
        std::string signature = is_partial ? ("partial " + std::string(type_str) + " " + name)
                                           : (std::string(type_str) + " " + name);

        ctx.result.symbols.push_back(SymbolFact{
            .name = name,
            .qualified_name = qname,
            .kind = kind,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = signature,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = name,
            .qualified_name = qname,
            .kind = kind,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });

        // Check base_list for inheritance / implementation
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "base_list") {
                for (uint32_t j = 0; j < ch.child_count(); ++j) {
                    auto bch = ch.child(j);
                    if (bch.type() == "identifier" || bch.type() == "generic_name" ||
                        bch.type() == "qualified_name") {
                        const std::string base_name = std::string(bch.text(ctx.source));
                        ctx.handled_identifier_byte_starts.insert(bch.start_byte());

                        // By C# convention or AST, interface names often start with 'I' followed by
                        // uppercase
                        const auto last_dot = base_name.rfind('.');
                        const std::string_view unqualified =
                            (last_dot == std::string::npos)
                                ? std::string_view(base_name)
                                : std::string_view(base_name).substr(last_dot + 1);
                        const bool is_interface =
                            (unqualified.size() >= 2 && unqualified[0] == 'I' &&
                             std::isupper(static_cast<unsigned char>(unqualified[1])));
                        const auto fact_kind = is_interface ? worker::FactKind::implementation
                                                            : worker::FactKind::inheritance;

                        ctx.result.occurrences.push_back(OccurrenceFact{
                            .kind = fact_kind,
                            .written_name = base_name,
                            .range = bch.byte_range(),
                            .display_range = bch.display_range(),
                            .enclosing_scope = qname,
                            .candidate_targets = {base_name},
                        });
                    }
                }
            }
        }
    }

    if (!body_node.is_null()) {
        if (!name.empty()) {
            ctx.scope_stack.push_back(name);
        }
        walk_node(body_node, ctx);
        if (!name.empty()) {
            ctx.scope_stack.pop_back();
        }
    }
}

void process_method(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");
    if (body_node.is_null()) {
        // Check for expression-bodied method
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            if (node.child(i).type() == "arrow_expression_clause") {
                body_node = node.child(i);
                break;
            }
        }
    }

    std::string fn_name;
    if (!name_node.is_null()) {
        fn_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + fn_name : fn_name;
        const std::string signature =
            std::string(node.text(ctx.source)
                            .substr(0, std::min<size_t>(node.text(ctx.source).find('{'), 128)));

        ctx.result.symbols.push_back(SymbolFact{
            .name = fn_name,
            .qualified_name = qname,
            .kind = NodeKind::method,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = signature,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = fn_name,
            .qualified_name = qname,
            .kind = NodeKind::method,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = !body_node.is_null(),
        });
    }

    // Parameters
    treesitter::Node params_node = node.child_by_field_name("parameters");
    if (!params_node.is_null()) {
        walk_node(params_node, ctx);
    }

    if (!body_node.is_null()) {
        if (!fn_name.empty()) {
            ctx.scope_stack.push_back(fn_name);
        }
        walk_node(body_node, ctx);
        if (!fn_name.empty()) {
            ctx.scope_stack.pop_back();
        }
    }
}

void process_field_or_property(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (name_node.is_null() && node.type() == "field_declaration") {
        // Look inside variable_declaration -> variable_declarator
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "variable_declaration") {
                for (uint32_t j = 0; j < ch.child_count(); ++j) {
                    auto vch = ch.child(j);
                    if (vch.type() == "variable_declarator") {
                        name_node = vch.child_by_field_name("name");
                        if (!name_node.is_null())
                            break;
                    }
                }
            }
        }
    }

    if (!name_node.is_null()) {
        const std::string name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + name : name;

        ctx.result.symbols.push_back(SymbolFact{
            .name = name,
            .qualified_name = qname,
            .kind = NodeKind::variable,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = name,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = name,
            .qualified_name = qname,
            .kind = NodeKind::variable,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });
    }
}

void process_invocation(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node func_node = node.child_by_field_name("function");
    if (!func_node.is_null()) {
        std::string callee;
        treesitter::Node callee_name_node = func_node;

        if (func_node.type() == "identifier") {
            callee = std::string(func_node.text(ctx.source));
        } else if (func_node.type() == "member_access_expression") {
            treesitter::Node name_node = func_node.child_by_field_name("name");
            if (!name_node.is_null()) {
                callee = std::string(name_node.text(ctx.source));
                callee_name_node = name_node;
            }
        }

        if (!callee.empty()) {
            ctx.handled_identifier_byte_starts.insert(callee_name_node.start_byte());
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::call,
                .written_name = callee,
                .range = callee_name_node.byte_range(),
                .display_range = callee_name_node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {callee},
            });
        }

        if (func_node.type() == "member_access_expression") {
            treesitter::Node exp_node = func_node.child_by_field_name("expression");
            if (!exp_node.is_null()) {
                walk_node(exp_node, ctx);
            }
        }
    }

    treesitter::Node args_node = node.child_by_field_name("arguments");
    if (!args_node.is_null()) {
        walk_node(args_node, ctx);
    }
}

void walk_node(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null())
        return;
    if (ctx.stop_token.stop_requested())
        return;

    const auto type = node.type();

    if (type == "using_directive") {
        process_using_directive(node, ctx);
        return;
    }

    if (type == "namespace_declaration" || type == "file_scoped_namespace_declaration") {
        process_namespace(node, ctx);
        return;
    }

    if (type == "class_declaration" || type == "struct_declaration" ||
        type == "interface_declaration" || type == "enum_declaration" ||
        type == "record_declaration" || type == "record_struct_declaration") {
        process_type_declaration(node, ctx);
        return;
    }

    if (type == "method_declaration" || type == "constructor_declaration") {
        process_method(node, ctx);
        return;
    }

    if (type == "property_declaration" || type == "field_declaration") {
        process_field_or_property(node, ctx);
        return;
    }

    if (type == "invocation_expression") {
        process_invocation(node, ctx);
        return;
    }

    if (type == "identifier") {
        if (!ctx.handled_identifier_byte_starts.contains(node.start_byte())) {
            const std::string name = std::string(node.text(ctx.source));
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::reference,
                .written_name = name,
                .range = node.byte_range(),
                .display_range = node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {name},
            });
            ctx.handled_identifier_byte_starts.insert(node.start_byte());
        }
        return;
    }

    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        walk_node(node.child(i), ctx);
    }
}

} // namespace

CSharpAdapter::CSharpAdapter() {
    capabilities_ = {
        .functions = CapabilityStatus::unavailable,
        .methods = CapabilityStatus::supported,
        .classes = CapabilityStatus::supported,
        .structs = CapabilityStatus::supported,
        .interfaces = CapabilityStatus::supported,
        .enums = CapabilityStatus::supported,
        .records = CapabilityStatus::supported,
        .namespaces = CapabilityStatus::supported,
        .variables = CapabilityStatus::supported,
        .modules = CapabilityStatus::unavailable,
        .packages = CapabilityStatus::unavailable,
        .templates = CapabilityStatus::unavailable,
        .partial_types = CapabilityStatus::supported,
        .containment = CapabilityStatus::supported,
        .calls = CapabilityStatus::supported,
        .references = CapabilityStatus::supported,
        .inheritance = CapabilityStatus::supported,
        .implementation = CapabilityStatus::supported,
        .imports = CapabilityStatus::supported,
        .includes = CapabilityStatus::unavailable,
        .api_endpoints = CapabilityStatus::deferred,
        .database_tables = CapabilityStatus::deferred,
        .test_declarations = CapabilityStatus::deferred,
        .override_analysis = CapabilityStatus::deferred,
        .instantiation_analysis = CapabilityStatus::deferred,
    };
}

const LanguageCapabilities& CSharpAdapter::capabilities() const noexcept {
    return capabilities_;
}

Result<AdapterResult> CSharpAdapter::parse(std::string_view source,
                                           const std::filesystem::path& /*file_path*/,
                                           const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        return unexpected_result<AdapterResult>(ErrorCode::cancelled, "parsing cancelled");
    }

    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(Language::csharp);
    if (!ts_lang) {
        return unexpected_result<AdapterResult>(ErrorCode::invalid_argument,
                                                "C# grammar not available");
    }

    auto set_lang = parser.set_language(ts_lang);
    if (!set_lang) {
        return unexpected_result<AdapterResult>(set_lang.error().code, set_lang.error().message);
    }

    auto tree_res = parser.parse_string(source, nullptr, stop_token);
    if (!tree_res) {
        return unexpected_result<AdapterResult>(tree_res.error().code, tree_res.error().message);
    }

    AdapterResult result{
        .language = Language::csharp,
        .status = worker::CompletionStatus::complete,
        .symbols = {},
        .declarations = {},
        .occurrences = {},
        .diagnostics = {},
    };

    treesitter::Node root = tree_res->root_node();
    result.diagnostics = collect_syntax_errors(root, Language::csharp);

    ASTContext ctx{
        .source = source,
        .stop_token = stop_token,
        .result = result,
        .scope_stack = {},
        .handled_identifier_byte_starts = {},
    };

    walk_node(root, ctx);

    if (stop_token.stop_requested()) {
        return unexpected_result<AdapterResult>(ErrorCode::cancelled, "parsing cancelled");
    }

    if (!result.diagnostics.empty()) {
        result.status = (result.symbols.empty() && result.occurrences.empty())
                            ? worker::CompletionStatus::failed
                            : worker::CompletionStatus::degraded;
    }

    return result;
}

} // namespace codelenses::adapters
