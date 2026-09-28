-- C# library profile identity and durable, ordered source-root identities.
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
