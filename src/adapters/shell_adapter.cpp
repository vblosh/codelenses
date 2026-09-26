#include "codelenses/adapters/shell_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <set>
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

bool is_shell_builtin(std::string_view name) noexcept {
    static const std::unordered_set<std::string_view> kBuiltins = {
        ":",       ".",         "[",        "alias",   "bg",       "bind",    "break",    "builtin",
        "caller",  "cd",        "command",  "compgen", "complete", "compopt", "continue", "declare",
        "dirs",    "disown",    "echo",     "enable",  "eval",     "exec",    "exit",     "export",
        "false",   "fc",        "fg",       "getopts", "hash",     "help",    "history",  "jobs",
        "kill",    "let",       "local",    "mapfile", "popd",     "printf",  "pushd",    "pwd",
        "read",    "readarray", "readonly", "return",  "set",      "shift",   "shopt",    "source",
        "suspend", "test",      "times",    "trap",    "true",     "type",    "typeset",  "ulimit",
        "umask",   "unalias",   "unset",    "wait",
    };
    return kBuiltins.contains(name);
}

struct AliasDefinition {
    std::string name;
    std::string signature;
    ByteRange name_range;
    treesitter::Node node;
};

std::vector<AliasDefinition> extract_alias_definitions(treesitter::Node cmd_node,
                                                       treesitter::Node cmd_name_node,
                                                       std::string_view source) {
    std::vector<AliasDefinition> defs;
    bool passed_cmd = false;
    for (uint32_t i = 0; i < cmd_node.child_count(); ++i) {
        auto ch = cmd_node.child(i);
        if (!passed_cmd) {
            if (ch == cmd_name_node) {
                passed_cmd = true;
            }
            continue;
        }
        const auto t = ch.type();
        if (t == "comment" || t == "file_redirect" || t == "heredoc_redirect" ||
            t == "herestring_redirect") {
            continue;
        }
        const auto ch_text = ch.text(source);
        auto eq_pos = ch_text.find('=');
        if (eq_pos != std::string_view::npos && eq_pos > 0) {
            const auto name = std::string(ch_text.substr(0, eq_pos));
            const uint32_t start_byte = ch.start_byte();
            const uint32_t end_byte = start_byte + static_cast<uint32_t>(eq_pos);
            defs.push_back(AliasDefinition{
                .name = name,
                .signature = "alias " + std::string(ch_text),
                .name_range = ByteRange{.start = start_byte, .end = end_byte},
                .node = ch,
            });
        }
    }
    return defs;
}

struct PrepassContext {
    std::unordered_set<std::string> defined_functions;
    std::unordered_set<std::string> defined_aliases;
};

void prepass_collect(treesitter::Node node, std::string_view source, PrepassContext& pctx) {
    if (node.is_null())
        return;
    const auto t = node.type();
    if (t == "function_definition") {
        treesitter::Node name_node = node.child_by_field_name("name");
        if (name_node.is_null()) {
            for (uint32_t i = 0; i < node.child_count(); ++i) {
                auto ch = node.child(i);
                if (ch.type() == "word") {
                    name_node = ch;
                    break;
                }
            }
        }
        if (!name_node.is_null()) {
            pctx.defined_functions.insert(std::string(name_node.text(source)));
        }
    } else if (t == "command") {
        treesitter::Node cmd_name_node = node.child_by_field_name("name");
        if (cmd_name_node.is_null()) {
            for (uint32_t i = 0; i < node.child_count(); ++i) {
                auto ch = node.child(i);
                if (ch.type() == "command_name") {
                    cmd_name_node = ch;
                    break;
                }
            }
        }
        if (!cmd_name_node.is_null() && cmd_name_node.text(source) == "alias") {
            for (const auto& def : extract_alias_definitions(node, cmd_name_node, source)) {
                pctx.defined_aliases.insert(def.name);
            }
        }
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        prepass_collect(node.child(i), source, pctx);
    }
}

bool is_range_handled(ByteRange r, const std::set<ByteRange>& handled) {
    return handled.contains(r);
}

