#include "codelenses/adapters/cpp_adapter.hpp"

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
    std::vector<std::string> scope_stack;
    std::unordered_set<uint32_t> handled_identifier_byte_starts;
    std::unordered_set<std::string> macro_param_names;
    std::unordered_set<std::string> template_param_names;
    const CompileCommandContext* compile_context{nullptr};
    std::filesystem::path file_path;
    std::unordered_set<std::string> command_line_macro_names;
    std::unordered_map<std::string, std::string> defined_macros;
    std::unordered_set<std::string> undefined_macros;
    size_t class_depth{0};

    [[nodiscard]] std::optional<std::string> current_scope() const {
        if (scope_stack.empty()) {
            return std::nullopt;
        }
        std::string full;
        for (size_t i = 0; i < scope_stack.size(); ++i) {
            if (i > 0) {
                full += "::";
            }
            full += scope_stack[i];
        }
        return full;
    }

    [[nodiscard]] bool is_inside_class() const noexcept { return class_depth > 0; }
};

struct DeclaratorInfo {
    treesitter::Node id_node;
    std::string base_name;
    std::optional<std::string> qualifier;
    treesitter::Node scope_node;
};

DeclaratorInfo unwrap_declarator_info(treesitter::Node node, std::string_view source) {
    treesitter::Node curr = node;
    while (!curr.is_null()) {
        const auto type = curr.type();
        if (type == "qualified_identifier") {
            treesitter::Node name_node = curr.child_by_field_name("name");
            treesitter::Node scope_node = curr.child_by_field_name("scope");
            DeclaratorInfo info;
            info.id_node = !name_node.is_null() ? name_node : curr;
            info.base_name = std::string(info.id_node.text(source));
            if (!scope_node.is_null()) {
                info.qualifier = std::string(scope_node.text(source));
                info.scope_node = scope_node;
            }
            return info;
        }
        if (type == "identifier" || type == "type_identifier" || type == "field_identifier" ||
            type == "destructor_name" || type == "operator_name") {
            return DeclaratorInfo{
                .id_node = curr,
                .base_name = std::string(curr.text(source)),
                .qualifier = std::nullopt,
                .scope_node = treesitter::Node{},
            };
        }
        if (type == "function_declarator" || type == "pointer_declarator" ||
            type == "reference_declarator" || type == "array_declarator" ||
            type == "parenthesized_declarator" || type == "init_declarator" ||
            type == "attributed_declarator" || type == "template_function") {
            treesitter::Node inner = curr.child_by_field_name("declarator");
            if (!inner.is_null()) {
                curr = inner;
                continue;
            }
            treesitter::Node name_field = curr.child_by_field_name("name");
            if (!name_field.is_null()) {
                curr = name_field;
                continue;
            }
        }
        for (uint32_t i = 0; i < curr.named_child_count(); ++i) {
            auto child = curr.named_child(i);
            auto found = unwrap_declarator_info(child, source);
            if (!found.id_node.is_null()) {
                return found;
            }
        }
        break;
    }
    return DeclaratorInfo{};
}

bool has_function_declarator(treesitter::Node node) {
    if (node.is_null()) {
        return false;
    }
    if (node.type() == "function_declarator") {
        return true;
    }
    for (uint32_t i = 0; i < node.named_child_count(); ++i) {
        if (has_function_declarator(node.named_child(i))) {
            return true;
        }
    }
    return false;
}

bool is_function_name_declarator(treesitter::Node id_node, treesitter::Node root_decl) {
    if (id_node.is_null() || root_decl.is_null()) {
        return false;
    }

    treesitter::Node curr = id_node;
    while (!curr.is_null() && curr != root_decl) {
        treesitter::Node parent = curr.parent();
        if (parent.is_null()) {
            break;
        }

        const auto ptype = parent.type();
        if (ptype == "function_declarator") {
            treesitter::Node fn_decl = parent.child_by_field_name("declarator");
            return fn_decl == curr;
        }
        if (ptype == "pointer_declarator" || ptype == "reference_declarator" ||
            ptype == "array_declarator") {
            return false;
        }

        curr = parent;
    }

    return curr == root_decl && curr.type() == "function_declarator";
}

std::string strip_template_arguments(std::string_view name) {
    std::string result;
    int depth = 0;
    for (char c : name) {
        if (c == '<') {
            depth++;
        } else if (c == '>') {
            if (depth > 0) {
                depth--;
            }
        } else if (depth == 0) {
            result += c;
        }
    }
    return result;
}

std::string cplusplus_macro_value(const std::optional<std::string>& standard) {
    if (!standard.has_value() || standard->empty()) {
        return "202002L";
    }
    std::string std_str = *standard;
    if (std_str.starts_with("-std=")) {
        std_str = std_str.substr(5);
    } else if (std_str.starts_with("--std=")) {
        std_str = std_str.substr(6);
    }
    std::string s;
    s.reserve(std_str.size());
    for (char c : std_str) {
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (s == "c++98" || s == "c++03" || s == "gnu++98" || s == "gnu++03" || s == "iso14882:1998" ||
        s == "iso14882:2003") {
        return "199711L";
    }
    if (s == "c++11" || s == "c++0x" || s == "gnu++11" || s == "gnu++0x" || s == "iso14882:2011") {
        return "201103L";
    }
    if (s == "c++14" || s == "c++1y" || s == "gnu++14" || s == "gnu++1y" || s == "iso14882:2014") {
        return "201402L";
    }
    if (s == "c++17" || s == "c++1z" || s == "gnu++17" || s == "gnu++1z" || s == "iso14882:2017") {
        return "201703L";
    }
    if (s == "c++20" || s == "c++2a" || s == "gnu++20" || s == "gnu++2a" || s == "iso14882:2020") {
        return "202002L";
    }
    if (s == "c++23" || s == "c++2b" || s == "gnu++23" || s == "gnu++2b" || s == "iso14882:2023") {
        return "202302L";
    }
    if (s == "c++26" || s == "c++2c" || s == "gnu++26" || s == "gnu++2c") {
        return "202612L";
    }
    return "202002L";
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

std::vector<std::string> split_scope_qualifier(std::string_view qual) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start < qual.size()) {
        auto pos = qual.find("::", start);
        if (pos == std::string_view::npos) {
            parts.emplace_back(qual.substr(start));
            break;
        }
        parts.emplace_back(qual.substr(start, pos - start));
        start = pos + 2;
    }
    return parts;
}

void walk_node(treesitter::Node node, ASTContext& ctx);

bool is_macro_defined(std::string_view name, const ASTContext& ctx) {
    std::string s(name);
    if (ctx.defined_macros.contains(s)) {
        return true;
    }
    if (ctx.undefined_macros.contains(s)) {
        return false;
    }
    return false;
}

