#include "codelenses/adapters/ts_adapter.hpp"

#include <algorithm>
#include <cctype>
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

enum class ScopeKind {
    module,
    class_,
    function,
};

struct ScopeEntry {
    std::string name;
    ScopeKind kind{ScopeKind::module};
};

struct ASTContext {
    std::string_view source;
    const std::stop_token& stop_token;
    AdapterResult& result;
    std::vector<ScopeEntry> scope_stack;
    std::unordered_set<uint32_t> handled_identifier_byte_starts;

    [[nodiscard]] std::optional<std::string> current_scope() const {
        if (scope_stack.empty())
            return std::nullopt;
        std::string full;
        for (size_t i = 0; i < scope_stack.size(); ++i) {
            if (i > 0)
                full += ".";
            full += scope_stack[i].name;
        }
        return full;
    }

    [[nodiscard]] bool is_module_or_class_scope() const {
        if (scope_stack.empty())
            return true;
        return scope_stack.back().kind == ScopeKind::module ||
               scope_stack.back().kind == ScopeKind::class_;
    }

    void push_scope(std::string name, ScopeKind kind) {
        scope_stack.push_back(ScopeEntry{std::move(name), kind});
    }

    void pop_scope() {
        if (!scope_stack.empty()) {
            scope_stack.pop_back();
        }
    }
};

std::string clean_quotes(std::string_view text) {
    if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') ||
                             (text.front() == '\'' && text.back() == '\'') ||
                             (text.front() == '`' && text.back() == '`'))) {
        return std::string(text.substr(1, text.size() - 2));
    }
    return std::string(text);
}

void mark_all_identifiers_handled(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null())
        return;
    const auto type = node.type();
    if (type == "identifier" || type == "property_identifier" || type == "type_identifier" ||
        type == "shorthand_property_identifier") {
        ctx.handled_identifier_byte_starts.insert(node.start_byte());
    }
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        mark_all_identifiers_handled(node.child(i), ctx);
    }
}

void walk_node(treesitter::Node node, ASTContext& ctx);

void process_function(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");

    std::string fn_name;
    if (!name_node.is_null()) {
        fn_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + fn_name : fn_name;
        const size_t cut_pos = !body_node.is_null() ? (body_node.start_byte() - node.start_byte())
                                                    : node.text(ctx.source).find('{');
        const std::string signature =
            std::string(node.text(ctx.source).substr(0, std::min<size_t>(cut_pos, 128)));

        ctx.result.symbols.push_back(SymbolFact{
            .name = fn_name,
            .qualified_name = qname,
            .kind = NodeKind::function,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = signature,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = fn_name,
            .qualified_name = qname,
            .kind = NodeKind::function,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = !body_node.is_null(),
        });
    }

    treesitter::Node type_params = node.child_by_field_name("type_parameters");
    if (!type_params.is_null()) {
        walk_node(type_params, ctx);
    }

    treesitter::Node params_node = node.child_by_field_name("parameters");
    if (!params_node.is_null()) {
        walk_node(params_node, ctx);
    }

    treesitter::Node return_type = node.child_by_field_name("return_type");
    if (!return_type.is_null()) {
        walk_node(return_type, ctx);
    }

    if (!body_node.is_null()) {
        if (!fn_name.empty()) {
            ctx.push_scope(fn_name, ScopeKind::function);
        }
        walk_node(body_node, ctx);
        if (!fn_name.empty()) {
            ctx.pop_scope();
        }
    }
}

