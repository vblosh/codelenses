#include "codelenses/adapters/python_adapter.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
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

struct ScopeEntry {
    std::string name;
    bool is_class{false};
};

struct ASTContext {
    std::string_view source;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    const std::stop_token& stop_token;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    AdapterResult& result;
    std::vector<ScopeEntry> scope_stack;
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
            full += scope_stack[i].name;
        }
        return full;
    }

    [[nodiscard]] std::optional<std::string> current_class_name() const {
        for (const auto& entry : std::ranges::reverse_view(scope_stack)) {
            if (entry.is_class) {
                return entry.name;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool is_module_or_class_scope() const {
        if (scope_stack.empty()) {
            return true;
        }
        return scope_stack.back().is_class;
    }

    void push_scope(std::string name, bool is_class) {
        scope_stack.push_back(ScopeEntry{.name = std::move(name), .is_class = is_class});
    }

    void pop_scope() {
        if (!scope_stack.empty()) {
            scope_stack.pop_back();
        }
    }
};

std::string escape_json_string(std::string_view str) {
    std::string out;
    out.reserve(str.size() + 4);
    for (char c : str) {
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += c;
        }
    }
    return out;
}

void mark_handled_identifiers(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null()) {
        return;
    }
    if (node.type() == "identifier") {
        ctx.handled_identifier_byte_starts.insert(node.start_byte());
    }
    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        mark_handled_identifiers(node.child(i), ctx);
    }
}

void collect_target_identifiers(treesitter::Node node, std::vector<treesitter::Node>& out) {
    if (node.is_null()) {
        return;
    }
    if (node.type() == "identifier") {
        out.push_back(node);
        return;
    }
    if (node.type() == "pattern_list" || node.type() == "tuple_pattern" ||
        node.type() == "list_pattern" || node.type() == "parenthesized_expression") {
        const uint32_t count = node.child_count();
        for (uint32_t i = 0; i < count; ++i) {
            collect_target_identifiers(node.child(i), out);
        }
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

std::string build_function_signature(treesitter::Node node, ASTContext& ctx) {
    std::string sig;
    bool is_async = false;
    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        if (node.child(i).type() == "async") {
            is_async = true;
            break;
        }
    }
    if (is_async) {
        sig += "async ";
    }
    sig += "def ";

    treesitter::Node name_node = node.child_by_field_name("name");
    if (!name_node.is_null()) {
        sig += name_node.text(ctx.source);
    }

    treesitter::Node type_params = node.child_by_field_name("type_parameters");
    if (!type_params.is_null()) {
        sig += type_params.text(ctx.source);
    }

    treesitter::Node params_node = node.child_by_field_name("parameters");
    if (!params_node.is_null()) {
        sig += params_node.text(ctx.source);
    } else {
        sig += "()";
    }

    treesitter::Node ret_node = node.child_by_field_name("return_type");
    if (!ret_node.is_null()) {
        sig += " -> ";
        sig += ret_node.text(ctx.source);
    }

    return clean_signature(sig);
}

void walk_node(treesitter::Node node, ASTContext& ctx);

void mark_handled_attribute_chain(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null()) {
        return;
    }
    if (node.type() == "attribute") {
        treesitter::Node attr = node.child_by_field_name("attribute");
        if (!attr.is_null()) {
            mark_handled_identifiers(attr, ctx);
        }
        treesitter::Node obj = node.child_by_field_name("object");
        if (!obj.is_null()) {
            if (obj.type() == "identifier") {
                mark_handled_identifiers(obj, ctx);
            } else if (obj.type() == "attribute") {
                mark_handled_attribute_chain(obj, ctx);
            } else {
                walk_node(obj, ctx);
            }
        }
    }
}

