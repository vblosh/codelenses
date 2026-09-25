#include "codelenses/adapters/shell_adapter.hpp"

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

bool is_literal_path(treesitter::Node node) {
    if (node.is_null())
        return false;
    const auto t = node.type();
    if (t == "word")
        return true;
    if (t == "raw_string")
        return true;
    if (t == "string") {
        return !contains_dynamic_expansion(node);
    }
    return false;
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
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

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
        }
    }

    if (!var_id_node.is_null()) {
        const std::string var_name = std::string(var_id_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(var_id_node.start_byte());

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

    if (cmd_name == "source" || cmd_name == ".") {
        ctx.handled_identifier_byte_starts.insert(cmd_name_node.start_byte());
        for (uint32_t i = 0; i < cmd_name_node.child_count(); ++i) {
            ctx.handled_identifier_byte_starts.insert(cmd_name_node.child(i).start_byte());
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

        if (!first_arg.is_null() && is_literal_path(first_arg)) {
            const std::string arg_text = clean_literal_path(first_arg.text(ctx.source));
            ctx.handled_identifier_byte_starts.insert(first_arg.start_byte());

            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::import,
                .written_name = arg_text,
                .range = first_arg.byte_range(),
                .display_range = first_arg.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {arg_text},
            });
        }

        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch == cmd_name_node)
                continue;
            if (!first_arg.is_null() && ch == first_arg && is_literal_path(first_arg))
                continue;
            walk_node(ch, ctx);
        }
        return;
    }

    if (!cmd_name.empty()) {
        ctx.handled_identifier_byte_starts.insert(cmd_name_node.start_byte());
        for (uint32_t i = 0; i < cmd_name_node.child_count(); ++i) {
            ctx.handled_identifier_byte_starts.insert(cmd_name_node.child(i).start_byte());
        }

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::call,
            .written_name = cmd_name,
            .range = cmd_name_node.byte_range(),
            .display_range = cmd_name_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = {cmd_name},
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
    if (ctx.handled_identifier_byte_starts.contains(node.start_byte())) {
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
        if (pt == "command" || pt == "function_definition" || pt == "variable_assignment") {
            break;
        }
        parent = parent.parent();
    }

    if (inside_expansion) {
        const std::string name = std::string(node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(node.start_byte());
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

    if (type == "command") {
        process_command(node, ctx);
        return;
    }

    if (type == "variable_name") {
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
    const auto* ts_lang = treesitter::grammar_for_language(Language::bash);
    if (!ts_lang) {
        return unexpected_result<AdapterResult>(ErrorCode::invalid_argument,
                                                "Bash grammar not available");
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
