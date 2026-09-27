#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "codelenses/adapters/adapter.hpp"
#include "codelenses/adapters/java_adapter.hpp"
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

TEST_CASE("H8-01: Java adapter grammar registration and file extensions", "[adapter][java][h8]") {
    // 1. Grammar registration
    const auto* ts_lang = treesitter::grammar_for_language(Language::java);
    REQUIRE(ts_lang != nullptr);
    REQUIRE(treesitter::has_grammar_for_language(Language::java));
    REQUIRE(treesitter::grammar_version(Language::java) == "0.23.5");

    // 2. File extension mappings
    REQUIRE(language_from_extension(".java") == Language::java);
    REQUIRE(language_from_extension("java") == Language::java);

    // 3. Path detection
    REQUIRE(language_from_path("src/com/example/Main.java") == Language::java);
    REQUIRE(language_from_path("billing/Invoice.java") == Language::java);

    // 4. String conversion
    auto parsed_java = language_from_string("java");
    REQUIRE(parsed_java.has_value());
    REQUIRE(*parsed_java == Language::java);
    REQUIRE(to_string(Language::java) == "Java");

    // 5. Registry integration
    auto& registry = default_adapter_registry();
    REQUIRE(registry.has_adapter(Language::java));

    auto* adapter_by_lang = registry.get_adapter(Language::java);
    REQUIRE(adapter_by_lang != nullptr);
    REQUIRE(adapter_by_lang->language() == Language::java);
    REQUIRE(adapter_by_lang->name() == "JavaAdapter");

    auto* adapter_by_path = registry.get_adapter_for_path("src/App.java");
    REQUIRE(adapter_by_path != nullptr);
    REQUIRE(adapter_by_path->language() == Language::java);

    // 6. Capabilities check
    const auto& caps = adapter_by_lang->capabilities();
    REQUIRE(caps.methods == CapabilityStatus::supported);
    REQUIRE(caps.classes == CapabilityStatus::supported);
    REQUIRE(caps.interfaces == CapabilityStatus::supported);
    REQUIRE(caps.enums == CapabilityStatus::supported);
    REQUIRE(caps.records == CapabilityStatus::supported);
    REQUIRE(caps.packages == CapabilityStatus::supported);
    REQUIRE(caps.templates == CapabilityStatus::supported);
    REQUIRE(caps.containment == CapabilityStatus::supported);
    REQUIRE(caps.calls == CapabilityStatus::supported);
    REQUIRE(caps.references == CapabilityStatus::supported);
    REQUIRE(caps.inheritance == CapabilityStatus::supported);
    REQUIRE(caps.implementation == CapabilityStatus::supported);
    REQUIRE(caps.imports == CapabilityStatus::supported);
    REQUIRE(caps.variables == CapabilityStatus::supported);

    REQUIRE(caps.functions == CapabilityStatus::unavailable);
    REQUIRE(caps.structs == CapabilityStatus::unavailable);
    REQUIRE(caps.namespaces == CapabilityStatus::unavailable);
    REQUIRE(caps.modules == CapabilityStatus::unavailable);
    REQUIRE(caps.includes == CapabilityStatus::unavailable);
    REQUIRE(caps.partial_types == CapabilityStatus::unavailable);

    REQUIRE(caps.override_analysis == CapabilityStatus::deferred);
    REQUIRE(caps.instantiation_analysis == CapabilityStatus::deferred);
}