int64_t evaluate_preproc_expression(treesitter::Node node, const ASTContext& ctx) {
    if (node.is_null()) {
        return 0;
    }
    const auto type = node.type();

    if (type == "number_literal") {
        std::string text = std::string(node.text(ctx.source));
        while (!text.empty() && (text.back() == 'u' || text.back() == 'U' || text.back() == 'l' ||
                                 text.back() == 'L')) {
            text.pop_back();
        }
        try {
            return std::stoll(text, nullptr, 0);
        } catch (...) {
            return 0;
        }
    }

    if (type == "char_literal") {
        std::string_view t = node.text(ctx.source);
        if (t.size() >= 3 && t.front() == '\'' && t.back() == '\'') {
            return static_cast<int64_t>(static_cast<unsigned char>(t[1]));
        }
        return 0;
    }

    if (type == "preproc_defined") {
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto child = node.child(i);
            if (child.type() == "identifier") {
                return is_macro_defined(child.text(ctx.source), ctx) ? 1 : 0;
            }
        }
        return 0;
    }

    if (type == "identifier") {
        std::string name = std::string(node.text(ctx.source));
        if (ctx.defined_macros.contains(name)) {
            const std::string& val = ctx.defined_macros.at(name);
            if (val.empty()) {
                return 1;
            }
            std::string text = val;
            while (!text.empty() && (text.back() == 'u' || text.back() == 'U' ||
                                     text.back() == 'l' || text.back() == 'L')) {
                text.pop_back();
            }
            try {
                return std::stoll(text, nullptr, 0);
            } catch (...) {
                return 1;
            }
        }
        return 0;
    }

    if (type == "parenthesized_expression") {
        for (uint32_t i = 0; i < node.named_child_count(); ++i) {
            return evaluate_preproc_expression(node.named_child(i), ctx);
        }
        return 0;
    }

    if (type == "unary_expression") {
        treesitter::Node op_node = node.child_by_field_name("operator");
        treesitter::Node arg_node = node.child_by_field_name("argument");
        if (arg_node.is_null()) {
            for (uint32_t i = 0; i < node.child_count(); ++i) {
                const auto ctype = node.child(i).type();
                if (ctype != "!" && ctype != "-" && ctype != "~" && ctype != "+") {
                    arg_node = node.child(i);
                    break;
                }
            }
        }
        std::string op = op_node.is_null() ? "" : std::string(op_node.text(ctx.source));
        if (op.empty() && node.child_count() > 0) {
            op = std::string(node.child(0).text(ctx.source));
        }
        int64_t val = evaluate_preproc_expression(arg_node, ctx);
        if (op == "!") {
            return (val == 0) ? 1 : 0;
        }
        if (op == "-") {
            return -val;
        }
        if (op == "~") {
            return ~val;
        }
        return val;
    }

    if (type == "binary_expression") {
        treesitter::Node left_node = node.child_by_field_name("left");
        treesitter::Node right_node = node.child_by_field_name("right");
        treesitter::Node op_node = node.child_by_field_name("operator");

        std::string op = op_node.is_null() ? "" : std::string(op_node.text(ctx.source));
        if (op == "&&") {
            int64_t l = evaluate_preproc_expression(left_node, ctx);
            if (l == 0) {
                return 0;
            }
            int64_t r = evaluate_preproc_expression(right_node, ctx);
            return (r != 0) ? 1 : 0;
        }
        if (op == "||") {
            int64_t l = evaluate_preproc_expression(left_node, ctx);
            if (l != 0) {
                return 1;
            }
            int64_t r = evaluate_preproc_expression(right_node, ctx);
            return (r != 0) ? 1 : 0;
        }

        int64_t l = evaluate_preproc_expression(left_node, ctx);
        int64_t r = evaluate_preproc_expression(right_node, ctx);
        if (op == "==") {
            return (l == r) ? 1 : 0;
        }
        if (op == "!=") {
            return (l != r) ? 1 : 0;
        }
        if (op == "<") {
            return (l < r) ? 1 : 0;
        }
        if (op == "<=") {
            return (l <= r) ? 1 : 0;
        }
        if (op == ">") {
            return (l > r) ? 1 : 0;
        }
        if (op == ">=") {
            return (l >= r) ? 1 : 0;
        }
        if (op == "+") {
            return l + r;
        }
        if (op == "-") {
            return l - r;
        }
        if (op == "*") {
            return l * r;
        }
        if (op == "/" && r != 0) {
            return l / r;
        }
        if (op == "%" && r != 0) {
            return l % r;
        }
        if (op == "&") {
            return l & r;
        }
        if (op == "|") {
            return l | r;
        }
        if (op == "^") {
            return l ^ r;
        }
        if (op == "<<" && r >= 0 && r < 64) {
            return l << r;
        }
        if (op == ">>" && r >= 0 && r < 64) {
            return l >> r;
        }
        return 0;
    }

    return 0;
}

void walk_preproc_condition_references(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null()) {
        return;
    }
    const auto type = node.type();
    if (type == "identifier") {
        if (!ctx.handled_identifier_byte_starts.contains(node.start_byte())) {
            const std::string name = std::string(node.text(ctx.source));
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::reference,
                .written_name = name,
                .range = node.byte_range(),
                .display_range = node.display_range(),
                .enclosing_scope = std::nullopt,
                .candidate_targets = {name},
            });
            ctx.handled_identifier_byte_starts.insert(node.start_byte());
        }
        return;
    }
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        walk_preproc_condition_references(node.child(i), ctx);
    }
}

void walk_preproc_alternative(treesitter::Node alt_node, ASTContext& ctx);
void walk_preproc_skipped(treesitter::Node node, ASTContext& ctx);

void walk_preproc_skipped(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null()) {
        return;
    }
    const auto type = node.type();
    if (type == "preproc_elif" || type == "preproc_elifdef" || type == "preproc_elifndef") {
        treesitter::Node name_node = node.child_by_field_name("name");
        treesitter::Node cond_node = node.child_by_field_name("condition");
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
        } else if (!cond_node.is_null()) {
            walk_preproc_condition_references(cond_node, ctx);
        }
        treesitter::Node alt = node.child_by_field_name("alternative");
        if (!alt.is_null()) {
            walk_preproc_skipped(alt, ctx);
        }
    }
}

void process_preproc_conditional(treesitter::Node node, ASTContext& ctx) {
    const auto type = node.type();
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node cond_node = node.child_by_field_name("condition");
    treesitter::Node alt_node = node.child_by_field_name("alternative");

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
    } else if (!cond_node.is_null()) {
        walk_preproc_condition_references(cond_node, ctx);
    }

    bool is_active = false;
    bool is_negated = (type == "preproc_ifndef" || type == "preproc_elifndef");
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        const auto ct = node.child(i).type();
        if (ct == "#ifndef" || ct == "#elifndef") {
            is_negated = true;
            break;
        }
        if (ct == "#ifdef" || ct == "#elifdef") {
            is_negated = false;
            break;
        }
    }

    if (type == "preproc_ifdef" || type == "preproc_ifndef" || type == "preproc_elifdef" ||
        type == "preproc_elifndef") {
        if (!name_node.is_null()) {
            const bool def = is_macro_defined(name_node.text(ctx.source), ctx);
            is_active = is_negated ? !def : def;
        }
    } else if (type == "preproc_if" || type == "preproc_elif") {
        if (!cond_node.is_null()) {
            is_active = (evaluate_preproc_expression(cond_node, ctx) != 0);
        }
    }

    if (is_active) {
        const uint32_t count = node.child_count();
        for (uint32_t i = 0; i < count; ++i) {
            auto child = node.child(i);
            if (child == name_node || child == cond_node || child == alt_node) {
                continue;
            }
            const auto ctype = child.type();
            if (ctype == "#ifdef" || ctype == "#ifndef" || ctype == "#if" || ctype == "#elif" ||
                ctype == "#elifdef" || ctype == "#elifndef" || ctype == "#else" ||
                ctype == "#endif" || ctype == "preproc_directive" || ctype == "\n") {
                continue;
            }
            walk_node(child, ctx);
        }
        if (!alt_node.is_null()) {
            walk_preproc_skipped(alt_node, ctx);
        }
    } else if (!alt_node.is_null()) {
        walk_preproc_alternative(alt_node, ctx);
    }
}

