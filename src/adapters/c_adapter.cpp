#include "codelenses/adapters/c_adapter.hpp"

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
                full += "::";
            full += scope_stack[i];
        }
        return full;
    }
};

treesitter::Node unwrap_declarator_identifier(treesitter::Node node) {
    while (!node.is_null()) {
        const auto type = node.type();
        if (type == "identifier" || type == "type_identifier") {
            return node;
        }
        if (type == "function_declarator" || type == "pointer_declarator" ||
            type == "array_declarator" || type == "parenthesized_declarator" ||
            type == "init_declarator" || type == "attributed_declarator") {
            treesitter::Node inner = node.child_by_field_name("declarator");
            if (!inner.is_null()) {
                node = inner;
                continue;
            }
        }
        // Search children if field name didn't match
        for (uint32_t i = 0; i < node.named_child_count(); ++i) {
            auto child = node.named_child(i);
            auto found = unwrap_declarator_identifier(child);
            if (!found.is_null())
                return found;
        }
        break;
    }
    return treesitter::Node{};
}

bool has_function_declarator(treesitter::Node node) {
    if (node.is_null())
        return false;
    if (node.type() == "function_declarator")
        return true;
    for (uint32_t i = 0; i < node.named_child_count(); ++i) {
        if (has_function_declarator(node.named_child(i)))
            return true;
    }
    return false;
}

bool is_extern_declaration(treesitter::Node node, std::string_view source) {
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto child = node.child(i);
        if (child.type() == "storage_class_specifier" && child.text(source) == "extern") {
            return true;
        }
    }
    return false;
}

std::string clean_include_target(std::string_view raw) {
    std::string target(raw);
    if ((target.starts_with('<') && target.ends_with('>')) ||
        (target.starts_with('"') && target.ends_with('"'))) {
        if (target.size() >= 2) {
            target = target.substr(1, target.size() - 2);
        }
    }
    return target;
}

void walk_node(treesitter::Node node, ASTContext& ctx);

void process_include(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node path_node = node.child_by_field_name("path");
    if (path_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "system_lib_string" || ch.type() == "string_literal") {
                path_node = ch;
                break;
            }
        }
    }

    if (!path_node.is_null()) {
        const std::string raw = std::string(path_node.text(ctx.source));
        const std::string target = clean_include_target(raw);
        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::include,
            .written_name = target,
            .range = path_node.byte_range(),
            .display_range = path_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = {target},
        });
    }
}

