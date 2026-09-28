#include "codelenses/resolver/symbol_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace codelenses::resolver {

namespace {

std::optional<uint32_t> extract_signature_arity(std::string_view sig) {
    auto lparen = sig.find('(');
    auto rparen = sig.rfind(')');
    if (lparen == std::string_view::npos || rparen == std::string_view::npos || rparen <= lparen) {
        return std::nullopt;
    }

    std::string_view params = sig.substr(lparen + 1, rparen - lparen - 1);
    while (!params.empty() && std::isspace(static_cast<unsigned char>(params.front()))) {
        params.remove_prefix(1);
    }
    while (!params.empty() && std::isspace(static_cast<unsigned char>(params.back()))) {
        params.remove_suffix(1);
    }

    if (params.empty() || params == "void") {
        return 0;
    }

    uint32_t count = 1;
    int angle_depth = 0;
    int paren_depth = 0;
    for (char c : params) {
        if (c == '<') {
            angle_depth++;
        } else if (c == '>') {
            angle_depth = std::max(0, angle_depth - 1);
        } else if (c == '(') {
            paren_depth++;
        } else if (c == ')') {
            paren_depth = std::max(0, paren_depth - 1);
        } else if (c == ',' && angle_depth == 0 && paren_depth == 0) {
            count++;
        }
    }
    return count;
}

bool is_interface_or_abstract_candidate(const SymbolCandidate* s) {
    if (s->signature.has_value()) {
        if (s->signature->find("= 0") != std::string::npos) return true;
        if (s->signature->find("virtual") != std::string::npos &&
            s->signature->find("override") == std::string::npos) return true;
    }
    if (s->qualified_name.has_value()) {
        if (s->qualified_name->find("::I") != std::string::npos ||
            s->qualified_name->starts_with("I")) return true;
    }
    if (s->enclosing_scope.has_value()) {
        if (s->enclosing_scope->find("::I") != std::string::npos ||
            s->enclosing_scope->starts_with("I")) return true;
    }
    return false;
}

} // namespace

void SymbolResolver::add_symbol(const SymbolCandidate& candidate) {
    if (symbol_id_to_index_.contains(candidate.symbol_id)) {
        return;
    }

    std::size_t idx = symbols_.size();
    symbols_.push_back(candidate);
    symbol_id_to_index_[candidate.symbol_id] = idx;
    symbols_by_name_[candidate.name].push_back(idx);

    if (candidate.qualified_name.has_value() && !candidate.qualified_name->empty()) {
        symbols_by_qname_[*candidate.qualified_name].push_back(idx);
    }
    symbols_by_file_id_[candidate.file_id].push_back(idx);

    if (!candidate.symbol_key.empty()) {
        const auto owner_key = std::to_string(candidate.owner_workspace_id) + "\x1f" +
                               candidate.symbol_key;
        symbol_by_owner_key_[owner_key] = idx;
    }
}

void SymbolResolver::add_symbol(const Symbol& symbol, const std::string& file_path) {
    add_symbol(symbol, file_path, 0);
}

void SymbolResolver::add_symbol(const Symbol& symbol, const std::string& file_path,
                                int64_t owner_workspace_id) {
    SymbolCandidate cand{
        .symbol_id = symbol.id,
        .file_id = symbol.file_id,
        .file_path = file_path,
        .symbol_key = symbol.symbol_key,
        .name = symbol.name,
        .qualified_name = symbol.qualified_name,
        .kind = symbol.kind,
        .language = symbol.language,
        .signature = symbol.signature,
        .enclosing_scope = symbol.container_name,
        .range = symbol.range,
        .is_definition = symbol.is_definition,
        .scope_distance = 0,
        .owner_workspace_id = owner_workspace_id,
    };
    add_symbol(cand);
}

void SymbolResolver::clear() {
    symbols_.clear();
    symbol_id_to_index_.clear();
    symbols_by_name_.clear();
    symbols_by_qname_.clear();
    symbols_by_file_id_.clear();
    symbol_by_owner_key_.clear();
}

const SymbolCandidate* SymbolResolver::find_symbol_by_id(int64_t id) const {
    auto it = symbol_id_to_index_.find(id);
    if (it != symbol_id_to_index_.end()) {
        return &symbols_[it->second];
    }
    return nullptr;
}

