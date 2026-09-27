#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "codelenses/adapters/go_adapter.hpp"
#include "codelenses/adapters/registry.hpp"
#include "codelenses/language.hpp"
#include "codelenses/parser/fixture.hpp"
#include "codelenses/parser/highlight.hpp"
#include "codelenses/treesitter/grammars.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace codelenses;
using namespace codelenses::adapters;

TEST_CASE("H7-01: Go grammar, file mappings, registry, and capabilities", "[adapter][go][h7]") {
    REQUIRE(treesitter::grammar_for_language(Language::go) != nullptr);
    REQUIRE(treesitter::has_grammar_for_language(Language::go));
    REQUIRE(treesitter::grammar_version(Language::go) == "0.23.4");

    REQUIRE(language_from_extension(".go") == Language::go);
    REQUIRE(language_from_extension("go") == Language::go);
    REQUIRE(language_from_path("pkg/queue/worker.go") == Language::go);

    GoAdapter adapter;
    REQUIRE(adapter.language() == Language::go);
    REQUIRE(adapter.name() == "GoAdapter");
    CHECK(adapter.capabilities().packages == CapabilityStatus::supported);
    CHECK(adapter.capabilities().imports == CapabilityStatus::supported);
    CHECK(adapter.capabilities().structs == CapabilityStatus::supported);
    CHECK(adapter.capabilities().interfaces == CapabilityStatus::supported);
    CHECK(adapter.capabilities().functions == CapabilityStatus::supported);
    CHECK(adapter.capabilities().methods == CapabilityStatus::supported);
    CHECK(adapter.capabilities().calls == CapabilityStatus::supported);
    CHECK(adapter.capabilities().implementation == CapabilityStatus::candidate_only);

    auto& registry = default_adapter_registry();
    REQUIRE(registry.has_adapter(Language::go));
    REQUIRE(registry.get_adapter_for_path("pkg/queue/worker.go") != nullptr);
}

