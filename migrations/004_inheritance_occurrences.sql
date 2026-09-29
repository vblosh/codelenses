-- Preserve inheritance facts as first-class occurrences so hierarchy relations can be built.
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
