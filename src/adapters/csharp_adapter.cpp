#include "codelenses/adapters/csharp_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "codelenses/parser/coordinate_converter.hpp"
#include "codelenses/parser/highlight.hpp"
#include "codelenses/treesitter/grammars.hpp"
#include "codelenses/treesitter/parser.hpp"

namespace codelenses::adapters {

namespace {

struct ASTContext {
    std::string_view source;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    const std::stop_token& stop_token;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    AdapterResult& result;
    std::vector<std::string> scope_stack;
    std::unordered_set<uint32_t> handled_identifier_byte_starts;

    [[nodiscard]] std::optional<std::string> current_scope() const {
        if (scope_stack.empty()) {
            return std::nullopt;
        }
        std::string full;
        for (size_t i = 0; i < scope_stack.size(); ++i) {
            if (i > 0) {
                full += ".";
            }
            full += scope_stack[i];
        }
        return full;
    }
};

void mark_identifiers_handled(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null()) {
        return;
    }
    const auto type = node.type();
    if (type == "identifier" || type == "type_identifier") {
        ctx.handled_identifier_byte_starts.insert(node.start_byte());
    }
    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        mark_identifiers_handled(node.child(i), ctx);
    }
}

bool has_modifier(treesitter::Node node, std::string_view modifier_name, std::string_view source) {
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "modifier" && ch.text(source) == modifier_name) {
            return true;
        }
    }
    return false;
}

std::string clean_signature(std::string_view text, size_t max_len = 128) {
    size_t cutoff = text.find('{');
    size_t semi = text.find(';');
    if (semi != std::string_view::npos && (cutoff == std::string_view::npos || semi < cutoff)) {
        cutoff = semi;
    }
    if (cutoff == std::string_view::npos) {
        cutoff = text.size();
    }
    cutoff = std::min(cutoff, max_len);
    while (cutoff > 0 && (text[cutoff - 1] == ' ' || text[cutoff - 1] == '\t' ||
                          text[cutoff - 1] == '\r' || text[cutoff - 1] == '\n')) {
        --cutoff;
    }

    std::string result;
    result.reserve(cutoff);
    bool in_space = false;
    for (size_t i = 0; i < cutoff; ++i) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
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

std::string get_unqualified_name(std::string_view full_name) {
    auto paren_pos = full_name.find('(');
    std::string_view stripped =
        (paren_pos != std::string_view::npos) ? full_name.substr(0, paren_pos) : full_name;
    auto generic_pos = stripped.find('<');
    if (generic_pos != std::string_view::npos) {
        stripped = stripped.substr(0, generic_pos);
    }
    auto last_dot = stripped.rfind('.');
    if (last_dot != std::string_view::npos) {
        stripped = stripped.substr(last_dot + 1);
    }
    return std::string(stripped);
}

void walk_node(treesitter::Node node, ASTContext& ctx);

void mark_named_type_identifiers(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null() || node.type() == "type_argument_list") return;
    const auto type = node.type();
    if (type == "identifier" || type == "type_identifier") {
        ctx.handled_identifier_byte_starts.insert(node.start_byte());
    }
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        mark_named_type_identifiers(node.child(i), ctx);
    }
}

void walk_type_arguments(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null()) return;
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto child = node.child(i);
        if (child.type() == "type_argument_list") {
            walk_node(child, ctx);
        } else {
            walk_type_arguments(child, ctx);
        }
    }
}

void process_named_type(treesitter::Node node, ASTContext& ctx) {
    const std::string name = std::string(node.text(ctx.source));
    if (name.empty()) return;
    mark_named_type_identifiers(node, ctx);
    std::vector<std::string> candidates{name};
    const auto unqualified = get_unqualified_name(name);
    if (unqualified != name && !unqualified.empty()) candidates.push_back(unqualified);
    ctx.result.occurrences.push_back(OccurrenceFact{
        .kind = worker::FactKind::reference,
        .written_name = name,
        .range = node.byte_range(),
        .display_range = node.display_range(),
        .enclosing_scope = ctx.current_scope(),
        .candidate_targets = std::move(candidates),
    });
    walk_type_arguments(node, ctx);
}