TEST_CASE("H7-03: Go aliases and multi-file package fixtures", "[adapter][go][h7]") {
    GoAdapter adapter;
    const auto fixture_dir =
        std::filesystem::path(CODELENSES_SOURCE_DIR) / "tests" / "fixtures" / "go";

    auto parse_fixture = [&](const std::filesystem::path& relative_path) {
        const auto fixture_path = fixture_dir / relative_path;
        std::ifstream file(fixture_path);
        REQUIRE(file.is_open());
        const std::string source((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
        auto parsed = adapter.parse(source, fixture_path);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->status == worker::CompletionStatus::complete);
        REQUIRE(parsed->diagnostics.empty());
        REQUIRE_FALSE(parsed->symbols.empty());

        const auto golden_path = std::filesystem::path(fixture_path.string() + ".golden.json");
        const char* update_env = std::getenv("CODELENSES_UPDATE_GOLDEN");
        if ((update_env != nullptr && std::string_view(update_env) == "1") ||
            !std::filesystem::exists(golden_path)) {
            std::ofstream out(golden_path);
            REQUIRE(out.is_open());
            out << adapter_result_to_json(*parsed).dump(2) << "\n";
        }
        const auto comparison = compare_golden_file(*parsed, golden_path);
        INFO("Diff for " << relative_path << ": " << comparison.diff);
        REQUIRE(comparison.matches);
        return *parsed;
    };

    const auto aliases = parse_fixture("aliases.go");
    bool found_alias = false;
    bool found_named_type = false;
    bool found_model_import = false;
    for (const auto& symbol : aliases.symbols) {
        if (symbol.name == "Task" && symbol.kind == NodeKind::type_alias) {
            found_alias = true;
            CHECK(symbol.qualified_name == "aliasfixture.Task");
        }
        if (symbol.name == "TaskID" && symbol.kind == NodeKind::type_alias) {
            found_named_type = true;
        }
    }
    for (const auto& occurrence : aliases.occurrences) {
        found_model_import |= occurrence.kind == worker::FactKind::import &&
                              occurrence.written_name == "example.org/project/model";
    }
    CHECK(found_alias);
    CHECK(found_named_type);
    CHECK(found_model_import);
    CHECK(std::ranges::any_of(aliases.occurrences, [](const auto& occurrence) {
        return occurrence.kind == worker::FactKind::reference &&
               occurrence.written_name == "model.Task";
    }));
    CHECK_FALSE(std::ranges::any_of(aliases.occurrences, [](const auto& occurrence) {
        return occurrence.kind == worker::FactKind::reference &&
               occurrence.written_name == "Task" && occurrence.enclosing_scope &&
               *occurrence.enclosing_scope == "aliasfixture";
    }));
    CHECK_FALSE(std::ranges::any_of(aliases.occurrences, [](const auto& occurrence) {
        return occurrence.kind == worker::FactKind::reference &&
               (occurrence.written_name == "int" || occurrence.written_name == "string" ||
                occurrence.written_name == "error");
    }));

    const auto task_file = parse_fixture("multi_file/task.go");
    const auto worker_file = parse_fixture("multi_file/worker.go");
    CHECK(std::ranges::any_of(task_file.symbols, [](const auto& symbol) {
        return symbol.name == "Task" && symbol.qualified_name == "queue.Task";
    }));
    CHECK(std::ranges::any_of(worker_file.symbols, [](const auto& symbol) {
        return symbol.name == "Worker" && symbol.kind == NodeKind::interface_ &&
               symbol.qualified_name == "queue.Worker";
    }));
    CHECK(std::ranges::any_of(worker_file.symbols, [](const auto& symbol) {
        return symbol.name == "Work" && symbol.kind == NodeKind::method &&
               symbol.qualified_name == "queue.LocalWorker.Work";
    }));
    CHECK(std::ranges::any_of(worker_file.declarations, [](const auto& declaration) {
        return declaration.qualified_name == "queue.Worker.Work" && !declaration.is_definition;
    }));
}

TEST_CASE("H7-02: Go interface implementation candidates", "[adapter][go][h7]") {
    GoAdapter adapter;
    const auto fixture_path = std::filesystem::path(CODELENSES_SOURCE_DIR) / "tests" / "fixtures" /
                              "go" / "multi_file" / "worker.go";
    std::ifstream file(fixture_path);
    REQUIRE(file.is_open());
    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    auto parsed = adapter.parse(source, fixture_path);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->diagnostics.empty());

    CHECK(std::ranges::count_if(parsed->occurrences, [](const auto& occurrence) {
              return occurrence.kind == worker::FactKind::implementation &&
                     occurrence.written_name == "queue.Worker";
          }) == 1);
    CHECK(std::ranges::any_of(parsed->occurrences, [](const auto& occurrence) {
        return occurrence.kind == worker::FactKind::call && occurrence.written_name == "Println";
    }));
    CHECK(std::ranges::any_of(parsed->symbols, [](const auto& symbol) {
        return symbol.name == "Work" && symbol.kind == NodeKind::method &&
               symbol.qualified_name == "queue.LocalWorker.Work";
    }));
    CHECK(std::ranges::any_of(parsed->symbols, [](const auto& symbol) {
        return symbol.name == "GenericWorker" &&
               symbol.signature == "type GenericWorker[T any] struct";
    }));
    CHECK(std::ranges::any_of(parsed->occurrences, [](const auto& occurrence) {
        return occurrence.kind == worker::FactKind::reference && occurrence.written_name == "Task";
    }));
}

TEST_CASE("H7-01: Go highlighting query runs against the pinned grammar", "[adapter][go][h7]") {
    GoAdapter adapter;
    const std::string source = "package highlight\nfunc Say() { println(\"hello\") }\n";
    auto highlighted = adapter.highlight(source);
    if (!highlighted)
        FAIL("Highlight error: " << highlighted.error().message);
    REQUIRE(highlighted.has_value());
    REQUIRE_FALSE(highlighted->empty());
    const auto type_type = HighlightLegend::default_legend().token_type_index("type");
    const auto definition_modifier =
        HighlightLegend::default_legend().token_modifier_bit("definition");
    REQUIRE(type_type.has_value());
    REQUIRE(definition_modifier.has_value());
    CHECK(std::ranges::any_of(*highlighted, [&](const auto& token) {
        return token.token_type == *type_type &&
               (token.token_modifiers & *definition_modifier) != 0 &&
               source.substr(token.byte_range.start,
                             token.byte_range.end - token.byte_range.start) == "highlight";
    }));
    const auto string_type = HighlightLegend::default_legend().token_type_index("string");
    REQUIRE(string_type.has_value());
    CHECK(std::ranges::any_of(*highlighted, [&](const auto& token) {
        return token.token_type == *string_type &&
               source.substr(token.byte_range.start,
                             token.byte_range.end - token.byte_range.start) == "\"hello\"";
    }));
}
