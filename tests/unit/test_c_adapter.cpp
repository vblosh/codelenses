#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "codelenses/adapters/adapter.hpp"
#include "codelenses/adapters/c_adapter.hpp"
#include "codelenses/adapters/registry.hpp"
#include "codelenses/language.hpp"
#include "codelenses/parser/coordinate_converter.hpp"
#include "codelenses/parser/fixture.hpp"
#include "codelenses/parser/highlight.hpp"
#include "codelenses/parser/kinds.hpp"
#include "codelenses/treesitter/grammars.hpp"
#include "codelenses/treesitter/parser.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace codelenses;
using namespace codelenses::adapters;

TEST_CASE("H1-01: C adapter grammar registration and file extensions", "[adapter][c][h1]") {
    // 1. Grammar registration
    const auto* ts_lang = treesitter::grammar_for_language(Language::c);
    REQUIRE(ts_lang != nullptr);
    REQUIRE(treesitter::has_grammar_for_language(Language::c));
    REQUIRE(treesitter::grammar_version(Language::c) == "0.24.2");

    // 2. File extension mappings
    REQUIRE(language_from_extension(".c") == Language::c);
    REQUIRE(language_from_extension("c") == Language::c);
    REQUIRE(language_from_extension(".h") == Language::c);
    REQUIRE(language_from_extension("h") == Language::c);
    REQUIRE(language_from_extension(".i") == Language::c);
    REQUIRE(language_from_extension("i") == Language::c);

    // 3. Path detection
    REQUIRE(language_from_path("src/main.c") == Language::c);
    REQUIRE(language_from_path("include/header.h") == Language::c);
    REQUIRE(language_from_path("build/preprocessed.i") == Language::c);

    // 4. Registry integration
    auto& registry = default_adapter_registry();
    REQUIRE(registry.has_adapter(Language::c));

    auto* adapter_by_lang = registry.get_adapter(Language::c);
    REQUIRE(adapter_by_lang != nullptr);
    REQUIRE(adapter_by_lang->language() == Language::c);
    REQUIRE(adapter_by_lang->name() == "CAdapter");

    auto* adapter_by_path_c = registry.get_adapter_for_path("foo/bar.c");
    REQUIRE(adapter_by_path_c != nullptr);
    REQUIRE(adapter_by_path_c->language() == Language::c);

    auto* adapter_by_path_h = registry.get_adapter_for_path("foo/bar.h");
    REQUIRE(adapter_by_path_h != nullptr);
    REQUIRE(adapter_by_path_h->language() == Language::c);

    auto* adapter_by_path_i = registry.get_adapter_for_path("foo/bar.i");
    REQUIRE(adapter_by_path_i != nullptr);
    REQUIRE(adapter_by_path_i->language() == Language::c);

    // 5. Capabilities check
    const auto& caps = adapter_by_lang->capabilities();
    REQUIRE(caps.functions == CapabilityStatus::supported);
    REQUIRE(caps.variables == CapabilityStatus::supported);
    REQUIRE(caps.structs == CapabilityStatus::supported);
    REQUIRE(caps.enums == CapabilityStatus::supported);
    REQUIRE(caps.includes == CapabilityStatus::supported);
    REQUIRE(caps.calls == CapabilityStatus::supported);
    REQUIRE(caps.references == CapabilityStatus::supported);
    REQUIRE(caps.containment == CapabilityStatus::supported);
    REQUIRE(caps.classes == CapabilityStatus::unavailable);
    REQUIRE(caps.methods == CapabilityStatus::unavailable);
    REQUIRE(caps.namespaces == CapabilityStatus::unavailable);
}

