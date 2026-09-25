# Code Indexer/Browser Requirements Specification

## 1. Purpose

Build a local-first code indexer and browser with a C++ backend. The system parses source code with Tree-sitter, stores the index in SQLite, serves a JSON/HTTP API through `cpp-httplib`, and provides a VS-like browser UI with a folder tree, syntax-highlighted source view, symbol outline, and referencer view.

The first release targets C, C++, C#, Python, TypeScript/TSX, JavaScript/JSX, Go, Java, and POSIX shell/Bash.

## 2. Goals

- Index a workspace incrementally.
- Display folders and files with lazy tree loading.
- Open source files and navigate by line/range.
- Provide Tree-sitter-based syntax highlighting.
- Extract declarations, definitions, imports/includes, and references.
- Select a symbol and display its definitions, references, callers, callees, and relationships.
- Search files and symbols.
- Continue serving the UI while indexing runs in the background.
- Report unresolved and ambiguous references instead of silently discarding them.

## 3. Non-goals for v1

- A full replacement for language servers or compilers.
- Perfect semantic resolution for C++ templates/macros, dynamic Python, or dynamic JavaScript.
- Code modification, formatting, refactoring, compilation, debugging, or source control operations.
- Remote multi-user indexing or cloud storage.
- Full build-system modeling beyond optional compilation/build metadata.

## 4. Terminology

- **Workspace**: indexed project root.
- **File**: source file known to the workspace.
- **Symbol**: declaration or definition such as a class, function, method, type, or variable.
- **Occurrence**: source range associated with a symbol.
- **Reference**: use of a symbol from another source location.
- **Relation**: directed relation such as `calls`, `inherits`, `implements`, `imports`, or `contains`.
- **Adapter**: language-specific Tree-sitter extraction and highlighting implementation.
- **Resolver**: component that maps extracted names to symbol identities.

## 5. Functional requirements

### FR-1 Workspace management

1. Create a workspace from an absolute root path.
2. Store include and exclude patterns.
3. List, inspect, update, and delete workspaces.
4. Reject roots that do not exist or are not directories.
5. Normalize paths and prevent path traversal outside the workspace root.
6. Expose indexing status and last error.

### FR-2 File discovery

1. Discover files recursively under the workspace root.
2. Apply ordered include/exclude rules.
3. Ignore common generated/dependency directories by default: `.git`, `node_modules`, `build`, `dist`, `target`, `vendor`, and configurable equivalents.
4. Detect language from filename and, where useful, shebang/content.
5. Record unknown files as browseable files but do not parse them.
6. Detect binary files and avoid returning binary content as source text.

### FR-3 Incremental indexing

1. Compare file size, modification time, and content hash.
2. Reuse unchanged parse/index results.
3. Remove records for deleted files.
4. Re-index changed files transactionally.
5. Support full and incremental indexing modes.
6. Expose progress, error count, and cancellation.
7. Avoid blocking HTTP requests while indexing.

### FR-4 Parsing and language adapters

1. Use Tree-sitter grammars selected by language adapter.
2. Each adapter must support parsing, symbol extraction, reference extraction, dependency extraction, and highlight extraction.
3. Adapters must return byte and line/column ranges using one documented coordinate convention.
4. Parser errors must be recorded as diagnostics and must not crash indexing.
5. Adapters must identify unsupported or uncertain constructs.
6. The adapter registry must allow adding a language without changing core indexing code.

### FR-5 Language coverage

The initial adapters must support at least:

- C: functions, variables, structs, enums, typedefs, macros, includes.
- C++: namespaces, classes, structs, enums, functions, methods, templates, aliases, includes, inheritance.
- C#: namespaces, classes, structs, interfaces, enums, methods, properties, fields, using directives, inheritance.
- Python: modules, classes, functions, methods, variables, imports, decorators, calls.
- TypeScript/TSX: modules, imports/exports, classes, interfaces, types, enums, functions, variables, JSX references.
- JavaScript/JSX: imports/exports, classes, functions, variables, methods, JSX references.
- Go: packages, imports, types, structs, interfaces, functions, methods, receivers.
- Java: packages, imports, classes, interfaces, enums, methods, fields, inheritance.
- POSIX shell/Bash: functions, variables, sourced files, commands, aliases where statically detectable.

### FR-6 Symbol model

1. Assign every indexed symbol a database ID and a stable-ish symbol key.
2. Store name, qualified name, kind, signature, documentation when available, containing symbol, visibility, definition/declaration flags, and source ranges.
3. Store declarations and definitions separately when both exist.
4. Preserve unresolved and ambiguous target names.
5. Provide deterministic ordering by source position.

### FR-7 Reference and relation resolution

