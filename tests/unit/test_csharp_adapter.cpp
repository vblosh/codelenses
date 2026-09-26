#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "codelenses/adapters/adapter.hpp"
#include "codelenses/adapters/csharp_adapter.hpp"
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

TEST_CASE("H3-01: C# adapter grammar registration and file extensions", "[adapter][csharp][h3]") {
    // 1. Grammar registration
    const auto* ts_lang = treesitter::grammar_for_language(Language::csharp);
    REQUIRE(ts_lang != nullptr);
    REQUIRE(treesitter::has_grammar_for_language(Language::csharp));
    REQUIRE(treesitter::grammar_version(Language::csharp) == "0.23.1");

    // 2. File extension mappings
    REQUIRE(language_from_extension(".cs") == Language::csharp);
    REQUIRE(language_from_extension("cs") == Language::csharp);

    // 3. Path detection
    REQUIRE(language_from_path("src/Program.cs") == Language::csharp);
    REQUIRE(language_from_path("Models/User.cs") == Language::csharp);

    // 4. String conversion
    auto parsed_cs = language_from_string("c#");
    REQUIRE(parsed_cs.has_value());
    REQUIRE(*parsed_cs == Language::csharp);

    auto parsed_csharp = language_from_string("csharp");
    REQUIRE(parsed_csharp.has_value());
    REQUIRE(*parsed_csharp == Language::csharp);

    REQUIRE(to_string(Language::csharp) == "C#");

    // 5. Registry integration
    auto& registry = default_adapter_registry();
    REQUIRE(registry.has_adapter(Language::csharp));

    auto* adapter_by_lang = registry.get_adapter(Language::csharp);
    REQUIRE(adapter_by_lang != nullptr);
    REQUIRE(adapter_by_lang->language() == Language::csharp);
    REQUIRE(adapter_by_lang->name() == "CSharpAdapter");

    auto* adapter_by_path = registry.get_adapter_for_path("src/App.cs");
    REQUIRE(adapter_by_path != nullptr);
    REQUIRE(adapter_by_path->language() == Language::csharp);

    // 6. Capabilities check
    const auto& caps = adapter_by_lang->capabilities();
    REQUIRE(caps.functions == CapabilityStatus::supported);
    REQUIRE(caps.methods == CapabilityStatus::supported);
    REQUIRE(caps.classes == CapabilityStatus::supported);
    REQUIRE(caps.structs == CapabilityStatus::supported);
    REQUIRE(caps.interfaces == CapabilityStatus::supported);
    REQUIRE(caps.enums == CapabilityStatus::supported);
    REQUIRE(caps.records == CapabilityStatus::supported);
    REQUIRE(caps.namespaces == CapabilityStatus::supported);
    REQUIRE(caps.variables == CapabilityStatus::supported);
    REQUIRE(caps.templates == CapabilityStatus::supported);
    REQUIRE(caps.partial_types == CapabilityStatus::supported);
    REQUIRE(caps.containment == CapabilityStatus::supported);
    REQUIRE(caps.calls == CapabilityStatus::supported);
    REQUIRE(caps.references == CapabilityStatus::supported);
    REQUIRE(caps.inheritance == CapabilityStatus::supported);
    REQUIRE(caps.implementation == CapabilityStatus::supported);
    REQUIRE(caps.imports == CapabilityStatus::supported);
    REQUIRE(caps.includes == CapabilityStatus::unavailable);
    REQUIRE(caps.modules == CapabilityStatus::unavailable);
    REQUIRE(caps.packages == CapabilityStatus::unavailable);
}