TEST_CASE("H1-02: C adapter extraction of functions, variables, structs, enums, typedefs, macros, "
          "includes",
          "[adapter][c][h1]") {
    CAdapter adapter;

    std::string_view source = R"(
#include <stdio.h>
#include "my_lib.h"

#define MAX_BUFFER 1024
#define CLAMP(v, min, max) (((v) < (min)) ? (min) : (((v) > (max)) ? (max) : (v)))

typedef unsigned long ulong_t;

struct Vector {
    float x;
    float y;
};

enum LogLevel {
    LOG_INFO = 0,
    LOG_WARN = 1,
    LOG_ERROR = 2
};

int global_counter = 42;
static const int MAX_RETRIES = 5;

static void log_internal(const char* msg) {
    (void)msg;
}

int add(int a, int b) {
    int sum = a + b;
    return sum;
}
)";

    auto res = adapter.parse(source, "sample.c");
    REQUIRE(res.has_value());
    REQUIRE(res->status == worker::CompletionStatus::complete);
    REQUIRE(res->diagnostics.empty());

    // 1. Includes
    bool found_stdio = false;
    bool found_mylib = false;
    for (const auto& occ : res->occurrences) {
        if (occ.kind == worker::FactKind::include) {
            if (occ.written_name == "stdio.h")
                found_stdio = true;
            if (occ.written_name == "my_lib.h")
                found_mylib = true;
        }
    }
    REQUIRE(found_stdio);
    REQUIRE(found_mylib);

    // 2. Macros
    bool found_max_buffer = false;
    bool found_clamp = false;
    for (const auto& sym : res->symbols) {
        if (sym.name == "MAX_BUFFER") {
            found_max_buffer = true;
            REQUIRE(sym.kind == NodeKind::macro);
        }
        if (sym.name == "CLAMP") {
            found_clamp = true;
            REQUIRE(sym.kind == NodeKind::macro);
        }
    }
    REQUIRE(found_max_buffer);
    REQUIRE(found_clamp);

    // 3. Typedef
    bool found_typedef = false;
    for (const auto& sym : res->symbols) {
        if (sym.name == "ulong_t") {
            found_typedef = true;
            REQUIRE(sym.kind == NodeKind::type_alias);
        }
    }
    REQUIRE(found_typedef);

    // 4. Struct & Fields
    bool found_struct = false;
    bool found_field_x = false;
    bool found_field_y = false;
    for (const auto& sym : res->symbols) {
        if (sym.name == "Vector") {
            found_struct = true;
            REQUIRE(sym.kind == NodeKind::struct_);
        }
        if (sym.name == "x" && sym.enclosing_scope == "Vector") {
            found_field_x = true;
            REQUIRE(sym.kind == NodeKind::field);
        }
        if (sym.name == "y" && sym.enclosing_scope == "Vector") {
            found_field_y = true;
            REQUIRE(sym.kind == NodeKind::field);
        }
    }
    REQUIRE(found_struct);
    REQUIRE(found_field_x);
    REQUIRE(found_field_y);

    // 5. Enum & Enumerators
    bool found_enum = false;
    bool found_log_info = false;
    bool found_log_error = false;
    for (const auto& sym : res->symbols) {
        if (sym.name == "LogLevel") {
            found_enum = true;
            REQUIRE(sym.kind == NodeKind::enum_);
        }
        if (sym.name == "LOG_INFO" && sym.enclosing_scope == "LogLevel") {
            found_log_info = true;
            REQUIRE(sym.kind == NodeKind::enum_member);
        }
        if (sym.name == "LOG_ERROR" && sym.enclosing_scope == "LogLevel") {
            found_log_error = true;
            REQUIRE(sym.kind == NodeKind::enum_member);
        }
    }
    REQUIRE(found_enum);
    REQUIRE(found_log_info);
    REQUIRE(found_log_error);

    // 6. Variables
    bool found_global = false;
    bool found_local = false;
    bool found_retries = false;
    for (const auto& sym : res->symbols) {
        if (sym.name == "global_counter") {
            found_global = true;
            REQUIRE(sym.kind == NodeKind::variable);
        }
        if (sym.name == "MAX_RETRIES") {
            found_retries = true;
            REQUIRE(sym.kind == NodeKind::variable);
        }
        if (sym.name == "sum" && sym.enclosing_scope == "add") {
            found_local = true;
            REQUIRE(sym.kind == NodeKind::variable);
        }
    }
    REQUIRE(found_global);
    REQUIRE(found_retries);
    REQUIRE(found_local);

    // 7. Functions
    bool found_add = false;
    bool found_log_internal = false;
    for (const auto& sym : res->symbols) {
        if (sym.name == "add") {
            found_add = true;
            REQUIRE(sym.kind == NodeKind::function);
            CHECK(sym.signature == "int add(int a, int b)");
        }
        if (sym.name == "log_internal") {
            found_log_internal = true;
            REQUIRE(sym.kind == NodeKind::function);
            CHECK(sym.signature == "static void log_internal(const char* msg)");
        }
        if (sym.name == "Vector") {
            CHECK(sym.signature == "struct Vector");
        }
        if (sym.name == "x" && sym.enclosing_scope == "Vector") {
            CHECK(sym.signature == "float x");
        }
        if (sym.name == "LogLevel") {
            CHECK(sym.signature == "enum LogLevel");
        }
        if (sym.name == "LOG_INFO") {
            CHECK(sym.signature == "LOG_INFO");
        }
        if (sym.name == "MAX_BUFFER") {
            CHECK(sym.signature == "#define MAX_BUFFER 1024");
        }
        if (sym.name == "global_counter") {
            CHECK(sym.signature == "int global_counter");
        }
        if (sym.name == "MAX_RETRIES") {
            CHECK(sym.signature == "static const int MAX_RETRIES");
        }
    }
    REQUIRE(found_add);
    REQUIRE(found_log_internal);
}

