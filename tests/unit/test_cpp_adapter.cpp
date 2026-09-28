#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "codelenses/adapters/adapter.hpp"
#include "codelenses/adapters/cpp_adapter.hpp"
#include "codelenses/adapters/registry.hpp"
#include "codelenses/language.hpp"
#include "codelenses/parser/coordinate_converter.hpp"
#include "codelenses/parser/fixture.hpp"
#include "codelenses/parser/highlight.hpp"
#include "codelenses/parser/kinds.hpp"
#include "codelenses/resolver/symbol_key.hpp"
#include "codelenses/treesitter/grammars.hpp"
#include "codelenses/treesitter/parser.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace codelenses;
using namespace codelenses::adapters;

TEST_CASE("H2-01: C++ adapter grammar registration and file extensions", "[adapter][cpp][h2]") {
    // 1. Grammar registration
    const auto* ts_lang = treesitter::grammar_for_language(Language::cpp);
    REQUIRE(ts_lang != nullptr);
    REQUIRE(treesitter::has_grammar_for_language(Language::cpp));
    REQUIRE(treesitter::grammar_version(Language::cpp) == "0.23.4");

    // 2. File extension mappings
    REQUIRE(language_from_extension(".cpp") == Language::cpp);
    REQUIRE(language_from_extension("cpp") == Language::cpp);
    REQUIRE(language_from_extension(".cc") == Language::cpp);
    REQUIRE(language_from_extension("cc") == Language::cpp);
    REQUIRE(language_from_extension(".cxx") == Language::cpp);
    REQUIRE(language_from_extension("cxx") == Language::cpp);
    REQUIRE(language_from_extension(".hpp") == Language::cpp);
    REQUIRE(language_from_extension("hpp") == Language::cpp);
    REQUIRE(language_from_extension(".hh") == Language::cpp);
    REQUIRE(language_from_extension("hh") == Language::cpp);
    REQUIRE(language_from_extension(".hxx") == Language::cpp);
    REQUIRE(language_from_extension("hxx") == Language::cpp);
    REQUIRE(language_from_extension(".tpp") == Language::cpp);
    REQUIRE(language_from_extension(".ipp") == Language::cpp);
    REQUIRE(language_from_extension(".c++") == Language::cpp);
    REQUIRE(language_from_extension(".h++") == Language::cpp);

    // 3. Path detection
    REQUIRE(language_from_path("src/main.cpp") == Language::cpp);
    REQUIRE(language_from_path("include/header.hpp") == Language::cpp);
    REQUIRE(language_from_path("lib/module.cc") == Language::cpp);

    // 4. Registry integration
    auto& registry = default_adapter_registry();
    REQUIRE(registry.has_adapter(Language::cpp));

    auto* adapter_by_lang = registry.get_adapter(Language::cpp);
    REQUIRE(adapter_by_lang != nullptr);
    REQUIRE(adapter_by_lang->language() == Language::cpp);
    REQUIRE(adapter_by_lang->name() == "CppAdapter");

    auto* adapter_by_path_cpp = registry.get_adapter_for_path("foo/bar.cpp");
    REQUIRE(adapter_by_path_cpp != nullptr);
    REQUIRE(adapter_by_path_cpp->language() == Language::cpp);

    auto* adapter_by_path_hpp = registry.get_adapter_for_path("foo/bar.hpp");
    REQUIRE(adapter_by_path_hpp != nullptr);
    REQUIRE(adapter_by_path_hpp->language() == Language::cpp);

    // 5. Capabilities check
    const auto& caps = adapter_by_lang->capabilities();
    REQUIRE(caps.functions == CapabilityStatus::supported);
    REQUIRE(caps.methods == CapabilityStatus::supported);
    REQUIRE(caps.classes == CapabilityStatus::supported);
    REQUIRE(caps.structs == CapabilityStatus::supported);
    REQUIRE(caps.enums == CapabilityStatus::supported);
    REQUIRE(caps.namespaces == CapabilityStatus::supported);
    REQUIRE(caps.variables == CapabilityStatus::supported);
    REQUIRE(caps.templates == CapabilityStatus::supported);
    REQUIRE(caps.containment == CapabilityStatus::supported);
    REQUIRE(caps.calls == CapabilityStatus::supported);
    REQUIRE(caps.references == CapabilityStatus::supported);
    REQUIRE(caps.inheritance == CapabilityStatus::supported);
    REQUIRE(caps.includes == CapabilityStatus::supported);
    REQUIRE(caps.interfaces == CapabilityStatus::unavailable);
}

