#include "codelenses/adapters/go_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
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
    std::string package_name;
    struct InterfaceInfo {
        std::string qualified_name;
        std::map<std::string, std::string> methods;
        bool has_unsupported_elements{false};
    };
    std::map<std::string, InterfaceInfo> interfaces;
    std::map<std::string, std::map<std::string, std::string>> value_receiver_methods;
    std::unordered_set<std::string> generic_types;

    [[nodiscard]] std::optional<std::string> current_scope() const {
        if (package_name.empty() && scope_stack.empty())
            return std::nullopt;
        std::string full = package_name;
        for (size_t i = 0; i < scope_stack.size(); ++i) {
            if (!full.empty())
                full += ".";
            full += scope_stack[i];
        }
        return full;
    }
};

std::string normalize_type_text(std::string_view raw) {
    std::string result;
    bool pending_space = false;
    for (char ch : raw) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            pending_space = true;
            continue;
        }
        if (pending_space && !result.empty() &&
            (std::isalnum(static_cast<unsigned char>(result.back())) || result.back() == '_') &&
            (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) {
            result.push_back(' ');
        }
        result.push_back(ch);
        pending_space = false;
    }
    return result;
}

std::string parameter_types(treesitter::Node parameters, std::string_view source) {
    if (parameters.is_null())
        return "()";
    std::string result = "(";
    bool first = true;
    for (uint32_t i = 0; i < parameters.child_count(); ++i) {
        treesitter::Node parameter = parameters.child(i);
        if (parameter.type() != "parameter_declaration" &&
            parameter.type() != "variadic_parameter_declaration") {
            continue;
        }
        treesitter::Node type_node = parameter.child_by_field_name("type");
        if (type_node.is_null())
            continue;

        uint32_t name_count = 0;
        for (uint32_t j = 0; j < parameter.child_count(); ++j) {
            treesitter::Node child = parameter.child(j);
            if (child.end_byte() <= type_node.start_byte() && child.type() == "identifier") {
                ++name_count;
            }
        }
        if (name_count == 0)
            name_count = 1;

        std::string type = normalize_type_text(type_node.text(source));
        if (parameter.type() == "variadic_parameter_declaration")
            type = "..." + type;
        for (uint32_t j = 0; j < name_count; ++j) {
            if (!first)
                result += ",";
            result += type;
            first = false;
        }
    }
    result += ")";
    return result;
}

std::string method_signature_key(treesitter::Node node, std::string_view source) {
    treesitter::Node parameters = node.child_by_field_name("parameters");
    treesitter::Node result = node.child_by_field_name("result");
    std::string key = parameter_types(parameters, source) + "->";
    if (result.is_null())
        return key;
    if (result.type() == "parameter_list")
        return key + parameter_types(result, source);
    return key + normalize_type_text(result.text(source));
}

bool receiver_is_pointer(treesitter::Node receiver_node) {
    if (receiver_node.is_null())
        return false;
    for (uint32_t i = 0; i < receiver_node.child_count(); ++i) {
        treesitter::Node child = receiver_node.child(i);
        if (child.type() == "parameter_declaration") {
            treesitter::Node type = child.child_by_field_name("type");
            return !type.is_null() && type.type() == "pointer_type";
        }
    }
    return false;
}

bool is_builtin_type(std::string_view name) {
    static constexpr std::string_view builtins[] = {
        "any",     "bool",  "byte",   "comparable", "complex64", "complex128", "error", "float32",
        "float64", "int",   "int8",   "int16",      "int32",     "int64",      "rune",  "string",
        "uint",    "uint8", "uint16", "uint32",     "uint64",    "uintptr",
    };
    for (const auto builtin : builtins) {
        if (builtin == name)
            return true;
    }
    return false;
}