void process_import_statement(treesitter::Node node, ASTContext& ctx) {
    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        auto ch = node.child(i);
        if (ch.type() == "dotted_name") {
            const std::string name = std::string(ch.text(ctx.source));
            mark_handled_identifiers(ch, ctx);

            std::vector<std::string> cands = {name};
            const auto dot = name.find('.');
            if (dot != std::string::npos) {
                cands.push_back(name.substr(0, dot));
            }

            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::import,
                .written_name = name,
                .range = ch.byte_range(),
                .display_range = ch.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = std::move(cands),
                .confidence = 1.0,
            });
        } else if (ch.type() == "aliased_import") {
            treesitter::Node orig_name_node = ch.child_by_field_name("name");
            if (orig_name_node.is_null()) {
                orig_name_node = ch.child(0);
            }
            treesitter::Node alias_node = ch.child_by_field_name("alias");

            const std::string orig_name = std::string(orig_name_node.text(ctx.source));
            mark_handled_identifiers(orig_name_node, ctx);

            std::vector<std::string> cands = {orig_name};
            const auto dot = orig_name.find('.');
            if (dot != std::string::npos) {
                cands.push_back(orig_name.substr(0, dot));
            }

            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::import,
                .written_name = orig_name,
                .range = orig_name_node.byte_range(),
                .display_range = orig_name_node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = std::move(cands),
                .confidence = 1.0,
            });

            if (!alias_node.is_null()) {
                const std::string alias_name = std::string(alias_node.text(ctx.source));
                mark_handled_identifiers(alias_node, ctx);

                const auto scope = ctx.current_scope();
                const std::string qname = scope ? *scope + "." + alias_name : alias_name;
                std::string sig = "import ";
                sig += orig_name;
                sig += " as ";
                sig += alias_name;

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
            }
        }
    }
}

void process_import_from_statement(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node mod_node = node.child_by_field_name("module_name");
    std::string mod_text;

    if (!mod_node.is_null()) {
        mod_text = std::string(mod_node.text(ctx.source));
        mark_handled_identifiers(mod_node, ctx);
    } else if (node.type() == "future_import_statement") {
        mod_text = "__future__";
        for (uint32_t i = 0; i < node.child_count(); ++i) {
            auto ch = node.child(i);
            if (ch.type() == "identifier" || ch.type() == "dotted_name") {
                if (ch.text(ctx.source) == "__future__") {
                    mark_handled_identifiers(ch, ctx);
                }
            }
        }
    } else {
        // Collect tokens between "from" and "import" for relative imports like `from . import foo`
        bool past_from = false;
        const uint32_t count = node.child_count();
        for (uint32_t i = 0; i < count; ++i) {
            auto ch = node.child(i);
            if (ch.type() == "from") {
                past_from = true;
                continue;
            }
            if (ch.type() == "import") {
                break;
            }
            if (past_from) {
                mod_text += std::string(ch.text(ctx.source));
                mark_handled_identifiers(ch, ctx);
            }
        }
    }

    auto handle_import_item = [&](treesitter::Node ch) {
        if (ch.type() == "dotted_name" || ch.type() == "identifier") {
            const std::string item_name = std::string(ch.text(ctx.source));
            mark_handled_identifiers(ch, ctx);

            std::string target;
            if (mod_text.empty()) {
                target = item_name;
            } else if (mod_text.ends_with('.')) {
                target = mod_text + item_name;
            } else {
                target = mod_text + "." + item_name;
            }

            std::vector<std::string> cands = {target};
            if (target != item_name) {
                cands.push_back(item_name);
            }

            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::import,
                .written_name = item_name,
                .range = ch.byte_range(),
                .display_range = ch.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = std::move(cands),
                .confidence = 1.0,
            });
        } else if (ch.type() == "aliased_import") {
            treesitter::Node orig_name_node = ch.child_by_field_name("name");
            if (orig_name_node.is_null()) {
                orig_name_node = ch.child(0);
            }
            treesitter::Node alias_node = ch.child_by_field_name("alias");

            const std::string item_name = std::string(orig_name_node.text(ctx.source));
            mark_handled_identifiers(orig_name_node, ctx);

            std::string target;
            if (mod_text.empty()) {
                target = item_name;
            } else if (mod_text.ends_with('.')) {
                target = mod_text + item_name;
            } else {
                target = mod_text + "." + item_name;
            }

            std::vector<std::string> cands = {target};
            if (target != item_name) {
                cands.push_back(item_name);
            }

            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::import,
                .written_name = item_name,
                .range = orig_name_node.byte_range(),
                .display_range = orig_name_node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = std::move(cands),
                .confidence = 1.0,
            });

            if (!alias_node.is_null()) {
                const std::string alias_name = std::string(alias_node.text(ctx.source));
                mark_handled_identifiers(alias_node, ctx);

                const auto scope = ctx.current_scope();
                const std::string qname = scope ? *scope + "." + alias_name : alias_name;
                std::string sig = "from ";
                sig += mod_text;
                sig += " import ";
                sig += item_name;
                sig += " as ";
                sig += alias_name;

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
            }
        } else if (ch.type() == "wildcard_import") {
            std::string target;
            if (mod_text.empty()) {
                target = "*";
            } else if (mod_text.ends_with('.')) {
                target = mod_text + "*";
            } else {
                target = mod_text + ".*";
            }

            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::import,
                .written_name = "*",
                .range = ch.byte_range(),
                .display_range = ch.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {target},
                .confidence = 1.0,
            });
        }
    };

    bool past_import_keyword = false;
    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        auto ch = node.child(i);
        if (!past_import_keyword) {
            if (ch.type() == "import") {
                past_import_keyword = true;
            }
            continue;
        }

        handle_import_item(ch);
    }
}