TEST_CASE(
    "H2-01: C++ adapter extraction of namespaces, classes, structs, enums, aliases, templates, "
    "functions, methods",
    "[adapter][cpp][h2]") {
    CppAdapter adapter;

    std::string_view source = R"(
#include <string>

namespace Math::Linear {

template <typename T>
class Matrix {
public:
    Matrix(int rows, int cols);
    ~Matrix();

    T get(int r, int c) const;
    void set(int r, int c, T val);

    static Matrix identity(int n);

private:
    int rows_;
    int cols_;
};

struct Point3D {
    float x;
    float y;
    float z;
};

enum class ColorMode : uint8_t {
    RGB = 0,
    RGBA = 1,
    Grayscale = 2
};

using FloatMatrix = Matrix<float>;
typedef Point3D PointAlias;

template <typename T>
T clamp(T val, T min_v, T max_v) {
    if (val < min_v) return min_v;
    if (val > max_v) return max_v;
    return val;
}

} // namespace Math::Linear
)";

    auto res = adapter.parse(source, "src/math.cpp");
    REQUIRE(res.has_value());
    const AdapterResult& result = *res;

    REQUIRE(result.language == Language::cpp);
    REQUIRE(result.status == worker::CompletionStatus::complete);
    REQUIRE(result.diagnostics.empty());

    // 1. Verify Namespace
    bool found_ns = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "Math::Linear") {
            found_ns = true;
            CHECK(sym.kind == NodeKind::namespace_);
            CHECK(sym.qualified_name == "Math::Linear");
        }
    }
    REQUIRE(found_ns);

    // 2. Verify Class and Struct
    bool found_matrix = false;
    bool found_point = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "Matrix" && sym.kind == NodeKind::class_) {
            found_matrix = true;
            CHECK(sym.qualified_name == "Math::Linear::Matrix");
            CHECK(sym.enclosing_scope == "Math::Linear");
        }
        if (sym.name == "Point3D") {
            found_point = true;
            CHECK(sym.kind == NodeKind::struct_);
            CHECK(sym.qualified_name == "Math::Linear::Point3D");
            CHECK(sym.enclosing_scope == "Math::Linear");
        }
    }
    REQUIRE(found_matrix);
    REQUIRE(found_point);

    // 3. Verify Methods and Fields in Class/Struct
    bool found_ctor = false;
    bool found_dtor = false;
    bool found_get = false;
    bool found_static_id = false;
    bool found_rows = false;
    bool found_point_x = false;

    for (const auto& sym : result.symbols) {
        if (sym.name == "Matrix" && sym.kind == NodeKind::method) {
            found_ctor = true;
            CHECK(sym.qualified_name == "Math::Linear::Matrix::Matrix");
            CHECK(sym.enclosing_scope == "Math::Linear::Matrix");
        }
        if (sym.name == "~Matrix") {
            found_dtor = true;
            CHECK(sym.kind == NodeKind::method);
            CHECK(sym.qualified_name == "Math::Linear::Matrix::~Matrix");
        }
        if (sym.name == "get") {
            found_get = true;
            CHECK(sym.kind == NodeKind::method);
            CHECK(sym.qualified_name == "Math::Linear::Matrix::get");
        }
        if (sym.name == "identity") {
            found_static_id = true;
            CHECK(sym.kind == NodeKind::method);
            CHECK(sym.qualified_name == "Math::Linear::Matrix::identity");
        }
        if (sym.name == "rows_") {
            found_rows = true;
            CHECK(sym.kind == NodeKind::field);
            CHECK(sym.qualified_name == "Math::Linear::Matrix::rows_");
        }
        if (sym.name == "x") {
            found_point_x = true;
            CHECK(sym.kind == NodeKind::field);
            CHECK(sym.qualified_name == "Math::Linear::Point3D::x");
        }
    }
    REQUIRE(found_ctor);
    REQUIRE(found_dtor);
    REQUIRE(found_get);
    REQUIRE(found_static_id);
    REQUIRE(found_rows);
    REQUIRE(found_point_x);

    // 4. Verify Enum and Enum Members
    bool found_enum = false;
    bool found_rgb = false;
    bool found_rgba = false;
    bool found_gray = false;

    for (const auto& sym : result.symbols) {
        if (sym.name == "ColorMode") {
            found_enum = true;
            CHECK(sym.kind == NodeKind::enum_);
            CHECK(sym.qualified_name == "Math::Linear::ColorMode");
        }
        if (sym.name == "RGB") {
            found_rgb = true;
            CHECK(sym.kind == NodeKind::enum_member);
            CHECK(sym.qualified_name == "Math::Linear::ColorMode::RGB");
        }
        if (sym.name == "RGBA") {
            found_rgba = true;
            CHECK(sym.kind == NodeKind::enum_member);
            CHECK(sym.qualified_name == "Math::Linear::ColorMode::RGBA");
        }
        if (sym.name == "Grayscale") {
            found_gray = true;
            CHECK(sym.kind == NodeKind::enum_member);
            CHECK(sym.qualified_name == "Math::Linear::ColorMode::Grayscale");
        }
    }
    REQUIRE(found_enum);
    REQUIRE(found_rgb);
    REQUIRE(found_rgba);
    REQUIRE(found_gray);

    // 5. Verify Aliases (using and typedef)
    bool found_using_alias = false;
    bool found_typedef_alias = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "FloatMatrix") {
            found_using_alias = true;
            CHECK(sym.kind == NodeKind::type_alias);
            CHECK(sym.qualified_name == "Math::Linear::FloatMatrix");
        }
        if (sym.name == "PointAlias") {
            found_typedef_alias = true;
            CHECK(sym.kind == NodeKind::type_alias);
            CHECK(sym.qualified_name == "Math::Linear::PointAlias");
        }
    }
    REQUIRE(found_using_alias);
    REQUIRE(found_typedef_alias);

    // 6. Verify Function Template
    bool found_clamp = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "clamp") {
            found_clamp = true;
            CHECK(sym.kind == NodeKind::function);
            CHECK(sym.qualified_name == "Math::Linear::clamp");
            CHECK(sym.signature.find("template") != std::string::npos);
        }
    }
    REQUIRE(found_clamp);
}

