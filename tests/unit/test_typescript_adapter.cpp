#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "codelenses/adapters/adapter.hpp"
#include "codelenses/adapters/js_adapter.hpp"
#include "codelenses/adapters/registry.hpp"
#include "codelenses/adapters/ts_adapter.hpp"
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

TEST_CASE("H5-01: TypeScript adapter grammar registration, capabilities, and properties",
          "[adapter][typescript][h5]") {
    // 1. Grammar registration
    const auto* ts_lang = treesitter::grammar_for_language(Language::typescript);
    REQUIRE(ts_lang != nullptr);
    REQUIRE(treesitter::has_grammar_for_language(Language::typescript));
    REQUIRE(treesitter::grammar_version(Language::typescript) == "0.23.2");

    const auto* tsx_lang = treesitter::grammar_for_tsx();
    REQUIRE(tsx_lang != nullptr);

    const auto* js_lang = treesitter::grammar_for_language(Language::javascript);
    REQUIRE(js_lang != nullptr);
    REQUIRE(treesitter::has_grammar_for_language(Language::javascript));
    REQUIRE(treesitter::grammar_version(Language::javascript) == "0.23.1");

    // 2. File extension mappings
    REQUIRE(language_from_extension(".ts") == Language::typescript);
    REQUIRE(language_from_extension("ts") == Language::typescript);
    REQUIRE(language_from_extension(".tsx") == Language::typescript);
    REQUIRE(language_from_extension("tsx") == Language::typescript);
    REQUIRE(language_from_extension(".mts") == Language::typescript);
    REQUIRE(language_from_extension(".cts") == Language::typescript);

    // 3. Path detection
    REQUIRE(language_from_path("src/index.ts") == Language::typescript);
    REQUIRE(language_from_path("components/Button.tsx") == Language::typescript);
    REQUIRE(language_from_path("utils/helper.mts") == Language::typescript);
    REQUIRE(language_from_path("config/db.cts") == Language::typescript);

    // 4. String conversion
    auto parsed_ts = language_from_string("typescript");
    REQUIRE(parsed_ts.has_value());
    REQUIRE(*parsed_ts == Language::typescript);

    auto parsed_ts_short = language_from_string("ts");
    REQUIRE(parsed_ts_short.has_value());
    REQUIRE(*parsed_ts_short == Language::typescript);

    auto parsed_tsx = language_from_string("tsx");
    REQUIRE(parsed_tsx.has_value());
    REQUIRE(*parsed_tsx == Language::typescript);

    REQUIRE(to_string(Language::typescript) == "TypeScript");

    // 5. Adapter capabilities
    TypeScriptAdapter adapter;
    REQUIRE(adapter.language() == Language::typescript);
    REQUIRE(adapter.name() == "TypeScriptAdapter");

    const auto& caps = adapter.capabilities();
    CHECK(caps.functions == CapabilityStatus::supported);
    CHECK(caps.methods == CapabilityStatus::supported);
    CHECK(caps.classes == CapabilityStatus::supported);
    CHECK(caps.interfaces == CapabilityStatus::supported);
    CHECK(caps.enums == CapabilityStatus::supported);
    CHECK(caps.namespaces == CapabilityStatus::supported);
    CHECK(caps.variables == CapabilityStatus::supported);
    CHECK(caps.modules == CapabilityStatus::supported);
    CHECK(caps.containment == CapabilityStatus::supported);
    CHECK(caps.calls == CapabilityStatus::supported);
    CHECK(caps.references == CapabilityStatus::supported);
    CHECK(caps.inheritance == CapabilityStatus::supported);
    CHECK(caps.implementation == CapabilityStatus::supported);
    CHECK(caps.imports == CapabilityStatus::supported);
    CHECK(caps.templates == CapabilityStatus::supported);
    CHECK(caps.structs == CapabilityStatus::unavailable);
    CHECK(caps.packages == CapabilityStatus::unavailable);

    JavaScriptAdapter js_adapter;
    REQUIRE(js_adapter.language() == Language::javascript);
    REQUIRE(js_adapter.name() == "JavaScriptAdapter");
    CHECK(js_adapter.capabilities().templates == CapabilityStatus::unavailable);
    CHECK(js_adapter.capabilities().interfaces == CapabilityStatus::unavailable);

    // 6. Registry integration
    const auto* reg_adapter = default_adapter_registry().get_adapter(Language::typescript);
    REQUIRE(reg_adapter != nullptr);
    CHECK(reg_adapter->language() == Language::typescript);
    CHECK(reg_adapter->name() == "TypeScriptAdapter");
}