void process_class(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");

    std::string name;
    if (!name_node.is_null()) {
        name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + name : name;
        const std::string signature = "class " + name;

        ctx.result.symbols.push_back(SymbolFact{
            .name = name,
            .qualified_name = qname,
            .kind = NodeKind::class_,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = signature,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = name,
            .qualified_name = qname,
            .kind = NodeKind::class_,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = true,
        });

        treesitter::Node superclasses_node = node.child_by_field_name("superclasses");
        if (!superclasses_node.is_null()) {
            const uint32_t sc_count = superclasses_node.child_count();
            for (uint32_t i = 0; i < sc_count; ++i) {
                auto arg = superclasses_node.child(i);
                if (arg.type() == "identifier" || arg.type() == "attribute") {
                    const std::string base_name = std::string(arg.text(ctx.source));
                    mark_handled_identifiers(arg, ctx);

                    std::vector<std::string> cands = {base_name};
                    if (arg.type() == "attribute") {
                        treesitter::Node attr = arg.child_by_field_name("attribute");
                        if (!attr.is_null()) {
                            std::string unqual = std::string(attr.text(ctx.source));
                            if (unqual != base_name) {
                                cands.push_back(std::move(unqual));
                            }
                        }
                    }

                    ctx.result.occurrences.push_back(OccurrenceFact{
                        .kind = worker::FactKind::inheritance,
                        .written_name = base_name,
                        .range = arg.byte_range(),
                        .display_range = arg.display_range(),
                        .enclosing_scope = qname,
                        .candidate_targets = std::move(cands),
                        .confidence = 1.0,
                    });
                } else if (arg.type() == "subscript") {
                    treesitter::Node val = arg.child_by_field_name("value");
                    if (!val.is_null()) {
                        const std::string base_name = std::string(val.text(ctx.source));
                        mark_handled_identifiers(val, ctx);
                        ctx.result.occurrences.push_back(OccurrenceFact{
                            .kind = worker::FactKind::inheritance,
                            .written_name = base_name,
                            .range = val.byte_range(),
                            .display_range = val.display_range(),
                            .enclosing_scope = qname,
                            .candidate_targets = {base_name},
                            .confidence = 1.0,
                        });
                    }
                    treesitter::Node sub = arg.child_by_field_name("subscript");
                    if (!sub.is_null()) {
                        walk_node(sub, ctx);
                    }
                } else if (arg.type() == "keyword_argument") {
                    treesitter::Node val = arg.child_by_field_name("value");
                    if (!val.is_null()) {
                        walk_node(val, ctx);
                    }
                }
            }
        }
    }

    if (!body_node.is_null()) {
        if (!name.empty()) {
            ctx.push_scope(name, /*is_class=*/true);
        }
        walk_node(body_node, ctx);
        if (!name.empty()) {
            ctx.pop_scope();
        }
    }
}

