#include "codelenses/resolver/relationship_builder.hpp"

#include <algorithm>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace codelenses::resolver {

namespace {

// Uniquely identifies a directed relation graph edge (source -> target / target_name).
// Multiple occurrences of identical unresolved relations (e.g. repeated calls to the same
// unresolved function from the same caller) are intentionally deduplicated into a single graph
// edge.
using RelationKey = std::tuple<int64_t, int64_t, std::string, std::string>;

RelationKey make_key(const SymbolRelation& rel) {
    return {
        rel.source_symbol_id,
        rel.target_symbol_id.value_or(0),
        rel.relation_kind,
        rel.target_name.value_or(""),
    };
}

} // namespace

std::vector<SymbolRelation> RelationshipBuilder::build_relations(
    int64_t workspace_id, const std::vector<Symbol>& symbols,
    const std::vector<Occurrence>& occurrences, const std::vector<ReferenceOccurrence>& references,
    const std::vector<FileDependency>& dependencies, const SymbolResolver& resolver) {
    std::vector<SymbolRelation> all_relations;
    std::set<RelationKey> seen;

    auto add_unique = [&](SymbolRelation rel) {
        auto key = make_key(rel);
        if (seen.insert(key).second) {
            all_relations.push_back(std::move(rel));
        }
    };

    // 1. Containment relations (contains)
    for (auto& rel : build_containment_relations(workspace_id, symbols, resolver)) {
        add_unique(std::move(rel));
    }

    // 2. Call relations (calls)
    for (auto& rel : build_call_relations(workspace_id, references, resolver)) {
        add_unique(std::move(rel));
    }

    // 3. Inheritance & Implementation relations (inherits, implements)
    auto hierarchy = build_hierarchy_relations(workspace_id, occurrences, references, resolver);
    for (const auto& rel : hierarchy) {
        add_unique(rel);
    }

    // 4. Overrides relations (overrides)
    for (auto& rel : build_override_relations(workspace_id, symbols, hierarchy, resolver)) {
        add_unique(std::move(rel));
    }

    // 5. Imports and includes relations (imports, includes)
    for (auto& rel : build_import_relations(workspace_id, symbols, dependencies, occurrences)) {
        add_unique(std::move(rel));
    }

    return all_relations;
}

std::vector<SymbolRelation> RelationshipBuilder::build_containment_relations(
    int64_t workspace_id, const std::vector<Symbol>& symbols, const SymbolResolver& resolver) {
    std::vector<SymbolRelation> relations;

    for (const auto& child : symbols) {
        std::optional<int64_t> parent_id = child.scope_symbol_id;

        // If scope_symbol_id is not already set, find parent via range containment or
        // container_name
        if (!parent_id.has_value()) {
            parent_id = resolver.find_enclosing_symbol_id(child.file_id, child.range);
        }

        // Avoid self-containment
        if (parent_id.has_value() && *parent_id != child.id) {
            SymbolRelation rel{
                .workspace_id = workspace_id,
                .source_symbol_id = *parent_id,
                .target_symbol_id = child.id,
                .relation_kind = "contains",
                .target_name = child.name,
                .resolution = "resolved",
                .confidence = 1.0,
            };
            relations.push_back(std::move(rel));
        }
    }

    return relations;
}

