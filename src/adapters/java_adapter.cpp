#include "codelenses/adapters/java_adapter.hpp"

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

struct ASTContext {
    std::string_view source;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    const std::stop_token& stop_token;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    AdapterResult& result;
    std::optional<std::string> package_prefix;
    std::vector<std::string> scope_stack;
    std::unordered_set<uint32_t> handled_identifier_byte_starts;
    uint32_t anon_class_counter{0};

    [[nodiscard]] std::optional<std::string> current_scope() const {
        if (!package_prefix && scope_stack.empty()) {
            return std::nullopt;
        }
        std::string full;
        if (package_prefix) {
            full = *package_prefix;
        }
        for (const auto& s : scope_stack) {
            if (!full.empty()) {
                full += ".";
            }
            full += s;
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

std::string clean_signature(std::string_view raw) {
    std::string result;
    bool in_space = false;
    for (char c : raw) {
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!in_space && !result.empty()) {
                result += ' ';
                in_space = true;
            }
        } else {
            result += c;
            in_space = false;
        }
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result;
}

std::string get_unqualified_name(std::string_view name) {
    auto generic_pos = name.find('<');
    if (generic_pos != std::string_view::npos) {
        name = name.substr(0, generic_pos);
    }
    auto dot_pos = name.rfind('.');
    if (dot_pos != std::string_view::npos) {
        name = name.substr(dot_pos + 1);
    }
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
        name.remove_prefix(1);
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.remove_suffix(1);
    }
    return std::string(name);
}

std::string extract_signature(treesitter::Node node, std::string_view source) {
    uint32_t start_byte = node.start_byte();
    uint32_t end_byte = node.end_byte();

    treesitter::Node body_node = node.child_by_field_name("body");
    if (body_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            const auto ct = node.child(i).type();
            if (ct == "block" || ct == "constructor_body" || ct == "class_body" ||
                ct == "interface_body" || ct == "enum_body" || ct == "annotation_type_body") {
                body_node = node.child(i);
                break;
            }
        }
    }

    if (!body_node.is_null()) {
        end_byte = body_node.start_byte();
    } else {
        if (end_byte > start_byte && end_byte <= source.size() && source[end_byte - 1] == ';') {
            --end_byte;
        }
    }

    // Skip leading annotations in signature
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "modifiers") {
            bool found_non_annotation = false;
            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto mch = ch.child(j);
                if (mch.type() != "annotation" && mch.type() != "marker_annotation") {
                    start_byte = mch.start_byte();
                    found_non_annotation = true;
                    break;
                }
            }
            if (!found_non_annotation) {
                for (uint32_t k = i + 1; k < node.child_count(); ++k) {
                    auto next_ch = node.child(k);
                    if (next_ch.type() != "annotation" && next_ch.type() != "marker_annotation" &&
                        next_ch.type() != "comment") {
                        start_byte = next_ch.start_byte();
                        break;
                    }
                }
            }
            break;
        }
        if (ch.type() != "annotation" && ch.type() != "marker_annotation" &&
            ch.type() != "comment") {
            start_byte = ch.start_byte();
            break;
        }
    }

    if (start_byte < end_byte && end_byte <= source.size()) {
        return clean_signature(source.substr(start_byte, end_byte - start_byte));
    }
    return clean_signature(node.text(source));
}

void walk_node(treesitter::Node node, ASTContext& ctx);

void process_annotation(treesitter::Node node, ASTContext& ctx,
                        std::optional<std::string> scope_override = std::nullopt) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (name_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "identifier" || ch.type() == "scoped_identifier") {
                name_node = ch;
                break;
            }
        }
    }

    if (!name_node.is_null()) {
        const std::string attr_name = std::string(name_node.text(ctx.source));
        mark_identifiers_handled(name_node, ctx);

        std::string unqual = get_unqualified_name(attr_name);
        std::vector<std::string> candidates = {attr_name};
        if (unqual != attr_name && !unqual.empty()) {
            candidates.push_back(unqual);
        }

        std::string metadata;
        if (unqual == "Override") {
            metadata = R"({"is_annotation":true,"is_override":true})";
        } else {
            metadata = R"({"is_annotation":true})";
        }

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::reference,
            .written_name = attr_name,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope_override.has_value() ? scope_override : ctx.current_scope(),
            .candidate_targets = std::move(candidates),
            .confidence = 1.0,
            .metadata_json = std::move(metadata),
        });
    }

    treesitter::Node args_node = node.child_by_field_name("arguments");
    if (!args_node.is_null()) {
        walk_node(args_node, ctx);
    }
}

void walk_modifiers_for_annotations(treesitter::Node node, ASTContext& ctx,
                                    std::optional<std::string> scope_override) {
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "modifiers") {
            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto mch = ch.child(j);
                if (mch.type() == "annotation" || mch.type() == "marker_annotation") {
                    process_annotation(mch, ctx, scope_override);
                }
            }
        } else if (ch.type() == "annotation" || ch.type() == "marker_annotation") {
            process_annotation(ch, ctx, scope_override);
        }
    }
}

