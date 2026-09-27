-- 002_library_indexes.sql: Library index support

-- Workspaces can act as project workspaces or as backing storage for a
-- library profile. Replace global root uniqueness with project-root uniqueness
-- so multiple library instances or different language configurations can share roots.
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

-- Library profiles: explicitly configured library indexes (no SDK discovery).
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

-- Attachments of library profiles to project workspaces.
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
