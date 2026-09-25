#pragma once

#include <cstdint>
#include <vector>

#include "codelenses/domain/dependency.hpp"
#include "codelenses/domain/occurrence.hpp"
#include "codelenses/domain/reference.hpp"
#include "codelenses/domain/relation.hpp"
#include "codelenses/domain/symbol.hpp"
#include "codelenses/resolver/symbol_resolver.hpp"

namespace codelenses::resolver {

class RelationshipBuilder {
public:
    RelationshipBuilder() = default;

    // Builds all symbol relations for a workspace from symbols, occurrences, references, and
    // dependencies.
    [[nodiscard]] std::vector<SymbolRelation>
    build_relations(int64_t workspace_id, const std::vector<Symbol>& symbols,
                    const std::vector<Occurrence>& occurrences,
                    const std::vector<ReferenceOccurrence>& references,
                    const std::vector<FileDependency>& dependencies,
                    const SymbolResolver& resolver);

    // Builds containment relations (parent contains child).
    [[nodiscard]] std::vector<SymbolRelation>
    build_containment_relations(int64_t workspace_id, const std::vector<Symbol>& symbols,
                                const SymbolResolver& resolver);

    // Builds call relations (caller calls callee).
    [[nodiscard]] std::vector<SymbolRelation>
    build_call_relations(int64_t workspace_id, const std::vector<ReferenceOccurrence>& references,
                         const SymbolResolver& resolver);

    // Builds inheritance and interface implementation relations.
    [[nodiscard]] std::vector<SymbolRelation>
    build_hierarchy_relations(int64_t workspace_id, const std::vector<Occurrence>& occurrences,
                              const std::vector<ReferenceOccurrence>& references,
                              const SymbolResolver& resolver);

    // Builds override relations between derived methods and base methods.
    [[nodiscard]] std::vector<SymbolRelation>
    build_override_relations(int64_t workspace_id, const std::vector<Symbol>& symbols,
                             const std::vector<SymbolRelation>& inheritance_relations,
                             const SymbolResolver& resolver);

    // Builds import and include relations.
    [[nodiscard]] std::vector<SymbolRelation>
    build_import_relations(int64_t workspace_id, const std::vector<Symbol>& symbols,
                           const std::vector<FileDependency>& dependencies,
                           const std::vector<Occurrence>& occurrences);
};

} // namespace codelenses::resolver