void walk_preproc_alternative(treesitter::Node alt_node, ASTContext& ctx) {
    if (alt_node.is_null()) {
        return;
    }
    const auto type = alt_node.type();
    if (type == "preproc_elif" || type == "preproc_elifdef" || type == "preproc_elifndef") {
        process_preproc_conditional(alt_node, ctx);
        return;
    }
    if (type == "preproc_else") {
        const uint32_t count = alt_node.child_count();
        for (uint32_t i = 0; i < count; ++i) {
            auto child = alt_node.child(i);
            const auto ctype = child.type();
            if (ctype == "#else" || ctype == "preproc_directive" || ctype == "\n") {
                continue;
            }
            walk_node(child, ctx);
        }
    }
}

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
        const bool is_quote = (raw.starts_with('"') && raw.ends_with('"'));

        std::vector<std::string> candidates;
        std::unordered_set<std::string> seen;

        candidates.push_back(target);
        seen.insert(target);

        if (ctx.compile_context != nullptr) {
            if (is_quote && !ctx.file_path.empty()) {
                auto parent = ctx.file_path.parent_path();
                if (!parent.empty()) {
                    auto cand = (parent / target).lexically_normal().generic_string();
                    if (seen.insert(cand).second) {
                        candidates.push_back(cand);
                    }
                }
            }

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
    if (name_node.is_null()) {
        return;
    }

    const std::string name = std::string(name_node.text(ctx.source));
    ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

    const auto scope = ctx.current_scope();
    const std::string qname = scope ? *scope + "::" + name : name;
    std::string signature = clean_signature(node.text(ctx.source));

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

    std::string macro_val = "1";
    treesitter::Node val_node = node.child_by_field_name("value");
    if (!val_node.is_null()) {
        macro_val = clean_signature(val_node.text(ctx.source));
    }
    ctx.defined_macros[name] = macro_val;
    ctx.undefined_macros.erase(name);

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

    if (!val_node.is_null()) {
        walk_node(val_node, ctx);
    }

    ctx.macro_param_names = std::move(saved_params);
}

void process_namespace(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");

    std::vector<std::string> parts;
    std::string full_name;

    if (!name_node.is_null()) {
        full_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        parts = split_scope_qualifier(full_name);

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "::" + full_name : full_name;

        ctx.result.symbols.push_back(SymbolFact{
            .name = full_name,
            .qualified_name = qname,
            .kind = NodeKind::namespace_,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = "namespace " + full_name,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = full_name,
            .qualified_name = qname,
            .kind = NodeKind::namespace_,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });
    }
    // NOTE: Anonymous namespaces (`namespace {}`) have name_node.is_null() and do not push to
    // scope_stack, so members retain their enclosing namespace scope (or global TU scope). This
    // aligns with C++ name lookup where anonymous namespace members are accessible without
    // qualifier in the enclosing scope.

    if (!body_node.is_null()) {
        for (const auto& p : parts) {
            ctx.scope_stack.push_back(p);
        }
        walk_node(body_node, ctx);
        for (size_t i = 0; i < parts.size(); ++i) {
            ctx.scope_stack.pop_back();
        }
    }
}

void process_namespace_alias(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (name_node.is_null()) {
        return;
    }

    std::string name = std::string(name_node.text(ctx.source));
    ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

    const auto scope = ctx.current_scope();
    const std::string qname = scope ? *scope + "::" + name : name;
    std::string sig = clean_signature(node.text(ctx.source));

    ctx.result.symbols.push_back(SymbolFact{
        .name = name,
        .qualified_name = qname,
        .kind = NodeKind::namespace_,
        .range = name_node.byte_range(),
        .display_range = name_node.display_range(),
        .enclosing_scope = scope,
        .signature = sig,
    });

    ctx.result.declarations.push_back(DeclarationFact{
        .symbol_name = name,
        .qualified_name = qname,
        .kind = NodeKind::namespace_,
        .range = name_node.byte_range(),
        .display_range = name_node.display_range(),
        .enclosing_scope = scope,
        .is_definition = true,
    });

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch != name_node && ch.type() != "namespace" && ch.type() != "=" && ch.type() != ";") {
            if (ch.type() == "nested_namespace_specifier" || ch.type() == "namespace_identifier" ||
                ch.type() == "identifier") {
                std::string target = std::string(ch.text(ctx.source));
                ctx.result.occurrences.push_back(OccurrenceFact{
                    .kind = worker::FactKind::reference,
                    .written_name = target,
                    .range = ch.byte_range(),
                    .display_range = ch.display_range(),
                    .enclosing_scope = scope,
                    .candidate_targets = {target},
                });
                ctx.handled_identifier_byte_starts.insert(ch.start_byte());
            }
        }
    }
}

void process_using_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node target_node;

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "identifier" || ch.type() == "qualified_identifier" ||
            ch.type() == "nested_namespace_specifier" || ch.type() == "type_identifier" ||
            ch.type() == "namespace_identifier") {
            target_node = ch;
        }
    }

    if (!target_node.is_null()) {
        std::string target = std::string(target_node.text(ctx.source));
        std::vector<std::string> candidates;
        candidates.push_back(target);
        if (auto pos = target.rfind("::"); pos != std::string::npos) {
            candidates.push_back(target.substr(pos + 2));
        }

        ctx.result.occurrences.push_back(OccurrenceFact{
            .kind = worker::FactKind::reference,
            .written_name = target,
            .range = target_node.byte_range(),
            .display_range = target_node.display_range(),
            .enclosing_scope = ctx.current_scope(),
            .candidate_targets = std::move(candidates),
        });
        ctx.handled_identifier_byte_starts.insert(target_node.start_byte());
    }
}

void process_alias_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node type_node = node.child_by_field_name("type");

    if (name_node.is_null()) {
        return;
    }

    std::string name = std::string(name_node.text(ctx.source));
    ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

    const auto scope = ctx.current_scope();
    const std::string qname = scope ? *scope + "::" + name : name;

    std::string sig;
    if (node.parent().type() == "template_declaration") {
        auto tmpl = node.parent();
        sig = clean_signature(
            ctx.source.substr(tmpl.start_byte(), node.end_byte() - tmpl.start_byte()));
    } else {
        sig = clean_signature(node.text(ctx.source));
    }

    ctx.result.symbols.push_back(SymbolFact{
        .name = name,
        .qualified_name = qname,
        .kind = NodeKind::type_alias,
        .range = name_node.byte_range(),
        .display_range = name_node.display_range(),
        .enclosing_scope = scope,
        .signature = sig,
    });

    ctx.result.declarations.push_back(DeclarationFact{
        .symbol_name = name,
        .qualified_name = qname,
        .kind = NodeKind::type_alias,
        .range = name_node.byte_range(),
        .display_range = name_node.display_range(),
        .enclosing_scope = scope,
        .is_definition = true,
    });

    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }
}

void process_inheritance(treesitter::Node class_node, const std::string& class_qname,
                         ASTContext& ctx) {
    for (uint32_t i = 0; i < class_node.child_count(); ++i) {
        auto ch = class_node.child(i);
        if (ch.type() == "base_class_clause") {
            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto bch = ch.child(j);
                if (bch.type() == "type_identifier" || bch.type() == "qualified_identifier" ||
                    bch.type() == "template_type") {
                    std::string raw_name = std::string(bch.text(ctx.source));
                    std::vector<std::string> candidates;
                    candidates.push_back(raw_name);

                    if (bch.type() == "template_type") {
                        treesitter::Node name_field = bch.child_by_field_name("name");
                        if (!name_field.is_null()) {
                            std::string base_id = std::string(name_field.text(ctx.source));
                            if (base_id != raw_name) {
                                candidates.push_back(base_id);
                            }
                        }
                    } else if (bch.type() == "qualified_identifier") {
                        treesitter::Node name_field = bch.child_by_field_name("name");
                        if (!name_field.is_null()) {
                            std::string base_id = std::string(name_field.text(ctx.source));
                            if (base_id != raw_name) {
                                candidates.push_back(base_id);
                            }
                        }
                    }

                    ctx.result.occurrences.push_back(OccurrenceFact{
                        .kind = worker::FactKind::inheritance,
                        .written_name = raw_name,
                        .range = bch.byte_range(),
                        .display_range = bch.display_range(),
                        .enclosing_scope = class_qname,
                        .candidate_targets = std::move(candidates),
                    });
                    ctx.handled_identifier_byte_starts.insert(bch.start_byte());
                }
            }
        }
    }
}

