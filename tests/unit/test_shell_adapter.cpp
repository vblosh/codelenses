#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "codelenses/adapters/adapter.hpp"
#include "codelenses/adapters/registry.hpp"
#include "codelenses/adapters/shell_adapter.hpp"
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

TEST_CASE("H9-01: Shell/Bash adapter grammar registration and file extensions",
          "[adapter][shell][bash][h9]") {
    // 1. Grammar registration
    const auto* ts_lang_shell = treesitter::grammar_for_language(Language::shell);
    REQUIRE(ts_lang_shell != nullptr);
    const auto* ts_lang_bash = treesitter::grammar_for_language(Language::bash);
    REQUIRE(ts_lang_bash != nullptr);
    REQUIRE(treesitter::has_grammar_for_language(Language::shell));
    REQUIRE(treesitter::has_grammar_for_language(Language::bash));
    REQUIRE(treesitter::grammar_version(Language::shell) == "0.23.3");
    REQUIRE(treesitter::grammar_version(Language::bash) == "0.23.3");

    // 2. File extension mappings
    REQUIRE(language_from_extension(".sh") == Language::shell);
    REQUIRE(language_from_extension("sh") == Language::shell);
    REQUIRE(language_from_extension(".command") == Language::shell);
    REQUIRE(language_from_extension("command") == Language::shell);
    REQUIRE(language_from_extension(".bash") == Language::bash);
    REQUIRE(language_from_extension("bash") == Language::bash);

    // 3. Path detection
    REQUIRE(language_from_path("scripts/build.sh") == Language::shell);
    REQUIRE(language_from_path("bin/run.command") == Language::shell);
    REQUIRE(language_from_path("deploy.bash") == Language::bash);

    // 4. Registry integration
    auto& registry = default_adapter_registry();
    REQUIRE(registry.has_adapter(Language::shell));
    REQUIRE(registry.has_adapter(Language::bash));

    auto* adapter_sh = registry.get_adapter(Language::shell);
    REQUIRE(adapter_sh != nullptr);
    REQUIRE(adapter_sh->language() == Language::shell);
    REQUIRE(adapter_sh->name() == "ShellAdapter");

    auto* adapter_bash = registry.get_adapter(Language::bash);
    REQUIRE(adapter_bash != nullptr);
    REQUIRE(adapter_bash->language() == Language::bash);
    REQUIRE(adapter_bash->name() == "BashAdapter");

    // 5. Capabilities check
    const auto& caps = adapter_sh->capabilities();
    REQUIRE(caps.functions == CapabilityStatus::supported);
    REQUIRE(caps.variables == CapabilityStatus::supported);
    REQUIRE(caps.containment == CapabilityStatus::supported);
    REQUIRE(caps.calls == CapabilityStatus::supported);
    REQUIRE(caps.references == CapabilityStatus::supported);
    REQUIRE(caps.imports == CapabilityStatus::supported);
    REQUIRE(caps.classes == CapabilityStatus::unavailable);
    REQUIRE(caps.structs == CapabilityStatus::unavailable);
    REQUIRE(caps.interfaces == CapabilityStatus::unavailable);
    REQUIRE(caps.enums == CapabilityStatus::unavailable);
    REQUIRE(caps.methods == CapabilityStatus::unavailable);
}