void process_class_heritage(treesitter::Node heritage_node, const std::string& qname,
                            ASTContext& ctx) {
    bool has_clauses = false;
    for (uint32_t i = 0; i < heritage_node.child_count(); ++i) {
        auto clause = heritage_node.child(i);
        if (clause.type() == "extends_clause") {
            has_clauses = true;
            for (uint32_t j = 0; j < clause.child_count(); ++j) {
                auto bch = clause.child(j);
                if (bch.is_named() && bch.type() != "type_arguments" && bch.type() != "comment") {
                    treesitter::Node tnode = bch;
                    if (bch.type() == "generic_type") {
                        auto n = bch.child_by_field_name("name");
                        if (!n.is_null())
                            tnode = n;
                    }
                    const std::string base_name = std::string(tnode.text(ctx.source));
                    ctx.handled_identifier_byte_starts.insert(tnode.start_byte());
                    mark_all_identifiers_handled(bch, ctx);

                    ctx.result.occurrences.push_back(OccurrenceFact{
                        .kind = worker::FactKind::inheritance,
                        .written_name = base_name,
                        .range = tnode.byte_range(),
                        .display_range = tnode.display_range(),
                        .enclosing_scope = qname,
                        .candidate_targets = {base_name},
                    });
                }
            }
        } else if (clause.type() == "implements_clause") {
            has_clauses = true;
            for (uint32_t j = 0; j < clause.child_count(); ++j) {
                auto bch = clause.child(j);
                if (bch.is_named() && bch.type() != "comment") {
                    treesitter::Node tnode = bch;
                    if (bch.type() == "generic_type") {
                        auto n = bch.child_by_field_name("name");
                        if (!n.is_null())
                            tnode = n;
                    }
                    const std::string iface_name = std::string(tnode.text(ctx.source));
                    ctx.handled_identifier_byte_starts.insert(tnode.start_byte());
                    mark_all_identifiers_handled(bch, ctx);

                    ctx.result.occurrences.push_back(OccurrenceFact{
                        .kind = worker::FactKind::implementation,
                        .written_name = iface_name,
                        .range = tnode.byte_range(),
                        .display_range = tnode.display_range(),
                        .enclosing_scope = qname,
                        .candidate_targets = {iface_name},
                    });
                }
            }
        }
    }

    if (!has_clauses) {
        for (uint32_t i = 0; i < heritage_node.child_count(); ++i) {
            auto bch = heritage_node.child(i);
            if (bch.is_named() && bch.type() != "comment") {
                const std::string base_name = std::string(bch.text(ctx.source));
                ctx.handled_identifier_byte_starts.insert(bch.start_byte());
                mark_all_identifiers_handled(bch, ctx);

                ctx.result.occurrences.push_back(OccurrenceFact{
                    .kind = worker::FactKind::inheritance,
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

void process_class(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");
    treesitter::Node type_params = node.child_by_field_name("type_parameters");

    if (!type_params.is_null()) {
        walk_node(type_params, ctx);
    }

    std::string class_name;
    std::string qname;
    if (!name_node.is_null()) {
        class_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        qname = scope ? *scope + "." + class_name : class_name;
        const std::string signature =
            std::string(node.text(ctx.source)
                            .substr(0, std::min<size_t>(node.text(ctx.source).find('{'), 128)));

        ctx.result.symbols.push_back(SymbolFact{
            .name = class_name,
            .qualified_name = qname,
            .kind = NodeKind::class_,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = signature,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = class_name,
            .qualified_name = qname,
            .kind = NodeKind::class_,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "class_heritage") {
            process_class_heritage(ch, qname, ctx);
        }
    }

    if (!body_node.is_null()) {
        if (!class_name.empty()) {
            ctx.push_scope(class_name, ScopeKind::class_);
        }
        walk_node(body_node, ctx);
        if (!class_name.empty()) {
            ctx.pop_scope();
        }
    }
}

void process_interface(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");
    treesitter::Node type_params = node.child_by_field_name("type_parameters");

    if (!type_params.is_null()) {
        walk_node(type_params, ctx);
    }

    std::string iface_name;
    std::string qname;
    if (!name_node.is_null()) {
        iface_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        qname = scope ? *scope + "." + iface_name : iface_name;
        const std::string signature =
            std::string(node.text(ctx.source)
                            .substr(0, std::min<size_t>(node.text(ctx.source).find('{'), 128)));

        ctx.result.symbols.push_back(SymbolFact{
            .name = iface_name,
            .qualified_name = qname,
            .kind = NodeKind::interface_,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = signature,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = iface_name,
            .qualified_name = qname,
            .kind = NodeKind::interface_,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "extends_type_clause" || ch.type() == "extends_clause" ||
            ch.type() == "heritage_clause") {
            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto bch = ch.child(j);
                if (bch.is_named() && bch.type() != "comment") {
                    treesitter::Node tnode = bch;
                    if (bch.type() == "generic_type") {
                        auto n = bch.child_by_field_name("name");
                        if (!n.is_null())
                            tnode = n;
                    }
                    const std::string base_name = std::string(tnode.text(ctx.source));
                    ctx.handled_identifier_byte_starts.insert(tnode.start_byte());
                    mark_all_identifiers_handled(bch, ctx);

                    ctx.result.occurrences.push_back(OccurrenceFact{
                        .kind = worker::FactKind::inheritance,
                        .written_name = base_name,
                        .range = tnode.byte_range(),
                        .display_range = tnode.display_range(),
                        .enclosing_scope = qname,
                        .candidate_targets = {base_name},
                    });
                }
            }
        }
    }

    if (!body_node.is_null()) {
        if (!iface_name.empty()) {
            ctx.push_scope(iface_name, ScopeKind::class_);
        }
        walk_node(body_node, ctx);
        if (!iface_name.empty()) {
            ctx.pop_scope();
        }
    }
}

void process_type_alias(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node val_node = node.child_by_field_name("value");
    treesitter::Node type_params = node.child_by_field_name("type_parameters");

    if (!type_params.is_null()) {
        walk_node(type_params, ctx);
    }

    if (!name_node.is_null()) {
        std::string type_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + type_name : type_name;
        const std::string signature =
            std::string(node.text(ctx.source)
                            .substr(0, std::min<size_t>(node.text(ctx.source).find(';'), 128)));

        ctx.result.symbols.push_back(SymbolFact{
            .name = type_name,
            .qualified_name = qname,
            .kind = NodeKind::type_alias,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = signature.empty() ? ("type " + type_name) : signature,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = type_name,
            .qualified_name = qname,
            .kind = NodeKind::type_alias,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });
    }

    if (!val_node.is_null()) {
        walk_node(val_node, ctx);
    }
}

void process_property_signature(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node type_node = node.child_by_field_name("type");

    if (!name_node.is_null()) {
        std::string field_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + field_name : field_name;

        ctx.result.symbols.push_back(SymbolFact{
            .name = field_name,
            .qualified_name = qname,
            .kind = NodeKind::field,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = field_name,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = field_name,
            .qualified_name = qname,
            .kind = NodeKind::field,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });
    }

    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }
}

void process_method_signature(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node params_node = node.child_by_field_name("parameters");
    treesitter::Node return_type = node.child_by_field_name("return_type");
    treesitter::Node type_params = node.child_by_field_name("type_parameters");

    if (!type_params.is_null()) {
        walk_node(type_params, ctx);
    }

    if (!name_node.is_null()) {
        std::string method_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + method_name : method_name;
        const std::string signature =
            std::string(node.text(ctx.source)
                            .substr(0, std::min<size_t>(node.text(ctx.source).find(';'), 128)));

        ctx.result.symbols.push_back(SymbolFact{
            .name = method_name,
            .qualified_name = qname,
            .kind = NodeKind::method,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = signature.empty() ? method_name : signature,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = method_name,
            .qualified_name = qname,
            .kind = NodeKind::method,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = false,
        });
    }

    if (!params_node.is_null()) {
        walk_node(params_node, ctx);
    }
    if (!return_type.is_null()) {
        walk_node(return_type, ctx);
    }
}

void process_enum(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");

    std::string enum_name;
    if (!name_node.is_null()) {
        enum_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + enum_name : enum_name;
        const std::string signature = "enum " + enum_name;

        ctx.result.symbols.push_back(SymbolFact{
            .name = enum_name,
            .qualified_name = qname,
            .kind = NodeKind::enum_,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = signature,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = enum_name,
            .qualified_name = qname,
            .kind = NodeKind::enum_,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });
    }

    if (!body_node.is_null()) {
        if (!enum_name.empty()) {
            ctx.push_scope(enum_name, ScopeKind::class_);
        }
        for (uint32_t i = 0; i < body_node.child_count(); ++i) {
            auto ch = body_node.child(i);
            if (ch.type() == "enum_assignment") {
                treesitter::Node mem_name = ch.child_by_field_name("name");
                if (!mem_name.is_null()) {
                    std::string mname = std::string(mem_name.text(ctx.source));
                    ctx.handled_identifier_byte_starts.insert(mem_name.start_byte());
                    const auto scope = ctx.current_scope();
                    const std::string mqname = scope ? *scope + "." + mname : mname;

                    ctx.result.symbols.push_back(SymbolFact{
                        .name = mname,
                        .qualified_name = mqname,
                        .kind = NodeKind::enum_member,
                        .range = mem_name.byte_range(),
                        .display_range = mem_name.display_range(),
                        .enclosing_scope = scope,
                        .signature = mname,
                    });
                    ctx.result.declarations.push_back(DeclarationFact{
                        .symbol_name = mname,
                        .qualified_name = mqname,
                        .kind = NodeKind::enum_member,
                        .range = mem_name.byte_range(),
                        .display_range = mem_name.display_range(),
                        .enclosing_scope = scope,
                        .is_definition = true,
                    });
                }
                treesitter::Node val = ch.child_by_field_name("value");
                if (!val.is_null()) {
                    walk_node(val, ctx);
                }
            } else if (ch.type() == "property_identifier") {
                std::string mname = std::string(ch.text(ctx.source));
                ctx.handled_identifier_byte_starts.insert(ch.start_byte());
                const auto scope = ctx.current_scope();
                const std::string mqname = scope ? *scope + "." + mname : mname;

                ctx.result.symbols.push_back(SymbolFact{
                    .name = mname,
                    .qualified_name = mqname,
                    .kind = NodeKind::enum_member,
                    .range = ch.byte_range(),
                    .display_range = ch.display_range(),
                    .enclosing_scope = scope,
                    .signature = mname,
                });
                ctx.result.declarations.push_back(DeclarationFact{
                    .symbol_name = mname,
                    .qualified_name = mqname,
                    .kind = NodeKind::enum_member,
                    .range = ch.byte_range(),
                    .display_range = ch.display_range(),
                    .enclosing_scope = scope,
                    .is_definition = true,
                });
            } else {
                walk_node(ch, ctx);
            }
        }
        if (!enum_name.empty()) {
            ctx.pop_scope();
        }
    }
}

void process_module(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");

    std::string ns_name;
    if (!name_node.is_null()) {
        ns_name = std::string(name_node.text(ctx.source));
        mark_all_identifiers_handled(name_node, ctx);

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

    if (!ns_name.empty()) {
        ctx.push_scope(ns_name, ScopeKind::module);
    }

    if (!body_node.is_null()) {
        walk_node(body_node, ctx);
    } else {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch != name_node) {
                walk_node(ch, ctx);
            }
        }
    }

    if (!ns_name.empty()) {
        ctx.pop_scope();
    }
}

void process_method(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");
    treesitter::Node type_params = node.child_by_field_name("type_parameters");

    if (!type_params.is_null()) {
        walk_node(type_params, ctx);
    }

    std::string fn_name;
    if (!name_node.is_null()) {
        fn_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + fn_name : fn_name;
        const size_t cut_pos = !body_node.is_null() ? (body_node.start_byte() - node.start_byte())
                                                    : node.text(ctx.source).find('{');
        const std::string signature =
            std::string(node.text(ctx.source).substr(0, std::min<size_t>(cut_pos, 128)));

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

    treesitter::Node params_node = node.child_by_field_name("parameters");
    if (!params_node.is_null()) {
        walk_node(params_node, ctx);
    }

    treesitter::Node return_type = node.child_by_field_name("return_type");
    if (!return_type.is_null()) {
        walk_node(return_type, ctx);
    }

    if (!body_node.is_null()) {
        if (!fn_name.empty()) {
            ctx.push_scope(fn_name, ScopeKind::function);
        }
        walk_node(body_node, ctx);
        if (!fn_name.empty()) {
            ctx.pop_scope();
        }
    }
}

void process_variable_declarator(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node val_node = node.child_by_field_name("value");
    treesitter::Node type_node = node.child_by_field_name("type");

    if (!name_node.is_null()) {
        if (name_node.type() == "identifier") {
            std::string var_name = std::string(name_node.text(ctx.source));
            ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

            const bool is_func = !val_node.is_null() && (val_node.type() == "arrow_function" ||
                                                         val_node.type() == "function_expression");

            if (is_func) {
                const auto scope = ctx.current_scope();
                const std::string qname = scope ? *scope + "." + var_name : var_name;
                const std::string signature = std::string(
                    node.text(ctx.source)
                        .substr(0, std::min<size_t>(node.text(ctx.source).find('{'), 128)));

                ctx.result.symbols.push_back(SymbolFact{
                    .name = var_name,
                    .qualified_name = qname,
                    .kind = NodeKind::function,
                    .range = name_node.byte_range(),
                    .display_range = name_node.display_range(),
                    .enclosing_scope = scope,
                    .signature = signature,
                });

                ctx.result.declarations.push_back(DeclarationFact{
                    .symbol_name = var_name,
                    .qualified_name = qname,
                    .kind = NodeKind::function,
                    .range = name_node.byte_range(),
                    .display_range = name_node.display_range(),
                    .enclosing_scope = scope,
                    .is_definition = true,
                });

                if (!type_node.is_null()) {
                    walk_node(type_node, ctx);
                }

                ctx.push_scope(var_name, ScopeKind::function);
                walk_node(val_node, ctx);
                ctx.pop_scope();
                return;
            } else if (ctx.is_module_or_class_scope()) {
                const auto scope = ctx.current_scope();
                const std::string qname = scope ? *scope + "." + var_name : var_name;

                ctx.result.symbols.push_back(SymbolFact{
                    .name = var_name,
                    .qualified_name = qname,
                    .kind = NodeKind::variable,
                    .range = name_node.byte_range(),
                    .display_range = name_node.display_range(),
                    .enclosing_scope = scope,
                    .signature = var_name,
                });

                ctx.result.declarations.push_back(DeclarationFact{
                    .symbol_name = var_name,
                    .qualified_name = qname,
                    .kind = NodeKind::variable,
                    .range = name_node.byte_range(),
                    .display_range = name_node.display_range(),
                    .enclosing_scope = scope,
                    .is_definition = true,
                });
            }
        } else if (name_node.type() == "object_pattern" || name_node.type() == "array_pattern") {
            auto collect_pattern_ids = [&](auto& self, treesitter::Node pnode) -> void {
                if (pnode.type() == "identifier" ||
                    pnode.type() == "shorthand_property_identifier_pattern") {
                    std::string var_name = std::string(pnode.text(ctx.source));
                    ctx.handled_identifier_byte_starts.insert(pnode.start_byte());
                    if (ctx.is_module_or_class_scope()) {
                        const auto scope = ctx.current_scope();
                        const std::string qname = scope ? *scope + "." + var_name : var_name;

                        ctx.result.symbols.push_back(SymbolFact{
                            .name = var_name,
                            .qualified_name = qname,
                            .kind = NodeKind::variable,
                            .range = pnode.byte_range(),
                            .display_range = pnode.display_range(),
                            .enclosing_scope = scope,
                            .signature = var_name,
                        });

                        ctx.result.declarations.push_back(DeclarationFact{
                            .symbol_name = var_name,
                            .qualified_name = qname,
                            .kind = NodeKind::variable,
                            .range = pnode.byte_range(),
                            .display_range = pnode.display_range(),
                            .enclosing_scope = scope,
                            .is_definition = true,
                        });
                    }
                } else {
                    for (uint32_t i = 0; i < pnode.child_count(); ++i) {
                        self(self, pnode.child(i));
                    }
                }
            };
            collect_pattern_ids(collect_pattern_ids, name_node);
        }
    }

    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }
    if (!val_node.is_null()) {
        walk_node(val_node, ctx);
    }
}

void process_field_definition(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (name_node.is_null()) {
        name_node = node.child_by_field_name("property");
    }
    treesitter::Node val_node = node.child_by_field_name("value");

    if (!name_node.is_null()) {
        std::string field_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const bool is_func = !val_node.is_null() && (val_node.type() == "arrow_function" ||
                                                     val_node.type() == "function_expression");

        if (is_func) {
            const auto scope = ctx.current_scope();
            const std::string qname = scope ? *scope + "." + field_name : field_name;
            const std::string signature =
                std::string(node.text(ctx.source)
                                .substr(0, std::min<size_t>(node.text(ctx.source).find('{'), 128)));

            ctx.result.symbols.push_back(SymbolFact{
                .name = field_name,
                .qualified_name = qname,
                .kind = NodeKind::function,
                .range = name_node.byte_range(),
                .display_range = name_node.display_range(),
                .enclosing_scope = scope,
                .signature = signature,
            });

            ctx.result.declarations.push_back(DeclarationFact{
                .symbol_name = field_name,
                .qualified_name = qname,
                .kind = NodeKind::function,
                .range = name_node.byte_range(),
                .display_range = name_node.display_range(),
                .enclosing_scope = scope,
                .is_definition = true,
            });

            ctx.push_scope(field_name, ScopeKind::function);
            walk_node(val_node, ctx);
            ctx.pop_scope();
            return;
        } else {
            const auto scope = ctx.current_scope();
            const std::string qname = scope ? *scope + "." + field_name : field_name;

            ctx.result.symbols.push_back(SymbolFact{
                .name = field_name,
                .qualified_name = qname,
                .kind = NodeKind::variable,
                .range = name_node.byte_range(),
                .display_range = name_node.display_range(),
                .enclosing_scope = scope,
                .signature = field_name,
            });

            ctx.result.declarations.push_back(DeclarationFact{
                .symbol_name = field_name,
                .qualified_name = qname,
                .kind = NodeKind::variable,
                .range = name_node.byte_range(),
                .display_range = name_node.display_range(),
                .enclosing_scope = scope,
                .is_definition = true,
            });
        }
    }

    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }
    if (!val_node.is_null()) {
        walk_node(val_node, ctx);
    }
}

void process_import_statement(treesitter::Node node, ASTContext& ctx) {
    auto source_node = node.child_by_field_name("source");
    if (!source_node.is_null()) {
        std::string path = clean_quotes(source_node.text(ctx.source));
        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::import,
            .written_name = path,
            .range = source_node.byte_range(),
            .display_range = source_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = {path},
        });
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch == source_node)
            continue;
        if (ch.type() == "import_clause") {
            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto clause_child = ch.child(j);
                if (clause_child.type() == "named_imports") {
                    for (uint32_t k = 0; k < clause_child.child_count(); ++k) {
                        auto spec = clause_child.child(k);
                        if (spec.type() == "import_specifier") {
                            auto name_node = spec.child_by_field_name("name");
                            auto alias_node = spec.child_by_field_name("alias");
                            if (!alias_node.is_null()) {
                                std::string alias_str = std::string(alias_node.text(ctx.source));
                                std::string orig_str = std::string(name_node.text(ctx.source));
                                ctx.handled_identifier_byte_starts.insert(alias_node.start_byte());
                                ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

                                const auto scope = ctx.current_scope();
                                const std::string qname =
                                    scope ? *scope + "." + alias_str : alias_str;

                                ctx.result.symbols.push_back(SymbolFact{
                                    .name = alias_str,
                                    .qualified_name = qname,
                                    .kind = NodeKind::type_alias,
                                    .range = alias_node.byte_range(),
                                    .display_range = alias_node.display_range(),
                                    .enclosing_scope = scope,
                                    .signature = "import { " + orig_str + " as " + alias_str + " }",
                                });
                                ctx.result.declarations.push_back(DeclarationFact{
                                    .symbol_name = alias_str,
                                    .qualified_name = qname,
                                    .kind = NodeKind::type_alias,
                                    .range = alias_node.byte_range(),
                                    .display_range = alias_node.display_range(),
                                    .enclosing_scope = scope,
                                    .is_definition = true,
                                });
                                ctx.result.occurrences.push_back(OccurrenceFact{
                                    .kind = worker::FactKind::reference,
                                    .written_name = orig_str,
                                    .range = name_node.byte_range(),
                                    .display_range = name_node.display_range(),
                                    .enclosing_scope = scope,
                                    .candidate_targets = {orig_str},
                                });
                            } else if (!name_node.is_null()) {
                                std::string name_str = std::string(name_node.text(ctx.source));
                                ctx.handled_identifier_byte_starts.insert(name_node.start_byte());
                                ctx.result.occurrences.push_back(OccurrenceFact{
                                    .kind = worker::FactKind::reference,
                                    .written_name = name_str,
                                    .range = name_node.byte_range(),
                                    .display_range = name_node.display_range(),
                                    .enclosing_scope = ctx.current_scope(),
                                    .candidate_targets = {name_str},
                                });
                            }
                        }
                    }
                } else if (clause_child.type() == "namespace_import") {
                    for (uint32_t k = 0; k < clause_child.child_count(); ++k) {
                        auto nsch = clause_child.child(k);
                        if (nsch.type() == "identifier") {
                            std::string ns_str = std::string(nsch.text(ctx.source));
                            ctx.handled_identifier_byte_starts.insert(nsch.start_byte());
                            const auto scope = ctx.current_scope();
                            const std::string qname = scope ? *scope + "." + ns_str : ns_str;
                            ctx.result.symbols.push_back(SymbolFact{
                                .name = ns_str,
                                .qualified_name = qname,
                                .kind = NodeKind::namespace_,
                                .range = nsch.byte_range(),
                                .display_range = nsch.display_range(),
                                .enclosing_scope = scope,
                                .signature = "import * as " + ns_str,
                            });
                            ctx.result.declarations.push_back(DeclarationFact{
                                .symbol_name = ns_str,
                                .qualified_name = qname,
                                .kind = NodeKind::namespace_,
                                .range = nsch.byte_range(),
                                .display_range = nsch.display_range(),
                                .enclosing_scope = scope,
                                .is_definition = true,
                            });
                        }
                    }
                } else if (clause_child.type() == "identifier") {
                    std::string def_name = std::string(clause_child.text(ctx.source));
                    ctx.handled_identifier_byte_starts.insert(clause_child.start_byte());
                    const auto scope = ctx.current_scope();
                    const std::string qname = scope ? *scope + "." + def_name : def_name;
                    ctx.result.symbols.push_back(SymbolFact{
                        .name = def_name,
                        .qualified_name = qname,
                        .kind = NodeKind::variable,
                        .range = clause_child.byte_range(),
                        .display_range = clause_child.display_range(),
                        .enclosing_scope = scope,
                        .signature = "import " + def_name,
                    });
                    ctx.result.declarations.push_back(DeclarationFact{
                        .symbol_name = def_name,
                        .qualified_name = qname,
                        .kind = NodeKind::variable,
                        .range = clause_child.byte_range(),
                        .display_range = clause_child.display_range(),
                        .enclosing_scope = scope,
                        .is_definition = true,
                    });
                }
            }
        }
    }
    mark_all_identifiers_handled(node, ctx);
}