void record_type_reference(treesitter::Node node, ASTContext& ctx,
                           std::string_view declared_type_name) {
    if (node.is_null())
        return;

    const auto type = node.type();
    if (type == "type_identifier") {
        const std::string name(node.text(ctx.source));
        if (name.empty() || name == declared_type_name || is_builtin_type(name))
            return;
        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::reference,
            .written_name = name,
            .range = node.byte_range(),
            .display_range = node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = {name},
        });
        return;
    }
    if (type == "qualified_type") {
        const std::string name(node.text(ctx.source));
        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::reference,
            .written_name = name,
            .range = node.byte_range(),
            .display_range = node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = {name},
        });
        return;
    }

    if (type == "parameter_list") {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            record_type_reference(node.child(i), ctx, declared_type_name);
        }
        return;
    }
    if (type == "parameter_declaration" || type == "variadic_parameter_declaration") {
        record_type_reference(node.child_by_field_name("type"), ctx, declared_type_name);
        return;
    }

    if (type == "generic_type") {
        record_type_reference(node.child_by_field_name("type"), ctx, declared_type_name);
        record_type_reference(node.child_by_field_name("type_arguments"), ctx, declared_type_name);
    } else if (type == "array_type" || type == "slice_type" ||
               type == "implicit_length_array_type") {
        record_type_reference(node.child_by_field_name("element"), ctx, declared_type_name);
    } else if (type == "map_type") {
        record_type_reference(node.child_by_field_name("key"), ctx, declared_type_name);
        record_type_reference(node.child_by_field_name("value"), ctx, declared_type_name);
    } else if (type == "channel_type") {
        record_type_reference(node.child_by_field_name("value"), ctx, declared_type_name);
    } else if (type == "pointer_type" || type == "parenthesized_type" || type == "type_arguments" ||
               type == "type_elem") {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            record_type_reference(node.child(i), ctx, declared_type_name);
        }
    }
}

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

    const bool has_scope = !fn_name.empty();
    if (has_scope)
        ctx.scope_stack.push_back(fn_name);
    if (node.child_by_field_name("type_parameters").is_null()) {
        record_type_reference(node.child_by_field_name("parameters"), ctx, fn_name);
        record_type_reference(node.child_by_field_name("result"), ctx, fn_name);
    }
    if (!body_node.is_null())
        walk_node(body_node, ctx);
    if (has_scope)
        ctx.scope_stack.pop_back();
}

void process_method_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node receiver_node = node.child_by_field_name("receiver");
    const std::string receiver_type = extract_receiver_type_name(receiver_node, ctx.source);
    const bool pointer_receiver = receiver_is_pointer(receiver_node);

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

        const std::string receiver_scope =
            receiver_type.empty()
                ? std::string{}
                : (ctx.package_name.empty() ? receiver_type
                                            : ctx.package_name + "." + receiver_type);
        const std::string qname =
            receiver_scope.empty() ? method_name : receiver_scope + "." + method_name;
        scope_entry = receiver_type.empty() ? method_name : receiver_type + "." + method_name;

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
            .enclosing_scope = receiver_scope.empty() ? ctx.current_scope()
                                                      : std::optional<std::string>(receiver_scope),
            .signature = signature,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = method_name,
            .qualified_name = qname,
            .kind = NodeKind::method,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = receiver_scope.empty() ? ctx.current_scope()
                                                      : std::optional<std::string>(receiver_scope),
            .is_definition = !body_node.is_null(),
        });

        if (!receiver_type.empty() && !pointer_receiver) {
            ctx.value_receiver_methods[receiver_type][method_name] =
                method_signature_key(node, ctx.source);
        }
    } else if (!receiver_type.empty()) {
        scope_entry = receiver_type;
    }

    const bool has_scope = !scope_entry.empty();
    if (has_scope)
        ctx.scope_stack.push_back(scope_entry);
    const bool generic_receiver =
        receiver_node.text(ctx.source).find('[') != std::string_view::npos;
    if (!generic_receiver) {
        record_type_reference(node.child_by_field_name("parameters"), ctx, receiver_type);
        record_type_reference(node.child_by_field_name("result"), ctx, receiver_type);
    }
    if (!body_node.is_null())
        walk_node(body_node, ctx);
    if (has_scope)
        ctx.scope_stack.pop_back();
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
    const bool is_alias = spec.type() == "type_alias";
    NodeKind kind = is_alias ? NodeKind::type_alias : NodeKind::struct_;
    if (!type_node.is_null() && !is_alias) {
        const auto t_type = type_node.type();
        if (t_type == "struct_type") {
            kind = NodeKind::struct_;
        } else if (t_type == "interface_type") {
            kind = NodeKind::interface_;
        } else {
            // The normalized model has no general named-type kind. Preserve
            // scalar and other defined types as type-like symbols; the Go
            // signature distinguishes aliases (`=`) from defined types.
            kind = NodeKind::type_alias;
        }
    }

    std::string name;
    if (!name_node.is_null()) {
        name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());
        if (!spec.child_by_field_name("type_parameters").is_null())
            ctx.generic_types.insert(name);

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + name : name;
        treesitter::Node type_parameters = spec.child_by_field_name("type_parameters");

        std::string signature = "type " + name;
        if (!type_parameters.is_null())
            signature += std::string(type_parameters.text(ctx.source));
        if (is_alias)
            signature += " =";
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

        if (!type_node.is_null() && kind != NodeKind::struct_ && kind != NodeKind::interface_ &&
            type_parameters.is_null()) {
            record_type_reference(type_node, ctx, name);
        }

        if (kind == NodeKind::interface_ && !type_node.is_null()) {
            ASTContext::InterfaceInfo info{
                .qualified_name = qname,
                .methods = {},
                .has_unsupported_elements = !type_parameters.is_null(),
            };
            ctx.scope_stack.push_back(name);
            for (uint32_t i = 0; i < type_node.child_count(); ++i) {
                treesitter::Node child = type_node.child(i);
                if (child.type() == "method_elem") {
                    treesitter::Node method_name = child.child_by_field_name("name");
                    if (!method_name.is_null()) {
                        const std::string method_name_text =
                            std::string(method_name.text(ctx.source));
                        const std::string method_qname = qname + "." + method_name_text;
                        ctx.handled_identifier_byte_starts.insert(method_name.start_byte());
                        info.methods[method_name_text] = method_signature_key(child, ctx.source);
                        if (type_parameters.is_null()) {
                            record_type_reference(child.child_by_field_name("parameters"), ctx,
                                                  name);
                            record_type_reference(child.child_by_field_name("result"), ctx, name);
                        }
                        const auto method_signature = child.text(ctx.source);
                        ctx.result.symbols.push_back(SymbolFact{
                            .name = method_name_text,
                            .qualified_name = method_qname,
                            .kind = NodeKind::method,
                            .range = method_name.byte_range(),
                            .display_range = method_name.display_range(),
                            .enclosing_scope = qname,
                            .signature = std::string(method_signature),
                        });
                        ctx.result.declarations.push_back(DeclarationFact{
                            .symbol_name = method_name_text,
                            .qualified_name = method_qname,
                            .kind = NodeKind::method,
                            .range = method_name.byte_range(),
                            .display_range = method_name.display_range(),
                            .enclosing_scope = qname,
                            .is_definition = false,
                        });
                    }
                } else if (child.type() == "type_elem") {
                    info.has_unsupported_elements = true;
                }
            }
            ctx.scope_stack.pop_back();
            ctx.interfaces[name] = std::move(info);
        } else if (kind == NodeKind::struct_ && type_parameters.is_null() && !type_node.is_null()) {
            ctx.scope_stack.push_back(name);
            for (uint32_t i = 0; i < type_node.child_count(); ++i) {
                treesitter::Node child = type_node.child(i);
                if (child.type() != "field_declaration_list")
                    continue;
                for (uint32_t j = 0; j < child.child_count(); ++j) {
                    treesitter::Node field = child.child(j);
                    if (field.type() == "field_declaration") {
                        record_type_reference(field.child_by_field_name("type"), ctx, name);
                    }
                }
            }
            ctx.scope_stack.pop_back();
        }
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
            record_type_reference(type_node, ctx, {});
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
        .implementation = CapabilityStatus::candidate_only,
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