void process_package_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node;
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "scoped_identifier" || ch.type() == "identifier") {
            name_node = ch;
            break;
        }
    }

    if (!name_node.is_null()) {
        const std::string pkg_name = std::string(name_node.text(ctx.source));
        mark_identifiers_handled(name_node, ctx);

        ctx.result.symbols.push_back(SymbolFact{
            .name = pkg_name,
            .qualified_name = pkg_name,
            .kind = NodeKind::package,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = std::nullopt,
            .signature = "package " + pkg_name,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = pkg_name,
            .qualified_name = pkg_name,
            .kind = NodeKind::package,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = std::nullopt,
            .is_definition = true,
        });

        ctx.package_prefix = pkg_name;
    }
}

void process_import_declaration(treesitter::Node node, ASTContext& ctx) {
    mark_identifiers_handled(node, ctx);

    treesitter::Node first_ch{};
    treesitter::Node last_ch{};
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() != "import" && ch.type() != "static" && ch.type() != ";") {
            if (first_ch.is_null()) {
                first_ch = ch;
            }
            last_ch = ch;
        }
    }

    std::string written_name;
    ByteRange range = node.byte_range();
    DisplayRange display_range = node.display_range();

    if (!first_ch.is_null() && !last_ch.is_null()) {
        const auto sb = first_ch.start_byte();
        const auto eb = last_ch.end_byte();
        if (sb <= eb && eb <= ctx.source.size()) {
            written_name = std::string(ctx.source.substr(sb, eb - sb));
            range = ByteRange{sb, eb};
            const auto sp = first_ch.start_position();
            const auto ep = last_ch.end_position();
            display_range = DisplayRange{sp.line, sp.column, ep.line, ep.column};
        }
    }

    if (written_name.empty()) {
        std::string_view full = node.text(ctx.source);
        auto import_pos = full.find("import");
        size_t start = (import_pos != std::string_view::npos) ? (import_pos + 6) : 0;
        while (start < full.size() && (full[start] == ' ' || full[start] == '\t')) {
            ++start;
        }
        if (full.substr(start).starts_with("static")) {
            start += 6;
            while (start < full.size() && (full[start] == ' ' || full[start] == '\t')) {
                ++start;
            }
        }
        size_t end = full.size();
        while (end > start && (full[end - 1] == ' ' || full[end - 1] == '\t' ||
                               full[end - 1] == '\r' || full[end - 1] == '\n')) {
            --end;
        }
        if (end > start && full[end - 1] == ';') {
            --end;
            while (end > start && (full[end - 1] == ' ' || full[end - 1] == '\t')) {
                --end;
            }
        }
        written_name = std::string(full.substr(start, end - start));
    }

    std::vector<std::string> candidates = {written_name};
    if (written_name.ends_with(".*")) {
        std::string prefix = written_name.substr(0, written_name.size() - 2);
        candidates.push_back(std::move(prefix));
    } else {
        std::string unqual = get_unqualified_name(written_name);
        if (unqual != written_name && !unqual.empty()) {
            candidates.push_back(std::move(unqual));
        }
    }

    ctx.result.occurrences.push_back(OccurrenceFact{
        .kind = worker::FactKind::import,
        .written_name = written_name,
        .range = range,
        .display_range = display_range,
        .enclosing_scope = ctx.current_scope(),
        .candidate_targets = std::move(candidates),
    });
}

void process_type_parameters(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null()) {
        return;
    }
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "type_parameter") {
            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto pch = ch.child(j);
                if (pch.type() == "annotation" || pch.type() == "marker_annotation") {
                    process_annotation(pch, ctx);
                } else if (pch.type() == "type_identifier" || pch.type() == "identifier") {
                    mark_identifiers_handled(pch, ctx);
                } else if (pch.type() == "type_bound") {
                    walk_node(pch, ctx);
                }
            }
        }
    }
}

