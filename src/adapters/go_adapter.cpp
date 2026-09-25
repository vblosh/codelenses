#include "codelenses/adapters/go_adapter.hpp"

#include <algorithm>
#include <cctype>
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
    std::string package_name;

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

std::string strip_quotes(std::string_view raw) {
    if (raw.size() >= 2) {
        if ((raw.front() == '"' && raw.back() == '"') ||
            (raw.front() == '`' && raw.back() == '`')) {
            return std::string(raw.substr(1, raw.size() - 2));
        }
    }
    return std::string(raw);
}

std::string extract_receiver_type_name(treesitter::Node receiver_node, std::string_view source) {
    if (receiver_node.is_null())
        return {};
    const uint32_t count = receiver_node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        auto ch = receiver_node.child(i);
        if (ch.type() == "parameter_declaration") {
            treesitter::Node type_node = ch.child_by_field_name("type");
            if (type_node.is_null()) {
                for (uint32_t j = 0; j < ch.child_count(); ++j) {
                    auto tch = ch.child(j);
                    if (tch.type() == "type_identifier" || tch.type() == "pointer_type" ||
                        tch.type() == "generic_type") {
                        type_node = tch;
                        break;
                    }
                }
            }
            while (!type_node.is_null()) {
                const auto tt = type_node.type();
                if (tt == "pointer_type") {
                    treesitter::Node inner = type_node.child_by_field_name("type");
                    if (inner.is_null()) {
                        for (uint32_t j = 0; j < type_node.child_count(); ++j) {
                            auto pch = type_node.child(j);
                            if (pch.type() != "*") {
                                inner = pch;
                                break;
                            }
                        }
                    }
                    type_node = inner;
                } else if (tt == "generic_type") {
                    treesitter::Node inner = type_node.child_by_field_name("type");
                    if (!inner.is_null()) {
                        type_node = inner;
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }
            if (!type_node.is_null() && type_node.type() == "type_identifier") {
                return std::string(type_node.text(source));
            }
        }
    }
    return {};
}

void walk_node(treesitter::Node node, ASTContext& ctx);

void process_package_clause(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node pkg_id_node = node.child_by_field_name("name");
    if (pkg_id_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "package_identifier") {
                pkg_id_node = ch;
                break;
            }
        }
    }

    if (!pkg_id_node.is_null()) {
        const std::string pkg_name = std::string(pkg_id_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(pkg_id_node.start_byte());
        ctx.package_name = pkg_name;

        ctx.result.symbols.push_back(SymbolFact{
            .name = pkg_name,
            .qualified_name = pkg_name,
            .kind = NodeKind::package,
            .range = pkg_id_node.byte_range(),
            .display_range = pkg_id_node.display_range(),
            .enclosing_scope = std::nullopt,
            .signature = "package " + pkg_name,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = pkg_name,
            .qualified_name = pkg_name,
            .kind = NodeKind::package,
            .range = pkg_id_node.byte_range(),
            .display_range = pkg_id_node.display_range(),
            .enclosing_scope = std::nullopt,
            .is_definition = true,
        });
    }
}

void process_import_spec(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node path_node = node.child_by_field_name("path");
    if (path_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "interpreted_string_literal" || ch.type() == "raw_string_literal" ||
                ch.type() == "string_literal") {
                path_node = ch;
                break;
            }
        }
    }

    treesitter::Node name_node = node.child_by_field_name("name");
    if (!name_node.is_null()) {
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());
    }

    if (!path_node.is_null()) {
        const std::string raw = std::string(path_node.text(ctx.source));
        const std::string path = strip_quotes(raw);

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::import,
            .written_name = path,
            .range = path_node.byte_range(),
            .display_range = path_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = {path},
        });
    }
}

void process_import_declaration(treesitter::Node node, ASTContext& ctx) {
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "import_spec") {
            process_import_spec(ch, ctx);
        } else if (ch.type() == "import_spec_list") {
            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto gch = ch.child(j);
                if (gch.type() == "import_spec") {
                    process_import_spec(gch, ctx);
                }
            }
        }
    }
}