std::string_view GoAdapter::highlighting_query() noexcept {
    static constexpr std::string_view query = R"(
(comment) @comment
(identifier) @variable
(type_identifier) @type
(package_clause (package_identifier) @type.definition)
(type_spec name: (type_identifier) @type.definition)
(type_alias name: (type_identifier) @type.definition)
(function_declaration name: (identifier) @function.definition)
(method_declaration name: (field_identifier) @method.definition)
(method_elem name: (field_identifier) @method.definition)
(call_expression function: (identifier) @function)
(call_expression function: (selector_expression field: (field_identifier) @method))
(parameter_declaration name: (identifier) @parameter)
(variadic_parameter_declaration name: (identifier) @parameter)
(var_spec name: (identifier) @variable.definition)
(const_spec name: (identifier) @variable.definition)
(field_declaration name: (field_identifier) @property.definition)
(interpreted_string_literal) @string
(raw_string_literal) @string
(rune_literal) @string
(int_literal) @number
(float_literal) @number
(imaginary_literal) @number
"package" @keyword
"import" @keyword
"func" @keyword
"type" @keyword
"struct" @keyword
"interface" @keyword
"var" @keyword
"const" @keyword
"return" @keyword
"if" @keyword
"else" @keyword
"for" @keyword
"range" @keyword
"go" @keyword
"defer" @keyword
"select" @keyword
"case" @keyword
"default" @keyword
"break" @keyword
"continue" @keyword
"fallthrough" @keyword
"switch" @keyword
"map" @keyword
"chan" @keyword
"goto" @keyword
(nil) @keyword
(true) @keyword
(false) @keyword
(iota) @keyword
)";
    return query;
}

