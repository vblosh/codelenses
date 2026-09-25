#pragma once

#include <cstdint>
#include <stop_token>

#include "codelenses/db/database.hpp"
#include "codelenses/result.hpp"

namespace codelenses::resolver {

struct ResolutionStats {
    int64_t dependencies_resolved{0};
    int64_t dependencies_external{0};
    int64_t occurrences_resolved{0};
    int64_t occurrences_ambiguous{0};
    int64_t occurrences_unresolved{0};
    int64_t relations_built{0};
};

class WorkspaceResolver {
public:
    explicit WorkspaceResolver(Database& db);

    // Resolves dependencies, occurrences, references, and builds symbol relations across a
    // workspace.
    [[nodiscard]] Result<ResolutionStats> resolve_workspace(int64_t workspace_id,
                                                            std::stop_token stop = {});

private:
    Database& db_;
};

} // namespace codelenses::resolver