const SymbolCandidate* SymbolResolver::find_symbol_by_key(std::string_view key,
                                                          int64_t owner_workspace_id) const {
    const auto owner_key = std::to_string(owner_workspace_id) + "\x1f" + std::string(key);
    auto it = symbol_by_owner_key_.find(owner_key);
    if (it != symbol_by_owner_key_.end()) {
        return &symbols_[it->second];
    }
    return nullptr;
}

std::vector<const SymbolCandidate*>
SymbolResolver::find_symbols_by_name(std::string_view name) const {
    std::vector<const SymbolCandidate*> results;
    auto it = symbols_by_name_.find(std::string(name));
    if (it != symbols_by_name_.end()) {
        results.reserve(it->second.size());
        for (std::size_t idx : it->second) {
            results.push_back(&symbols_[idx]);
        }
    }
    return results;
}

std::vector<const SymbolCandidate*>
SymbolResolver::find_symbols_by_qualified_name(std::string_view qname) const {
    std::vector<const SymbolCandidate*> results;
    auto it = symbols_by_qname_.find(std::string(qname));
    if (it != symbols_by_qname_.end()) {
        results.reserve(it->second.size());
        for (std::size_t idx : it->second) {
            results.push_back(&symbols_[idx]);
        }
    }
    return results;
}

std::optional<int64_t> SymbolResolver::find_enclosing_symbol_id(int64_t file_id,
                                                                const SourceRange& range) const {
    auto it = symbols_by_file_id_.find(file_id);
    if (it == symbols_by_file_id_.end()) {
        return std::nullopt;
    }

    std::optional<int64_t> best_id;
    int64_t smallest_len = std::numeric_limits<int64_t>::max();

    for (std::size_t idx : it->second) {
        const auto& sym = symbols_[idx];
        if (sym.range.start_byte <= range.start_byte && range.end_byte <= sym.range.end_byte) {
            int64_t len = sym.range.end_byte - sym.range.start_byte;
            if (len < smallest_len) {
                smallest_len = len;
                best_id = sym.symbol_id;
            }
        }
    }
    return best_id;
}