void process_function_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (name_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "identifier") {
                name_node = ch;
                break;
            }
        }
    }

    treesitter::Node body_node = node.child_by_field_name("body");
    std::string fn_name;
    if (!name_node.is_null()) {
        fn_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + fn_name : fn_name;

        std::string signature;
        const auto node_text = node.text(ctx.source);
        const auto brace_pos = node_text.find('{');
        if (brace_pos != std::string_view::npos) {
            signature = std::string(node_text.substr(0, std::min<size_t>(brace_pos, 128)));
            while (!signature.empty() &&
                   std::isspace(static_cast<unsigned char>(signature.back()))) {
                signature.pop_back();
            }
        } else {
            signature = std::string(node_text.substr(0, std::min<size_t>(node_text.size(), 128)));
        }

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

void process_method_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node receiver_node = node.child_by_field_name("receiver");
    const std::string receiver_type = extract_receiver_type_name(receiver_node, ctx.source);

    treesitter::Node name_node = node.child_by_field_name("name");
    if (name_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "field_identifier" || ch.type() == "identifier") {
                name_node = ch;
                break;
            }
        }
    }

    treesitter::Node body_node = node.child_by_field_name("body");
    std::string method_name;
    std::string scope_entry;

    if (!name_node.is_null()) {
        method_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const std::string qname =
            receiver_type.empty() ? method_name : (receiver_type + "." + method_name);
        scope_entry = qname;

        std::string signature;
        const auto node_text = node.text(ctx.source);
        const auto brace_pos = node_text.find('{');
        if (brace_pos != std::string_view::npos) {
            signature = std::string(node_text.substr(0, std::min<size_t>(brace_pos, 128)));
            while (!signature.empty() &&
                   std::isspace(static_cast<unsigned char>(signature.back()))) {
                signature.pop_back();
            }
        } else {
            signature = std::string(node_text.substr(0, std::min<size_t>(node_text.size(), 128)));
        }

        ctx.result.symbols.push_back(SymbolFact{
            .name = method_name,
            .qualified_name = qname,
            .kind = NodeKind::method,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .signature = signature,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = method_name,
            .qualified_name = qname,
            .kind = NodeKind::method,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .is_definition = !body_node.is_null(),
        });
    } else if (!receiver_type.empty()) {
        scope_entry = receiver_type;
    }

    if (!body_node.is_null()) {
        if (!scope_entry.empty()) {
            ctx.scope_stack.push_back(scope_entry);
        }
        walk_node(body_node, ctx);
        if (!scope_entry.empty()) {
            ctx.scope_stack.pop_back();
        }
    }
}

void process_type_spec(treesitter::Node spec, ASTContext& ctx) {
    treesitter::Node name_node = spec.child_by_field_name("name");
    if (name_node.is_null()) {
        for (uint32_t i = 0; i < spec.child_count(); ++i) {
            auto ch = spec.child(i);
            if (ch.type() == "type_identifier") {
                name_node = ch;
                break;
            }
        }
    }

    treesitter::Node type_node = spec.child_by_field_name("type");
    NodeKind kind = NodeKind::struct_;
    if (!type_node.is_null()) {
        const auto t_type = type_node.type();
        if (t_type == "struct_type") {
            kind = NodeKind::struct_;
        } else if (t_type == "interface_type") {
            kind = NodeKind::interface_;
        } else {
            kind = NodeKind::struct_;
        }
    }

    std::string name;
    if (!name_node.is_null()) {
        name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + name : name;

        std::string signature = "type " + name;
        if (!type_node.is_null()) {
            const auto t_type = type_node.type();
            if (t_type == "struct_type") {
                signature += " struct";
            } else if (t_type == "interface_type") {
                signature += " interface";
            } else {
                signature += " " + std::string(type_node.text(ctx.source));
            }
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

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = name,
            .qualified_name = qname,
            .kind = kind,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });
    }

    if (!name.empty()) {
        ctx.scope_stack.push_back(name);
    }
    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }
    if (!name.empty()) {
        ctx.scope_stack.pop_back();
    }
}

