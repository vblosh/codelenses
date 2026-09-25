#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "codelenses/adapters/adapter.hpp"
#include "codelenses/adapters/c_adapter.hpp"
#include "codelenses/adapters/cpp_adapter.hpp"
#include "codelenses/adapters/registry.hpp"
#include "codelenses/parser/coordinate_converter.hpp"
#include "codelenses/parser/fixture.hpp"
#include "codelenses/parser/highlight.hpp"
#include "codelenses/parser/kinds.hpp"
#include "codelenses/parser/reference_adapter.hpp"
#include "codelenses/treesitter/grammars.hpp"
#include "codelenses/treesitter/parser.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace codelenses;
using namespace codelenses::adapters;

TEST_CASE("LanguageAdapter interface and capabilities", "[adapters][sdk]") {
    ReferenceAdapter ref_adapter;
    REQUIRE(ref_adapter.language() == Language::c);
    REQUIRE(ref_adapter.name() == "c");

    const auto& caps = ref_adapter.capabilities();
    REQUIRE(caps.functions == CapabilityStatus::supported);
    REQUIRE(caps.variables == CapabilityStatus::supported);
    REQUIRE(caps.includes == CapabilityStatus::supported);
    REQUIRE(caps.classes == CapabilityStatus::unavailable);

    CAdapter c_adapter;
    REQUIRE(c_adapter.language() == Language::c);
    REQUIRE(c_adapter.name() == "CAdapter");

    CppAdapter cpp_adapter;
    REQUIRE(cpp_adapter.language() == Language::cpp);
    REQUIRE(cpp_adapter.name() == "CppAdapter");
    REQUIRE(cpp_adapter.capabilities().classes == CapabilityStatus::supported);
}

TEST_CASE("Source range and IR types", "[adapters][sdk]") {
    ByteRange br{10, 25};
    DisplayRange dr{1, 2, 1, 17};

    REQUIRE(dr.start_line == 1);
    REQUIRE(dr.start_column == 2);
    REQUIRE(dr.end_line == 1);
    REQUIRE(dr.end_column == 17);

    SymbolFact sym{
        .name = "calculate",
        .qualified_name = "math::calculate",
        .kind = NodeKind::function,
        .range = br,
        .display_range = dr,
        .enclosing_scope = "math",
        .signature = "int calculate(int x)",
    };

    REQUIRE(sym.name == "calculate");
    REQUIRE(sym.qualified_name == "math::calculate");
    REQUIRE(sym.kind == NodeKind::function);
    REQUIRE(sym.range == br);
    REQUIRE(sym.display_range == dr);

    DeclarationFact decl{
        .symbol_name = "calculate",
        .qualified_name = "math::calculate",
        .kind = NodeKind::function,
        .range = br,
        .display_range = dr,
        .enclosing_scope = "math",
        .is_definition = true,
    };
    REQUIRE(decl.is_definition);

    OccurrenceFact occ{
        .kind = worker::FactKind::call,
        .written_name = "calculate",
        .range = br,
        .display_range = dr,
        .enclosing_scope = "main",
        .candidate_targets = {"calculate", "math::calculate"},
    };
    REQUIRE(occ.candidate_targets.size() == 2);

    ParseDiagnostic diag{
        .severity = DiagnosticSeverity::error,
        .language = Language::c,
        .code = "syntax.error",
        .message = "Unexpected token",
        .byte_range = br,
        .display_range = dr,
    };
    REQUIRE(diag.code == "syntax.error");
    REQUIRE(diag.severity == DiagnosticSeverity::error);
}