ResolvedOccurrence
SymbolResolver::resolve_occurrence(const Occurrence& occ, const std::string& file_path,
                                   Language lang,
                                   const std::vector<int64_t>& imported_file_ids) const {
    ResolvedOccurrence res;
    res.id = occ.id;
    res.file_id = occ.file_id;
    res.file_path = file_path;
    res.enclosing_symbol_id = find_enclosing_symbol_id(occ.file_id, occ.range);
    res.written_name = occ.name;
    res.range = occ.range;
    res.enclosing_scope = std::nullopt;
    res.resolution = Resolution::unresolved;
    res.confidence = 0.0;
    res.occurrence_kind = occ.occurrence_kind;

    if (occ.name.empty()) {
        return res;
    }

    std::string_view written = occ.name;
    const bool is_explicit_qual = (written.find("::") != std::string_view::npos ||
                                   written.find('.') != std::string_view::npos);

    // Explicit qualification resolution
    if (is_explicit_qual) {
        std::string written_str(written);
        if (written_str.starts_with("::")) {
            written_str = written_str.substr(2);
        }

        std::vector<const SymbolCandidate*> matches;
        auto q_it = symbols_by_qname_.find(written_str);
        if (q_it != symbols_by_qname_.end()) {
            for (std::size_t idx : q_it->second) {
                const auto& sym = symbols_[idx];
                auto sym_lang = language_from_string(sym.language).value_or(Language::unknown);
                if (languages_compatible(lang, sym_lang)) {
                    // Visibility filtering: library symbols are visible only if their file is in imported_file_ids!
                    if (sym.owner_workspace_id != 0) {
                        if (std::find(imported_file_ids.begin(), imported_file_ids.end(),
                                      sym.file_id) == imported_file_ids.end()) {
                            continue;
                        }
                    }
                    matches.push_back(&sym);
                }
            }
        }

        if (matches.size() > 1) {
            std::vector<const SymbolCandidate*> project_matches;
            for (const auto* m : matches) {
                if (m->owner_workspace_id == 0) {
                    project_matches.push_back(m);
                }
            }
            if (!project_matches.empty()) {
                matches = std::move(project_matches);
            }
        }

        if (matches.size() == 1) {
            res.resolution = Resolution::resolved;
            res.confidence = 1.0;
            res.candidates.push_back(CandidateTarget{
                .target_symbol_id = matches[0]->symbol_id,
                .target_symbol_key = matches[0]->symbol_key,
                .target_file_path = matches[0]->file_path,
                .rank = 1,
                .confidence = 1.0,
                .reason = "explicit_qualification",
                .owner_workspace_id = (matches[0]->owner_workspace_id != 0)
                                          ? std::make_optional(matches[0]->owner_workspace_id)
                                          : std::nullopt,
                .target_file_id = matches[0]->file_id,
            });
        } else if (matches.size() > 1) {
            res.resolution = Resolution::ambiguous;
            res.confidence = 0.5;
            for (const auto* m : matches) {
                res.candidates.push_back(CandidateTarget{
                    .target_symbol_id = m->symbol_id,
                    .target_symbol_key = m->symbol_key,
                    .target_file_path = m->file_path,
                    .rank = 0,
                    .confidence = 0.5,
                    .reason = "overload",
                    .owner_workspace_id = (m->owner_workspace_id != 0)
                                              ? std::make_optional(m->owner_workspace_id)
                                              : std::nullopt,
                    .target_file_id = m->file_id,
                });
            }
        } else {
            res.resolution = Resolution::unresolved;
            res.confidence = 0.0;
        }
        return res;
    }

    // Unqualified name resolution
    std::string_view delim = scope_delimiter(lang);

    std::optional<uint32_t> call_arity = std::nullopt;
    std::optional<std::string> call_receiver = std::nullopt;
    if (occ.metadata_json.has_value() && !occ.metadata_json->empty()) {
        try {
            auto j = nlohmann::json::parse(*occ.metadata_json);
            if (j.contains("arity") && j["arity"].is_number_integer()) {
                call_arity = j["arity"].get<uint32_t>();
            }
            if (j.contains("receiver") && j["receiver"].is_string()) {
                call_receiver = j["receiver"].get<std::string>();
            }
        } catch (...) {}
    }
    bool is_foreign_receiver =
        (call_receiver.has_value() && !call_receiver->empty() && *call_receiver != "this");

    // Determine occurrence's enclosing scope from enclosing symbol if not explicitly given
    std::optional<std::string> enc_scope;
    if (res.enclosing_symbol_id.has_value()) {
        if (const auto* enc_sym = find_symbol_by_id(*res.enclosing_symbol_id)) {
            if (enc_sym->qualified_name.has_value() && !enc_sym->qualified_name->empty()) {
                enc_scope = enc_sym->qualified_name;
            } else if (!enc_sym->name.empty()) {
                enc_scope = enc_sym->name;
            }
        }
    }
    res.enclosing_scope = enc_scope;

    auto scopes = get_scope_hierarchy(
        enc_scope.has_value() ? std::optional<std::string_view>(*enc_scope) : std::nullopt, delim);

    // Step A: Same-file lexical scope resolution with innermost shadowing (E-03)
    for (const auto& scope : scopes) {
        std::vector<const SymbolCandidate*> same_file_matches;
        auto it_f = symbols_by_file_id_.find(occ.file_id);
        if (it_f != symbols_by_file_id_.end()) {
            for (std::size_t idx : it_f->second) {
                const auto& sym = symbols_[idx];
                if (sym.name == written) {
                    if (is_foreign_receiver && (sym.kind == "method" || sym.kind == "function")) {
                        if (!sym.enclosing_scope.has_value() || *sym.enclosing_scope != *call_receiver) {
                            continue;
                        }
                    }
                    if (call_arity.has_value() && sym.signature.has_value()) {
                        auto cand_arity = extract_signature_arity(*sym.signature);
                        if (cand_arity.has_value() && *cand_arity != *call_arity) {
                            continue;
                        }
                    }
                    bool in_scope = false;
                    if (scope.empty()) {
                        in_scope =
                            (!sym.enclosing_scope.has_value() || sym.enclosing_scope->empty());
                    } else {
                        in_scope =
                            (sym.enclosing_scope.has_value() && *sym.enclosing_scope == scope);
                    }
                    if (in_scope) {
                        same_file_matches.push_back(&sym);
                    }
                }
            }
        }

        if (!same_file_matches.empty()) {
            if (same_file_matches.size() == 1) {
                res.resolution = Resolution::resolved;
                res.confidence = 1.0;
                res.candidates.push_back(CandidateTarget{
                    .target_symbol_id = same_file_matches[0]->symbol_id,
                    .target_symbol_key = same_file_matches[0]->symbol_key,
                    .target_file_path = same_file_matches[0]->file_path,
                    .rank = 1,
                    .confidence = 1.0,
                    .reason = "lexical_scope",
                });
            } else {
                res.resolution = Resolution::ambiguous;
                res.confidence = 0.5;
                for (const auto* m : same_file_matches) {
                    res.candidates.push_back(CandidateTarget{
                        .target_symbol_id = m->symbol_id,
                        .target_symbol_key = m->symbol_key,
                        .target_file_path = m->file_path,
                        .rank = 0,
                        .confidence = 0.5,
                        .reason = "overload",
                    });
                }
            }
            return res; // Shadowing: do not check outer scopes or other files!
        }
    }

    // Step B: Same-file any scope fallback
    std::vector<const SymbolCandidate*> same_file_any;
    auto it_f = symbols_by_file_id_.find(occ.file_id);
    if (it_f != symbols_by_file_id_.end()) {
        for (std::size_t idx : it_f->second) {
            const auto& sym = symbols_[idx];
            if (sym.name == written) {
                if (is_foreign_receiver && (sym.kind == "method" || sym.kind == "function")) {
                    if (!sym.enclosing_scope.has_value() || *sym.enclosing_scope != *call_receiver) {
                        continue;
                    }
                }
                if (call_arity.has_value() && sym.signature.has_value()) {
                    auto cand_arity = extract_signature_arity(*sym.signature);
                    if (cand_arity.has_value() && *cand_arity != *call_arity) {
                        continue;
                    }
                }
                same_file_any.push_back(&sym);
            }
        }
    }
    if (!same_file_any.empty()) {
        if (same_file_any.size() == 1) {
            res.resolution = Resolution::resolved;
            res.confidence = 1.0;
            res.candidates.push_back(CandidateTarget{
                .target_symbol_id = same_file_any[0]->symbol_id,
                .target_symbol_key = same_file_any[0]->symbol_key,
                .target_file_path = same_file_any[0]->file_path,
                .rank = 1,
                .confidence = 1.0,
                .reason = "same_file",
            });
        } else {
            res.resolution = Resolution::ambiguous;
            res.confidence = 0.5;
            for (const auto* m : same_file_any) {
                res.candidates.push_back(CandidateTarget{
                    .target_symbol_id = m->symbol_id,
                    .target_symbol_key = m->symbol_key,
                    .target_file_path = m->file_path,
                    .rank = 0,
                    .confidence = 0.5,
                    .reason = "overload",
                });
            }
        }
        return res;
    }

    // Step C: Cross-file resolution via imported dependencies (E-05)
    if (!imported_file_ids.empty()) {
        std::vector<const SymbolCandidate*> imported_matches;
        for (int64_t imp_fid : imported_file_ids) {
            auto it_imp = symbols_by_file_id_.find(imp_fid);
            if (it_imp != symbols_by_file_id_.end()) {
                for (std::size_t idx : it_imp->second) {
                    const auto& sym = symbols_[idx];
                    if (sym.name == written) {
                        if (call_arity.has_value() && sym.signature.has_value()) {
                            auto cand_arity = extract_signature_arity(*sym.signature);
                            if (cand_arity.has_value() && *cand_arity != *call_arity) {
                                continue;
                            }
                        }
                        imported_matches.push_back(&sym);
                    }
                }
            }
        }
        if (!imported_matches.empty()) {
            std::stable_sort(imported_matches.begin(), imported_matches.end(),
                             [](const SymbolCandidate* a, const SymbolCandidate* b) {
                                 bool a_iface = is_interface_or_abstract_candidate(a);
                                 bool b_iface = is_interface_or_abstract_candidate(b);
                                 if (a_iface != b_iface) return a_iface;
                                 return false;
                             });
            if (imported_matches.size() == 1) {
                res.resolution = Resolution::resolved;
                res.confidence = 0.9;
                res.candidates.push_back(CandidateTarget{
                    .target_symbol_id = imported_matches[0]->symbol_id,
                    .target_symbol_key = imported_matches[0]->symbol_key,
                    .target_file_path = imported_matches[0]->file_path,
                    .rank = 1,
                    .confidence = 0.9,
                    .reason = "import",
                    .owner_workspace_id = (imported_matches[0]->owner_workspace_id != 0)
                                              ? std::make_optional(imported_matches[0]->owner_workspace_id)
                                              : std::nullopt,
                    .target_file_id = imported_matches[0]->file_id,
                });
            } else {
                res.resolution = Resolution::ambiguous;
                res.confidence = 0.5;
                for (size_t mi = 0; mi < imported_matches.size(); ++mi) {
                    const auto* m = imported_matches[mi];
                    res.candidates.push_back(CandidateTarget{
                        .target_symbol_id = m->symbol_id,
                        .target_symbol_key = m->symbol_key,
                        .target_file_path = m->file_path,
                        .rank = (mi == 0) ? 1 : 0,
                        .confidence = 0.5,
                        .reason = "overload",
                        .owner_workspace_id = (m->owner_workspace_id != 0)
                                                  ? std::make_optional(m->owner_workspace_id)
                                                  : std::nullopt,
                        .target_file_id = m->file_id,
                    });
                }
            }
            return res;
        }
    }

    // Step D: Workspace-wide cross-file fallback (E-05)
    std::vector<const SymbolCandidate*> global_matches;
    auto it_name = symbols_by_name_.find(std::string(written));
    if (it_name != symbols_by_name_.end()) {
        for (std::size_t idx : it_name->second) {
            const auto& sym = symbols_[idx];
            // Library symbols must NEVER match in workspace fallback without include evidence!
            if (sym.owner_workspace_id != 0) {
                continue;
            }
            auto sym_lang = language_from_string(sym.language).value_or(Language::unknown);
            if (languages_compatible(lang, sym_lang)) {
                if (call_arity.has_value() && sym.signature.has_value()) {
                    auto cand_arity = extract_signature_arity(*sym.signature);
                    if (cand_arity.has_value() && *cand_arity != *call_arity) {
                        continue;
                    }
                }
                global_matches.push_back(&sym);
            }
        }
    }

    if (!global_matches.empty()) {
        std::stable_sort(global_matches.begin(), global_matches.end(),
                         [](const SymbolCandidate* a, const SymbolCandidate* b) {
                             bool a_iface = is_interface_or_abstract_candidate(a);
                             bool b_iface = is_interface_or_abstract_candidate(b);
                             if (a_iface != b_iface) return a_iface;
                             return false;
                         });
        if (global_matches.size() == 1) {
            res.resolution = Resolution::resolved;
            res.confidence = 0.8;
            res.candidates.push_back(CandidateTarget{
                .target_symbol_id = global_matches[0]->symbol_id,
                .target_symbol_key = global_matches[0]->symbol_key,
                .target_file_path = global_matches[0]->file_path,
                .rank = 1,
                .confidence = 0.8,
                .reason = "workspace_fallback",
                .owner_workspace_id = std::nullopt,
                .target_file_id = global_matches[0]->file_id,
            });
        } else {
            res.resolution = Resolution::ambiguous;
            res.confidence = 0.5;
            for (size_t mi = 0; mi < global_matches.size(); ++mi) {
                const auto* m = global_matches[mi];
                res.candidates.push_back(CandidateTarget{
                    .target_symbol_id = m->symbol_id,
                    .target_symbol_key = m->symbol_key,
                    .target_file_path = m->file_path,
                    .rank = (mi == 0) ? 1 : 0,
                    .confidence = 0.5,
                    .reason = "ambiguous_name",
                    .owner_workspace_id = std::nullopt,
                    .target_file_id = m->file_id,
                });
            }
        }
        return res;
    }

    // Step E: Unresolved
    res.resolution = Resolution::unresolved;
    res.confidence = 0.0;
    return res;
}