TEST_CASE("H1-03: C adapter extraction of declarations and calls", "[adapter][c][h1]") {
    CAdapter adapter;

    std::string_view source = R"(
extern int g_external;
int g_defined = 100;

int proto_func(int x, int y);

struct ForwardStruct;

struct RealStruct {
    int (*handler)(int);
};

int proto_func(int x, int y) {
    return x * y;
}

int caller(struct RealStruct* s) {
    int val = proto_func(2, 3);
    int handled = s->handler(val);
    int direct_ptr = (*s->handler)(val);
    return handled + direct_ptr;
}
)";

    auto res = adapter.parse(source, "decls_and_calls.c");
    REQUIRE(res.has_value());
    REQUIRE(res->status == worker::CompletionStatus::complete);

    // Verify declarations distinguishing definitions vs declarations
    bool found_extern_decl = false;
    bool found_defined_var = false;
    bool found_proto_decl = false;
    bool found_func_def = false;
    bool found_forward_struct = false;
    bool found_real_struct_def = false;

    for (const auto& decl : res->declarations) {
        if (decl.symbol_name == "g_external") {
            found_extern_decl = true;
            REQUIRE_FALSE(decl.is_definition);
        }
        if (decl.symbol_name == "g_defined") {
            found_defined_var = true;
            REQUIRE(decl.is_definition);
        }
        if (decl.symbol_name == "proto_func" && !decl.is_definition) {
            found_proto_decl = true;
        }
        if (decl.symbol_name == "proto_func" && decl.is_definition) {
            found_func_def = true;
        }
        if (decl.symbol_name == "ForwardStruct") {
            found_forward_struct = true;
            REQUIRE_FALSE(decl.is_definition);
        }
        if (decl.symbol_name == "RealStruct") {
            found_real_struct_def = true;
            REQUIRE(decl.is_definition);
        }
    }
    REQUIRE(found_extern_decl);
    REQUIRE(found_defined_var);
    REQUIRE(found_proto_decl);
    REQUIRE(found_func_def);
    REQUIRE(found_forward_struct);
    REQUIRE(found_real_struct_def);

    // Verify calls
    bool found_call_proto = false;
    int count_call_handler = 0;
    for (const auto& occ : res->occurrences) {
        if (occ.kind == worker::FactKind::call) {
            if (occ.written_name == "proto_func")
                found_call_proto = true;
            if (occ.written_name == "handler") {
                count_call_handler++;
                CHECK(source.substr(occ.range.start, occ.range.size()) == "handler");
            }
        }
    }
    REQUIRE(found_call_proto);
    REQUIRE(count_call_handler == 2);

    int count_ref_s = 0;
    for (const auto& occ : res->occurrences) {
        if (occ.kind == worker::FactKind::reference && occ.written_name == "s") {
            count_ref_s++;
        }
    }
    REQUIRE(count_ref_s >= 2);
}

