#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "codelenses/adapters/adapter.hpp"
#include "codelenses/adapters/js_adapter.hpp"
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

TEST_CASE("H6-01: JavaScript adapter grammar registration, capabilities, and properties",
          "[adapter][javascript][h6]") {
    // 1. Grammar registration
    const auto* js_lang = treesitter::grammar_for_language(Language::javascript);
    REQUIRE(js_lang != nullptr);
    REQUIRE(treesitter::has_grammar_for_language(Language::javascript));
    REQUIRE(treesitter::grammar_version(Language::javascript) == "0.23.1");

    // 2. File extension mappings
    REQUIRE(language_from_extension(".js") == Language::javascript);
    REQUIRE(language_from_extension("js") == Language::javascript);
    REQUIRE(language_from_extension(".jsx") == Language::javascript);
    REQUIRE(language_from_extension("jsx") == Language::javascript);
    REQUIRE(language_from_extension(".mjs") == Language::javascript);
    REQUIRE(language_from_extension("mjs") == Language::javascript);
    REQUIRE(language_from_extension(".cjs") == Language::javascript);
    REQUIRE(language_from_extension("cjs") == Language::javascript);

    // 3. Path detection
    REQUIRE(language_from_path("src/index.js") == Language::javascript);
    REQUIRE(language_from_path("components/Button.jsx") == Language::javascript);
    REQUIRE(language_from_path("utils/helper.mjs") == Language::javascript);
    REQUIRE(language_from_path("config/db.cjs") == Language::javascript);

    // 4. String conversion
    auto parsed_js = language_from_string("javascript");
    REQUIRE(parsed_js.has_value());
    REQUIRE(*parsed_js == Language::javascript);

    auto parsed_js_short = language_from_string("js");
    REQUIRE(parsed_js_short.has_value());
    REQUIRE(*parsed_js_short == Language::javascript);

    auto parsed_mjs = language_from_string("mjs");
    REQUIRE(parsed_mjs.has_value());
    REQUIRE(*parsed_mjs == Language::javascript);

    auto parsed_cjs = language_from_string("cjs");
    REQUIRE(parsed_cjs.has_value());
    REQUIRE(*parsed_cjs == Language::javascript);

    auto parsed_jsx = language_from_string("jsx");
    REQUIRE(parsed_jsx.has_value());
    REQUIRE(*parsed_jsx == Language::javascript);

    // 5. Adapter properties and capabilities
    JavaScriptAdapter adapter;
    REQUIRE(adapter.language() == Language::javascript);
    REQUIRE(adapter.name() == "JavaScriptAdapter");

    const auto& caps = adapter.capabilities();
    CHECK(caps.functions == CapabilityStatus::supported);
    CHECK(caps.methods == CapabilityStatus::supported);
    CHECK(caps.classes == CapabilityStatus::supported);
    CHECK(caps.interfaces == CapabilityStatus::unavailable);
    CHECK(caps.enums == CapabilityStatus::unavailable);
    CHECK(caps.namespaces == CapabilityStatus::unavailable);
    CHECK(caps.variables == CapabilityStatus::supported);
    CHECK(caps.modules == CapabilityStatus::supported);
    CHECK(caps.containment == CapabilityStatus::supported);
    CHECK(caps.calls == CapabilityStatus::supported);
    CHECK(caps.references == CapabilityStatus::supported);
    CHECK(caps.inheritance == CapabilityStatus::supported);
    CHECK(caps.implementation == CapabilityStatus::unavailable);
    CHECK(caps.imports == CapabilityStatus::supported);
    CHECK(caps.templates == CapabilityStatus::unavailable);
    CHECK(caps.structs == CapabilityStatus::unavailable);
    CHECK(caps.packages == CapabilityStatus::unavailable);

    // 6. Registry integration
    auto& registry = default_adapter_registry();
    REQUIRE(registry.has_adapter(Language::javascript));
    auto* reg_adapter = registry.get_adapter(Language::javascript);
    REQUIRE(reg_adapter != nullptr);
    REQUIRE(reg_adapter->language() == Language::javascript);
    REQUIRE(reg_adapter->name() == "JavaScriptAdapter");

    auto* path_adapter = registry.get_adapter_for_path("src/app.js");
    REQUIRE(path_adapter == reg_adapter);
    auto* jsx_adapter = registry.get_adapter_for_path("src/component.jsx");
    REQUIRE(jsx_adapter == reg_adapter);
}

