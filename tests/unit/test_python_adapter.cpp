#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "codelenses/adapters/adapter.hpp"
#include "codelenses/adapters/python_adapter.hpp"
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

TEST_CASE("H4-01: Python adapter grammar registration and file extensions",
          "[adapter][python][h4]") {
    // 1. Grammar registration
    const auto* ts_lang = treesitter::grammar_for_language(Language::python);
    REQUIRE(ts_lang != nullptr);
    REQUIRE(treesitter::has_grammar_for_language(Language::python));
    REQUIRE(treesitter::grammar_version(Language::python) == "0.23.6");

    // 2. File extension mappings
    REQUIRE(language_from_extension(".py") == Language::python);
    REQUIRE(language_from_extension("py") == Language::python);
    REQUIRE(language_from_extension(".pyi") == Language::python);
    REQUIRE(language_from_extension("pyi") == Language::python);

    // 3. Path detection
    REQUIRE(language_from_path("src/main.py") == Language::python);
    REQUIRE(language_from_path("stubs/typing.pyi") == Language::python);

    // 4. String conversion
    auto parsed_py = language_from_string("python");
    REQUIRE(parsed_py.has_value());
    REQUIRE(parsed_py.value() == Language::python);

    auto parsed_py_short = language_from_string("py");
    REQUIRE(parsed_py_short.has_value());
    REQUIRE(parsed_py_short.value() == Language::python);

    REQUIRE(to_string(Language::python) == "Python");

    // 5. Registry integration
    auto& registry = default_adapter_registry();
    REQUIRE(registry.has_adapter(Language::python));

    auto* adapter_by_lang = registry.get_adapter(Language::python);
    REQUIRE(adapter_by_lang != nullptr);
    REQUIRE(adapter_by_lang->language() == Language::python);
    REQUIRE(adapter_by_lang->name() == "PythonAdapter");

    auto* adapter_by_path = registry.get_adapter_for_path("src/script.py");
    REQUIRE(adapter_by_path != nullptr);
    REQUIRE(adapter_by_path->language() == Language::python);

    // 6. Capabilities check
    const auto& caps = adapter_by_lang->capabilities();
    REQUIRE(caps.functions == CapabilityStatus::supported);
    REQUIRE(caps.methods == CapabilityStatus::supported);
    REQUIRE(caps.classes == CapabilityStatus::supported);
    REQUIRE(caps.structs == CapabilityStatus::unavailable);
    REQUIRE(caps.interfaces == CapabilityStatus::unavailable);
    REQUIRE(caps.enums == CapabilityStatus::unavailable);
    REQUIRE(caps.records == CapabilityStatus::unavailable);
    REQUIRE(caps.namespaces == CapabilityStatus::unavailable);
    REQUIRE(caps.variables == CapabilityStatus::supported);
    REQUIRE(caps.modules == CapabilityStatus::supported);
    REQUIRE(caps.packages == CapabilityStatus::unavailable);
    REQUIRE(caps.templates == CapabilityStatus::unavailable);
    REQUIRE(caps.partial_types == CapabilityStatus::unavailable);
    REQUIRE(caps.containment == CapabilityStatus::supported);
    REQUIRE(caps.calls == CapabilityStatus::supported);
    REQUIRE(caps.references == CapabilityStatus::supported);
    REQUIRE(caps.inheritance == CapabilityStatus::supported);
    REQUIRE(caps.implementation == CapabilityStatus::unavailable);
    REQUIRE(caps.imports == CapabilityStatus::supported);
    REQUIRE(caps.includes == CapabilityStatus::unavailable);
}