TEST_CASE("H2-02: C++ adapter extraction of inheritance, using declarations, includes, and calls",
          "[adapter][cpp][h2]") {
    CppAdapter adapter;

    std::string_view source = R"(
#include <iostream>
#include "base.hpp"

namespace Graphics {

class Shape {
public:
    virtual ~Shape() = default;
    virtual void draw() = 0;
};

class Drawable {
public:
    virtual void render() = 0;
};

template <typename T>
class Tagged {};

class Circle : public Shape, private Drawable, public Tagged<Circle> {
public:
    void draw() override {
        Shape::draw();
        render();
    }

    void render() override;
};

void run_graphics() {
    using namespace Graphics;
    using std::cout;

    Circle c;
    c.draw();
}

} // namespace Graphics
)";

    auto res = adapter.parse(source, "src/graphics.cpp");
    REQUIRE(res.has_value());
    const AdapterResult& result = *res;
    REQUIRE(result.status == worker::CompletionStatus::complete);

    // 1. Verify Includes
    bool found_iostream = false;
    bool found_base_hpp = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::include) {
            if (occ.written_name == "iostream") {
                found_iostream = true;
            }
            if (occ.written_name == "base.hpp") {
                found_base_hpp = true;
            }
        }
    }
    REQUIRE(found_iostream);
    REQUIRE(found_base_hpp);

    // 2. Verify Inheritance occurrences
    bool found_shape_base = false;
    bool found_drawable_base = false;
    bool found_tagged_base = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::inheritance) {
            if (occ.written_name == "Shape") {
                found_shape_base = true;
                CHECK(occ.enclosing_scope == "Graphics::Circle");
            }
            if (occ.written_name == "Drawable") {
                found_drawable_base = true;
                CHECK(occ.enclosing_scope == "Graphics::Circle");
            }
            if (occ.written_name == "Tagged<Circle>") {
                found_tagged_base = true;
                CHECK(occ.enclosing_scope == "Graphics::Circle");
                CHECK(std::ranges::find(occ.candidate_targets, "Tagged") !=
                      occ.candidate_targets.end());
            }
        }
    }
    REQUIRE(found_shape_base);
    REQUIRE(found_drawable_base);
    REQUIRE(found_tagged_base);

    // 3. Verify Using declarations and directives
    bool found_using_ns = false;
    bool found_using_cout = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::reference) {
            if (occ.written_name == "Graphics") {
                found_using_ns = true;
            }
            if (occ.written_name == "std::cout") {
                found_using_cout = true;
                CHECK(std::ranges::find(occ.candidate_targets, "cout") !=
                      occ.candidate_targets.end());
            }
        }
    }
    REQUIRE(found_using_ns);
    REQUIRE(found_using_cout);

    // 4. Verify Calls
    bool found_draw_call = false;
    bool found_render_call = false;
    bool found_method_call = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::call) {
            if (occ.written_name == "Shape::draw") {
                found_draw_call = true;
                CHECK(std::ranges::find(occ.candidate_targets, "draw") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "render") {
                found_render_call = true;
            }
            if (occ.written_name == "draw") {
                found_method_call = true;
            }
        }
    }
    REQUIRE(found_draw_call);
    REQUIRE(found_render_call);
    REQUIRE(found_method_call);
}

TEST_CASE("H2-03: C++ adapter qualified-name and overload metadata", "[adapter][cpp][h2]") {
    CppAdapter adapter;

    std::string_view source = R"(
namespace Net {

class Client {
public:
    void send(const char* data);
    void send(const char* data, int len);
    void send(int code);

    static Client connect(const char* host);
};

void Client::send(const char* data) {
}

void Client::send(const char* data, int len) {
}

void Client::send(int code) {
}

} // namespace Net
)";

    auto res = adapter.parse(source, "src/net.cpp");
    REQUIRE(res.has_value());
    const AdapterResult& result = *res;
    REQUIRE(result.status == worker::CompletionStatus::complete);

    // 1. Verify out-of-line method definitions have qualified name Net::Client::send
    int send_def_count = 0;
    std::vector<std::string> signatures;

    int send_sym_count = 0;
    for (const auto& sym : result.symbols) {
        if (sym.name == "send" && sym.kind == NodeKind::method) {
            send_sym_count++;
            CHECK(sym.qualified_name == "Net::Client::send");
            CHECK(sym.enclosing_scope == "Net::Client");
        }
    }
    REQUIRE(send_sym_count == 6);

    for (const auto& decl : result.declarations) {
        if (decl.symbol_name == "send" && decl.is_definition) {
            send_def_count++;
            CHECK(decl.qualified_name == "Net::Client::send");
            CHECK(decl.enclosing_scope == "Net::Client");
        }
    }
    REQUIRE(send_def_count == 3);

    // 2. Verify all 3 overloads have distinct signatures
    for (const auto& sym : result.symbols) {
        if (sym.name == "send") {
            signatures.push_back(sym.signature);
        }
    }

    std::unordered_set<std::string> distinct_keys;
    for (const auto& sig : signatures) {
        std::string key = resolver::generate_symbol_key(Language::cpp, "src/net.cpp",
                                                        NodeKind::method, "Net::Client::send", sig);
        distinct_keys.insert(key);
    }

    // Overload metadata produces distinct symbol keys
    CHECK(distinct_keys.size() >= 3);
}