TEST_CASE("H5-01: TypeScript IR extraction for core language constructs",
          "[adapter][typescript][h5]") {
    TypeScriptAdapter adapter;

    std::string_view source = R"(
import { readFile as read, writeFile } from "fs";
import * as path from "path";
import config from "./config";

export type ID = string | number;
export type Handler<T> = (item: T) => boolean;

export enum Status {
    Active,
    Inactive = 2,
}

export interface User {
    id: ID;
    name: string;
    greet(): string;
}

export class BaseEntity {
    id: ID;
}

export class Account extends BaseEntity implements User {
    name: string = "default";
    status: Status = Status.Active;

    constructor(name: string) {
        super();
        this.name = name;
    }

    greet(): string {
        return "Hello " + this.name;
    }
}

export function processUser(user: User): boolean {
    const message = user.greet();
    writeFile("log.txt", message);
    return true;
}

export const computeTotal = (price: number, tax: number): number => {
    return price + tax;
};

export namespace Utilities {
    export function helper(): void {}
}
)";

    auto parse_res = adapter.parse(source, "main.ts");
    REQUIRE(parse_res.has_value());
    const AdapterResult& result = *parse_res;

    REQUIRE(result.language == Language::typescript);
    REQUIRE(result.status == worker::CompletionStatus::complete);
    REQUIRE(result.diagnostics.empty());

    // 1. Verify Imports and Aliases
    bool found_fs_import = false;
    bool found_path_import = false;
    bool found_config_import = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::import) {
            if (occ.written_name == "fs")
                found_fs_import = true;
            if (occ.written_name == "path")
                found_path_import = true;
            if (occ.written_name == "./config")
                found_config_import = true;
        }
    }
    CHECK(found_fs_import);
    CHECK(found_path_import);
    CHECK(found_config_import);

    // Import alias: read
    bool found_read_alias = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "read" && sym.kind == NodeKind::type_alias) {
            found_read_alias = true;
        }
    }
    CHECK(found_read_alias);

    // Default import: config
    bool found_config_sym = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "config" && sym.kind == NodeKind::variable) {
            found_config_sym = true;
        }
    }
    CHECK(found_config_sym);

    // Namespace import: path
    bool found_path_sym = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "path" && sym.kind == NodeKind::namespace_) {
            found_path_sym = true;
        }
    }
    CHECK(found_path_sym);

    // 2. Verify Type Aliases
    bool found_id_type = false;
    bool found_handler_type = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "ID" && sym.kind == NodeKind::type_alias) {
            found_id_type = true;
            CHECK(sym.signature == "type ID = string | number");
        }
        if (sym.name == "Handler" && sym.kind == NodeKind::type_alias) {
            found_handler_type = true;
            CHECK(sym.signature == "type Handler<T> = (item: T) => boolean");
        }
    }
    CHECK(found_id_type);
    CHECK(found_handler_type);

    // 3. Verify Enums and Enum Members
    bool found_status_enum = false;
    bool found_active_member = false;
    bool found_inactive_member = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "Status" && sym.kind == NodeKind::enum_) {
            found_status_enum = true;
        }
        if (sym.name == "Active" && sym.kind == NodeKind::enum_member) {
            found_active_member = true;
            CHECK(sym.qualified_name == "Status.Active");
        }
        if (sym.name == "Inactive" && sym.kind == NodeKind::enum_member) {
            found_inactive_member = true;
            CHECK(sym.qualified_name == "Status.Inactive");
        }
    }
    CHECK(found_status_enum);
    CHECK(found_active_member);
    CHECK(found_inactive_member);

    // 4. Verify Interfaces and Signatures
    bool found_user_interface = false;
    bool found_user_id_field = false;
    bool found_user_greet_method = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "User" && sym.kind == NodeKind::interface_) {
            found_user_interface = true;
        }
        if (sym.name == "id" && sym.kind == NodeKind::field && sym.enclosing_scope == "User") {
            found_user_id_field = true;
        }
        if (sym.name == "greet" && sym.kind == NodeKind::method && sym.enclosing_scope == "User") {
            found_user_greet_method = true;
        }
    }
    CHECK(found_user_interface);
    CHECK(found_user_id_field);
    CHECK(found_user_greet_method);

    // 5. Verify Classes, Inheritance, and Implementation
    bool found_base_entity = false;
    bool found_account = false;
    bool found_account_greet = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "BaseEntity" && sym.kind == NodeKind::class_) {
            found_base_entity = true;
        }
        if (sym.name == "Account" && sym.kind == NodeKind::class_) {
            found_account = true;
        }
        if (sym.name == "greet" && sym.kind == NodeKind::method &&
            sym.enclosing_scope == "Account") {
            found_account_greet = true;
        }
    }
    CHECK(found_base_entity);
    CHECK(found_account);
    CHECK(found_account_greet);

    bool found_extends = false;
    bool found_implements = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::inheritance && occ.written_name == "BaseEntity") {
            found_extends = true;
            CHECK(occ.enclosing_scope == "Account");
        }
        if (occ.kind == worker::FactKind::implementation && occ.written_name == "User") {
            found_implements = true;
            CHECK(occ.enclosing_scope == "Account");
        }
    }
    CHECK(found_extends);
    CHECK(found_implements);

    // 6. Verify Functions (Standard & Arrow)
    bool found_process_user = false;
    bool found_compute_total = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "processUser" && sym.kind == NodeKind::function) {
            found_process_user = true;
        }
        if (sym.name == "computeTotal" && sym.kind == NodeKind::function) {
            found_compute_total = true;
        }
    }
    CHECK(found_process_user);
    CHECK(found_compute_total);

    // 7. Verify Namespace
    bool found_utilities_ns = false;
    bool found_helper_fn = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "Utilities" && sym.kind == NodeKind::namespace_) {
            found_utilities_ns = true;
        }
        if (sym.name == "helper" && sym.kind == NodeKind::function &&
            sym.enclosing_scope == "Utilities") {
            found_helper_fn = true;
            CHECK(sym.qualified_name == "Utilities.helper");
        }
    }
    CHECK(found_utilities_ns);
    CHECK(found_helper_fn);

    // 8. Verify Calls
    bool found_write_file_call = false;
    bool found_greet_call = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::call) {
            if (occ.written_name == "writeFile")
                found_write_file_call = true;
            if (occ.written_name == "greet")
                found_greet_call = true;
        }
    }
    CHECK(found_write_file_call);
    CHECK(found_greet_call);
}