void process_attribute(treesitter::Node node, ASTContext& ctx,
                       std::optional<std::string> scope_override = std::nullopt) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (!name_node.is_null()) {
        const std::string attr_name = std::string(name_node.text(ctx.source));
        mark_identifiers_handled(name_node, ctx);

        std::vector<std::string> candidates = {attr_name};
        static constexpr std::string_view kAttrSuffix = "Attribute";
        if (attr_name.ends_with(kAttrSuffix) && attr_name.size() > kAttrSuffix.size()) {
            candidates.push_back(attr_name.substr(0, attr_name.size() - kAttrSuffix.size()));
        } else if (!attr_name.ends_with(kAttrSuffix)) {
            candidates.push_back(attr_name + std::string(kAttrSuffix));
        }

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::reference,
            .written_name = attr_name,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope_override.has_value() ? scope_override : ctx.current_scope(),
            .candidate_targets = std::move(candidates),
        });
    }

    auto old_scope = ctx.scope_stack;
    if (scope_override.has_value()) {
        std::vector<std::string> parts;
        std::string_view s = *scope_override;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t next = s.find('.', pos);
            if (next == std::string_view::npos) {
                parts.emplace_back(s.substr(pos));
                break;
            }
            parts.emplace_back(s.substr(pos, next - pos));
            pos = next + 1;
        }
        ctx.scope_stack = std::move(parts);
    }

    // In tree-sitter-c-sharp, attribute argument list is an unnamed child attribute_argument_list
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "attribute_argument_list") {
            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto arg = ch.child(j);
                if (arg.type() == "attribute_argument") {
                    // Mark argument name handled if named argument (e.g. Roles = "Admin" or Name:
                    // "val")
                    for (uint32_t k = 0; k < arg.child_count(); ++k) {
                        auto ach = arg.child(k);
                        if (ach.type() == "identifier" && k + 1 < arg.child_count() &&
                            (arg.child(k + 1).type() == "=" || arg.child(k + 1).type() == ":")) {
                            mark_identifiers_handled(ach, ctx);
                            break;
                        }
                    }
                    walk_node(arg, ctx);
                }
            }
        }
    }

    ctx.scope_stack = std::move(old_scope);
}

void walk_attributes(treesitter::Node node, ASTContext& ctx,
                     std::optional<std::string> scope_override = std::nullopt) {
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "attribute_list" || ch.type() == "global_attribute") {
            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto attr = ch.child(j);
                if (attr.type() == "attribute") {
                    process_attribute(attr, ctx, scope_override);
                }
            }
        }
    }
}

void process_using_directive(treesitter::Node node, ASTContext& ctx) {
    walk_attributes(node, ctx);
    bool is_global = node.type() == "global_using_directive";
    bool is_static = false;
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        const auto type = node.child(i).type();
        is_global = is_global || type == "global";
        is_static = is_static || type == "static";
    }
    const auto scope = ctx.current_scope();
    auto import_metadata = [&](std::string kind, std::string target,
                               std::optional<std::string> alias = std::nullopt,
                               std::string target_kind = "unknown") {
        nlohmann::json data{
            {"kind", std::move(kind)},
            {"target", std::move(target)},
            {"targetKind", std::move(target_kind)},
            {"global", is_global},
            {"scope", scope ? nlohmann::json(*scope) : nlohmann::json(nullptr)},
        };
        if (alias) data["alias"] = *alias;
        return nlohmann::json{{"csharpImport", std::move(data)}}.dump();
    };

    treesitter::Node alias_node = node.child_by_field_name("name");
    if (!alias_node.is_null()) {
        // Alias using: using Project = MyCompany.Project;
        const std::string alias_name = std::string(alias_node.text(ctx.source));
        mark_identifiers_handled(alias_node, ctx);

        std::string target_type_str;
        treesitter::Node target_node{};
        bool past_equals = false;
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "=") {
                past_equals = true;
                continue;
            }
            if (past_equals && ch.type() != ";" && ch.type() != "using") {
                target_node = ch;
                target_type_str = std::string(ch.text(ctx.source));
                break;
            }
        }

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + alias_name : alias_name;
        const std::string sig = clean_signature(node.text(ctx.source));

        ctx.result.symbols.push_back(SymbolFact{
            .name = alias_name,
            .qualified_name = qname,
            .kind = NodeKind::type_alias,
            .range = alias_node.byte_range(),
            .display_range = alias_node.display_range(),
            .enclosing_scope = scope,
            .signature = sig,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = alias_name,
            .qualified_name = qname,
            .kind = NodeKind::type_alias,
            .range = alias_node.byte_range(),
            .display_range = alias_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });

        if (!target_node.is_null()) {
            mark_identifiers_handled(target_node, ctx);
            std::vector<std::string> candidates = {target_type_str};
            std::string unqual = get_unqualified_name(target_type_str);
            if (unqual != target_type_str && !unqual.empty()) {
                candidates.push_back(std::move(unqual));
            }
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::import,
                .written_name = target_type_str,
                .range = target_node.byte_range(),
                .display_range = target_node.display_range(),
                .enclosing_scope = scope,
                .candidate_targets = std::move(candidates),
                .metadata_json = import_metadata("alias", target_type_str, alias_name),
            });
        }
        return;
    }

    // Standard, static, or global using directive
    treesitter::Node import_name_node{};
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "identifier" || ch.type() == "qualified_name" ||
            ch.type() == "generic_name" || ch.type() == "alias_qualified_name") {
            import_name_node = ch;
            break;
        }
    }

    if (!import_name_node.is_null()) {
        const std::string name = std::string(import_name_node.text(ctx.source));
        mark_identifiers_handled(import_name_node, ctx);

        std::vector<std::string> candidates = {name};
        if (is_static) {
            std::string unqual = get_unqualified_name(name);
            if (unqual != name && !unqual.empty()) {
                candidates.push_back(std::move(unqual));
            }
        }

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::import,
            .written_name = name,
            .range = import_name_node.byte_range(),
            .display_range = import_name_node.display_range(),
            .enclosing_scope = scope,
            .candidate_targets = std::move(candidates),
            .metadata_json = import_metadata(is_static ? "static" : "namespace_or_type", name,
                                             std::nullopt, is_static ? "type" : "unknown"),
        });
    }
}