struct ASTContext {
    std::string_view source;
    CoordinateConverter converter;
    const std::stop_token& stop_token;
    AdapterResult& result;
    std::vector<std::string> scope_stack;
    std::set<ByteRange> handled_identifier_ranges;
    const std::unordered_set<std::string>& defined_functions;
    const std::unordered_set<std::string>& defined_aliases;

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

bool contains_dynamic_expansion(treesitter::Node n) {
    if (n.is_null())
        return false;
    const auto t = n.type();
    if (t == "simple_expansion" || t == "expansion" || t == "command_substitution" ||
        t == "process_substitution") {
        return true;
    }
    for (uint32_t i = 0; i < n.child_count(); ++i) {
        if (contains_dynamic_expansion(n.child(i)))
            return true;
    }
    return false;
}

std::string clean_literal_path(std::string_view raw) {
    if (raw.size() >= 2) {
        if ((raw.front() == '"' && raw.back() == '"') ||
            (raw.front() == '\'' && raw.back() == '\'')) {
            return std::string(raw.substr(1, raw.size() - 2));
        }
    }
    return std::string(raw);
}

void walk_node(treesitter::Node node, ASTContext& ctx);

void process_function_definition(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (name_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "word") {
                name_node = ch;
                break;
            }
        }
    }

    std::string fn_name;
    if (!name_node.is_null()) {
        fn_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_ranges.insert(name_node.byte_range());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + fn_name : fn_name;

        treesitter::Node body_node = node.child_by_field_name("body");
        std::string signature;
        const auto node_text = node.text(ctx.source);
        if (!body_node.is_null() && body_node.start_byte() >= node.start_byte()) {
            const size_t header_len = body_node.start_byte() - node.start_byte();
            signature = std::string(node_text.substr(0, std::min<size_t>(header_len, 128)));
        } else {
            const auto brace_pos = node_text.find('{');
            if (brace_pos != std::string_view::npos) {
                signature = std::string(node_text.substr(0, std::min<size_t>(brace_pos, 128)));
            } else {
                signature =
                    std::string(node_text.substr(0, std::min<size_t>(node_text.size(), 128)));
            }
        }
        while (!signature.empty() && std::isspace(static_cast<unsigned char>(signature.back()))) {
            signature.pop_back();
        }
        if (signature.empty()) {
            signature = fn_name + "()";
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
            .is_definition = true,
        });
    }

    treesitter::Node body_node = node.child_by_field_name("body");
    if (!body_node.is_null()) {
        if (!fn_name.empty()) {
            ctx.scope_stack.push_back(fn_name);
        }
        walk_node(body_node, ctx);
        if (!fn_name.empty()) {
            ctx.scope_stack.pop_back();
        }
    } else {
        if (!fn_name.empty()) {
            ctx.scope_stack.push_back(fn_name);
        }
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch != name_node) {
                walk_node(ch, ctx);
            }
        }
        if (!fn_name.empty()) {
            ctx.scope_stack.pop_back();
        }
    }
}