std::vector<SymbolRelation>
RelationshipBuilder::build_call_relations(int64_t workspace_id,
                                          const std::vector<ReferenceOccurrence>& references,
                                          const SymbolResolver& resolver) {
    std::vector<SymbolRelation> relations;

    for (const auto& ref : references) {
        if (ref.reference_kind != "call" && ref.reference_kind != "invocation") {
            continue;
        }

        std::optional<int64_t> caller_id = ref.source_symbol_id;
        if (!caller_id.has_value()) {
            caller_id = resolver.find_enclosing_symbol_id(ref.source_file_id, ref.range);
        }

        if (!caller_id.has_value()) {
            continue; // Need a containing caller symbol to form a call relation
        }

        std::vector<int64_t> targets;
        std::unordered_set<int64_t> seen_targets;
        auto add_target = [&](int64_t target_id) {
            if (resolver.find_symbol_by_id(target_id) && seen_targets.insert(target_id).second) {
                targets.push_back(target_id);
            }
        };
        if (ref.target_symbol_id) add_target(*ref.target_symbol_id);

        if (ref.metadata_json && !ref.metadata_json->empty()) {
            try {
                const auto metadata = nlohmann::json::parse(*ref.metadata_json);
                if (metadata.contains("candidates") && metadata["candidates"].is_array()) {
                    for (const auto& candidate : metadata["candidates"]) {
                        if (candidate.is_number_integer()) add_target(candidate.get<int64_t>());
                    }
                }
            } catch (...) {
                // Ignore malformed candidate metadata and retain any resolved target above.
            }
        }

        // Preserve the same-name fallback used by caller queries for unresolved calls.
        if (!ref.target_symbol_id) {
            for (const auto* candidate : resolver.find_symbols_by_name(ref.name)) {
                add_target(candidate->symbol_id);
            }
        }

        auto add_relation = [&](std::optional<int64_t> target_id) {
            std::string target_name = ref.name;
            if (target_id) {
                if (const auto* target_sym = resolver.find_symbol_by_id(*target_id)) {
                    target_name = target_sym->name;
                }
            }
            relations.push_back(SymbolRelation{
                .workspace_id = workspace_id,
                .source_symbol_id = *caller_id,
                .target_symbol_id = target_id,
                .relation_kind = "calls",
                .target_name = target_name,
                .resolution = ref.resolution,
                .confidence = ref.confidence,
            });
        };
        if (targets.empty()) {
            add_relation(std::nullopt);
        } else {
            for (int64_t target_id : targets) add_relation(target_id);
        }
    }

    return relations;
}

std::vector<SymbolRelation> RelationshipBuilder::build_hierarchy_relations(
    int64_t workspace_id, const std::vector<Occurrence>& occurrences,
    const std::vector<ReferenceOccurrence>& references, const SymbolResolver& resolver) {
    std::vector<SymbolRelation> relations;

    // Process occurrences
    for (const auto& occ : occurrences) {
        if (occ.occurrence_kind == "inheritance" || occ.occurrence_kind == "inherits" ||
            occ.occurrence_kind == "implementation" || occ.occurrence_kind == "implements") {

            auto enclosing_id = resolver.find_enclosing_symbol_id(occ.file_id, occ.range);
            if (!enclosing_id && occ.metadata_json) {
                try {
                    const auto metadata = nlohmann::json::parse(*occ.metadata_json);
                    if (metadata.contains("enclosingScope") &&
                        metadata["enclosingScope"].is_string()) {
                        const auto candidates = resolver.find_symbols_by_qualified_name(
                            metadata["enclosingScope"].get<std::string>());
                        auto enclosing = std::find_if(
                            candidates.begin(), candidates.end(), [&](const auto* candidate) {
                                return candidate->file_id == occ.file_id;
                            });
                        if (enclosing != candidates.end()) enclosing_id = (*enclosing)->symbol_id;
                    }
                } catch (...) {
                    // Malformed metadata should not prevent other relations from being built.
                }
            }
            if (!enclosing_id.has_value()) {
                continue;
            }

            std::string kind =
                (occ.occurrence_kind.starts_with("inherit")) ? "inherits" : "implements";

            SymbolRelation rel{
                .workspace_id = workspace_id,
                .source_symbol_id = *enclosing_id,
                .target_symbol_id = occ.symbol_id,
                .relation_kind = kind,
                .target_name = occ.name,
                .resolution = occ.resolution,
                .confidence = occ.confidence,
            };
            relations.push_back(std::move(rel));
        }
    }

    // Process references
    for (const auto& ref : references) {
        if (ref.reference_kind == "inherits" || ref.reference_kind == "inheritance" ||
            ref.reference_kind == "implements" || ref.reference_kind == "implementation") {

            auto enclosing_id = ref.source_symbol_id;
            if (!enclosing_id.has_value()) {
                enclosing_id = resolver.find_enclosing_symbol_id(ref.source_file_id, ref.range);
            }
            if (!enclosing_id.has_value()) {
                continue;
            }

            std::string kind =
                (ref.reference_kind.starts_with("inherit")) ? "inherits" : "implements";

            SymbolRelation rel{
                .workspace_id = workspace_id,
                .source_symbol_id = *enclosing_id,
                .target_symbol_id = ref.target_symbol_id,
                .relation_kind = kind,
                .target_name = ref.name,
                .resolution = ref.resolution,
                .confidence = ref.confidence,
            };
            relations.push_back(std::move(rel));
        }
    }

    return relations;
}