void process_export_statement(treesitter::Node node, ASTContext& ctx) {
    auto source_node = node.child_by_field_name("source");
    if (!source_node.is_null()) {
        std::string path = clean_quotes(source_node.text(ctx.source));
        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::import,
            .written_name = path,
            .range = source_node.byte_range(),
            .display_range = source_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = {path},
        });
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch == source_node)
            continue;
        if (ch.type() == "export_clause") {
            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto spec = ch.child(j);
                if (spec.type() == "export_specifier") {
                    auto name_node = spec.child_by_field_name("name");
                    auto alias_node = spec.child_by_field_name("alias");
                    if (!alias_node.is_null()) {
                        std::string alias_str = std::string(alias_node.text(ctx.source));
                        std::string orig_str = std::string(name_node.text(ctx.source));
                        ctx.handled_identifier_byte_starts.insert(alias_node.start_byte());
                        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

                        const auto scope = ctx.current_scope();
                        const std::string qname = scope ? *scope + "." + alias_str : alias_str;

                        ctx.result.symbols.push_back(SymbolFact{
                            .name = alias_str,
                            .qualified_name = qname,
                            .kind = NodeKind::type_alias,
                            .range = alias_node.byte_range(),
                            .display_range = alias_node.display_range(),
                            .enclosing_scope = scope,
                            .signature = "export { " + orig_str + " as " + alias_str + " }",
                        });
                        ctx.result.declarations.push_back(DeclarationFact{
                            .symbol_name = alias_str,
                            .qualified_name = qname,
                            .kind = NodeKind::type_alias,
                            .range = alias_node.byte_range(),
                            .display_range = alias_node.display_range(),
                            .enclosing_scope = scope,
                            .is_definition = true,
                        });
                        ctx.result.occurrences.push_back(OccurrenceFact{
                            .kind = worker::FactKind::reference,
                            .written_name = orig_str,
                            .range = name_node.byte_range(),
                            .display_range = name_node.display_range(),
                            .enclosing_scope = scope,
                            .candidate_targets = {orig_str},
                        });
                    } else if (!name_node.is_null()) {
                        std::string name_str = std::string(name_node.text(ctx.source));
                        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());
                        ctx.result.occurrences.push_back(OccurrenceFact{
                            .kind = worker::FactKind::reference,
                            .written_name = name_str,
                            .range = name_node.byte_range(),
                            .display_range = name_node.display_range(),
                            .enclosing_scope = ctx.current_scope(),
                            .candidate_targets = {name_str},
                        });
                    }
                }
            }
        }
    }

    auto decl_node = node.child_by_field_name("declaration");
    if (!decl_node.is_null()) {
        walk_node(decl_node, ctx);
    } else if (source_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() != "export_clause") {
                walk_node(ch, ctx);
            }
        }
    }
}

