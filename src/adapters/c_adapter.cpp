#include "codelenses/adapters/c_adapter.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "codelenses/parser/coordinate_converter.hpp"
#include "codelenses/parser/highlight.hpp"
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
    std::unordered_set<std::string> macro_param_names;
    const CompileCommandContext* compile_context{nullptr};
    std::filesystem::path file_path{};
    std::unordered_set<std::string> command_line_macro_names{};

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
        if (type == "identifier" || type == "type_identifier" || type == "field_identifier") {
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

bool is_function_name_declarator(treesitter::Node id_node, treesitter::Node root_decl) {
    if (id_node.is_null() || root_decl.is_null())
        return false;

    treesitter::Node curr = id_node;
    while (!curr.is_null() && curr != root_decl) {
        treesitter::Node parent = curr.parent();
        if (parent.is_null())
            break;

        const auto ptype = parent.type();
        if (ptype == "function_declarator") {
            treesitter::Node fn_decl = parent.child_by_field_name("declarator");
            return fn_decl == curr;
        }
        if (ptype == "pointer_declarator" || ptype == "array_declarator") {
            return false;
        }

        curr = parent;
    }

    if (curr == root_decl && curr.type() == "function_declarator") {
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
        bool is_quote = (raw.starts_with('"') && raw.ends_with('"'));

        std::vector<std::string> candidates;
        std::unordered_set<std::string> seen;

        candidates.push_back(target);
        seen.insert(target);

        if (ctx.compile_context != nullptr) {
            // Quote include checks current file directory first
            if (is_quote && !ctx.file_path.empty()) {
                auto parent = ctx.file_path.parent_path();
                if (!parent.empty()) {
                    auto cand = (parent / target).lexically_normal().generic_string();
                    if (seen.insert(cand).second) {
                        candidates.push_back(cand);
                    }
                }
            }

            // Include directories from compile command
            for (const auto& dir : ctx.compile_context->include_dirs) {
                auto cand = (dir / target).lexically_normal().generic_string();
                if (seen.insert(cand).second) {
                    candidates.push_back(cand);
                }
            }
        }

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::include,
            .written_name = target,
            .range = path_node.byte_range(),
            .display_range = path_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = std::move(candidates),
        });
    }
}

void process_macro(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (name_node.is_null())
        return;

    const std::string name = std::string(name_node.text(ctx.source));
    ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

    const auto scope = ctx.current_scope();
    const std::string qname = scope ? *scope + "::" + name : name;
    std::string_view sig_view = node.text(ctx.source);
    while (!sig_view.empty() &&
           (sig_view.back() == '\n' || sig_view.back() == '\r' || sig_view.back() == ' ')) {
        sig_view.remove_suffix(1);
    }
    const std::string signature(sig_view);

    ctx.result.symbols.push_back(SymbolFact{
        .name = name,
        .qualified_name = qname,
        .kind = NodeKind::macro,
        .range = name_node.byte_range(),
        .display_range = name_node.display_range(),
        .enclosing_scope = scope,
        .signature = signature,
    });

    ctx.result.declarations.push_back(DeclarationFact{
        .symbol_name = name,
        .qualified_name = qname,
        .kind = NodeKind::macro,
        .range = name_node.byte_range(),
        .display_range = name_node.display_range(),
        .enclosing_scope = scope,
        .is_definition = true,
    });

    // Collect macro parameter names if function-like macro
    std::unordered_set<std::string> saved_params = std::move(ctx.macro_param_names);

    treesitter::Node params_node = node.child_by_field_name("parameters");
    if (!params_node.is_null()) {
        for (uint32_t i = 0; i < params_node.named_child_count(); ++i) {
            auto param = params_node.named_child(i);
            if (param.type() == "identifier") {
                ctx.handled_identifier_byte_starts.insert(param.start_byte());
                ctx.macro_param_names.insert(std::string(param.text(ctx.source)));
            }
        }
    }

    // Walk macro body value
    treesitter::Node val_node = node.child_by_field_name("value");
    if (!val_node.is_null()) {
        walk_node(val_node, ctx);
    }

    ctx.macro_param_names = std::move(saved_params);
}