void process_variable_assignment(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (name_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "variable_name" || ch.type() == "subscript") {
                name_node = ch;
                break;
            }
        }
    }

    treesitter::Node var_id_node = name_node;
    if (!var_id_node.is_null() && var_id_node.type() == "subscript") {
        auto inner = var_id_node.child_by_field_name("name");
        if (!inner.is_null()) {
            var_id_node = inner;
        } else {
            for (uint32_t i = 0; i < var_id_node.child_count(); ++i) {
                auto ch = var_id_node.child(i);
                if (ch.type() == "variable_name") {
                    var_id_node = ch;
                    break;
                }
            }
        }
    }

    if (!var_id_node.is_null()) {
        const std::string var_name = std::string(var_id_node.text(ctx.source));
        ctx.handled_identifier_ranges.insert(var_id_node.byte_range());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + var_name : var_name;

        ctx.result.symbols.push_back(SymbolFact{
            .name = var_name,
            .qualified_name = qname,
            .kind = NodeKind::variable,
            .range = var_id_node.byte_range(),
            .display_range = var_id_node.display_range(),
            .enclosing_scope = scope,
            .signature = var_name,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = var_name,
            .qualified_name = qname,
            .kind = NodeKind::variable,
            .range = var_id_node.byte_range(),
            .display_range = var_id_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (!var_id_node.is_null() && ch == var_id_node)
            continue;
        walk_node(ch, ctx);
    }
}

void process_declaration_command(treesitter::Node node, ASTContext& ctx) {
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        const auto t = ch.type();
        if (t == "variable_assignment") {
            process_variable_assignment(ch, ctx);
        } else if (t == "variable_name") {
            const std::string var_name = std::string(ch.text(ctx.source));
            ctx.handled_identifier_ranges.insert(ch.byte_range());

            const auto scope = ctx.current_scope();
            const std::string qname = scope ? *scope + "." + var_name : var_name;

            ctx.result.symbols.push_back(SymbolFact{
                .name = var_name,
                .qualified_name = qname,
                .kind = NodeKind::variable,
                .range = ch.byte_range(),
                .display_range = ch.display_range(),
                .enclosing_scope = scope,
                .signature = var_name,
            });

            ctx.result.declarations.push_back(DeclarationFact{
                .symbol_name = var_name,
                .qualified_name = qname,
                .kind = NodeKind::variable,
                .range = ch.byte_range(),
                .display_range = ch.display_range(),
                .enclosing_scope = scope,
                .is_definition = true,
            });
        } else {
            ctx.handled_identifier_ranges.insert(ch.byte_range());
            walk_node(ch, ctx);
        }
    }
}