void process_type_declaration(treesitter::Node node, ASTContext& ctx) {
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "type_spec" || ch.type() == "type_alias") {
            process_type_spec(ch, ctx);
        }
    }
}

void process_call_expression(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node func_node = node.child_by_field_name("function");
    if (!func_node.is_null()) {
        std::string callee;
        treesitter::Node callee_name_node = func_node;

        if (func_node.type() == "identifier") {
            callee = std::string(func_node.text(ctx.source));
        } else if (func_node.type() == "selector_expression") {
            treesitter::Node field_node = func_node.child_by_field_name("field");
            if (field_node.is_null()) {
                for (uint32_t i = 0; i < func_node.child_count(); ++i) {
                    auto ch = func_node.child(i);
                    if (ch.type() == "field_identifier") {
                        field_node = ch;
                        break;
                    }
                }
            }
            if (!field_node.is_null()) {
                callee = std::string(field_node.text(ctx.source));
                callee_name_node = field_node;
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

        if (func_node.type() == "selector_expression") {
            treesitter::Node operand_node = func_node.child_by_field_name("operand");
            if (!operand_node.is_null()) {
                walk_node(operand_node, ctx);
            }
        } else if (func_node.type() != "identifier") {
            walk_node(func_node, ctx);
        }
    }

    treesitter::Node args_node = node.child_by_field_name("arguments");
    if (args_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "argument_list") {
                args_node = ch;
                break;
            }
        }
    }
    if (!args_node.is_null()) {
        walk_node(args_node, ctx);
    }
}