TEST_CASE(
    "H2-04: C++ adapter fixtures for templates, overloads, namespaces, and header/source pairs",
    "[adapter][cpp][h2]") {
    CppAdapter adapter;

    auto find_fixture_dir = []() -> std::filesystem::path {
#ifdef CODELENSES_SOURCE_DIR
        std::filesystem::path p =
            std::filesystem::path(CODELENSES_SOURCE_DIR) / "tests/fixtures/cpp";
        if (std::filesystem::exists(p)) {
            return p;
        }
#endif
        for (const auto& candidate : {std::filesystem::path("tests/fixtures/cpp"),
                                      std::filesystem::path("../../tests/fixtures/cpp"),
                                      std::filesystem::path("../tests/fixtures/cpp")}) {
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
        return "tests/fixtures/cpp";
    };
    const auto fixture_dir = find_fixture_dir();

    auto test_fixture = [&](const std::string& fixture_name, const std::string& file_ext) {
        std::filesystem::path fixture_path = fixture_dir / (fixture_name + file_ext);
        std::filesystem::path golden_path =
            fixture_dir / (fixture_name + file_ext + ".golden.json");

        REQUIRE(std::filesystem::exists(fixture_path));

        std::ifstream src_file(fixture_path);
        REQUIRE(src_file.is_open());
        std::string source((std::istreambuf_iterator<char>(src_file)),
                           std::istreambuf_iterator<char>());

        auto parse_res = adapter.parse(source, fixture_path);
        REQUIRE(parse_res.has_value());
        const AdapterResult& result = *parse_res;
        REQUIRE(result.language == Language::cpp);
        REQUIRE(result.status == worker::CompletionStatus::complete);
        REQUIRE(result.diagnostics.empty());
        REQUIRE_FALSE(result.symbols.empty());

        const char* update_env = std::getenv("CODELENSES_UPDATE_GOLDEN");
        const bool should_update = (update_env != nullptr && std::string_view(update_env) == "1");

        if (should_update || !std::filesystem::exists(golden_path)) {
            std::ofstream out(golden_path);
            REQUIRE(out.is_open());
            out << adapter_result_to_json(result).dump(2) << "\n";
        }

        REQUIRE(std::filesystem::exists(golden_path));

        auto cmp = compare_golden_file(result, golden_path);
        INFO("Diff for " << fixture_name << ": " << cmp.diff);
        REQUIRE(cmp.matches);
    };

    SECTION("Templates fixture") {
        test_fixture("templates", ".cpp");
    }

    SECTION("Overloads fixture") {
        test_fixture("overloads", ".cpp");
    }

    SECTION("Namespaces fixture") {
        test_fixture("namespaces", ".cpp");
    }

    SECTION("Header pair header fixture") {
        test_fixture("header_pair", ".hpp");
    }

    SECTION("Header pair source fixture") {
        test_fixture("header_pair", ".cpp");
    }
}

TEST_CASE("C++ adapter compile-command context: defines, undefines, and standards",
          "[adapter][cpp][h2][compile_commands]") {
    CppAdapter adapter;

    CompileCommandContext ctx{
        .directory = "/workspace",
        .file = "src/main.cpp",
        .include_dirs = {"include", "third_party"},
        .defines = {"ENABLE_FEATURE=1", "MAX_WORKERS=8", "TEMP_FLAG", "-UTEMP_FLAG"},
        .language_standard = "c++20",
    };

    std::string_view source = R"(
#ifdef ENABLE_FEATURE
int feature_enabled = MAX_WORKERS;
#endif
)";

    auto res = adapter.parse(source, "src/main.cpp", ctx);
    REQUIRE(res.has_value());
    REQUIRE(res->status == worker::CompletionStatus::complete);
    REQUIRE(res->diagnostics.empty());
    REQUIRE(res->compile_command.has_value());

    bool found_enable = false;
    bool found_workers = false;
    bool found_temp = false;

    for (const auto& sym : res->symbols) {
        if (sym.name == "ENABLE_FEATURE") {
            found_enable = true;
        }
        if (sym.name == "MAX_WORKERS") {
            found_workers = true;
        }
        if (sym.name == "TEMP_FLAG") {
            found_temp = true;
        }
    }
    REQUIRE(found_enable);
    REQUIRE(found_workers);
    REQUIRE_FALSE(found_temp);

    SECTION("Warning on non-C++ standard") {
        CompileCommandContext bad_std_ctx{
            .language_standard = "c99",
        };
        auto bad_res = adapter.parse("int a = 10;\n", "test.cpp", bad_std_ctx);
        REQUIRE(bad_res.has_value());
        bool found_diag = false;
        for (const auto& d : bad_res->diagnostics) {
            if (d.code == "unsupported_standard" && d.severity == DiagnosticSeverity::warning) {
                found_diag = true;
            }
        }
        REQUIRE(found_diag);
    }
}

TEST_CASE("C++ adapter syntax highlighting tokens generation", "[adapter][cpp][h2][highlight]") {
    CppAdapter adapter;

    std::string_view source = R"(
#include <vector>

namespace Demo {

template <typename T>
class Widget {
public:
    constexpr Widget(T val) : val_(val) {}
    T get() const noexcept { return val_; }
private:
    T val_;
};

} // namespace Demo

int main() {
    Demo::Widget<int> w(42);
    int res = w.get();
    return 0;
}
)";

    auto res = adapter.highlight(source);
    if (!res) {
        INFO("Highlight error: " << res.error().message);
    }
    REQUIRE(res.has_value());
    const auto& tokens = *res;
    REQUIRE_FALSE(tokens.empty());

    const auto& legend = HighlightLegend::default_legend();
    auto kw_opt = legend.token_type_index("keyword");
    auto type_opt = legend.token_type_index("type");
    REQUIRE(kw_opt.has_value());
    REQUIRE(type_opt.has_value());
    if (!kw_opt.has_value() || !type_opt.has_value()) {
        return;
    }
    uint32_t kw_idx = *kw_opt;
    uint32_t type_idx = *type_opt;

    bool found_class_kw = false;
    bool found_template_kw = false;
    bool found_constexpr_kw = false;
    bool found_int_type = false;

    for (const auto& tok : tokens) {
        std::string_view text = source.substr(tok.byte_range.start, tok.length);
        if (tok.token_type == kw_idx) {
            if (text == "class") {
                found_class_kw = true;
            }
            if (text == "template") {
                found_template_kw = true;
            }
            if (text == "constexpr") {
                found_constexpr_kw = true;
            }
        }
        if (tok.token_type == type_idx && text == "int") {
            found_int_type = true;
        }
    }

    CHECK(found_class_kw);
    CHECK(found_template_kw);
    CHECK(found_constexpr_kw);
    CHECK(found_int_type);
}