TEST_CASE("H6-01: JavaScript CommonJS and ES module IR extraction",
          "[adapter][javascript][h6]") {
    JavaScriptAdapter adapter;

    std::string_view source = R"(
const fs = require('fs');
const { join, resolve: pathResolve } = require('path');
const config = require('./config');

const VERSION = '1.0.0';

function calculateSum(a, b) {
    return a + b;
}

class BaseService {
    constructor(name) {
        this.name = name;
    }

    start() {
        console.log(this.name + ' started');
    }
}

class WorkerService extends BaseService {
    work() {
        this.start();
        calculateSum(10, 20);
    }
}

function LegacyClass(id) {
    this.id = id;
}

LegacyClass.prototype.getId = function() {
    return this.id;
};

LegacyClass.prototype.active = true;

exports.calculateSum = calculateSum;
exports.WorkerService = WorkerService;
exports.VERSION = VERSION;
module.exports.format = function(val) {
    return String(val);
};

export const MAX_TIMEOUT = 5000;
export default function runMain() {
    return calculateSum(1, 2);
}
)";

    auto parse_res = adapter.parse(source, "main.js");
    REQUIRE(parse_res.has_value());
    const AdapterResult& result = *parse_res;

    REQUIRE(result.language == Language::javascript);
    REQUIRE(result.status == worker::CompletionStatus::complete);
    REQUIRE(result.diagnostics.empty());

    // 1. Verify Imports and require() occurrences
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

    // 2. Destructured require symbols
    bool found_join = false;
    bool found_pathResolve = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "join" && sym.kind == NodeKind::variable)
            found_join = true;
        if (sym.name == "pathResolve" && sym.kind == NodeKind::variable)
            found_pathResolve = true;
    }
    CHECK(found_join);
    CHECK(found_pathResolve);

    // 3. Functions and Classes
    bool found_calculateSum = false;
    bool found_BaseService = false;
    bool found_WorkerService = false;
    bool found_LegacyClass = false;
    bool found_getId_method = false;
    bool found_active_field = false;
    bool found_runMain = false;
    bool found_format = false;

    for (const auto& sym : result.symbols) {
        if (sym.name == "calculateSum" && sym.kind == NodeKind::function)
            found_calculateSum = true;
        if (sym.name == "BaseService" && sym.kind == NodeKind::class_)
            found_BaseService = true;
        if (sym.name == "WorkerService" && sym.kind == NodeKind::class_)
            found_WorkerService = true;
        if (sym.name == "LegacyClass" && sym.kind == NodeKind::function)
            found_LegacyClass = true;
        if (sym.name == "getId" && sym.kind == NodeKind::method)
            found_getId_method = true;
        if (sym.name == "active" && sym.kind == NodeKind::field)
            found_active_field = true;
        if (sym.name == "runMain" && sym.kind == NodeKind::function)
            found_runMain = true;
        if (sym.name == "format" && sym.kind == NodeKind::function)
            found_format = true;
    }
    CHECK(found_calculateSum);
    CHECK(found_BaseService);
    CHECK(found_WorkerService);
    CHECK(found_LegacyClass);
    CHECK(found_getId_method);
    CHECK(found_active_field);
    CHECK(found_runMain);
    CHECK(found_format);

    // 4. Calls relation / occurrence
    bool found_call_calculateSum = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::call && occ.written_name == "calculateSum") {
            found_call_calculateSum = true;
        }
    }
    CHECK(found_call_calculateSum);

    // 5. Heritage occurrence for WorkerService extends BaseService
    bool found_base_heritage = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::inheritance && occ.written_name == "BaseService") {
            found_base_heritage = true;
        }
    }
    CHECK(found_base_heritage);

    // 6. Bare module.exports = { ... } does not emit module/exports reference occurrences
    std::string_view bare_export_source = R"(