TEST_CASE("Normalized symbol kinds and relation kinds", "[adapters][sdk]") {
    // SymbolKind round-trip
    REQUIRE(to_string(SymbolKind::function) == "function");
    REQUIRE(to_string(SymbolKind::class_) == "class");
    REQUIRE(to_string(SymbolKind::struct_) == "struct");
    REQUIRE(to_string(SymbolKind::variable) == "variable");
    REQUIRE(to_string(SymbolKind::macro) == "macro");
    REQUIRE(to_string(SymbolKind::type_alias) == "type_alias");

    REQUIRE(symbol_kind_from_string("function") == SymbolKind::function);
    REQUIRE(symbol_kind_from_string("class") == SymbolKind::class_);
    REQUIRE(symbol_kind_from_string("struct") == SymbolKind::struct_);
    REQUIRE(symbol_kind_from_string("unknown_xyz") == SymbolKind::unknown);

    REQUIRE(symbol_kind_from_node_kind(NodeKind::function) == SymbolKind::function);
    REQUIRE(symbol_kind_from_node_kind(NodeKind::class_) == SymbolKind::class_);
    REQUIRE(symbol_kind_from_node_kind(NodeKind::struct_) == SymbolKind::struct_);

    // RelationKind round-trip
    REQUIRE(to_string(RelationKind::calls) == "calls");
    REQUIRE(to_string(RelationKind::imports) == "imports");
    REQUIRE(to_string(RelationKind::includes) == "includes");
    REQUIRE(to_string(RelationKind::inherits) == "inherits");
    REQUIRE(to_string(RelationKind::implements) == "implements");
    REQUIRE(to_string(RelationKind::overrides) == "overrides");
    REQUIRE(to_string(RelationKind::instantiates) == "instantiates");

    REQUIRE(relation_kind_from_string("calls") == RelationKind::calls);
    REQUIRE(relation_kind_from_string("imports") == RelationKind::imports);
    REQUIRE(relation_kind_from_string("inherits") == RelationKind::inherits);

    REQUIRE(relation_kind_from_fact_kind(worker::FactKind::call) == RelationKind::calls);
    REQUIRE(relation_kind_from_fact_kind(worker::FactKind::include) == RelationKind::includes);
    REQUIRE(relation_kind_from_fact_kind(worker::FactKind::import) == RelationKind::imports);
    REQUIRE(relation_kind_from_fact_kind(worker::FactKind::inheritance) == RelationKind::inherits);
    REQUIRE(relation_kind_from_fact_kind(worker::FactKind::implementation) ==
            RelationKind::implements);

    // OccurrenceKind round-trip
    REQUIRE(to_string(OccurrenceKind::definition) == "definition");
    REQUIRE(to_string(OccurrenceKind::declaration) == "declaration");
    REQUIRE(to_string(OccurrenceKind::reference) == "reference");
    REQUIRE(occurrence_kind_from_string("definition") == OccurrenceKind::definition);
}

TEST_CASE("Tree-sitter Parser wrapper and tree ownership", "[adapters][sdk]") {
    treesitter::Parser parser;
    REQUIRE(parser.language() == nullptr);

    // Cannot parse without setting language
    auto no_lang_res = parser.parse_string("int x = 1;");
    REQUIRE_FALSE(no_lang_res.has_value());
    REQUIRE(no_lang_res.error().code == ErrorCode::invalid_argument);

    // Set C language
    const auto* c_lang = treesitter::grammar_for_language(Language::c);
    REQUIRE(c_lang != nullptr);
    auto set_lang_res = parser.set_language(c_lang);
    REQUIRE(set_lang_res.has_value());
    REQUIRE(parser.language() == c_lang);

    // Parse valid C code
    std::string_view c_code = "int square(int x) { return x * x; }";
    auto tree_res = parser.parse_string(c_code);
    REQUIRE(tree_res.has_value());

    treesitter::Tree tree = std::move(*tree_res);
    REQUIRE(tree.raw() != nullptr);

    treesitter::Node root = tree.root_node();
    REQUIRE_FALSE(root.is_null());
    REQUIRE(root.type() == "translation_unit");
    REQUIRE(root.child_count() >= 1);
    REQUIRE_FALSE(root.has_error());

    treesitter::Node fn_node = root.child(0);
    REQUIRE(fn_node.type() == "function_definition");
    REQUIRE(fn_node.text(c_code) == c_code);

    // Move tree
    treesitter::Tree moved_tree = std::move(tree);
    REQUIRE(moved_tree.raw() != nullptr);
    REQUIRE_FALSE(moved_tree.root_node().is_null());

    // Stop token cancellation
    std::stop_source stop_source;
    stop_source.request_stop();
    auto cancelled_res = parser.parse_string(c_code, nullptr, stop_source.get_token());
    REQUIRE_FALSE(cancelled_res.has_value());
    REQUIRE(cancelled_res.error().code == ErrorCode::cancelled);
}