TEST_CASE("H9-01: Extraction of functions, variables, source directives, and command occurrences",
          "[adapter][shell][bash][h9]") {
    ShellAdapter adapter;

    std::string_view source = R"(
#!/bin/bash
GLOBAL_VAR="hello"
MAX_COUNT=100

foo() {
    local local_var="inner"
    echo "$local_var $GLOBAL_VAR"
    bar
}

function bar {
    local count=1
    while [ $count -le 5 ]; do
        echo "Count: $count"
        count=$(( count + 1 ))
    done
}

function with_parens() {
    return 0
}

isolated() (
    cd /tmp && pwd
)

for item in alpha beta gamma; do
    echo "Item: $item"
done

export EXPORTED_VAR="public"
declare -r READONLY_VAR="immutable"
readonly ANOTHER_CONST=42

source ./lib/utils.sh
. ./config/settings.sh
source "$DYNAMIC_CONFIG"
source "lib/quoted_helpers.sh"

foo
)";

    auto res = adapter.parse(source, "test.sh");
    REQUIRE(res.has_value());
    REQUIRE(res->status == worker::CompletionStatus::complete);
    REQUIRE(res->diagnostics.empty());

    // Check symbols
    auto find_sym = [&](std::string_view name) -> const SymbolFact* {
        for (const auto& s : res->symbols) {
            if (s.name == name)
                return &s;
        }
        return nullptr;
    };

    // Functions
    const auto* sym_foo = find_sym("foo");
    REQUIRE(sym_foo != nullptr);
    CHECK(sym_foo->kind == NodeKind::function);
    CHECK_FALSE(sym_foo->enclosing_scope.has_value());

    const auto* sym_bar = find_sym("bar");
    REQUIRE(sym_bar != nullptr);
    CHECK(sym_bar->kind == NodeKind::function);

    const auto* sym_with_parens = find_sym("with_parens");
    REQUIRE(sym_with_parens != nullptr);
    CHECK(sym_with_parens->kind == NodeKind::function);

    const auto* sym_isolated = find_sym("isolated");
    REQUIRE(sym_isolated != nullptr);
    CHECK(sym_isolated->kind == NodeKind::function);

    // Variables
    const auto* sym_global = find_sym("GLOBAL_VAR");
    REQUIRE(sym_global != nullptr);
    CHECK(sym_global->kind == NodeKind::variable);

    const auto* sym_max = find_sym("MAX_COUNT");
    REQUIRE(sym_max != nullptr);
    CHECK(sym_max->kind == NodeKind::variable);

    const auto* sym_local = find_sym("local_var");
    REQUIRE(sym_local != nullptr);
    CHECK(sym_local->kind == NodeKind::variable);
    CHECK(sym_local->enclosing_scope == "foo");
    CHECK(sym_local->qualified_name == "foo.local_var");

    const auto* sym_count = find_sym("count");
    REQUIRE(sym_count != nullptr);
    CHECK(sym_count->kind == NodeKind::variable);
    CHECK(sym_count->enclosing_scope == "bar");

    const auto* sym_item = find_sym("item");
    REQUIRE(sym_item != nullptr);
    CHECK(sym_item->kind == NodeKind::variable);

    const auto* sym_exported = find_sym("EXPORTED_VAR");
    REQUIRE(sym_exported != nullptr);
    CHECK(sym_exported->kind == NodeKind::variable);

    const auto* sym_readonly = find_sym("READONLY_VAR");
    REQUIRE(sym_readonly != nullptr);
    CHECK(sym_readonly->kind == NodeKind::variable);

    const auto* sym_const = find_sym("ANOTHER_CONST");
    REQUIRE(sym_const != nullptr);
    CHECK(sym_const->kind == NodeKind::variable);

    // Check imports (source directives)
    auto find_import = [&](std::string_view name) -> const OccurrenceFact* {
        for (const auto& occ : res->occurrences) {
            if (occ.kind == worker::FactKind::import && occ.written_name == name)
                return &occ;
        }
        return nullptr;
    };

    const auto* imp_utils = find_import("./lib/utils.sh");
    REQUIRE(imp_utils != nullptr);
    CHECK(imp_utils->confidence == 1.0);
    CHECK_FALSE(imp_utils->metadata_json.has_value());

    const auto* imp_settings = find_import("./config/settings.sh");
    REQUIRE(imp_settings != nullptr);
    CHECK(imp_settings->confidence == 1.0);

    const auto* imp_dynamic = find_import("$DYNAMIC_CONFIG");
    REQUIRE(imp_dynamic != nullptr);
    CHECK(imp_dynamic->confidence == 0.0);
    REQUIRE(imp_dynamic->metadata_json.has_value());
    CHECK(imp_dynamic->metadata_json->find("dynamic") != std::string::npos);

    const auto* imp_quoted = find_import("lib/quoted_helpers.sh");
    REQUIRE(imp_quoted != nullptr);
    CHECK(imp_quoted->confidence == 1.0);
    CHECK(source.substr(imp_quoted->range.start, imp_quoted->range.size()) ==
          "lib/quoted_helpers.sh");

    // Check variable reference occurrences
    auto has_var_ref = [&](std::string_view name) {
        return std::ranges::any_of(res->occurrences, [&](const OccurrenceFact& occ) {
            return occ.kind == worker::FactKind::reference && occ.written_name == name;
        });
    };
    CHECK(has_var_ref("local_var"));
    CHECK(has_var_ref("GLOBAL_VAR"));
    CHECK(has_var_ref("count"));
    CHECK(has_var_ref("item"));
    CHECK(has_var_ref("DYNAMIC_CONFIG"));

    // Check call occurrences
    auto find_call = [&](std::string_view name) -> const OccurrenceFact* {
        for (const auto& occ : res->occurrences) {
            if (occ.kind == worker::FactKind::call && occ.written_name == name)
                return &occ;
        }
        return nullptr;
    };

    const auto* call_bar = find_call("bar");
    REQUIRE(call_bar != nullptr);
    CHECK(call_bar->enclosing_scope == "foo");

    const auto* call_foo = find_call("foo");
    REQUIRE(call_foo != nullptr);
    CHECK_FALSE(call_foo->enclosing_scope.has_value());
}