TEST_CASE("H4-01: Python adapter extraction of modules, imports, aliases, classes, functions, "
          "methods, decorators",
          "[adapter][python][h4]") {
    PythonAdapter adapter;

    std::string_view source = R"(
import math
import numpy as np
from os.path import join as path_join
from typing import Optional, List

MAX_ITEMS: int = 100
DEFAULT_NAME = "default"

def standalone_func(count: int) -> int:
    return count * 2

@dataclass
class Animal:
    species: str = "unknown"

    def __init__(self, name: str):
        self.name = name

    @property
    def display_name(self) -> str:
        return self.name

class Dog(Animal):
    def __init__(self, name: str, breed: str):
        super().__init__(name)
        self.breed = breed

    def speak(self) -> str:
        return "Woof"

def create_dog(name: str, breed: str = "mutt") -> Dog:
    d = Dog(name, breed)
    return d
)";

    auto res = adapter.parse(source, "src/pets.py");
    REQUIRE(res.has_value());
    const AdapterResult& result = res.value();

    REQUIRE(result.language == Language::python);
    REQUIRE(result.status == worker::CompletionStatus::complete);
    REQUIRE(result.diagnostics.empty());

    // 1. Verify Module Symbol
    bool found_module = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "pets" && sym.kind == NodeKind::module) {
            found_module = true;
            CHECK(sym.qualified_name == "pets");
            CHECK(sym.signature == "module pets");
        }
    }
    REQUIRE(found_module);

    bool found_module_decl = false;
    for (const auto& decl : result.declarations) {
        if (decl.symbol_name == "pets" && decl.kind == NodeKind::module) {
            found_module_decl = true;
            CHECK(decl.is_definition);
        }
    }
    REQUIRE(found_module_decl);

    // 2. Verify Imports
    bool found_math = false;
    bool found_numpy = false;
    bool found_os_path_join = false;
    bool found_optional = false;
    bool found_list = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::import) {
            if (occ.written_name == "math") {
                found_math = true;
            }
            if (occ.written_name == "numpy") {
                found_numpy = true;
            }
            if (occ.written_name == "join") {
                found_os_path_join = true;
                CHECK(std::ranges::find(occ.candidate_targets, "os.path.join") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "Optional") {
                found_optional = true;
            }
            if (occ.written_name == "List") {
                found_list = true;
            }
        }
    }
    REQUIRE(found_math);
    REQUIRE(found_numpy);
    REQUIRE(found_os_path_join);
    REQUIRE(found_optional);
    REQUIRE(found_list);

    // 3. Verify Import Aliases
    bool found_np_alias = false;
    bool found_path_join_alias = false;

    for (const auto& sym : result.symbols) {
        if (sym.name == "np" && sym.kind == NodeKind::type_alias) {
            found_np_alias = true;
            CHECK(sym.signature == "import numpy as np");
        }
        if (sym.name == "path_join" && sym.kind == NodeKind::type_alias) {
            found_path_join_alias = true;
            CHECK(sym.signature == "from os.path import join as path_join");
        }
    }
    REQUIRE(found_np_alias);
    REQUIRE(found_path_join_alias);

    // 4. Verify Variables
    bool found_max_items = false;
    bool found_default_name = false;
    bool found_species_var = false;

    for (const auto& sym : result.symbols) {
        if (sym.name == "MAX_ITEMS" && sym.kind == NodeKind::variable) {
            found_max_items = true;
        }
        if (sym.name == "DEFAULT_NAME" && sym.kind == NodeKind::variable) {
            found_default_name = true;
        }
        if (sym.name == "species" && sym.kind == NodeKind::variable) {
            found_species_var = true;
            CHECK(sym.enclosing_scope == "Animal");
        }
    }
    REQUIRE(found_max_items);
    REQUIRE(found_default_name);
    REQUIRE(found_species_var);

    // 5. Verify Classes
    bool found_animal_cls = false;
    bool found_dog_cls = false;

    for (const auto& sym : result.symbols) {
        if (sym.name == "Animal" && sym.kind == NodeKind::class_) {
            found_animal_cls = true;
            CHECK(sym.qualified_name == "Animal");
        }
        if (sym.name == "Dog" && sym.kind == NodeKind::class_) {
            found_dog_cls = true;
            CHECK(sym.qualified_name == "Dog");
        }
    }
    REQUIRE(found_animal_cls);
    REQUIRE(found_dog_cls);

    // 6. Verify Inheritance
    bool found_dog_inherits_animal = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::inheritance) {
            if (occ.written_name == "Animal" && occ.enclosing_scope == "Dog") {
                found_dog_inherits_animal = true;
                CHECK(std::ranges::find(occ.candidate_targets, "Animal") !=
                      occ.candidate_targets.end());
            }
        }
    }
    REQUIRE(found_dog_inherits_animal);

    // 7. Verify Functions and Methods
    bool found_standalone = false;
    bool found_create_dog = false;
    bool found_animal_init = false;
    bool found_animal_display = false;
    bool found_dog_init = false;
    bool found_dog_speak = false;

    for (const auto& sym : result.symbols) {
        if (sym.name == "standalone_func" && sym.kind == NodeKind::function) {
            found_standalone = true;
            CHECK_FALSE(sym.enclosing_scope.has_value());
        }
        if (sym.name == "create_dog" && sym.kind == NodeKind::function) {
            found_create_dog = true;
            CHECK(sym.signature == "def create_dog(name: str, breed: str = \"mutt\") -> Dog");
        }
        if (sym.name == "__init__" && sym.enclosing_scope == "Animal") {
            found_animal_init = true;
            CHECK(sym.kind == NodeKind::method);
            CHECK(sym.qualified_name == "Animal.__init__");
        }
        if (sym.name == "display_name" && sym.enclosing_scope == "Animal") {
            found_animal_display = true;
            CHECK(sym.kind == NodeKind::method);
        }
        if (sym.name == "__init__" && sym.enclosing_scope == "Dog") {
            found_dog_init = true;
            CHECK(sym.kind == NodeKind::method);
            CHECK(sym.qualified_name == "Dog.__init__");
        }
        if (sym.name == "speak" && sym.enclosing_scope == "Dog") {
            found_dog_speak = true;
            CHECK(sym.kind == NodeKind::method);
        }
    }
    REQUIRE(found_standalone);
    REQUIRE(found_create_dog);
    REQUIRE(found_animal_init);
    REQUIRE(found_animal_display);
    REQUIRE(found_dog_init);
    REQUIRE(found_dog_speak);

    // 8. Verify Decorators
    bool found_dataclass_dec = false;
    bool found_property_dec = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::reference) {
            if (occ.written_name == "dataclass") {
                found_dataclass_dec = true;
                CHECK(occ.metadata_json.has_value());
                CHECK(occ.metadata_json->find("is_decorator") != std::string::npos);
            }
            if (occ.written_name == "property") {
                found_property_dec = true;
                CHECK(occ.metadata_json.has_value());
                CHECK(occ.metadata_json->find("is_decorator") != std::string::npos);
            }
        }
    }
    REQUIRE(found_dataclass_dec);
    REQUIRE(found_property_dec);
}