TEST_CASE("Parse-error collection and diagnostic reporting", "[adapters][sdk]") {
    treesitter::Parser parser;
    const auto* c_lang = treesitter::grammar_for_language(Language::c);
    REQUIRE(parser.set_language(c_lang).has_value());

    // Invalid C syntax with a missing closing paren and brace
    std::string_view bad_code = "int broken(int a { return a;";
    auto tree_res = parser.parse_string(bad_code);
    REQUIRE(tree_res.has_value());

    treesitter::Node root = tree_res->root_node();
    REQUIRE(root.has_error());

    auto diags = collect_syntax_errors(root, Language::c);
    REQUIRE_FALSE(diags.empty());
    REQUIRE(diags[0].severity == DiagnosticSeverity::error);
    REQUIRE(diags[0].language == Language::c);
    REQUIRE_FALSE(diags[0].code.empty());
    REQUIRE_FALSE(diags[0].message.empty());
}

TEST_CASE("Adapter registry by language ID and filename mapping", "[adapters][sdk]") {
    // Path to language mapping
    REQUIRE(language_from_path("foo/bar/main.c") == Language::c);
    REQUIRE(language_from_path("foo/bar/header.h") == Language::c);
    REQUIRE(language_from_path("src/module.cpp") == Language::cpp);
    REQUIRE(language_from_path("src/module.hpp") == Language::cpp);
    REQUIRE(language_from_path("src/module.cc") == Language::cpp);
    REQUIRE(language_from_path("app/Program.cs") == Language::csharp);
    REQUIRE(language_from_path("scripts/deploy.py") == Language::python);
    REQUIRE(language_from_path("scripts/types.pyi") == Language::python);
    REQUIRE(language_from_path("ui/app.ts") == Language::typescript);
    REQUIRE(language_from_path("ui/component.tsx") == Language::typescript);
    REQUIRE(language_from_path("web/index.js") == Language::javascript);
    REQUIRE(language_from_path("web/index.mjs") == Language::javascript);
    REQUIRE(language_from_path("server/main.go") == Language::go);
    REQUIRE(language_from_path("app/Main.java") == Language::java);
    REQUIRE(language_from_path("build.sh") == Language::shell);
    REQUIRE(language_from_path("build.bash") == Language::bash);
    REQUIRE(language_from_path("readme.txt") == Language::unknown);

    // Language conversions
    REQUIRE(to_string(Language::c) == "C");
    REQUIRE(to_string(Language::cpp) == "C++");
    auto parsed_c = language_from_string("c");
    REQUIRE(parsed_c.has_value());
    REQUIRE(*parsed_c == Language::c);
    auto parsed_cpp = language_from_string("cpp");
    REQUIRE(parsed_cpp.has_value());
    REQUIRE(*parsed_cpp == Language::cpp);
    auto parsed_py = language_from_string("python");
    REQUIRE(parsed_py.has_value());
    REQUIRE(*parsed_py == Language::python);

    // Registry functionality
    AdapterRegistry registry;
    REQUIRE(registry.get_adapter(Language::c) == nullptr);
    REQUIRE_FALSE(registry.has_adapter(Language::c));

    registry.register_adapter(std::make_unique<ReferenceAdapter>());
    REQUIRE(registry.has_adapter(Language::c));
    REQUIRE(registry.get_adapter(Language::c) != nullptr);
    REQUIRE(registry.get_adapter_for_path("src/file.c") != nullptr);
    REQUIRE(registry.get_adapter_for_path("src/file.py") == nullptr);

    // Default registry contains all supported languages
    auto& def_reg = default_adapter_registry();
    REQUIRE(def_reg.has_adapter(Language::c));
    REQUIRE(def_reg.has_adapter(Language::cpp));
    REQUIRE(def_reg.has_adapter(Language::python));
    REQUIRE(def_reg.has_adapter(Language::typescript));
    REQUIRE(def_reg.has_adapter(Language::javascript));
    REQUIRE(def_reg.has_adapter(Language::go));
    REQUIRE(def_reg.has_adapter(Language::java));
    REQUIRE(def_reg.has_adapter(Language::shell));
    REQUIRE(def_reg.has_adapter(Language::bash));
}