void process_for_statement(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node var_node = node.child_by_field_name("variable");
    if (var_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "variable_name") {
                var_node = ch;
                break;
            }
        }
    }

    if (!var_node.is_null()) {
        const std::string var_name = std::string(var_node.text(ctx.source));
        ctx.handled_identifier_ranges.insert(var_node.byte_range());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + var_name : var_name;

        ctx.result.symbols.push_back(SymbolFact{
            .name = var_name,
            .qualified_name = qname,
            .kind = NodeKind::variable,
            .range = var_node.byte_range(),
            .display_range = var_node.display_range(),
            .enclosing_scope = scope,
            .signature = var_name,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = var_name,
            .qualified_name = qname,
            .kind = NodeKind::variable,
            .range = var_node.byte_range(),
            .display_range = var_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (!var_node.is_null() && ch == var_node)
            continue;
        walk_node(ch, ctx);
    }
}

void process_command(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node cmd_name_node = node.child_by_field_name("name");
    if (cmd_name_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "command_name") {
                cmd_name_node = ch;
                break;
            }
        }
    }

    if (cmd_name_node.is_null()) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            walk_node(node.child(i), ctx);
        }
        return;
    }

    if (contains_dynamic_expansion(cmd_name_node)) {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            walk_node(node.child(i), ctx);
        }
        return;
    }

    const std::string cmd_name = std::string(cmd_name_node.text(ctx.source));

    // Handle source / . directives (H9-01)
    if (cmd_name == "source" || cmd_name == ".") {
        ctx.handled_identifier_ranges.insert(cmd_name_node.byte_range());
        for (uint32_t i = 0; i < cmd_name_node.child_count(); ++i) {
            ctx.handled_identifier_ranges.insert(cmd_name_node.child(i).byte_range());
        }

        treesitter::Node first_arg;
        bool passed_name = false;
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (!passed_name) {
                if (ch == cmd_name_node) {
                    passed_name = true;
                }
                continue;
            }
            const auto t = ch.type();
            if (t == "file_redirect" || t == "heredoc_redirect" || t == "comment" ||
                t == "herestring_redirect") {
                continue;
            }
            first_arg = ch;
            break;
        }

        if (!first_arg.is_null()) {
            const bool is_dyn = contains_dynamic_expansion(first_arg);
            const std::string arg_text = clean_literal_path(first_arg.text(ctx.source));

            ByteRange import_range = first_arg.byte_range();
            DisplayRange import_disp = first_arg.display_range();
            const auto raw_text = first_arg.text(ctx.source);
            if (raw_text.size() >= 2 && ((raw_text.front() == '"' && raw_text.back() == '"') ||
                                         (raw_text.front() == '\'' && raw_text.back() == '\''))) {
                import_range.start += 1;
                import_range.end -= 1;
                Point sp = ctx.converter.byte_to_point(static_cast<uint32_t>(import_range.start));
                Point ep = ctx.converter.byte_to_point(static_cast<uint32_t>(import_range.end));
                import_disp = DisplayRange{sp.line + 1, sp.column + 1, ep.line + 1, ep.column + 1};
            }

            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::import,
                .written_name = arg_text,
                .range = import_range,
                .display_range = import_disp,
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {arg_text},
                .confidence = is_dyn ? 0.0 : 1.0,
                .metadata_json =
                    is_dyn ? std::optional<std::string>(R"({"dynamic":true})") : std::nullopt,
            });
        }

        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch == cmd_name_node)
                continue;
            walk_node(ch, ctx);
        }
        return;
    }

    // Handle alias definitions (H9-01, H9-03)
    if (cmd_name == "alias") {
        ctx.handled_identifier_ranges.insert(cmd_name_node.byte_range());
        for (uint32_t i = 0; i < cmd_name_node.child_count(); ++i) {
            ctx.handled_identifier_ranges.insert(cmd_name_node.child(i).byte_range());
        }

        auto alias_defs = extract_alias_definitions(node, cmd_name_node, ctx.source);
        if (!alias_defs.empty()) {
            for (const auto& def : alias_defs) {
                ctx.handled_identifier_ranges.insert(def.node.byte_range());
                ctx.handled_identifier_ranges.insert(def.name_range);

                const auto scope = ctx.current_scope();
                const std::string qname = scope ? *scope + "." + def.name : def.name;

                Point sp = ctx.converter.byte_to_point(static_cast<uint32_t>(def.name_range.start));
                Point ep = ctx.converter.byte_to_point(static_cast<uint32_t>(def.name_range.end));
                DisplayRange dr{sp.line + 1, sp.column + 1, ep.line + 1, ep.column + 1};

                ctx.result.symbols.push_back(SymbolFact{
                    .name = def.name,
                    .qualified_name = qname,
                    .kind = NodeKind::macro,
                    .range = def.name_range,
                    .display_range = dr,
                    .enclosing_scope = scope,
                    .signature = def.signature,
                });

                ctx.result.declarations.push_back(DeclarationFact{
                    .symbol_name = def.name,
                    .qualified_name = qname,
                    .kind = NodeKind::macro,
                    .range = def.name_range,
                    .display_range = dr,
                    .enclosing_scope = scope,
                    .is_definition = true,
                });
            }
        } else {
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::call,
                .written_name = cmd_name,
                .range = cmd_name_node.byte_range(),
                .display_range = cmd_name_node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {cmd_name},
                .confidence = 1.0,
                .metadata_json = R"({"command_type":"builtin","is_builtin":true})",
            });
        }

        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch == cmd_name_node)
                continue;
            bool is_def_node = false;
            for (const auto& def : alias_defs) {
                if (ch == def.node) {
                    is_def_node = true;
                    break;
                }
            }
            if (!is_def_node) {
                walk_node(ch, ctx);
            }
        }
        return;
    }

    // Handle builtin / command prefixes (H9-02)
    if (cmd_name == "builtin" || cmd_name == "command") {
        ctx.handled_identifier_ranges.insert(cmd_name_node.byte_range());
        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::call,
            .written_name = cmd_name,
            .range = cmd_name_node.byte_range(),
            .display_range = cmd_name_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = {cmd_name},
            .confidence = 1.0,
            .metadata_json = R"({"command_type":"builtin","is_builtin":true})",
        });

        treesitter::Node target_arg;
        bool passed_name = false;
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (!passed_name) {
                if (ch == cmd_name_node)
                    passed_name = true;
                continue;
            }
            const auto t = ch.type();
            if (t == "comment" || t == "file_redirect" || t == "heredoc_redirect")
                continue;
            const auto txt = ch.text(ctx.source);
            if (txt.starts_with('-'))
                continue;
            target_arg = ch;
            break;
        }

        if (!target_arg.is_null() && !contains_dynamic_expansion(target_arg)) {
            const std::string target_name = clean_literal_path(target_arg.text(ctx.source));
            if (!target_name.empty()) {
                ctx.handled_identifier_ranges.insert(target_arg.byte_range());
                bool is_builtin = (cmd_name == "builtin") || is_shell_builtin(target_name);
                std::string target_meta = is_builtin
                                              ? R"({"command_type":"builtin","is_builtin":true})"
                                              : R"({"command_type":"external","is_builtin":false})";

                ctx.result.occurrences.push_back(OccurrenceFact{
                    .kind = worker::FactKind::call,
                    .written_name = target_name,
                    .range = target_arg.byte_range(),
                    .display_range = target_arg.display_range(),
                    .enclosing_scope = ctx.current_scope(),
                    .candidate_targets = {target_name},
                    .confidence = 1.0,
                    .metadata_json = std::move(target_meta),
                });
            }
        }

        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch == cmd_name_node || (!target_arg.is_null() && ch == target_arg))
                continue;
            walk_node(ch, ctx);
        }
        return;
    }

    if (!cmd_name.empty()) {
        ctx.handled_identifier_ranges.insert(cmd_name_node.byte_range());
        for (uint32_t i = 0; i < cmd_name_node.child_count(); ++i) {
            ctx.handled_identifier_ranges.insert(cmd_name_node.child(i).byte_range());
        }

        std::string meta;
        if (cmd_name.find('/') != std::string_view::npos) {
            meta = R"({"command_type":"external","is_builtin":false})";
        } else if (ctx.defined_aliases.contains(cmd_name)) {
            meta = R"({"command_type":"alias","is_builtin":false})";
        } else if (ctx.defined_functions.contains(cmd_name)) {
            meta = R"({"command_type":"function","is_builtin":false})";
        } else if (is_shell_builtin(cmd_name)) {
            meta = R"({"command_type":"builtin","is_builtin":true})";
        } else {
            meta = R"({"command_type":"external","is_builtin":false})";
        }

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::call,
            .written_name = cmd_name,
            .range = cmd_name_node.byte_range(),
            .display_range = cmd_name_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = {cmd_name},
            .confidence = 1.0,
            .metadata_json = std::move(meta),
        });
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch == cmd_name_node)
            continue;
        walk_node(ch, ctx);
    }
}