TEST_CASE("CppAdapter preprocessor conditional branch selection with -D and -U",
          "[adapter][cpp][preprocessor]") {
    CppAdapter adapter;
    CompileCommandContext ctx{
        .defines = {"FEATURE_ENABLED=1", "-UFEATURE_DISABLED"},
    };
    std::string_view source = R"(
#ifdef __cplusplus
void active_cpp_func(void);
#else
void inactive_c_func(void);
#endif

#ifdef FEATURE_ENABLED
void active_func_a(void);
#else
void inactive_func_a(void);
#endif

#ifdef FEATURE_DISABLED
void inactive_func_b(void);
#else
void active_func_b(void);
#endif

#ifndef FEATURE_DISABLED
void active_func_c(void);
#else
void inactive_func_c(void);
#endif

#if 0
void inactive_func_d(void);
#elif 1
void active_func_d(void);
#else
void inactive_func_e(void);
#endif

#define INFILE_CPP_SET 1
#ifdef INFILE_CPP_SET
void active_func_e(void);
#else
void inactive_func_f(void);
#endif

#undef INFILE_CPP_SET
#ifdef INFILE_CPP_SET
void inactive_func_g(void);
#else
void active_func_f(void);
#endif
)";
    auto res = adapter.parse(source, "test.cpp", ctx);
    REQUIRE(res.has_value());
    REQUIRE(res->status == worker::CompletionStatus::complete);

    auto has_symbol = [&](std::string_view name) {
        return std::ranges::any_of(res->symbols,
                                   [&](const SymbolFact& sym) { return sym.name == name; });
    };

    // Active functions should be indexed
    CHECK(has_symbol("active_cpp_func"));
    CHECK(has_symbol("active_func_a"));
    CHECK(has_symbol("active_func_b"));
    CHECK(has_symbol("active_func_c"));
    CHECK(has_symbol("active_func_d"));
    CHECK(has_symbol("active_func_e"));
    CHECK(has_symbol("active_func_f"));

    // Inactive functions should NOT be indexed
    CHECK_FALSE(has_symbol("inactive_c_func"));
    CHECK_FALSE(has_symbol("inactive_func_a"));
    CHECK_FALSE(has_symbol("inactive_func_b"));
    CHECK_FALSE(has_symbol("inactive_func_c"));
    CHECK_FALSE(has_symbol("inactive_func_d"));
    CHECK_FALSE(has_symbol("inactive_func_e"));
    CHECK_FALSE(has_symbol("inactive_func_f"));
    CHECK_FALSE(has_symbol("inactive_func_g"));

    // Macro names in directives should still be recorded as reference occurrences
    auto has_occurrence = [&](std::string_view name) {
        return std::ranges::any_of(res->occurrences, [&](const OccurrenceFact& occ) {
            return occ.kind == worker::FactKind::reference && occ.written_name == name;
        });
    };
    CHECK(has_occurrence("__cplusplus"));
    CHECK(has_occurrence("FEATURE_ENABLED"));
    CHECK(has_occurrence("FEATURE_DISABLED"));
    CHECK(has_occurrence("INFILE_CPP_SET"));
}

TEST_CASE("CppAdapter template method call receiver reference [P2]",
          "[adapter][cpp][template_call]") {
    CppAdapter adapter;
    std::string_view source = R"(
struct Payload {};

struct Container {
    template <typename T>
    T get() { return T{}; }
};

void run(Container obj, Container* ptr) {
    Payload a = obj.template get<Payload>();
    Payload b = ptr->template get<Payload>();
    Payload c = obj.get<Payload>();
}
)";
    auto res = adapter.parse(source, "test.cpp");
    REQUIRE(res.has_value());
    REQUIRE(res->status == worker::CompletionStatus::complete);

    // Verify calls to 'get'
    std::vector<OccurrenceFact> call_occurrences;
    for (const auto& occ : res->occurrences) {
        if (occ.kind == worker::FactKind::call && occ.written_name == "get") {
            call_occurrences.push_back(occ);
        }
    }
    CHECK(call_occurrences.size() == 3);

    // Verify receiver references to 'obj' and 'ptr'
    size_t obj_ref_count = 0;
    size_t ptr_ref_count = 0;
    for (const auto& occ : res->occurrences) {
        if (occ.kind == worker::FactKind::reference) {
            if (occ.written_name == "obj") {
                ++obj_ref_count;
            } else if (occ.written_name == "ptr") {
                ++ptr_ref_count;
            }
        }
    }
    // 'obj' appears as parameter, then twice as receiver (obj.template get<Payload>() and
    // obj.get<Payload>())
    CHECK(obj_ref_count >= 2);
    // 'ptr' appears as parameter, then as receiver (ptr->template get<Payload>())
    CHECK(ptr_ref_count >= 1);

    // Verify template argument references 'Payload'
    bool has_payload_ref = false;
    for (const auto& occ : res->occurrences) {
        if (occ.kind == worker::FactKind::reference && occ.written_name == "Payload") {
            has_payload_ref = true;
        }
    }
    CHECK(has_payload_ref);
}

TEST_CASE("CppAdapter parameter declaration suppression [Issue 1]", "[adapter][cpp][parameters]") {
    CppAdapter adapter;
    std::string_view source = R"(
int add(int a, int b) {
    return a + b;
}
)";
    auto res = adapter.parse(source, "test.cpp");
    REQUIRE(res.has_value());

    // 'a' and 'b' should appear as references inside the body (a + b),
    // but their declarations in the parameter list must NOT be reference occurrences.
    size_t a_count = 0;
    size_t b_count = 0;
    for (const auto& occ : res->occurrences) {
        if (occ.kind == worker::FactKind::reference) {
            if (occ.written_name == "a") {
                ++a_count;
            } else if (occ.written_name == "b") {
                ++b_count;
            }
        }
    }
    CHECK(a_count == 1);
    CHECK(b_count == 1);
}