TEST_CASE("H8-01: Java adapter extraction of packages, imports, classes, interfaces, enums, "
          "fields, methods",
          "[adapter][java][h8]") {
    JavaAdapter adapter;

    std::string_view source = R"(
package com.example.billing;

import java.util.ArrayList;
import java.util.List;
import static java.lang.Math.PI;
import com.example.model.*;

public enum Priority {
    LOW,
    MEDIUM,
    HIGH
}

public interface Billable {
    String getReference();
    double calculateTotal();
}

public record Receipt(String transactionId, double amount) {}

public class Invoice implements Billable {
    private final String reference;
    private double taxRate;
    private int itemCount, pageCount;

    public Invoice(String reference, double taxRate) {
        this.reference = reference;
        this.taxRate = taxRate;
    }

    @Override
    public String getReference() {
        return this.reference;
    }

    @Override
    public double calculateTotal() {
        return 100.0 * (1.0 + this.taxRate);
    }

    public void process(int factor) {
        double result = calculateTotal();
    }
}
)";

    auto res = adapter.parse(source, "src/com/example/billing/Invoice.java");
    REQUIRE(res.has_value());
    const AdapterResult& result = *res;

    REQUIRE(result.language == Language::java);
    REQUIRE(result.status == worker::CompletionStatus::complete);
    REQUIRE(result.diagnostics.empty());

    // 1. Verify Package
    bool found_package_sym = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "com.example.billing" && sym.kind == NodeKind::package) {
            found_package_sym = true;
            CHECK(sym.qualified_name == "com.example.billing");
            CHECK(sym.signature == "package com.example.billing");
        }
    }
    REQUIRE(found_package_sym);

    bool found_package_decl = false;
    for (const auto& decl : result.declarations) {
        if (decl.symbol_name == "com.example.billing" && decl.kind == NodeKind::package) {
            found_package_decl = true;
            CHECK(decl.is_definition);
        }
    }
    REQUIRE(found_package_decl);

    // 2. Verify Imports
    bool found_array_list_import = false;
    bool found_list_import = false;
    bool found_static_pi_import = false;
    bool found_wildcard_model_import = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::import) {
            if (occ.written_name == "java.util.ArrayList") {
                found_array_list_import = true;
                CHECK(std::ranges::find(occ.candidate_targets, "ArrayList") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "java.util.List") {
                found_list_import = true;
                CHECK(std::ranges::find(occ.candidate_targets, "List") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "java.lang.Math.PI") {
                found_static_pi_import = true;
                CHECK(std::ranges::find(occ.candidate_targets, "PI") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "com.example.model.*") {
                found_wildcard_model_import = true;
                CHECK(std::ranges::find(occ.candidate_targets, "com.example.model") !=
                      occ.candidate_targets.end());
            }
        }
    }
    REQUIRE(found_array_list_import);
    REQUIRE(found_list_import);
    REQUIRE(found_static_pi_import);
    REQUIRE(found_wildcard_model_import);

    // 3. Verify Enum and Members
    bool found_enum = false;
    bool found_low = false;
    bool found_medium = false;
    bool found_high = false;

    for (const auto& sym : result.symbols) {
        if (sym.name == "Priority" && sym.kind == NodeKind::enum_) {
            found_enum = true;
            CHECK(sym.qualified_name == "com.example.billing.Priority");
        }
        if (sym.name == "LOW" && sym.kind == NodeKind::enum_member) {
            found_low = true;
            CHECK(sym.qualified_name == "com.example.billing.Priority.LOW");
            CHECK(sym.enclosing_scope == "com.example.billing.Priority");
        }
        if (sym.name == "MEDIUM" && sym.kind == NodeKind::enum_member) {
            found_medium = true;
            CHECK(sym.qualified_name == "com.example.billing.Priority.MEDIUM");
        }
        if (sym.name == "HIGH" && sym.kind == NodeKind::enum_member) {
            found_high = true;
            CHECK(sym.qualified_name == "com.example.billing.Priority.HIGH");
        }
    }
    REQUIRE(found_enum);
    REQUIRE(found_low);
    REQUIRE(found_medium);
    REQUIRE(found_high);

    // 4. Verify Interface and Abstract Methods
    bool found_iface = false;
    bool found_iface_ref_m = false;
    bool found_iface_calc_m = false;

    for (const auto& sym : result.symbols) {
        if (sym.name == "Billable" && sym.kind == NodeKind::interface_) {
            found_iface = true;
            CHECK(sym.qualified_name == "com.example.billing.Billable");
        }
        if (sym.name == "getReference" && sym.enclosing_scope == "com.example.billing.Billable") {
            found_iface_ref_m = true;
            CHECK(sym.kind == NodeKind::method);
        }
        if (sym.name == "calculateTotal" && sym.enclosing_scope == "com.example.billing.Billable") {
            found_iface_calc_m = true;
            CHECK(sym.kind == NodeKind::method);
        }
    }
    REQUIRE(found_iface);
    REQUIRE(found_iface_ref_m);
    REQUIRE(found_iface_calc_m);

    // Interface method declarations should have is_definition = false
    for (const auto& decl : result.declarations) {
        if (decl.symbol_name == "getReference" &&
            decl.enclosing_scope == "com.example.billing.Billable") {
            CHECK_FALSE(decl.is_definition);
        }
    }

    // 5. Verify Record and Record Fields
    bool found_record = false;
    bool found_rec_tx = false;
    bool found_rec_amount = false;

    for (const auto& sym : result.symbols) {
        if (sym.name == "Receipt" && sym.kind == NodeKind::class_) {
            found_record = true;
            CHECK(sym.qualified_name == "com.example.billing.Receipt");
        }
        if (sym.name == "transactionId" && sym.enclosing_scope == "com.example.billing.Receipt") {
            found_rec_tx = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "amount" && sym.enclosing_scope == "com.example.billing.Receipt") {
            found_rec_amount = true;
            CHECK(sym.kind == NodeKind::field);
        }
    }
    REQUIRE(found_record);
    REQUIRE(found_rec_tx);
    REQUIRE(found_rec_amount);

    // 6. Verify Class, Constructor, Fields, and Methods
    bool found_invoice_class = false;
    bool found_invoice_ctor = false;
    bool found_ref_field = false;
    bool found_tax_field = false;
    bool found_item_count_field = false;
    bool found_page_count_field = false;
    bool found_inv_get_ref = false;
    bool found_inv_calc_total = false;
    bool found_inv_process = false;

    for (const auto& sym : result.symbols) {
        if (sym.name == "Invoice" && sym.kind == NodeKind::class_) {
            found_invoice_class = true;
            CHECK(sym.qualified_name == "com.example.billing.Invoice");
        }
        if (sym.name == "Invoice" && sym.kind == NodeKind::method) {
            found_invoice_ctor = true;
            CHECK(sym.qualified_name == "com.example.billing.Invoice.Invoice");
            CHECK(sym.enclosing_scope == "com.example.billing.Invoice");
        }
        if (sym.name == "reference" && sym.enclosing_scope == "com.example.billing.Invoice") {
            found_ref_field = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "taxRate" && sym.enclosing_scope == "com.example.billing.Invoice") {
            found_tax_field = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "itemCount" && sym.enclosing_scope == "com.example.billing.Invoice") {
            found_item_count_field = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "pageCount" && sym.enclosing_scope == "com.example.billing.Invoice") {
            found_page_count_field = true;
            CHECK(sym.kind == NodeKind::field);
        }
        if (sym.name == "getReference" && sym.enclosing_scope == "com.example.billing.Invoice") {
            found_inv_get_ref = true;
            CHECK(sym.kind == NodeKind::method);
        }
        if (sym.name == "calculateTotal" && sym.enclosing_scope == "com.example.billing.Invoice") {
            found_inv_calc_total = true;
            CHECK(sym.kind == NodeKind::method);
        }
        if (sym.name == "process" && sym.enclosing_scope == "com.example.billing.Invoice") {
            found_inv_process = true;
            CHECK(sym.kind == NodeKind::method);
        }
    }
    REQUIRE(found_invoice_class);
    REQUIRE(found_invoice_ctor);
    REQUIRE(found_ref_field);
    REQUIRE(found_tax_field);
    REQUIRE(found_item_count_field);
    REQUIRE(found_page_count_field);
    REQUIRE(found_inv_get_ref);
    REQUIRE(found_inv_calc_total);
    REQUIRE(found_inv_process);

    // Class methods are definitions
    for (const auto& decl : result.declarations) {
        if (decl.symbol_name == "calculateTotal" &&
            decl.enclosing_scope == "com.example.billing.Invoice") {
            CHECK(decl.is_definition);
        }
    }
}

TEST_CASE("H8-02: Java adapter extraction of inheritance, overrides, overload metadata, and calls",
          "[adapter][java][h8]") {
    JavaAdapter adapter;

    std::string_view source = R"(
package com.example.zoo;

public class Animal {
    public void speak() {}
}

public interface IPet {
    void play();
}

public interface IAdoptable extends IPet {
    boolean isAdopted();
}

public class Dog extends Animal implements IAdoptable {
    private String tag;

    public Dog() {
        this("DefaultTag");
    }

    public Dog(String tag) {
        this.tag = tag;
    }

    @Override
    public void speak() {
        play();
    }

    @Override
    public void play() {}

    @Override
    public boolean isAdopted() {
        return false;
    }

    public int add(int a, int b) {
        return a + b;
    }

    public int add(int a, int b, int c) {
        return a + b + c;
    }

    public double add(double a, double b) {
        return a + b;
    }
}

public class Shelter {
    public void run() {
        Dog dog = new Dog();
        dog.speak();
        dog.play();
        int sum = dog.add(1, 2);
    }
}
)";

    auto res = adapter.parse(source, "src/com/example/zoo/Dog.java");
    REQUIRE(res.has_value());
    const AdapterResult& result = *res;
    REQUIRE(result.status == worker::CompletionStatus::complete);

    // 1. Verify Inheritance and Implementation Occurrences
    bool found_animal_base = false;
    bool found_pet_base_of_adoptable = false;
    bool found_adoptable_impl = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::inheritance) {
            if (occ.written_name == "Animal" && occ.enclosing_scope == "com.example.zoo.Dog") {
                found_animal_base = true;
                CHECK(std::ranges::find(occ.candidate_targets, "Animal") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "IPet" && occ.enclosing_scope == "com.example.zoo.IAdoptable") {
                found_pet_base_of_adoptable = true;
            }
        }
        if (occ.kind == worker::FactKind::implementation) {
            if (occ.written_name == "IAdoptable" && occ.enclosing_scope == "com.example.zoo.Dog") {
                found_adoptable_impl = true;
                CHECK(std::ranges::find(occ.candidate_targets, "IAdoptable") !=
                      occ.candidate_targets.end());
            }
        }
    }
    REQUIRE(found_animal_base);
    REQUIRE(found_pet_base_of_adoptable);
    REQUIRE(found_adoptable_impl);

    // 2. Verify Overrides and Annotations
    bool found_override_speak = false;
    bool found_override_play = false;
    bool found_override_is_adopted = false;

    for (const auto& occ : result.occurrences) {
        if (occ.written_name == "Override" && occ.kind == worker::FactKind::reference) {
            if (occ.enclosing_scope == "com.example.zoo.Dog.speak") {
                found_override_speak = true;
                CHECK(occ.metadata_json.has_value());
                CHECK(occ.metadata_json->find("is_override") != std::string::npos);
            }
            if (occ.enclosing_scope == "com.example.zoo.Dog.play") {
                found_override_play = true;
            }
            if (occ.enclosing_scope == "com.example.zoo.Dog.isAdopted") {
                found_override_is_adopted = true;
            }
        }
    }
    REQUIRE(found_override_speak);
    REQUIRE(found_override_play);
    REQUIRE(found_override_is_adopted);

    // 3. Verify Overload Metadata producing distinct symbol keys
    std::vector<std::string> add_keys;
    for (const auto& sym : result.symbols) {
        if (sym.name == "add" && sym.enclosing_scope == "com.example.zoo.Dog") {
            add_keys.push_back(
                resolver::generate_symbol_key(Language::java, "src/com/example/zoo/Dog.java",
                                              NodeKind::method, sym.qualified_name, sym.signature));
        }
    }
    REQUIRE(add_keys.size() == 3);
    std::unordered_set<std::string> unique_keys(add_keys.begin(), add_keys.end());
    CHECK(unique_keys.size() == 3);

    // 4. Verify Calls
    bool found_this_ctor_call = false;
    bool found_new_dog_call = false;
    bool found_dog_speak_call = false;
    bool found_dog_play_call = false;
    bool found_internal_play_call = false;
    bool found_dog_add_call = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::call) {
            if (occ.written_name == "this" && occ.enclosing_scope == "com.example.zoo.Dog.Dog") {
                found_this_ctor_call = true;
            }
            if (occ.written_name == "Dog") {
                found_new_dog_call = true;
                CHECK(std::ranges::find(occ.candidate_targets, "Dog") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "dog.speak") {
                found_dog_speak_call = true;
                CHECK(std::ranges::find(occ.candidate_targets, "speak") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "dog.play") {
                found_dog_play_call = true;
            }
            if (occ.written_name == "play" && occ.enclosing_scope == "com.example.zoo.Dog.speak") {
                found_internal_play_call = true;
            }
            if (occ.written_name == "dog.add") {
                found_dog_add_call = true;
                CHECK(std::ranges::find(occ.candidate_targets, "add") !=
                      occ.candidate_targets.end());
            }
        }
    }
    REQUIRE(found_this_ctor_call);
    REQUIRE(found_new_dog_call);
    REQUIRE(found_dog_speak_call);
    REQUIRE(found_dog_play_call);
    REQUIRE(found_internal_play_call);
    REQUIRE(found_dog_add_call);
}

