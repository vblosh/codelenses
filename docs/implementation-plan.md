# Implementation Plan

## 1. Delivery strategy

Implement the system as independently testable vertical components. Keep the extraction pipeline independent from SQLite and HTTP so adapters can be developed and tested without the server.

Recommended order:

1. Repository/build foundation.
2. Shared domain model and database migrations.
3. Tree-sitter adapter framework.
4. File discovery and indexing pipeline.
5. Resolver and relationship builder.
6. HTTP API.
7. Frontend shell and source viewer.
8. Language adapter expansion.
9. Integration, performance, and packaging.

## 2. Workstreams and independence

| Part | Workstream | Can start independently | Main dependency | Primary output |
|---|---|---:|---|---|
| A | Build and repository foundation | Yes | None | Build, tests, dependency policy |
| B | Domain model and SQLite | Yes | A for build integration | Migrations, repositories, queries |
| C | Tree-sitter adapter SDK | Yes | A | Adapter interface, registry, fixtures |
| D | File discovery and incremental indexer | Partly | A, B, C interfaces | Index jobs and file state |
| E | Symbol resolver and graph builder | Partly | B, C, D extracted IR | Resolved references/relations |
| F | HTTP API server | Yes after contracts | A, B interfaces | REST API and OpenAPI-like contract |
| G | Frontend browser | Yes after API mock contract | API contract | Explorer/editor/symbol panels |
| H | Language adapters | Yes in parallel | C, fixtures | Nine language implementations |
| I | Testing, performance, packaging | Partly | All core components | CI, benchmarks, release package |

## 3. Dependency rules

- The adapter layer emits an intermediate representation; it must not depend on SQLite.
- The database layer accepts normalized domain objects; it must not parse Tree-sitter nodes.
- The HTTP layer calls application services; it must not issue ad-hoc SQL.
- The frontend uses stable API DTOs; it must not depend on backend database details.
- Resolver behavior must be testable with in-memory extracted fixtures.

## 4. Milestones

### M0: Foundation

Deliverable: buildable repository with test runner, formatting, linting, dependency lock/version policy, and basic executable.

Exit criteria: clean build and one passing unit test on Linux.

### M1: Syntax index

Deliverable: files, parse trees, syntax symbols, highlights, file tree, and source retrieval.

Exit criteria: a workspace can be indexed and browsed without semantic reference resolution.

### M2: Semantic navigation

Deliverable: references, definitions, calls, imports/includes, and symbol graph.

Exit criteria: selecting a symbol returns referencers with confidence/resolution state.

### M3: Browser UI

Deliverable: VS-like three-pane browser with tree, source editor, outline, and references.

Exit criteria: user can open a file, click a symbol, and navigate to a reference.

### M4: Language breadth

Deliverable: all nine language families with fixtures and baseline extraction.

Exit criteria: each adapter passes required fixture tests and reports unsupported constructs safely.

### M5: Hardening

Deliverable: incremental performance, cancellation, migrations, diagnostics, security checks, packaging, and documentation.

Exit criteria: acceptance suite passes and a representative repository can be indexed repeatedly.

## 5. Suggested repository layout

```text
codelenses/
  CMakeLists.txt
  cmake/
  include/codelenses/
    domain/
    db/
    parser/
    index/
    resolver/
    http/
  src/
    domain/
    db/
    parser/
    index/
    resolver/
    http/
    app/
  adapters/
    c/
    cpp/
    csharp/
    python/
    typescript/
    javascript/
    go/
    java/
    shell/
  web/
  migrations/
  tests/
    unit/
    integration/
    fixtures/
  docs/
```

## 6. Architecture decisions to make early

1. C++ standard: C++20 recommended.
2. Build system: CMake with a reproducible dependency mechanism.
3. Frontend editor: Monaco is recommended for VS-like behavior; backend tokens remain useful for semantic coloring.
4. Content ownership: read source from filesystem, cache only metadata/tokens unless source search requires stored content.
5. SQLite access: one writer with transactions and bounded reader connections.
6. API coordinate convention: use zero-based line/column internally and document it in DTOs.
7. Job model: one workspace indexing job at a time, with cancellation and monotonic progress.
8. Symbol identity: deterministic symbol key plus database ID; never use row ID as a cross-reindex identity.

## 7. Risks and mitigations

- **C++ semantic complexity**: begin with qualified-name and scope resolution; optionally consume `compile_commands.json`; label heuristic matches.
- **Large repositories**: lazy tree API, incremental hashing, bounded queues, range-based content, pagination.
- **Tree-sitter grammar differences**: adapter-specific fixtures and a shared normalized IR.
- **Stale references**: replace all file-owned extracted records atomically and remove deleted-file records.
- **SQLite writer contention**: single indexing writer, WAL mode, short transactions.
- **Frontend/backend drift**: maintain DTO schemas and contract tests with mocked API responses.
- **Path security**: canonicalize root and requested path, then verify it remains under the root.