TEST_CASE("CppAdapter template qualified-name decl-def linking and specialization [Issue 2]",
          "[adapter][cpp][templates]") {
    CppAdapter adapter;
    std::string_view source = R"(
template <typename T>
class Box {
public:
    void set(T val);
};

template <typename T>
void Box<T>::set(T val) {}

template <typename T>
struct Traits {
    static constexpr int val = 0;
};

template <typename T>
struct Traits<T*> {
    static constexpr int val = 1;
};
)";
    auto res = adapter.parse(source, "test.cpp");
    REQUIRE(res.has_value());

    // In-class declaration and out-of-line definition must share qualified_name "Box::set"
    const DeclarationFact* decl_set = nullptr;
    const DeclarationFact* def_set = nullptr;
    for (const auto& decl : res->declarations) {
        if (decl.symbol_name == "set") {
            if (!decl.is_definition) {
                decl_set = &decl;
            } else {
                def_set = &decl;
            }
        }
    }
    REQUIRE(decl_set != nullptr);
    REQUIRE(def_set != nullptr);
    CHECK(decl_set->qualified_name == "Box::set");
    CHECK(def_set->qualified_name == "Box::set");
    CHECK(decl_set->enclosing_scope == "Box");
    CHECK(def_set->enclosing_scope == "Box");

    // Specialization Traits<T*> should normalize to name "Traits"
    int traits_count = 0;
    for (const auto& sym : res->symbols) {
        if (sym.name == "Traits") {
            traits_count++;
            CHECK(sym.qualified_name == "Traits");
        }
    }
    CHECK(traits_count == 2);
}

TEST_CASE("CppAdapter function-pointer variables vs functions [Issue 3]",
          "[adapter][cpp][declarations]") {
    CppAdapter adapter;
    std::string_view source = R"(
void normal_func(int x);
void (*callback)(int x);
int* (*fn_ptr_returning_ptr)(double);
)";
    auto res = adapter.parse(source, "test.cpp");
    REQUIRE(res.has_value());

    const SymbolFact* sym_normal = nullptr;
    const SymbolFact* sym_cb = nullptr;
    const SymbolFact* sym_ret_ptr = nullptr;
    for (const auto& sym : res->symbols) {
        if (sym.name == "normal_func") {
            sym_normal = &sym;
        } else if (sym.name == "callback") {
            sym_cb = &sym;
        } else if (sym.name == "fn_ptr_returning_ptr") {
            sym_ret_ptr = &sym;
        }
    }

    REQUIRE(sym_normal != nullptr);
    CHECK(sym_normal->kind == NodeKind::function);

    REQUIRE(sym_cb != nullptr);
    CHECK(sym_cb->kind == NodeKind::variable);

    REQUIRE(sym_ret_ptr != nullptr);
    CHECK(sym_ret_ptr->kind == NodeKind::variable);
}

TEST_CASE("CppAdapter __cplusplus mapped to language standard [Issue 5]",
          "[adapter][cpp][standards]") {
    CppAdapter adapter;

    // C++11
    CompileCommandContext ctx11{
        .language_standard = "c++11",
    };
    std::string_view src11 = R"(
#if __cplusplus >= 201703L
void post_cpp17_func(void);
#else
void cpp11_func(void);
#endif
)";
    auto res11 = adapter.parse(src11, "test.cpp", ctx11);
    REQUIRE(res11.has_value());
    bool has_cpp11 = false;
    bool has_post17 = false;
    for (const auto& sym : res11->symbols) {
        if (sym.name == "cpp11_func") {
            has_cpp11 = true;
        }
        if (sym.name == "post_cpp17_func") {
            has_post17 = true;
        }
    }
    CHECK(has_cpp11);
    CHECK_FALSE(has_post17);

    // C++20
    CompileCommandContext ctx20{
        .language_standard = "-std=c++20",
    };
    std::string_view src20 = R"(
#if __cplusplus >= 202002L
void cpp20_func(void);
#else
void pre_cpp20_func(void);
#endif
)";
    auto res20 = adapter.parse(src20, "test.cpp", ctx20);
    REQUIRE(res20.has_value());
    bool has_cpp20 = false;
    bool has_pre20 = false;
    for (const auto& sym : res20->symbols) {
        if (sym.name == "cpp20_func") {
            has_cpp20 = true;
        }
        if (sym.name == "pre_cpp20_func") {
            has_pre20 = true;
        }
    }
    CHECK(has_cpp20);
    CHECK_FALSE(has_pre20);
}

