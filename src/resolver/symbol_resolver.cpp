#include "codelenses/resolver/symbol_resolver.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace codelenses::resolver {

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
        symbol_by_key_[candidate.symbol_key] = idx;
    }
}

void SymbolResolver::add_symbol(const Symbol& symbol, const std::string& file_path) {
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
    };
    add_symbol(cand);
}

void SymbolResolver::clear() {
    symbols_.clear();
    symbol_id_to_index_.clear();
    symbols_by_name_.clear();
    symbols_by_qname_.clear();
    symbols_by_file_id_.clear();
    symbol_by_key_.clear();
}

const SymbolCandidate* SymbolResolver::find_symbol_by_id(int64_t id) const {
    auto it = symbol_id_to_index_.find(id);
    if (it != symbol_id_to_index_.end()) {
        return &symbols_[it->second];
    }
    return nullptr;
}

const SymbolCandidate* SymbolResolver::find_symbol_by_key(std::string_view key) const {
    auto it = symbol_by_key_.find(std::string(key));
    if (it != symbol_by_key_.end()) {
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
                    matches.push_back(&sym);
                }
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
                        imported_matches.push_back(&sym);
                    }
                }
            }
        }
        if (!imported_matches.empty()) {
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
                });
            } else {
                res.resolution = Resolution::ambiguous;
                res.confidence = 0.5;
                for (const auto* m : imported_matches) {
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
    }

    // Step D: Workspace-wide cross-file fallback (E-05)
    std::vector<const SymbolCandidate*> global_matches;
    auto it_name = symbols_by_name_.find(std::string(written));
    if (it_name != symbols_by_name_.end()) {
        for (std::size_t idx : it_name->second) {
            const auto& sym = symbols_[idx];
            auto sym_lang = language_from_string(sym.language).value_or(Language::unknown);
            if (languages_compatible(lang, sym_lang)) {
                global_matches.push_back(&sym);
            }
        }
    }

    if (!global_matches.empty()) {
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
            });
        } else {
            res.resolution = Resolution::ambiguous;
            res.confidence = 0.5;
            for (const auto* m : global_matches) {
                res.candidates.push_back(CandidateTarget{
                    .target_symbol_id = m->symbol_id,
                    .target_symbol_key = m->symbol_key,
                    .target_file_path = m->file_path,
                    .rank = 0,
                    .confidence = 0.5,
                    .reason = "ambiguous_name",
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
