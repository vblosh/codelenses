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

namespace {

std::string library_indexes_sql() {
    return R"SQL(
-- 002_library_indexes: Library index support

PRAGMA foreign_keys = OFF;

CREATE TABLE IF NOT EXISTS workspace_migration (
    id                  INTEGER PRIMARY KEY,
    root_path           TEXT NOT NULL,
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
    last_error          TEXT,
    kind                TEXT NOT NULL DEFAULT 'project'
);

INSERT INTO workspace_migration (id, root_path, name, include_json, exclude_json, default_ignores_json,
                                 compile_commands_path, default_compile_command, created_at, updated_at,
                                 revision, status, last_error, kind)
SELECT id, root_path, name, include_json, exclude_json, default_ignores_json,
       compile_commands_path, default_compile_command, created_at, updated_at,
       revision, status, last_error, 'project'
FROM workspace;

DROP TABLE workspace;

ALTER TABLE workspace_migration RENAME TO workspace;

CREATE UNIQUE INDEX IF NOT EXISTS workspace_project_root_idx
    ON workspace(root_path) WHERE kind = 'project';

CREATE INDEX IF NOT EXISTS workspace_status_idx
    ON workspace(status);

CREATE INDEX IF NOT EXISTS workspace_kind_idx
    ON workspace(kind);

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS library_profile (
    id                          INTEGER PRIMARY KEY,
    workspace_id                INTEGER NOT NULL UNIQUE
                                REFERENCES workspace(id) ON DELETE CASCADE,
    name                        TEXT NOT NULL,
    language                    TEXT NOT NULL,
    provider                    TEXT NOT NULL DEFAULT '',
    sdk_version                 TEXT,
    target_environment          TEXT,
    language_standard           TEXT,
    sysroot                     TEXT,
    source_roots_json           TEXT NOT NULL DEFAULT '[]',
    default_include_roots_json  TEXT NOT NULL DEFAULT '[]',
    defines_json                TEXT NOT NULL DEFAULT '[]',
    include_json                TEXT NOT NULL DEFAULT '[]',
    exclude_json                TEXT NOT NULL DEFAULT '[]',
    header_rules_json           TEXT NOT NULL DEFAULT '{}',
    resource_limits_json        TEXT NOT NULL DEFAULT '{}',
    fingerprint                 TEXT NOT NULL DEFAULT '',
    created_at                  TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at                  TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS library_profile_language_idx
    ON library_profile(language);

CREATE TABLE IF NOT EXISTS workspace_library (
    workspace_id        INTEGER NOT NULL
                        REFERENCES workspace(id) ON DELETE CASCADE,
    profile_id          INTEGER NOT NULL
                        REFERENCES library_profile(id) ON DELETE CASCADE,
    attached_at         TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (workspace_id, profile_id)
);

CREATE INDEX IF NOT EXISTS workspace_library_profile_idx
    ON workspace_library(profile_id);
)SQL";
}

std::string csharp_library_profiles_sql() {
    return R"SQL(
ALTER TABLE library_profile ADD COLUMN target_framework TEXT;

CREATE TABLE IF NOT EXISTS library_source_root (
    id          INTEGER PRIMARY KEY,
    profile_id  INTEGER NOT NULL REFERENCES library_profile(id) ON DELETE CASCADE,
    ordinal     INTEGER NOT NULL,
    root_path   TEXT NOT NULL,
    UNIQUE(profile_id, root_path),
    UNIQUE(profile_id, ordinal)
);

CREATE INDEX IF NOT EXISTS library_source_root_profile_idx
    ON library_source_root(profile_id, ordinal);

-- Give every explicitly configured root a durable identity for multi-root indexing.
INSERT INTO library_source_root (profile_id, ordinal, root_path)
SELECT p.id, CAST(root.key AS INTEGER), root.value
FROM library_profile p,
     json_each(CASE WHEN json_valid(p.source_roots_json)
                    THEN p.source_roots_json ELSE '[]' END) AS root
WHERE root.type = 'text' AND length(trim(root.value)) > 0
ON CONFLICT(profile_id, root_path) DO NOTHING;

-- Older rows can have no serialized source roots; retain their backing workspace root.
INSERT INTO library_source_root (profile_id, ordinal, root_path)
SELECT p.id, 0, w.root_path
FROM library_profile p
JOIN workspace w ON w.id = p.workspace_id
WHERE NOT EXISTS (
    SELECT 1 FROM library_source_root r WHERE r.profile_id = p.id
);
)SQL";
}