void process_variable_name(treesitter::Node node, ASTContext& ctx) {
    if (is_range_handled(node.byte_range(), ctx.handled_identifier_ranges)) {
        return;
    }

    bool inside_expansion = false;
    auto parent = node.parent();
    while (!parent.is_null()) {
        const auto pt = parent.type();
        if (pt == "expansion" || pt == "simple_expansion" || pt == "arithmetic_expansion") {
            inside_expansion = true;
            break;
        }
        if (pt == "command" || pt == "function_definition" || pt == "variable_assignment" ||
            pt == "declaration_command" || pt == "for_statement") {
            break;
        }
        parent = parent.parent();
    }

    if (inside_expansion) {
        const std::string name = std::string(node.text(ctx.source));
        ctx.handled_identifier_ranges.insert(node.byte_range());
        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::reference,
            .written_name = name,
            .range = node.byte_range(),
            .display_range = node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = {name},
        });
    }
}

void walk_node(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null())
        return;
    if (ctx.stop_token.stop_requested())
        return;

    const auto type = node.type();

    if (type == "function_definition") {
        process_function_definition(node, ctx);
        return;
    }

    if (type == "variable_assignment") {
        process_variable_assignment(node, ctx);
        return;
    }

    if (type == "declaration_command") {
        process_declaration_command(node, ctx);
        return;
    }

    if (type == "for_statement") {
        process_for_statement(node, ctx);
        return;
    }

    if (type == "command") {
        process_command(node, ctx);
        return;
    }

    if (type == "variable_name" || type == "special_variable_name") {
        process_variable_name(node, ctx);
        return;
    }

    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        walk_node(node.child(i), ctx);
    }
}

} // namespace