TEST_CASE("H9-02: Distinguishing shell built-ins, external commands, functions, and aliases",
          "[adapter][shell][bash][h9]") {
    ShellAdapter adapter;

    std::string_view source = R"(
#!/bin/bash
alias ll='ls -la'
alias my_alias="echo aliased"
alias cd='pushd'

custom_func() {
    echo "inside function"
}

# Built-ins
echo "hello"
pwd
test -f /etc/passwd

# Functions
custom_func

# Aliases
ll
my_alias
cd /tmp

# External commands
curl -s https://example.com
grep -rn "pattern" .
/usr/bin/python3 script.py
./local_tool.sh

# Explicit builtin and command invocations
builtin cd /var
command ls -l
)";

    auto res = adapter.parse(source, "test_builtins.sh");
    REQUIRE(res.has_value());
    REQUIRE(res->status == worker::CompletionStatus::complete);

    // Verify alias definitions are extracted
    auto find_alias = [&](std::string_view name) -> const SymbolFact* {
        for (const auto& s : res->symbols) {
            if (s.name == name && s.kind == NodeKind::macro)
                return &s;
        }
        return nullptr;
    };

    const auto* alias_ll = find_alias("ll");
    REQUIRE(alias_ll != nullptr);
    CHECK(alias_ll->signature.starts_with("alias ll="));

    const auto* alias_my = find_alias("my_alias");
    REQUIRE(alias_my != nullptr);

    const auto* alias_cd = find_alias("cd");
    REQUIRE(alias_cd != nullptr);

    // Verify metadata distinguishing command types
    auto get_call_meta = [&](std::string_view cmd_name) -> std::optional<std::string> {
        for (const auto& occ : res->occurrences) {
            if (occ.kind == worker::FactKind::call && occ.written_name == cmd_name) {
                return occ.metadata_json;
            }
        }
        return std::nullopt;
    };

    // Builtins
    auto echo_meta = get_call_meta("echo");
    REQUIRE(echo_meta.has_value());
    CHECK(echo_meta->find(R"("command_type":"builtin")") != std::string::npos);
    CHECK(echo_meta->find(R"("is_builtin":true)") != std::string::npos);

    // Aliases and precedence: alias cd shadows builtin cd, but builtin cd /var remains builtin
    std::vector<std::string> cd_metas;
    for (const auto& occ : res->occurrences) {
        if (occ.kind == worker::FactKind::call && occ.written_name == "cd") {
            if (occ.metadata_json)
                cd_metas.push_back(*occ.metadata_json);
        }
    }
    REQUIRE(cd_metas.size() == 2);
    CHECK(cd_metas[0].find(R"("command_type":"alias")") != std::string::npos);
    CHECK(cd_metas[0].find(R"("is_builtin":false)") != std::string::npos);
    CHECK(cd_metas[1].find(R"("command_type":"builtin")") != std::string::npos);
    CHECK(cd_metas[1].find(R"("is_builtin":true)") != std::string::npos);

    auto pwd_meta = get_call_meta("pwd");
    REQUIRE(pwd_meta.has_value());
    CHECK(pwd_meta->find(R"("command_type":"builtin")") != std::string::npos);

    auto test_meta = get_call_meta("test");
    REQUIRE(test_meta.has_value());
    CHECK(test_meta->find(R"("command_type":"builtin")") != std::string::npos);

    // Functions
    auto func_meta = get_call_meta("custom_func");
    REQUIRE(func_meta.has_value());
    CHECK(func_meta->find(R"("command_type":"function")") != std::string::npos);
    CHECK(func_meta->find(R"("is_builtin":false)") != std::string::npos);

    // Aliases
    auto ll_meta = get_call_meta("ll");
    REQUIRE(ll_meta.has_value());
    CHECK(ll_meta->find(R"("command_type":"alias")") != std::string::npos);
    CHECK(ll_meta->find(R"("is_builtin":false)") != std::string::npos);

    // External commands
    auto curl_meta = get_call_meta("curl");
    REQUIRE(curl_meta.has_value());
    CHECK(curl_meta->find(R"("command_type":"external")") != std::string::npos);
    CHECK(curl_meta->find(R"("is_builtin":false)") != std::string::npos);

    auto grep_meta = get_call_meta("grep");
    REQUIRE(grep_meta.has_value());
    CHECK(grep_meta->find(R"("command_type":"external")") != std::string::npos);

    auto py_meta = get_call_meta("/usr/bin/python3");
    REQUIRE(py_meta.has_value());
    CHECK(py_meta->find(R"("command_type":"external")") != std::string::npos);

    auto tool_meta = get_call_meta("./local_tool.sh");
    REQUIRE(tool_meta.has_value());
    CHECK(tool_meta->find(R"("command_type":"external")") != std::string::npos);

    // command ls -> ls should be external
    auto ls_meta = get_call_meta("ls");
    REQUIRE(ls_meta.has_value());
    CHECK(ls_meta->find(R"("command_type":"external")") != std::string::npos);
}