void process_namespace(treesitter::Node node, ASTContext& ctx) {
    walk_attributes(node, ctx);

    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");

    std::string ns_name;
    if (!name_node.is_null()) {
        ns_name = std::string(name_node.text(ctx.source));
        mark_identifiers_handled(name_node, ctx);

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
    }

    // In tree-sitter-c-sharp, file_scoped_namespace_declaration has no body (it is simply
    // 'namespace' NAME ';'), and subsequent declarations in the file are siblings in
    // compilation_unit. We intentionally do NOT pop ns_name from scope_stack so that all
    // subsequent top-level declarations in the file are enclosed in this namespace.
    if (!file_scoped && !ns_name.empty()) {
        ctx.scope_stack.pop_back();
    }
}

void process_type_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");

    const auto type_str = node.type();
    NodeKind kind = NodeKind::class_;
    if (type_str == "struct_declaration") {
        kind = NodeKind::struct_;
    } else if (type_str == "interface_declaration") {
        kind = NodeKind::interface_;
    } else if (type_str == "enum_declaration") {
        kind = NodeKind::enum_;
    } else if (type_str == "record_declaration") {
        bool has_struct_keyword = false;
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            if (node.child(i).type() == "struct") {
                has_struct_keyword = true;
                break;
            }
        }
        kind = has_struct_keyword ? NodeKind::struct_ : NodeKind::class_;
    } else if (type_str == "delegate_declaration") {
        kind = NodeKind::type_alias;
    }

    const bool is_partial = has_modifier(node, "partial", ctx.source);
    std::string name;
    if (!name_node.is_null()) {
        name = std::string(name_node.text(ctx.source));
        mark_identifiers_handled(name_node, ctx);

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + name : name;
        walk_attributes(node, ctx, qname);

        std::string signature = clean_signature(node.text(ctx.source));
        if (is_partial && signature.find("partial") == std::string::npos) {
            signature = "partial " + signature;
        }

        ctx.result.symbols.push_back(SymbolFact{
            .name = name,
            .qualified_name = qname,
            .kind = kind,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = signature,
        });

        // Delegate declarations are type definitions like structs or classes
        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = name,
            .qualified_name = qname,
            .kind = kind,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });

        // Mark type parameters handled so they are not treated as unknown identifier references
        treesitter::Node type_params = node.child_by_field_name("type_parameters");
        if (!type_params.is_null()) {
            mark_identifiers_handled(type_params, ctx);
        }

        // Delegate return type and parameters
        if (type_str == "delegate_declaration") {
            treesitter::Node ret_type = node.child_by_field_name("type");
            if (!ret_type.is_null()) {
                walk_node(ret_type, ctx);
            }
            treesitter::Node params = node.child_by_field_name("parameters");
            if (!params.is_null()) {
                for (uint32_t j = 0; j < params.child_count(); ++j) {
                    auto pch = params.child(j);
                    if (pch.type() == "parameter") {
                        treesitter::Node p_name = pch.child_by_field_name("name");
                        if (!p_name.is_null()) {
                            mark_identifiers_handled(p_name, ctx);
                        }
                        treesitter::Node p_type = pch.child_by_field_name("type");
                        if (!p_type.is_null()) {
                            walk_node(p_type, ctx);
                        }
                    }
                }
            }
        }

        // Extract positional record parameters as properties/fields
        if (node.type() == "record_declaration") {
            treesitter::Node params = node.child_by_field_name("parameters");
            if (params.is_null()) {
                for (uint32_t i = 0; i < node.child_count(); ++i) {
                    if (node.child(i).type() == "parameter_list") {
                        params = node.child(i);
                        break;
                    }
                }
            }
            if (!params.is_null()) {
                for (uint32_t j = 0; j < params.child_count(); ++j) {
                    auto pch = params.child(j);
                    if (pch.type() == "parameter") {
                        walk_attributes(pch, ctx);
                        treesitter::Node p_name_node = pch.child_by_field_name("name");
                        treesitter::Node p_type_node = pch.child_by_field_name("type");
                        if (!p_type_node.is_null()) {
                            walk_node(p_type_node, ctx);
                        }
                        if (!p_name_node.is_null()) {
                            const std::string p_name = std::string(p_name_node.text(ctx.source));
                            mark_identifiers_handled(p_name_node, ctx);

                            const std::string p_qname = qname + "." + p_name;
                            const std::string p_sig = clean_signature(pch.text(ctx.source));

                            ctx.result.symbols.push_back(SymbolFact{
                                .name = p_name,
                                .qualified_name = p_qname,
                                .kind = NodeKind::field,
                                .range = p_name_node.byte_range(),
                                .display_range = p_name_node.display_range(),
                                .enclosing_scope = qname,
                                .signature = p_sig,
                            });

                            ctx.result.declarations.push_back(DeclarationFact{
                                .symbol_name = p_name,
                                .qualified_name = p_qname,
                                .kind = NodeKind::field,
                                .range = p_name_node.byte_range(),
                                .display_range = p_name_node.display_range(),
                                .enclosing_scope = qname,
                                .is_definition = true,
                            });
                        }
                    }
                }
            }
        }

        // Check base_list / record_base for inheritance and interface implementation.
        // NOTE: In C#, classes/records allow at most one base class, which must appear first.
        // As a syntactic heuristic: base_index > 0 or naming convention I[A-Z] is treated as
        // an interface implementation; otherwise as class inheritance. For structs, all base
        // types are interfaces (implementation); for interfaces, all base types are inherited.
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "base_list" || ch.type() == "record_base") {
                size_t base_index = 0;
                for (uint32_t j = 0; j < ch.child_count(); ++j) {
                    auto bch = ch.child(j);
                    if (bch.type() == ":" || bch.type() == ",") {
                        continue;
                    }

                    treesitter::Node target_type_node = bch;
                    if (bch.type() == "primary_constructor_base_type") {
                        treesitter::Node t_node = bch.child_by_field_name("type");
                        if (!t_node.is_null()) {
                            target_type_node = t_node;
                        }
                        // Walk argument expressions passed to base constructor
                        treesitter::Node args = bch.child_by_field_name("arguments");
                        if (args.is_null()) {
                            for (uint32_t k = 0; k < bch.child_count(); ++k) {
                                if (bch.child(k).type() == "argument_list") {
                                    args = bch.child(k);
                                    break;
                                }
                            }
                        }
                        if (!args.is_null()) {
                            if (!name.empty()) {
                                ctx.scope_stack.push_back(name);
                            }
                            walk_node(args, ctx);
                            if (!name.empty()) {
                                ctx.scope_stack.pop_back();
                            }
                        }
                    }

                    const std::string base_name = std::string(target_type_node.text(ctx.source));
                    mark_named_type_identifiers(target_type_node, ctx);

                    std::string unqualified = get_unqualified_name(base_name);
                    const bool is_interface_name =
                        (unqualified.size() >= 2 && unqualified[0] == 'I' &&
                         std::isupper(static_cast<unsigned char>(unqualified[1])));

                    worker::FactKind fact_kind = worker::FactKind::inheritance;
                    if (kind == NodeKind::interface_) {
                        fact_kind = worker::FactKind::inheritance;
                    } else if (kind == NodeKind::struct_) {
                        fact_kind = worker::FactKind::implementation;
                    } else if (base_index > 0 || is_interface_name) {
                        fact_kind = worker::FactKind::implementation;
                    }

                    std::vector<std::string> candidates = {base_name};
                    if (unqualified != base_name && !unqualified.empty()) {
                        candidates.push_back(unqualified);
                    }

                    ctx.result.occurrences.push_back(OccurrenceFact{
                        .kind = fact_kind,
                        .written_name = base_name,
                        .range = target_type_node.byte_range(),
                        .display_range = target_type_node.display_range(),
                        .enclosing_scope = qname,
                        .candidate_targets = std::move(candidates),
                    });
                    walk_type_arguments(target_type_node, ctx);
                    ++base_index;
                }
            }
        }
    }

    if (type_str == "enum_declaration") {
        if (!name.empty()) {
            ctx.scope_stack.push_back(name);
        }
        if (!body_node.is_null()) {
            for (uint32_t i = 0; i < body_node.child_count(); ++i) {
                auto ch = body_node.child(i);
                if (ch.type() == "enum_member_declaration") {
                    treesitter::Node m_name_node = ch.child_by_field_name("name");
                    if (!m_name_node.is_null()) {
                        const std::string m_name = std::string(m_name_node.text(ctx.source));
                        mark_identifiers_handled(m_name_node, ctx);

                        const auto scope = ctx.current_scope();
                        const std::string m_qname = scope ? *scope + "." + m_name : m_name;
                        walk_attributes(ch, ctx, m_qname);

                        ctx.result.symbols.push_back(SymbolFact{
                            .name = m_name,
                            .qualified_name = m_qname,
                            .kind = NodeKind::enum_member,
                            .range = m_name_node.byte_range(),
                            .display_range = m_name_node.display_range(),
                            .enclosing_scope = scope,
                            .signature = m_name,
                        });

                        ctx.result.declarations.push_back(DeclarationFact{
                            .symbol_name = m_name,
                            .qualified_name = m_qname,
                            .kind = NodeKind::enum_member,
                            .range = m_name_node.byte_range(),
                            .display_range = m_name_node.display_range(),
                            .enclosing_scope = scope,
                            .is_definition = true,
                        });
                    }

                    // Walk member value expression if present
                    treesitter::Node val_node = ch.child_by_field_name("value");
                    if (!val_node.is_null()) {
                        walk_node(val_node, ctx);
                    }
                }
            }
        }
        if (!name.empty()) {
            ctx.scope_stack.pop_back();
        }
        return;
    }

    // Generic constraints
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "type_parameter_constraints_clause") {
            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto cch = ch.child(j);
                if (cch.type() == "identifier") {
                    mark_identifiers_handled(cch, ctx);
                } else if (cch.type() == "type_parameter_constraint") {
                    treesitter::Node ctype = cch.child_by_field_name("type");
                    if (!ctype.is_null()) {
                        const std::string cname = std::string(ctype.text(ctx.source));
                        mark_identifiers_handled(ctype, ctx);
                        std::vector<std::string> candidates = {cname};
                        std::string unqual = get_unqualified_name(cname);
                        if (unqual != cname && !unqual.empty()) {
                            candidates.push_back(std::move(unqual));
                        }
                        ctx.result.occurrences.push_back(OccurrenceFact{
                            .kind = worker::FactKind::reference,
                            .written_name = cname,
                            .range = ctype.byte_range(),
                            .display_range = ctype.display_range(),
                            .enclosing_scope = ctx.current_scope(),
                            .candidate_targets = std::move(candidates),
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
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            if (node.child(i).type() == "arrow_expression_clause") {
                body_node = node.child(i);
                break;
            }
        }
    }

    std::string fn_name;
    if (node.type() == "operator_declaration") {
        treesitter::Node op_node = node.child_by_field_name("operator");
        if (!op_node.is_null()) {
            fn_name = "operator " + std::string(op_node.text(ctx.source));
            name_node = op_node;
        }
    } else if (node.type() == "conversion_operator_declaration") {
        treesitter::Node type_node = node.child_by_field_name("type");
        std::string conv_kind = "implicit";
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            if (node.child(i).type() == "explicit") {
                conv_kind = "explicit";
                break;
            }
        }
        if (!type_node.is_null()) {
            fn_name = conv_kind + " operator " + std::string(type_node.text(ctx.source));
            name_node = type_node;
        }
    } else if (!name_node.is_null()) {
        fn_name = std::string(name_node.text(ctx.source));
        if (node.type() == "destructor_declaration" && !fn_name.starts_with('~')) {
            fn_name = "~" + fn_name;
        }
    }

    std::string qname;
    if (!fn_name.empty()) {
        mark_identifiers_handled(name_node, ctx);

        const auto scope = ctx.current_scope();
        qname = scope ? *scope + "." + fn_name : fn_name;
        walk_attributes(node, ctx, qname);

        const std::string signature = clean_signature(node.text(ctx.source));

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

    // Return type of method / local function / operator
    treesitter::Node ret_type = node.child_by_field_name("returns");
    if (ret_type.is_null()) {
        ret_type = node.child_by_field_name("type");
    }
    if (!ret_type.is_null()) {
        walk_node(ret_type, ctx);
    }

    // Type parameters in generic method
    treesitter::Node type_params = node.child_by_field_name("type_parameters");
    if (!type_params.is_null()) {
        mark_identifiers_handled(type_params, ctx);
    }

    // Parameters
    treesitter::Node params_node = node.child_by_field_name("parameters");
    if (!params_node.is_null()) {
        for (uint32_t i = 0; i < params_node.child_count(); ++i) {
            auto ch = params_node.child(i);
            if (ch.type() == "parameter") {
                walk_attributes(ch, ctx);
                treesitter::Node p_name = ch.child_by_field_name("name");
                if (!p_name.is_null()) {
                    mark_identifiers_handled(p_name, ctx);
                }
                treesitter::Node p_type = ch.child_by_field_name("type");
                if (!p_type.is_null()) {
                    walk_node(p_type, ctx);
                }
            }
        }
    }

    // Constructor initializer (: base(...) or : this(...))
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "constructor_initializer") {
            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto init_ch = ch.child(j);
                if (init_ch.type() == "base" || init_ch.type() == "this") {
                    std::string ctor_call_name = std::string(init_ch.text(ctx.source));
                    ctx.result.occurrences.push_back(OccurrenceFact{
                        .kind = worker::FactKind::call,
                        .written_name = ctor_call_name,
                        .range = init_ch.byte_range(),
                        .display_range = init_ch.display_range(),
                        .enclosing_scope = qname,
                        .candidate_targets = {ctor_call_name},
                    });
                } else if (init_ch.type() == "argument_list") {
                    if (!fn_name.empty()) {
                        ctx.scope_stack.push_back(fn_name);
                    }
                    walk_node(init_ch, ctx);
                    if (!fn_name.empty()) {
                        ctx.scope_stack.pop_back();
                    }
                }
            }
        }
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

void process_property_or_indexer(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    std::string name;
    ByteRange range{};
    DisplayRange display_range{};

    if (!name_node.is_null()) {
        name = std::string(name_node.text(ctx.source));
        range = name_node.byte_range();
        display_range = name_node.display_range();
        mark_identifiers_handled(name_node, ctx);
    } else if (node.type() == "indexer_declaration") {
        name = "this";
        // Use the 'this' keyword token range for indexer declarations
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "this") {
                name_node = ch;
                range = ch.byte_range();
                display_range = ch.display_range();
                break;
            }
        }
        if (range.end == 0) {
            range = node.byte_range();
            display_range = node.display_range();
        }
    }

    if (!name.empty()) {
        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + name : name;
        walk_attributes(node, ctx, qname);

        const std::string signature = clean_signature(node.text(ctx.source));

        ctx.result.symbols.push_back(SymbolFact{
            .name = name,
            .qualified_name = qname,
            .kind = NodeKind::field,
            .range = range,
            .display_range = display_range,
            .enclosing_scope = scope,
            .signature = signature,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = name,
            .qualified_name = qname,
            .kind = NodeKind::field,
            .range = range,
            .display_range = display_range,
            .enclosing_scope = scope,
            .is_definition = true,
        });

        treesitter::Node type_node = node.child_by_field_name("type");
        if (!type_node.is_null()) {
            walk_node(type_node, ctx);
        }

        // Indexer parameters
        treesitter::Node params_node = node.child_by_field_name("parameters");
        if (!params_node.is_null()) {
            for (uint32_t i = 0; i < params_node.child_count(); ++i) {
                auto ch = params_node.child(i);
                if (ch.type() == "parameter") {
                    treesitter::Node p_name = ch.child_by_field_name("name");
                    if (!p_name.is_null()) {
                        mark_identifiers_handled(p_name, ctx);
                    }
                    treesitter::Node p_type = ch.child_by_field_name("type");
                    if (!p_type.is_null()) {
                        walk_node(p_type, ctx);
                    }
                }
            }
        }

        // Scoping: property accessors report the property's scope (e.g. Type.Prop)
        treesitter::Node accessors_node = node.child_by_field_name("accessors");
        if (!accessors_node.is_null()) {
            ctx.scope_stack.push_back(name);
            walk_node(accessors_node, ctx);
            ctx.scope_stack.pop_back();
        }

        treesitter::Node val_node = node.child_by_field_name("value");
        if (!val_node.is_null()) {
            ctx.scope_stack.push_back(name);
            walk_node(val_node, ctx);
            ctx.scope_stack.pop_back();
        }
    }
}