void process_enum(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");

    bool is_scoped = false;
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "class" || ch.type() == "struct") {
            is_scoped = true;
            break;
        }
    }

    std::string name;
    if (!name_node.is_null()) {
        name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "::" + name : name;
        const bool is_def = !body_node.is_null();

        std::string sig = "enum ";
        if (is_scoped) {
            sig += "class ";
        }
        sig += name;

        treesitter::Node base_node = node.child_by_field_name("base");
        if (!base_node.is_null()) {
            sig += " : " + std::string(base_node.text(ctx.source));
        }

        ctx.result.symbols.push_back(SymbolFact{
            .name = name,
            .qualified_name = qname,
            .kind = NodeKind::enum_,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = sig,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = name,
            .qualified_name = qname,
            .kind = NodeKind::enum_,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = is_def,
        });
    }

    if (!body_node.is_null()) {
        if (!name.empty() && is_scoped) {
            ctx.scope_stack.push_back(name);
        }
        walk_node(body_node, ctx);
        if (!name.empty() && is_scoped) {
            ctx.scope_stack.pop_back();
        }
    }
}

void process_enumerator(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    if (!name_node.is_null()) {
        const std::string name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "::" + name : name;

        std::string sig = clean_signature(node.text(ctx.source));

        ctx.result.symbols.push_back(SymbolFact{
            .name = name,
            .qualified_name = qname,
            .kind = NodeKind::enum_member,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = sig,
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

void process_parameter_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }
    treesitter::Node decl_node = node.child_by_field_name("declarator");
    if (!decl_node.is_null()) {
        DeclaratorInfo info = unwrap_declarator_info(decl_node, ctx.source);
        if (!info.id_node.is_null()) {
            ctx.handled_identifier_byte_starts.insert(info.id_node.start_byte());
        }
        // Walk any nested type qualifiers / declarators
        for (uint32_t i = 0; i < decl_node.named_child_count(); ++i) {
            auto child = decl_node.named_child(i);
            if (child != info.id_node && child.type() != "identifier") {
                walk_node(child, ctx);
            }
        }
    }
    treesitter::Node def_val = node.child_by_field_name("default_value");
    if (!def_val.is_null()) {
        walk_node(def_val, ctx);
    }
    treesitter::Node val_node = node.child_by_field_name("value");
    if (!val_node.is_null()) {
        walk_node(val_node, ctx);
    }
}

void process_class_or_struct(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");

    const bool is_def = !body_node.is_null();
    const NodeKind kind = (node.type() == "class_specifier") ? NodeKind::class_ : NodeKind::struct_;

    std::string name;
    if (!name_node.is_null()) {
        std::string raw_name = std::string(name_node.text(ctx.source));
        name = strip_template_arguments(raw_name);
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        std::string_view tag = "union";
        if (node.type() == "class_specifier") {
            tag = "class";
        } else if (node.type() == "struct_specifier") {
            tag = "struct";
        }
        std::string sig = std::string(tag) + " " + raw_name;
        if (node.parent().type() == "template_declaration") {
            auto tmpl = node.parent();
            std::size_t end_b = name_node.end_byte();
            sig = clean_signature(ctx.source.substr(tmpl.start_byte(), end_b - tmpl.start_byte()));
        }

        if (!is_def) {
            bool is_forward_decl = false;
            auto parent = node.parent();
            if (!parent.is_null()) {
                if (parent.type() == "translation_unit" || parent.type() == "declaration_list" ||
                    parent.type() == "template_declaration") {
                    is_forward_decl = true;
                } else if (parent.type() == "declaration") {
                    bool has_decl = false;
                    for (uint32_t i = 0; i < parent.child_count(); ++i) {
                        const auto ct = parent.child(i).type();
                        if (ct == "init_declarator" || ct == "declarator" ||
                            ct == "pointer_declarator" || ct == "function_declarator" ||
                            ct == "array_declarator" || ct == "identifier") {
                            has_decl = true;
                            break;
                        }
                    }
                    if (!has_decl) {
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
                    .kind = kind,
                    .range = name_node.byte_range(),
                    .display_range = name_node.display_range(),
                    .enclosing_scope = scope,
                    .signature = sig,
                });

                ctx.result.declarations.push_back(DeclarationFact{
                    .symbol_name = name,
                    .qualified_name = qname,
                    .kind = kind,
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

            ctx.result.symbols.push_back(SymbolFact{
                .name = name,
                .qualified_name = qname,
                .kind = kind,
                .range = name_node.byte_range(),
                .display_range = name_node.display_range(),
                .enclosing_scope = scope,
                .signature = sig,
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

            process_inheritance(node, qname, ctx);
        }
    }

    if (is_def && !body_node.is_null()) {
        if (!name.empty()) {
            ctx.scope_stack.push_back(name);
        }
        ctx.class_depth++;
        walk_node(body_node, ctx);
        ctx.class_depth--;
        if (!name.empty()) {
            ctx.scope_stack.pop_back();
        }
    }
}

void process_function_definition(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node decl_node = node.child_by_field_name("declarator");
    treesitter::Node type_node = node.child_by_field_name("type");
    DeclaratorInfo info = unwrap_declarator_info(decl_node, ctx.source);

    std::vector<std::string> qualifier_parts;
    std::string fn_name;

    if (!info.id_node.is_null()) {
        fn_name = info.base_name;
        ctx.handled_identifier_byte_starts.insert(info.id_node.start_byte());

        std::optional<std::string> scope;
        if (info.qualifier.has_value()) {
            std::string norm_qualifier = strip_template_arguments(*info.qualifier);
            qualifier_parts = split_scope_qualifier(norm_qualifier);
            auto cur = ctx.current_scope();
            scope = cur ? *cur + "::" + norm_qualifier : norm_qualifier;
            if (!info.scope_node.is_null()) {
                ctx.handled_identifier_byte_starts.insert(info.scope_node.start_byte());
                std::vector<std::string> cands = {norm_qualifier};
                if (norm_qualifier != *info.qualifier) {
                    cands.push_back(*info.qualifier);
                }
                if (!qualifier_parts.empty() && qualifier_parts.back() != norm_qualifier) {
                    cands.push_back(qualifier_parts.back());
                }
                ctx.result.occurrences.push_back(OccurrenceFact{
                    .kind = worker::FactKind::reference,
                    .written_name = *info.qualifier,
                    .range = info.scope_node.byte_range(),
                    .display_range = info.scope_node.display_range(),
                    .enclosing_scope = ctx.current_scope(),
                    .candidate_targets = std::move(cands),
                });
            }
        } else {
            scope = ctx.current_scope();
        }

        const std::string qname = scope ? *scope + "::" + fn_name : fn_name;

        // Build complete signature including template header, return type, and declarator
        std::string sig;
        std::size_t sig_start = node.start_byte();
        if (node.parent().type() == "template_declaration") {
            sig_start = node.parent().start_byte();
        }
        std::size_t sig_end = !decl_node.is_null() ? decl_node.end_byte() : info.id_node.end_byte();
        if (sig_end > sig_start && sig_end <= ctx.source.size()) {
            sig = clean_signature(ctx.source.substr(sig_start, sig_end - sig_start));
        } else {
            sig = fn_name;
        }

        const bool is_method = ctx.is_inside_class() || info.qualifier.has_value();
        const NodeKind kind = is_method ? NodeKind::method : NodeKind::function;

        ctx.result.symbols.push_back(SymbolFact{
            .name = fn_name,
            .qualified_name = qname,
            .kind = kind,
            .range = info.id_node.byte_range(),
            .display_range = info.id_node.display_range(),
            .enclosing_scope = scope,
            .signature = sig,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = fn_name,
            .qualified_name = qname,
            .kind = kind,
            .range = info.id_node.byte_range(),
            .display_range = info.id_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });
    }

    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }

    if (!decl_node.is_null()) {
        walk_node(decl_node, ctx);
    }

    // Walk constructor field_initializer_list if present
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "field_initializer_list") {
            walk_node(ch, ctx);
        }
    }

    treesitter::Node body_node = node.child_by_field_name("body");
    if (!body_node.is_null()) {
        for (const auto& qp : qualifier_parts) {
            ctx.scope_stack.push_back(qp);
        }
        if (!fn_name.empty()) {
            ctx.scope_stack.push_back(fn_name);
        }
        walk_node(body_node, ctx);
        if (!fn_name.empty()) {
            ctx.scope_stack.pop_back();
        }
        for (size_t i = 0; i < qualifier_parts.size(); ++i) {
            ctx.scope_stack.pop_back();
        }
    }
}

void process_field_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        const auto type_name = type_node.type();
        if (type_name == "class_specifier" || type_name == "struct_specifier" ||
            type_name == "union_specifier") {
            process_class_or_struct(type_node, ctx);
        } else if (type_name == "enum_specifier") {
            process_enum(type_node, ctx);
        } else {
            walk_node(type_node, ctx);
        }
    }

    treesitter::Node first_decl;
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto child = node.child(i);
        if (child == type_node || child.type() == "type") {
            continue;
        }
        DeclaratorInfo info = unwrap_declarator_info(child, ctx.source);
        if (!info.id_node.is_null()) {
            first_decl = child;
            break;
        }
    }

    std::string prefix;
    if (!first_decl.is_null() && first_decl.start_byte() > node.start_byte()) {
        prefix = clean_signature(
            ctx.source.substr(node.start_byte(), first_decl.start_byte() - node.start_byte()));
    } else if (!type_node.is_null()) {
        prefix = clean_signature(type_node.text(ctx.source));
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto child = node.child(i);
        if (child == type_node || child.type() == "type") {
            continue;
        }

        const bool is_fn = has_function_declarator(child);
        DeclaratorInfo info = unwrap_declarator_info(child, ctx.source);

        if (!info.id_node.is_null()) {
            const std::string name = info.base_name;
            ctx.handled_identifier_byte_starts.insert(info.id_node.start_byte());

            const auto scope = ctx.current_scope();
            const std::string qname = scope ? *scope + "::" + name : name;

            std::string sig;
            if (!prefix.empty()) {
                sig = prefix + " ";
            }
            sig += clean_signature(child.text(ctx.source));

            if (is_fn) {
                ctx.result.symbols.push_back(SymbolFact{
                    .name = name,
                    .qualified_name = qname,
                    .kind = NodeKind::method,
                    .range = info.id_node.byte_range(),
                    .display_range = info.id_node.display_range(),
                    .enclosing_scope = scope,
                    .signature = sig,
                });

                ctx.result.declarations.push_back(DeclarationFact{
                    .symbol_name = name,
                    .qualified_name = qname,
                    .kind = NodeKind::method,
                    .range = info.id_node.byte_range(),
                    .display_range = info.id_node.display_range(),
                    .enclosing_scope = scope,
                    .is_definition = false,
                });
            } else {
                ctx.result.symbols.push_back(SymbolFact{
                    .name = name,
                    .qualified_name = qname,
                    .kind = NodeKind::field,
                    .range = info.id_node.byte_range(),
                    .display_range = info.id_node.display_range(),
                    .enclosing_scope = scope,
                    .signature = sig,
                });

                ctx.result.declarations.push_back(DeclarationFact{
                    .symbol_name = name,
                    .qualified_name = qname,
                    .kind = NodeKind::field,
                    .range = info.id_node.byte_range(),
                    .display_range = info.id_node.display_range(),
                    .enclosing_scope = scope,
                    .is_definition = true,
                });
            }

            walk_node(child, ctx);
        }
    }
}

void process_declaration(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node type_node = node.child_by_field_name("type");
    if (!type_node.is_null()) {
        const auto type_name = type_node.type();
        if (type_name == "class_specifier" || type_name == "struct_specifier" ||
            type_name == "union_specifier") {
            process_class_or_struct(type_node, ctx);
        } else if (type_name == "enum_specifier") {
            process_enum(type_node, ctx);
        } else {
            walk_node(type_node, ctx);
        }
    }

    treesitter::Node first_decl;
    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto child = node.child(i);
        if (child == type_node || child.type() == "type") {
            continue;
        }

        DeclaratorInfo info = unwrap_declarator_info(child, ctx.source);
        if (!info.id_node.is_null()) {
            first_decl = child;
            break;
        }
    }

    std::string prefix;
    if (node.parent().type() == "template_declaration") {
        auto tmpl = node.parent();
        std::size_t end_b = !first_decl.is_null() ? first_decl.start_byte() : node.end_byte();
        prefix = clean_signature(ctx.source.substr(tmpl.start_byte(), end_b - tmpl.start_byte()));
    } else if (!first_decl.is_null() && first_decl.start_byte() > node.start_byte()) {
        prefix = clean_signature(
            ctx.source.substr(node.start_byte(), first_decl.start_byte() - node.start_byte()));
    } else if (!type_node.is_null()) {
        prefix = clean_signature(type_node.text(ctx.source));
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto child = node.child(i);
        const auto child_type = child.type();
        if (child == type_node || child_type == "type") {
            continue;
        }

        if (child_type == "class_specifier" || child_type == "struct_specifier" ||
            child_type == "union_specifier") {
            process_class_or_struct(child, ctx);
            continue;
        }

        if (child_type == "enum_specifier") {
            process_enum(child, ctx);
            continue;
        }

        if (child_type == "init_declarator" || child_type == "declarator" ||
            child_type == "function_declarator" || child_type == "pointer_declarator" ||
            child_type == "reference_declarator" || child_type == "array_declarator" ||
            child_type == "field_declarator" || child_type == "identifier" ||
            child_type == "parenthesized_declarator" || child_type == "attributed_declarator" ||
            child_type == "template_function") {
            DeclaratorInfo info = unwrap_declarator_info(child, ctx.source);
            const bool is_fn = is_function_name_declarator(info.id_node, child);

            if (!info.id_node.is_null()) {
                const std::string name = info.base_name;
                ctx.handled_identifier_byte_starts.insert(info.id_node.start_byte());

                std::optional<std::string> scope;
                if (info.qualifier.has_value()) {
                    std::string norm_qualifier = strip_template_arguments(*info.qualifier);
                    auto cur = ctx.current_scope();
                    scope = cur ? *cur + "::" + norm_qualifier : norm_qualifier;
                    if (!info.scope_node.is_null()) {
                        ctx.handled_identifier_byte_starts.insert(info.scope_node.start_byte());
                        std::vector<std::string> cands = {norm_qualifier};
                        if (norm_qualifier != *info.qualifier) {
                            cands.push_back(*info.qualifier);
                        }
                        ctx.result.occurrences.push_back(OccurrenceFact{
                            .kind = worker::FactKind::reference,
                            .written_name = *info.qualifier,
                            .range = info.scope_node.byte_range(),
                            .display_range = info.scope_node.display_range(),
                            .enclosing_scope = ctx.current_scope(),
                            .candidate_targets = std::move(cands),
                        });
                    }
                } else {
                    scope = ctx.current_scope();
                }

                const std::string qname = scope ? *scope + "::" + name : name;

                std::string decl_sig;
                if (!prefix.empty()) {
                    decl_sig = prefix + " ";
                }
                decl_sig += clean_signature(child.text(ctx.source));

                if (is_fn) {
                    const bool is_method = ctx.is_inside_class() || info.qualifier.has_value();
                    const NodeKind kind = is_method ? NodeKind::method : NodeKind::function;

                    ctx.result.symbols.push_back(SymbolFact{
                        .name = name,
                        .qualified_name = qname,
                        .kind = kind,
                        .range = info.id_node.byte_range(),
                        .display_range = info.id_node.display_range(),
                        .enclosing_scope = scope,
                        .signature = decl_sig,
                    });

                    ctx.result.declarations.push_back(DeclarationFact{
                        .symbol_name = name,
                        .qualified_name = qname,
                        .kind = kind,
                        .range = info.id_node.byte_range(),
                        .display_range = info.id_node.display_range(),
                        .enclosing_scope = scope,
                        .is_definition = false,
                    });
                } else {
                    const NodeKind kind =
                        ctx.is_inside_class() ? NodeKind::field : NodeKind::variable;
                    ctx.result.symbols.push_back(SymbolFact{
                        .name = name,
                        .qualified_name = qname,
                        .kind = kind,
                        .range = info.id_node.byte_range(),
                        .display_range = info.id_node.display_range(),
                        .enclosing_scope = scope,
                        .signature = decl_sig,
                    });

                    ctx.result.declarations.push_back(DeclarationFact{
                        .symbol_name = name,
                        .qualified_name = qname,
                        .kind = kind,
                        .range = info.id_node.byte_range(),
                        .display_range = info.id_node.display_range(),
                        .enclosing_scope = scope,
                        .is_definition = true,
                    });
                }
            }

            if (child_type == "init_declarator") {
                treesitter::Node val_node = child.child_by_field_name("value");
                if (!val_node.is_null()) {
                    walk_node(val_node, ctx);
                }
            } else {
                walk_node(child, ctx);
            }
        }
    }
}