void process_type_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (name_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            if (node.child(i).type() == "identifier") {
                name_node = node.child(i);
                break;
            }
        }
    }

    const auto type_str = node.type();
    NodeKind kind = NodeKind::class_;
    if (type_str == "interface_declaration" || type_str == "annotation_type_declaration") {
        kind = NodeKind::interface_;
    } else if (type_str == "enum_declaration") {
        kind = NodeKind::enum_;
    } else if (type_str == "record_declaration") {
        kind = NodeKind::class_;
    } else if (type_str == "class_declaration") {
        kind = NodeKind::class_;
    }

    std::string name;
    std::string qname;
    if (!name_node.is_null()) {
        name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        qname = scope ? *scope + "." + name : name;
        const std::string signature = extract_signature(node, ctx.source);

        walk_modifiers_for_annotations(node, ctx, qname);

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

        // Type parameters (generics)
        treesitter::Node type_params = node.child_by_field_name("type_parameters");
        if (!type_params.is_null()) {
            process_type_parameters(type_params, ctx);
        }

        // Superclass for class_declaration
        if (type_str == "class_declaration") {
            treesitter::Node superclass_node = node.child_by_field_name("superclass");
            if (superclass_node.is_null()) {
                for (uint32_t i = 0; i < node.child_count(); ++i) {
                    if (node.child(i).type() == "superclass") {
                        superclass_node = node.child(i);
                        break;
                    }
                }
            }
            if (!superclass_node.is_null()) {
                for (uint32_t i = 0; i < superclass_node.child_count(); ++i) {
                    auto ch = superclass_node.child(i);
                    if (ch.type() != "extends" && ch.is_named()) {
                        const std::string base_name = std::string(ch.text(ctx.source));
                        mark_identifiers_handled(ch, ctx);

                        std::string unqual = get_unqualified_name(base_name);
                        std::vector<std::string> candidates = {base_name};
                        if (unqual != base_name && !unqual.empty()) {
                            candidates.push_back(unqual);
                        }

                        ctx.result.occurrences.push_back(OccurrenceFact{
                            .kind = worker::FactKind::inheritance,
                            .written_name = base_name,
                            .range = ch.byte_range(),
                            .display_range = ch.display_range(),
                            .enclosing_scope = qname,
                            .candidate_targets = std::move(candidates),
                        });
                    }
                }
            }
        }

        // Super interfaces for class_declaration, record_declaration, enum_declaration
        if (type_str == "class_declaration" || type_str == "record_declaration" ||
            type_str == "enum_declaration") {
            treesitter::Node ifaces_node = node.child_by_field_name("interfaces");
            if (ifaces_node.is_null()) {
                for (uint32_t i = 0; i < node.child_count(); ++i) {
                    if (node.child(i).type() == "super_interfaces") {
                        ifaces_node = node.child(i);
                        break;
                    }
                }
            }
            if (!ifaces_node.is_null()) {
                treesitter::Node type_list_node = ifaces_node;
                for (uint32_t i = 0; i < ifaces_node.child_count(); ++i) {
                    if (ifaces_node.child(i).type() == "type_list") {
                        type_list_node = ifaces_node.child(i);
                        break;
                    }
                }
                for (uint32_t i = 0; i < type_list_node.child_count(); ++i) {
                    auto ch = type_list_node.child(i);
                    if (ch.type() != "implements" && ch.type() != "," && ch.is_named()) {
                        const std::string iface_name = std::string(ch.text(ctx.source));
                        mark_identifiers_handled(ch, ctx);

                        std::string unqual = get_unqualified_name(iface_name);
                        std::vector<std::string> candidates = {iface_name};
                        if (unqual != iface_name && !unqual.empty()) {
                            candidates.push_back(unqual);
                        }

                        ctx.result.occurrences.push_back(OccurrenceFact{
                            .kind = worker::FactKind::implementation,
                            .written_name = iface_name,
                            .range = ch.byte_range(),
                            .display_range = ch.display_range(),
                            .enclosing_scope = qname,
                            .candidate_targets = std::move(candidates),
                        });
                    }
                }
            }
        }

        // Extends interfaces for interface_declaration
        if (type_str == "interface_declaration") {
            treesitter::Node extends_node = node.child_by_field_name("extends_interfaces");
            if (extends_node.is_null()) {
                for (uint32_t i = 0; i < node.child_count(); ++i) {
                    if (node.child(i).type() == "extends_interfaces") {
                        extends_node = node.child(i);
                        break;
                    }
                }
            }
            if (!extends_node.is_null()) {
                treesitter::Node type_list_node = extends_node;
                for (uint32_t i = 0; i < extends_node.child_count(); ++i) {
                    if (extends_node.child(i).type() == "type_list") {
                        type_list_node = extends_node.child(i);
                        break;
                    }
                }
                for (uint32_t i = 0; i < type_list_node.child_count(); ++i) {
                    auto ch = type_list_node.child(i);
                    if (ch.type() != "extends" && ch.type() != "," && ch.is_named()) {
                        const std::string base_name = std::string(ch.text(ctx.source));
                        mark_identifiers_handled(ch, ctx);

                        std::string unqual = get_unqualified_name(base_name);
                        std::vector<std::string> candidates = {base_name};
                        if (unqual != base_name && !unqual.empty()) {
                            candidates.push_back(unqual);
                        }

                        ctx.result.occurrences.push_back(OccurrenceFact{
                            .kind = worker::FactKind::inheritance,
                            .written_name = base_name,
                            .range = ch.byte_range(),
                            .display_range = ch.display_range(),
                            .enclosing_scope = qname,
                            .candidate_targets = std::move(candidates),
                        });
                    }
                }
            }
        }

        // Permits for sealed class_declaration or interface_declaration
        treesitter::Node permits_node = node.child_by_field_name("permits");
        if (permits_node.is_null()) {
            for (uint32_t i = 0; i < node.child_count(); ++i) {
                if (node.child(i).type() == "permits") {
                    permits_node = node.child(i);
                    break;
                }
            }
        }
        if (!permits_node.is_null()) {
            treesitter::Node type_list_node = permits_node;
            for (uint32_t i = 0; i < permits_node.child_count(); ++i) {
                if (permits_node.child(i).type() == "type_list") {
                    type_list_node = permits_node.child(i);
                    break;
                }
            }
            for (uint32_t i = 0; i < type_list_node.child_count(); ++i) {
                auto ch = type_list_node.child(i);
                if (ch.type() != "permits" && ch.type() != "," && ch.is_named()) {
                    const std::string perm_name = std::string(ch.text(ctx.source));
                    mark_identifiers_handled(ch, ctx);

                    std::string unqual = get_unqualified_name(perm_name);
                    std::vector<std::string> candidates = {perm_name};
                    if (unqual != perm_name && !unqual.empty()) {
                        candidates.push_back(unqual);
                    }

                    ctx.result.occurrences.push_back(OccurrenceFact{
                        .kind = worker::FactKind::reference,
                        .written_name = perm_name,
                        .range = ch.byte_range(),
                        .display_range = ch.display_range(),
                        .enclosing_scope = qname,
                        .candidate_targets = std::move(candidates),
                    });
                }
            }
        }

        // Record parameters
        if (type_str == "record_declaration") {
            treesitter::Node params_node = node.child_by_field_name("parameters");
            if (!params_node.is_null()) {
                for (uint32_t i = 0; i < params_node.child_count(); ++i) {
                    auto ch = params_node.child(i);
                    if (ch.type() == "formal_parameter" || ch.type() == "spread_parameter") {
                        treesitter::Node p_name = ch.child_by_field_name("name");
                        if (p_name.is_null()) {
                            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                                auto cj = ch.child(j);
                                if (cj.type() == "identifier") {
                                    p_name = cj;
                                    break;
                                }
                                if (cj.type() == "variable_declarator") {
                                    p_name = cj.child_by_field_name("name");
                                    if (p_name.is_null()) {
                                        for (uint32_t k = 0; k < cj.child_count(); ++k) {
                                            if (cj.child(k).type() == "identifier") {
                                                p_name = cj.child(k);
                                                break;
                                            }
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                        if (!p_name.is_null()) {
                            const std::string field_name = std::string(p_name.text(ctx.source));
                            ctx.handled_identifier_byte_starts.insert(p_name.start_byte());
                            const std::string field_qname = qname + "." + field_name;

                            ctx.result.symbols.push_back(SymbolFact{
                                .name = field_name,
                                .qualified_name = field_qname,
                                .kind = NodeKind::field,
                                .range = p_name.byte_range(),
                                .display_range = p_name.display_range(),
                                .enclosing_scope = qname,
                                .signature = field_name,
                            });

                            ctx.result.declarations.push_back(DeclarationFact{
                                .symbol_name = field_name,
                                .qualified_name = field_qname,
                                .kind = NodeKind::field,
                                .range = p_name.byte_range(),
                                .display_range = p_name.display_range(),
                                .enclosing_scope = qname,
                                .is_definition = true,
                            });
                        }
                        treesitter::Node p_type = ch.child_by_field_name("type");
                        if (!p_type.is_null()) {
                            walk_node(p_type, ctx);
                        } else {
                            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                                auto cj = ch.child(j);
                                if (cj.type() != "modifiers" && cj.type() != "..." &&
                                    cj.type() != "variable_declarator" &&
                                    cj.type() != "annotation") {
                                    walk_node(cj, ctx);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Walk body
    treesitter::Node body_node = node.child_by_field_name("body");
    if (body_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            const auto ct = node.child(i).type();
            if (ct == "class_body" || ct == "interface_body" || ct == "enum_body" ||
                ct == "annotation_type_body") {
                body_node = node.child(i);
                break;
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

void process_enum_constant(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (name_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            if (node.child(i).type() == "identifier") {
                name_node = node.child(i);
                break;
            }
        }
    }

    if (!name_node.is_null()) {
        const std::string const_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + const_name : const_name;

        walk_modifiers_for_annotations(node, ctx, qname);

        ctx.result.symbols.push_back(SymbolFact{
            .name = const_name,
            .qualified_name = qname,
            .kind = NodeKind::enum_member,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = const_name,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = const_name,
            .qualified_name = qname,
            .kind = NodeKind::enum_member,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });

        treesitter::Node args_node = node.child_by_field_name("arguments");
        if (!args_node.is_null()) {
            walk_node(args_node, ctx);
        }

        treesitter::Node body_node = node.child_by_field_name("body");
        if (!body_node.is_null()) {
            ctx.scope_stack.push_back(const_name);
            walk_node(body_node, ctx);
            ctx.scope_stack.pop_back();
        }
    }
}

void process_method(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");
    if (body_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            const auto ct = node.child(i).type();
            if (ct == "block" || ct == "constructor_body") {
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
        const std::string signature = extract_signature(node, ctx.source);

        walk_modifiers_for_annotations(node, ctx, qname);

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

    // Type parameters (generics on method)
    treesitter::Node type_params_node = node.child_by_field_name("type_parameters");
    if (!type_params_node.is_null()) {
        process_type_parameters(type_params_node, ctx);
    }

    // Parameters
    treesitter::Node params_node = node.child_by_field_name("parameters");
    if (!params_node.is_null()) {
        for (uint32_t i = 0; i < params_node.child_count(); ++i) {
            auto ch = params_node.child(i);
            if (ch.type() == "formal_parameter" || ch.type() == "spread_parameter") {
                treesitter::Node p_name = ch.child_by_field_name("name");
                if (p_name.is_null()) {
                    for (uint32_t j = 0; j < ch.child_count(); ++j) {
                        if (ch.child(j).type() == "identifier") {
                            p_name = ch.child(j);
                            break;
                        }
                    }
                }
                if (!p_name.is_null()) {
                    mark_identifiers_handled(p_name, ctx);
                }
                treesitter::Node p_type = ch.child_by_field_name("type");
                if (!p_type.is_null()) {
                    walk_node(p_type, ctx);
                }
            } else {
                walk_node(ch, ctx);
            }
        }
    }

    // Return type
    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }

    // Throws
    treesitter::Node throws_node = node.child_by_field_name("throws");
    if (throws_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            if (node.child(i).type() == "throws") {
                throws_node = node.child(i);
                break;
            }
        }
    }
    if (!throws_node.is_null()) {
        walk_node(throws_node, ctx);
    }

    // Default value for annotation_type_element_declaration
    treesitter::Node val_node = node.child_by_field_name("value");
    if (!val_node.is_null()) {
        walk_node(val_node, ctx);
    }

    // Body
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
    walk_modifiers_for_annotations(node, ctx, ctx.current_scope());

    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "variable_declarator") {
            treesitter::Node name_node = ch.child_by_field_name("name");
            if (name_node.is_null()) {
                for (uint32_t j = 0; j < ch.child_count(); ++j) {
                    if (ch.child(j).type() == "identifier") {
                        name_node = ch.child(j);
                        break;
                    }
                }
            }

            if (!name_node.is_null()) {
                const std::string name = std::string(name_node.text(ctx.source));
                ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

                const auto scope = ctx.current_scope();
                const std::string qname = scope ? *scope + "." + name : name;
                const std::string signature = extract_signature(node, ctx.source);

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

            treesitter::Node val_node = ch.child_by_field_name("value");
            if (!val_node.is_null()) {
                walk_node(val_node, ctx);
            }
        }
    }
}

void process_local_variable_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "variable_declarator") {
            treesitter::Node name_node = ch.child_by_field_name("name");
            if (!name_node.is_null()) {
                mark_identifiers_handled(name_node, ctx);
            }
            treesitter::Node val_node = ch.child_by_field_name("value");
            if (!val_node.is_null()) {
                walk_node(val_node, ctx);
            }
        }
    }
}

void process_enhanced_for_statement(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }

    treesitter::Node name_node = node.child_by_field_name("name");
    if (!name_node.is_null()) {
        mark_identifiers_handled(name_node, ctx);
    }

    treesitter::Node val_node = node.child_by_field_name("value");
    if (!val_node.is_null()) {
        walk_node(val_node, ctx);
    }

    treesitter::Node body_node = node.child_by_field_name("body");
    if (!body_node.is_null()) {
        walk_node(body_node, ctx);
    }
}

void process_catch_clause(treesitter::Node node, ASTContext& ctx) {
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "catch_formal_parameter") {
            treesitter::Node name_node = ch.child_by_field_name("name");
            if (!name_node.is_null()) {
                mark_identifiers_handled(name_node, ctx);
            }
            treesitter::Node type_node = ch.child_by_field_name("type");
            if (!type_node.is_null()) {
                walk_node(type_node, ctx);
            }
        }
    }

    treesitter::Node body_node = node.child_by_field_name("body");
    if (!body_node.is_null()) {
        walk_node(body_node, ctx);
    }
}

void process_method_invocation(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node object_node = node.child_by_field_name("object");

    if (!name_node.is_null()) {
        const std::string method_name = std::string(name_node.text(ctx.source));
        mark_identifiers_handled(name_node, ctx);

        if (!object_node.is_null()) {
            const uint32_t sb = object_node.start_byte();
            const uint32_t eb = name_node.end_byte();
            if (sb <= eb && eb <= ctx.source.size()) {
                const std::string full_call = std::string(ctx.source.substr(sb, eb - sb));
                const auto sp = object_node.start_position();
                const auto ep = name_node.end_position();

                std::vector<std::string> candidates = {full_call};
                if (method_name != full_call) {
                    candidates.push_back(method_name);
                }

                ctx.result.occurrences.push_back(OccurrenceFact{
                    .kind = worker::FactKind::call,
                    .written_name = full_call,
                    .range = ByteRange{sb, eb},
                    .display_range = DisplayRange{sp.line, sp.column, ep.line, ep.column},
                    .enclosing_scope = ctx.current_scope(),
                    .candidate_targets = std::move(candidates),
                });
            }
        } else {
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::call,
                .written_name = method_name,
                .range = name_node.byte_range(),
                .display_range = name_node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {method_name},
            });
        }
    }

    // NOTE (Finding 8): Walking object_node emits nested occurrences for chained calls
    // (e.g., item.getId().equals(...) emits call "item.getId().equals", call "item.getId", and ref
    // "item"). This design conforms to the CodeLenses cross-language convention (Python/CSharp
    // adapters) enabling callers and references to resolve at both expression and receiver levels.
    if (!object_node.is_null()) {
        walk_node(object_node, ctx);
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

void process_object_creation(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node type_node = node.child_by_field_name("type");
    std::string type_name;
    std::string unqual;
    if (!type_node.is_null()) {
        type_name = std::string(type_node.text(ctx.source));
        mark_identifiers_handled(type_node, ctx);

        unqual = get_unqualified_name(type_name);
        std::vector<std::string> candidates = {type_name};
        if (unqual != type_name && !unqual.empty()) {
            candidates.push_back(unqual);
        }

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::call,
            .written_name = type_name,
            .range = type_node.byte_range(),
            .display_range = type_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = std::move(candidates),
        });
    }

    treesitter::Node type_args_node = node.child_by_field_name("type_arguments");
    if (!type_args_node.is_null()) {
        walk_node(type_args_node, ctx);
    }

    treesitter::Node args_node = node.child_by_field_name("arguments");
    if (!args_node.is_null()) {
        walk_node(args_node, ctx);
    }

    treesitter::Node class_body_node{};
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "class_body") {
            class_body_node = ch;
            break;
        }
    }

    if (!class_body_node.is_null()) {
        ++ctx.anon_class_counter;
        std::string base_id = !unqual.empty() ? unqual : "class";
        std::string anon_name = "$anon_" + base_id + "_" + std::to_string(ctx.anon_class_counter);
        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + anon_name : anon_name;

        uint32_t sig_end = class_body_node.start_byte();
        std::string anon_signature =
            clean_signature(ctx.source.substr(node.start_byte(), sig_end - node.start_byte()));

        ctx.result.symbols.push_back(SymbolFact{
            .name = anon_name,
            .qualified_name = qname,
            .kind = NodeKind::class_,
            .range = node.byte_range(),
            .display_range = node.display_range(),
            .enclosing_scope = scope,
            .signature = std::move(anon_signature),
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = anon_name,
            .qualified_name = qname,
            .kind = NodeKind::class_,
            .range = node.byte_range(),
            .display_range = node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });

        if (!type_node.is_null() && !type_name.empty()) {
            std::vector<std::string> candidates = {type_name};
            if (unqual != type_name && !unqual.empty()) {
                candidates.push_back(unqual);
            }
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::implementation,
                .written_name = type_name,
                .range = type_node.byte_range(),
                .display_range = type_node.display_range(),
                .enclosing_scope = qname,
                .candidate_targets = std::move(candidates),
            });
        }

        ctx.scope_stack.push_back(anon_name);
        walk_node(class_body_node, ctx);
        ctx.scope_stack.pop_back();
    }
}