TEST_CASE("C adapter function prototype vs function pointer detection", "[adapter][c][h1]") {
    CAdapter adapter;

    std::string_view source = R"(
void register_cb(void (*cb)(int));
int (parenthesized_fn)(int x);
int (*fn_ptr)(int x);
void (*signal_handler(int sig, void (*handler)(int)))(int);
int (ordinary_var);
)";

    auto res = adapter.parse(source, "func_ptrs.c");
    REQUIRE(res.has_value());
    REQUIRE(res->status == worker::CompletionStatus::complete);

    std::unordered_map<std::string, DeclarationFact> decl_map;
    for (const auto& decl : res->declarations) {
        decl_map[decl.symbol_name] = decl;
    }

    // 1. Prototype with function-pointer parameter must be a function prototype, NOT variable
    REQUIRE(decl_map.contains("register_cb"));
    CHECK(decl_map["register_cb"].kind == NodeKind::function);
    CHECK_FALSE(decl_map["register_cb"].is_definition);

    // 2. Parenthesized function name must be a function prototype, NOT function pointer
    REQUIRE(decl_map.contains("parenthesized_fn"));
    CHECK(decl_map["parenthesized_fn"].kind == NodeKind::function);
    CHECK_FALSE(decl_map["parenthesized_fn"].is_definition);

    // 3. Real function pointer variable must be a variable definition
    REQUIRE(decl_map.contains("fn_ptr"));
    CHECK(decl_map["fn_ptr"].kind == NodeKind::variable);
    CHECK(decl_map["fn_ptr"].is_definition);

    // 4. Function returning a function pointer and taking a function pointer parameter
    REQUIRE(decl_map.contains("signal_handler"));
    CHECK(decl_map["signal_handler"].kind == NodeKind::function);
    CHECK_FALSE(decl_map["signal_handler"].is_definition);

    // 5. Ordinary parenthesized variable must be a variable
    REQUIRE(decl_map.contains("ordinary_var"));
    CHECK(decl_map["ordinary_var"].kind == NodeKind::variable);
    CHECK(decl_map["ordinary_var"].is_definition);
}

TEST_CASE("H1-04: C adapter highlighting queries", "[adapter][c][h1]") {
    CAdapter adapter;

    // 1. Highlighting query is non-empty and compiles cleanly
    auto q_str = CAdapter::highlighting_query();
    REQUIRE_FALSE(q_str.empty());

    const auto* ts_lang = treesitter::grammar_for_language(Language::c);
    auto q_res = treesitter::Query::create(ts_lang, q_str);
    REQUIRE(q_res.has_value());
    REQUIRE(q_res->pattern_count() > 0);
    REQUIRE(q_res->capture_count() > 0);

    // 2. Highlighting execution produces sorted tokens with proper types
    std::string_view code = R"(#include <stdio.h>
#define MULTIPLY(a, b) ((a) * (b))

typedef int integer_t;

/* Add two integers */
int add(integer_t a, integer_t b) {
    if (a < 0) {
        return 0;
    }
    return MULTIPLY(a, b);
}
)";

    auto hl_res = adapter.highlight(code);
    REQUIRE(hl_res.has_value());
    const auto& tokens = *hl_res;
    REQUIRE_FALSE(tokens.empty());

    // Verify tokens are sorted by line and column
    for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i - 1].line == tokens[i].line) {
            REQUIRE(tokens[i - 1].start_column <= tokens[i].start_column);
        } else {
            REQUIRE(tokens[i - 1].line < tokens[i].line);
        }
    }

    const auto& legend = HighlightLegend::default_legend();

    // Check presence of expected token types
    bool has_keyword = false;
    bool has_type = false;
    bool has_function = false;
    bool has_macro = false;
    bool has_comment = false;
    bool has_number = false;
    bool has_operator = false;

    uint32_t kw_idx = *legend.token_type_index("keyword");
    uint32_t type_idx = *legend.token_type_index("type");
    uint32_t fn_idx = *legend.token_type_index("function");
    uint32_t macro_idx = *legend.token_type_index("macro");
    uint32_t comment_idx = *legend.token_type_index("comment");
    uint32_t num_idx = *legend.token_type_index("number");
    uint32_t op_idx = *legend.token_type_index("operator");

    for (const auto& tok : tokens) {
        if (tok.token_type == kw_idx)
            has_keyword = true;
        if (tok.token_type == type_idx)
            has_type = true;
        if (tok.token_type == fn_idx)
            has_function = true;
        if (tok.token_type == macro_idx)
            has_macro = true;
        if (tok.token_type == comment_idx)
            has_comment = true;
        if (tok.token_type == num_idx)
            has_number = true;
        if (tok.token_type == op_idx)
            has_operator = true;
    }

    REQUIRE(has_keyword);
    REQUIRE(has_type);
    REQUIRE(has_function);
    REQUIRE(has_macro);
    REQUIRE(has_comment);
    REQUIRE(has_number);
    REQUIRE(has_operator);

    // Verify definition modifier is set on function definitions
    uint32_t def_bit = *legend.token_modifier_bit("definition");
    bool found_fn_def = false;
    for (const auto& tok : tokens) {
        if (tok.token_type == fn_idx && (tok.token_modifiers & def_bit) != 0) {
            found_fn_def = true;
        }
    }
    CHECK(found_fn_def);

    // Verify tokens do not overlap
    for (size_t i = 1; i < tokens.size(); ++i) {
        const auto& prev = tokens[i - 1];
        const auto& curr = tokens[i];
        if (prev.line == curr.line) {
            CHECK(prev.start_column + prev.length <= curr.start_column);
        }
    }
}