TEST_CASE("H5-02: JSX/TSX component references and calls", "[adapter][typescript][h5][tsx]") {
    TypeScriptAdapter adapter;

    std::string_view tsx_source = R"(
import React from "react";

export interface ButtonProps {
    title: string;
    onClick: () => void;
}

export function CustomButton(props: ButtonProps) {
    return <button onClick={props.onClick}>{props.title}</button>;
}

export const Feed = {
    Header: (props: { label: string }) => <h1>{props.label}</h1>,
};

export function App() {
    const handleClick = () => {
        console.log("clicked");
    };

    return (
        <div className="container">
            <Feed.Header label="Welcome" />
            <CustomButton title="Click Me" onClick={handleClick} />
        </div>
    );
}
)";

    auto parse_res = adapter.parse(tsx_source, "App.tsx");
    REQUIRE(parse_res.has_value());
    const AdapterResult& result = *parse_res;

    REQUIRE(result.language == Language::typescript);
    REQUIRE(result.status == worker::CompletionStatus::complete);
    REQUIRE(result.diagnostics.empty());

    // 1. Verify custom component call occurrences
    bool found_custom_button_call = false;
    bool found_feed_header_call = false;
    bool found_div_call = false;
    bool found_button_call = false;
    bool found_handle_click_ref = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::call) {
            if (occ.written_name == "CustomButton") {
                found_custom_button_call = true;
                CHECK(occ.enclosing_scope == "App");
            }
            if (occ.written_name == "Feed.Header") {
                found_feed_header_call = true;
                CHECK(occ.enclosing_scope == "App");
                CHECK(std::ranges::find(occ.candidate_targets, "Feed") !=
                      occ.candidate_targets.end());
            }
            if (occ.written_name == "div") {
                found_div_call = true;
            }
            if (occ.written_name == "button") {
                found_button_call = true;
            }
        }
        if (occ.kind == worker::FactKind::reference) {
            if (occ.written_name == "handleClick") {
                found_handle_click_ref = true;
            }
        }
    }

    CHECK(found_custom_button_call);
    CHECK(found_feed_header_call);
    CHECK_FALSE(found_div_call);    // HTML tags should NOT be treated as custom component calls
    CHECK_FALSE(found_button_call); // HTML tags should NOT be treated as custom component calls
    CHECK(found_handle_click_ref);
}