ShellAdapter::ShellAdapter(Language lang)
    : lang_((lang == Language::bash) ? Language::bash : Language::shell) {
    capabilities_ = {
        .functions = CapabilityStatus::supported,
        .methods = CapabilityStatus::unavailable,
        .classes = CapabilityStatus::unavailable,
        .structs = CapabilityStatus::unavailable,
        .interfaces = CapabilityStatus::unavailable,
        .enums = CapabilityStatus::unavailable,
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
        .imports = CapabilityStatus::supported,
        .includes = CapabilityStatus::unavailable,
        .api_endpoints = CapabilityStatus::deferred,
        .database_tables = CapabilityStatus::deferred,
        .test_declarations = CapabilityStatus::deferred,
        .override_analysis = CapabilityStatus::deferred,
        .instantiation_analysis = CapabilityStatus::deferred,
    };
}

const LanguageCapabilities& ShellAdapter::capabilities() const noexcept {
    return capabilities_;
}

Result<AdapterResult> ShellAdapter::parse(std::string_view source,
                                          const std::filesystem::path& /*file_path*/,
                                          const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        return unexpected_result<AdapterResult>(ErrorCode::cancelled, "parsing cancelled");
    }

    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(lang_);
    if (!ts_lang) {
        return unexpected_result<AdapterResult>(ErrorCode::invalid_argument,
                                                "Grammar not available for shell adapter");
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
        .language = lang_,
        .status = worker::CompletionStatus::complete,
        .symbols = {},
        .declarations = {},
        .occurrences = {},
        .diagnostics = {},
    };

    treesitter::Node root = tree_res->root_node();
    result.diagnostics = collect_syntax_errors(root, lang_);

    PrepassContext pctx;
    prepass_collect(root, source, pctx);

    ASTContext ctx{
        .source = source,
        .converter = CoordinateConverter(source),
        .stop_token = stop_token,
        .result = result,
        .scope_stack = {},
        .handled_identifier_ranges = {},
        .defined_functions = pctx.defined_functions,
        .defined_aliases = pctx.defined_aliases,
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

std::string_view ShellAdapter::highlighting_query() noexcept {
    static constexpr std::string_view kHighlightQuery = R"(
;; Strings
[
  (string)
  (raw_string)
  (ansi_c_string)
  (heredoc_body)
  (heredoc_start)
] @string

;; Functions and commands
(command_name) @function

;; Variables
(variable_name) @variable
(special_variable_name) @variable

;; Keywords
[
  "case"
  "do"
  "done"
  "elif"
  "else"
  "esac"
  "export"
  "fi"
  "for"
  "function"
  "if"
  "in"
  "select"
  "then"
  "unset"
  "declare"
  "local"
  "readonly"
  "typeset"
  "until"
  "while"
] @keyword

;; Comments
(comment) @comment

;; Function definitions
(function_definition name: (word) @function)

;; Numbers
(number) @number
(file_descriptor) @number

;; Operators
[
  "$"
  "&&"
  ">"
  ">>"
  "<"
  "|"
  "="
  "+"
  "-"
  ";"
] @operator
)";
    return kHighlightQuery;
}

Result<std::vector<HighlightToken>> ShellAdapter::highlight(std::string_view source,
                                                            const treesitter::Tree& tree) {
    const auto* ts_lang = treesitter::grammar_for_language(lang_);
    if (ts_lang == nullptr) {
        return unexpected_result<std::vector<HighlightToken>>(
            ErrorCode::invalid_argument, "Grammar not available for shell adapter");
    }

    static std::string s_query_error;
    static const auto s_query = []() -> std::optional<treesitter::Query> {
        const auto* lang = treesitter::grammar_for_language(Language::bash);
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
                .byte_range = ByteRange{.start = sb, .end = eb},
                .display_range = node.display_range(),
            };
        }
    }

    std::vector<HighlightToken> tokens;
    tokens.reserve(token_map.size());
    for (auto& [k, tok] : token_map) {
        tokens.push_back(std::move(tok));
    }

    std::ranges::sort(tokens, [](const HighlightToken& a, const HighlightToken& b) {
        if (a.line != b.line) {
            return a.line < b.line;
        }
        return a.start_column < b.start_column;
    });

    return tokens;
}

Result<std::vector<HighlightToken>> ShellAdapter::highlight(std::string_view source) {
    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(lang_);
    if (ts_lang == nullptr) {
        return unexpected_result<std::vector<HighlightToken>>(
            ErrorCode::invalid_argument, "Grammar not available for shell adapter");
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