TEST_CASE("H3-01: C# adapter extraction of namespaces, types, methods, properties, fields, using "
          "directives",
          "[adapter][csharp][h3]") {
    CSharpAdapter adapter;

    std::string_view source = R"(
using System;
using System.Collections.Generic;
using static System.Math;
using StringList = System.Collections.Generic.List<string>;

namespace App.Core {

public delegate void StateCallback(int status);

public enum Priority {
    Low = 0,
    Medium = 1,
    High = 2
}

public interface IProcessor {
    void Run();
}

public struct Vector3 {
    public float X;
    public float Y;
    public float Z;
}

public record struct Dimensions(double Width, double Height);

public record Person(string Name, int Age);

public class Calculator : IProcessor {
    private int _seed;
    private int _min, _max;

    public Calculator(int seed) {
        _seed = seed;
    }

    ~Calculator() {
    }

    public int Seed {
        get { return _seed; }
        set { _seed = value; }
    }

    public int DoubleSeed => _seed * 2;

    public string this[int index] {
        get { return index.ToString(); }
    }

    public void Run() {
        int a = 10;
        int b = 20;
        int res = Add(a, b);
    }

    public int Add(int a, int b) {
        return a + b;
    }

    public int Add(int a, int b, int c) {
        return a + b + c;
    }
}

}
)";

    auto res = adapter.parse(source, "src/App.cs");
    REQUIRE(res.has_value());
    const AdapterResult& result = *res;

    REQUIRE(result.language == Language::csharp);
    REQUIRE(result.status == worker::CompletionStatus::complete);
    REQUIRE(result.diagnostics.empty());

    // 1. Verify Using Directives
    bool found_sys = false;
    bool found_sys_col = false;
    bool found_static_math = false;
    bool found_alias_list = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::import) {
            if (occ.written_name == "System")
                found_sys = true;
            if (occ.written_name == "System.Collections.Generic")
                found_sys_col = true;
            if (occ.written_name == "System.Math") {
                found_static_math = true;
                CHECK(std::ranges::find(occ.candidate_targets, "Math") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "System.Collections.Generic.List<string>")
                found_alias_list = true;
        }
    }
    REQUIRE(found_sys);
    REQUIRE(found_sys_col);
    REQUIRE(found_static_math);
    REQUIRE(found_alias_list);

    // Using alias symbol
    bool found_alias_sym = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "StringList" && sym.kind == NodeKind::type_alias) {
            found_alias_sym = true;
        }
    }
    REQUIRE(found_alias_sym);

    // 2. Verify Namespace
    bool found_ns = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "App.Core" && sym.kind == NodeKind::namespace_) {
            found_ns = true;
            CHECK(sym.qualified_name == "App.Core");
        }
    }
    REQUIRE(found_ns);

    // 3. Verify Delegate
    bool found_delegate = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "StateCallback" && sym.kind == NodeKind::type_alias) {
            found_delegate = true;
            CHECK(sym.qualified_name == "App.Core.StateCallback");
            CHECK(sym.enclosing_scope == "App.Core");
        }
    }
    REQUIRE(found_delegate);

    for (const auto& decl : result.declarations) {
        if (decl.symbol_name == "StateCallback" && decl.enclosing_scope == "App.Core") {
            CHECK(decl.is_definition);
        }
    }

    // 4. Verify Enum and Members
    bool found_enum = false;
    bool found_low = false;
    bool found_med = false;
    bool found_high = false;

    for (const auto& sym : result.symbols) {
        if (sym.name == "Priority" && sym.kind == NodeKind::enum_) {
            found_enum = true;
            CHECK(sym.qualified_name == "App.Core.Priority");
            CHECK(sym.enclosing_scope == "App.Core");
        }
        if (sym.name == "Low" && sym.kind == NodeKind::enum_member) {
            found_low = true;
            CHECK(sym.qualified_name == "App.Core.Priority.Low");
            CHECK(sym.enclosing_scope == "App.Core.Priority");
        }
        if (sym.name == "Medium" && sym.kind == NodeKind::enum_member) {
            found_med = true;
            CHECK(sym.qualified_name == "App.Core.Priority.Medium");
        }
        if (sym.name == "High" && sym.kind == NodeKind::enum_member) {
            found_high = true;
            CHECK(sym.qualified_name == "App.Core.Priority.High");
        }
    }
    REQUIRE(found_enum);
    REQUIRE(found_low);
    REQUIRE(found_med);
    REQUIRE(found_high);

    // 5. Verify Interface
    bool found_iface = false;
    bool found_iface_run = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "IProcessor" && sym.kind == NodeKind::interface_) {
            found_iface = true;
            CHECK(sym.qualified_name == "App.Core.IProcessor");
        }
        if (sym.name == "Run" && sym.enclosing_scope == "App.Core.IProcessor") {
            found_iface_run = true;
            CHECK(sym.kind == NodeKind::method);
        }
    }
    REQUIRE(found_iface);
    REQUIRE(found_iface_run);

    // Interface method declaration has is_definition == false
    for (const auto& decl : result.declarations) {
        if (decl.symbol_name == "Run" && decl.enclosing_scope == "App.Core.IProcessor") {
            CHECK_FALSE(decl.is_definition);
        }
    }

    // 6. Verify Struct and Fields
    bool found_struct = false;
    bool found_vx = false;
    bool found_vy = false;
    bool found_vz = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "Vector3" && sym.kind == NodeKind::struct_) {
            found_struct = true;
            CHECK(sym.qualified_name == "App.Core.Vector3");
        }
        if (sym.name == "X" && sym.enclosing_scope == "App.Core.Vector3") {
            found_vx = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "Y" && sym.enclosing_scope == "App.Core.Vector3")
            found_vy = true;
        if (sym.name == "Z" && sym.enclosing_scope == "App.Core.Vector3")
            found_vz = true;
    }
    REQUIRE(found_struct);
    REQUIRE(found_vx);
    REQUIRE(found_vy);
    REQUIRE(found_vz);

    // 7. Verify Records
    bool found_rec_struct = false;
    bool found_record = false;
    bool found_dim_width = false;
    bool found_dim_height = false;
    bool found_person_name = false;
    bool found_person_age = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "Dimensions" && sym.kind == NodeKind::struct_) {
            found_rec_struct = true;
            CHECK(sym.qualified_name == "App.Core.Dimensions");
        }
        if (sym.name == "Person" && sym.kind == NodeKind::class_) {
            found_record = true;
            CHECK(sym.qualified_name == "App.Core.Person");
        }
        if (sym.name == "Width" && sym.enclosing_scope == "App.Core.Dimensions") {
            found_dim_width = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "Height" && sym.enclosing_scope == "App.Core.Dimensions") {
            found_dim_height = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "Name" && sym.enclosing_scope == "App.Core.Person") {
            found_person_name = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "Age" && sym.enclosing_scope == "App.Core.Person") {
            found_person_age = true;
            CHECK(sym.kind == NodeKind::field);
        }
    }
    REQUIRE(found_rec_struct);
    REQUIRE(found_record);
    REQUIRE(found_dim_width);
    REQUIRE(found_dim_height);
    REQUIRE(found_person_name);
    REQUIRE(found_person_age);

    // 8. Verify Class, Constructor, Destructor, Methods, Properties, Fields
    bool found_calc = false;
    bool found_ctor = false;
    bool found_dtor = false;
    bool found_seed_field = false;
    bool found_min_field = false;
    bool found_max_field = false;
    bool found_seed_prop = false;
    bool found_double_seed_prop = false;
    bool found_indexer = false;
    bool found_calc_run = false;
    int add_methods_count = 0;

    for (const auto& sym : result.symbols) {
        if (sym.name == "Calculator" && sym.kind == NodeKind::class_) {
            found_calc = true;
            CHECK(sym.qualified_name == "App.Core.Calculator");
        }
        if (sym.name == "Calculator" && sym.kind == NodeKind::method) {
            found_ctor = true;
            CHECK(sym.qualified_name == "App.Core.Calculator.Calculator");
            CHECK(sym.enclosing_scope == "App.Core.Calculator");
        }
        if (sym.name == "~Calculator" && sym.kind == NodeKind::method) {
            found_dtor = true;
            CHECK(sym.qualified_name == "App.Core.Calculator.~Calculator");
        }
        if (sym.name == "_seed" && sym.enclosing_scope == "App.Core.Calculator") {
            found_seed_field = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "_min" && sym.enclosing_scope == "App.Core.Calculator") {
            found_min_field = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "_max" && sym.enclosing_scope == "App.Core.Calculator") {
            found_max_field = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "Seed" && sym.enclosing_scope == "App.Core.Calculator") {
            found_seed_prop = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "DoubleSeed" && sym.enclosing_scope == "App.Core.Calculator") {
            found_double_seed_prop = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "this" && sym.enclosing_scope == "App.Core.Calculator") {
            found_indexer = true;
            CHECK(sym.kind == NodeKind::field);
            std::string_view sym_span =
                source.substr(sym.range.start, sym.range.end - sym.range.start);
            CHECK(sym_span == "this");
        }
        if (sym.name == "Run" && sym.enclosing_scope == "App.Core.Calculator") {
            found_calc_run = true;
            CHECK(sym.kind == NodeKind::method);
        }
        if (sym.name == "Add" && sym.enclosing_scope == "App.Core.Calculator") {
            add_methods_count++;
            CHECK(sym.kind == NodeKind::method);
        }
    }

    REQUIRE(found_calc);
    REQUIRE(found_ctor);
    REQUIRE(found_dtor);
    REQUIRE(found_seed_field);
    REQUIRE(found_min_field);
    REQUIRE(found_max_field);
    REQUIRE(found_seed_prop);
    REQUIRE(found_double_seed_prop);
    REQUIRE(found_indexer);
    REQUIRE(found_calc_run);
    REQUIRE(add_methods_count == 2);

    // Overload distinctness check
    std::vector<std::string> add_keys;
    for (const auto& sym : result.symbols) {
        if (sym.name == "Add" && sym.enclosing_scope == "App.Core.Calculator") {
            add_keys.push_back(resolver::generate_symbol_key(Language::csharp, "src/App.cs",
                                                             NodeKind::method, sym.qualified_name,
                                                             sym.signature));
        }
    }
    REQUIRE(add_keys.size() == 2);
    CHECK(add_keys[0] != add_keys[1]);
}