void process_field_declaration(treesitter::Node node, ASTContext& ctx) {
    const auto scope = ctx.current_scope();
    walk_attributes(node, ctx, scope);

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "variable_declaration") {
            treesitter::Node type_node = ch.child_by_field_name("type");
            if (!type_node.is_null()) {
                walk_node(type_node, ctx);
            }

            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto vch = ch.child(j);
                if (vch.type() == "variable_declarator") {
                    treesitter::Node name_node = vch.child_by_field_name("name");
                    if (!name_node.is_null()) {
                        const std::string name = std::string(name_node.text(ctx.source));
                        mark_identifiers_handled(name_node, ctx);

                        const std::string qname = scope ? *scope + "." + name : name;
                        const std::string signature = clean_signature(node.text(ctx.source));

                        ctx.result.symbols.push_back(SymbolFact{
                            .name = name,
                            .qualified_name = qname,
                            .kind = NodeKind::field,
                            .range = name_node.byte_range(),
                            .display_range = name_node.display_range(),
                            .enclosing_scope = scope,
                            .signature = signature,
                        });

                        ctx.result.declarations.push_back(DeclarationFact{
                            .symbol_name = name,
                            .qualified_name = qname,
                            .kind = NodeKind::field,
                            .range = name_node.byte_range(),
                            .display_range = name_node.display_range(),
                            .enclosing_scope = scope,
                            .is_definition = true,
                        });
                    }

                    treesitter::Node val_node = vch.child_by_field_name("value");
                    if (!val_node.is_null()) {
                        walk_node(val_node, ctx);
                    }
                }
            }
        }
    }
}

