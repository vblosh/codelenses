#include "codelenses/db/migration.hpp"

#include <algorithm>
#include <unordered_set>

#include "codelenses/db/connection.hpp"
#include "codelenses/db/error.hpp"
#include "codelenses/db/statement.hpp"
#include "codelenses/db/transaction.hpp"

namespace codelenses {

std::string MigrationRunner::initial_schema_sql() {
    return R"SQL(
-- 2. Workspaces
CREATE TABLE IF NOT EXISTS workspace (
    id                  INTEGER PRIMARY KEY,
    root_path           TEXT NOT NULL UNIQUE,
    name                TEXT NOT NULL,
    include_json        TEXT NOT NULL DEFAULT '[]',
    exclude_json        TEXT NOT NULL DEFAULT '[]',
    default_ignores_json TEXT NOT NULL DEFAULT '[]',
    compile_commands_path TEXT,
    default_compile_command TEXT,
    created_at          TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    revision            INTEGER NOT NULL DEFAULT 0,
    status              TEXT NOT NULL DEFAULT 'idle'
                        CHECK (status IN ('idle', 'indexing', 'ready', 'error')),
    last_error          TEXT
);

CREATE INDEX IF NOT EXISTS workspace_status_idx
    ON workspace(status);

-- 3. Index Jobs
CREATE TABLE IF NOT EXISTS index_job (
    id                  INTEGER PRIMARY KEY,
    workspace_id        INTEGER NOT NULL
                        REFERENCES workspace(id) ON DELETE CASCADE,
    job_type            TEXT NOT NULL
                        CHECK (job_type IN ('full', 'incremental', 'resolve', 'rebuild_search')),
    status              TEXT NOT NULL DEFAULT 'queued'
                        CHECK (status IN (
                            'queued',
                            'running',
                            'cancelling',
                            'cancelled',
                            'completed',
                            'failed'
                        )),
    requested_mode      TEXT,
    queued_at           TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    started_at          TEXT,
    finished_at         TEXT,
    files_total         INTEGER NOT NULL DEFAULT 0,
    files_processed     INTEGER NOT NULL DEFAULT 0,
    files_skipped       INTEGER NOT NULL DEFAULT 0,
    error_count         INTEGER NOT NULL DEFAULT 0,
    warning_count       INTEGER NOT NULL DEFAULT 0,
    workspace_revision  INTEGER,
    error_message       TEXT
);

CREATE INDEX IF NOT EXISTS job_workspace_idx
    ON index_job(workspace_id, queued_at DESC);

CREATE INDEX IF NOT EXISTS job_status_idx
    ON index_job(status, queued_at);

-- 4. Files
CREATE TABLE IF NOT EXISTS file (
    id                  INTEGER PRIMARY KEY,
    workspace_id        INTEGER NOT NULL
                        REFERENCES workspace(id) ON DELETE CASCADE,
    path                TEXT NOT NULL,
    relative_path       TEXT NOT NULL,
    name                TEXT NOT NULL,
    extension           TEXT,
    language            TEXT NOT NULL DEFAULT 'unknown',
    encoding            TEXT NOT NULL DEFAULT 'utf-8',
    size_bytes          INTEGER NOT NULL DEFAULT 0
                        CHECK (size_bytes >= 0),
    modified_ns         INTEGER NOT NULL DEFAULT 0,
    content_hash        TEXT,
    parse_hash          TEXT,
    language_version    TEXT,
    is_binary           INTEGER NOT NULL DEFAULT 0
                        CHECK (is_binary IN (0, 1)),
    is_generated        INTEGER NOT NULL DEFAULT 0
                        CHECK (is_generated IN (0, 1)),
    is_deleted          INTEGER NOT NULL DEFAULT 0
                        CHECK (is_deleted IN (0, 1)),
    last_index_job_id   INTEGER,
    indexed_at          TEXT,
    created_at          TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(workspace_id, relative_path),
    FOREIGN KEY (last_index_job_id)
        REFERENCES index_job(id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS file_workspace_idx
    ON file(workspace_id);

CREATE INDEX IF NOT EXISTS file_workspace_path_idx
    ON file(workspace_id, relative_path);

CREATE INDEX IF NOT EXISTS file_workspace_language_idx
    ON file(workspace_id, language);

CREATE INDEX IF NOT EXISTS file_workspace_hash_idx
    ON file(workspace_id, content_hash);

CREATE INDEX IF NOT EXISTS file_deleted_idx
    ON file(workspace_id, is_deleted);

-- 5. Symbols
CREATE TABLE IF NOT EXISTS symbol (
    id                  INTEGER PRIMARY KEY,
    workspace_id        INTEGER NOT NULL
                        REFERENCES workspace(id) ON DELETE CASCADE,
    file_id             INTEGER NOT NULL
                        REFERENCES file(id) ON DELETE CASCADE,
    symbol_key          TEXT NOT NULL,
    name                TEXT NOT NULL,
    qualified_name      TEXT,
    display_name        TEXT,
    kind                TEXT NOT NULL,
    language            TEXT NOT NULL,
    signature           TEXT,
    documentation       TEXT,
    container_name      TEXT,
    scope_symbol_id     INTEGER
                        REFERENCES symbol(id) ON DELETE SET NULL,
    visibility          TEXT,
    is_definition       INTEGER NOT NULL DEFAULT 0
                        CHECK (is_definition IN (0, 1)),
    is_declaration      INTEGER NOT NULL DEFAULT 0
                        CHECK (is_declaration IN (0, 1)),
    is_generated        INTEGER NOT NULL DEFAULT 0
                        CHECK (is_generated IN (0, 1)),
    start_byte          INTEGER NOT NULL CHECK (start_byte >= 0),
    end_byte            INTEGER NOT NULL CHECK (end_byte >= start_byte),
    start_line          INTEGER NOT NULL CHECK (start_line >= 0),
    start_column        INTEGER NOT NULL CHECK (start_column >= 0),
    end_line            INTEGER NOT NULL CHECK (end_line >= 0),
    end_column          INTEGER NOT NULL CHECK (end_column >= 0),
    selection_start_byte INTEGER,
    selection_end_byte   INTEGER,
    signature_hash      TEXT,
    parser_version      TEXT,
    created_at          TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(workspace_id, symbol_key),
    CHECK (
        selection_start_byte IS NULL
        OR selection_end_byte IS NULL
        OR selection_end_byte >= selection_start_byte
    )
);

CREATE INDEX IF NOT EXISTS symbol_file_range_idx
    ON symbol(file_id, start_byte, end_byte);

CREATE INDEX IF NOT EXISTS symbol_workspace_name_idx
    ON symbol(workspace_id, name);

CREATE INDEX IF NOT EXISTS symbol_workspace_qualified_name_idx
    ON symbol(workspace_id, qualified_name);

CREATE INDEX IF NOT EXISTS symbol_scope_idx
    ON symbol(scope_symbol_id);

CREATE INDEX IF NOT EXISTS symbol_workspace_kind_idx
    ON symbol(workspace_id, kind);

-- 6. Occurrences
CREATE TABLE IF NOT EXISTS occurrence (
    id                  INTEGER PRIMARY KEY,
    workspace_id        INTEGER NOT NULL
                        REFERENCES workspace(id) ON DELETE CASCADE,
    file_id             INTEGER NOT NULL
                        REFERENCES file(id) ON DELETE CASCADE,
    symbol_id           INTEGER
                        REFERENCES symbol(id) ON DELETE SET NULL,
    occurrence_kind     TEXT NOT NULL
                        CHECK (occurrence_kind IN (
                            'definition',
                            'declaration',
                            'reference',
                            'implementation',
                            'override',
                            'import',
                            'include'
                        )),
    name                TEXT NOT NULL,
    start_byte          INTEGER NOT NULL CHECK (start_byte >= 0),
    end_byte            INTEGER NOT NULL CHECK (end_byte >= start_byte),
    start_line          INTEGER NOT NULL CHECK (start_line >= 0),
    start_column        INTEGER NOT NULL CHECK (start_column >= 0),
    end_line            INTEGER NOT NULL CHECK (end_line >= 0),
    end_column          INTEGER NOT NULL CHECK (end_column >= 0),
    confidence          REAL NOT NULL DEFAULT 1.0
                        CHECK (confidence >= 0.0 AND confidence <= 1.0),
    resolution          TEXT NOT NULL DEFAULT 'unresolved'
                        CHECK (resolution IN (
                            'resolved',
                            'unresolved',
                            'ambiguous',
                            'external'
                        )),
    metadata_json       TEXT
);

CREATE INDEX IF NOT EXISTS occurrence_symbol_idx
    ON occurrence(symbol_id);

CREATE INDEX IF NOT EXISTS occurrence_file_range_idx
    ON occurrence(file_id, start_byte, end_byte);

CREATE INDEX IF NOT EXISTS occurrence_workspace_name_idx
    ON occurrence(workspace_id, name);

-- 7. References
CREATE TABLE IF NOT EXISTS reference_occurrence (
    id                  INTEGER PRIMARY KEY,
    workspace_id        INTEGER NOT NULL
                        REFERENCES workspace(id) ON DELETE CASCADE,
    source_file_id      INTEGER NOT NULL
                        REFERENCES file(id) ON DELETE CASCADE,
    source_symbol_id    INTEGER
                        REFERENCES symbol(id) ON DELETE SET NULL,
    target_symbol_id    INTEGER
                        REFERENCES symbol(id) ON DELETE SET NULL,
    name                TEXT NOT NULL,
    reference_kind      TEXT NOT NULL,
    start_byte          INTEGER NOT NULL CHECK (start_byte >= 0),
    end_byte            INTEGER NOT NULL CHECK (end_byte >= start_byte),
    start_line          INTEGER NOT NULL CHECK (start_line >= 0),
    start_column        INTEGER NOT NULL CHECK (start_column >= 0),
    end_line            INTEGER NOT NULL CHECK (end_line >= 0),
    end_column          INTEGER NOT NULL CHECK (end_column >= 0),
    resolution          TEXT NOT NULL DEFAULT 'unresolved'
                        CHECK (resolution IN (
                            'resolved',
                            'unresolved',
                            'ambiguous',
                            'external'
                        )),
    confidence          REAL NOT NULL DEFAULT 1.0
                        CHECK (confidence >= 0.0 AND confidence <= 1.0),
    metadata_json       TEXT
);

CREATE INDEX IF NOT EXISTS reference_target_idx
    ON reference_occurrence(target_symbol_id);

CREATE INDEX IF NOT EXISTS reference_source_file_idx
    ON reference_occurrence(source_file_id, start_byte);

CREATE INDEX IF NOT EXISTS reference_source_symbol_idx
    ON reference_occurrence(source_symbol_id);

CREATE INDEX IF NOT EXISTS reference_workspace_name_idx
    ON reference_occurrence(workspace_id, name);

CREATE INDEX IF NOT EXISTS reference_target_kind_idx
    ON reference_occurrence(target_symbol_id, reference_kind);

-- 8. Symbol relations
CREATE TABLE IF NOT EXISTS symbol_relation (
    id                  INTEGER PRIMARY KEY,
    workspace_id        INTEGER NOT NULL
                        REFERENCES workspace(id) ON DELETE CASCADE,
    source_symbol_id    INTEGER NOT NULL
                        REFERENCES symbol(id) ON DELETE CASCADE,
    target_symbol_id    INTEGER
                        REFERENCES symbol(id) ON DELETE SET NULL,
    relation_kind       TEXT NOT NULL,
    target_name         TEXT,
    resolution          TEXT NOT NULL DEFAULT 'unresolved'
                        CHECK (resolution IN (
                            'resolved',
                            'unresolved',
                            'ambiguous',
                            'external'
                        )),
    confidence          REAL NOT NULL DEFAULT 1.0
                        CHECK (confidence >= 0.0 AND confidence <= 1.0),
    metadata_json       TEXT,
    UNIQUE(source_symbol_id, target_symbol_id, relation_kind, target_name)
);

CREATE INDEX IF NOT EXISTS relation_source_idx
    ON symbol_relation(source_symbol_id, relation_kind);

CREATE INDEX IF NOT EXISTS relation_target_idx
    ON symbol_relation(target_symbol_id, relation_kind);

CREATE INDEX IF NOT EXISTS relation_workspace_kind_idx
    ON symbol_relation(workspace_id, relation_kind);

-- 9. File dependencies
CREATE TABLE IF NOT EXISTS file_dependency (
    id                  INTEGER PRIMARY KEY,
    workspace_id        INTEGER NOT NULL
                        REFERENCES workspace(id) ON DELETE CASCADE,
    source_file_id      INTEGER NOT NULL
                        REFERENCES file(id) ON DELETE CASCADE,
    target_file_id      INTEGER
                        REFERENCES file(id) ON DELETE SET NULL,
    dependency_kind     TEXT NOT NULL,
    raw_name            TEXT NOT NULL,
    resolved_path       TEXT,
    resolution          TEXT NOT NULL DEFAULT 'unresolved'
                        CHECK (resolution IN (
                            'resolved',
                            'unresolved',
                            'external'
                        )),
    start_byte          INTEGER,
    end_byte            INTEGER,
    metadata_json       TEXT,
    UNIQUE(source_file_id, raw_name, dependency_kind)
);

CREATE INDEX IF NOT EXISTS dependency_source_idx
    ON file_dependency(source_file_id, dependency_kind);

CREATE INDEX IF NOT EXISTS dependency_target_idx
    ON file_dependency(target_file_id);

-- 10. Diagnostics
CREATE TABLE IF NOT EXISTS diagnostic (
    id                  INTEGER PRIMARY KEY,
    workspace_id        INTEGER NOT NULL
                        REFERENCES workspace(id) ON DELETE CASCADE,
    job_id              INTEGER,
    file_id             INTEGER
                        REFERENCES file(id) ON DELETE CASCADE,
    severity            TEXT NOT NULL
                        CHECK (severity IN ('info', 'warning', 'error')),
    source              TEXT NOT NULL
                        CHECK (source IN (
                            'filesystem',
                            'parser',
                            'adapter',
                            'resolver',
                            'database',
                            'indexer'
                        )),
    code                TEXT NOT NULL,
    message             TEXT NOT NULL,
    start_byte          INTEGER,
    end_byte            INTEGER,
    start_line          INTEGER,
    start_column        INTEGER,
    end_line            INTEGER,
    end_column          INTEGER,
    metadata_json       TEXT,
    created_at          TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (job_id)
        REFERENCES index_job(id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS diagnostic_workspace_idx
    ON diagnostic(workspace_id, severity, created_at);

CREATE INDEX IF NOT EXISTS diagnostic_file_idx
    ON diagnostic(file_id, start_line, start_column);

CREATE INDEX IF NOT EXISTS diagnostic_job_idx
    ON diagnostic(job_id);

-- 11. Full-text search (Symbol search)
CREATE VIRTUAL TABLE IF NOT EXISTS symbol_search USING fts5(
    name,
    qualified_name,
    signature,
    documentation,
    content='symbol',
    content_rowid='id',
    tokenize='unicode61 remove_diacritics 1'
);

CREATE TRIGGER IF NOT EXISTS symbol_ai AFTER INSERT ON symbol BEGIN
    INSERT INTO symbol_search(rowid, name, qualified_name, signature, documentation)
    VALUES (
        new.id,
        new.name,
        COALESCE(new.qualified_name, ''),
        COALESCE(new.signature, ''),
        COALESCE(new.documentation, '')
    );
END;

CREATE TRIGGER IF NOT EXISTS symbol_ad AFTER DELETE ON symbol BEGIN
    INSERT INTO symbol_search(symbol_search, rowid, name, qualified_name, signature, documentation)
    VALUES ('delete', old.id, old.name, old.qualified_name, old.signature, old.documentation);
END;

CREATE TRIGGER IF NOT EXISTS symbol_au AFTER UPDATE ON symbol BEGIN
    INSERT INTO symbol_search(symbol_search, rowid, name, qualified_name, signature, documentation)
    VALUES ('delete', old.id, old.name, old.qualified_name, old.signature, old.documentation);
    INSERT INTO symbol_search(rowid, name, qualified_name, signature, documentation)
    VALUES (
        new.id,
        new.name,
        COALESCE(new.qualified_name, ''),
        COALESCE(new.signature, ''),
        COALESCE(new.documentation, '')
    );
END;

-- 12. Optional full-text search (File source search)
CREATE TABLE IF NOT EXISTS file_content (
    file_id         INTEGER PRIMARY KEY
                    REFERENCES file(id) ON DELETE CASCADE,
    workspace_id    INTEGER NOT NULL
                    REFERENCES workspace(id) ON DELETE CASCADE,
    content         TEXT NOT NULL,
    content_hash    TEXT NOT NULL,
    updated_at      TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE VIRTUAL TABLE IF NOT EXISTS file_search USING fts5(
    content,
    content='file_content',
    content_rowid='file_id',
    tokenize='unicode61 remove_diacritics 1'
);

CREATE TRIGGER IF NOT EXISTS file_content_ai AFTER INSERT ON file_content BEGIN
    INSERT INTO file_search(rowid, content)
    VALUES (new.file_id, new.content);
END;

CREATE TRIGGER IF NOT EXISTS file_content_ad AFTER DELETE ON file_content BEGIN
    INSERT INTO file_search(file_search, rowid, content)
    VALUES ('delete', old.file_id, old.content);
END;

CREATE TRIGGER IF NOT EXISTS file_content_au AFTER UPDATE ON file_content BEGIN
    INSERT INTO file_search(file_search, rowid, content)
    VALUES ('delete', old.file_id, old.content);
    INSERT INTO file_search(rowid, content)
    VALUES (new.file_id, new.content);
END;
)SQL";
}

MigrationRunner::MigrationRunner() {
    register_migration(Migration{
        .version = 1,
        .name = "001_initial_schema",
        .up_sql = initial_schema_sql(),
    });
}

void MigrationRunner::register_migration(Migration migration) {
    auto it = std::find_if(migrations_.begin(), migrations_.end(),
                           [&](const Migration& m) { return m.version == migration.version; });
    if (it != migrations_.end()) {
        *it = std::move(migration);
    } else {
        migrations_.push_back(std::move(migration));
    }
    std::sort(migrations_.begin(), migrations_.end(),
              [](const Migration& a, const Migration& b) { return a.version < b.version; });
}

void MigrationRunner::ensure_migration_table(Connection& conn) {
    conn.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS schema_migration (
            version         INTEGER PRIMARY KEY,
            name            TEXT NOT NULL,
            applied_at      TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
        );
    )SQL");
}

int64_t MigrationRunner::get_current_version(Connection& conn) {
    ensure_migration_table(conn);
    Statement stmt(conn.handle(), "SELECT COALESCE(MAX(version), 0) FROM schema_migration;");
    if (stmt.step()) {
        return stmt.column_int64(0);
    }
    return 0;
}

std::vector<int64_t> MigrationRunner::get_applied_versions(Connection& conn) {
    ensure_migration_table(conn);
    Statement stmt(conn.handle(), "SELECT version FROM schema_migration ORDER BY version ASC;");
    std::vector<int64_t> result;
    while (stmt.step()) {
        result.push_back(stmt.column_int64(0));
    }
    return result;
}

int64_t MigrationRunner::max_supported_version() const {
    if (migrations_.empty()) {
        return 0;
    }
    return migrations_.back().version;
}

void MigrationRunner::apply_pending(Connection& conn) {
    ensure_migration_table(conn);

    int64_t current_ver = get_current_version(conn);
    int64_t max_ver = max_supported_version();

    // Section 3: "The application must reject a database with a newer schema version than it
    // supports."
    if (current_ver > max_ver) {
        throw MigrationError("Database schema version (" + std::to_string(current_ver) +
                             ") is newer than supported version (" + std::to_string(max_ver) + ")");
    }

    auto applied_vec = get_applied_versions(conn);
    std::unordered_set<int64_t> applied(applied_vec.begin(), applied_vec.end());

    for (const auto& migration : migrations_) {
        if (applied.find(migration.version) != applied.end()) {
            continue;
        }

        Transaction tx(conn, TransactionType::immediate);
        conn.execute(migration.up_sql);

        Statement insert_stmt(conn.handle(),
                              "INSERT INTO schema_migration (version, name) VALUES (?, ?);");
        insert_stmt.bind_int64(1, migration.version);
        insert_stmt.bind_text(2, migration.name);
        insert_stmt.execute();

        tx.commit();
    }
}

} // namespace codelenses