void process_function(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node name_node = node.child_by_field_name("name");
    treesitter::Node body_node = node.child_by_field_name("body");

    std::string fn_name;
    if (!name_node.is_null()) {
        fn_name = std::string(name_node.text(ctx.source));
        ctx.handled_identifier_byte_starts.insert(name_node.start_byte());

        const auto scope = ctx.current_scope();
        const std::string qname = scope ? *scope + "." + fn_name : fn_name;
        const NodeKind kind = (!ctx.scope_stack.empty() && ctx.scope_stack.back().is_class)
                                  ? NodeKind::method
                                  : NodeKind::function;

        const std::string signature = build_function_signature(node, ctx);

        ctx.result.symbols.push_back(SymbolFact{
            .name = fn_name,
            .qualified_name = qname,
            .kind = kind,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .signature = signature,
        });

        ctx.result.declarations.push_back(DeclarationFact{
            .symbol_name = fn_name,
            .qualified_name = qname,
            .kind = kind,
            .range = name_node.byte_range(),
            .display_range = name_node.display_range(),
            .enclosing_scope = scope,
            .is_definition = !body_node.is_null(),
        });
    }

    treesitter::Node params_node = node.child_by_field_name("parameters");
    if (!params_node.is_null()) {
        const uint32_t p_count = params_node.child_count();
        for (uint32_t i = 0; i < p_count; ++i) {
            auto p = params_node.child(i);
            if (p.type() == "identifier") {
                ctx.handled_identifier_byte_starts.insert(p.start_byte());
            } else if (p.type() == "default_parameter") {
                treesitter::Node pname = p.child_by_field_name("name");
                if (!pname.is_null()) {
                    ctx.handled_identifier_byte_starts.insert(pname.start_byte());
                }
                treesitter::Node pval = p.child_by_field_name("value");
                if (!pval.is_null()) {
                    walk_node(pval, ctx);
                }
            } else if (p.type() == "typed_parameter") {
                for (uint32_t j = 0; j < p.child_count(); ++j) {
                    auto ch = p.child(j);
                    if (ch.type() == "identifier") {
                        ctx.handled_identifier_byte_starts.insert(ch.start_byte());
                        break;
                    }
                }
                treesitter::Node ptype = p.child_by_field_name("type");
                if (!ptype.is_null()) {
                    walk_node(ptype, ctx);
                }
            } else if (p.type() == "typed_default_parameter") {
                treesitter::Node pname = p.child_by_field_name("name");
                if (!pname.is_null()) {
                    ctx.handled_identifier_byte_starts.insert(pname.start_byte());
                }
                treesitter::Node ptype = p.child_by_field_name("type");
                if (!ptype.is_null()) {
                    walk_node(ptype, ctx);
                }
                treesitter::Node pval = p.child_by_field_name("value");
                if (!pval.is_null()) {
                    walk_node(pval, ctx);
                }
            } else {
                walk_node(p, ctx);
            }
        }
    }

    treesitter::Node ret_node = node.child_by_field_name("return_type");
    if (!ret_node.is_null()) {
        walk_node(ret_node, ctx);
    }

    if (!body_node.is_null()) {
        if (!fn_name.empty()) {
            ctx.push_scope(fn_name, /*is_class=*/false);
        }
        walk_node(body_node, ctx);
        if (!fn_name.empty()) {
            ctx.pop_scope();
        }
    }
}