TEST_CASE("H3-02: C# adapter extraction of inheritance, interfaces, calls, and attributes",
          "[adapter][csharp][h3]") {
    CSharpAdapter adapter;

    std::string_view source = R"(
using System;

namespace Enterprise.Domain {

public class Animal {
    public virtual void Speak() {}
}

public interface IPet {
    void Play();
}

public interface IAdoptable : IPet {
}

[Serializable]
[Obsolete("Use DogV2 instead")]
public class Dog : Animal, IAdoptable, IDisposable {
    [NonSerialized]
    private string _tag;

    [Obsolete]
    public void Speak() {
        Play();
    }

    public void Play() {
    }

    public void Dispose() {
    }
}

public class AdoptionCenter {
    public void Process() {
        Dog dog = new Dog();
        dog.Speak();
        dog.Play();
    }
}

}
)";

    auto res = adapter.parse(source, "src/Enterprise.cs");
    REQUIRE(res.has_value());
    const AdapterResult& result = *res;
    REQUIRE(result.status == worker::CompletionStatus::complete);

    // 1. Verify Inheritance and Interface Occurrences
    bool found_animal_base = false;
    bool found_adoptable_iface = false;
    bool found_disposable_iface = false;
    bool found_pet_base_of_adoptable = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::inheritance) {
            if (occ.written_name == "Animal" && occ.enclosing_scope == "Enterprise.Domain.Dog") {
                found_animal_base = true;
                CHECK(std::ranges::find(occ.candidate_targets, "Animal") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "IPet" &&
                occ.enclosing_scope == "Enterprise.Domain.IAdoptable") {
                found_pet_base_of_adoptable = true;
            }
        }
        if (occ.kind == worker::FactKind::implementation) {
            if (occ.written_name == "IAdoptable" &&
                occ.enclosing_scope == "Enterprise.Domain.Dog") {
                found_adoptable_iface = true;
                CHECK(std::ranges::find(occ.candidate_targets, "IAdoptable") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "IDisposable" &&
                occ.enclosing_scope == "Enterprise.Domain.Dog") {
                found_disposable_iface = true;
                CHECK(std::ranges::find(occ.candidate_targets, "IDisposable") !=
                      occ.candidate_targets.end());
            }
        }
    }

    REQUIRE(found_animal_base);
    REQUIRE(found_adoptable_iface);
    REQUIRE(found_disposable_iface);
    REQUIRE(found_pet_base_of_adoptable);

    // 2. Verify Attributes
    bool found_serializable = false;
    bool found_obsolete_class = false;
    bool found_nonserialized_field = false;
    bool found_obsolete_method = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::reference) {
            if (occ.written_name == "Serializable") {
                found_serializable = true;
                CHECK(std::ranges::find(occ.candidate_targets, "SerializableAttribute") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "Obsolete") {
                if (occ.enclosing_scope == "Enterprise.Domain" ||
                    occ.enclosing_scope == "Enterprise.Domain.Dog") {
                    found_obsolete_class = true;
                }
                if (occ.enclosing_scope == "Enterprise.Domain.Dog.Speak") {
                    found_obsolete_method = true;
                }
                CHECK(std::ranges::find(occ.candidate_targets, "ObsoleteAttribute") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "NonSerialized") {
                found_nonserialized_field = true;
                CHECK(std::ranges::find(occ.candidate_targets, "NonSerializedAttribute") !=
                      occ.candidate_targets.end());
            }
        }
    }

    REQUIRE(found_serializable);
    REQUIRE(found_obsolete_class);
    REQUIRE(found_nonserialized_field);
    REQUIRE(found_obsolete_method);

    // 3. Verify Calls
    bool found_new_dog_call = false;
    bool found_speak_call = false;
    bool found_play_call = false;
    bool found_internal_play_call = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::call) {
            if (occ.written_name == "Dog") {
                found_new_dog_call = true;
                CHECK(std::ranges::find(occ.candidate_targets, "Dog") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "dog.Speak") {
                found_speak_call = true;
                CHECK(std::ranges::find(occ.candidate_targets, "Speak") !=
                      occ.candidate_targets.end());
                std::string_view occ_span =
                    source.substr(occ.range.start, occ.range.end - occ.range.start);
                CHECK(occ_span == "dog.Speak");
            }
            if (occ.written_name == "dog.Play") {
                found_play_call = true;
                CHECK(std::ranges::find(occ.candidate_targets, "Play") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "Play" &&
                occ.enclosing_scope == "Enterprise.Domain.Dog.Speak") {
                found_internal_play_call = true;
            }
        }
    }

    REQUIRE(found_new_dog_call);
    REQUIRE(found_speak_call);
    REQUIRE(found_play_call);
    REQUIRE(found_internal_play_call);
}