void process_explicit_constructor_invocation(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node ctor_node = node.child_by_field_name("constructor");
    if (!ctor_node.is_null()) {
        const std::string name = std::string(ctor_node.text(ctx.source));
        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::call,
            .written_name = name,
            .range = ctor_node.byte_range(),
            .display_range = ctor_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = {name},
        });
    }
    treesitter::Node args = node.child_by_field_name("arguments");
    if (!args.is_null()) {
        walk_node(args, ctx);
    }
}

void process_field_access(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node field_node = node.child_by_field_name("field");
    treesitter::Node object_node = node.child_by_field_name("object");

    if (!field_node.is_null() && field_node.type() == "identifier") {
        const std::string field_name = std::string(field_node.text(ctx.source));
        mark_identifiers_handled(field_node, ctx);

        if (!object_node.is_null()) {
            const uint32_t sb = object_node.start_byte();
            const uint32_t eb = field_node.end_byte();
            if (sb <= eb && eb <= ctx.source.size()) {
                const std::string full_name = std::string(ctx.source.substr(sb, eb - sb));
                const auto sp = object_node.start_position();
                const auto ep = field_node.end_position();

                std::vector<std::string> candidates = {full_name};
                if (field_name != full_name) {
                    candidates.push_back(field_name);
                }

                ctx.result.occurrences.push_back(OccurrenceFact{
                    .kind = worker::FactKind::reference,
                    .written_name = full_name,
                    .range = ByteRange{sb, eb},
                    .display_range = DisplayRange{sp.line, sp.column, ep.line, ep.column},
                    .enclosing_scope = ctx.current_scope(),
                    .candidate_targets = std::move(candidates),
                });
            }
        } else {
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::reference,
                .written_name = field_name,
                .range = field_node.byte_range(),
                .display_range = field_node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {field_name},
            });
        }
    }

    if (!object_node.is_null()) {
        walk_node(object_node, ctx);
    }
}

