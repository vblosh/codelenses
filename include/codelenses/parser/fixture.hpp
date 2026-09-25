#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "codelenses/adapters/adapter.hpp"
#include "codelenses/language.hpp"
#include "codelenses/model.hpp"
#include "codelenses/worker/protocol.hpp"
#include <nlohmann/json.hpp>

namespace codelenses {

constexpr std::string_view to_string(worker::CompletionStatus s) noexcept {
    switch (s) {
    case worker::CompletionStatus::complete:
        return "complete";
    case worker::CompletionStatus::failed:
        return "failed";
    case worker::CompletionStatus::cancelled:
        return "cancelled";
    case worker::CompletionStatus::degraded:
        return "degraded";
    }
    return "complete";
}

constexpr std::string_view to_string(worker::FactKind k) noexcept {
    switch (k) {
    case worker::FactKind::symbol:
        return "symbol";
    case worker::FactKind::declaration:
        return "declaration";
    case worker::FactKind::scope:
        return "scope";
    case worker::FactKind::reference:
        return "reference";
    case worker::FactKind::call:
        return "call";
    case worker::FactKind::inheritance:
        return "inheritance";
    case worker::FactKind::implementation:
        return "implementation";
    case worker::FactKind::include:
        return "include";
    case worker::FactKind::import:
        return "import";
    }
    return "reference";
}

inline nlohmann::json adapter_result_to_json(const adapters::AdapterResult& result) {
    nlohmann::json symbols_json = nlohmann::json::array();
    for (const auto& sym : result.symbols) {
        nlohmann::json s = {
            {"name", sym.name},
            {"qualified_name", sym.qualified_name},
            {"kind", to_string(sym.kind)},
            {"start_byte", sym.range.start},
            {"end_byte", sym.range.end},
            {"start_line", sym.display_range.start_line},
            {"start_column", sym.display_range.start_column},
            {"end_line", sym.display_range.end_line},
            {"end_column", sym.display_range.end_column},
        };
        if (sym.enclosing_scope.has_value()) {
            s["enclosing_scope"] = *sym.enclosing_scope;
        }
        if (!sym.signature.empty()) {
            s["signature"] = sym.signature;
        }
        symbols_json.push_back(std::move(s));
    }

    nlohmann::json decls_json = nlohmann::json::array();
    for (const auto& decl : result.declarations) {
        nlohmann::json d = {
            {"symbol_name", decl.symbol_name}, {"qualified_name", decl.qualified_name},
            {"kind", to_string(decl.kind)},    {"start_byte", decl.range.start},
            {"end_byte", decl.range.end},      {"is_definition", decl.is_definition},
        };
        if (decl.enclosing_scope.has_value()) {
            d["enclosing_scope"] = *decl.enclosing_scope;
        }
        decls_json.push_back(std::move(d));
    }

    nlohmann::json occs_json = nlohmann::json::array();
    for (const auto& occ : result.occurrences) {
        nlohmann::json o = {
            {"kind", to_string(occ.kind)},
            {"written_name", occ.written_name},
            {"start_byte", occ.range.start},
            {"end_byte", occ.range.end},
            {"candidate_targets", occ.candidate_targets},
        };
        if (occ.enclosing_scope.has_value()) {
            o["enclosing_scope"] = *occ.enclosing_scope;
        }
        occs_json.push_back(std::move(o));
    }

    nlohmann::json diags_json = nlohmann::json::array();
    for (const auto& diag : result.diagnostics) {
        diags_json.push_back({
            {"severity", to_string(diag.severity)},
            {"code", diag.code},
            {"message", diag.message},
            {"start_byte", diag.byte_range.start},
            {"end_byte", diag.byte_range.end},
        });
    }

    return nlohmann::json{
        {"language", to_string(result.language)}, {"status", to_string(result.status)},
        {"symbols", std::move(symbols_json)},     {"declarations", std::move(decls_json)},
        {"occurrences", std::move(occs_json)},    {"diagnostics", std::move(diags_json)},
    };
}

struct ComparisonResult {
    bool matches = true;
    std::string diff;

    explicit operator bool() const noexcept { return matches; }
};

inline ComparisonResult compare_golden(const adapters::AdapterResult& actual,
                                       const nlohmann::json& expected) {
    nlohmann::json actual_json = adapter_result_to_json(actual);

    ComparisonResult result;
    if (actual_json == expected) {
        result.matches = true;
        return result;
    }

    result.matches = false;
    std::ostringstream ss;
    ss << "Golden mismatch:\n";

    // Compare fields individually for clear error reporting
    for (const auto& key : {"language", "status"}) {
        if (actual_json.contains(key) && expected.contains(key)) {
            if (actual_json[key] != expected[key]) {
                ss << "  Field '" << key << "' mismatch: expected '" << expected[key] << "', got '"
                   << actual_json[key] << "'\n";
            }
        }
    }

    for (const auto& arr_key : {"symbols", "declarations", "occurrences", "diagnostics"}) {
        if (actual_json.contains(arr_key) && expected.contains(arr_key)) {
            const auto& act_arr = actual_json[arr_key];
            const auto& exp_arr = expected[arr_key];
            if (act_arr.size() != exp_arr.size()) {
                ss << "  Array '" << arr_key << "' size mismatch: expected " << exp_arr.size()
                   << ", got " << act_arr.size() << "\n";
            } else if (act_arr != exp_arr) {
                ss << "  Array '" << arr_key << "' element content mismatch.\n";
            }
        }
    }

    ss << "\nActual JSON:\n" << actual_json.dump(2) << "\n";
    ss << "\nExpected JSON:\n" << expected.dump(2) << "\n";
    result.diff = ss.str();
    return result;
}

inline ComparisonResult compare_golden_file(const adapters::AdapterResult& actual,
                                            const std::filesystem::path& golden_path) {
    if (!std::filesystem::exists(golden_path)) {
        ComparisonResult res;
        res.matches = false;
        res.diff = "Golden file not found: " + golden_path.string();
        return res;
    }

    std::ifstream file(golden_path);
    if (!file.is_open()) {
        ComparisonResult res;
        res.matches = false;
        res.diff = "Failed to open golden file: " + golden_path.string();
        return res;
    }

    nlohmann::json expected;
    try {
        file >> expected;
    } catch (const std::exception& e) {
        ComparisonResult res;
        res.matches = false;
        res.diff = "Failed to parse golden JSON: " + std::string(e.what());
        return res;
    }

    return compare_golden(actual, expected);
}

} // namespace codelenses
