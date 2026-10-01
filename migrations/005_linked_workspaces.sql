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