void process_invocation(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node func_node = node.child_by_field_name("function");
    if (!func_node.is_null()) {
        std::string full_call = std::string(func_node.text(ctx.source));
        std::string method_name;

        if (func_node.type() == "identifier") {
            method_name = full_call;
            mark_identifiers_handled(func_node, ctx);
        } else if (func_node.type() == "member_access_expression") {
            treesitter::Node name_node = func_node.child_by_field_name("name");
            if (!name_node.is_null()) {
                if (name_node.type() == "generic_name") {
                    for (uint32_t i = 0; i < name_node.child_count(); ++i) {
                        if (name_node.child(i).type() == "identifier") {
                            method_name = std::string(name_node.child(i).text(ctx.source));
                            mark_identifiers_handled(name_node.child(i), ctx);
                            break;
                        }
                    }
                    for (uint32_t i = 0; i < name_node.child_count(); ++i) {
                        auto gch = name_node.child(i);
                        if (gch.type() == "type_argument_list") {
                            walk_node(gch, ctx);
                        }
                    }
                } else {
                    method_name = std::string(name_node.text(ctx.source));
                    mark_identifiers_handled(name_node, ctx);
                }
            }
            treesitter::Node exp_node = func_node.child_by_field_name("expression");
            if (!exp_node.is_null()) {
                walk_node(exp_node, ctx);
            }
        } else if (func_node.type() == "member_binding_expression") {
            treesitter::Node name_node = func_node.child_by_field_name("name");
            if (!name_node.is_null()) {
                if (name_node.type() == "generic_name") {
                    for (uint32_t i = 0; i < name_node.child_count(); ++i) {
                        if (name_node.child(i).type() == "identifier") {
                            method_name = std::string(name_node.child(i).text(ctx.source));
                            mark_identifiers_handled(name_node.child(i), ctx);
                            break;
                        }
                    }
                    for (uint32_t i = 0; i < name_node.child_count(); ++i) {
                        auto gch = name_node.child(i);
                        if (gch.type() == "type_argument_list") {
                            walk_node(gch, ctx);
                        }
                    }
                } else {
                    method_name = std::string(name_node.text(ctx.source));
                    mark_identifiers_handled(name_node, ctx);
                }
            }
        } else if (func_node.type() == "generic_name") {
            for (uint32_t i = 0; i < func_node.child_count(); ++i) {
                if (func_node.child(i).type() == "identifier") {
                    method_name = std::string(func_node.child(i).text(ctx.source));
                    mark_identifiers_handled(func_node.child(i), ctx);
                    break;
                }
            }
            for (uint32_t i = 0; i < func_node.child_count(); ++i) {
                auto gch = func_node.child(i);
                if (gch.type() == "type_argument_list") {
                    walk_node(gch, ctx);
                }
            }
        } else if (func_node.type() == "conditional_access_expression") {
            treesitter::Node cond_node = func_node.child_by_field_name("condition");
            if (!cond_node.is_null()) {
                walk_node(cond_node, ctx);
            }
            for (uint32_t i = 0; i < func_node.child_count(); ++i) {
                auto ch = func_node.child(i);
                if (ch.type() == "member_binding_expression") {
                    treesitter::Node name_node = ch.child_by_field_name("name");
                    if (!name_node.is_null()) {
                        if (name_node.type() == "generic_name") {
                            for (uint32_t k = 0; k < name_node.child_count(); ++k) {
                                if (name_node.child(k).type() == "identifier") {
                                    method_name = std::string(name_node.child(k).text(ctx.source));
                                    mark_identifiers_handled(name_node.child(k), ctx);
                                    break;
                                }
                            }
                            for (uint32_t k = 0; k < name_node.child_count(); ++k) {
                                auto gch = name_node.child(k);
                                if (gch.type() == "type_argument_list") {
                                    walk_node(gch, ctx);
                                }
                            }
                        } else {
                            method_name = std::string(name_node.text(ctx.source));
                            mark_identifiers_handled(name_node, ctx);
                        }
                    }
                }
            }
        }

        if (!full_call.empty()) {
            std::vector<std::string> candidates = {full_call};
            if (!method_name.empty() && method_name != full_call) {
                candidates.push_back(method_name);
            }

            // Occurrence range matches the full written call text
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::call,
                .written_name = full_call,
                .range = func_node.byte_range(),
                .display_range = func_node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = std::move(candidates),
            });
        }
    }

    treesitter::Node args_node = node.child_by_field_name("arguments");
    if (!args_node.is_null()) {
        walk_node(args_node, ctx);
    }
}