TEST_CASE("CppAdapter auto variables references in range-for and structured bindings [Issue 4]",
          "[adapter][cpp][auto][issue4]") {
    CppAdapter adapter;
    std::string_view source = R"(
void test_range_for() {
    for (auto it : vec) {
        use(it);
    }
    for (const auto& item : items) {
        use(item);
    }
}

void test_structured_bindings() {
    auto [first, second] = get_pair();
    use(first);
    use(second);

    for (auto [key, val] : map) {
        use(key);
        use(val);
    }
}

void test_condition_declaration() {
    if (auto ptr = get_ptr()) {
        use(ptr);
    }
}

void test_range_for_init() {
    for (auto v = get_vec(); auto elem : v) {
        use(elem);
    }
}
)";

    auto res = adapter.parse(source, "test.cpp");
    REQUIRE(res.has_value());

    auto find_sym = [&](std::string_view name) -> const SymbolFact* {
        for (const auto& s : res->symbols) {
            if (s.name == name) {
                return &s;
            }
        }
        return nullptr;
    };

    auto find_decl = [&](std::string_view name) -> const DeclarationFact* {
        for (const auto& d : res->declarations) {
            if (d.symbol_name == name) {
                return &d;
            }
        }
        return nullptr;
    };

    auto count_occ = [&](std::string_view name, worker::FactKind kind) -> size_t {
        size_t count = 0;
        for (const auto& o : res->occurrences) {
            if (o.written_name == name && o.kind == kind) {
                ++count;
            }
        }
        return count;
    };

    // 1. Range-for variable 'it'
    const auto* sym_it = find_sym("it");
    REQUIRE(sym_it != nullptr);
    CHECK(sym_it->kind == NodeKind::variable);
    CHECK(sym_it->qualified_name == "test_range_for::it");
    CHECK(sym_it->signature == "auto it");
    const auto* decl_it = find_decl("it");
    REQUIRE(decl_it != nullptr);
    CHECK(decl_it->is_definition);
    // 'it' must be referenced in the body (use(it)), but NOT at its declaration site
    CHECK(count_occ("it", worker::FactKind::reference) == 1);

    // 2. Const ref range-for variable 'item'
    const auto* sym_item = find_sym("item");
    REQUIRE(sym_item != nullptr);
    CHECK(sym_item->kind == NodeKind::variable);
    CHECK(sym_item->signature == "const auto& item");
    CHECK(count_occ("item", worker::FactKind::reference) == 1);

    // 3. Structured bindings 'first' and 'second'
    const auto* sym_first = find_sym("first");
    const auto* sym_second = find_sym("second");
    REQUIRE(sym_first != nullptr);
    REQUIRE(sym_second != nullptr);
    CHECK(sym_first->kind == NodeKind::variable);
    CHECK(sym_second->kind == NodeKind::variable);
    CHECK(sym_first->qualified_name == "test_structured_bindings::first");
    CHECK(sym_second->qualified_name == "test_structured_bindings::second");
    const auto* decl_first = find_decl("first");
    const auto* decl_second = find_decl("second");
    REQUIRE(decl_first != nullptr);
    REQUIRE(decl_second != nullptr);
    CHECK(decl_first->is_definition);
    CHECK(decl_second->is_definition);
    CHECK(count_occ("first", worker::FactKind::reference) == 1);
    CHECK(count_occ("second", worker::FactKind::reference) == 1);
    CHECK(count_occ("get_pair", worker::FactKind::call) == 1);

    // 4. Structured bindings in range-for: 'key' and 'val'
    const auto* sym_key = find_sym("key");
    const auto* sym_val = find_sym("val");
    REQUIRE(sym_key != nullptr);
    REQUIRE(sym_val != nullptr);
    CHECK(sym_key->kind == NodeKind::variable);
    CHECK(sym_val->kind == NodeKind::variable);
    CHECK(sym_key->signature == "auto [key, val]");
    CHECK(sym_val->signature == "auto [key, val]");
    CHECK(count_occ("key", worker::FactKind::reference) == 1);
    CHECK(count_occ("val", worker::FactKind::reference) == 1);

    // 5. Condition declaration 'ptr' and function call 'get_ptr'
    const auto* sym_ptr = find_sym("ptr");
    REQUIRE(sym_ptr != nullptr);
    CHECK(sym_ptr->kind == NodeKind::variable);
    CHECK(count_occ("ptr", worker::FactKind::reference) == 1);
    CHECK(count_occ("get_ptr", worker::FactKind::call) == 1);

    // 6. Range-for with init statement: 'v' and 'elem'
    const auto* sym_v = find_sym("v");
    const auto* sym_elem = find_sym("elem");
    REQUIRE(sym_v != nullptr);
    REQUIRE(sym_elem != nullptr);
    CHECK(sym_v->kind == NodeKind::variable);
    CHECK(sym_elem->kind == NodeKind::variable);
    CHECK(count_occ("get_vec", worker::FactKind::call) == 1);
    CHECK(count_occ("v", worker::FactKind::reference) == 1);
    CHECK(count_occ("elem", worker::FactKind::reference) == 1);
}