ResolvedOccurrence SymbolResolver::resolve_csharp_occurrence(
    const Occurrence& occ, const std::string& file_path,
    const std::vector<CSharpImportDirective>& imports,
    const std::vector<int64_t>& visible_library_owners) const {
    ResolvedOccurrence res{
        .id = occ.id,
        .file_id = occ.file_id,
        .file_path = file_path,
        .enclosing_symbol_id = find_enclosing_symbol_id(occ.file_id, occ.range),
        .written_name = occ.name,
        .range = occ.range,
        .resolution = Resolution::unresolved,
        .confidence = 0.0,
        .occurrence_kind = occ.occurrence_kind,
    };
    if (occ.name.empty()) return res;

    auto normalize = [](std::string value) {
        if (value.starts_with("global::")) value.erase(0, 8);
        for (std::size_t pos = value.find("::"); pos != std::string::npos;
             pos = value.find("::", pos)) {
            value.replace(pos, 2, ".");
            ++pos;
        }
        std::string result;
        int generic_depth = 0;
        for (char c : value) {
            if (c == '<') {
                ++generic_depth;
            } else if (c == '>' && generic_depth > 0) {
                --generic_depth;
            } else if (generic_depth == 0 && !std::isspace(static_cast<unsigned char>(c))) {
                result += c;
            }
        }
        while (!result.empty() && result.front() == '.') result.erase(result.begin());
        while (!result.empty() && (result.back() == '?' || result.back() == '*'))
            result.pop_back();
        while (result.ends_with("[]")) result.resize(result.size() - 2);
        return result;
    };
    const std::string written = normalize(occ.name);
    if (written.empty()) return res;

    auto owner_visible = [&](const SymbolCandidate& sym) {
        return sym.owner_workspace_id == 0 ||
               std::find(visible_library_owners.begin(), visible_library_owners.end(),
                         sym.owner_workspace_id) != visible_library_owners.end();
    };
    auto csharp_symbol = [](const SymbolCandidate& sym) {
        return language_from_string(sym.language).value_or(Language::unknown) == Language::csharp;
    };
    auto unique_matches = [](std::vector<const SymbolCandidate*>& matches) {
        std::sort(matches.begin(), matches.end(), [](const auto* a, const auto* b) {
            return a->symbol_id < b->symbol_id;
        });
        matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    };
    auto candidate_for = [](const SymbolCandidate& sym, std::string reason, double confidence) {
        return CandidateTarget{
            .target_symbol_id = sym.symbol_id,
            .target_symbol_key = sym.symbol_key,
            .target_file_path = sym.file_path,
            .rank = 1,
            .confidence = confidence,
            .reason = std::move(reason),
            .owner_workspace_id = sym.owner_workspace_id != 0
                                      ? std::make_optional(sym.owner_workspace_id)
                                      : std::nullopt,
            .target_file_id = sym.file_id,
        };
    };
    auto emit = [&](std::vector<const SymbolCandidate*> matches, std::string reason,
                    double confidence = 0.9) {
        unique_matches(matches);
        if (matches.empty()) return false;
        bool has_project = std::any_of(matches.begin(), matches.end(), [](const auto* sym) {
            return sym->owner_workspace_id == 0;
        });
        if (has_project) {
            std::erase_if(matches, [](const auto* sym) { return sym->owner_workspace_id != 0; });
        }
        if (matches.size() == 1) {
            res.resolution = Resolution::resolved;
            res.confidence = confidence;
            res.candidates.push_back(candidate_for(*matches.front(), std::move(reason), confidence));
        } else {
            res.resolution = Resolution::ambiguous;
            res.confidence = 0.5;
            for (const auto* sym : matches) {
                auto candidate = candidate_for(*sym, "csharp_ambiguous", 0.5);
                candidate.rank = 0;
                res.candidates.push_back(std::move(candidate));
            }
        }
        return true;
    };
    auto qualified_matches = [&](std::string_view qname) {
        std::vector<const SymbolCandidate*> matches;
        auto it = symbols_by_qname_.find(std::string(qname));
        if (it != symbols_by_qname_.end()) {
            for (std::size_t idx : it->second) {
                const auto& sym = symbols_[idx];
                if (csharp_symbol(sym) && owner_visible(sym)) matches.push_back(&sym);
            }
        }
        return matches;
    };

    if (occ.occurrence_kind == "import") {
        for (const auto& imp : imports) {
            if (imp.file_id != occ.file_id || normalize(imp.target) != written) continue;
            if (emit(qualified_matches(written), "csharp_using_target")) return res;
        }
    }

    std::string current_qualified_scope;
    if (occ.metadata_json) {
        try {
            auto metadata = nlohmann::json::parse(*occ.metadata_json);
            if (metadata.contains("enclosingScope") && metadata["enclosingScope"].is_string()) {
                current_qualified_scope = metadata["enclosingScope"].get<std::string>();
                auto file_symbols = symbols_by_file_id_.find(occ.file_id);
                if (file_symbols != symbols_by_file_id_.end()) {
                    for (auto idx : file_symbols->second) {
                        const auto& sym = symbols_[idx];
                        if (sym.qualified_name && *sym.qualified_name == current_qualified_scope) {
                            res.enclosing_symbol_id = sym.symbol_id;
                            break;
                        }
                    }
                }
            }
        } catch (...) {
        }
    }
    if (current_qualified_scope.empty() && res.enclosing_symbol_id) {
        if (const auto* enclosing = find_symbol_by_id(*res.enclosing_symbol_id)) {
            if (enclosing->qualified_name) current_qualified_scope = *enclosing->qualified_name;
            else current_qualified_scope = enclosing->name;
        }
    }
    if (!current_qualified_scope.empty()) res.enclosing_scope = current_qualified_scope;
    std::string current_namespace;
    for (const auto& sym : symbols_) {
        if (!csharp_symbol(sym) || !owner_visible(sym) || sym.kind != "namespace" ||
            !sym.qualified_name || sym.qualified_name->empty()) continue;
        const auto& qname = *sym.qualified_name;
        if ((current_qualified_scope == qname ||
             (current_qualified_scope.starts_with(qname) &&
              current_qualified_scope.size() > qname.size() &&
              current_qualified_scope[qname.size()] == '.')) &&
            qname.size() > current_namespace.size()) {
            current_namespace = qname;
        }
    }

    const bool explicitly_qualified = written.find('.') != std::string::npos;
    if (explicitly_qualified) {
        std::string direct = written;
        for (const auto& imp : imports) {
            const bool in_scope = imp.is_global || imp.file_id == occ.file_id;
            if (!in_scope || imp.kind != "alias" || !imp.alias ||
                (direct != *imp.alias && !direct.starts_with(*imp.alias + "."))) continue;
            const bool namespace_scope = !imp.scope || imp.scope->empty() ||
                                         current_namespace == *imp.scope ||
                                         current_namespace.starts_with(*imp.scope + ".");
            if (!namespace_scope) continue;
            direct.replace(0, imp.alias->size(), normalize(imp.target));
            break;
        }
        if (emit(qualified_matches(direct), "csharp_qualified", 1.0)) return res;
    }

    const auto dot = written.rfind('.');
    const std::string short_name =
        dot == std::string::npos ? written : written.substr(dot + 1);

    // Same-file lexical declarations retain C# shadowing order.
    std::vector<std::string> lexical_scopes;
    if (!current_qualified_scope.empty()) {
        lexical_scopes.push_back(current_qualified_scope);
        std::string scope = current_qualified_scope;
        while (true) {
            auto pos = scope.rfind('.');
            if (pos == std::string::npos) break;
            scope.resize(pos);
            lexical_scopes.push_back(scope);
        }
    }
    for (const auto& scope : lexical_scopes) {
        std::vector<const SymbolCandidate*> matches;
        auto it = symbols_by_file_id_.find(occ.file_id);
        if (it != symbols_by_file_id_.end()) {
            for (auto idx : it->second) {
                const auto& sym = symbols_[idx];
                if (sym.name == short_name && sym.enclosing_scope &&
                    *sym.enclosing_scope == scope && owner_visible(sym) && csharp_symbol(sym)) {
                    matches.push_back(&sym);
                }
            }
        }
        if (emit(std::move(matches), "csharp_lexical", 1.0)) return res;
    }

    // Types and nested namespaces in the current namespace are visible across files.
    if (!current_namespace.empty() && !explicitly_qualified) {
        auto matches = qualified_matches(current_namespace + "." + short_name);
        std::erase_if(matches, [](const auto* sym) {
            return sym->kind != "class" && sym->kind != "struct" && sym->kind != "interface" &&
                   sym->kind != "enum" && sym->kind != "type_alias" && sym->kind != "namespace";
        });
        if (emit(std::move(matches), "csharp_same_namespace")) return res;
    }

    std::vector<const CSharpImportDirective*> applicable;
    for (const auto& imp : imports) {
        if (!imp.is_global && imp.file_id != occ.file_id) continue;
        if (imp.scope && !imp.scope->empty() && current_namespace != *imp.scope &&
            !current_namespace.starts_with(*imp.scope + ".")) continue;
        applicable.push_back(&imp);
    }

    // Aliases are local to their using scope and substitute a namespace or type name.
    for (const auto* imp : applicable) {
        if (imp->kind != "alias" || !imp->alias || short_name != *imp->alias) continue;
        if (emit(qualified_matches(normalize(imp->target)), "csharp_alias")) return res;
    }

    // Namespace and type imports expose direct members by short name.
    for (const auto* imp : applicable) {
        if (imp->kind != "namespace" && imp->kind != "type" &&
            imp->kind != "namespace_or_type") continue;
        std::string target = normalize(imp->target);
        if (!target.empty() && !explicitly_qualified) {
            const auto target_separator = target.rfind('.');
            const std::string target_name = target_separator == std::string::npos
                                                ? target
                                                : target.substr(target_separator + 1);
            if ((imp->kind == "type" || imp->kind == "namespace_or_type") &&
                target_name == short_name) {
                auto imported_type = qualified_matches(target);
                std::erase_if(imported_type, [](const auto* sym) {
                    return sym->kind == "namespace";
                });
                if (emit(std::move(imported_type), "csharp_using_type")) return res;
            }
            if (imp->kind != "type" &&
                emit(qualified_matches(target + "." + short_name), "csharp_using")) return res;
        }
    }

    // using static exposes static members by their unqualified member names.
    for (const auto* imp : applicable) {
        if (imp->kind != "static") continue;
        auto matches = qualified_matches(normalize(imp->target) + "." + short_name);
        if (emit(std::move(matches), "csharp_using_static")) return res;
    }

    return res;
}