void process_type_definition(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node type_node = node.child_by_field_name("type");

    if (!type_node.is_null()) {
        const auto type_name = type_node.type();
        if (type_name == "class_specifier" || type_name == "struct_specifier" ||
            type_name == "union_specifier") {
            process_class_or_struct(type_node, ctx);
        } else if (type_name == "enum_specifier") {
            process_enum(type_node, ctx);
        } else {
            walk_node(type_node, ctx);
        }
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto child = node.child(i);
        if (child == type_node || child.type() == "typedef" || child.type() == "type") {
            continue;
        }

        DeclaratorInfo info = unwrap_declarator_info(child, ctx.source);
        if (!info.id_node.is_null()) {
            const std::string name = info.base_name;
            ctx.handled_identifier_byte_starts.insert(info.id_node.start_byte());

            const auto scope = ctx.current_scope();
            const std::string qname = scope ? *scope + "::" + name : name;
            std::string sig = clean_signature(node.text(ctx.source));

            ctx.result.symbols.push_back(SymbolFact{
                .name = name,
                .qualified_name = qname,
                .kind = NodeKind::type_alias,
                .range = info.id_node.byte_range(),
                .display_range = info.id_node.display_range(),
                .enclosing_scope = scope,
                .signature = sig,
            });

            ctx.result.declarations.push_back(DeclarationFact{
                .symbol_name = name,
                .qualified_name = qname,
                .kind = NodeKind::type_alias,
                .range = info.id_node.byte_range(),
                .display_range = info.id_node.display_range(),
                .enclosing_scope = scope,
                .is_definition = true,
            });
        }
    }
}