void process_call(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node func_node = node.child_by_field_name("function");
    if (!func_node.is_null()) {
        if (func_node.type() == "identifier") {
            const std::string callee = std::string(func_node.text(ctx.source));
            ctx.handled_identifier_byte_starts.insert(func_node.start_byte());

            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::call,
                .written_name = callee,
                .range = func_node.byte_range(),
                .display_range = func_node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {callee},
                .confidence = 1.0,
            });
        } else if (func_node.type() == "attribute") {
            const std::string full_call = std::string(func_node.text(ctx.source));
            treesitter::Node obj_node = func_node.child_by_field_name("object");
            treesitter::Node attr_node = func_node.child_by_field_name("attribute");

            if (!attr_node.is_null()) {
                const std::string method_name = std::string(attr_node.text(ctx.source));
                const std::string obj_text =
                    !obj_node.is_null() ? std::string(obj_node.text(ctx.source)) : "";

                ctx.handled_identifier_byte_starts.insert(attr_node.start_byte());
                if (!obj_node.is_null() && obj_node.type() == "identifier") {
                    ctx.handled_identifier_byte_starts.insert(obj_node.start_byte());
                }

                std::vector<std::string> candidates = {full_call, method_name};
                double conf = 0.5;
                std::string meta;

                if (obj_text == "self" || obj_text == "cls") {
                    auto cls_name = ctx.current_class_name();
                    if (cls_name.has_value()) {
                        candidates.insert(candidates.begin(), *cls_name + "." + method_name);
                    }
                    conf = 1.0;
                    meta = R"({"receiver":")" + obj_text + R"(","is_method":true})";
                } else if (obj_text.starts_with("super(") || obj_text == "super") {
                    conf = 0.9;
                    candidates.insert(candidates.begin(), "super." + method_name);
                    meta = R"({"receiver":"super","is_super":true})";
                } else {
                    conf = 0.5;
                    meta =
                        R"({"dynamic":true,"receiver":")" + escape_json_string(obj_text) + R"("})";
                }

                ctx.result.occurrences.push_back(OccurrenceFact{
                    .kind = worker::FactKind::call,
                    .written_name = full_call,
                    .range = func_node.byte_range(),
                    .display_range = func_node.display_range(),
                    .enclosing_scope = ctx.current_scope(),
                    .candidate_targets = std::move(candidates),
                    .confidence = conf,
                    .metadata_json = std::move(meta),
                });

                if (!obj_node.is_null() && obj_node.type() != "identifier") {
                    walk_node(obj_node, ctx);
                }
            }
        } else {
            walk_node(func_node, ctx);
        }
    }

    treesitter::Node args_node = node.child_by_field_name("arguments");
    if (!args_node.is_null()) {
        walk_node(args_node, ctx);
    }
}

void process_attribute(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node obj_node = node.child_by_field_name("object");
    treesitter::Node attr_node = node.child_by_field_name("attribute");
    if (attr_node.is_null()) {
        return;
    }

    const std::string full_name = std::string(node.text(ctx.source));
    const std::string attr_name = std::string(attr_node.text(ctx.source));
    const std::string obj_text = !obj_node.is_null() ? std::string(obj_node.text(ctx.source)) : "";

    mark_handled_identifiers(attr_node, ctx);
    if (!obj_node.is_null()) {
        if (obj_node.type() == "identifier") {
            mark_handled_identifiers(obj_node, ctx);
        } else if (obj_node.type() == "attribute") {
            mark_handled_attribute_chain(obj_node, ctx);
        } else {
            walk_node(obj_node, ctx);
        }
    }

    std::vector<std::string> candidates = {full_name, attr_name};
    double conf = 0.5;
    std::string meta;

    if (obj_text == "self" || obj_text == "cls") {
        auto cls_name = ctx.current_class_name();
        if (cls_name.has_value()) {
            candidates.insert(candidates.begin(), *cls_name + "." + attr_name);
        }
        conf = 0.9;
        meta = R"({"receiver":")" + obj_text + R"(","is_attribute":true})";
    } else if (obj_text.starts_with("super(") || obj_text == "super") {
        conf = 0.9;
        candidates.insert(candidates.begin(), "super." + attr_name);
        meta = R"({"receiver":"super","is_super":true})";
    } else {
        conf = 0.5;
        meta = R"({"dynamic":true,"receiver":")" + escape_json_string(obj_text) + R"("})";
    }

    ctx.result.occurrences.push_back(OccurrenceFact{
        .kind = worker::FactKind::reference,
        .written_name = full_name,
        .range = node.byte_range(),
        .display_range = node.display_range(),
        .enclosing_scope = ctx.current_scope(),
        .candidate_targets = std::move(candidates),
        .confidence = conf,
        .metadata_json = std::move(meta),
    });
}