std::vector<SymbolRelation> RelationshipBuilder::build_override_relations(
    int64_t workspace_id, const std::vector<Symbol>& symbols,
    const std::vector<SymbolRelation>& inheritance_relations, const SymbolResolver& resolver) {
    std::vector<SymbolRelation> relations;

    // Derived class ID -> list of base class IDs
    std::unordered_map<int64_t, std::vector<int64_t>> base_types_by_derived;
    for (const auto& rel : inheritance_relations) {
        if ((rel.relation_kind == "inherits" || rel.relation_kind == "implements") &&
            rel.target_symbol_id.has_value()) {
            base_types_by_derived[rel.source_symbol_id].push_back(*rel.target_symbol_id);
        }
    }

    if (base_types_by_derived.empty()) {
        return relations;
    }

    // Group methods by containing class
    std::unordered_map<int64_t, std::vector<const Symbol*>> methods_by_class;
    for (const auto& sym : symbols) {
        if (sym.kind == "method" || sym.kind == "function") {
            auto parent_id = sym.scope_symbol_id;
            if (!parent_id.has_value()) {
                parent_id = resolver.find_enclosing_symbol_id(sym.file_id, sym.range);
            }
            if (parent_id.has_value()) {
                methods_by_class[*parent_id].push_back(&sym);
            }
        }
    }

    // For each derived class with inheritance, check if derived methods override base methods
    for (const auto& [derived_id, base_ids] : base_types_by_derived) {
        auto derived_methods_it = methods_by_class.find(derived_id);
        if (derived_methods_it == methods_by_class.end()) {
            continue;
        }

        for (int64_t base_id : base_ids) {
            auto base_methods_it = methods_by_class.find(base_id);
            if (base_methods_it == methods_by_class.end()) {
                continue;
            }

            for (const auto* derived_m : derived_methods_it->second) {
                for (const auto* base_m : base_methods_it->second) {
                    if (derived_m->name == base_m->name) {
                        SymbolRelation rel{
                            .workspace_id = workspace_id,
                            .source_symbol_id = derived_m->id,
                            .target_symbol_id = base_m->id,
                            .relation_kind = "overrides",
                            .target_name = base_m->name,
                            .resolution = "resolved",
                            .confidence = 1.0,
                        };
                        relations.push_back(std::move(rel));
                    }
                }
            }
        }
    }

    return relations;
}

std::vector<SymbolRelation> RelationshipBuilder::build_import_relations(
    int64_t workspace_id, const std::vector<Symbol>& /*symbols*/,
    const std::vector<FileDependency>& dependencies, const std::vector<Occurrence>& occurrences) {
    std::vector<SymbolRelation> relations;

    // From occurrences with kind "import" or "include" that have a containing symbol
    for (const auto& occ : occurrences) {
        if ((occ.occurrence_kind == "import" || occ.occurrence_kind == "include") &&
            occ.symbol_id.has_value()) {
            SymbolRelation rel{
                .workspace_id = workspace_id,
                .source_symbol_id = *occ.symbol_id,
                .target_symbol_id = std::nullopt,
                .relation_kind = (occ.occurrence_kind == "include") ? "includes" : "imports",
                .target_name = occ.name,
                .resolution = occ.resolution,
                .confidence = occ.confidence,
            };
            relations.push_back(std::move(rel));
        }
    }

    // Suppress unused variable warning for dependencies
    (void)dependencies;

    return relations;
}

} // namespace codelenses::resolver