TEST_CASE(
    "H4-02: Python adapter extraction of calls and attribute references with confidence metadata",
    "[adapter][python][h4]") {
    PythonAdapter adapter;

    std::string_view source = R"(
class Base:
    def __init__(self):
        pass

class Service(Base):
    def __init__(self, name: str):
        super().__init__()
        self.name = name
        self.counter = 0

    @classmethod
    def create(cls, name: str):
        return cls.build(name)

    def increment(self) -> int:
        self.counter += 1
        return self.get_count()

    def get_count(self) -> int:
        return self.counter

def process(service, client, event):
    service.increment()
    result = client.dispatch(event)
    data = event.payload.body
    print("finished")
    return result
)";

    auto res = adapter.parse(source, "src/service.py");
    REQUIRE(res.has_value());
    const AdapterResult& result = res.value();
    REQUIRE(result.status == worker::CompletionStatus::complete);

    // 1. Verify Calls
    bool found_self_get_count_call = false;
    bool found_service_increment_call = false;
    bool found_client_dispatch_call = false;
    bool found_print_call = false;
    bool found_cls_build_call = false;
    bool found_super_init_call = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::call) {
            if (occ.written_name == "self.get_count") {
                found_self_get_count_call = true;
                CHECK(occ.confidence == 1.0);
                CHECK(occ.metadata_json.has_value());
                CHECK(occ.metadata_json->find("is_method") != std::string::npos);
                CHECK(occ.metadata_json->find(R"("receiver":"self")") != std::string::npos);
                CHECK(std::ranges::find(occ.candidate_targets, "Service.get_count") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "cls.build") {
                found_cls_build_call = true;
                CHECK(occ.confidence == 1.0);
                CHECK(occ.metadata_json.has_value());
                CHECK(occ.metadata_json->find(R"("receiver":"cls")") != std::string::npos);
                CHECK(std::ranges::find(occ.candidate_targets, "Service.build") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "super().__init__") {
                found_super_init_call = true;
                CHECK(occ.confidence == 0.9);
                CHECK(occ.metadata_json.has_value());
                CHECK(occ.metadata_json->find(R"("receiver":"super")") != std::string::npos);
            }
            if (occ.written_name == "service.increment") {
                found_service_increment_call = true;
                CHECK(occ.confidence == 0.5);
                CHECK(occ.metadata_json.has_value());
                CHECK(occ.metadata_json->find("dynamic") != std::string::npos);
                CHECK(occ.metadata_json->find("service") != std::string::npos);
                CHECK(std::ranges::find(occ.candidate_targets, "increment") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "client.dispatch") {
                found_client_dispatch_call = true;
                CHECK(occ.confidence == 0.5);
                CHECK(occ.metadata_json.has_value());
                CHECK(occ.metadata_json->find("client") != std::string::npos);
            }
            if (occ.written_name == "print") {
                found_print_call = true;
                CHECK(occ.confidence == 1.0);
            }
        }
    }

    REQUIRE(found_self_get_count_call);
    REQUIRE(found_cls_build_call);
    REQUIRE(found_super_init_call);
    REQUIRE(found_service_increment_call);
    REQUIRE(found_client_dispatch_call);
    REQUIRE(found_print_call);

    // 2. Verify Attribute References
    bool found_self_counter_ref = false;
    bool found_event_payload_ref = false;
    bool found_intermediate_payload_ref = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::reference) {
            if (occ.written_name == "self.counter") {
                found_self_counter_ref = true;
                CHECK(occ.confidence == 0.9);
                CHECK(occ.metadata_json.has_value());
                CHECK(occ.metadata_json->find("is_attribute") != std::string::npos);
                CHECK(occ.metadata_json->find(R"("receiver":"self")") != std::string::npos);
                CHECK(std::ranges::find(occ.candidate_targets, "Service.counter") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "event.payload.body") {
                found_event_payload_ref = true;
                CHECK(occ.confidence == 0.5);
                CHECK(occ.metadata_json.has_value());
                CHECK(occ.metadata_json->find("dynamic") != std::string::npos);
                CHECK(std::ranges::find(occ.candidate_targets, "body") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "event.payload") {
                found_intermediate_payload_ref = true;
            }
        }
    }

    REQUIRE(found_self_counter_ref);
    REQUIRE(found_event_payload_ref);
    CHECK_FALSE(found_intermediate_payload_ref);
}

TEST_CASE("H4-01: Python future imports and __init__.py package name", "[adapter][python][h4]") {
    PythonAdapter adapter;

    SECTION("from __future__ import annotations is parsed as import fact") {
        std::string_view src = "from __future__ import annotations, division\n";
        auto res = adapter.parse(src, "src/mod.py");
        REQUIRE(res.has_value());
        bool found_annotations = false;
        bool found_division = false;
        for (const auto& occ : res->occurrences) {
            if (occ.kind == worker::FactKind::import) {
                if (occ.written_name == "annotations") {
                    found_annotations = true;
                    CHECK(std::ranges::find(occ.candidate_targets, "__future__.annotations") !=
                          occ.candidate_targets.end());
                }
                if (occ.written_name == "division") {
                    found_division = true;
                    CHECK(std::ranges::find(occ.candidate_targets, "__future__.division") !=
                          occ.candidate_targets.end());
                }
            }
        }
        REQUIRE(found_annotations);
        REQUIRE(found_division);
    }

    SECTION("__init__.py resolves package name from parent directory") {
        std::string_view src = "# package init\n";
        auto res = adapter.parse(src, "src/mypackage/__init__.py");
        REQUIRE(res.has_value());
        bool found_pkg = false;
        for (const auto& sym : res->symbols) {
            if (sym.kind == NodeKind::module && sym.name == "mypackage") {
                found_pkg = true;
                CHECK(sym.signature == "module mypackage");
            }
        }
        REQUIRE(found_pkg);
    }
}

TEST_CASE("H4-02: Python adapter syntax highlighting query and token generation",
          "[adapter][python][h4][highlight]") {
    PythonAdapter adapter;

    std::string_view source = R"(
import os
from math import sqrt

# This is a comment
@property
def compute_area(radius: float) -> float:
    """Docstring."""
    if radius <= 0:
        return 0.0
    return 3.14159 * radius * radius

is_valid = True
flag = False
nothing = None

class Shape:
    name = "Shape"

    def area(self):
        return self.name
)";

    auto res = adapter.highlight(source);
    if (!res) {
        FAIL_CHECK("Highlight error: " << res.error().message);
    }
    REQUIRE(res.has_value());
    const auto& tokens = res.value();
    REQUIRE_FALSE(tokens.empty());

    const auto& legend = HighlightLegend::default_legend();
    auto kw_opt = legend.token_type_index("keyword");
    auto func_opt = legend.token_type_index("function");
    auto class_opt = legend.token_type_index("class");
    auto dec_opt = legend.token_type_index("decorator");
    auto str_opt = legend.token_type_index("string");
    auto num_opt = legend.token_type_index("number");
    auto comment_opt = legend.token_type_index("comment");
    auto var_opt = legend.token_type_index("variable");

    REQUIRE(kw_opt.has_value());
    REQUIRE(func_opt.has_value());
    REQUIRE(class_opt.has_value());
    REQUIRE(dec_opt.has_value());
    REQUIRE(str_opt.has_value());
    REQUIRE(num_opt.has_value());
    REQUIRE(comment_opt.has_value());
    REQUIRE(var_opt.has_value());

    uint32_t kw_idx = 0;
    uint32_t func_idx = 0;
    uint32_t class_idx = 0;
    uint32_t dec_idx = 0;
    uint32_t str_idx = 0;
    uint32_t num_idx = 0;
    uint32_t comment_idx = 0;
    uint32_t var_idx = 0;

    if (kw_opt && func_opt && class_opt && dec_opt && str_opt && num_opt && comment_opt &&
        var_opt) {
        kw_idx = kw_opt.value();
        func_idx = func_opt.value();
        class_idx = class_opt.value();
        dec_idx = dec_opt.value();
        str_idx = str_opt.value();
        num_idx = num_opt.value();
        comment_idx = comment_opt.value();
        var_idx = var_opt.value();
    }

    bool found_def_kw = false;
    bool found_class_kw = false;
    bool found_return_kw = false;
    bool found_true_kw = false;
    bool found_false_kw = false;
    bool found_none_kw = false;
    bool found_compute_func = false;
    bool found_shape_class = false;
    bool found_prop_dec = false;
    bool found_comment = false;
    bool found_num = false;
    bool found_str = false;
    bool found_self_var = false;

    for (const auto& tok : tokens) {
        std::string_view text = source.substr(tok.byte_range.start, tok.length);
        if (tok.token_type == kw_idx) {
            if (text == "def") {
                found_def_kw = true;
            }
            if (text == "class") {
                found_class_kw = true;
            }
            if (text == "return") {
                found_return_kw = true;
            }
            if (text == "True") {
                found_true_kw = true;
            }
            if (text == "False") {
                found_false_kw = true;
            }
            if (text == "None") {
                found_none_kw = true;
            }
        }
        if (tok.token_type == func_idx && text == "compute_area") {
            found_compute_func = true;
        }
        if (tok.token_type == class_idx && text == "Shape") {
            found_shape_class = true;
        }
        if (tok.token_type == dec_idx && text == "property") {
            found_prop_dec = true;
        }
        if (tok.token_type == comment_idx && text.starts_with("#")) {
            found_comment = true;
        }
        if (tok.token_type == num_idx && text == "3.14159") {
            found_num = true;
        }
        if (tok.token_type == str_idx && text == "\"Shape\"") {
            found_str = true;
        }
        if (tok.token_type == var_idx && text == "self") {
            found_self_var = true;
        }
    }

    CHECK(found_def_kw);
    CHECK(found_class_kw);
    CHECK(found_return_kw);
    CHECK(found_true_kw);
    CHECK(found_false_kw);
    CHECK(found_none_kw);
    CHECK(found_compute_func);
    CHECK(found_shape_class);
    CHECK(found_prop_dec);
    CHECK(found_comment);
    CHECK(found_num);
    CHECK(found_str);
    CHECK(found_self_var);
}