1. Resolve references within the same file where possible.
2. Resolve imports/includes and cross-file references using language-specific rules.
3. Store confidence and resolution state: `resolved`, `unresolved`, `ambiguous`, or `external`.
4. Support relations: `contains`, `calls`, `imports`, `includes`, `inherits`, `implements`, `overrides`, and `instantiates` where available.
5. Do not claim certainty for heuristic matches; expose confidence to clients.
6. Use `compile_commands.json` for C/C++ configuration when provided.

### FR-8 Browsing and source retrieval

1. Return a lazy folder tree.
2. Return file metadata and source content.
3. Support line-range retrieval for large files.
4. Return UTF-8 text and a clear error for binary files.
5. Return source ranges in both zero-based API coordinates and display-friendly values, or document one convention consistently.

### FR-9 Highlighting

1. Return syntax tokens for a file or line range.
2. Return a stable token legend.
3. Support semantic modifiers such as declaration, definition, readonly, static, async, and deprecated where available.
4. Never require the UI to parse Tree-sitter output.

### FR-10 Navigation and search

1. Search symbols by name and qualified name.
2. Search source text with file and line results.
3. Retrieve file outline.
4. Resolve the symbol under a source position.
5. Retrieve definitions, declarations, references, callers, callees, derived types, base types, and implementations.
6. Support pagination and result limits.
7. Include source preview text for reference results.

### FR-11 Diagnostics and observability

1. Store parser, file, adapter, and resolver diagnostics.
2. Return diagnostics filtered by file, severity, and indexing job.
3. Log request ID, workspace ID, job ID, duration, and failure reason.
4. Avoid logging source contents by default.

## 6. HTTP API requirements

Base path: `/api/v1`.

Required endpoints:

- `POST /workspaces`
- `GET /workspaces`
- `GET /workspaces/{id}`
- `PATCH /workspaces/{id}`
- `DELETE /workspaces/{id}`
- `POST /workspaces/{id}/index`
- `GET /workspaces/{id}/status`
- `GET /jobs/{id}`
- `POST /jobs/{id}/cancel`
- `GET /workspaces/{id}/tree`
- `GET /workspaces/{id}/files/{fileId}`
- `GET /workspaces/{id}/files/{fileId}/content`
- `GET /workspaces/{id}/files/{fileId}/highlights`
- `GET /workspaces/{id}/files/{fileId}/symbols`
- `GET /workspaces/{id}/files/{fileId}/outline`
- `GET /workspaces/{id}/files/{fileId}/occurrences`
- `GET /workspaces/{id}/symbols`
- `GET /workspaces/{id}/symbols/{symbolId}`
- `GET /workspaces/{id}/symbols/{symbolId}/references`
- `GET /workspaces/{id}/symbols/{symbolId}/definitions`
- `GET /workspaces/{id}/symbols/{symbolId}/callers`
- `GET /workspaces/{id}/symbols/{symbolId}/callees`
- `GET /workspaces/{id}/symbols/{symbolId}/graph`
- `GET /workspaces/{id}/search`
- `GET /workspaces/{id}/search/symbols`

Errors must use a consistent object containing `code`, `message`, optional `details`, and `requestId`.

## 7. Persistence requirements

1. SQLite must enable foreign keys and WAL mode.
2. Database writes must be transactional per file or per bounded batch.
3. Schema migrations must be versioned.
4. Deleting a workspace must cascade all workspace-owned data.
5. Indexes must support file lookup, symbol lookup, target-reference lookup, and source-range lookup.
6. FTS5 must support symbol search and optionally source search.

## 8. Non-functional requirements

### Performance

- The UI must receive a response for tree and metadata requests without waiting for a complete workspace index.
- Incremental indexing must skip unchanged files.
- Large result sets must be paginated.
- Source content must support range retrieval.

### Reliability

- A malformed source file must not abort the entire job.
- A failed file transaction must not leave partial symbol/reference rows.
- Cancellation must stop new work and leave the database consistent.

### Security

- Bind to localhost by default.
- Validate workspace paths and all relative paths.
- Never allow arbitrary filesystem reads outside a configured workspace.
- Add authentication before exposing beyond localhost.

### Portability

- Linux is required initially; avoid platform-specific APIs in core modules.
- Build with a modern C++ standard, preferably C++20.
- Support clean builds with all dependencies supplied by the selected package/build system.

## 9. Acceptance criteria for v1

1. A workspace containing all target language families can be indexed without server failure.
2. The UI can browse folders, open files, and display highlighting.
3. Selecting a function/class shows its declarations and references.
4. Unresolved references are visible and labeled rather than omitted.
5. Re-running incremental indexing does not reparse unchanged files.
6. Deleting or renaming a file removes stale index records.
7. Parser failures are reported through diagnostics.
8. API responses are versioned, paginated where applicable, and covered by integration tests.
9. Database migrations can create a new database and upgrade an older one.
10. The backend passes unit, integration, and basic load tests.