void process_call_expression(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node func_node = node.child_by_field_name("function");
    if (!func_node.is_null()) {
        if (func_node.type() == "identifier" && func_node.text(ctx.source) == "require") {
            treesitter::Node args_node = node.child_by_field_name("arguments");
            if (!args_node.is_null()) {
                for (uint32_t i = 0; i < args_node.child_count(); ++i) {
                    auto ch = args_node.child(i);
                    if (ch.type() == "string" || ch.type() == "template_string") {
                        std::string path = clean_quotes(ch.text(ctx.source));
                        ctx.handled_identifier_byte_starts.insert(func_node.start_byte());
                        mark_all_identifiers_handled(ch, ctx);

                        ctx.result.occurrences.push_back(OccurrenceFact{
                            .kind = worker::FactKind::import,
                            .written_name = path,
                            .range = ch.byte_range(),
                            .display_range = ch.display_range(),
                            .enclosing_scope = ctx.current_scope(),
                            .candidate_targets = {path},
                        });
                        return;
                    }
                }
            }
        }

        std::string callee;
        treesitter::Node callee_name_node = func_node;

        if (func_node.type() == "identifier") {
            callee = std::string(func_node.text(ctx.source));
        } else if (func_node.type() == "member_expression") {
            treesitter::Node prop_node = func_node.child_by_field_name("property");
            if (!prop_node.is_null()) {
                callee = std::string(prop_node.text(ctx.source));
                callee_name_node = prop_node;
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

            if (callee == "defineProperty") {
                treesitter::Node args_node = node.child_by_field_name("arguments");
                if (!args_node.is_null()) {
                    for (uint32_t i = 0; i < args_node.child_count(); ++i) {
                        auto ch = args_node.child(i);
                        if (ch.type() == "string") {
                            std::string prop = clean_quotes(ch.text(ctx.source));
                            if (!prop.empty()) {
                                ctx.result.symbols.push_back(SymbolFact{
                                    .name = prop,
                                    .qualified_name = prop,
                                    .kind = NodeKind::field,
                                    .range = ch.byte_range(),
                                    .display_range = ch.display_range(),
                                    .enclosing_scope = ctx.current_scope(),
                                    .signature = prop,
                                });
                                ctx.result.declarations.push_back(DeclarationFact{
                                    .symbol_name = prop,
                                    .qualified_name = prop,
                                    .kind = NodeKind::field,
                                    .range = ch.byte_range(),
                                    .display_range = ch.display_range(),
                                    .enclosing_scope = ctx.current_scope(),
                                    .is_definition = true,
                                });
                                mark_all_identifiers_handled(ch, ctx);
                                break;
                            }
                        }
                    }
                }
            }
        }

        if (func_node.type() == "member_expression") {
            treesitter::Node obj_node = func_node.child_by_field_name("object");
            if (!obj_node.is_null()) {
                walk_node(obj_node, ctx);
            }
        } else if (callee.empty()) {
            walk_node(func_node, ctx);
        }
    }

    treesitter::Node type_args_node = node.child_by_field_name("type_arguments");
    if (!type_args_node.is_null()) {
        walk_node(type_args_node, ctx);
    }

    treesitter::Node args_node = node.child_by_field_name("arguments");
    if (!args_node.is_null()) {
        walk_node(args_node, ctx);
    }
}

void process_new_expression(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node ctor_node = node.child_by_field_name("constructor");
    if (ctor_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.is_named() && ch.type() != "arguments" && ch.type() != "type_arguments") {
                ctor_node = ch;
                break;
            }
        }
    }

    if (!ctor_node.is_null()) {
        std::string callee;
        treesitter::Node callee_name_node = ctor_node;

        if (ctor_node.type() == "generic_type") {
            auto n = ctor_node.child_by_field_name("name");
            if (!n.is_null()) {
                callee_name_node = n;
                callee = std::string(n.text(ctx.source));
            }
            auto type_args = ctor_node.child_by_field_name("type_arguments");
            if (!type_args.is_null()) {
                walk_node(type_args, ctx);
            }
        } else if (ctor_node.type() == "identifier" || ctor_node.type() == "type_identifier") {
            callee = std::string(ctor_node.text(ctx.source));
        } else if (ctor_node.type() == "member_expression") {
            treesitter::Node prop_node = ctor_node.child_by_field_name("property");
            if (!prop_node.is_null()) {
                callee = std::string(prop_node.text(ctx.source));
                callee_name_node = prop_node;
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

        if (ctor_node.type() == "member_expression") {
            treesitter::Node obj_node = ctor_node.child_by_field_name("object");
            if (!obj_node.is_null()) {
                walk_node(obj_node, ctx);
            }
        } else if (callee.empty()) {
            walk_node(ctor_node, ctx);
        }
    }

    treesitter::Node type_args_node = node.child_by_field_name("type_arguments");
    if (!type_args_node.is_null()) {
        walk_node(type_args_node, ctx);
    }

    treesitter::Node args_node = node.child_by_field_name("arguments");
    if (!args_node.is_null()) {
        walk_node(args_node, ctx);
    }
}

void process_jsx_element(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node;
    treesitter::Node open_tag;
    treesitter::Node close_tag;

    if (node.type() == "jsx_self_closing_element") {
        name_node = node.child_by_field_name("name");
    } else {
        open_tag = node.child_by_field_name("open_tag");
        close_tag = node.child_by_field_name("close_tag");
        if (!open_tag.is_null()) {
            name_node = open_tag.child_by_field_name("name");
        }
    }

    if (!name_node.is_null()) {
        std::string tag_name = std::string(name_node.text(ctx.source));
        bool is_custom = false;
        if (!tag_name.empty() && std::isupper(static_cast<unsigned char>(tag_name[0]))) {
            is_custom = true;
        } else if (tag_name.find('.') != std::string::npos) {
            is_custom = true;
        }

        if (is_custom) {
            std::vector<std::string> targets = {tag_name};
            if (auto dot = tag_name.find('.'); dot != std::string::npos) {
                targets.push_back(tag_name.substr(0, dot));
            }
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::call,
                .written_name = tag_name,
                .range = name_node.byte_range(),
                .display_range = name_node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = std::move(targets),
            });
        }
        mark_all_identifiers_handled(name_node, ctx);
    }

    if (!close_tag.is_null()) {
        treesitter::Node close_name = close_tag.child_by_field_name("name");
        if (!close_name.is_null()) {
            mark_all_identifiers_handled(close_name, ctx);
        }
    }

    if (node.type() == "jsx_self_closing_element") {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch != name_node) {
                walk_node(ch, ctx);
            }
        }
    } else {
        if (!open_tag.is_null()) {
            for (uint32_t i = 0; i < open_tag.child_count(); ++i) {
                auto ch = open_tag.child(i);
                if (ch != name_node) {
                    walk_node(ch, ctx);
                }
            }
        }
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch != open_tag && ch != close_tag) {
                walk_node(ch, ctx);
            }
        }
    }
}