void process_parameter_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }
    treesitter::Node decl_node = node.child_by_field_name("declarator");
    if (!decl_node.is_null()) {
        treesitter::Node id_node = unwrap_declarator_identifier(decl_node);
        if (!id_node.is_null()) {
            ctx.handled_identifier_byte_starts.insert(id_node.start_byte());
        }
        // Walk any nested type qualifiers / declarators
        for (uint32_t i = 0; i < decl_node.named_child_count(); ++i) {
            auto child = decl_node.named_child(i);
            if (child != id_node && child.type() != "identifier") {
                walk_node(child, ctx);
            }
        }
    }
}

void process_function_definition(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node decl_node = node.child_by_field_name("declarator");
    treesitter::Node id_node = unwrap_declarator_identifier(decl_node);

    treesitter::Node type_node = node.child_by_field_name("type");
    std::string fn_name;
    if (!id_node.is_null()) {
        fn_name = std::string(id_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(id_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "::" + fn_name : fn_name;
        std::string signature;
        if (!decl_node.is_null() && decl_node.end_byte() > node.start_byte()) {
            signature = std::string(
                ctx.source.substr(node.start_byte(), decl_node.end_byte() - node.start_byte()));
        } else {
            if (!type_node.is_null()) {
                signature = std::string(type_node.text(ctx.source)) + " ";
            }
            if (!decl_node.is_null()) {
                signature += std::string(decl_node.text(ctx.source));
            } else {
                signature += fn_name;
            }
        }

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

    // Return type
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

void process_field_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }

    treesitter::Node first_decl;
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto child = node.child(i);
        if (child == type_node || child.type() == "type")
            continue;
        treesitter::Node id_node = unwrap_declarator_identifier(child);
        if (!id_node.is_null()) {
            first_decl = child;
            break;
        }
    }

    std::string prefix;
    if (!first_decl.is_null() && first_decl.start_byte() > node.start_byte()) {
        std::string_view raw =
            ctx.source.substr(node.start_byte(), first_decl.start_byte() - node.start_byte());
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\n' ||
                                raw.back() == '\r')) {
            raw.remove_suffix(1);
        }
        while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t' || raw.front() == '\n' ||
                                raw.front() == '\r')) {
            raw.remove_prefix(1);
        }
        prefix = std::string(raw);
    } else if (!type_node.is_null()) {
        prefix = std::string(type_node.text(ctx.source));
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto child = node.child(i);
        if (child == type_node || child.type() == "type")
            continue;

        treesitter::Node id_node = unwrap_declarator_identifier(child);
        if (!id_node.is_null()) {
            const std::string name = std::string(id_node.text(ctx.source));
            ctx.handled_identifier_byte_starts.insert(id_node.start_byte());

            const auto scope = ctx.current_scope();
            const std::string qname = scope ? *scope + "::" + name : name;

            treesitter::Node decl_for_sig = child;
            if (child.type() == "init_declarator") {
                treesitter::Node d = child.child_by_field_name("declarator");
                if (!d.is_null())
                    decl_for_sig = d;
            }

            std::string sig;
            if (!prefix.empty()) {
                sig = prefix + " ";
            }
            sig += std::string(decl_for_sig.text(ctx.source));

            ctx.result.symbols.push_back(SymbolFact{
                .name = name,
                .qualified_name = qname,
                .kind = NodeKind::field,
                .range = id_node.byte_range(),
                .display_range = id_node.display_range(),
                .enclosing_scope = scope,
                .signature = sig,
            });

            ctx.result.declarations.push_back(DeclarationFact{
                .symbol_name = name,
                .qualified_name = qname,
                .kind = NodeKind::field,
                .range = id_node.byte_range(),
                .display_range = id_node.display_range(),
                .enclosing_scope = scope,
                .is_definition = true,
            });
        }
    }
}

