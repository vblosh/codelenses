#include "codelenses/adapters/python_adapter.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

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
        return scope_stack.back().is_class;
    }

    void push_scope(std::string name, bool is_class) {
        scope_stack.push_back(ScopeEntry{std::move(name), is_class});
    }

    void pop_scope() {
        if (!scope_stack.empty()) {
            scope_stack.pop_back();
        }
    }
};

void mark_handled_identifiers(treesitter::Node node, ASTContext& ctx) {
    if (node.is_null())
        return;
    if (node.type() == "identifier") {
        ctx.handled_identifier_byte_starts.insert(node.start_byte());
    }
    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        mark_handled_identifiers(node.child(i), ctx);
    }
}

void collect_target_identifiers(treesitter::Node node, std::vector<treesitter::Node>& out) {
    if (node.is_null())
        return;
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

void walk_node(treesitter::Node node, ASTContext& ctx);

void process_import_statement(treesitter::Node node, ASTContext& ctx) {
    const uint32_t count = node.child_count();
    for (uint32_t i = 0; i < count; ++i) {
        auto ch = node.child(i);
        if (ch.type() == "dotted_name") {
            const std::string name = std::string(ch.text(ctx.source));
            mark_handled_identifiers(ch, ctx);

            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::import,
                .written_name = name,
                .range = ch.byte_range(),
                .display_range = ch.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {name},
            });
        } else if (ch.type() == "aliased_import") {
            treesitter::Node orig_name_node = ch.child_by_field_name("name");
            if (orig_name_node.is_null()) {
                orig_name_node = ch.child(0);
            }
            const std::string name = std::string(orig_name_node.text(ctx.source));
            mark_handled_identifiers(ch, ctx);

            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::import,
                .written_name = name,
                .range = orig_name_node.byte_range(),
                .display_range = orig_name_node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {name},
            });
        }
    }
}

void process_import_from_statement(treesitter::Node node, ASTContext& ctx) {
    treesitter::Node mod_node = node.child_by_field_name("module_name");
    std::string mod_text;
    if (!mod_node.is_null()) {
        mod_text = std::string(mod_node.text(ctx.source));
        mark_handled_identifiers(mod_node, ctx);
    }

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

            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::import,
                .written_name = item_name,
                .range = ch.byte_range(),
                .display_range = ch.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {target},
            });
        } else if (ch.type() == "aliased_import") {
            treesitter::Node orig_name_node = ch.child_by_field_name("name");
            if (orig_name_node.is_null()) {
                orig_name_node = ch.child(0);
            }
            const std::string item_name = std::string(orig_name_node.text(ctx.source));
            mark_handled_identifiers(ch, ctx);

            std::string target;
            if (mod_text.empty()) {
                target = item_name;
            } else if (mod_text.ends_with('.')) {
                target = mod_text + item_name;
            } else {
                target = mod_text + "." + item_name;
            }

            ctx.result.occurrences.push_back(OccurrenceFact{
                .kind = worker::FactKind::import,
                .written_name = item_name,
                .range = orig_name_node.byte_range(),
                .display_range = orig_name_node.display_range(),
                .enclosing_scope = ctx.current_scope(),
                .candidate_targets = {target},
            });
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
            });
        }
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

                    ctx.result.occurrences.push_back(OccurrenceFact{
                        .kind = worker::FactKind::inheritance,
                        .written_name = base_name,
                        .range = arg.byte_range(),
                        .display_range = arg.display_range(),
                        .enclosing_scope = qname,
                        .candidate_targets = {base_name},
                    });
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
        const NodeKind kind = ctx.scope_stack.empty() ? NodeKind::function : NodeKind::method;

        const auto colon_pos = node.text(ctx.source).find(':');
        const std::string signature =
            std::string(node.text(ctx.source).substr(0, std::min<size_t>(colon_pos, 128)));

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
        walk_node(params_node, ctx);
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
        std::string callee;
        treesitter::Node callee_name_node = func_node;

        if (func_node.type() == "identifier") {
            callee = std::string(func_node.text(ctx.source));
        } else if (func_node.type() == "attribute") {
            treesitter::Node attr_node = func_node.child_by_field_name("attribute");
            if (!attr_node.is_null()) {
                callee = std::string(attr_node.text(ctx.source));
                callee_name_node = attr_node;
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

        if (func_node.type() == "attribute") {
            treesitter::Node obj_node = func_node.child_by_field_name("object");
            if (!obj_node.is_null()) {
                walk_node(obj_node, ctx);
            }
        } else if (func_node.type() != "identifier") {
            walk_node(func_node, ctx);
        }
    }

    treesitter::Node args_node = node.child_by_field_name("arguments");
    if (!args_node.is_null()) {
        walk_node(args_node, ctx);
    }
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
                    process_call(expr_node, ctx);
                } else {
                    const std::string name = std::string(expr_node.text(ctx.source));
                    mark_handled_identifiers(expr_node, ctx);

                    ctx.result.occurrences.push_back(OccurrenceFact{
                        .kind = worker::FactKind::reference,
                        .written_name = name,
                        .range = expr_node.byte_range(),
                        .display_range = expr_node.display_range(),
                        .enclosing_scope = ctx.current_scope(),
                        .candidate_targets = {name},
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
    if (node.is_null())
        return;
    if (ctx.stop_token.stop_requested())
        return;

    const auto type = node.type();

    if (type == "import_statement") {
        process_import_statement(node, ctx);
        return;
    }

    if (type == "import_from_statement") {
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
                                           const std::filesystem::path& /*file_path*/,
                                           const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        return unexpected_result<AdapterResult>(ErrorCode::cancelled, "parsing cancelled");
    }

    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(Language::python);
    if (!ts_lang) {
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