const config = { a: 1 };
module.exports = {
    config: config
};
)";
    auto bare_res = adapter.parse(bare_export_source, "bare_export.js");
    REQUIRE(bare_res.has_value());
    REQUIRE(bare_res->status == worker::CompletionStatus::complete);

    bool found_module_ref = false;
    bool found_exports_ref = false;
    for (const auto& occ : bare_res->occurrences) {
        if (occ.written_name == "module")
            found_module_ref = true;
        if (occ.written_name == "exports")
            found_exports_ref = true;
    }
    CHECK_FALSE(found_module_ref);
    CHECK_FALSE(found_exports_ref);
}

TEST_CASE("H6-02: JSX component references and calls in JavaScript/JSX",
          "[adapter][javascript][h6]") {
    JavaScriptAdapter adapter;

    std::string_view jsx_source = R"(
import React, { useState } from 'react';
import { Button, Nav, Dialog } from './ui';

export function UserDashboard({ user, onSelect }) {
    const [count, setCount] = useState(0);

    const handleClick = () => {
        setCount(count + 1);
        onSelect(user.id);
    };

    return (
        <div className="dashboard">
            <Nav.Bar theme="dark">
                <Nav.Item label="Overview" active={true} />
                <Nav.Item label="Settings" active={false} />
            </Nav.Bar>
            <main>
                <h1>User: {user.name}</h1>
                <Button label="Click Me" onClick={handleClick} count={count}>
                    <span>Submit</span>
                </Button>
                <Dialog.Confirm onConfirm={handleClick} />
            </main>
        </div>
    );
}
)";

    auto parse_res = adapter.parse(jsx_source, "UserDashboard.jsx");
    REQUIRE(parse_res.has_value());
    const AdapterResult& result = *parse_res;

    REQUIRE(result.language == Language::javascript);
    REQUIRE(result.status == worker::CompletionStatus::complete);
    REQUIRE(result.diagnostics.empty());

    // 1. Verify custom component calls
    bool found_button_call = false;
    bool found_nav_bar_call = false;
    bool found_nav_item_call = false;
    bool found_dialog_confirm_call = false;
    bool found_html_div_call = false;
    bool found_html_span_call = false;

    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::call) {
            if (occ.written_name == "Button")
                found_button_call = true;
            if (occ.written_name == "Nav.Bar")
                found_nav_bar_call = true;
            if (occ.written_name == "Nav.Item")
                found_nav_item_call = true;
            if (occ.written_name == "Dialog.Confirm")
                found_dialog_confirm_call = true;
            if (occ.written_name == "div")
                found_html_div_call = true;
            if (occ.written_name == "span")
                found_html_span_call = true;
        }
    }

    CHECK(found_button_call);
    CHECK(found_nav_bar_call);
    CHECK(found_nav_item_call);
    CHECK(found_dialog_confirm_call);
    // HTML tags are not custom components
    CHECK_FALSE(found_html_div_call);
    CHECK_FALSE(found_html_span_call);

    // 2. Verify attribute expression references
    bool found_handleClick_ref = false;
    bool found_count_ref = false;
    for (const auto& occ : result.occurrences) {
        if (occ.kind == worker::FactKind::reference) {
            if (occ.written_name == "handleClick")
                found_handleClick_ref = true;
            if (occ.written_name == "count")
                found_count_ref = true;
        }
    }
    CHECK(found_handleClick_ref);
    CHECK(found_count_ref);
}

