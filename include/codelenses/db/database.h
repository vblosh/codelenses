#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "codelenses/db/connection.h"
#include "codelenses/db/dependency_repository.h"
#include "codelenses/db/diagnostic_repository.h"
#include "codelenses/db/file_repository.h"
#include "codelenses/db/fts_repository.h"
#include "codelenses/db/job_repository.h"
#include "codelenses/db/migration.h"
#include "codelenses/db/occurrence_repository.h"
#include "codelenses/db/reference_repository.h"
#include "codelenses/db/relation_repository.h"
#include "codelenses/db/symbol_repository.h"
#include "codelenses/db/transaction.h"
#include "codelenses/db/workspace_repository.h"

namespace codelenses {

struct FileIndexData {
    int64_t size_bytes = 0;
    int64_t modified_ns = 0;
    std::optional<std::string> content_hash = std::nullopt;
    std::optional<std::string> parse_hash = std::nullopt;
    std::string language = "unknown";
    bool is_binary = false;
    std::optional<int64_t> last_index_job_id = std::nullopt;

    std::vector<Symbol> symbols = {};
    std::vector<Occurrence> occurrences = {};
    std::vector<ReferenceOccurrence> references = {};
    std::vector<SymbolRelation> relations = {};
    std::vector<FileDependency> dependencies = {};
};

class Database {
public:
    static std::unique_ptr<Database> open(const std::string& path, bool apply_migrations = true);
    static std::unique_ptr<Database> open_memory(bool apply_migrations = true);

    explicit Database(std::unique_ptr<Connection> conn);
    ~Database() = default;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = default;
    Database& operator=(Database&&) = default;

    [[nodiscard]] Connection& connection() noexcept { return *conn_; }
    [[nodiscard]] MigrationRunner& migration_runner() noexcept { return migrations_; }

    [[nodiscard]] WorkspaceRepository& workspaces() noexcept { return workspaces_; }
    [[nodiscard]] FileRepository& files() noexcept { return files_; }
    [[nodiscard]] SymbolRepository& symbols() noexcept { return symbols_; }
    [[nodiscard]] OccurrenceRepository& occurrences() noexcept { return occurrences_; }
    [[nodiscard]] ReferenceRepository& references() noexcept { return references_; }
    [[nodiscard]] RelationRepository& relations() noexcept { return relations_; }
    [[nodiscard]] DependencyRepository& dependencies() noexcept { return dependencies_; }
    [[nodiscard]] DiagnosticRepository& diagnostics() noexcept { return diagnostics_; }
    [[nodiscard]] JobRepository& jobs() noexcept { return jobs_; }
    [[nodiscard]] FtsRepository& fts() noexcept { return fts_; }

    // Section 6: Transactional file replacement
    void replace_file_index(int64_t file_id, const FileIndexData& data);

private:
    std::unique_ptr<Connection> conn_;
    MigrationRunner migrations_;
    WorkspaceRepository workspaces_;
    FileRepository files_;
    SymbolRepository symbols_;
    OccurrenceRepository occurrences_;
    ReferenceRepository references_;
    RelationRepository relations_;
    DependencyRepository dependencies_;
    DiagnosticRepository diagnostics_;
    JobRepository jobs_;
    FtsRepository fts_;
};

} // namespace codelenses