TEST_CASE("H9: Shell adapter syntax highlighting query and token generation",
          "[adapter][shell][bash][h9][highlight]") {
    ShellAdapter adapter;

    auto query = ShellAdapter::highlighting_query();
    REQUIRE_FALSE(query.empty());

    std::string_view source = R"(
# Configuration script
APP_NAME="CodeLenses"
PORT=8080

greet() {
    local user="$1"
    if [ -n "$user" ]; then
        echo "Hello, $user!"
    else
        echo "Hello, anonymous!"
    fi
}

greet "developer"
)";

    auto hl_res = adapter.highlight(source);
    REQUIRE(hl_res.has_value());
    const auto& tokens = *hl_res;
    REQUIRE_FALSE(tokens.empty());

    const auto& legend = HighlightLegend::default_legend();

    auto has_token = [&](std::string_view type_name) {
        auto type_idx = legend.token_type_index(type_name);
        if (!type_idx.has_value())
            return false;
        return std::ranges::any_of(
            tokens, [&](const HighlightToken& t) { return t.token_type == *type_idx; });
    };

    CHECK(has_token("comment"));
    CHECK(has_token("variable"));
    CHECK(has_token("string"));
    CHECK(has_token("number"));
    CHECK(has_token("keyword"));
    CHECK(has_token("function"));
    CHECK(has_token("operator"));
}

TEST_CASE("H9-03: Shell adapter fixtures for quoting, subshells, aliases, and sourced files",
          "[adapter][shell][bash][h9][fixtures]") {
    ShellAdapter adapter;

    auto find_fixture_dir = []() -> std::filesystem::path {
#ifdef CODELENSES_SOURCE_DIR
        std::filesystem::path p =
            std::filesystem::path(CODELENSES_SOURCE_DIR) / "tests/fixtures/shell";
        if (std::filesystem::exists(p))
            return p;
#endif
        for (const auto& candidate : {std::filesystem::path("tests/fixtures/shell"),
                                      std::filesystem::path("../../tests/fixtures/shell"),
                                      std::filesystem::path("../tests/fixtures/shell")}) {
            if (std::filesystem::exists(candidate))
                return candidate;
        }
        return "tests/fixtures/shell";
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
        REQUIRE(result.language == Language::shell);
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

    SECTION("Quoting fixture") {
        test_fixture("quoting", ".sh");
    }

    SECTION("Subshells fixture") {
        test_fixture("subshells", ".sh");
    }

    SECTION("Aliases fixture") {
        test_fixture("aliases", ".sh");
    }

    SECTION("Sourced files fixture") {
        test_fixture("sourced_files", ".sh");
    }
}

TEST_CASE("H9: Shell adapter cancellation and error resilience", "[adapter][shell][bash][h9]") {
    ShellAdapter adapter;

    SECTION("Cooperative cancellation") {
        std::stop_source stop_source;
        stop_source.request_stop();

        std::string_view source = "echo 'hello world'\n";
        auto res = adapter.parse(source, "cancelled.sh", stop_source.get_token());
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().code == ErrorCode::cancelled);
    }

    SECTION("Syntax error with partial symbols produces degraded status") {
        std::string_view partial = R"(
MY_VAR="hello"
function unclosed() {
    echo "incomplete"
)";
        auto res = adapter.parse(partial, "partial.sh");
        REQUIRE(res.has_value());
        CHECK(res->status == worker::CompletionStatus::degraded);
        CHECK_FALSE(res->diagnostics.empty());
        CHECK(res->has_errors());
    }

    SECTION("Malformed shell script without symbols produces failed status") {
        std::string_view malformed =
            "if [ 1 -eq 1 ]; then\n"; // missing fi, no symbols or occurrences
        auto res = adapter.parse(malformed, "malformed.sh");
        REQUIRE(res.has_value());
        CHECK(res->status == worker::CompletionStatus::failed);
        CHECK_FALSE(res->diagnostics.empty());
        CHECK(res->has_errors());
    }
}