void process_assignment_expression(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node left = node.child_by_field_name("left");
    treesitter::Node right = node.child_by_field_name("right");
    if (left.is_null()) {
        if (!right.is_null()) {
            walk_node(right, ctx);
        }
        return;
    }

    if (left.type() == "member_expression") {
        if (left.text(ctx.source) == "module.exports") {
            mark_all_identifiers_handled(left, ctx);
            if (!right.is_null()) {
                walk_node(right, ctx);
            }
            return;
        }

        treesitter::Node prop = left.child_by_field_name("property");
        treesitter::Node obj = left.child_by_field_name("object");

        if (!obj.is_null() && !prop.is_null()) {
            std::string prop_name = std::string(prop.text(ctx.source));

            // Case A: <Class>.prototype.<method> = function(...) {} / () => {}
            if (obj.type() == "member_expression") {
                treesitter::Node inner_obj = obj.child_by_field_name("object");
                treesitter::Node inner_prop = obj.child_by_field_name("property");
                if (!inner_obj.is_null() && !inner_prop.is_null() &&
                    inner_prop.text(ctx.source) == "prototype") {
                    std::string class_name = std::string(inner_obj.text(ctx.source));
                    ctx.handled_identifier_byte_starts.insert(prop.start_byte());
                    ctx.handled_identifier_byte_starts.insert(inner_prop.start_byte());
                    ctx.handled_identifier_byte_starts.insert(inner_obj.start_byte());

                    const std::string qname = class_name + "." + prop_name;
                    const bool is_func =
                        !right.is_null() &&
                        (right.type() == "function_expression" || right.type() == "arrow_function");

                    ctx.result.symbols.push_back(SymbolFact{
                        .name = prop_name,
                        .qualified_name = qname,
                        .kind = is_func ? NodeKind::method : NodeKind::field,
                        .range = prop.byte_range(),
                        .display_range = prop.display_range(),
                        .enclosing_scope = class_name,
                        .signature = prop_name,
                    });
                    ctx.result.declarations.push_back(DeclarationFact{
                        .symbol_name = prop_name,
                        .qualified_name = qname,
                        .kind = is_func ? NodeKind::method : NodeKind::field,
                        .range = prop.byte_range(),
                        .display_range = prop.display_range(),
                        .enclosing_scope = class_name,
                        .is_definition = true,
                    });

                    if (is_func) {
                        ctx.push_scope(class_name, ScopeKind::class_);
                        ctx.push_scope(prop_name, ScopeKind::function);
                        walk_node(right, ctx);
                        ctx.pop_scope();
                        ctx.pop_scope();
                        return;
                    }
                }
            }

            // Case B: exports.<name> = ... or module.exports.<name> = ...
            std::string obj_text = std::string(obj.text(ctx.source));
            if (obj_text == "exports" || obj_text == "module.exports") {
                ctx.handled_identifier_byte_starts.insert(prop.start_byte());
                mark_all_identifiers_handled(obj, ctx);

                const auto scope = ctx.current_scope();
                const std::string qname = scope ? *scope + "." + prop_name : prop_name;
                const bool is_func = !right.is_null() && (right.type() == "function_expression" ||
                                                          right.type() == "arrow_function");

                ctx.result.symbols.push_back(SymbolFact{
                    .name = prop_name,
                    .qualified_name = qname,
                    .kind = is_func ? NodeKind::function : NodeKind::variable,
                    .range = prop.byte_range(),
                    .display_range = prop.display_range(),
                    .enclosing_scope = scope,
                    .signature = prop_name,
                });
                ctx.result.declarations.push_back(DeclarationFact{
                    .symbol_name = prop_name,
                    .qualified_name = qname,
                    .kind = is_func ? NodeKind::function : NodeKind::variable,
                    .range = prop.byte_range(),
                    .display_range = prop.display_range(),
                    .enclosing_scope = scope,
                    .is_definition = true,
                });

                if (is_func) {
                    ctx.push_scope(prop_name, ScopeKind::function);
                    walk_node(right, ctx);
                    ctx.pop_scope();
                    return;
                }
            }
        }
    }

    walk_node(left, ctx);
    if (!right.is_null()) {
        walk_node(right, ctx);
    }
}