ResolvedOccurrence SymbolResolver::resolve_csharp_reference(
    const ReferenceOccurrence& ref, const std::string& file_path,
    const std::vector<CSharpImportDirective>& imports,
    const std::vector<int64_t>& visible_library_owners) const {
    Occurrence occ{
        .id = ref.id,
        .workspace_id = ref.workspace_id,
        .file_id = ref.source_file_id,
        .symbol_id = ref.target_symbol_id,
        .occurrence_kind = ref.reference_kind,
        .name = ref.name,
        .range = ref.range,
        .confidence = ref.confidence,
        .resolution = ref.resolution,
        .metadata_json = ref.metadata_json,
    };
    auto result = resolve_csharp_occurrence(occ, file_path, imports, visible_library_owners);
    if (ref.source_symbol_id) result.enclosing_symbol_id = ref.source_symbol_id;
    return result;
}

ResolvedOccurrence
SymbolResolver::resolve_reference(const ReferenceOccurrence& ref, const std::string& file_path,
                                  Language lang,
                                  const std::vector<int64_t>& imported_file_ids) const {
    Occurrence occ{
        .id = ref.id,
        .workspace_id = ref.workspace_id,
        .file_id = ref.source_file_id,
        .symbol_id = ref.target_symbol_id,
        .occurrence_kind = ref.reference_kind,
        .name = ref.name,
        .range = ref.range,
        .confidence = ref.confidence,
        .resolution = ref.resolution,
        .metadata_json = ref.metadata_json,
    };
    auto res = resolve_occurrence(occ, file_path, lang, imported_file_ids);
    if (ref.source_symbol_id.has_value()) {
        res.enclosing_symbol_id = ref.source_symbol_id;
    }
    return res;
}

} // namespace codelenses::resolver