void process_call_expression(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node func_node = node.child_by_field_name("function");
    if (!func_node.is_null()) {
        std::string callee;
        std::vector<std::string> candidates;
        treesitter::Node target_node = func_node;

        if (func_node.type() == "identifier" || func_node.type() == "field_identifier" ||
            func_node.type() == "type_identifier") {
            callee = std::string(func_node.text(ctx.source));
            candidates.push_back(callee);
            target_node = func_node;
            ctx.handled_identifier_byte_starts.insert(func_node.start_byte());
        } else if (func_node.type() == "field_expression") {
            treesitter::Node field = func_node.child_by_field_name("field");
            if (!field.is_null()) {
                if (field.type() == "template_method") {
                    treesitter::Node name_part = field.child_by_field_name("name");
                    if (!name_part.is_null()) {
                        callee = std::string(name_part.text(ctx.source));
                        candidates.push_back(callee);
                        target_node = name_part;
                        ctx.handled_identifier_byte_starts.insert(name_part.start_byte());
                    }
                    treesitter::Node tmpl_args = field.child_by_field_name("arguments");
                    if (!tmpl_args.is_null()) {
                        walk_node(tmpl_args, ctx);
                    }
                } else if (field.type() == "dependent_name") {
                    treesitter::Node inner_method;
                    for (uint32_t i = 0; i < field.named_child_count(); ++i) {
                        auto child = field.named_child(i);
                        if (child.type() == "template_method" ||
                            child.type() == "field_identifier") {
                            inner_method = child;
                            break;
                        }
                    }
                    if (!inner_method.is_null() && inner_method.type() == "template_method") {
                        treesitter::Node name_part = inner_method.child_by_field_name("name");
                        if (!name_part.is_null()) {
                            callee = std::string(name_part.text(ctx.source));
                            candidates.push_back(callee);
                            target_node = name_part;
                            ctx.handled_identifier_byte_starts.insert(name_part.start_byte());
                        }
                        treesitter::Node tmpl_args = inner_method.child_by_field_name("arguments");
                        if (!tmpl_args.is_null()) {
                            walk_node(tmpl_args, ctx);
                        }
                    } else if (!inner_method.is_null()) {
                        callee = std::string(inner_method.text(ctx.source));
                        candidates.push_back(callee);
                        target_node = inner_method;
                        ctx.handled_identifier_byte_starts.insert(inner_method.start_byte());
                    } else {
                        callee = std::string(field.text(ctx.source));
                        candidates.push_back(callee);
                        target_node = field;
                        ctx.handled_identifier_byte_starts.insert(field.start_byte());
                    }
                } else {
                    callee = std::string(field.text(ctx.source));
                    candidates.push_back(callee);
                    target_node = field;
                    ctx.handled_identifier_byte_starts.insert(field.start_byte());
                }
            }
            treesitter::Node arg = func_node.child_by_field_name("argument");
            if (!arg.is_null()) {
                walk_node(arg, ctx);
            }
        } else if (func_node.type() == "qualified_identifier") {
            callee = std::string(func_node.text(ctx.source));
            candidates.push_back(callee);
            treesitter::Node name_part = func_node.child_by_field_name("name");
            if (!name_part.is_null()) {
                std::string short_name = std::string(name_part.text(ctx.source));
                if (short_name != callee) {
                    candidates.push_back(short_name);
                }
            }
            target_node = func_node;
            ctx.handled_identifier_byte_starts.insert(func_node.start_byte());
        } else if (func_node.type() == "template_function") {
            treesitter::Node name_part = func_node.child_by_field_name("name");
            if (!name_part.is_null()) {
                callee = std::string(name_part.text(ctx.source));
                candidates.push_back(callee);
                target_node = name_part;
                ctx.handled_identifier_byte_starts.insert(name_part.start_byte());
            }
            treesitter::Node args = func_node.child_by_field_name("arguments");
            if (!args.is_null()) {
                walk_node(args, ctx);
            }
        } else if (func_node.type() == "template_method") {
            treesitter::Node name_part = func_node.child_by_field_name("name");
            if (!name_part.is_null()) {
                callee = std::string(name_part.text(ctx.source));
                candidates.push_back(callee);
                target_node = name_part;
                ctx.handled_identifier_byte_starts.insert(name_part.start_byte());
            }
            treesitter::Node tmpl_args = func_node.child_by_field_name("arguments");
            if (!tmpl_args.is_null()) {
                walk_node(tmpl_args, ctx);
            }
            treesitter::Node recv = func_node.child_by_field_name("argument");
            if (!recv.is_null()) {
                walk_node(recv, ctx);
            } else if (func_node.parent().type() == "field_expression") {
                treesitter::Node parent_recv = func_node.parent().child_by_field_name("argument");
                if (!parent_recv.is_null()) {
                    walk_node(parent_recv, ctx);
                }
            } else if (func_node.parent().type() == "dependent_name" &&
                       func_node.parent().parent().type() == "field_expression") {
                treesitter::Node parent_recv =
                    func_node.parent().parent().child_by_field_name("argument");
                if (!parent_recv.is_null()) {
                    walk_node(parent_recv, ctx);
                }
            }
        } else {
            walk_node(func_node, ctx);
        }

        if (!callee.empty()) {
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::call,
                .written_name = callee,
                .range = target_node.byte_range(),
                .display_range = target_node.display_range(),
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

void process_template_declaration(treesitter::Node node, ASTContext& ctx) {
    std::unordered_set<std::string> saved_params = ctx.template_param_names;

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "template_parameter_list" || ch.type() == "parameters") {
            for (uint32_t j = 0; j < ch.named_child_count(); ++j) {
                auto param = ch.named_child(j);
                treesitter::Node id = param.child_by_field_name("name");
                if (!id.is_null()) {
                    ctx.handled_identifier_byte_starts.insert(id.start_byte());
                    ctx.template_param_names.insert(std::string(id.text(ctx.source)));
                } else {
                    for (uint32_t k = 0; k < param.child_count(); ++k) {
                        auto pk = param.child(k);
                        if (pk.type() == "type_identifier" || pk.type() == "identifier") {
                            ctx.handled_identifier_byte_starts.insert(pk.start_byte());
                            ctx.template_param_names.insert(std::string(pk.text(ctx.source)));
                        }
                    }
                }
            }
        }
    }

    for (uint32_t i = 0; i < node.child_count(); ++i) {
        auto ch = node.child(i);
        if (ch.type() == "template_parameter_list" || ch.type() == "parameters" ||
            ch.type() == "requires_clause" || ch.type() == "template") {
            continue;
        }
        walk_node(ch, ctx);
    }

    ctx.template_param_names = std::move(saved_params);
}

void walk_node(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null()) {
        return;
    }
    if (ctx.stop_token.stop_requested()) {
        return;
    }

    const auto type = node.type();

    if (type == "preproc_include") {
        process_include(node, ctx);
        return;
    }

    if (type == "preproc_def" || type == "preproc_function_def") {
        process_macro(node, ctx);
        return;
    }

    if (type == "namespace_definition") {
        process_namespace(node, ctx);
        return;
    }

    if (type == "namespace_alias_definition") {
        process_namespace_alias(node, ctx);
        return;
    }

    if (type == "using_declaration") {
        process_using_declaration(node, ctx);
        return;
    }

    if (type == "alias_declaration") {
        process_alias_declaration(node, ctx);
        return;
    }

    if (type == "type_definition") {
        process_type_definition(node, ctx);
        return;
    }

    if (type == "class_specifier" || type == "struct_specifier" || type == "union_specifier") {
        process_class_or_struct(node, ctx);
        return;
    }

    if (type == "enum_specifier") {
        process_enum(node, ctx);
        return;
    }

    if (type == "enumerator") {
        process_enumerator(node, ctx);
        return;
    }

    if (type == "template_declaration") {
        process_template_declaration(node, ctx);
        return;
    }

    if (type == "function_definition") {
        process_function_definition(node, ctx);
        return;
    }

    if (type == "field_declaration") {
        process_field_declaration(node, ctx);
        return;
    }

    if (type == "declaration") {
        process_declaration(node, ctx);
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
            if (name == "#undef") {
                treesitter::Node arg = node.child_by_field_name("argument");
                if (!arg.is_null()) {
                    std::string undef_name = clean_signature(arg.text(ctx.source));
                    ctx.defined_macros.erase(undef_name);
                    ctx.undefined_macros.insert(undef_name);
                }
            }
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

    if (type == "parameter_declaration" || type == "optional_parameter_declaration" ||
        type == "variadic_parameter_declaration") {
        process_parameter_declaration(node, ctx);
        return;
    }

    if (type == "preproc_ifdef" || type == "preproc_ifndef" || type == "preproc_if" ||
        type == "preproc_elifdef" || type == "preproc_elifndef" || type == "preproc_elif") {
        process_preproc_conditional(node, ctx);
        return;
    }

    if (type == "template_method") {
        treesitter::Node name_node = node.child_by_field_name("name");
        if (!name_node.is_null() &&
            !ctx.handled_identifier_byte_starts.contains(name_node.start_byte())) {
            const std::string name = std::string(name_node.text(ctx.source));
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::reference,
                .written_name = name,
                .range = name_node.byte_range(),
                .display_range = name_node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {name},
            });
            ctx.handled_identifier_byte_starts.insert(name_node.start_byte());
        }
        treesitter::Node tmpl_args = node.child_by_field_name("arguments");
        if (!tmpl_args.is_null()) {
            walk_node(tmpl_args, ctx);
        }
        return;
    }

    if (type == "dependent_name") {
        for (uint32_t i = 0; i < node.named_child_count(); ++i) {
            walk_node(node.named_child(i), ctx);
        }
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
            if (!ctx.template_param_names.contains(name)) {
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
            }
            ctx.handled_identifier_byte_starts.insert(node.start_byte());
        }
        return;
    }

    if (type == "identifier") {
        if (!ctx.handled_identifier_byte_starts.contains(node.start_byte())) {
            const std::string name = std::string(node.text(ctx.source));
            if (!ctx.macro_param_names.contains(name) && !ctx.template_param_names.contains(name)) {
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
            }
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

CppAdapter::CppAdapter() {
    capabilities_ = {
        .functions = CapabilityStatus::supported,
        .methods = CapabilityStatus::supported,
        .classes = CapabilityStatus::supported,
        .structs = CapabilityStatus::supported,
        .interfaces = CapabilityStatus::unavailable,
        .enums = CapabilityStatus::supported,
        .records = CapabilityStatus::unavailable,
        .namespaces = CapabilityStatus::supported,
        .variables = CapabilityStatus::supported,
        .modules = CapabilityStatus::unavailable,
        .packages = CapabilityStatus::unavailable,
        .templates = CapabilityStatus::supported,
        .partial_types = CapabilityStatus::unavailable,
        .containment = CapabilityStatus::supported,
        .calls = CapabilityStatus::supported,
        .references = CapabilityStatus::supported,
        .inheritance = CapabilityStatus::supported,
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

const LanguageCapabilities& CppAdapter::capabilities() const noexcept {
    return capabilities_;
}

void CppAdapter::set_compile_command_context(CompileCommandContext context) {
    compile_context_ = std::move(context);
}

void CppAdapter::clear_compile_command_context() noexcept {
    compile_context_.reset();
}

const std::optional<CompileCommandContext>& CppAdapter::compile_command_context() const noexcept {
    return compile_context_;
}

Result<AdapterResult> CppAdapter::parse(std::string_view source,
                                        const std::filesystem::path& file_path,
                                        const std::stop_token& stop_token) {
    if (compile_context_.has_value()) {
        return parse(source, file_path, *compile_context_, stop_token);
    }
    return parse(source, file_path, CompileCommandContext{}, stop_token);
}

Result<AdapterResult> CppAdapter::parse(std::string_view source,
                                        const std::filesystem::path& file_path,
                                        const CompileCommandContext& context,
                                        const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        return unexpected_result<AdapterResult>(ErrorCode::cancelled, "parsing cancelled");
    }

    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(Language::cpp);
    if (ts_lang == nullptr) {
        return unexpected_result<AdapterResult>(ErrorCode::invalid_argument,
                                                "C++ grammar not available");
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
        .language = Language::cpp,
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
    result.diagnostics = collect_syntax_errors(root, Language::cpp);

    // Validate language standard if specified
    if (context.language_standard.has_value() && !context.language_standard->empty()) {
        std::string std_str = *context.language_standard;
        if (std_str.starts_with("-std=")) {
            std_str = std_str.substr(5);
        } else if (std_str.starts_with("--std=")) {
            std_str = std_str.substr(6);
        }

        std::string lower_std = std_str;
        std::ranges::transform(lower_std, lower_std.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        static const std::unordered_set<std::string> valid_cpp_standards = {
            "c++98",         "c++03",         "c++11",         "c++0x",         "c++14",
            "c++1y",         "c++17",         "c++1z",         "c++20",         "c++2a",
            "c++23",         "c++2b",         "c++26",         "c++2c",         "gnu++98",
            "gnu++03",       "gnu++11",       "gnu++0x",       "gnu++14",       "gnu++1y",
            "gnu++17",       "gnu++1z",       "gnu++20",       "gnu++2a",       "gnu++23",
            "gnu++2b",       "gnu++26",       "gnu++2c",       "iso14882:1998", "iso14882:2003",
            "iso14882:2011", "iso14882:2014", "iso14882:2017", "iso14882:2020", "iso14882:2023",
        };

        if (!valid_cpp_standards.contains(lower_std)) {
            result.diagnostics.push_back(ParseDiagnostic{
                .severity = DiagnosticSeverity::warning,
                .language = Language::cpp,
                .code = "unsupported_standard",
                .message = "Unrecognized or non-C++ language standard specified: " +
                           *context.language_standard,
                .byte_range = ByteRange{.start = 0, .end = 0},
                .display_range =
                    DisplayRange{
                        .start_line = 1, .start_column = 1, .end_line = 1, .end_column = 1},
            });
        }
    }

    // Process active macro defines (-D and -U)
    // NOTE: Command-line macros have no source location in the file, so synthetic range {0, 0}
    // is used to make them resolvable across the translation unit.
    const auto active_macros = context.active_defines();

    std::unordered_set<std::string> cmd_macro_names;
    for (const auto& m : active_macros) {
        std::string sig = "#define " + m.name + (m.value.empty() ? "" : " " + m.value);
        result.symbols.push_back(SymbolFact{
            .name = m.name,
            .qualified_name = m.name,
            .kind = NodeKind::macro,
            .range = ByteRange{.start = 0, .end = 0},
            .display_range =
                DisplayRange{.start_line = 1, .start_column = 1, .end_line = 1, .end_column = 1},
            .enclosing_scope = std::nullopt,
            .signature = sig,
        });

        result.declarations.push_back(DeclarationFact{
            .symbol_name = m.name,
            .qualified_name = m.name,
            .kind = NodeKind::macro,
            .range = ByteRange{.start = 0, .end = 0},
            .display_range =
                DisplayRange{.start_line = 1, .start_column = 1, .end_line = 1, .end_column = 1},
            .enclosing_scope = std::nullopt,
            .is_definition = true,
        });

        cmd_macro_names.insert(m.name);
    }

    std::unordered_map<std::string, std::string> initial_defined_macros;
    initial_defined_macros["__cplusplus"] = cplusplus_macro_value(context.language_standard);
    std::unordered_set<std::string> initial_undefined_macros;

    for (const auto& m : active_macros) {
        initial_defined_macros[m.name] = m.value;
    }
    for (const auto& def_arg : context.defines) {
        if (def_arg.starts_with("-U")) {
            initial_undefined_macros.insert(def_arg.substr(2));
        }
    }

    ASTContext ctx{
        .source = source,
        .stop_token = stop_token,
        .result = result,
        .scope_stack = {},
        .handled_identifier_byte_starts = {},
        .macro_param_names = {},
        .template_param_names = {},
        .compile_context = has_context ? &context : nullptr,
        .file_path = file_path,
        .command_line_macro_names = std::move(cmd_macro_names),
        .defined_macros = std::move(initial_defined_macros),
        .undefined_macros = std::move(initial_undefined_macros),
        .class_depth = 0,
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

std::string_view CppAdapter::highlighting_query() noexcept {
    static constexpr std::string_view kHighlightQuery = R"(
;; Generic identifiers as variables (low precedence at top)
(identifier) @variable

;; Comments
(comment) @comment

;; Keywords
"break" @keyword
"case" @keyword
"catch" @keyword
"class" @keyword
"co_await" @keyword
"co_return" @keyword
"co_yield" @keyword
"concept" @keyword
"const" @keyword
"consteval" @keyword
"constexpr" @keyword
"constinit" @keyword
"continue" @keyword
"default" @keyword
"delete" @keyword
"do" @keyword
"else" @keyword
"enum" @keyword
"explicit" @keyword
"extern" @keyword
"final" @keyword
"for" @keyword
"friend" @keyword
"goto" @keyword
"if" @keyword
"inline" @keyword
"mutable" @keyword
"namespace" @keyword
"new" @keyword
"noexcept" @keyword
"override" @keyword
"private" @keyword
"protected" @keyword
"public" @keyword
"requires" @keyword
"return" @keyword
"sizeof" @keyword
"static" @keyword
"struct" @keyword
"switch" @keyword
"template" @keyword
(this) @variable
"throw" @keyword
"try" @keyword
"typedef" @keyword
"typename" @keyword
"union" @keyword
"using" @keyword
"virtual" @keyword
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
"->*" @operator
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
"<=>" @operator
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
"::" @operator
"." @operator
".*" @operator

;; Literals
(string_literal) @string
(system_lib_string) @string
(raw_string_literal) @string
(char_literal) @string
(number_literal) @number
(null) @keyword
(true) @keyword
(false) @keyword

;; Types
(primitive_type) @type
(type_identifier) @type
(auto) @type
(namespace_identifier) @type

;; Fields / Properties
(field_identifier) @property

;; Labels
(statement_identifier) @label

;; Macros
(preproc_def
  name: (identifier) @macro)

(preproc_function_def
  name: (identifier) @macro)

;; Functions and Methods
(function_declarator
  declarator: (identifier) @function)

(function_declarator
  declarator: (field_identifier) @function)

(function_declarator
  declarator: (qualified_identifier
    name: (identifier) @function))

(function_definition
  declarator: (function_declarator
    declarator: (identifier) @function.definition))

(function_definition
  declarator: (function_declarator
    declarator: (qualified_identifier
      name: (identifier) @function.definition)))

(call_expression
  function: (identifier) @function)

(call_expression
  function: (field_expression
    field: (field_identifier) @function))

(call_expression
  function: (qualified_identifier
    name: (identifier) @function))

(template_function
  name: (identifier) @function)

(template_method
  name: (field_identifier) @function)
)";
    return kHighlightQuery;
}

Result<std::vector<HighlightToken>> CppAdapter::highlight(std::string_view source,
                                                          const treesitter::Tree& tree) {
    const auto* ts_lang = treesitter::grammar_for_language(Language::cpp);
    if (ts_lang == nullptr) {
        return unexpected_result<std::vector<HighlightToken>>(ErrorCode::invalid_argument,
                                                              "C++ grammar not available");
    }

    static std::string s_query_error;
    static const auto s_query = []() -> std::optional<treesitter::Query> {
        const auto* lang = treesitter::grammar_for_language(Language::cpp);
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

Result<std::vector<HighlightToken>> CppAdapter::highlight(std::string_view source) {
    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(Language::cpp);
    if (ts_lang == nullptr) {
        return unexpected_result<std::vector<HighlightToken>>(ErrorCode::invalid_argument,
                                                              "C++ grammar not available");
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