TEST_CASE("C adapter operator highlighting coverage", "[adapter][c][h1]") {
    CAdapter adapter;

    std::string_view op_code = R"(
void test_ops(struct Point p) {
    int a = 10 / 2;
    a /= 2;
    a *= 3;
    a &= 1;
    int b = (a > 0) ? a : 0;
    int c = p.x;
}
)";

    auto res = adapter.highlight(op_code);
    REQUIRE(res.has_value());
    const auto& tokens = *res;

    const auto& legend = HighlightLegend::default_legend();
    uint32_t op_idx = *legend.token_type_index("operator");

    std::unordered_set<std::string> seen_ops;
    for (const auto& tok : tokens) {
        if (tok.token_type == op_idx) {
            std::string op_text(op_code.substr(tok.byte_range.start, tok.byte_range.size()));
            seen_ops.insert(op_text);
        }
    }

    CHECK(seen_ops.contains("/"));
    CHECK(seen_ops.contains("/="));
    CHECK(seen_ops.contains("*="));
    CHECK(seen_ops.contains("&="));
    CHECK(seen_ops.contains("?"));
    CHECK(seen_ops.contains(":"));
    CHECK(seen_ops.contains("."));
}

TEST_CASE("H1-05: C adapter fixtures for headers, pointers, macros, and declarations",
          "[adapter][c][h1]") {
    CAdapter adapter;

    auto find_fixture_dir = []() -> std::filesystem::path {
#ifdef CODELENSES_SOURCE_DIR
        std::filesystem::path p = std::filesystem::path(CODELENSES_SOURCE_DIR) / "tests/fixtures/c";
        if (std::filesystem::exists(p))
            return p;
#endif
        for (const auto& candidate : {std::filesystem::path("tests/fixtures/c"),
                                      std::filesystem::path("../../tests/fixtures/c"),
                                      std::filesystem::path("../tests/fixtures/c")}) {
            if (std::filesystem::exists(candidate))
                return candidate;
        }
        return "tests/fixtures/c";
    };
    const auto fixture_dir = find_fixture_dir();

    auto test_fixture = [&](const std::string& fixture_name, const std::string& file_ext) {
        std::filesystem::path fixture_path = fixture_dir / (fixture_name + file_ext);
        std::filesystem::path golden_path = fixture_dir / (fixture_name + ".golden.json");

        REQUIRE(std::filesystem::exists(fixture_path));

        std::ifstream src_file(fixture_path);
        REQUIRE(src_file.is_open());
        std::string source((std::istreambuf_iterator<char>(src_file)),
                           std::istreambuf_iterator<char>());

        auto parse_res = adapter.parse(source, fixture_path);
        REQUIRE(parse_res.has_value());
        const AdapterResult& result = *parse_res;
        REQUIRE(result.language == Language::c);
        REQUIRE(result.status == worker::CompletionStatus::complete);
        REQUIRE(result.diagnostics.empty());
        REQUIRE_FALSE(result.symbols.empty());

        const char* update_env = std::getenv("CODELENSES_UPDATE_GOLDEN");
        const bool should_update = (update_env != nullptr && std::string_view(update_env) == "1");

        if (should_update) {
            std::ofstream out(golden_path);
            REQUIRE(out.is_open());
            out << adapter_result_to_json(result).dump(2) << "\n";
        }

        REQUIRE(std::filesystem::exists(golden_path));

        auto cmp = compare_golden_file(result, golden_path);
        INFO("Diff for " << fixture_name << ": " << cmp.diff);
        REQUIRE(cmp.matches);
    };

    SECTION("Headers fixture") {
        test_fixture("headers", ".h");
    }

    SECTION("Pointers fixture") {
        test_fixture("pointers", ".c");
    }

    SECTION("Macros fixture") {
        test_fixture("macros", ".c");
    }

    SECTION("Declarations fixture") {
        test_fixture("declarations", ".c");
    }
}