TEST_CASE("C++ classes decorated with export macros are parsed as classes not functions", "[adapter][cpp][classes]") {
    CppAdapter adapter;
    std::string_view source = R"(
#define FOO_API
#define DLL_EXPORT
#define MATH_API
#define EXPORT

class FOO_API Bar {
public:
    Bar();
    ~Bar();
    void do_something();
    int get_val() const { return val_; }

private:
    int val_;
    std::string name_;
};

class DLL_EXPORT BarDerived : public Base {
public:
    void do_something();
};

class API BarFinal final : public Base {
public:
    void do_something();
};

class DLL_EXPORT ALIGN_16 MultiMacro {
public:
    void run();
};

template <typename T>
class MATH_API Matrix {
public:
    Matrix(int r, int c);
    void transpose();
private:
    T* data_;
};

struct EXPORT Point3D {
    float x;
    float y;
    float z;
};

union EXPORT ValueUnion {
    int i;
    float f;
};

class FOO_API ForwardClass;
struct DLL_EXPORT ForwardStruct;

template <class T>
class Foo {
public:
    T val;
};

template <class T>
class FooForward;

template <class T>
class FOO_API FooWithMacro {
public:
    T val;
};

struct Point pt;
struct Point get_origin() { return Point{}; }
)";

    auto res = adapter.parse(source, "src/classes.cpp");
    REQUIRE(res.has_value());
    const AdapterResult& result = *res;

    auto find_sym = [&](std::string_view name) -> const SymbolFact* {
        for (const auto& s : result.symbols) {
            if (s.name == name) return &s;
        }
        return nullptr;
    };

    auto count_sym = [&](std::string_view name, NodeKind kind) -> size_t {
        size_t count = 0;
        for (const auto& s : result.symbols) {
            if (s.name == name && s.kind == kind) ++count;
        }
        return count;
    };

    auto find_decl = [&](std::string_view name) -> const DeclarationFact* {
        for (const auto& d : result.declarations) {
            if (d.symbol_name == name) return &d;
        }
        return nullptr;
    };

    // 1. Basic class with export macro
    const auto* sym_bar = find_sym("Bar");
    REQUIRE(sym_bar != nullptr);
    CHECK(sym_bar->kind == NodeKind::class_);
    CHECK(sym_bar->qualified_name == "Bar");

    // Methods inside Bar must be method, not function
    CHECK(count_sym("Bar", NodeKind::method) >= 1);
    const auto* sym_dtor = find_sym("~Bar");
    REQUIRE(sym_dtor != nullptr);
    CHECK(sym_dtor->kind == NodeKind::method);
    CHECK(sym_dtor->enclosing_scope == "Bar");

    const auto* sym_do_something = find_sym("do_something");
    REQUIRE(sym_do_something != nullptr);
    CHECK(sym_do_something->kind == NodeKind::method);

    const auto* sym_get_val = find_sym("get_val");
    REQUIRE(sym_get_val != nullptr);
    CHECK(sym_get_val->kind == NodeKind::method);
    CHECK(sym_get_val->enclosing_scope == "Bar");

    // Fields inside Bar must be field, not variable
    const auto* sym_val = find_sym("val_");
    REQUIRE(sym_val != nullptr);
    CHECK(sym_val->kind == NodeKind::field);
    CHECK(sym_val->enclosing_scope == "Bar");

    const auto* sym_name = find_sym("name_");
    REQUIRE(sym_name != nullptr);
    CHECK(sym_name->kind == NodeKind::field);
    CHECK(sym_name->enclosing_scope == "Bar");

    // 2. Derived class with macro
    const auto* sym_derived = find_sym("BarDerived");
    REQUIRE(sym_derived != nullptr);
    CHECK(sym_derived->kind == NodeKind::class_);

    // Check inheritance occurrence for Base
    bool found_base_inheritance = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::inheritance && occ.written_name == "Base") {
            found_base_inheritance = true;
            break;
        }
    }
    CHECK(found_base_inheritance);

    // 3. Class with final and macro
    const auto* sym_final = find_sym("BarFinal");
    REQUIRE(sym_final != nullptr);
    CHECK(sym_final->kind == NodeKind::class_);

    // 4. Class with multiple macros
    const auto* sym_multi = find_sym("MultiMacro");
    REQUIRE(sym_multi != nullptr);
    CHECK(sym_multi->kind == NodeKind::class_);

    // 5. Template class with macro
    const auto* sym_matrix = find_sym("Matrix");
    REQUIRE(sym_matrix != nullptr);
    CHECK(sym_matrix->kind == NodeKind::class_);
    const auto* sym_transpose = find_sym("transpose");
    REQUIRE(sym_transpose != nullptr);
    CHECK(sym_transpose->kind == NodeKind::method);
    CHECK(sym_transpose->enclosing_scope == "Matrix");
    const auto* sym_data = find_sym("data_");
    REQUIRE(sym_data != nullptr);
    CHECK(sym_data->kind == NodeKind::field);

    // 6. Struct with macro
    const auto* sym_point3d = find_sym("Point3D");
    REQUIRE(sym_point3d != nullptr);
    CHECK(sym_point3d->kind == NodeKind::struct_);
    const auto* sym_x = find_sym("x");
    REQUIRE(sym_x != nullptr);
    CHECK(sym_x->kind == NodeKind::field);

    // 7. Union with macro
    const auto* sym_union = find_sym("ValueUnion");
    REQUIRE(sym_union != nullptr);
    CHECK(sym_union->kind == NodeKind::struct_);

    // 8. Forward declarations with macros
    const auto* decl_fwd_class = find_decl("ForwardClass");
    REQUIRE(decl_fwd_class != nullptr);
    CHECK(decl_fwd_class->kind == NodeKind::class_);
    CHECK_FALSE(decl_fwd_class->is_definition);

    const auto* decl_fwd_struct = find_decl("ForwardStruct");
    REQUIRE(decl_fwd_struct != nullptr);
    CHECK(decl_fwd_struct->kind == NodeKind::struct_);
    CHECK_FALSE(decl_fwd_struct->is_definition);

    // 8b. template <class T> class Foo
    const auto* sym_foo = find_sym("Foo");
    REQUIRE(sym_foo != nullptr);
    CHECK(sym_foo->kind == NodeKind::class_);
    const auto* sym_foo_val = find_sym("val");
    REQUIRE(sym_foo_val != nullptr);
    CHECK(sym_foo_val->kind == NodeKind::field);

    const auto* decl_foo_fwd = find_decl("FooForward");
    REQUIRE(decl_foo_fwd != nullptr);
    CHECK(decl_foo_fwd->kind == NodeKind::class_);
    CHECK_FALSE(decl_foo_fwd->is_definition);

    const auto* sym_foo_macro = find_sym("FooWithMacro");
    REQUIRE(sym_foo_macro != nullptr);
    CHECK(sym_foo_macro->kind == NodeKind::class_);
    CHECK(sym_foo_macro->signature == "template <class T> class FOO_API FooWithMacro");

    // 9. Non-class constructs must NOT be misparsed
    const auto* sym_pt = find_sym("pt");
    REQUIRE(sym_pt != nullptr);
    CHECK(sym_pt->kind == NodeKind::variable);

    const auto* sym_get_origin = find_sym("get_origin");
    REQUIRE(sym_get_origin != nullptr);
    CHECK(sym_get_origin->kind == NodeKind::function);

    // 10. Check macro reference occurrences
    bool found_foo_api = false;
    bool found_dll_export = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::reference) {
            if (occ.written_name == "FOO_API") found_foo_api = true;
            if (occ.written_name == "DLL_EXPORT") found_dll_export = true;
        }
    }
    CHECK(found_foo_api);
    CHECK(found_dll_export);

    // 11. Highlighting verification: Bar must be highlighted as type
    auto hl_res = adapter.highlight(source);
    REQUIRE(hl_res.has_value());
    const auto& tokens = *hl_res;
    const auto& legend = HighlightLegend::default_legend();
    auto type_idx = legend.token_type_index("type");
    REQUIRE(type_idx.has_value());

    CoordinateConverter conv(source);
    auto bar_point = conv.byte_to_point(sym_bar->range.start);
    bool bar_highlighted_as_type = false;
    for (const auto& tok : tokens) {
        if (tok.line == bar_point.line && tok.start_column == bar_point.column && tok.token_type == *type_idx) {
            bar_highlighted_as_type = true;
            break;
        }
    }
    CHECK(bar_highlighted_as_type);
}