void process_method_reference(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node target_node = (node.child_count() > 0) ? node.child(0) : treesitter::Node{};
    treesitter::Node method_node{};
    bool is_new = false;

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "new") {
            method_node = ch;
            is_new = true;
        } else if (ch.type() == "identifier" && ch != target_node) {
            method_node = ch;
        }
    }

    std::string target_name;
    if (!target_node.is_null()) {
        target_name = std::string(target_node.text(ctx.source));
    }

    if (is_new) {
        std::vector<std::string> candidates;
        if (!target_name.empty()) {
            std::string unqual = get_unqualified_name(target_name);
            candidates.push_back(target_name);
            if (unqual != target_name && !unqual.empty()) {
                candidates.push_back(unqual);
            }
        }
        candidates.emplace_back("new");

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::call,
            .written_name = "new",
            .range = method_node.byte_range(),
            .display_range = method_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = std::move(candidates),
        });
    } else if (!method_node.is_null()) {
        const std::string fn_name = std::string(method_node.text(ctx.source));
        mark_identifiers_handled(method_node, ctx);

        std::vector<std::string> candidates;
        if (!target_name.empty()) {
            candidates.push_back(target_name + "." + fn_name);
            std::string unqual = get_unqualified_name(target_name);
            if (unqual != target_name && !unqual.empty()) {
                candidates.push_back(unqual + "." + fn_name);
            }
        }
        candidates.push_back(fn_name);

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::call,
            .written_name = fn_name,
            .range = method_node.byte_range(),
            .display_range = method_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = std::move(candidates),
        });
    }

    if (!target_node.is_null()) {
        walk_node(target_node, ctx);
    }

    treesitter::Node type_args = node.child_by_field_name("type_arguments");
    if (!type_args.is_null()) {
        walk_node(type_args, ctx);
    }
}