TEST_CASE("H3-02: C# adapter syntax highlighting query and token generation",
          "[adapter][csharp][h3][highlight]") {
    CSharpAdapter adapter;

    std::string_view source = R"(
using System;

namespace Demo {

[Serializable]
public class Widget {
    public int Value { get; set; }

    public Widget(int value) {
        Value = value;
    }

    public int Compute(int factor) {
        return Value * factor;
    }
}

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
    auto class_opt = legend.token_type_index("class");
    auto method_opt = legend.token_type_index("method");
    auto dec_opt = legend.token_type_index("decorator");

    REQUIRE(kw_opt.has_value());
    REQUIRE(type_opt.has_value());
    REQUIRE(class_opt.has_value());
    REQUIRE(method_opt.has_value());
    REQUIRE(dec_opt.has_value());

    uint32_t kw_idx = *kw_opt;
    uint32_t type_idx = *type_opt;
    uint32_t class_idx = *class_opt;
    uint32_t method_idx = *method_opt;
    uint32_t dec_idx = *dec_opt;

    bool found_class_kw = false;
    bool found_public_kw = false;
    bool found_int_type = false;
    bool found_widget_class = false;
    bool found_compute_method = false;
    bool found_serializable_dec = false;

    for (const auto& tok : tokens) {
        std::string_view text = source.substr(tok.byte_range.start, tok.length);
        if (tok.token_type == kw_idx) {
            if (text == "class")
                found_class_kw = true;
            if (text == "public")
                found_public_kw = true;
        }
        if (tok.token_type == type_idx && text == "int") {
            found_int_type = true;
        }
        if (tok.token_type == class_idx && text == "Widget") {
            found_widget_class = true;
        }
        if (tok.token_type == method_idx && text == "Compute") {
            found_compute_method = true;
        }
        if (tok.token_type == dec_idx && text == "Serializable") {
            found_serializable_dec = true;
        }
    }

    CHECK(found_class_kw);
    CHECK(found_public_kw);
    CHECK(found_int_type);
    CHECK(found_widget_class);
    CHECK(found_compute_method);
    CHECK(found_serializable_dec);
}

TEST_CASE("H3-03: C# adapter fixtures for generics and partial declarations",
          "[adapter][csharp][h3][fixtures]") {
    CSharpAdapter adapter;

    auto find_fixture_dir = []() -> std::filesystem::path {
#ifdef CODELENSES_SOURCE_DIR
        std::filesystem::path p =
            std::filesystem::path(CODELENSES_SOURCE_DIR) / "tests/fixtures/csharp";
        if (std::filesystem::exists(p)) {
            return p;
        }
#endif
        for (const auto& candidate : {std::filesystem::path("tests/fixtures/csharp"),
                                      std::filesystem::path("../../tests/fixtures/csharp"),
                                      std::filesystem::path("../tests/fixtures/csharp")}) {
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
        return "tests/fixtures/csharp";
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
        REQUIRE(result.language == Language::csharp);
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

    SECTION("Generics fixture") {
        test_fixture("generics", ".cs");
    }

    SECTION("Partial declarations fixture") {
        test_fixture("partial", ".cs");
    }
}

TEST_CASE("H3-04: C# adapter review findings regression tests", "[adapter][csharp][h3]") {
    CSharpAdapter adapter;

    std::string_view source = R"(
global using System.Threading;
using System;

namespace Advanced.Features;

[AttributeUsage(AttributeTargets.All)]
public class CustomAttrAttribute : Attribute {
    public CustomAttrAttribute(Type targetType, int code) {}
    public string Tag { get; set; }
}

public interface IBaseEntity {}

public class Person {
    public string Name { get; set; }
    public Person(string name) {
        Name = name;
    }
}

public record Employee(string Name, int Id) : Person(Name), IBaseEntity;

public struct Complex {
    public double Real;
    public double Imag;

    public Complex(double real, double imag) {
        Real = real;
        Imag = imag;
    }

    public Complex(double real) : this(real, 0.0) {}

    public static Complex operator +(Complex a, Complex b) {
        return new Complex(a.Real + b.Real, a.Imag + b.Imag);
    }

    public static implicit operator double(Complex c) {
        return c.Real;
    }
}

public class BaseService {
    public BaseService(string config) {}
}

public class AdvancedService : BaseService {
    public event Action<string> OnSimpleEvent;

    private Action<string> _customHandler;
    public event Action<string> OnCustomEvent {
        add {
            _customHandler += value;
        }
        remove {
            _customHandler -= value;
        }
    }

    public AdvancedService(string cfg) : base(cfg) {}

    [CustomAttr(typeof(Complex), 42, Tag = "production")]
    public Complex Calculate(Complex input) {
        return input;
    }

    public void TestCalls(AdvancedService svc, int[] numbers) {
        svc?.Calculate(new Complex(1.0, 2.0));
        var item = numbers?[0];
    }
}
)";

    auto res = adapter.parse(source, "src/AdvancedFeatures.cs");
    REQUIRE(res.has_value());
    const AdapterResult& result = *res;
    REQUIRE(result.status == worker::CompletionStatus::complete);
    REQUIRE(result.diagnostics.empty());

    // 1. File-scoped namespace
    bool found_ns = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "Advanced.Features" && sym.kind == NodeKind::namespace_) {
            found_ns = true;
            CHECK(sym.qualified_name == "Advanced.Features");
        }
        if (sym.name == "Person" && sym.kind == NodeKind::class_) {
            CHECK(sym.enclosing_scope == "Advanced.Features");
            CHECK(sym.qualified_name == "Advanced.Features.Person");
        }
    }
    REQUIRE(found_ns);

    // 2. Global using directive
    bool found_global_using = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::import && occ.written_name == "System.Threading") {
            found_global_using = true;
        }
    }
    REQUIRE(found_global_using);

    // 3. Record primary constructor base call: Person(Name)
    // Written name must be "Person", NOT "Person(Name)"
    bool found_person_base = false;
    bool found_base_entity_iface = false;
    bool found_person_arg_name = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::inheritance &&
            occ.enclosing_scope == "Advanced.Features.Employee") {
            if (occ.written_name == "Person") {
                found_person_base = true;
                CHECK(std::ranges::find(occ.candidate_targets, "Person") !=
                      occ.candidate_targets.end());
            }
        }
        if (occ.kind == worker::FactKind::implementation &&
            occ.enclosing_scope == "Advanced.Features.Employee") {
            if (occ.written_name == "IBaseEntity") {
                found_base_entity_iface = true;
            }
        }
        // Base argument 'Name' passed in Person(Name)
        if (occ.kind == worker::FactKind::reference && occ.written_name == "Name" &&
            occ.enclosing_scope == "Advanced.Features.Employee") {
            found_person_arg_name = true;
        }
    }
    REQUIRE(found_person_base);
    REQUIRE(found_base_entity_iface);
    REQUIRE(found_person_arg_name);

    // Record positional parameters
    bool found_emp_name = false;
    bool found_emp_id = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "Name" && sym.enclosing_scope == "Advanced.Features.Employee") {
            found_emp_name = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "Id" && sym.enclosing_scope == "Advanced.Features.Employee") {
            found_emp_id = true;
            CHECK(sym.kind == NodeKind::field);
        }
    }
    REQUIRE(found_emp_name);
    REQUIRE(found_emp_id);

    // 4. Operators (+ and implicit operator double)
    bool found_op_plus_sym = false;
    bool found_op_plus_decl = false;
    bool found_conv_op_sym = false;
    bool found_conv_op_decl = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "operator +" && sym.enclosing_scope == "Advanced.Features.Complex") {
            found_op_plus_sym = true;
            CHECK(sym.kind == NodeKind::method);
            CHECK(sym.qualified_name == "Advanced.Features.Complex.operator +");
        }
        if (sym.name == "implicit operator double" &&
            sym.enclosing_scope == "Advanced.Features.Complex") {
            found_conv_op_sym = true;
            CHECK(sym.kind == NodeKind::method);
            CHECK(sym.qualified_name == "Advanced.Features.Complex.implicit operator double");
        }
    }
    for (const auto& decl : result.declarations) {
        if (decl.symbol_name == "operator +" &&
            decl.enclosing_scope == "Advanced.Features.Complex") {
            found_op_plus_decl = true;
            CHECK(decl.is_definition);
        }
        if (decl.symbol_name == "implicit operator double" &&
            decl.enclosing_scope == "Advanced.Features.Complex") {
            found_conv_op_decl = true;
            CHECK(decl.is_definition);
        }
    }
    REQUIRE(found_op_plus_sym);
    REQUIRE(found_op_plus_decl);
    REQUIRE(found_conv_op_sym);
    REQUIRE(found_conv_op_decl);

    // 5. Constructor initializers (: this(...) and : base(...))
    bool found_ctor_this_call = false;
    bool found_ctor_base_call = false;
    bool found_base_arg_cfg = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::call) {
            if (occ.written_name == "this" &&
                occ.enclosing_scope == "Advanced.Features.Complex.Complex") {
                found_ctor_this_call = true;
            }
            if (occ.written_name == "base" &&
                occ.enclosing_scope == "Advanced.Features.AdvancedService.AdvancedService") {
                found_ctor_base_call = true;
            }
        }
        if (occ.kind == worker::FactKind::reference && occ.written_name == "cfg" &&
            occ.enclosing_scope == "Advanced.Features.AdvancedService.AdvancedService") {
            found_base_arg_cfg = true;
        }
    }
    REQUIRE(found_ctor_this_call);
    REQUIRE(found_ctor_base_call);
    REQUIRE(found_base_arg_cfg);

    // 6. Attribute arguments: [CustomAttr(typeof(Complex), 42, Tag = "production")]
    bool found_custom_attr = false;
    bool found_typeof_complex = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::reference) {
            if (occ.written_name == "CustomAttr") {
                found_custom_attr = true;
                CHECK(std::ranges::find(occ.candidate_targets, "CustomAttrAttribute") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "Complex" &&
                occ.enclosing_scope == "Advanced.Features.AdvancedService.Calculate") {
                found_typeof_complex = true;
            }
        }
    }
    REQUIRE(found_custom_attr);
    REQUIRE(found_typeof_complex);

    // 7. Method return type walked: Complex Calculate(...)
    bool found_calc_ret_type = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::reference && occ.written_name == "Complex" &&
            occ.enclosing_scope == "Advanced.Features.AdvancedService") {
            found_calc_ret_type = true;
        }
    }
    REQUIRE(found_calc_ret_type);

    // 8. Events
    bool found_simple_event = false;
    bool found_custom_event = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "OnSimpleEvent" &&
            sym.enclosing_scope == "Advanced.Features.AdvancedService") {
            found_simple_event = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "OnCustomEvent" &&
            sym.enclosing_scope == "Advanced.Features.AdvancedService") {
            found_custom_event = true;
            CHECK(sym.kind == NodeKind::field);
        }
    }
    REQUIRE(found_simple_event);
    REQUIRE(found_custom_event);

    // Accessor body scoping for OnCustomEvent
    bool found_accessor_scope = false;
    for (const auto& occ : result.occurrences) {
        if (occ.enclosing_scope.has_value() &&
            occ.enclosing_scope->starts_with("Advanced.Features.AdvancedService.OnCustomEvent")) {
            found_accessor_scope = true;
            break;
        }
    }
    REQUIRE(found_accessor_scope);

    // 9. Conditional access call and element access
    bool found_cond_call = false;
    bool found_cond_svc_ref = false;
    bool found_cond_elem_numbers = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::call && occ.written_name == "svc?.Calculate") {
            found_cond_call = true;
            CHECK(std::ranges::find(occ.candidate_targets, "Calculate") !=
                  occ.candidate_targets.end());
            std::string_view occ_span =
                source.substr(occ.range.start, occ.range.end - occ.range.start);
            CHECK(occ_span == "svc?.Calculate");
        }
        if (occ.kind == worker::FactKind::reference && occ.written_name == "svc") {
            found_cond_svc_ref = true;
        }
        if (occ.kind == worker::FactKind::reference && occ.written_name == "numbers") {
            found_cond_elem_numbers = true;
        }
    }
    REQUIRE(found_cond_call);
    REQUIRE(found_cond_svc_ref);
    REQUIRE(found_cond_elem_numbers);
}