Result<std::vector<HighlightToken>> GoAdapter::highlight(std::string_view source,
                                                         const treesitter::Tree& tree) {
    static std::string query_error;
    static const auto query = []() -> std::optional<treesitter::Query> {
        const auto* lang = treesitter::grammar_for_language(Language::go);
        if (lang == nullptr) {
            query_error = "null grammar";
            return std::nullopt;
        }
        auto parsed = treesitter::Query::create(lang, highlighting_query());
        if (!parsed) {
            query_error = parsed.error().message;
            return std::nullopt;
        }
        return std::move(*parsed);
    }();
    if (!query.has_value()) {
        return unexpected_result<std::vector<HighlightToken>>(
            ErrorCode::failed, "Failed to create Go highlight query: " + query_error);
    }

    treesitter::QueryCursor cursor;
    cursor.exec(*query, tree.root_node());
    CoordinateConverter converter(source);
    const auto& legend = HighlightLegend::default_legend();
    std::map<std::pair<uint32_t, uint32_t>, HighlightToken> token_map;
    TSQueryMatch match;
    uint32_t capture_index = 0;
    while (cursor.next_capture(match, capture_index)) {
        const auto& capture = match.captures[capture_index];
        treesitter::Node node{capture.node};
        const uint32_t start = node.start_byte();
        const uint32_t end = node.end_byte();
        if (end <= start || end > source.size())
            continue;

        const std::string_view capture_name = query->capture_name(capture.index);
        std::string_view token_name = capture_name;
        uint32_t modifiers = 0;
        if (const auto dot = capture_name.find('.'); dot != std::string_view::npos) {
            token_name = capture_name.substr(0, dot);
            const auto modifier = capture_name.substr(dot + 1);
            if (const auto bit = legend.token_modifier_bit(modifier))
                modifiers |= *bit;
        }
        const auto token_type = legend.token_type_index(token_name);
        if (!token_type)
            continue;

        const auto point = converter.byte_to_point(start);
        const auto key = std::pair{start, end};
        const auto it = token_map.find(key);
        if (it != token_map.end()) {
            it->second.token_type = *token_type;
            it->second.token_modifiers |= modifiers;
        } else {
            token_map.emplace(key, HighlightToken{
                                       .line = point.line,
                                       .start_column = point.column,
                                       .length = end - start,
                                       .token_type = *token_type,
                                       .token_modifiers = modifiers,
                                       .byte_range = node.byte_range(),
                                       .display_range = node.display_range(),
                                   });
        }
    }

    std::vector<HighlightToken> tokens;
    tokens.reserve(token_map.size());
    for (const auto& [range, token] : token_map) {
        (void)range;
        tokens.push_back(token);
    }
    std::ranges::sort(tokens, [](const HighlightToken& lhs, const HighlightToken& rhs) {
        if (lhs.line != rhs.line)
            return lhs.line < rhs.line;
        return lhs.start_column < rhs.start_column;
    });
    return tokens;
}

Result<std::vector<HighlightToken>> GoAdapter::highlight(std::string_view source) {
    treesitter::Parser parser;
    const auto* lang = treesitter::grammar_for_language(Language::go);
    if (lang == nullptr) {
        return unexpected_result<std::vector<HighlightToken>>(ErrorCode::invalid_argument,
                                                              "Go grammar not available");
    }
    auto set_lang = parser.set_language(lang);
    if (!set_lang) {
        return unexpected_result<std::vector<HighlightToken>>(set_lang.error().code,
                                                              set_lang.error().message);
    }
    auto tree = parser.parse_string(source);
    if (!tree) {
        return unexpected_result<std::vector<HighlightToken>>(tree.error().code,
                                                              tree.error().message);
    }
    return highlight(source, *tree);
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
        .interfaces = {},
        .value_receiver_methods = {},
        .generic_types = {},
    };

    walk_node(root, ctx);

    // A concrete Go type has a statically evident interface relationship when its
    // value-receiver method set covers every explicitly declared interface method
    // in this file with the same signature. Embedded/generic interfaces, generic
    // types, and pointer-only method sets are left unresolved because they need
    // fuller Go method-set analysis.
    for (const auto& symbol : result.symbols) {
        if (symbol.kind != NodeKind::struct_ || ctx.generic_types.contains(symbol.name))
            continue;
        auto methods = ctx.value_receiver_methods.find(symbol.name);
        if (methods == ctx.value_receiver_methods.end())
            continue;
        for (const auto& [interface_name, interface_info] : ctx.interfaces) {
            (void)interface_name;
            if (interface_info.has_unsupported_elements || interface_info.methods.empty())
                continue;
            const bool implements =
                std::ranges::all_of(interface_info.methods, [&](const auto& required) {
                    const auto found = methods->second.find(required.first);
                    return found != methods->second.end() && found->second == required.second;
                });
            if (!implements)
                continue;
            result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::implementation,
                .written_name = interface_info.qualified_name,
                .range = symbol.range,
                .display_range = symbol.display_range,
                .enclosing_scope = symbol.qualified_name,
                .candidate_targets = {interface_info.qualified_name},
                .confidence = 1.0,
            });
        }
    }

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