void process_lambda_expression(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node params_node = node.child_by_field_name("parameters");
    if (!params_node.is_null()) {
        if (params_node.type() == "identifier") {
            mark_identifiers_handled(params_node, ctx);
        } else {
            for (uint32_t i = 0; i < params_node.child_count(); ++i) {
                auto ch = params_node.child(i);
                if (ch.type() == "formal_parameter") {
                    treesitter::Node p_name = ch.child_by_field_name("name");
                    if (!p_name.is_null()) {
                        mark_identifiers_handled(p_name, ctx);
                    }
                    treesitter::Node p_type = ch.child_by_field_name("type");
                    if (!p_type.is_null()) {
                        walk_node(p_type, ctx);
                    }
                } else if (ch.type() == "identifier") {
                    mark_identifiers_handled(ch, ctx);
                }
            }
        }
    }

    treesitter::Node body_node = node.child_by_field_name("body");
    if (!body_node.is_null()) {
        walk_node(body_node, ctx);
    }
}

void process_resource(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "identifier" && ch != type_node) {
                ctx.handled_identifier_byte_starts.insert(ch.start_byte());
                break;
            }
            if (ch.type() == "variable_declarator") {
                mark_identifiers_handled(ch.child_by_field_name("name"), ctx);
                break;
            }
        }
        walk_node(type_node, ctx);
        treesitter::Node val_node = node.child_by_field_name("value");
        if (!val_node.is_null()) {
            walk_node(val_node, ctx);
        }
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "modifiers") {
                walk_modifiers_for_annotations(node, ctx, ctx.current_scope());
            }
        }
        return;
    }

    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        walk_node(node.child(i), ctx);
    }
}

