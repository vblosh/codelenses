#include "codelenses/adapters/java_adapter.hpp"

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
    std::optional<std::string> package_prefix;
    std::vector<std::string> scope_stack;
    std::unordered_set<uint32_t> handled_identifier_byte_starts;

    [[nodiscard]] std::optional<std::string> current_scope() const {
        if (!package_prefix && scope_stack.empty())
            return std::nullopt;
        std::string full;
        if (package_prefix) {
            full = *package_prefix;
        }
        for (const auto& s : scope_stack) {
            if (!full.empty())
                full += ".";
            full += s;
        }
        return full;
    }
};

void mark_identifiers_handled(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null())
        return;
    const auto type = node.type();
    if (type == "identifier" || type == "type_identifier") {
        ctx.handled_identifier_byte_starts.insert(node.start_byte());
    }
    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        mark_identifiers_handled(node.child(i), ctx);
    }
}

std::string extract_signature(treesitter::Node node, std::string_view source) {
    std::string_view text = node.text(source);
    size_t cutoff = text.find('{');
    size_t semi = text.find(';');
    if (semi != std::string_view::npos && (cutoff == std::string_view::npos || semi < cutoff)) {
        cutoff = semi;
    }
    if (cutoff == std::string_view::npos) {
        cutoff = text.size();
    }
    cutoff = std::min(cutoff, size_t(128));
    while (cutoff > 0 && (text[cutoff - 1] == ' ' || text[cutoff - 1] == '\t' ||
                          text[cutoff - 1] == '\r' || text[cutoff - 1] == '\n')) {
        --cutoff;
    }
    return std::string(text.substr(0, cutoff));
}

void walk_node(treesitter::Node node, ASTContext& ctx);

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
        if (ch.type() != "import" && ch.type() != ";") {
            if (first_ch.is_null())
                first_ch = ch;
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

    ctx.result.occurrences.push_back(OccurrenceFact{
        .kind = worker::FactKind::import,
        .written_name = written_name,
        .range = range,
        .display_range = display_range,
        .enclosing_scope = ctx.current_scope(),
        .candidate_targets = {written_name},
    });
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
    if (type_str == "interface_declaration") {
        kind = NodeKind::interface_;
    } else if (type_str == "enum_declaration") {
        kind = NodeKind::struct_;
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
                        ctx.result.occurrences.push_back(OccurrenceFact{
                            .kind = worker::FactKind::inheritance,
                            .written_name = base_name,
                            .range = ch.byte_range(),
                            .display_range = ch.display_range(),
                            .enclosing_scope = qname,
                            .candidate_targets = {base_name},
                        });
                    }
                }
            }
        }

        // Super interfaces for class_declaration or record_declaration
        if (type_str == "class_declaration" || type_str == "record_declaration") {
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
                        ctx.result.occurrences.push_back(OccurrenceFact{
                            .kind = worker::FactKind::implementation,
                            .written_name = iface_name,
                            .range = ch.byte_range(),
                            .display_range = ch.display_range(),
                            .enclosing_scope = qname,
                            .candidate_targets = {iface_name},
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
                        ctx.result.occurrences.push_back(OccurrenceFact{
                            .kind = worker::FactKind::inheritance,
                            .written_name = base_name,
                            .range = ch.byte_range(),
                            .display_range = ch.display_range(),
                            .enclosing_scope = qname,
                            .candidate_targets = {base_name},
                        });
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
            if (ct == "class_body" || ct == "interface_body" || ct == "enum_body") {
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

    // Type (return type)
    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }

    // Type parameters
    treesitter::Node type_params_node = node.child_by_field_name("type_parameters");
    if (!type_params_node.is_null()) {
        walk_node(type_params_node, ctx);
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

            treesitter::Node val_node = ch.child_by_field_name("value");
            if (!val_node.is_null()) {
                walk_node(val_node, ctx);
            }
        } else if (ch.type() != ";" && ch.type() != ",") {
            walk_node(ch, ctx);
        }
    }
}

void process_method_invocation(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (!name_node.is_null()) {
        const std::string callee = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::call,
            .written_name = callee,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = {callee},
        });
    }

    treesitter::Node object_node = node.child_by_field_name("object");
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
    if (!type_node.is_null()) {
        const std::string type_name = std::string(type_node.text(ctx.source));
        mark_identifiers_handled(type_node, ctx);

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::call,
            .written_name = type_name,
            .range = type_node.byte_range(),
            .display_range = type_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = {type_name},
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

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "class_body") {
            walk_node(ch, ctx);
        }
    }
}

void walk_node(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null())
        return;
    if (ctx.stop_token.stop_requested())
        return;

    const auto type = node.type();

    if (type == "package_declaration") {
        process_package_declaration(node, ctx);
        return;
    }

    if (type == "import_declaration") {
        process_import_declaration(node, ctx);
        return;
    }

    if (type == "class_declaration" || type == "interface_declaration" ||
        type == "enum_declaration" || type == "record_declaration") {
        process_type_declaration(node, ctx);
        return;
    }

    if (type == "method_declaration" || type == "constructor_declaration") {
        process_method(node, ctx);
        return;
    }

    if (type == "field_declaration" || type == "constant_declaration") {
        process_field_declaration(node, ctx);
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
        .templates = CapabilityStatus::unavailable,
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

} // namespace codelenses::adapters