void process_object_creation(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        const std::string type_name = std::string(type_node.text(ctx.source));
        mark_named_type_identifiers(type_node, ctx);

        std::vector<std::string> candidates = {type_name};
        std::string unqual = get_unqualified_name(type_name);
        if (unqual != type_name && !unqual.empty()) {
            candidates.push_back(std::move(unqual));
        }

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::call,
            .written_name = type_name,
            .range = type_node.byte_range(),
            .display_range = type_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = std::move(candidates),
        });
        walk_type_arguments(type_node, ctx);
    }

    treesitter::Node args_node = node.child_by_field_name("arguments");
    if (!args_node.is_null()) {
        walk_node(args_node, ctx);
    }

    treesitter::Node init_node = node.child_by_field_name("initializer");
    if (!init_node.is_null()) {
        walk_node(init_node, ctx);
    }
}

void walk_node(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null()) {
        return;
    }
    if (ctx.stop_token.stop_requested()) {
        return;
    }

    const auto type = node.type();

    if (type == "using_directive" || type == "global_using_directive") {
        process_using_directive(node, ctx);
        return;
    }

    if (type == "namespace_declaration" || type == "file_scoped_namespace_declaration") {
        process_namespace(node, ctx);
        return;
    }

    if (type == "class_declaration" || type == "struct_declaration" ||
        type == "interface_declaration" || type == "enum_declaration" ||
        type == "record_declaration" || type == "delegate_declaration") {
        process_type_declaration(node, ctx);
        return;
    }

    if (type == "method_declaration" || type == "constructor_declaration" ||
        type == "destructor_declaration" || type == "operator_declaration" ||
        type == "conversion_operator_declaration" || type == "local_function_statement") {
        process_method(node, ctx);
        return;
    }

    if (type == "property_declaration" || type == "indexer_declaration" ||
        type == "event_declaration") {
        process_property_or_indexer(node, ctx);
        return;
    }

    if (type == "field_declaration" || type == "event_field_declaration") {
        process_field_declaration(node, ctx);
        return;
    }

    if (type == "conditional_access_expression") {
        treesitter::Node cond_node = node.child_by_field_name("condition");
        if (!cond_node.is_null()) {
            walk_node(cond_node, ctx);
        }
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch != cond_node && ch.type() != "?") {
                walk_node(ch, ctx);
            }
        }
        return;
    }

    if (type == "invocation_expression") {
        process_invocation(node, ctx);
        return;
    }

    if (type == "object_creation_expression") {
        process_object_creation(node, ctx);
        return;
    }

    if (type == "qualified_name" || type == "alias_qualified_name" || type == "generic_name") {
        process_named_type(node, ctx);
        return;
    }

    if (type == "attribute") {
        process_attribute(node, ctx);
        return;
    }

    if (type == "identifier" || type == "type_identifier") {
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
        .functions = CapabilityStatus::supported,
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
        .templates = CapabilityStatus::supported,
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

std::string_view CSharpAdapter::highlighting_query() noexcept {
    static constexpr std::string_view kHighlightQuery = R"(
;; Keywords
[
  (modifier)
  "this"
  (implicit_type)
] @keyword

[
  "add"
  "alias"
  "as"
  "base"
  "break"
  "case"
  "catch"
  "checked"
  "class"
  "continue"
  "default"
  "delegate"
  "do"
  "else"
  "enum"
  "event"
  "explicit"
  "extern"
  "finally"
  "for"
  "foreach"
  "global"
  "goto"
  "if"
  "implicit"
  "interface"
  "is"
  "lock"
  "namespace"
  "notnull"
  "operator"
  "params"
  "return"
  "remove"
  "sizeof"
  "stackalloc"
  "static"
  "struct"
  "switch"
  "throw"
  "try"
  "typeof"
  "unchecked"
  "using"
  "while"
  "new"
  "await"
  "in"
  "yield"
  "get"
  "set"
  "when"
  "out"
  "ref"
  "from"
  "where"
  "select"
  "record"
  "init"
  "with"
  "let"
] @keyword

;; Literals
[
  (real_literal)
  (integer_literal)
] @number

[
  (character_literal)
  (string_literal)
  (raw_string_literal)
  (verbatim_string_literal)
  (interpolated_string_expression)
  (interpolation_start)
  (interpolation_quote)
] @string

[
  (boolean_literal)
  (null_literal)
] @keyword

;; Comments
(comment) @comment

;; Types
(predefined_type) @type
(generic_name (identifier) @type)
(type_parameter name: (identifier) @typeParameter)

(class_declaration name: (identifier) @class.definition)
(interface_declaration name: (identifier) @interface.definition)
(struct_declaration name: (identifier) @struct.definition)
(enum_declaration name: (identifier) @enum.definition)
(record_declaration name: (identifier) @class.definition)

;; Methods
(method_declaration name: (identifier) @method.definition)
(constructor_declaration name: (identifier) @method.definition)
(destructor_declaration name: (identifier) @method.definition)
(local_function_statement name: (identifier) @function.definition)

(invocation_expression function: (identifier) @method)
(invocation_expression function: (member_access_expression name: (identifier) @method))
(invocation_expression function: (member_access_expression name: (generic_name (identifier) @method)))

;; Properties & Fields
(property_declaration name: (identifier) @property.definition)
(enum_member_declaration name: (identifier) @enumMember.definition)
(variable_declarator name: (identifier) @variable.definition)
(parameter name: (identifier) @parameter)

;; Attributes
(attribute name: (identifier) @decorator)

;; Operators
[
  "+"
  "-"
  "*"
  "/"
  "%"
  "="
  "=="
  "!="
  "<"
  "<="
  ">"
  ">="
  "&&"
  "||"
  "!"
  "++"
  "--"
  "+="
  "-="
  "*="
  "/="
  "%="
  "=>"
  "??"
  "??="
  "?"
  ":"
] @operator
)";
    return kHighlightQuery;
}