void process_type_pattern(treesitter::Node node, ASTContext& ctx) {
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "identifier") {
            ctx.handled_identifier_byte_starts.insert(ch.start_byte());
        } else {
            walk_node(ch, ctx);
        }
    }
}

void process_record_pattern_component(treesitter::Node node, ASTContext& ctx) {
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "identifier") {
            ctx.handled_identifier_byte_starts.insert(ch.start_byte());
        } else {
            walk_node(ch, ctx);
        }
    }
}

void process_instanceof_expression(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node left_node = node.child_by_field_name("left");
    if (!left_node.is_null()) {
        walk_node(left_node, ctx);
    }

    treesitter::Node right_node = node.child_by_field_name("right");
    if (!right_node.is_null()) {
        walk_node(right_node, ctx);
    }

    treesitter::Node name_node = node.child_by_field_name("name");
    if (!name_node.is_null()) {
        mark_identifiers_handled(name_node, ctx);
    }

    treesitter::Node pattern_node = node.child_by_field_name("pattern");
    if (!pattern_node.is_null()) {
        walk_node(pattern_node, ctx);
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

    if (type == "package_declaration") {
        process_package_declaration(node, ctx);
        return;
    }

    if (type == "import_declaration") {
        process_import_declaration(node, ctx);
        return;
    }

    if (type == "marker_annotation" || type == "annotation") {
        process_annotation(node, ctx);
        return;
    }

    if (type == "class_declaration" || type == "interface_declaration" ||
        type == "enum_declaration" || type == "record_declaration" ||
        type == "annotation_type_declaration") {
        process_type_declaration(node, ctx);
        return;
    }

    if (type == "enum_constant") {
        process_enum_constant(node, ctx);
        return;
    }

    if (type == "method_declaration" || type == "constructor_declaration" ||
        type == "compact_constructor_declaration" ||
        type == "annotation_type_element_declaration") {
        process_method(node, ctx);
        return;
    }

    if (type == "field_declaration" || type == "constant_declaration") {
        process_field_declaration(node, ctx);
        return;
    }

    if (type == "local_variable_declaration") {
        process_local_variable_declaration(node, ctx);
        return;
    }

    if (type == "enhanced_for_statement") {
        process_enhanced_for_statement(node, ctx);
        return;
    }

    if (type == "catch_clause") {
        process_catch_clause(node, ctx);
        return;
    }

    if (type == "resource") {
        process_resource(node, ctx);
        return;
    }

    if (type == "type_pattern") {
        process_type_pattern(node, ctx);
        return;
    }

    if (type == "record_pattern_component") {
        process_record_pattern_component(node, ctx);
        return;
    }

    if (type == "instanceof_expression") {
        process_instanceof_expression(node, ctx);
        return;
    }

    if (type == "method_invocation") {
        process_method_invocation(node, ctx);
        return;
    }

    if (type == "object_creation_expression") {
        process_object_creation(node, ctx);
        return;
    }

    if (type == "explicit_constructor_invocation") {
        process_explicit_constructor_invocation(node, ctx);
        return;
    }

    if (type == "field_access") {
        process_field_access(node, ctx);
        return;
    }

    if (type == "method_reference") {
        process_method_reference(node, ctx);
        return;
    }

    if (type == "lambda_expression") {
        process_lambda_expression(node, ctx);
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

JavaAdapter::JavaAdapter() {
    capabilities_ = {
        .functions = CapabilityStatus::unavailable,
        .methods = CapabilityStatus::supported,
        .classes = CapabilityStatus::supported,
        .structs = CapabilityStatus::unavailable,
        .interfaces = CapabilityStatus::supported,
        .enums = CapabilityStatus::supported,
        .records = CapabilityStatus::supported,
        .namespaces = CapabilityStatus::unavailable,
        .variables = CapabilityStatus::supported,
        .modules = CapabilityStatus::unavailable,
        .packages = CapabilityStatus::supported,
        .templates = CapabilityStatus::supported,
        .partial_types = CapabilityStatus::unavailable,
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

const LanguageCapabilities& JavaAdapter::capabilities() const noexcept {
    return capabilities_;
}

Result<AdapterResult> JavaAdapter::parse(std::string_view source,
                                         const std::filesystem::path& /*file_path*/,
                                         const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        return unexpected_result<AdapterResult>(ErrorCode::cancelled, "parsing cancelled");
    }

    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(Language::java);
    if (!ts_lang) {
        return unexpected_result<AdapterResult>(ErrorCode::invalid_argument,
                                                "Java grammar not available");
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
        .language = Language::java,
        .status = worker::CompletionStatus::complete,
        .symbols = {},
        .declarations = {},
        .occurrences = {},
        .diagnostics = {},
    };

    treesitter::Node root = tree_res->root_node();
    result.diagnostics = collect_syntax_errors(root, Language::java);

    ASTContext ctx{
        .source = source,
        .stop_token = stop_token,
        .result = result,
        .package_prefix = std::nullopt,
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

std::string_view JavaAdapter::highlighting_query() noexcept {
    static constexpr std::string_view kHighlightQuery = R"(
;; Keywords
[
  "abstract"
  "assert"
  "break"
  "case"
  "catch"
  "class"
  "continue"
  "default"
  "do"
  "else"
  "enum"
  "exports"
  "extends"
  "final"
  "finally"
  "for"
  "if"
  "implements"
  "import"
  "instanceof"
  "interface"
  "module"
  "native"
  "new"
  "non-sealed"
  "open"
  "opens"
  "package"
  "permits"
  "private"
  "protected"
  "provides"
  "public"
  "requires"
  "record"
  "return"
  "sealed"
  "static"
  "strictfp"
  "switch"
  "synchronized"
  "throw"
  "throws"
  "to"
  "transient"
  "transitive"
  "try"
  "uses"
  "volatile"
  "when"
  "while"
  "with"
  "yield"
] @keyword

[
  (this)
  (super)
  (true)
  (false)
  (null_literal)
] @keyword

;; Primitive Types
[
  (boolean_type)
  (integral_type)
  (floating_point_type)
  (void_type)
] @type

;; Types
(type_identifier) @type

;; Declarations
(class_declaration name: (identifier) @class.definition)
(interface_declaration name: (identifier) @interface.definition)
(enum_declaration name: (identifier) @enum.definition)
(record_declaration name: (identifier) @class.definition)
(annotation_type_declaration name: (identifier) @interface.definition)

(method_declaration name: (identifier) @method.definition)
(constructor_declaration name: (identifier) @method.definition)
(compact_constructor_declaration name: (identifier) @method.definition)

(enum_constant name: (identifier) @enumMember.definition)
(field_declaration (variable_declarator name: (identifier) @property.definition))
(constant_declaration (variable_declarator name: (identifier) @property.definition))

(formal_parameter name: (identifier) @parameter)
(spread_parameter (variable_declarator name: (identifier) @parameter))

;; Method invocations
(method_invocation name: (identifier) @method)

;; Annotations
(marker_annotation name: (identifier) @decorator)
(marker_annotation name: (scoped_identifier) @decorator)
(annotation name: (identifier) @decorator)
(annotation name: (scoped_identifier) @decorator)

;; Literals
[
  (decimal_integer_literal)
  (hex_integer_literal)
  (octal_integer_literal)
  (binary_integer_literal)
  (decimal_floating_point_literal)
  (hex_floating_point_literal)
] @number

[
  (character_literal)
  (string_literal)
  (multiline_string_fragment)
] @string

;; Comments
[
  (line_comment)
  (block_comment)
] @comment

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
  "&="
  "|="
  "^="
  "<<"
  ">>"
  ">>>"
  "?"
  ":"
  "@"
  "->"
  "::"
] @operator
)";
    return kHighlightQuery;
}

Result<std::vector<HighlightToken>> JavaAdapter::highlight(std::string_view source,
                                                           const treesitter::Tree& tree) {
    const auto* ts_lang = treesitter::grammar_for_language(Language::java);
    if (ts_lang == nullptr) {
        return unexpected_result<std::vector<HighlightToken>>(ErrorCode::invalid_argument,
                                                              "Java grammar not available");
    }

    static std::string s_query_error;
    static const auto s_query = []() -> std::optional<treesitter::Query> {
        const auto* lang = treesitter::grammar_for_language(Language::java);
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
        if (a.start_column != b.start_column) {
            return a.start_column < b.start_column;
        }
        return a.length < b.length;
    });

    // Ensure non-overlapping tokens (LSP semantic tokens invariant)
    std::vector<HighlightToken> disjoint_tokens;
    disjoint_tokens.reserve(tokens.size());
    for (const auto& tok : tokens) {
        if (disjoint_tokens.empty()) {
            disjoint_tokens.push_back(tok);
            continue;
        }
        const auto& prev = disjoint_tokens.back();
        if (prev.line == tok.line && (prev.start_column + prev.length > tok.start_column)) {
            // Overlapping token on the same line: skip to maintain non-overlapping invariant
            continue;
        }
        disjoint_tokens.push_back(tok);
    }

    return disjoint_tokens;
}

Result<std::vector<HighlightToken>> JavaAdapter::highlight(std::string_view source) {
    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(Language::java);
    if (!ts_lang) {
        return unexpected_result<std::vector<HighlightToken>>(ErrorCode::invalid_argument,
                                                              "Java grammar not available");
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