void process_var_declaration(treesitter::Node node, ASTContext& ctx) {
    const bool is_package_level = ctx.scope_stack.empty();

    auto handle_spec = [&](treesitter::Node spec) {
        for (uint32_t i = 0; i < spec.child_count(); ++i) {
            const char* fn = ts_node_field_name_for_child(spec.raw(), i);
            if (fn && std::string_view(fn) == "name") {
                treesitter::Node name_node = spec.child(i);
                const std::string name = std::string(name_node.text(ctx.source));
                ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

                if (is_package_level) {
                    const auto scope = ctx.current_scope();
                    const std::string qname = scope ? *scope + "." + name : name;
                    ctx.result.symbols.push_back(SymbolFact{
                        .name = name,
                        .qualified_name = qname,
                        .kind = NodeKind::variable,
                        .range = name_node.byte_range(),
                        .display_range = name_node.display_range(),
                        .enclosing_scope = scope,
                        .signature = "var " + name,
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
        }

        treesitter::Node type_node = spec.child_by_field_name("type");
        if (!type_node.is_null()) {
            walk_node(type_node, ctx);
        }

        treesitter::Node value_node = spec.child_by_field_name("value");
        if (!value_node.is_null()) {
            walk_node(value_node, ctx);
        }
    };

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "var_spec" || ch.type() == "const_spec") {
            handle_spec(ch);
        } else if (ch.type() == "var_spec_list" || ch.type() == "const_spec_list") {
            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto gch = ch.child(j);
                if (gch.type() == "var_spec" || gch.type() == "const_spec") {
                    handle_spec(gch);
                }
            }
        }
    }
}

void process_short_var_declaration(treesitter::Node node, ASTContext& ctx) {
    const bool is_package_level = ctx.scope_stack.empty();
    treesitter::Node left_node = node.child_by_field_name("left");

    auto handle_lhs_identifier = [&](treesitter::Node id_node) {
        const std::string name = std::string(id_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(id_node.start_byte());

        if (is_package_level) {
            const auto scope = ctx.current_scope();
            const std::string qname = scope ? *scope + "." + name : name;
            ctx.result.symbols.push_back(SymbolFact{
                .name = name,
                .qualified_name = qname,
                .kind = NodeKind::variable,
                .range = id_node.byte_range(),
                .display_range = id_node.display_range(),
                .enclosing_scope = scope,
                .signature = name + " :=",
            });

            ctx.result.declarations.push_back(DeclarationFact{
                .symbol_name = name,
                .qualified_name = qname,
                .kind = NodeKind::variable,
                .range = id_node.byte_range(),
                .display_range = id_node.display_range(),
                .enclosing_scope = scope,
                .is_definition = true,
            });
        }
    };

    if (!left_node.is_null()) {
        if (left_node.type() == "identifier") {
            handle_lhs_identifier(left_node);
        } else if (left_node.type() == "expression_list") {
            for (uint32_t i = 0; i < left_node.child_count(); ++i) {
                auto ch = left_node.child(i);
                if (ch.type() == "identifier") {
                    handle_lhs_identifier(ch);
                } else {
                    walk_node(ch, ctx);
                }
            }
        }
    }

    treesitter::Node right_node = node.child_by_field_name("right");
    if (!right_node.is_null()) {
        walk_node(right_node, ctx);
    }
}

void walk_node(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null())
        return;
    if (ctx.stop_token.stop_requested())
        return;

    const auto type = node.type();

    if (type == "package_clause") {
        process_package_clause(node, ctx);
        return;
    }

    if (type == "import_declaration") {
        process_import_declaration(node, ctx);
        return;
    }

    if (type == "function_declaration") {
        process_function_declaration(node, ctx);
        return;
    }

    if (type == "method_declaration") {
        process_method_declaration(node, ctx);
        return;
    }

    if (type == "type_declaration") {
        process_type_declaration(node, ctx);
        return;
    }

    if (type == "call_expression") {
        process_call_expression(node, ctx);
        return;
    }

    if (type == "var_declaration" || type == "const_declaration") {
        process_var_declaration(node, ctx);
        return;
    }

    if (type == "short_var_declaration") {
        process_short_var_declaration(node, ctx);
        return;
    }

    if (type == "identifier" || type == "field_identifier") {
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

GoAdapter::GoAdapter() {
    capabilities_ = {
        .functions = CapabilityStatus::supported,
        .methods = CapabilityStatus::supported,
        .classes = CapabilityStatus::unavailable,
        .structs = CapabilityStatus::supported,
        .interfaces = CapabilityStatus::supported,
        .enums = CapabilityStatus::unavailable,
        .records = CapabilityStatus::unavailable,
        .namespaces = CapabilityStatus::unavailable,
        .variables = CapabilityStatus::supported,
        .modules = CapabilityStatus::unavailable,
        .packages = CapabilityStatus::supported,
        .templates = CapabilityStatus::unavailable,
        .partial_types = CapabilityStatus::unavailable,
        .containment = CapabilityStatus::supported,
        .calls = CapabilityStatus::supported,
        .references = CapabilityStatus::supported,
        .inheritance = CapabilityStatus::unavailable,
        .implementation = CapabilityStatus::unavailable,
        .imports = CapabilityStatus::supported,
        .includes = CapabilityStatus::unavailable,
        .api_endpoints = CapabilityStatus::deferred,
        .database_tables = CapabilityStatus::deferred,
        .test_declarations = CapabilityStatus::deferred,
        .override_analysis = CapabilityStatus::deferred,
        .instantiation_analysis = CapabilityStatus::deferred,
    };
}

const LanguageCapabilities& GoAdapter::capabilities() const noexcept {
    return capabilities_;
}

Result<AdapterResult> GoAdapter::parse(std::string_view source,
                                       const std::filesystem::path& /*file_path*/,
                                       const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        return unexpected_result<AdapterResult>(ErrorCode::cancelled, "parsing cancelled");
    }

    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(Language::go);
    if (!ts_lang) {
        return unexpected_result<AdapterResult>(ErrorCode::invalid_argument,
                                                "Go grammar not available");
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
        .language = Language::go,
        .status = worker::CompletionStatus::complete,
        .symbols = {},
        .declarations = {},
        .occurrences = {},
        .diagnostics = {},
    };

    treesitter::Node root = tree_res->root_node();
    result.diagnostics = collect_syntax_errors(root, Language::go);

    ASTContext ctx{
        .source = source,
        .stop_token = stop_token,
        .result = result,
        .scope_stack = {},
        .handled_identifier_byte_starts = {},
        .package_name = {},
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