TEST_CASE("H5-02: TypeScript and TSX syntax highlighting query and token generation",
          "[adapter][typescript][h5][highlight]") {
    TypeScriptAdapter adapter;

    SECTION("TypeScript syntax highlighting") {
        std::string_view ts_source = R"(
import { sqrt } from "math";

// Calculate distance
export interface Point {
    x: number;
    y: number;
}

export class Geometry {
    computeDistance(p: Point): number {
        return sqrt(p.x * p.x + p.y * p.y);
    }
}
)";

        auto res = adapter.highlight(ts_source);
        if (!res) {
            FAIL_CHECK("TS Highlight error: " << res.error().message);
        }
        REQUIRE(res.has_value());
        const auto& tokens = *res;
        REQUIRE_FALSE(tokens.empty());

        const auto& legend = HighlightLegend::default_legend();
        auto kw_idx = legend.token_type_index("keyword");
        auto type_idx = legend.token_type_index("type");
        auto class_idx = legend.token_type_index("class");
        auto interface_idx = legend.token_type_index("interface");
        auto comment_idx = legend.token_type_index("comment");
        auto str_idx = legend.token_type_index("string");
        auto op_idx = legend.token_type_index("operator");

        REQUIRE(kw_idx.has_value());
        REQUIRE(type_idx.has_value());
        REQUIRE(class_idx.has_value());
        REQUIRE(interface_idx.has_value());
        REQUIRE(comment_idx.has_value());
        REQUIRE(str_idx.has_value());
        REQUIRE(op_idx.has_value());

        bool found_kw = false;
        bool found_comment = false;
        bool found_string = false;
        bool found_operator = false;
        bool found_interface = false;
        bool found_class = false;

        for (const auto& tok : tokens) {
            if (tok.token_type == *kw_idx)
                found_kw = true;
            if (tok.token_type == *comment_idx)
                found_comment = true;
            if (tok.token_type == *str_idx)
                found_string = true;
            if (tok.token_type == *op_idx)
                found_operator = true;
            if (tok.token_type == *interface_idx)
                found_interface = true;
            if (tok.token_type == *class_idx)
                found_class = true;
        }

        CHECK(found_kw);
        CHECK(found_comment);
        CHECK(found_string);
        CHECK(found_operator);
        CHECK(found_interface);
        CHECK(found_class);
    }

    SECTION("TSX syntax highlighting with Tree") {
        std::string_view tsx_source = R"(
import React from "react";
export function Button(props: { label: string }) {
    return <button className="btn">{props.label}</button>;
}
)";
        treesitter::Parser parser;
        REQUIRE(parser.set_language(treesitter::grammar_for_tsx()));
        auto tree_res = parser.parse_string(tsx_source);
        REQUIRE(tree_res.has_value());

        auto res = adapter.highlight(tsx_source, *tree_res);
        if (!res) {
            FAIL_CHECK("TSX Highlight error: " << res.error().message);
        }
        REQUIRE(res.has_value());
        const auto& tokens = *res;
        REQUIRE_FALSE(tokens.empty());

        const auto& legend = HighlightLegend::default_legend();
        auto kw_idx = legend.token_type_index("keyword");
        auto prop_idx = legend.token_type_index("property");

        bool found_kw = false;
        bool found_prop = false;

        for (const auto& tok : tokens) {
            if (tok.token_type == *kw_idx)
                found_kw = true;
            if (tok.token_type == *prop_idx)
                found_prop = true;
        }

        CHECK(found_kw);
        CHECK(found_prop);
    }
}