TEST_CASE("H6-02: JavaScript and JSX syntax highlighting query and token generation",
          "[adapter][javascript][h6]") {
    JavaScriptAdapter adapter;

    SECTION("JavaScript highlighting query succeeds and returns valid tokens") {
        std::string_view js_source = R"(
// Single-line comment
/* Multi-line comment */
const PI = 3.14159;
let isReady = true;
var message = "Hello, world!";

function compute(val) {
    if (val > 0) {
        return val * 2;
    }
    return null;
}

class Service {
    run() {
        return this.active;
    }
}
)";

        auto hl_res = adapter.highlight(js_source);
        if (!hl_res) {
            FAIL_CHECK("JS Highlight error: " << hl_res.error().message);
        }
        REQUIRE(hl_res.has_value());
        const auto& tokens = *hl_res;
        REQUIRE_FALSE(tokens.empty());

        const auto& legend = HighlightLegend::default_legend();
        auto kw_idx = legend.token_type_index("keyword");
        auto comment_idx = legend.token_type_index("comment");
        auto str_idx = legend.token_type_index("string");
        auto num_idx = legend.token_type_index("number");
        auto fn_idx = legend.token_type_index("function");
        auto op_idx = legend.token_type_index("operator");
        auto class_idx = legend.token_type_index("class");
        auto param_idx = legend.token_type_index("parameter");

        REQUIRE(kw_idx.has_value());
        REQUIRE(comment_idx.has_value());
        REQUIRE(str_idx.has_value());
        REQUIRE(num_idx.has_value());
        REQUIRE(fn_idx.has_value());
        REQUIRE(op_idx.has_value());
        REQUIRE(class_idx.has_value());
        REQUIRE(param_idx.has_value());

        bool found_keyword = false;
        bool found_comment = false;
        bool found_string = false;
        bool found_number = false;
        bool found_function = false;
        bool found_operator = false;
        bool found_class = false;
        bool found_parameter = false;

        for (const auto& tok : tokens) {
            if (tok.token_type == *kw_idx)
                found_keyword = true;
            if (tok.token_type == *comment_idx)
                found_comment = true;
            if (tok.token_type == *str_idx)
                found_string = true;
            if (tok.token_type == *num_idx)
                found_number = true;
            if (tok.token_type == *fn_idx)
                found_function = true;
            if (tok.token_type == *op_idx)
                found_operator = true;
            if (tok.token_type == *class_idx)
                found_class = true;
            if (tok.token_type == *param_idx)
                found_parameter = true;
        }

        CHECK(found_keyword);
        CHECK(found_comment);
        CHECK(found_string);
        CHECK(found_number);
        CHECK(found_function);
        CHECK(found_operator);
        CHECK(found_class);
        CHECK(found_parameter);
    }

    SECTION("Highlight convenience overload routes case-insensitively via file_path") {
        std::string_view jsx_source = R"(
export function Widget() {
    return <button className="btn">Click</button>;
}
)";
        auto hl_res = adapter.highlight(jsx_source, "Widget.JSX");
        REQUIRE(hl_res.has_value());
        REQUIRE_FALSE(hl_res->empty());

        const auto& legend = HighlightLegend::default_legend();
        auto kw_idx = legend.token_type_index("keyword");
        auto prop_idx = legend.token_type_index("property");
        REQUIRE(kw_idx.has_value());
        REQUIRE(prop_idx.has_value());

        bool found_kw = false;
        bool found_prop = false;
        for (const auto& tok : *hl_res) {
            if (tok.token_type == *kw_idx)
                found_kw = true;
            if (tok.token_type == *prop_idx)
                found_prop = true;
        }
        CHECK(found_kw);
        CHECK(found_prop);
    }

    SECTION("JSX highlighting tokens") {
        std::string_view jsx_source = R"(
import React from 'react';

export function Button({ label, onClick }) {
    return (
        <button className="primary-btn" onClick={onClick}>
            <span>{label}</span>
        </button>
    );
}
)";

        treesitter::Parser parser;
        REQUIRE(parser.set_language(treesitter::grammar_for_tsx()).has_value());
        auto tree_res = parser.parse_string(jsx_source);
        REQUIRE(tree_res.has_value());

        auto hl_res = adapter.highlight(jsx_source, *tree_res);
        if (!hl_res) {
            FAIL_CHECK("JSX Highlight error: " << hl_res.error().message);
        }
        REQUIRE(hl_res.has_value());
        const auto& tokens = *hl_res;
        REQUIRE_FALSE(tokens.empty());

        const auto& legend = HighlightLegend::default_legend();
        auto kw_idx = legend.token_type_index("keyword");
        auto prop_idx = legend.token_type_index("property");

        REQUIRE(kw_idx.has_value());
        REQUIRE(prop_idx.has_value());

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