void process_subscript_expression(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node obj_node = node.child_by_field_name("object");
    treesitter::Node index_node = node.child_by_field_name("index");

    if (!obj_node.is_null()) {
        walk_node(obj_node, ctx);
    }

    if (!index_node.is_null()) {
        if (index_node.type() == "string") {
            std::string prop = clean_quotes(index_node.text(ctx.source));
            if (!prop.empty()) {
                ctx.result.occurrences.push_back(OccurrenceFact{
                    .kind = worker::FactKind::reference,
                    .written_name = prop,
                    .range = index_node.byte_range(),
                    .display_range = index_node.display_range(),
                    .enclosing_scope = ctx.current_scope(),
                    .candidate_targets = {prop},
                });
                mark_all_identifiers_handled(index_node, ctx);
                return;
            }
        }
        walk_node(index_node, ctx);
    }
}

void walk_node(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null())
        return;
    if (ctx.stop_token.stop_requested())
        return;

    const auto type = node.type();

    if (type == "ambient_declaration") {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() != "declare") {
                walk_node(ch, ctx);
            }
        }
        return;
    }

    if (type == "import_statement") {
        process_import_statement(node, ctx);
        return;
    }

    if (type == "export_statement") {
        process_export_statement(node, ctx);
        return;
    }

    if (type == "internal_module" || type == "module") {
        process_module(node, ctx);
        return;
    }

    if (type == "class_declaration" || type == "class" || type == "abstract_class_declaration") {
        process_class(node, ctx);
        return;
    }

    if (type == "interface_declaration") {
        process_interface(node, ctx);
        return;
    }

    if (type == "type_alias_declaration") {
        process_type_alias(node, ctx);
        return;
    }

    if (type == "enum_declaration") {
        process_enum(node, ctx);
        return;
    }

    if (type == "function_declaration" || type == "generator_function_declaration" ||
        type == "function_signature") {
        process_function(node, ctx);
        return;
    }

    if (type == "method_definition") {
        process_method(node, ctx);
        return;
    }

    if (type == "method_signature" || type == "abstract_method_signature") {
        process_method_signature(node, ctx);
        return;
    }

    if (type == "assignment_expression") {
        process_assignment_expression(node, ctx);
        return;
    }

    if (type == "subscript_expression") {
        process_subscript_expression(node, ctx);
        return;
    }

    if (type == "property_signature") {
        process_property_signature(node, ctx);
        return;
    }

    if (type == "variable_declarator") {
        process_variable_declarator(node, ctx);
        return;
    }

    if (type == "public_field_definition" || type == "field_definition" ||
        type == "property_definition") {
        process_field_definition(node, ctx);
        return;
    }

    if (type == "call_expression") {
        process_call_expression(node, ctx);
        return;
    }

    if (type == "new_expression") {
        process_new_expression(node, ctx);
        return;
    }

    if (type == "jsx_element" || type == "jsx_self_closing_element") {
        process_jsx_element(node, ctx);
        return;
    }

    if (type == "jsx_attribute") {
        if (node.child_count() > 0) {
            mark_all_identifiers_handled(node.child(0), ctx);
        }
        for (uint32_t i = 1; i < node.child_count(); ++i) {
            walk_node(node.child(i), ctx);
        }
        return;
    }

    if (type == "type_parameters") {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "type_parameter") {
                auto n = ch.child_by_field_name("name");
                if (!n.is_null()) {
                    ctx.handled_identifier_byte_starts.insert(n.start_byte());
                }
                auto constraint = ch.child_by_field_name("constraint");
                if (!constraint.is_null()) {
                    walk_node(constraint, ctx);
                }
                auto default_type = ch.child_by_field_name("default");
                if (!default_type.is_null()) {
                    walk_node(default_type, ctx);
                }
            }
        }
        return;
    }

    if (type == "identifier" || type == "property_identifier" || type == "type_identifier" ||
        type == "shorthand_property_identifier") {
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

TypeScriptAdapter::TypeScriptAdapter(Language lang) : lang_(lang) {
    const bool is_js = (lang_ == Language::javascript);
    capabilities_ = {
        .functions = CapabilityStatus::supported,
        .methods = CapabilityStatus::supported,
        .classes = CapabilityStatus::supported,
        .structs = CapabilityStatus::unavailable,
        .interfaces = is_js ? CapabilityStatus::unavailable : CapabilityStatus::supported,
        .enums = is_js ? CapabilityStatus::unavailable : CapabilityStatus::supported,
        .records = CapabilityStatus::unavailable,
        .namespaces = is_js ? CapabilityStatus::unavailable : CapabilityStatus::supported,
        .variables = CapabilityStatus::supported,
        .modules = CapabilityStatus::supported,
        .packages = CapabilityStatus::unavailable,
        .templates = is_js ? CapabilityStatus::unavailable : CapabilityStatus::supported,
        .partial_types = CapabilityStatus::unavailable,
        .containment = CapabilityStatus::supported,
        .calls = CapabilityStatus::supported,
        .references = CapabilityStatus::supported,
        .inheritance = CapabilityStatus::supported,
        .implementation = is_js ? CapabilityStatus::unavailable : CapabilityStatus::supported,
        .imports = CapabilityStatus::supported,
        .includes = CapabilityStatus::unavailable,
        .api_endpoints = CapabilityStatus::deferred,
        .database_tables = CapabilityStatus::deferred,
        .test_declarations = CapabilityStatus::deferred,
        .override_analysis = CapabilityStatus::deferred,
        .instantiation_analysis = CapabilityStatus::deferred,
    };
}

const LanguageCapabilities& TypeScriptAdapter::capabilities() const noexcept {
    return capabilities_;
}

Result<AdapterResult> TypeScriptAdapter::parse(std::string_view source,
                                               const std::filesystem::path& file_path,
                                               const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        return unexpected_result<AdapterResult>(ErrorCode::cancelled, "parsing cancelled");
    }

    std::string ext = file_path.extension().string();
    for (auto& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    const TSLanguage* ts_lang = nullptr;
    if (ext == ".tsx" || ext == ".jsx") {
        ts_lang = treesitter::grammar_for_tsx();
    } else if (lang_ == Language::javascript || ext == ".js" || ext == ".mjs" || ext == ".cjs") {
        ts_lang = treesitter::grammar_for_language(Language::javascript);
    } else {
        ts_lang = treesitter::grammar_for_language(Language::typescript);
    }

    if (!ts_lang) {
        return unexpected_result<AdapterResult>(ErrorCode::invalid_argument,
                                                "Grammar not available");
    }

    treesitter::Parser parser;
    auto set_lang = parser.set_language(ts_lang);
    if (!set_lang) {
        return unexpected_result<AdapterResult>(set_lang.error().code, set_lang.error().message);
    }

    auto tree_res = parser.parse_string(source, nullptr, stop_token);
    if (!tree_res) {
        return unexpected_result<AdapterResult>(tree_res.error().code, tree_res.error().message);
    }

    AdapterResult result{
        .language = lang_,
        .status = worker::CompletionStatus::complete,
        .symbols = {},
        .declarations = {},
        .occurrences = {},
        .diagnostics = {},
    };

    treesitter::Node root = tree_res->root_node();
    result.diagnostics = collect_syntax_errors(root, lang_);

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

std::string_view TypeScriptAdapter::javascript_highlighting_query() noexcept {
    static constexpr std::string_view kJSHighlightQuery = R"(
; Literals
(comment) @comment
(string) @string
(template_string) @string
(regex) @regexp
(number) @number

; Classes
(class_declaration name: (identifier) @class)
(class name: (identifier) @class)

; Functions & Methods
(function_declaration name: (identifier) @function)
(function_expression name: (identifier) @function)
(method_definition name: (property_identifier) @method)
(variable_declarator name: (identifier) @function value: [(arrow_function) (function_expression)])

; Calls
(call_expression function: (identifier) @function)
(call_expression function: (member_expression property: (property_identifier) @method))

; Variables & Parameters
(formal_parameters (identifier) @parameter)
(formal_parameters (assignment_pattern left: (identifier) @parameter))
(formal_parameters (rest_pattern (identifier) @parameter))
(arrow_function parameter: (identifier) @parameter)
(variable_declarator name: (identifier) @variable)

; Properties
(pair key: (property_identifier) @property)

; JSX Elements & Attributes
(jsx_attribute (property_identifier) @property)
(jsx_opening_element name: (identifier) @class)
(jsx_closing_element name: (identifier) @class)
(jsx_self_closing_element name: (identifier) @class)
(jsx_opening_element name: (member_expression property: (property_identifier) @class))
(jsx_closing_element name: (member_expression property: (property_identifier) @class))
(jsx_self_closing_element name: (member_expression property: (property_identifier) @class))

; Keywords
[
  "as"
  "async"
  "await"
  "break"
  "case"
  "catch"
  "class"
  "const"
  "continue"
  "debugger"
  "default"
  "delete"
  "do"
  "else"
  "export"
  "extends"
  "finally"
  "for"
  "from"
  "function"
  "get"
  "if"
  "import"
  "in"
  "instanceof"
  "let"
  "new"
  "of"
  "return"
  "set"
  "static"
  "switch"
  "throw"
  "try"
  "typeof"
  "var"
  "void"
  "while"
  "with"
  "yield"
] @keyword

[
  (this)
  (super)
  (true)
  (false)
  (null)
  (undefined)
] @keyword

; Operators
[
  "+" "-" "*" "/" "%" "=" "+=" "-=" "*=" "/=" "%="
  "==" "===" "!=" "!==" "<" "<=" ">" ">="
  "&&" "||" "!" "??" "??=" "&&=" "||="
  "&" "|" "^" "~" "<<" ">>" ">>>"
  "=>"
] @operator
)";
    return kJSHighlightQuery;
}

std::string_view TypeScriptAdapter::highlighting_query() noexcept {
    static constexpr std::string_view kHighlightQuery = R"(
; Types
(type_identifier) @type
(predefined_type) @type
(type_alias_declaration name: (type_identifier) @type)

; Classes, Interfaces, Enums
(class_declaration name: (type_identifier) @class)
(interface_declaration name: (type_identifier) @interface)
(enum_declaration name: (identifier) @enum)
(enum_assignment name: (property_identifier) @enumMember)
(enum_body (property_identifier) @enumMember)

; Variables & Parameters
(required_parameter (identifier) @parameter)
(optional_parameter (identifier) @parameter)
(variable_declarator name: (identifier) @variable)
(variable_declarator name: (identifier) @function value: [(arrow_function) (function_expression)])

; Functions & Methods
(function_declaration name: (identifier) @function)
(function_expression name: (identifier) @function)
(method_definition name: (property_identifier) @method)
(method_signature name: (property_identifier) @method)

; Calls
(call_expression function: (identifier) @function)
(call_expression function: (member_expression property: (property_identifier) @method))

; Properties & Fields
(property_signature name: (property_identifier) @property)
(public_field_definition name: (property_identifier) @property)

; Literals
(comment) @comment
(string) @string
(template_string) @string
(regex) @regexp
(number) @number

; Decorators
(decorator) @decorator

; Keywords
[
  "abstract"
  "as"
  "async"
  "await"
  "break"
  "case"
  "catch"
  "class"
  "const"
  "continue"
  "debugger"
  "declare"
  "default"
  "delete"
  "do"
  "else"
  "enum"
  "export"
  "extends"
  "finally"
  "for"
  "from"
  "function"
  "get"
  "if"
  "implements"
  "import"
  "in"
  "instanceof"
  "interface"
  "keyof"
  "let"
  "namespace"
  "new"
  "of"
  "override"
  "private"
  "protected"
  "public"
  "readonly"
  "return"
  "satisfies"
  "set"
  "static"
  "switch"
  "throw"
  "try"
  "type"
  "typeof"
  "var"
  "void"
  "while"
  "with"
  "yield"
] @keyword

[
  (this)
  (super)
  (true)
  (false)
  (null)
  (undefined)
] @keyword

; Operators
[
  "+" "-" "*" "/" "%" "=" "+=" "-=" "*=" "/=" "%="
  "==" "===" "!=" "!==" "<" "<=" ">" ">="
  "&&" "||" "!" "??" "??=" "&&=" "||="
  "&" "|" "^" "~" "<<" ">>" ">>>"
  "=>"
] @operator
)";
    return kHighlightQuery;
}