void process_function_definition(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node decl_node = node.child_by_field_name("declarator");
    treesitter::Node id_node = unwrap_declarator_identifier(decl_node);

    std::string fn_name;
    if (!id_node.is_null()) {
        fn_name = std::string(id_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(id_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "::" + fn_name : fn_name;
        const std::string signature =
            decl_node.is_null() ? fn_name : std::string(decl_node.text(ctx.source));

        ctx.result.symbols.push_back(SymbolFact{
            .name = fn_name,
            .qualified_name = qname,
            .kind = NodeKind::function,
            .range = id_node.byte_range(),
            .display_range = id_node.display_range(),
            .enclosing_scope = scope,
            .signature = signature,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = fn_name,
            .qualified_name = qname,
            .kind = NodeKind::function,
            .range = id_node.byte_range(),
            .display_range = id_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });
    }

    // Traverse children
    // Return type
    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }

    // Parameters in declarator
    if (!decl_node.is_null()) {
        walk_node(decl_node, ctx);
    }

    // Body with updated scope
    treesitter::Node body_node = node.child_by_field_name("body");
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

void process_struct_or_enum(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");

    const bool is_def = !body_node.is_null();
    std::string name;
    if (!name_node.is_null()) {
        name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        if (!is_def) {
            // Check if this is a forward declaration (e.g. struct ForwardDecl;)
            // vs a type reference in a variable or parameter declaration (e.g. struct Vec2* v)
            bool is_forward_decl = false;
            auto parent = node.parent();
            if (!parent.is_null()) {
                if (parent.type() == "translation_unit") {
                    is_forward_decl = true;
                } else if (parent.type() == "declaration") {
                    bool has_declarator = false;
                    for (uint32_t i = 0; i < parent.child_count(); ++i) {
                        const auto ct = parent.child(i).type();
                        if (ct == "init_declarator" || ct == "declarator" ||
                            ct == "pointer_declarator" || ct == "function_declarator" ||
                            ct == "array_declarator" || ct == "identifier") {
                            has_declarator = true;
                            break;
                        }
                    }
                    if (!has_declarator) {
                        is_forward_decl = true;
                    }
                }
            }

            if (is_forward_decl) {
                const auto scope = ctx.current_scope();
                const std::string qname = scope ? *scope + "::" + name : name;
                ctx.result.symbols.push_back(SymbolFact{
                    .name = name,
                    .qualified_name = qname,
                    .kind = NodeKind::struct_,
                    .range = name_node.byte_range(),
                    .display_range = name_node.display_range(),
                    .enclosing_scope = scope,
                    .signature = std::string(node.type()) + " " + name,
                });

                ctx.result.declarations.push_back(DeclarationFact{
                    .symbol_name = name,
                    .qualified_name = qname,
                    .kind = NodeKind::struct_,
                    .range = name_node.byte_range(),
                    .display_range = name_node.display_range(),
                    .enclosing_scope = scope,
                    .is_definition = false,
                });
            } else {
                // It's a type reference
                ctx.result.occurrences.push_back(OccurrenceFact{
                    .kind = worker::FactKind::reference,
                    .written_name = name,
                    .range = name_node.byte_range(),
                    .display_range = name_node.display_range(),
                    .enclosing_scope = ctx.current_scope(),
                    .candidate_targets = {name},
                });
            }
        } else {
            const auto scope = ctx.current_scope();
            const std::string qname = scope ? *scope + "::" + name : name;

            ctx.result.symbols.push_back(SymbolFact{
                .name = name,
                .qualified_name = qname,
                .kind = NodeKind::struct_,
                .range = name_node.byte_range(),
                .display_range = name_node.display_range(),
                .enclosing_scope = scope,
                .signature = std::string(node.type()) + " " + name,
            });

            ctx.result.declarations.push_back(DeclarationFact{
                .symbol_name = name,
                .qualified_name = qname,
                .kind = NodeKind::struct_,
                .range = name_node.byte_range(),
                .display_range = name_node.display_range(),
                .enclosing_scope = scope,
                .is_definition = true,
            });
        }
    }

    if (is_def) {
        if (!name.empty()) {
            ctx.scope_stack.push_back(name);
        }
        walk_node(body_node, ctx);
        if (!name.empty()) {
            ctx.scope_stack.pop_back();
        }
    }
}

void process_declaration(treesitter::Node node, ASTContext& ctx) {
    const bool is_extern = is_extern_declaration(node, ctx.source);

    // Check type specifier
    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        const auto type_name = type_node.type();
        if (type_name == "struct_specifier" || type_name == "union_specifier" ||
            type_name == "enum_specifier") {
            process_struct_or_enum(type_node, ctx);
        } else {
            walk_node(type_node, ctx);
        }
    }

    // Iterate over declarators
    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        auto child = node.child(i);
        const auto child_type = child.type();
        if (child_type == "type" || child == type_node)
            continue;

        if (child_type == "init_declarator" || child_type == "declarator" ||
            child_type == "function_declarator" || child_type == "pointer_declarator" ||
            child_type == "array_declarator" || child_type == "identifier") {
            const bool is_fn = has_function_declarator(child);
            treesitter::Node id_node = unwrap_declarator_identifier(child);

            if (!id_node.is_null()) {
                const std::string name = std::string(id_node.text(ctx.source));
                ctx.handled_identifier_byte_starts.insert(id_node.start_byte());

                const auto scope = ctx.current_scope();
                const std::string qname = scope ? *scope + "::" + name : name;

                if (is_fn) {
                    ctx.result.symbols.push_back(SymbolFact{
                        .name = name,
                        .qualified_name = qname,
                        .kind = NodeKind::function,
                        .range = id_node.byte_range(),
                        .display_range = id_node.display_range(),
                        .enclosing_scope = scope,
                        .signature = std::string(child.text(ctx.source)),
                    });

                    ctx.result.declarations.push_back(DeclarationFact{
                        .symbol_name = name,
                        .qualified_name = qname,
                        .kind = NodeKind::function,
                        .range = id_node.byte_range(),
                        .display_range = id_node.display_range(),
                        .enclosing_scope = scope,
                        .is_definition = false, // prototype
                    });
                } else {
                    const bool has_init = (child_type == "init_declarator");
                    const bool is_def = !is_extern || has_init;

                    ctx.result.symbols.push_back(SymbolFact{
                        .name = name,
                        .qualified_name = qname,
                        .kind = NodeKind::variable,
                        .range = id_node.byte_range(),
                        .display_range = id_node.display_range(),
                        .enclosing_scope = scope,
                        .signature = std::string(child.text(ctx.source)),
                    });

                    ctx.result.declarations.push_back(DeclarationFact{
                        .symbol_name = name,
                        .qualified_name = qname,
                        .kind = NodeKind::variable,
                        .range = id_node.byte_range(),
                        .display_range = id_node.display_range(),
                        .enclosing_scope = scope,
                        .is_definition = is_def,
                    });
                }
            }

            // If init_declarator, walk the initializer value
            if (child_type == "init_declarator") {
                treesitter::Node val_node = child.child_by_field_name("value");
                if (!val_node.is_null()) {
                    walk_node(val_node, ctx);
                }
            }
        }
    }
}