TEST_CASE("H1-06: C adapter compile-command context: defines and undefines (-D and -U)",
          "[adapter][c][h1][compile_commands]") {
    CAdapter adapter;

    CompileCommandContext ctx{
        .directory = "/workspace",
        .file = "src/main.c",
        .defines = {"FEATURE_FLAG=1", "DEBUG_MODE", "BUFFER_SIZE=512", "TEMP_FLAG", "-UTEMP_FLAG",
                    "VERSION_STR=\"2.0.1\""},
        .language_standard = "c17",
    };

    REQUIRE(ctx.has_define("FEATURE_FLAG"));
    REQUIRE(ctx.has_define("DEBUG_MODE"));
    REQUIRE(ctx.has_define("BUFFER_SIZE"));
    REQUIRE(ctx.has_define("VERSION_STR"));
    REQUIRE_FALSE(ctx.has_define("TEMP_FLAG"));
    REQUIRE_FALSE(ctx.has_define("NON_EXISTENT"));

    CHECK(ctx.get_define_value("FEATURE_FLAG") == "1");
    CHECK(ctx.get_define_value("DEBUG_MODE") == "1");
    CHECK(ctx.get_define_value("BUFFER_SIZE") == "512");
    CHECK(ctx.get_define_value("VERSION_STR") == "\"2.0.1\"");
    CHECK(ctx.get_define_value("TEMP_FLAG") == std::nullopt);

    std::string_view source = R"(
int compute_buffer(void) {
    if (FEATURE_FLAG) {
        return BUFFER_SIZE;
    }
    return 0;
}
)";

    auto res = adapter.parse(source, "src/main.c", ctx);
    REQUIRE(res.has_value());
    REQUIRE(res->status == worker::CompletionStatus::complete);
    REQUIRE(res->compile_command.has_value());
    CHECK(res->compile_command->language_standard == "c17");

    // 1. Verify extracted macro symbols
    bool found_feature = false;
    bool found_debug = false;
    bool found_buffer = false;
    bool found_temp = false;
    bool found_version = false;

    for (const auto& sym : res->symbols) {
        if (sym.name == "FEATURE_FLAG") {
            found_feature = true;
            CHECK(sym.kind == NodeKind::macro);
            CHECK(sym.signature == "#define FEATURE_FLAG 1");
        }
        if (sym.name == "DEBUG_MODE") {
            found_debug = true;
            CHECK(sym.kind == NodeKind::macro);
            CHECK(sym.signature == "#define DEBUG_MODE 1");
        }
        if (sym.name == "BUFFER_SIZE") {
            found_buffer = true;
            CHECK(sym.kind == NodeKind::macro);
            CHECK(sym.signature == "#define BUFFER_SIZE 512");
        }
        if (sym.name == "VERSION_STR") {
            found_version = true;
            CHECK(sym.kind == NodeKind::macro);
            CHECK(sym.signature == "#define VERSION_STR \"2.0.1\"");
        }
        if (sym.name == "TEMP_FLAG") {
            found_temp = true;
        }
    }

    REQUIRE(found_feature);
    REQUIRE(found_debug);
    REQUIRE(found_buffer);
    REQUIRE(found_version);
    REQUIRE_FALSE(found_temp); // Must be removed by -UTEMP_FLAG

    // 2. Verify DeclarationFacts
    bool decl_feature = false;
    bool decl_buffer = false;
    for (const auto& decl : res->declarations) {
        if (decl.symbol_name == "FEATURE_FLAG") {
            decl_feature = true;
            CHECK(decl.kind == NodeKind::macro);
            CHECK(decl.is_definition);
        }
        if (decl.symbol_name == "BUFFER_SIZE") {
            decl_buffer = true;
            CHECK(decl.kind == NodeKind::macro);
            CHECK(decl.is_definition);
        }
    }
    REQUIRE(decl_feature);
    REQUIRE(decl_buffer);

    // 3. Verify occurrences in source code matching command-line macros
    bool occ_feature = false;
    bool occ_buffer = false;
    for (const auto& occ : res->occurrences) {
        if (occ.kind == worker::FactKind::reference) {
            if (occ.written_name == "FEATURE_FLAG") {
                occ_feature = true;
            }
            if (occ.written_name == "BUFFER_SIZE") {
                occ_buffer = true;
            }
        }
    }
    REQUIRE(occ_feature);
    REQUIRE(occ_buffer);
}