std::string_view TypeScriptAdapter::tsx_highlighting_query() noexcept {
    static constexpr std::string_view kTSXHighlightQuery = R"(
; Types
(type_identifier) @type
(predefined_type) @type
(type_alias_declaration name: (type_identifier) @type)

; Classes, Interfaces, Enums
(class_declaration name: (type_identifier) @class)
(interface_declaration name: (type_identifier) @interface)
(enum_declaration name: (identifier) @enum)
(enum_assignment name: (property_identifier) @enumMember)
(enum_body (property_identifier) @enumMember)

; Variables & Parameters
(required_parameter (identifier) @parameter)
(optional_parameter (identifier) @parameter)
(variable_declarator name: (identifier) @variable)
(variable_declarator name: (identifier) @function value: [(arrow_function) (function_expression)])

; Functions & Methods
(function_declaration name: (identifier) @function)
(function_expression name: (identifier) @function)
(method_definition name: (property_identifier) @method)
(method_signature name: (property_identifier) @method)

; Calls
(call_expression function: (identifier) @function)
(call_expression function: (member_expression property: (property_identifier) @method))

; Properties & Fields
(property_signature name: (property_identifier) @property)
(public_field_definition name: (property_identifier) @property)

; JSX Elements & Attributes
(jsx_attribute (property_identifier) @property)
(jsx_opening_element name: (identifier) @class)
(jsx_closing_element name: (identifier) @class)
(jsx_self_closing_element name: (identifier) @class)
(jsx_opening_element name: (member_expression property: (property_identifier) @class))
(jsx_closing_element name: (member_expression property: (property_identifier) @class))
(jsx_self_closing_element name: (member_expression property: (property_identifier) @class))

; Literals
(comment) @comment
(string) @string
(template_string) @string
(regex) @regexp
(number) @number

; Decorators
(decorator) @decorator

; Keywords
[
  "abstract"
  "as"
  "async"
  "await"
  "break"
  "case"
  "catch"
  "class"
  "const"
  "continue"
  "debugger"
  "declare"
  "default"
  "delete"
  "do"
  "else"
  "enum"
  "export"
  "extends"
  "finally"
  "for"
  "from"
  "function"
  "get"
  "if"
  "implements"
  "import"
  "in"
  "instanceof"
  "interface"
  "keyof"
  "let"
  "namespace"
  "new"
  "of"
  "override"
  "private"
  "protected"
  "public"
  "readonly"
  "return"
  "satisfies"
  "set"
  "static"
  "switch"
  "throw"
  "try"
  "type"
  "typeof"
  "var"
  "void"
  "while"
  "with"
  "yield"
] @keyword

[
  (this)
  (super)
  (true)
  (false)
  (null)
  (undefined)
] @keyword

; Operators
[
  "+" "-" "*" "/" "%" "=" "+=" "-=" "*=" "/=" "%="
  "==" "===" "!=" "!==" "<" "<=" ">" ">="
  "&&" "||" "!" "??" "??=" "&&=" "||="
  "&" "|" "^" "~" "<<" ">>" ">>>"
  "=>"
] @operator
)";
    return kTSXHighlightQuery;
}