TEST_CASE("CoordinateConverter byte/line/column utilities with UTF-8 input", "[adapters][sdk]") {
    SECTION("ASCII single and multi-line") {
        std::string_view text = "hello\nworld\nfoo bar";
        CoordinateConverter conv(text);

        REQUIRE(conv.line_count() == 3);
        REQUIRE(conv.line_text(0) == "hello");
        REQUIRE(conv.line_text(1) == "world");
        REQUIRE(conv.line_text(2) == "foo bar");

        Point p0 = conv.byte_to_point(0);
        REQUIRE(p0.line == 0);
        REQUIRE(p0.column == 0);

        Point p_world = conv.byte_to_point(6); // 'w' in 'world'
        REQUIRE(p_world.line == 1);
        REQUIRE(p_world.column == 0);

        Point p_bar = conv.byte_to_point(16); // 'b' in 'bar'
        REQUIRE(p_bar.line == 2);
        REQUIRE(p_bar.column == 4);

        REQUIRE(conv.point_to_byte(p_bar) == 16);

        auto sr = conv.byte_range_to_source_range(ByteRange{6, 11});
        REQUIRE(sr.start_byte == 6);
        REQUIRE(sr.end_byte == 11);
        REQUIRE(sr.start_line == 1);
        REQUIRE(sr.start_column == 0);
        REQUIRE(sr.end_line == 1);
        REQUIRE(sr.end_column == 5);
    }

    SECTION("CRLF line endings") {
        std::string_view text = "line1\r\nline2\r\nline3";
        CoordinateConverter conv(text);

        REQUIRE(conv.line_count() == 3);
        REQUIRE(conv.line_text(0) == "line1");
        REQUIRE(conv.line_text(1) == "line2");
        REQUIRE(conv.line_text(2) == "line3");

        Point p2 = conv.byte_to_point(7); // 'l' in 'line2'
        REQUIRE(p2.line == 1);
        REQUIRE(p2.column == 0);
    }

    SECTION("UTF-8 Cyrillic multibyte (2 bytes per character)") {
        // "Привет" is 6 Cyrillic characters = 12 bytes
        std::string_view text = "Привет\nМир";
        CoordinateConverter conv(text);

        REQUIRE(conv.line_count() == 2);

        // Byte offset 0: line 0, byte col 0, codepoint col 0
        Point p0 = conv.byte_to_point(0);
        REQUIRE(p0.line == 0);
        REQUIRE(p0.column == 0);
        REQUIRE(conv.byte_to_codepoint_column(0, 0) == 0);

        // 1 Cyrillic character ('П') = 2 bytes
        REQUIRE(conv.byte_to_codepoint_column(0, 2) == 1);
        REQUIRE(conv.codepoint_to_byte_column(0, 1) == 2);

        // Full word "Привет" = 12 bytes = 6 codepoints = 6 UTF-16 units
        REQUIRE(conv.byte_to_codepoint_column(0, 12) == 6);
        REQUIRE(conv.byte_to_utf16_column(0, 12) == 6);

        // "Мир" starts at byte 13
        Point p_mir = conv.byte_to_point(13);
        REQUIRE(p_mir.line == 1);
        REQUIRE(p_mir.column == 0);
    }

    SECTION("UTF-8 4-byte Emojis (surrogate pairs in UTF-16)") {
        // "Hi 🚀 end"
        // 'H' (1) 'i' (1) ' ' (1) '🚀' (4) ' ' (1) 'e' (1) 'n' (1) 'd' (1) = 11 bytes
        std::string_view text = "Hi 🚀 end";
        CoordinateConverter conv(text);

        REQUIRE(conv.line_count() == 1);

        // Before rocket: byte 3 -> codepoint 3, UTF-16 3
        REQUIRE(conv.byte_to_codepoint_column(0, 3) == 3);
        REQUIRE(conv.byte_to_utf16_column(0, 3) == 3);

        // After rocket: byte 7 (3 + 4 bytes) -> codepoint 4, UTF-16 5 (surrogate pair!)
        REQUIRE(conv.byte_to_codepoint_column(0, 7) == 4);
        REQUIRE(conv.byte_to_utf16_column(0, 7) == 5);

        // UTF-16 column 5 back to byte offset 7
        REQUIRE(conv.utf16_to_byte_column(0, 5) == 7);
    }

    SECTION("Out of bounds and empty strings") {
        CoordinateConverter conv("");
        REQUIRE(conv.line_count() == 1);
        Point p = conv.byte_to_point(100);
        REQUIRE(p.line == 0);
        REQUIRE(p.column == 0);
    }
}