void process_struct_or_enum(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");

    const bool is_def = !body_node.is_null();
    const bool is_enum = (node.type() == "enum_specifier");
    const NodeKind item_kind = is_enum ? NodeKind::enum_ : NodeKind::struct_;

    std::string name;
    if (!name_node.is_null()) {
        name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        if (!is_def) {
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

            std::string_view tag = "struct";
            if (node.type() == "enum_specifier") {
                tag = "enum";
            } else if (node.type() == "union_specifier") {
                tag = "union";
            }
            const std::string sig = std::string(tag) + " " + name;

            if (is_forward_decl) {
                const auto scope = ctx.current_scope();
                const std::string qname = scope ? *scope + "::" + name : name;
                ctx.result.symbols.push_back(SymbolFact{
                    .name = name,
                    .qualified_name = qname,
                    .kind = item_kind,
                    .range = name_node.byte_range(),
                    .display_range = name_node.display_range(),
                    .enclosing_scope = scope,
                    .signature = sig,
                });

                ctx.result.declarations.push_back(DeclarationFact{
                    .symbol_name = name,
                    .qualified_name = qname,
                    .kind = item_kind,
                    .range = name_node.byte_range(),
                    .display_range = name_node.display_range(),
                    .enclosing_scope = scope,
                    .is_definition = false,
                });
            } else {
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
            std::string_view tag = "struct";
            if (node.type() == "enum_specifier") {
                tag = "enum";
            } else if (node.type() == "union_specifier") {
                tag = "union";
            }
            const std::string sig = std::string(tag) + " " + name;

            ctx.result.symbols.push_back(SymbolFact{
                .name = name,
                .qualified_name = qname,
                .kind = item_kind,
                .range = name_node.byte_range(),
                .display_range = name_node.display_range(),
                .enclosing_scope = scope,
                .signature = sig,
            });

            ctx.result.declarations.push_back(DeclarationFact{
                .symbol_name = name,
                .qualified_name = qname,
                .kind = item_kind,
                .range = name_node.byte_range(),
                .display_range = name_node.display_range(),
                .enclosing_scope = scope,
                .is_definition = true,
            });
        }
    } else if (is_def) {
        // Anonymous struct/enum: if parent is type_definition, use typedef declarator as scope
        auto parent = node.parent();
        if (!parent.is_null() && parent.type() == "type_definition") {
            for (uint32_t i = 0; i < parent.child_count(); ++i) {
                auto child = parent.child(i);
                if (child.type() != "type" && child != node && child.type() != "typedef") {
                    treesitter::Node id = unwrap_declarator_identifier(child);
                    if (!id.is_null()) {
                        name = std::string(id.text(ctx.source));
                        break;
                    }
                }
            }
        }
    }

    if (is_def && !body_node.is_null()) {
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

    // Find the first declarator to extract preceding specifiers, qualifiers, and type
    treesitter::Node first_decl;
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto child = node.child(i);
        const auto child_type = child.type();
        if (child_type == "type" || child == type_node)
            continue;

        if (child_type == "init_declarator" || child_type == "declarator" ||
            child_type == "function_declarator" || child_type == "pointer_declarator" ||
            child_type == "array_declarator" || child_type == "identifier" ||
            child_type == "parenthesized_declarator" || child_type == "attributed_declarator") {
            first_decl = child;
            break;
        }
    }

    std::string prefix;
    if (!first_decl.is_null() && first_decl.start_byte() > node.start_byte()) {
        std::string_view raw =
            ctx.source.substr(node.start_byte(), first_decl.start_byte() - node.start_byte());
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\n' ||
                                raw.back() == '\r')) {
            raw.remove_suffix(1);
        }
        while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t' || raw.front() == '\n' ||
                                raw.front() == '\r')) {
            raw.remove_prefix(1);
        }
        prefix = std::string(raw);
    } else if (!type_node.is_null()) {
        prefix = std::string(type_node.text(ctx.source));
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
            child_type == "array_declarator" || child_type == "identifier" ||
            child_type == "parenthesized_declarator" || child_type == "attributed_declarator") {
            treesitter::Node id_node = unwrap_declarator_identifier(child);

            if (!id_node.is_null()) {
                const bool is_fn = is_function_name_declarator(id_node, child);
                const std::string name = std::string(id_node.text(ctx.source));
                ctx.handled_identifier_byte_starts.insert(id_node.start_byte());

                const auto scope = ctx.current_scope();
                const std::string qname = scope ? *scope + "::" + name : name;

                treesitter::Node decl_for_sig = child;
                if (child_type == "init_declarator") {
                    treesitter::Node d = child.child_by_field_name("declarator");
                    if (!d.is_null()) {
                        decl_for_sig = d;
                    }
                }

                std::string decl_sig;
                if (!prefix.empty()) {
                    decl_sig = prefix + " ";
                }
                decl_sig += std::string(decl_for_sig.text(ctx.source));

                if (is_fn) {
                    ctx.result.symbols.push_back(SymbolFact{
                        .name = name,
                        .qualified_name = qname,
                        .kind = NodeKind::function,
                        .range = id_node.byte_range(),
                        .display_range = id_node.display_range(),
                        .enclosing_scope = scope,
                        .signature = decl_sig,
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

                    // Walk function declarator parameters so parameter types are recorded
                    walk_node(child, ctx);
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
                        .signature = decl_sig,
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

    // Find all declarators in the type_definition
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto child = node.child(i);
        if (child == type_node || child.type() == "typedef" || child.type() == "type")
            continue;

        treesitter::Node id_node = unwrap_declarator_identifier(child);
        if (!id_node.is_null()) {
            const std::string name = std::string(id_node.text(ctx.source));
            ctx.handled_identifier_byte_starts.insert(id_node.start_byte());

            const auto scope = ctx.current_scope();
            const std::string qname = scope ? *scope + "::" + name : name;

            ctx.result.symbols.push_back(SymbolFact{
                .name = name,
                .qualified_name = qname,
                .kind = NodeKind::type_alias,
                .range = id_node.byte_range(),
                .display_range = id_node.display_range(),
                .enclosing_scope = scope,
                .signature = std::string(node.text(ctx.source)),
            });

            ctx.result.declarations.push_back(DeclarationFact{
                .symbol_name = name,
                .qualified_name = qname,
                .kind = NodeKind::type_alias,
                .range = id_node.byte_range(),
                .display_range = id_node.display_range(),
                .enclosing_scope = scope,
                .is_definition = true,
            });
        }
    }
}

void process_call_expression(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node func_node = node.child_by_field_name("function");
    if (!func_node.is_null()) {
        treesitter::Node inner = func_node;
        while (!inner.is_null()) {
            if (inner.type() == "parenthesized_expression") {
                inner = inner.named_child(0);
                continue;
            }
            if (inner.type() == "pointer_expression") {
                inner = inner.child_by_field_name("argument");
                if (inner.is_null())
                    inner = inner.named_child(0);
                continue;
            }
            break;
        }

        std::string callee;
        treesitter::Node target_node = inner;

        if (!inner.is_null()) {
            if (inner.type() == "identifier") {
                callee = std::string(inner.text(ctx.source));
                target_node = inner;
                ctx.handled_identifier_byte_starts.insert(inner.start_byte());
            } else if (inner.type() == "field_expression") {
                treesitter::Node field = inner.child_by_field_name("field");
                if (!field.is_null()) {
                    callee = std::string(field.text(ctx.source));
                    target_node = field;
                    ctx.handled_identifier_byte_starts.insert(field.start_byte());
                }
                treesitter::Node arg = inner.child_by_field_name("argument");
                if (!arg.is_null()) {
                    walk_node(arg, ctx);
                }
            } else {
                walk_node(inner, ctx);
            }
        }

        if (!callee.empty()) {
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::call,
                .written_name = callee,
                .range = target_node.byte_range(),
                .display_range = target_node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {callee},
            });
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
            .kind = NodeKind::enum_member,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = name,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = name,
            .qualified_name = qname,
            .kind = NodeKind::enum_member,
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

    if (type == "preproc_def" || type == "preproc_function_def") {
        process_macro(node, ctx);
        return;
    }

    if (type == "function_definition") {
        process_function_definition(node, ctx);
        return;
    }

    if (type == "parameter_declaration") {
        process_parameter_declaration(node, ctx);
        return;
    }

    if (type == "declaration") {
        process_declaration(node, ctx);
        return;
    }

    if (type == "field_declaration") {
        process_field_declaration(node, ctx);
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

    if (type == "preproc_call") {
        treesitter::Node dir = node.child_by_field_name("directive");
        if (!dir.is_null()) {
            std::string name = std::string(dir.text(ctx.source));
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::call,
                .written_name = name,
                .range = dir.byte_range(),
                .display_range = dir.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {name},
            });
            ctx.handled_identifier_byte_starts.insert(dir.start_byte());
        }
        treesitter::Node arg = node.child_by_field_name("argument");
        if (!arg.is_null()) {
            walk_node(arg, ctx);
        }
        return;
    }

    if (type == "preproc_ifdef" || type == "preproc_ifndef" || type == "preproc_elifdef") {
        treesitter::Node name_node = node.child_by_field_name("name");
        if (!name_node.is_null()) {
            const std::string name = std::string(name_node.text(ctx.source));
            if (!ctx.handled_identifier_byte_starts.contains(name_node.start_byte())) {
                ctx.result.occurrences.push_back(OccurrenceFact{
                    .kind = worker::FactKind::reference,
                    .written_name = name,
                    .range = name_node.byte_range(),
                    .display_range = name_node.display_range(),
                    .enclosing_scope = std::nullopt,
                    .candidate_targets = {name},
                });
                ctx.handled_identifier_byte_starts.insert(name_node.start_byte());
            }
        }
        const uint32_t count = node.child_count();
        for (uint32_t i = 0; i < count; ++i) {
            auto child = node.child(i);
            if (child != name_node) {
                walk_node(child, ctx);
            }
        }
        return;
    }

    if (type == "statement_identifier") {
        // Goto / case label identifier - mark handled
        ctx.handled_identifier_byte_starts.insert(node.start_byte());
        return;
    }

    if (type == "field_identifier") {
        if (!ctx.handled_identifier_byte_starts.contains(node.start_byte())) {
            const std::string name = std::string(node.text(ctx.source));
            std::optional<std::string> scope = ctx.current_scope();
            std::vector<std::string> candidates = {name};
            if (ctx.command_line_macro_names.contains(name)) {
                scope = std::nullopt;
            }
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::reference,
                .written_name = name,
                .range = node.byte_range(),
                .display_range = node.display_range(),
                .enclosing_scope = scope,
                .candidate_targets = std::move(candidates),
            });
            ctx.handled_identifier_byte_starts.insert(node.start_byte());
        }
        return;
    }

    if (type == "type_identifier") {
        if (!ctx.handled_identifier_byte_starts.contains(node.start_byte())) {
            const std::string name = std::string(node.text(ctx.source));
            std::optional<std::string> scope = ctx.current_scope();
            std::vector<std::string> candidates = {name};
            if (ctx.command_line_macro_names.contains(name)) {
                scope = std::nullopt;
            }
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::reference,
                .written_name = name,
                .range = node.byte_range(),
                .display_range = node.display_range(),
                .enclosing_scope = scope,
                .candidate_targets = std::move(candidates),
            });
            ctx.handled_identifier_byte_starts.insert(node.start_byte());
        }
        return;
    }

    if (type == "identifier") {
        if (!ctx.handled_identifier_byte_starts.contains(node.start_byte())) {
            const std::string name = std::string(node.text(ctx.source));
            // Do not record macro parameter names as global reference occurrences
            if (!ctx.macro_param_names.contains(name)) {
                std::optional<std::string> scope = ctx.current_scope();
                std::vector<std::string> candidates = {name};

                // Link occurrences of command-line defined macros at translation-unit scope
                if (ctx.command_line_macro_names.contains(name)) {
                    scope = std::nullopt;
                }

                ctx.result.occurrences.push_back(OccurrenceFact{
                    .kind = worker::FactKind::reference,
                    .written_name = name,
                    .range = node.byte_range(),
                    .display_range = node.display_range(),
                    .enclosing_scope = scope,
                    .candidate_targets = std::move(candidates),
                });
            }
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

void CAdapter::set_compile_command_context(CompileCommandContext context) {
    compile_context_ = std::move(context);
}

void CAdapter::clear_compile_command_context() noexcept {
    compile_context_.reset();
}

const std::optional<CompileCommandContext>& CAdapter::compile_command_context() const noexcept {
    return compile_context_;
}

Result<AdapterResult> CAdapter::parse(std::string_view source,
                                      const std::filesystem::path& file_path,
                                      const std::stop_token& stop_token) {
    if (compile_context_.has_value()) {
        return parse(source, file_path, *compile_context_, stop_token);
    }
    return parse(source, file_path, CompileCommandContext{}, stop_token);
}

Result<AdapterResult> CAdapter::parse(std::string_view source,
                                      const std::filesystem::path& file_path,
                                      const CompileCommandContext& context,
                                      const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        return unexpected_result<AdapterResult>(ErrorCode::cancelled, "parsing cancelled");
    }

    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(Language::c);
    if (ts_lang == nullptr) {
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
        .compile_command = std::nullopt,
    };

    const bool has_context = !context.arguments.empty() || !context.defines.empty() ||
                             !context.include_dirs.empty() ||
                             context.language_standard.has_value() || !context.directory.empty();

    if (has_context) {
        result.compile_command = context;
    }

    treesitter::Node root = tree_res->root_node();
    result.diagnostics = collect_syntax_errors(root, Language::c);

    // Validate language standard if specified
    if (context.language_standard.has_value() && !context.language_standard->empty()) {
        std::string std_str = *context.language_standard;
        if (std_str.starts_with("-std=")) {
            std_str = std_str.substr(5);
        } else if (std_str.starts_with("--std=")) {
            std_str = std_str.substr(6);
        }

        std::string lower_std = std_str;
        std::transform(lower_std.begin(), lower_std.end(), lower_std.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        static const std::unordered_set<std::string> valid_c_standards = {
            "c89",          "c90",   "iso9899:1990", "c99",          "c9x",   "iso9899:1999",
            "c11",          "c1x",   "iso9899:2011", "c17",          "c18",   "iso9899:2017",
            "iso9899:2018", "c23",   "c2x",          "iso9899:2024", "gnu89", "gnu90",
            "gnu99",        "gnu9x", "gnu11",        "gnu1x",        "gnu17", "gnu18",
            "gnu23",        "gnu2x"};

        if (!valid_c_standards.contains(lower_std)) {
            result.diagnostics.push_back(ParseDiagnostic{
                .severity = DiagnosticSeverity::warning,
                .language = Language::c,
                .code = "unsupported_standard",
                .message = "Unrecognized or non-C language standard specified: " +
                           *context.language_standard,
                .byte_range = ByteRange{0, 0},
                .display_range = DisplayRange{1, 1, 1, 1},
            });
        }
    }

    // Process active macro defines (-D and -U)
    struct MacroDef {
        std::string name;
        std::string value;
    };
    std::vector<MacroDef> active_macros;
    for (const auto& def_arg : context.defines) {
        if (def_arg.starts_with("-U")) {
            std::string undef_name = def_arg.substr(2);
            std::erase_if(active_macros, [&](const MacroDef& m) { return m.name == undef_name; });
        } else {
            auto eq = def_arg.find('=');
            std::string name = (eq != std::string::npos) ? def_arg.substr(0, eq) : def_arg;
            std::string val = (eq != std::string::npos) ? def_arg.substr(eq + 1) : "1";
            std::erase_if(active_macros, [&](const MacroDef& m) { return m.name == name; });
            active_macros.push_back(MacroDef{.name = std::move(name), .value = std::move(val)});
        }
    }

    std::unordered_set<std::string> cmd_macro_names;
    for (const auto& m : active_macros) {
        std::string sig = "#define " + m.name + (m.value.empty() ? "" : " " + m.value);
        result.symbols.push_back(SymbolFact{
            .name = m.name,
            .qualified_name = m.name,
            .kind = NodeKind::macro,
            .range = ByteRange{0, 0},
            .display_range = DisplayRange{1, 1, 1, 1},
            .enclosing_scope = std::nullopt,
            .signature = sig,
        });

        result.declarations.push_back(DeclarationFact{
            .symbol_name = m.name,
            .qualified_name = m.name,
            .kind = NodeKind::macro,
            .range = ByteRange{0, 0},
            .display_range = DisplayRange{1, 1, 1, 1},
            .enclosing_scope = std::nullopt,
            .is_definition = true,
        });

        cmd_macro_names.insert(m.name);
    }

    ASTContext ctx{
        .source = source,
        .stop_token = stop_token,
        .result = result,
        .scope_stack = {},
        .handled_identifier_byte_starts = {},
        .macro_param_names = {},
        .compile_context = has_context ? &context : nullptr,
        .file_path = file_path,
        .command_line_macro_names = std::move(cmd_macro_names),
    };

    walk_node(root, ctx);

    if (stop_token.stop_requested()) {
        return unexpected_result<AdapterResult>(ErrorCode::cancelled, "parsing cancelled");
    }

    if (!result.diagnostics.empty()) {
        bool has_err = false;
        for (const auto& d : result.diagnostics) {
            if (d.severity == DiagnosticSeverity::error) {
                has_err = true;
                break;
            }
        }
        if (has_err) {
            result.status = (result.symbols.empty() && result.occurrences.empty())
                                ? worker::CompletionStatus::failed
                                : worker::CompletionStatus::degraded;
        }
    }

    return result;
}

std::string_view CAdapter::highlighting_query() noexcept {
    static constexpr std::string_view kHighlightQuery = R"(
;; Generic identifiers as variables (low precedence at top)
(identifier) @variable

;; Comments
(comment) @comment

;; Keywords
"break" @keyword
"case" @keyword
"const" @keyword
"continue" @keyword
"default" @keyword
"do" @keyword
"else" @keyword
"enum" @keyword
"extern" @keyword
"for" @keyword
"goto" @keyword
"if" @keyword
"inline" @keyword
"return" @keyword
"sizeof" @keyword
"static" @keyword
"struct" @keyword
"switch" @keyword
"typedef" @keyword
"union" @keyword
"volatile" @keyword
"while" @keyword

;; Preprocessor
"#define" @keyword
"#elif" @keyword
"#else" @keyword
"#endif" @keyword
"#if" @keyword
"#ifdef" @keyword
"#ifndef" @keyword
"#include" @keyword
(preproc_directive) @keyword

;; Operators
"--" @operator
"-" @operator
"-=" @operator
"->" @operator
"=" @operator
"!=" @operator
"*" @operator
"*=" @operator
"/" @operator
"/=" @operator
"%" @operator
"%=" @operator
"&" @operator
"&&" @operator
"&=" @operator
"+" @operator
"++" @operator
"+=" @operator
"<" @operator
"<=" @operator
"<<" @operator
"<<=" @operator
"==" @operator
">" @operator
">=" @operator
">>" @operator
">>=" @operator
"||" @operator
"!" @operator
"~" @operator
"^" @operator
"^=" @operator
"|" @operator
"|=" @operator
"?" @operator
":" @operator
"." @operator

;; Literals
(string_literal) @string
(system_lib_string) @string
(char_literal) @string
(number_literal) @number
(null) @keyword

;; Types
(primitive_type) @type
(type_identifier) @type

;; Fields / Properties
(field_identifier) @property

;; Labels
(statement_identifier) @label

;; Macros
(preproc_def
  name: (identifier) @macro)

(preproc_function_def
  name: (identifier) @macro)

;; Functions
(function_declarator
  declarator: (identifier) @function)

(function_definition
  declarator: (function_declarator
    declarator: (identifier) @function.definition))

(call_expression
  function: (identifier) @function)

(call_expression
  function: (field_expression
    field: (field_identifier) @function))
)";
    return kHighlightQuery;
}

Result<std::vector<HighlightToken>> CAdapter::highlight(std::string_view source,
                                                        const treesitter::Tree& tree) {
    const auto* ts_lang = treesitter::grammar_for_language(Language::c);
    if (ts_lang == nullptr) {
        return unexpected_result<std::vector<HighlightToken>>(ErrorCode::invalid_argument,
                                                              "C grammar not available");
    }

    static const auto s_query = []() -> std::optional<treesitter::Query> {
        const auto* lang = treesitter::grammar_for_language(Language::c);
        if (lang == nullptr)
            return std::nullopt;
        auto res = treesitter::Query::create(lang, highlighting_query());
        if (!res)
            return std::nullopt;
        return std::move(*res);
    }();

    if (!s_query.has_value()) {
        return unexpected_result<std::vector<HighlightToken>>(ErrorCode::failed,
                                                              "Failed to create highlight query");
    }

    const auto& query = *s_query;
    treesitter::QueryCursor cursor;
    cursor.exec(query, tree.root_node());

    CoordinateConverter converter(source);
    const auto& legend = HighlightLegend::default_legend();

    // Map by ByteRange so later, more specific captures overwrite earlier generic ones
    struct RangeKey {
        uint32_t start = 0;
        uint32_t end = 0;
        auto operator<=>(const RangeKey&) const = default;
    };
    std::map<RangeKey, HighlightToken> token_map;

    TSQueryMatch match;
    uint32_t capture_index = 0;
    while (cursor.next_capture(match, capture_index)) {
        const auto& cap = match.captures[capture_index];
        treesitter::Node node{cap.node};
        const uint32_t sb = node.start_byte();
        const uint32_t eb = node.end_byte();
        if (eb <= sb || eb > source.size()) {
            continue;
        }

        std::string_view cap_name = query.capture_name(cap.index);
        std::string_view base_type = cap_name;
        uint32_t modifiers = 0;
        if (auto dot = cap_name.find('.'); dot != std::string_view::npos) {
            base_type = cap_name.substr(0, dot);
            auto mod_name = cap_name.substr(dot + 1);
            if (auto bit = legend.token_modifier_bit(mod_name)) {
                modifiers |= *bit;
            }
        }

        auto token_type = legend.token_type_index(base_type);
        if (!token_type.has_value()) {
            continue;
        }

        Point pt = converter.byte_to_point(sb);
        RangeKey key{.start = sb, .end = eb};
        auto it = token_map.find(key);
        if (it != token_map.end()) {
            it->second.token_type = *token_type;
            it->second.token_modifiers |= modifiers;
        } else {
            token_map[key] = HighlightToken{
                .line = pt.line,
                .start_column = pt.column,
                .length = eb - sb,
                .token_type = *token_type,
                .token_modifiers = modifiers,
                .byte_range = node.byte_range(),
                .display_range = node.display_range(),
            };
        }
    }

    std::vector<HighlightToken> tokens;
    tokens.reserve(token_map.size());
    for (const auto& [key, tok] : token_map) {
        tokens.push_back(tok);
    }

    std::ranges::sort(tokens, [](const HighlightToken& a, const HighlightToken& b) {
        if (a.line != b.line) {
            return a.line < b.line;
        }
        return a.start_column < b.start_column;
    });

    return tokens;
}

Result<std::vector<HighlightToken>> CAdapter::highlight(std::string_view source) {
    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(Language::c);
    if (ts_lang == nullptr) {
        return unexpected_result<std::vector<HighlightToken>>(ErrorCode::invalid_argument,
                                                              "C grammar not available");
    }

    auto set_lang = parser.set_language(ts_lang);
    if (!set_lang) {
        return unexpected_result<std::vector<HighlightToken>>(set_lang.error().code,
                                                              set_lang.error().message);
    }

    auto tree_res = parser.parse_string(source);
    if (!tree_res) {
        return unexpected_result<std::vector<HighlightToken>>(tree_res.error().code,
                                                              tree_res.error().message);
    }

    return highlight(source, *tree_res);
}

} // namespace codelenses::adapters