std::string inheritance_occurrence_sql() {
    return R"SQL(
DROP INDEX IF EXISTS occurrence_symbol_idx;
DROP INDEX IF EXISTS occurrence_file_range_idx;
DROP INDEX IF EXISTS occurrence_workspace_name_idx;

ALTER TABLE occurrence RENAME TO occurrence_v3;

CREATE TABLE occurrence (
    id                  INTEGER PRIMARY KEY,
    workspace_id        INTEGER NOT NULL REFERENCES workspace(id) ON DELETE CASCADE,
    file_id             INTEGER NOT NULL REFERENCES file(id) ON DELETE CASCADE,
    symbol_id           INTEGER REFERENCES symbol(id) ON DELETE SET NULL,
    occurrence_kind     TEXT NOT NULL CHECK (occurrence_kind IN (
                            'definition', 'declaration', 'reference', 'inheritance',
                            'implementation', 'override', 'import', 'include'
                        )),
    name                TEXT NOT NULL,
    start_byte          INTEGER NOT NULL CHECK (start_byte >= 0),
    end_byte            INTEGER NOT NULL CHECK (end_byte >= start_byte),
    start_line          INTEGER NOT NULL CHECK (start_line >= 0),
    start_column        INTEGER NOT NULL CHECK (start_column >= 0),
    end_line            INTEGER NOT NULL CHECK (end_line >= 0),
    end_column          INTEGER NOT NULL CHECK (end_column >= 0),
    confidence          REAL NOT NULL DEFAULT 1.0 CHECK (confidence >= 0.0 AND confidence <= 1.0),
    resolution          TEXT NOT NULL DEFAULT 'unresolved' CHECK (resolution IN (
                            'resolved', 'unresolved', 'ambiguous', 'external'
                        )),
    metadata_json       TEXT
);

INSERT INTO occurrence (
    id, workspace_id, file_id, symbol_id, occurrence_kind, name,
    start_byte, end_byte, start_line, start_column, end_line, end_column,
    confidence, resolution, metadata_json
)
SELECT
    id, workspace_id, file_id, symbol_id, occurrence_kind, name,
    start_byte, end_byte, start_line, start_column, end_line, end_column,
    confidence, resolution, metadata_json
FROM occurrence_v3;

DROP TABLE occurrence_v3;

CREATE INDEX occurrence_symbol_idx ON occurrence(symbol_id);
CREATE INDEX occurrence_file_range_idx ON occurrence(file_id, start_byte, end_byte);
CREATE INDEX occurrence_workspace_name_idx ON occurrence(workspace_id, name);
)SQL";
}

} // namespace

MigrationRunner::MigrationRunner() {
    register_migration(Migration{
        .version = 1,
        .name = "001_initial_schema",
        .up_sql = initial_schema_sql(),
    });
    register_migration(Migration{
        .version = 2,
        .name = "002_library_indexes",
        .up_sql = library_indexes_sql(),
    });
    register_migration(Migration{
        .version = 3,
        .name = "003_csharp_library_profiles",
        .up_sql = csharp_library_profiles_sql(),
    });
    register_migration(Migration{
        .version = 4,
        .name = "004_inheritance_occurrences",
        .up_sql = inheritance_occurrence_sql(),
    });
    register_migration(Migration{.version = 5, .name = "005_linked_workspaces", .up_sql = R"SQL(
-- Unify libraries and projects without changing indexed identities.
DROP INDEX IF EXISTS workspace_project_root_idx;
ALTER TABLE library_profile RENAME TO workspace_index_settings;
ALTER TABLE library_source_root RENAME TO workspace_source_root;
CREATE TABLE workspace_link (
    workspace_id INTEGER NOT NULL REFERENCES workspace(id) ON DELETE CASCADE,
    target_workspace_id INTEGER NOT NULL REFERENCES workspace(id) ON DELETE CASCADE,
    linked_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY(workspace_id, target_workspace_id),
    CHECK(workspace_id <> target_workspace_id)
);
CREATE INDEX workspace_link_target_idx ON workspace_link(target_workspace_id);
INSERT INTO workspace_link(workspace_id, target_workspace_id, linked_at)
SELECT l.workspace_id, p.workspace_id, l.attached_at
FROM workspace_library l JOIN workspace_index_settings p ON p.id = l.profile_id;
DROP TABLE workspace_library;
UPDATE workspace SET kind = 'project';
)SQL"});
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

    // Disable foreign key enforcement before running migrations that may alter/rebuild tables.
    // In SQLite, PRAGMA foreign_keys is a no-op inside an active transaction, so it MUST be toggled
    // outside the transaction.
    conn.execute("PRAGMA foreign_keys = OFF;");
    struct FkGuard {
        Connection& c;
        ~FkGuard() {
            try {
                c.execute("PRAGMA foreign_keys = ON;");
            } catch (...) {
            }
        }
    } fk_guard{conn};

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

    // Verify foreign key integrity after all pending migrations have committed.
    Statement fk_check(conn.handle(), "PRAGMA foreign_key_check;");
    if (fk_check.step()) {
        std::string table = fk_check.column_text(0);
        int64_t rowid = fk_check.column_int64(1);
        std::string parent = fk_check.column_text(2);
        throw MigrationError("Foreign key constraint violation after migration in table " + table +
                             " (rowid " + std::to_string(rowid) + ") referencing " + parent);
    }
}

} // namespace codelenses