void process_decorated(treesitter::Node node, ASTContext& ctx) {
    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        auto ch = node.child(i);
        if (ch.type() == "decorator") {
            treesitter::Node expr_node{};
            for (uint32_t j = 0; j < ch.child_count(); ++j) {
                auto dch = ch.child(j);
                if (dch.type() != "@") {
                    expr_node = dch;
                    break;
                }
            }

            if (!expr_node.is_null()) {
                if (expr_node.type() == "call") {
                    treesitter::Node func_part = expr_node.child_by_field_name("function");
                    if (!func_part.is_null()) {
                        const std::string name = std::string(func_part.text(ctx.source));
                        mark_handled_identifiers(func_part, ctx);

                        std::vector<std::string> cands = {name};
                        if (func_part.type() == "attribute") {
                            treesitter::Node attr = func_part.child_by_field_name("attribute");
                            if (!attr.is_null()) {
                                std::string unqual = std::string(attr.text(ctx.source));
                                if (unqual != name) {
                                    cands.push_back(std::move(unqual));
                                }
                            }
                        }

                        ctx.result.occurrences.push_back(OccurrenceFact{
                            .kind = worker::FactKind::call,
                            .written_name = name,
                            .range = func_part.byte_range(),
                            .display_range = func_part.display_range(),
                            .enclosing_scope = ctx.current_scope(),
                            .candidate_targets = std::move(cands),
                            .confidence = 1.0,
                            .metadata_json = R"({"is_decorator":true})",
                        });
                    }

                    treesitter::Node args_node = expr_node.child_by_field_name("arguments");
                    if (!args_node.is_null()) {
                        walk_node(args_node, ctx);
                    }
                } else {
                    const std::string name = std::string(expr_node.text(ctx.source));
                    mark_handled_identifiers(expr_node, ctx);

                    std::vector<std::string> cands = {name};
                    if (expr_node.type() == "attribute") {
                        treesitter::Node attr = expr_node.child_by_field_name("attribute");
                        if (!attr.is_null()) {
                            std::string unqual = std::string(attr.text(ctx.source));
                            if (unqual != name) {
                                cands.push_back(std::move(unqual));
                            }
                        }
                    }

                    ctx.result.occurrences.push_back(OccurrenceFact{
                        .kind = worker::FactKind::reference,
                        .written_name = name,
                        .range = expr_node.byte_range(),
                        .display_range = expr_node.display_range(),
                        .enclosing_scope = ctx.current_scope(),
                        .candidate_targets = std::move(cands),
                        .confidence = 1.0,
                        .metadata_json = R"({"is_decorator":true})",
                    });
                }
            }
        }
    }

    treesitter::Node def_node = node.child_by_field_name("definition");
    if (def_node.is_null()) {
        for (uint32_t i = 0; i < count; ++i) {
            auto ch = node.child(i);
            if (ch.type() == "function_definition" || ch.type() == "class_definition") {
                def_node = ch;
                break;
            }
        }
    }

    if (!def_node.is_null()) {
        walk_node(def_node, ctx);
    }
}

