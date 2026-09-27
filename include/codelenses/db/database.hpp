#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "codelenses/db/connection.hpp"
#include "codelenses/db/dependency_repository.hpp"
#include "codelenses/db/diagnostic_repository.hpp"
#include "codelenses/db/file_repository.hpp"
#include "codelenses/db/fts_repository.hpp"
#include "codelenses/db/job_repository.hpp"
#include "codelenses/db/library_repository.hpp"
#include "codelenses/db/migration.hpp"
#include "codelenses/db/occurrence_repository.hpp"
#include "codelenses/db/reference_repository.hpp"
#include "codelenses/db/relation_repository.hpp"
#include "codelenses/db/symbol_repository.hpp"
#include "codelenses/db/transaction.hpp"
#include "codelenses/db/workspace_repository.hpp"

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

struct IndexingCoordinator {
    std::mutex mutex;
    std::condition_variable cv;
    bool job_running{false};
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
    [[nodiscard]] LibraryRepository& libraries() noexcept { return libraries_; }

    [[nodiscard]] std::shared_ptr<IndexingCoordinator> indexing_coordinator() noexcept {
        return indexing_coord_;
    }

    // Section 6: Transactional file replacement
    void replace_file_index(int64_t file_id, const FileIndexData& data);

private:
    std::shared_ptr<IndexingCoordinator> indexing_coord_{std::make_shared<IndexingCoordinator>()};
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
    LibraryRepository libraries_;
};

} // namespace codelenses