TEST_CASE("H8-02: Java adapter syntax highlighting query and token generation",
          "[adapter][java][h8][highlight]") {
    JavaAdapter adapter;

    std::string_view source = R"(
package com.example.demo;

import java.util.List;

@Deprecated
public class Widget {
    private int value;

    public Widget(int value) {
        this.value = value;
    }

    @Override
    public String toString() {
        return "Widget: " + this.value;
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
    auto prop_opt = legend.token_type_index("property");
    auto str_opt = legend.token_type_index("string");

    REQUIRE(kw_opt.has_value());
    REQUIRE(type_opt.has_value());
    REQUIRE(class_opt.has_value());
    REQUIRE(method_opt.has_value());
    REQUIRE(dec_opt.has_value());
    REQUIRE(prop_opt.has_value());
    REQUIRE(str_opt.has_value());

    uint32_t kw_idx = *kw_opt;
    uint32_t type_idx = *type_opt;
    uint32_t class_idx = *class_opt;
    uint32_t method_idx = *method_opt;
    uint32_t dec_idx = *dec_opt;
    uint32_t prop_idx = *prop_opt;
    uint32_t str_idx = *str_opt;

    bool found_package_kw = false;
    bool found_public_kw = false;
    bool found_class_kw = false;
    bool found_int_type = false;
    bool found_widget_class = false;
    bool found_widget_ctor = false;
    bool found_to_string_method = false;
    bool found_deprecated_dec = false;
    bool found_override_dec = false;
    bool found_value_prop = false;
    bool found_str_lit = false;

    for (const auto& tok : tokens) {
        std::string_view text = source.substr(tok.byte_range.start, tok.length);
        if (tok.token_type == kw_idx) {
            if (text == "package")
                found_package_kw = true;
            if (text == "public")
                found_public_kw = true;
            if (text == "class")
                found_class_kw = true;
        }
        if (tok.token_type == type_idx && text == "int") {
            found_int_type = true;
        }
        if (tok.token_type == class_idx && text == "Widget") {
            found_widget_class = true;
        }
        if (tok.token_type == method_idx) {
            if (text == "Widget")
                found_widget_ctor = true;
            if (text == "toString")
                found_to_string_method = true;
        }
        if (tok.token_type == dec_idx) {
            if (text == "Deprecated")
                found_deprecated_dec = true;
            if (text == "Override")
                found_override_dec = true;
        }
        if (tok.token_type == prop_idx && text == "value") {
            found_value_prop = true;
        }
        if (tok.token_type == str_idx && text.find("Widget:") != std::string_view::npos) {
            found_str_lit = true;
        }
    }

    CHECK(found_package_kw);
    CHECK(found_public_kw);
    CHECK(found_class_kw);
    CHECK(found_int_type);
    CHECK(found_widget_class);
    CHECK(found_widget_ctor);
    CHECK(found_to_string_method);
    CHECK(found_deprecated_dec);
    CHECK(found_override_dec);
    CHECK(found_value_prop);
    CHECK(found_str_lit);
}

TEST_CASE("H8-03: Java adapter fixtures for nested classes and generics",
          "[adapter][java][h8][fixtures]") {
    JavaAdapter adapter;

    auto find_fixture_dir = []() -> std::filesystem::path {
#ifdef CODELENSES_SOURCE_DIR
        std::filesystem::path p =
            std::filesystem::path(CODELENSES_SOURCE_DIR) / "tests/fixtures/java";
        if (std::filesystem::exists(p)) {
            return p;
        }
#endif
        for (const auto& candidate : {std::filesystem::path("tests/fixtures/java"),
                                      std::filesystem::path("../../tests/fixtures/java"),
                                      std::filesystem::path("../tests/fixtures/java")}) {
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
        return "tests/fixtures/java";
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
        REQUIRE(result.language == Language::java);
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

    SECTION("Nested classes fixture") {
        test_fixture("nested_classes", ".java");
    }

    SECTION("Generics fixture") {
        test_fixture("generics", ".java");
    }
}

TEST_CASE("H8-03: Java error diagnostics, cancellation, and edge cases", "[adapter][java][h8]") {
    JavaAdapter adapter;

    SECTION("Malformed Java source produces diagnostics and degraded status") {
        std::string_view malformed = R"(
package com.example;

public class Broken {
    public void oops( {
        int x = ;
    }
}
)";

        auto res = adapter.parse(malformed, "src/Broken.java");
        REQUIRE(res.has_value());
        const AdapterResult& result = *res;
        REQUIRE_FALSE(result.diagnostics.empty());
        REQUIRE(result.status == worker::CompletionStatus::degraded);
        bool has_error = false;
        for (const auto& diag : result.diagnostics) {
            if (diag.severity == DiagnosticSeverity::error) {
                has_error = true;
                break;
            }
        }
        REQUIRE(has_error);
    }

    SECTION("Completely unparseable content produces failed status") {
        std::string_view junk = "@@@@ #### %%%% ;;;; ;;;; ;;;;";
        auto res = adapter.parse(junk, "src/Junk.java");
        REQUIRE(res.has_value());
        REQUIRE(res->status == worker::CompletionStatus::failed);
        REQUIRE_FALSE(res->diagnostics.empty());
    }

    SECTION("Stop token cancellation halts execution cleanly") {
        std::stop_source stop_source;
        stop_source.request_stop();

        std::string_view valid = "package com.example; public class A {}";
        auto res = adapter.parse(valid, "src/A.java", stop_source.get_token());
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().code == ErrorCode::cancelled);
    }
}

TEST_CASE("H8-Review: Java adapter review fixes and edge cases", "[adapter][java][h8][review]") {
    JavaAdapter adapter;

    SECTION(
        "Anonymous classes push scopes, avoid method collision, and emit implementation facts") {
        std::string_view source = R"(
package com.example;

public class Outer {
    public void runBoth() {
        Runnable r1 = new Runnable() {
            @Override
            public void run() {
                System.out.println("r1");
            }
        };

        Runnable r2 = new Runnable() {
            @Override
            public void run() {
                System.out.println("r2");
            }
        };
    }
}
)";
        auto res = adapter.parse(source, "src/Outer.java");
        REQUIRE(res.has_value());
        const auto& r = *res;

        // Two anonymous classes should be extracted as class symbols
        std::vector<const SymbolFact*> anon_classes;
        for (const auto& sym : r.symbols) {
            if (sym.kind == NodeKind::class_ && sym.name.starts_with("$anon_Runnable_")) {
                anon_classes.push_back(&sym);
            }
        }
        REQUIRE(anon_classes.size() == 2);
        CHECK(anon_classes[0]->name != anon_classes[1]->name);
        CHECK(anon_classes[0]->enclosing_scope == "com.example.Outer.runBoth");
        CHECK(anon_classes[1]->enclosing_scope == "com.example.Outer.runBoth");

        // Methods inside anonymous classes should have distinct scopes and qualified names
        std::vector<const SymbolFact*> run_methods;
        for (const auto& sym : r.symbols) {
            if (sym.kind == NodeKind::method && sym.name == "run") {
                run_methods.push_back(&sym);
            }
        }
        REQUIRE(run_methods.size() == 2);
        CHECK(run_methods[0]->qualified_name != run_methods[1]->qualified_name);
        CHECK(run_methods[0]->enclosing_scope == anon_classes[0]->qualified_name);
        CHECK(run_methods[1]->enclosing_scope == anon_classes[1]->qualified_name);

        // Implementation facts for anonymous classes
        int impl_count = 0;
        for (const auto& occ : r.occurrences) {
            if (occ.kind == worker::FactKind::implementation && occ.written_name == "Runnable") {
                ++impl_count;
                CHECK((occ.enclosing_scope == anon_classes[0]->qualified_name ||
                       occ.enclosing_scope == anon_classes[1]->qualified_name));
            }
        }
        REQUIRE(impl_count == 2);
    }

    SECTION("Non-overlapping highlight tokens and keyword highlighting of this and super") {
        std::string_view source = R"(
import java.util.Map;

public class Demo extends Base {
    public void test(Map.Entry<String, Object> entry) {
        this.doSomething();
        super.doSomething();
    }
}
)";
        auto hl_res = adapter.highlight(source);
        REQUIRE(hl_res.has_value());
        const auto& tokens = *hl_res;
        REQUIRE_FALSE(tokens.empty());

        // Check LSP non-overlapping invariant: no two tokens overlap on the same line
        for (size_t i = 1; i < tokens.size(); ++i) {
            const auto& prev = tokens[i - 1];
            const auto& curr = tokens[i];
            if (prev.line == curr.line) {
                CHECK(prev.start_column + prev.length <= curr.start_column);
            }
        }

        // Check "this" and "super" keyword tokens
        const auto& legend = HighlightLegend::default_legend();
        auto kw_idx = legend.token_type_index("keyword");
        REQUIRE(kw_idx.has_value());

        bool found_this = false;
        bool found_super = false;
        for (const auto& tok : tokens) {
            if (tok.token_type == *kw_idx) {
                if (source.substr(tok.byte_range.start, tok.length) == "this") {
                    found_this = true;
                }
                if (source.substr(tok.byte_range.start, tok.length) == "super") {
                    found_super = true;
                }
            }
        }
        CHECK(found_this);
        CHECK(found_super);
    }

    SECTION("Try-with-resources does not emit resource variable name as a reference") {
        std::string_view source = R"(
package com.example;

public class IoTest {
    public void read() throws Exception {
        try (AutoCloseable res = new MyResource()) {
            res.toString();
        }
    }
}
)";
        auto res = adapter.parse(source, "src/IoTest.java");
        REQUIRE(res.has_value());
        const auto& r = *res;

        // In `try (AutoCloseable res = ...)`, "res" at declaration site should not be a reference
        // occurrence Only `res.toString()` should have a reference to "res"
        int res_ref_count = 0;
        for (const auto& occ : r.occurrences) {
            if (occ.kind == worker::FactKind::reference && occ.written_name == "res") {
                ++res_ref_count;
            }
        }
        CHECK(res_ref_count == 1);
    }

    SECTION("Pattern matching variables are not emitted as references") {
        std::string_view source = R"(
package com.example;

public class PatternTest {
    public void test(Object obj) {
        if (obj instanceof String strVal) {
            System.out.println(strVal);
        }
    }
}
)";
        auto res = adapter.parse(source, "src/PatternTest.java");
        REQUIRE(res.has_value());
        const auto& r = *res;

        // "String" should be referenced
        bool found_string_ref = false;
        int str_val_ref_count = 0;
        for (const auto& occ : r.occurrences) {
            if (occ.kind == worker::FactKind::reference && occ.written_name == "String") {
                found_string_ref = true;
            }
            if (occ.kind == worker::FactKind::reference && occ.written_name == "strVal") {
                ++str_val_ref_count;
            }
        }
        CHECK(found_string_ref);
        // Only the usage in println, not the pattern declaration site
        CHECK(str_val_ref_count == 1);
    }

    SECTION("Sealed classes and interfaces extract permits clause as references") {
        std::string_view source = R"(
package com.example;

public sealed class Shape permits Circle, Square {}
final class Circle extends Shape {}
final class Square extends Shape {}
)";
        auto res = adapter.parse(source, "src/Shape.java");
        REQUIRE(res.has_value());
        const auto& r = *res;

        bool found_circle_permit = false;
        bool found_square_permit = false;
        for (const auto& occ : r.occurrences) {
            if (occ.kind == worker::FactKind::reference && occ.written_name == "Circle" &&
                occ.enclosing_scope == "com.example.Shape") {
                found_circle_permit = true;
            }
            if (occ.kind == worker::FactKind::reference && occ.written_name == "Square" &&
                occ.enclosing_scope == "com.example.Shape") {
                found_square_permit = true;
            }
        }
        CHECK(found_circle_permit);
        CHECK(found_square_permit);
    }

    SECTION("Record with varargs (spread_parameter) extracts field") {
        std::string_view source = R"(
package com.example;

public record Command(String name, String... args) {}
)";
        auto res = adapter.parse(source, "src/Command.java");
        REQUIRE(res.has_value());
        const auto& r = *res;

        bool found_name_field = false;
        bool found_args_field = false;
        for (const auto& sym : r.symbols) {
            if (sym.kind == NodeKind::field && sym.name == "name") {
                found_name_field = true;
            }
            if (sym.kind == NodeKind::field && sym.name == "args") {
                found_args_field = true;
                CHECK(sym.qualified_name == "com.example.Command.args");
            }
        }
        CHECK(found_name_field);
        CHECK(found_args_field);
    }

    SECTION("Annotation default values are walked") {
        std::string_view source = R"(
package com.example;

public @interface Config {
    String value() default "standard";
    Class<?> type() default Object.class;
}
)";
        auto res = adapter.parse(source, "src/Config.java");
        REQUIRE(res.has_value());
        const auto& r = *res;

        bool found_object_ref = false;
        for (const auto& occ : r.occurrences) {
            if (occ.written_name == "Object") {
                found_object_ref = true;
            }
        }
        CHECK(found_object_ref);
    }

    SECTION("Method references Target::method and Target::new") {
        std::string_view source = R"(
package com.example;

import java.util.function.Function;
import java.util.function.Supplier;

public class RefDemo {
    public void demo() {
        Supplier<RefDemo> ctor = RefDemo::new;
        Function<RefDemo, String> fn = RefDemo::getName;
    }

    public String getName() { return "demo"; }
}
)";
        auto res = adapter.parse(source, "src/RefDemo.java");
        REQUIRE(res.has_value());
        const auto& r = *res;

        bool found_new_call = false;
        bool found_get_name_call = false;
        for (const auto& occ : r.occurrences) {
            if (occ.kind == worker::FactKind::call && occ.written_name == "new") {
                found_new_call = true;
                CHECK(std::ranges::find(occ.candidate_targets, "RefDemo") !=
                      occ.candidate_targets.end());
            }
            if (occ.kind == worker::FactKind::call && occ.written_name == "getName") {
                found_get_name_call = true;
                CHECK(std::ranges::find(occ.candidate_targets, "RefDemo.getName") !=
                      occ.candidate_targets.end());
            }
        }
        CHECK(found_new_call);
        CHECK(found_get_name_call);
    }

    SECTION("extract_signature with @Override and no visibility modifiers strips annotation") {
        std::string_view source = R"(
package com.example;

public class Sub extends Base {
    @Override
    void play() {}
}
)";
        auto res = adapter.parse(source, "src/Sub.java");
        REQUIRE(res.has_value());
        const auto& r = *res;

        const SymbolFact* play_sym = nullptr;
        for (const auto& sym : r.symbols) {
            if (sym.name == "play") {
                play_sym = &sym;
                break;
            }
        }
        REQUIRE(play_sym != nullptr);
        CHECK_FALSE(play_sym->signature.starts_with("@Override"));
        CHECK(play_sym->signature.starts_with("void play()"));
    }
}