void process_assignment(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node left_node = node.child_by_field_name("left");
    treesitter::Node type_node = node.child_by_field_name("type");
    treesitter::Node right_node = node.child_by_field_name("right");

    const bool is_module_or_class = ctx.is_module_or_class_scope();

    if (is_module_or_class && !left_node.is_null()) {
        std::vector<treesitter::Node> targets;
        collect_target_identifiers(left_node, targets);
        for (const auto& id_node : targets) {
            const std::string name = std::string(id_node.text(ctx.source));
            ctx.handled_identifier_byte_starts.insert(id_node.start_byte());

            const auto scope = ctx.current_scope();
            const std::string qname = scope ? *scope + "." + name : name;

            ctx.result.symbols.push_back(SymbolFact{
                .name = name,
                .qualified_name = qname,
                .kind = NodeKind::variable,
                .range = id_node.byte_range(),
                .display_range = id_node.display_range(),
                .enclosing_scope = scope,
                .signature = name,
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
    }

    if (!left_node.is_null()) {
        walk_node(left_node, ctx);
    }
    if (!type_node.is_null()) {
        walk_node(type_node, ctx);
    }
    if (!right_node.is_null()) {
        walk_node(right_node, ctx);
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

    if (type == "import_statement") {
        process_import_statement(node, ctx);
        return;
    }

    if (type == "import_from_statement" || type == "future_import_statement") {
        process_import_from_statement(node, ctx);
        return;
    }

    if (type == "class_definition") {
        process_class(node, ctx);
        return;
    }

    if (type == "function_definition") {
        process_function(node, ctx);
        return;
    }

    if (type == "decorated_definition") {
        process_decorated(node, ctx);
        return;
    }

    if (type == "assignment") {
        process_assignment(node, ctx);
        return;
    }

    if (type == "call") {
        process_call(node, ctx);
        return;
    }

    if (type == "attribute") {
        process_attribute(node, ctx);
        return;
    }

    if (type == "identifier") {
        if (!ctx.handled_identifier_byte_starts.contains(node.start_byte())) {
            const std::string name = std::string(node.text(ctx.source));
            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::reference,
                .written_name = name,
                .range = node.byte_range(),
                .display_range = node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {name},
                .confidence = 1.0,
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

PythonAdapter::PythonAdapter() {
    capabilities_ = {
        .functions = CapabilityStatus::supported,
        .methods = CapabilityStatus::supported,
        .classes = CapabilityStatus::supported,
        .structs = CapabilityStatus::unavailable,
        .interfaces = CapabilityStatus::unavailable,
        .enums = CapabilityStatus::unavailable,
        .records = CapabilityStatus::unavailable,
        .namespaces = CapabilityStatus::unavailable,
        .variables = CapabilityStatus::supported,
        .modules = CapabilityStatus::supported,
        .packages = CapabilityStatus::unavailable,
        .templates = CapabilityStatus::unavailable,
        .partial_types = CapabilityStatus::unavailable,
        .containment = CapabilityStatus::supported,
        .calls = CapabilityStatus::supported,
        .references = CapabilityStatus::supported,
        .inheritance = CapabilityStatus::supported,
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

const LanguageCapabilities& PythonAdapter::capabilities() const noexcept {
    return capabilities_;
}

Result<AdapterResult> PythonAdapter::parse(std::string_view source,
                                           const std::filesystem::path& file_path,
                                           const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        return unexpected_result<AdapterResult>(ErrorCode::cancelled, "parsing cancelled");
    }

    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(Language::python);
    if (ts_lang == nullptr) {
        return unexpected_result<AdapterResult>(ErrorCode::invalid_argument,
                                                "Python grammar not available");
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
        .language = Language::python,
        .status = worker::CompletionStatus::complete,
        .symbols = {},
        .declarations = {},
        .occurrences = {},
        .diagnostics = {},
    };

    treesitter::Node root = tree_res->root_node();
    result.diagnostics = collect_syntax_errors(root, Language::python);

    std::string mod_name =
        (!file_path.empty() && file_path.has_stem()) ? file_path.stem().string() : "__main__";
    if (mod_name == "__init__" && file_path.has_parent_path() &&
        !file_path.parent_path().filename().empty()) {
        mod_name = file_path.parent_path().filename().string();
    }

    result.symbols.push_back(SymbolFact{
        .name = mod_name,
        .qualified_name = mod_name,
        .kind = NodeKind::module,
        .range = root.byte_range(),
        .display_range = root.display_range(),
        .enclosing_scope = std::nullopt,
        .signature = "module " + mod_name,
    });

    result.declarations.push_back(DeclarationFact{
        .symbol_name = mod_name,
        .qualified_name = mod_name,
        .kind = NodeKind::module,
        .range = root.byte_range(),
        .display_range = root.display_range(),
        .enclosing_scope = std::nullopt,
        .is_definition = true,
    });

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
        result.status = (result.symbols.size() <= 1 && result.occurrences.empty())
                            ? worker::CompletionStatus::failed
                            : worker::CompletionStatus::degraded;
    }

    return result;
}

std::string_view PythonAdapter::highlighting_query() noexcept {
    static constexpr std::string_view kHighlightQuery = R"(
;; Keywords
[
  "def"
  "class"
  "import"
  "from"
  "as"
  "return"
  "yield"
  "if"
  "elif"
  "else"
  "for"
  "while"
  "break"
  "continue"
  "try"
  "except"
  "finally"
  "raise"
  "with"
  "assert"
  "del"
  "pass"
  "async"
  "await"
  "global"
  "nonlocal"
  "lambda"
  "match"
  "case"
] @keyword

;; Operators
[
  "and"
  "or"
  "not"
  "in"
  "is"
  "+"
  "-"
  "*"
  "/"
  "//"
  "%"
  "**"
  "=="
  "!="
  "<"
  ">"
  "<="
  ">="
  "="
  "+="
  "-="
  "*="
  "/="
  "//="
  "%="
  "**="
  "&="
  "|="
  "^="
  ">>="
  "<<="
  ":="
  "->"
  "&"
  "|"
  "^"
  "~"
  "<<"
  ">>"
] @operator

;; Literals
(string) @string
(escape_sequence) @string
(integer) @number
(float) @number
(true) @keyword
(false) @keyword
(none) @keyword

;; Comments
(comment) @comment

;; Definitions
(class_definition name: (identifier) @class.definition)
(function_definition name: (identifier) @function.definition)

;; Parameters
(parameters (identifier) @parameter)
(parameters (default_parameter name: (identifier) @parameter))
(parameters (typed_parameter (identifier) @parameter))
(parameters (typed_default_parameter name: (identifier) @parameter))

;; Calls & Attributes
(call function: (identifier) @function)
(call function: (attribute attribute: (identifier) @method))
(attribute object: (identifier) @variable)
(attribute attribute: (identifier) @property)

;; Decorators
(decorator "@" @decorator)
(decorator (identifier) @decorator)
(decorator (attribute attribute: (identifier) @decorator))
(decorator (call function: (identifier) @decorator))
(decorator (call function: (attribute attribute: (identifier) @decorator)))
)";
    return kHighlightQuery;
}

Result<std::vector<HighlightToken>> PythonAdapter::highlight(std::string_view source,
                                                             const treesitter::Tree& tree) {
    const auto* ts_lang = treesitter::grammar_for_language(Language::python);
    if (ts_lang == nullptr) {
        return unexpected_result<std::vector<HighlightToken>>(ErrorCode::invalid_argument,
                                                              "Python grammar not available");
    }

    static std::string s_query_error;
    static const auto s_query = []() -> std::optional<treesitter::Query> {
        const auto* lang = treesitter::grammar_for_language(Language::python);
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

Result<std::vector<HighlightToken>> PythonAdapter::highlight(std::string_view source) {
    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(Language::python);
    if (ts_lang == nullptr) {
        return unexpected_result<std::vector<HighlightToken>>(ErrorCode::invalid_argument,
                                                              "Python grammar not available");
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