TEST_CASE("H6-03: JavaScript adapter golden fixture validation",
          "[adapter][javascript][h6][golden]") {
    JavaScriptAdapter adapter;

    auto find_fixture_dir = []() -> std::filesystem::path {
#ifdef CODELENSES_SOURCE_DIR
        {
            auto candidate = std::filesystem::path(CODELENSES_SOURCE_DIR) / "tests/fixtures/javascript";
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
#endif
        const char* env_dir = std::getenv("CODELENSES_SOURCE_DIR");
        if (env_dir != nullptr && *env_dir != '\0') {
            auto candidate = std::filesystem::path(env_dir) / "tests/fixtures/javascript";
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
        for (const auto& candidate : {std::filesystem::path("tests/fixtures/javascript"),
                                      std::filesystem::path("../tests/fixtures/javascript")}) {
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
        return "tests/fixtures/javascript";
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
        REQUIRE(result.language == Language::javascript);
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

    SECTION("CommonJS fixture") {
        test_fixture("commonjs", ".js");
    }

    SECTION("ES Modules fixture") {
        test_fixture("es_modules", ".js");
    }

    SECTION("Dynamic Properties fixture") {
        test_fixture("dynamic_props", ".js");
    }

    SECTION("Component JSX fixture") {
        test_fixture("component", ".jsx");
    }
}

TEST_CASE("H6-03: JavaScript error diagnostics, cancellation, and edge cases",
          "[adapter][javascript][h6]") {
    JavaScriptAdapter adapter;

    SECTION("Syntax error produces diagnostics and degraded status") {
        std::string_view bad_source = R"(
function unclosed(x, y {
    return x + y;
)";
        auto res = adapter.parse(bad_source, "bad.js");
        REQUIRE(res.has_value());
        REQUIRE_FALSE(res->diagnostics.empty());
        REQUIRE(res->status == worker::CompletionStatus::degraded);
        REQUIRE(res->diagnostics[0].severity == DiagnosticSeverity::error);
    }

    SECTION("Cancellation via stop_token") {
        std::stop_source stop_src;
        stop_src.request_stop();

        std::string_view src = "const x = 42;\n";
        auto res = adapter.parse(src, "cancel.js", stop_src.get_token());
        REQUIRE_FALSE(res.has_value());
        REQUIRE(res.error().code == ErrorCode::cancelled);
    }

    SECTION("Dynamic properties and Object.defineProperty") {
        std::string_view src = R"(
const obj = {};
Object.defineProperty(obj, 'secretKey', {
    value: 'xyz',
    writable: false
});

const value = obj["secretKey"];
)";
        auto res = adapter.parse(src, "dyn.js");
        REQUIRE(res.has_value());
        REQUIRE(res->status == worker::CompletionStatus::complete);

        bool found_secretKey_sym = false;
        for (const auto& sym : res->symbols) {
            if (sym.name == "secretKey" && sym.kind == NodeKind::field) {
                found_secretKey_sym = true;
            }
        }
        CHECK(found_secretKey_sym);

        bool found_secretKey_ref = false;
        for (const auto& occ : res->occurrences) {
            if (occ.kind == worker::FactKind::reference && occ.written_name == "secretKey") {
                found_secretKey_ref = true;
            }
        }
        CHECK(found_secretKey_ref);
    }
}