TEST_CASE("H4-03: Python adapter fixtures for relative imports, aliases, nested scopes, and "
          "dynamic cases",
          "[adapter][python][h4][fixtures]") {
    PythonAdapter adapter;

    auto find_fixture_dir = []() -> std::filesystem::path {
#ifdef CODELENSES_SOURCE_DIR
        std::filesystem::path p =
            std::filesystem::path(CODELENSES_SOURCE_DIR) / "tests/fixtures/python";
        if (std::filesystem::exists(p)) {
            return p;
        }
#endif
        for (const auto& candidate : {std::filesystem::path("tests/fixtures/python"),
                                      std::filesystem::path("../../tests/fixtures/python"),
                                      std::filesystem::path("../tests/fixtures/python")}) {
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
        return "tests/fixtures/python";
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
        const AdapterResult& result = parse_res.value();
        REQUIRE(result.language == Language::python);
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

    SECTION("Relative imports fixture") {
        test_fixture("relative_imports", ".py");
    }

    SECTION("Aliases fixture") {
        test_fixture("aliases", ".py");
    }

    SECTION("Nested scopes fixture") {
        test_fixture("nested_scopes", ".py");
    }

    SECTION("Dynamic cases fixture") {
        test_fixture("dynamic_cases", ".py");
    }
}

TEST_CASE("H4-03: Python adapter error handling, cancellation, and edge cases",
          "[adapter][python][h4]") {
    PythonAdapter adapter;

    SECTION("Parse error produces diagnostics and degraded status") {
        std::string_view bad_source = R"(
def broken_fn(
    x = 10
    print("unclosed"
)";
        auto res = adapter.parse(bad_source, "bad.py");
        REQUIRE(res.has_value());
        REQUIRE_FALSE(res->diagnostics.empty());
        REQUIRE(res->status == worker::CompletionStatus::degraded);
        REQUIRE(res->diagnostics[0].severity == DiagnosticSeverity::error);
    }

    SECTION("Cooperative cancellation via stop_token") {
        std::stop_source stop_src;
        stop_src.request_stop();

        std::string_view src = "x = 42\ndef f(): pass\n";
        auto res = adapter.parse(src, "cancel.py", stop_src.get_token());
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().code == ErrorCode::cancelled);
    }

    SECTION("Async functions and await expressions") {
        std::string_view async_source = R"(
async def fetch_page(url: str) -> str:
    response = await client.get(url)
    return response.text
)";
        auto res = adapter.parse(async_source, "async_test.py");
        REQUIRE(res.has_value());
        REQUIRE(res->status == worker::CompletionStatus::complete);

        bool found_async_fn = false;
        for (const auto& sym : res->symbols) {
            if (sym.name == "fetch_page" && sym.kind == NodeKind::function) {
                found_async_fn = true;
            }
        }
        REQUIRE(found_async_fn);
    }

    SECTION("Walrus operator assignment") {
        std::string_view walrus_source = R"(
data = [1, 2, 3]
if (n := len(data)) > 0:
    print(n)
)";
        auto res = adapter.parse(walrus_source, "walrus.py");
        REQUIRE(res.has_value());
        REQUIRE(res->status == worker::CompletionStatus::complete);
    }
}