TEST_CASE("H1-06: C adapter compile-command context: include directories candidate expansion",
          "[adapter][c][h1][compile_commands]") {
    CAdapter adapter;

    CompileCommandContext ctx{
        .directory = "/workspace",
        .file = "src/core/engine.c",
        .include_dirs = {"include", "third_party/lib", "/usr/local/include"},
    };

    std::string_view source = R"(
#include "local_config.h"
#include <util/logger.h>
)";

    auto res = adapter.parse(source, "src/core/engine.c", ctx);
    REQUIRE(res.has_value());
    REQUIRE(res->status == worker::CompletionStatus::complete);

    const OccurrenceFact* local_inc = nullptr;
    const OccurrenceFact* sys_inc = nullptr;

    for (const auto& occ : res->occurrences) {
        if (occ.kind == worker::FactKind::include) {
            if (occ.written_name == "local_config.h") {
                local_inc = &occ;
            }
            if (occ.written_name == "util/logger.h") {
                sys_inc = &occ;
            }
        }
    }

    REQUIRE(local_inc != nullptr);
    REQUIRE(sys_inc != nullptr);

    // Verify candidate targets for quote include
    const auto& local_cands = local_inc->candidate_targets;
    REQUIRE(std::find(local_cands.begin(), local_cands.end(), "local_config.h") !=
            local_cands.end());
    REQUIRE(std::find(local_cands.begin(), local_cands.end(), "src/core/local_config.h") !=
            local_cands.end());
    REQUIRE(std::find(local_cands.begin(), local_cands.end(), "include/local_config.h") !=
            local_cands.end());
    REQUIRE(std::find(local_cands.begin(), local_cands.end(), "third_party/lib/local_config.h") !=
            local_cands.end());

    // Verify candidate targets for system include
    const auto& sys_cands = sys_inc->candidate_targets;
    REQUIRE(std::find(sys_cands.begin(), sys_cands.end(), "util/logger.h") != sys_cands.end());
    REQUIRE(std::find(sys_cands.begin(), sys_cands.end(), "include/util/logger.h") !=
            sys_cands.end());
    REQUIRE(std::find(sys_cands.begin(), sys_cands.end(), "third_party/lib/util/logger.h") !=
            sys_cands.end());
}

TEST_CASE("H1-06: C adapter compile-command context: language standard validation and diagnostics",
          "[adapter][c][h1][compile_commands]") {
    CAdapter adapter;

    SECTION("Valid C standards produce no warnings") {
        for (const auto& std_name : {"c89", "c99", "c11", "c17", "c23", "gnu11", "-std=c17"}) {
            CompileCommandContext ctx{
                .language_standard = std_name,
            };
            auto res = adapter.parse("int x = 42;\n", "test.c", ctx);
            REQUIRE(res.has_value());
            REQUIRE(res->diagnostics.empty());
            REQUIRE(res->status == worker::CompletionStatus::complete);
        }
    }

    SECTION("Invalid standard produces diagnostic warning without failing compilation") {
        CompileCommandContext ctx{
            .language_standard = "c++20", // C++ standard in C adapter
        };
        auto res = adapter.parse("int x = 42;\n", "test.c", ctx);
        REQUIRE(res.has_value());
        REQUIRE(res->status == worker::CompletionStatus::complete);
        REQUIRE_FALSE(res->diagnostics.empty());

        bool found_diag = false;
        for (const auto& d : res->diagnostics) {
            if (d.code == "unsupported_standard" && d.severity == DiagnosticSeverity::warning) {
                found_diag = true;
            }
        }
        REQUIRE(found_diag);
    }
}