Result<std::vector<HighlightToken>> CSharpAdapter::highlight(std::string_view source,
                                                             const treesitter::Tree& tree) {
    const auto* ts_lang = treesitter::grammar_for_language(Language::csharp);
    if (ts_lang == nullptr) {
        return unexpected_result<std::vector<HighlightToken>>(ErrorCode::invalid_argument,
                                                              "C# grammar not available");
    }

    static std::string s_query_error;
    static const auto s_query = []() -> std::optional<treesitter::Query> {
        const auto* lang = treesitter::grammar_for_language(Language::csharp);
        if (lang == nullptr) {
            s_query_error = "null grammar";
            return std::nullopt;
        }
        auto res = treesitter::Query::create(lang, highlighting_query());
        if (!res) {
            s_query_error = res.error().message;
            return std::nullopt;
        }
        return std::move(*res);
    }();

    if (!s_query.has_value()) {
        return unexpected_result<std::vector<HighlightToken>>(
            ErrorCode::failed, "Failed to create highlight query: " + s_query_error);
    }

    const auto& query = *s_query;
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

Result<std::vector<HighlightToken>> CSharpAdapter::highlight(std::string_view source) {
    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(Language::csharp);
    if (ts_lang == nullptr) {
        return unexpected_result<std::vector<HighlightToken>>(ErrorCode::invalid_argument,
                                                              "C# grammar not available");
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