TEST_CASE("Highlight token legend and token serialization", "[adapters][sdk]") {
    const auto& legend = HighlightLegend::default_legend();

    // Check standard types
    auto fn_idx = legend.token_type_index("function");
    REQUIRE(fn_idx.has_value());
    REQUIRE(legend.token_type_name(*fn_idx) == "function");

    auto type_idx = legend.token_type_index("type");
    REQUIRE(type_idx.has_value());
    REQUIRE(legend.token_type_name(*type_idx) == "type");

    REQUIRE_FALSE(legend.token_type_index("nonexistent_type").has_value());

    // Check modifiers bitmask
    auto def_bit = legend.token_modifier_bit("definition");
    REQUIRE(def_bit.has_value());

    auto ro_bit = legend.token_modifier_bit("readonly");
    REQUIRE(ro_bit.has_value());

    uint32_t mask = legend.encode_modifiers({"definition", "readonly"});
    REQUIRE(mask == (*def_bit | *ro_bit));

    auto decoded = legend.decode_modifiers(mask);
    REQUIRE(decoded.size() == 2);

    // Serialization to JSON
    nlohmann::json legend_json = legend.to_json();
    REQUIRE(legend_json.contains("tokenTypes"));
    REQUIRE(legend_json.contains("tokenModifiers"));
    REQUIRE(legend_json["tokenTypes"].is_array());

    HighlightLegend roundtrip = HighlightLegend::from_json(legend_json);
    REQUIRE(roundtrip.token_types() == legend.token_types());
    REQUIRE(roundtrip.token_modifiers() == legend.token_modifiers());

    // HighlightToken JSON serialization
    HighlightToken tok{
        .line = 5,
        .start_column = 10,
        .length = 8,
        .token_type = *fn_idx,
        .token_modifiers = mask,
    };
    nlohmann::json tok_json = tok;
    REQUIRE(tok_json["line"] == 5);
    REQUIRE(tok_json["startColumn"] == 10);
    REQUIRE(tok_json["length"] == 8);

    HighlightToken tok_back = tok_json.get<HighlightToken>();
    REQUIRE(tok_back.line == tok.line);
    REQUIRE(tok_back.start_column == tok.start_column);
    REQUIRE(tok_back.length == tok.length);
    REQUIRE(tok_back.token_type == tok.token_type);
    REQUIRE(tok_back.token_modifiers == tok.token_modifiers);
}

TEST_CASE("Fixture format and golden-output comparison helper", "[adapters][sdk]") {
    AdapterResult result{
        .language = Language::c,
        .status = worker::CompletionStatus::complete,
        .symbols =
            {
                SymbolFact{
                    .name = "foo",
                    .qualified_name = "foo",
                    .kind = NodeKind::function,
                    .range = ByteRange{0, 20},
                    .display_range = DisplayRange{1, 1, 2, 2},
                    .enclosing_scope = std::nullopt,
                    .signature = "void foo()",
                },
            },
        .declarations =
            {
                DeclarationFact{
                    .symbol_name = "foo",
                    .qualified_name = "foo",
                    .kind = NodeKind::function,
                    .range = ByteRange{0, 20},
                    .display_range = DisplayRange{1, 1, 2, 2},
                    .enclosing_scope = std::nullopt,
                    .is_definition = true,
                },
            },
        .occurrences =
            {
                OccurrenceFact{
                    .kind = worker::FactKind::call,
                    .written_name = "bar",
                    .range = ByteRange{10, 13},
                    .display_range = DisplayRange{1, 11, 1, 14},
                    .enclosing_scope = std::nullopt,
                    .candidate_targets = {"bar"},
                },
                OccurrenceFact{
                    .kind = worker::FactKind::include,
                    .written_name = "stdio.h",
                    .range = ByteRange{0, 19},
                    .display_range = DisplayRange{1, 1, 1, 20},
                    .enclosing_scope = std::nullopt,
                    .candidate_targets = {"stdio.h"},
                },
            },
        .diagnostics = {},
    };

    nlohmann::json golden = adapter_result_to_json(result);
    REQUIRE(golden["language"] == "C");
    REQUIRE(golden["status"] == "complete");
    REQUIRE(golden["symbols"].size() == 1);
    REQUIRE(golden["symbols"][0]["name"] == "foo");
    REQUIRE(golden["occurrences"].size() == 2);

    // Golden comparison match
    auto cmp_match = compare_golden(result, golden);
    REQUIRE(cmp_match.matches);
    REQUIRE(cmp_match.diff.empty());

    // Golden comparison mismatch
    nlohmann::json mutated_golden = golden;
    mutated_golden["status"] = "failed";
    auto cmp_mismatch = compare_golden(result, mutated_golden);
    REQUIRE_FALSE(cmp_mismatch.matches);
    REQUIRE_FALSE(cmp_mismatch.diff.empty());

    // File comparison
    const std::filesystem::path temp_file = "test_golden_temp.json";
    {
        std::ofstream out(temp_file);
        out << golden.dump(2);
    }

    auto cmp_file = compare_golden_file(result, temp_file);
    REQUIRE(cmp_file.matches);

    std::filesystem::remove(temp_file);
}