TEST_CASE("H1-06: C adapter instance-level context vs per-call context",
          "[adapter][c][h1][compile_commands]") {
    CAdapter adapter;

    REQUIRE_FALSE(adapter.compile_command_context().has_value());

    CompileCommandContext instance_ctx{
        .defines = {"GLOBAL_MACRO=100"},
    };
    adapter.set_compile_command_context(instance_ctx);
    REQUIRE(adapter.compile_command_context().has_value());

    // 1. Parsing without explicit context uses the configured instance context
    auto res1 = adapter.parse("int a = GLOBAL_MACRO;\n", "main.c");
    REQUIRE(res1.has_value());
    bool found_global = false;
    for (const auto& sym : res1->symbols) {
        if (sym.name == "GLOBAL_MACRO")
            found_global = true;
    }
    REQUIRE(found_global);

    // 2. Clear instance context
    adapter.clear_compile_command_context();
    REQUIRE_FALSE(adapter.compile_command_context().has_value());

    auto res2 = adapter.parse("int a = GLOBAL_MACRO;\n", "main.c");
    REQUIRE(res2.has_value());
    bool found_after_clear = false;
    for (const auto& sym : res2->symbols) {
        if (sym.name == "GLOBAL_MACRO")
            found_after_clear = true;
    }
    REQUIRE_FALSE(found_after_clear);
}

TEST_CASE("H1-06: Preprocessor and command-line macro occurrences reference linking",
          "[adapter][c][h1][compile_commands]") {
    CAdapter adapter;
    CompileCommandContext ctx{
        .defines = {"FLAG_A=1", "FLAG_B=2"},
    };
    std::string_view source = R"(
#ifdef FLAG_A
int x = 1;
#endif
#if defined(FLAG_B)
int y = 2;
#endif

int run(void) {
    int local_var = 10;
    return local_var + FLAG_A;
}
)";
    auto res = adapter.parse(source, "test.c", ctx);
    REQUIRE(res.has_value());
    REQUIRE(res->status == worker::CompletionStatus::complete);

    const OccurrenceFact* occ_ifdef_a = nullptr;
    const OccurrenceFact* occ_defined_b = nullptr;
    const OccurrenceFact* occ_body_a = nullptr;
    const OccurrenceFact* occ_local = nullptr;

    for (const auto& occ : res->occurrences) {
        if (occ.kind == worker::FactKind::reference) {
            if (occ.written_name == "FLAG_A") {
                if (occ.range.start < 20) {
                    occ_ifdef_a = &occ;
                } else {
                    occ_body_a = &occ;
                }
            } else if (occ.written_name == "FLAG_B") {
                occ_defined_b = &occ;
            } else if (occ.written_name == "local_var") {
                occ_local = &occ;
            }
        }
    }

    REQUIRE(occ_ifdef_a != nullptr);
    CHECK(occ_ifdef_a->enclosing_scope == std::nullopt);
    CHECK(occ_ifdef_a->candidate_targets == std::vector<std::string>{"FLAG_A"});

    REQUIRE(occ_defined_b != nullptr);
    CHECK(occ_defined_b->candidate_targets == std::vector<std::string>{"FLAG_B"});

    REQUIRE(occ_body_a != nullptr);
    // Command-line macro reference inside function has translation-unit enclosing scope
    CHECK(occ_body_a->enclosing_scope == std::nullopt);
    CHECK(occ_body_a->candidate_targets == std::vector<std::string>{"FLAG_A"});

    REQUIRE(occ_local != nullptr);
    // Ordinary local identifier retains function enclosing scope
    CHECK(occ_local->enclosing_scope == "run");
}