void process_type_definition(treesitter::Node node, ASTContext& ctx) {
    // typedef declaration
    treesitter::Node type_node = node.child_by_field_name("type");
    treesitter::Node decl_node = node.child_by_field_name("declarator");

    if (!type_node.is_null()) {
        const auto type_name = type_node.type();
        if (type_name == "struct_specifier" || type_name == "union_specifier" ||
            type_name == "enum_specifier") {
            process_struct_or_enum(type_node, ctx);
        } else {
            walk_node(type_node, ctx);
        }
    }

    treesitter::Node id_node = unwrap_declarator_identifier(decl_node);
    if (!id_node.is_null()) {
        const std::string name = std::string(id_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(id_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "::" + name : name;

        ctx.result.symbols.push_back(SymbolFact{
            .name = name,
            .qualified_name = qname,
            .kind = NodeKind::struct_,
            .range = id_node.byte_range(),
            .display_range = id_node.display_range(),
            .enclosing_scope = scope,
            .signature = std::string(node.text(ctx.source)),
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = name,
            .qualified_name = qname,
            .kind = NodeKind::struct_,
            .range = id_node.byte_range(),
            .display_range = id_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });
    }
}

void process_call_expression(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node func_node = node.child_by_field_name("function");
    if (!func_node.is_null()) {
        std::string callee;
        if (func_node.type() == "identifier") {
            callee = std::string(func_node.text(ctx.source));
            ctx.handled_identifier_byte_starts.insert(func_node.start_byte());
        } else if (func_node.type() == "field_expression") {
            treesitter::Node field = func_node.child_by_field_name("field");
            if (!field.is_null()) {
                callee = std::string(field.text(ctx.source));
                ctx.handled_identifier_byte_starts.insert(field.start_byte());
            }
        }

        if (!callee.empty()) {
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::call,
                .written_name = callee,
                .range = func_node.byte_range(),
                .display_range = func_node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {callee},
            });
        }

        // If it's a complex function expression like obj.func, walk the argument/argument object
        if (func_node.type() == "field_expression") {
            treesitter::Node arg = func_node.child_by_field_name("argument");
            if (!arg.is_null()) {
                walk_node(arg, ctx);
            }
        }
    }

    // Walk arguments
    treesitter::Node args_node = node.child_by_field_name("arguments");
    if (!args_node.is_null()) {
        walk_node(args_node, ctx);
    }
}

void process_enumerator(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (!name_node.is_null()) {
        const std::string name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "::" + name : name;

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

    treesitter::Node val_node = node.child_by_field_name("value");
    if (!val_node.is_null()) {
        walk_node(val_node, ctx);
    }
}

void walk_node(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null())
        return;
    if (ctx.stop_token.stop_requested())
        return;

    const auto type = node.type();

    if (type == "preproc_include") {
        process_include(node, ctx);
        return;
    }

    if (type == "function_definition") {
        process_function_definition(node, ctx);
        return;
    }

    if (type == "declaration") {
        process_declaration(node, ctx);
        return;
    }

    if (type == "type_definition") {
        process_type_definition(node, ctx);
        return;
    }

    if (type == "struct_specifier" || type == "union_specifier" || type == "enum_specifier") {
        process_struct_or_enum(node, ctx);
        return;
    }

    if (type == "enumerator") {
        process_enumerator(node, ctx);
        return;
    }

    if (type == "call_expression") {
        process_call_expression(node, ctx);
        return;
    }

    if (type == "type_identifier") {
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

    // Recurse over children
    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        walk_node(node.child(i), ctx);
    }
}

} // namespace

CAdapter::CAdapter() {
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

const LanguageCapabilities& CAdapter::capabilities() const noexcept {
    return capabilities_;
}

Result<AdapterResult> CAdapter::parse(std::string_view source,
                                      const std::filesystem::path& /*file_path*/,
                                      const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        return unexpected_result<AdapterResult>(ErrorCode::cancelled, "parsing cancelled");
    }

    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(Language::c);
    if (!ts_lang) {
        return unexpected_result<AdapterResult>(ErrorCode::invalid_argument,
                                                "C grammar not available");
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
        .language = Language::c,
        .status = worker::CompletionStatus::complete,
        .symbols = {},
        .declarations = {},
        .occurrences = {},
        .diagnostics = {},
    };

    treesitter::Node root = tree_res->root_node();
    result.diagnostics = collect_syntax_errors(root, Language::c);

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