TEST_CASE("Minimal ReferenceAdapter parses fixture text and produces normalized IR",
          "[adapters][sdk]") {
    ReferenceAdapter adapter;

    std::string_view source = "#include <stdio.h>\n"
                              "\n"
                              "int add(int a, int b) {\n"
                              "    return a + b;\n"
                              "}\n"
                              "\n"
                              "int main() {\n"
                              "    int result = add(1, 2);\n"
                              "    return 0;\n"
                              "}\n";

    auto parse_res = adapter.parse(source, "test.c");
    REQUIRE(parse_res.has_value());

    const AdapterResult& result = *parse_res;
    REQUIRE(result.language == Language::c);
    REQUIRE(result.status == worker::CompletionStatus::complete);
    REQUIRE(result.diagnostics.empty());

    // Verify symbols
    bool found_add = false;
    bool found_main = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "add" && sym.kind == NodeKind::function) {
            found_add = true;
            REQUIRE_FALSE(sym.signature.empty());
        }
        if (sym.name == "main" && sym.kind == NodeKind::function) {
            found_main = true;
        }
    }
    REQUIRE(found_add);
    REQUIRE(found_main);

    // Verify occurrences (call to add and include stdio.h)
    bool found_call_add = false;
    bool found_inc_stdio = false;
    for (const auto& occ : result.occurrences) {
        if (occ.written_name == "add" && occ.kind == worker::FactKind::call) {
            found_call_add = true;
        }
        if (occ.written_name == "stdio.h" && occ.kind == worker::FactKind::include) {
            found_inc_stdio = true;
        }
    }
    REQUIRE(found_call_add);
    REQUIRE(found_inc_stdio);

    // Verify highlights via adapter.highlight
    treesitter::Parser parser;
    const auto* ts_lang = treesitter::grammar_for_language(Language::c);
    REQUIRE(parser.set_language(ts_lang).has_value());
    auto tree_res = parser.parse_string(source);
    REQUIRE(tree_res.has_value());
    auto hl_res = adapter.highlight(source, *tree_res);
    REQUIRE(hl_res.has_value());
    REQUIRE_FALSE(hl_res->empty());

    // Golden output comparison works
    nlohmann::json golden = adapter_result_to_json(result);
    auto cmp = compare_golden(result, golden);
    REQUIRE(cmp.matches);
}

TEST_CASE("ReferenceAdapter handles syntax errors and cancellation", "[adapters][sdk]") {
    ReferenceAdapter adapter;

    SECTION("Syntax error creates diagnostics and degrades status") {
        std::string_view bad_code = "int broken(int a { return;";
        auto parse_res = adapter.parse(bad_code, "bad.c");
        REQUIRE(parse_res.has_value());

        const AdapterResult& result = *parse_res;
        REQUIRE_FALSE(result.diagnostics.empty());
        REQUIRE(result.status != worker::CompletionStatus::complete);
    }

    SECTION("Cancellation returns error") {
        std::stop_source stop_source;
        stop_source.request_stop();

        std::string_view code = "int foo() { return 42; }";
        auto parse_res = adapter.parse(code, "test.c", stop_source.get_token());
        REQUIRE_FALSE(parse_res.has_value());
        REQUIRE(parse_res.error().code == ErrorCode::cancelled);
    }
}