Result<std::vector<HighlightToken>> TypeScriptAdapter::highlight(std::string_view source,
                                                                 const treesitter::Tree& tree) {
    const auto* tree_lang = tree.language();
    const bool is_js =
        (tree_lang == treesitter::grammar_for_language(Language::javascript)) ||
        (lang_ == Language::javascript && tree_lang != treesitter::grammar_for_tsx());
    const bool is_tsx = (tree_lang == treesitter::grammar_for_tsx());

    const auto* ts_lang = is_js ? treesitter::grammar_for_language(Language::javascript)
                                : (is_tsx ? treesitter::grammar_for_tsx()
                                          : treesitter::grammar_for_language(Language::typescript));
    if (ts_lang == nullptr) {
        return unexpected_result<std::vector<HighlightToken>>(ErrorCode::invalid_argument,
                                                              "Grammar not available");
    }

    if (tree_lang != nullptr && tree_lang != ts_lang) {
        return unexpected_result<std::vector<HighlightToken>>(
            ErrorCode::invalid_argument, "Tree grammar does not match adapter grammar");
    }

    const auto query_source = is_js ? javascript_highlighting_query()
                                    : (is_tsx ? tsx_highlighting_query() : highlighting_query());

    auto query_res = treesitter::Query::create(ts_lang, query_source);
    if (!query_res) {
        return unexpected_result<std::vector<HighlightToken>>(
            ErrorCode::failed, "Failed to create highlight query: " + query_res.error().message);
    }

    const auto& query = *query_res;
    treesitter::QueryCursor cursor;
    cursor.exec(query, tree.root_node());

    CoordinateConverter converter(source);
    const auto& legend = HighlightLegend::default_legend();

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
            token_map.emplace(key, HighlightToken{
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

    std::vector<HighlightToken> tokens;
    tokens.reserve(token_map.size());
    for (auto& [k, v] : token_map) {
        tokens.push_back(v);
    }

    std::ranges::sort(tokens, [](const HighlightToken& a, const HighlightToken& b) {
        if (a.line != b.line) {
            return a.line < b.line;
        }
        return a.start_column < b.start_column;
    });

    return tokens;
}

Result<std::vector<HighlightToken>>
TypeScriptAdapter::highlight(std::string_view source, const std::filesystem::path& file_path) {
    treesitter::Parser parser;
    auto ext = file_path.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const bool is_tsx = (ext == ".tsx" || ext == ".jsx");
    const auto* ts_lang = is_tsx ? treesitter::grammar_for_tsx()
                          : (lang_ == Language::javascript)
                              ? treesitter::grammar_for_language(Language::javascript)
                              : treesitter::grammar_for_language(Language::typescript);
    if (ts_lang == nullptr) {
        return unexpected_result<std::vector<HighlightToken>>(ErrorCode::invalid_argument,
                                                              "Grammar not available");
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