TEST_CASE("H5-03: TypeScript adapter golden fixture validation",
          "[adapter][typescript][h5][golden]") {
    TypeScriptAdapter adapter;

    auto find_fixture_dir = []() -> std::filesystem::path {
#ifdef CODELENSES_SOURCE_DIR
        std::filesystem::path p =
            std::filesystem::path(CODELENSES_SOURCE_DIR) / "tests/fixtures/typescript";
        if (std::filesystem::exists(p)) {
            return p;
        }
#endif
        for (const auto& candidate : {std::filesystem::path("tests/fixtures/typescript"),
                                      std::filesystem::path("../../tests/fixtures/typescript"),
                                      std::filesystem::path("../tests/fixtures/typescript")}) {
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
        return "tests/fixtures/typescript";
    };
    const auto fixture_dir = find_fixture_dir();

    auto test_fixture = [&](const std::string& fixture_name, const std::string& extension) {
        const std::string filename = fixture_name + extension;
        const std::filesystem::path fixture_path = fixture_dir / filename;
        const std::filesystem::path golden_path = fixture_dir / (filename + ".golden.json");

        REQUIRE(std::filesystem::exists(fixture_path));

        std::ifstream file(fixture_path);
        REQUIRE(file.is_open());
        std::string source((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

        auto parse_res = adapter.parse(source, fixture_path);
        REQUIRE(parse_res.has_value());
        const AdapterResult& result = *parse_res;
        REQUIRE(result.language == Language::typescript);
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

    SECTION("Aliases fixture") {
        test_fixture("aliases", ".ts");
    }

    SECTION("Re-exports fixture") {
        test_fixture("re_exports", ".ts");
    }

    SECTION("Generics fixture") {
        test_fixture("generics", ".ts");
    }

    SECTION("Component TSX fixture") {
        test_fixture("component", ".tsx");
    }
}

TEST_CASE("H5-03: TypeScript error diagnostics, cancellation, and edge cases",
          "[adapter][typescript][h5]") {
    TypeScriptAdapter adapter;

    SECTION("Syntax error produces diagnostics and degraded status") {
        std::string_view bad_source = R"(
function unclosed(x: number {
    return x + 1;
)";
        auto res = adapter.parse(bad_source, "bad.ts");
        REQUIRE(res.has_value());
        REQUIRE_FALSE(res->diagnostics.empty());
        REQUIRE(res->status == worker::CompletionStatus::degraded);
        REQUIRE(res->diagnostics[0].severity == DiagnosticSeverity::error);
    }

    SECTION("Cancellation via stop_token") {
        std::stop_source stop_src;
        stop_src.request_stop();

        std::string_view src = "const x = 42;\n";
        auto res = adapter.parse(src, "cancel.ts", stop_src.get_token());
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().code == ErrorCode::cancelled);
    }

    SECTION("Generic new expression") {
        std::string_view src = R"(
class Repository<T, K> {}
const repo = new Repository<User, number>();
)";
        auto res = adapter.parse(src, "new_generic.ts");
        REQUIRE(res.has_value());
        REQUIRE(res->status == worker::CompletionStatus::complete);

        bool found_repo_call = false;
        for (const auto& occ : res->occurrences) {
            if (occ.kind == worker::FactKind::call && occ.written_name == "Repository") {
                found_repo_call = true;
            }
        }
        CHECK(found_repo_call);
    }

    SECTION("Destructuring patterns in variable declarators") {
        std::string_view src = R"(
const { foo, bar: renamed } = getObj();
const [first, second] = getArr();
)";
        auto res = adapter.parse(src, "destruct.ts");
        REQUIRE(res.has_value());
        REQUIRE(res->status == worker::CompletionStatus::complete);

        bool found_foo = false;
        bool found_renamed = false;
        bool found_first = false;
        bool found_second = false;

        for (const auto& sym : res->symbols) {
            if (sym.name == "foo" && sym.kind == NodeKind::variable)
                found_foo = true;
            if (sym.name == "renamed" && sym.kind == NodeKind::variable)
                found_renamed = true;
            if (sym.name == "first" && sym.kind == NodeKind::variable)
                found_first = true;
            if (sym.name == "second" && sym.kind == NodeKind::variable)
                found_second = true;
        }

        CHECK(found_foo);
        CHECK(found_renamed);
        CHECK(found_first);
        CHECK(found_second);
    }
}
