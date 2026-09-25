# Implementation Task Lists

Tasks are grouped by independent workstream. Each task has a suggested identifier, owner profile, dependencies, and completion criteria. Workstreams may proceed in parallel after the stated contracts are agreed.

## A. Build and repository foundation

**Dependencies:** none

- [x] A-01 Initialize CMake project with C++20 configuration.
- [x] A-02 Add dependency management for `cpp-httplib`, SQLite3, Tree-sitter core, and selected grammars.
- [x] A-03 Define Debug/Release builds and compiler warning policy.
- [x] A-04 Add unit-test framework and test discovery.
- [x] A-05 Add formatting and static-analysis configuration.
- [x] A-06 Add CI build/test workflow for Linux.
- [x] A-07 Add executable entry point and configuration loading.
- [x] A-08 Document local build, test, and dependency update commands.

**Done when:** clean checkout builds, tests run, and CI reports build/test status.

## B. Domain model and SQLite persistence

**Dependencies:** A-01, A-03

- [ ] B-01 Define domain structs for workspace, file, symbol, occurrence, reference, relation, dependency, diagnostic, and job.
- [ ] B-02 Create migration version table and migration runner.
- [ ] B-03 Implement initial schema for workspaces, files, symbols, occurrences, references, relations, dependencies, diagnostics, and jobs.
- [ ] B-04 Add SQLite initialization pragmas: foreign keys, WAL, busy timeout, synchronous mode.
- [ ] B-05 Implement transaction wrapper with rollback-on-error behavior.
- [ ] B-06 Implement workspace repository.
- [ ] B-07 Implement file repository and file-state comparison queries.
- [ ] B-08 Implement symbol/occurrence/reference/relation repositories.
- [ ] B-09 Implement FTS5 maintenance and search queries.
- [ ] B-10 Add indexes for file path, symbol name, target symbol, and source ranges.
- [ ] B-11 Add migration, cascade-delete, rollback, and query tests.

**Done when:** repositories can create a database, persist one complete file index transactionally, query referencers, and upgrade schema versions.

## C. Tree-sitter adapter SDK

**Dependencies:** A-02

- [ ] C-01 Define `LanguageAdapter` interface.
- [ ] C-02 Define source range, highlight token, extracted symbol, reference, dependency, and diagnostic types.
- [ ] C-03 Define normalized symbol kinds and relation kinds.
- [ ] C-04 Implement parser wrapper with tree ownership and parse-error collection.
- [ ] C-05 Implement adapter registry by language ID and filename mapping.
- [ ] C-06 Define byte/line/column conversion utilities and test them with UTF-8 input.
- [ ] C-07 Define highlight token legend.
- [ ] C-08 Define fixture format and golden-output comparison helper.
- [ ] C-09 Add a minimal reference adapter used by pipeline tests.

**Done when:** a test adapter can parse fixture text and produce normalized IR without any database dependency.

## D. File discovery and indexing pipeline

**Dependencies:** A, B interfaces, C interfaces

- [ ] D-01 Implement workspace path canonicalization and containment checks.
- [ ] D-02 Implement recursive file discovery with include/exclude rules.
- [ ] D-03 Implement default ignored-directory policy.
- [ ] D-04 Implement language and binary detection.
- [ ] D-05 Implement content hashing and file-state comparison.
- [ ] D-06 Implement index job lifecycle and progress counters.
- [ ] D-07 Implement bounded worker queue for parsing.
- [ ] D-08 Implement per-file extraction flow.
- [ ] D-09 Replace file-owned records in one transaction.
- [ ] D-10 Remove stale records for deleted files.
- [ ] D-11 Implement cancellation and safe shutdown.
- [ ] D-12 Add diagnostics for read, parse, extraction, and persistence failures.
- [ ] D-13 Add incremental-index tests with unchanged, modified, renamed, and deleted files.
- [ ] D-14 Add concurrency tests for one writer and multiple readers.

**Done when:** full and incremental jobs produce consistent database state and survive malformed files.

## E. Resolver and relationship builder

**Dependencies:** B, C, D

- [ ] E-01 Define scope and symbol-candidate lookup interfaces.
- [ ] E-02 Implement deterministic symbol-key generation.
- [ ] E-03 Implement same-file lexical/scope resolution.
- [ ] E-04 Implement dependency path resolution.
- [ ] E-05 Implement cross-file name resolution baseline.
- [ ] E-06 Implement confidence and ambiguity handling.
- [ ] E-07 Build `contains`, `imports`, and `includes` relations.
- [ ] E-08 Build `calls`, `inherits`, `implements`, and `overrides` where adapter data supports them.
- [ ] E-09 Add optional C/C++ compile-command configuration model.
- [ ] E-10 Add resolver fixture tests for resolved, unresolved, ambiguous, and external cases.
- [ ] E-11 Add referencer, caller, callee, and inheritance query tests.

**Done when:** the selected-symbol query returns complete, labeled results and never hides unresolved references.

## F. HTTP API server

**Dependencies:** A, B application-service interfaces

- [ ] F-01 Define versioned route and DTO schemas.
- [ ] F-02 Implement JSON error envelope with request IDs.
- [ ] F-03 Implement server configuration: bind address, port, database path, workspace policy.
- [ ] F-04 Implement workspace CRUD routes.
- [ ] F-05 Implement indexing, status, job, and cancellation routes.
- [ ] F-06 Implement tree, file metadata, and range content routes.
- [ ] F-07 Implement symbols, outline, occurrences, and highlights routes.
- [ ] F-08 Implement symbol detail, references, definitions, callers, callees, and graph routes.
- [ ] F-09 Implement source and symbol search routes with pagination.
- [ ] F-10 Add request validation and path traversal protection.
- [ ] F-11 Add CORS policy for local development.
- [ ] F-12 Add API contract tests and error-path tests.
- [ ] F-13 Add graceful shutdown and active-job handling.

**Done when:** all required endpoints return stable JSON DTOs and integration tests cover successful and invalid requests.

## G. Frontend browser

**Dependencies:** F-01 contract; backend may be mocked initially

- [ ] G-01 Create frontend build and development configuration.
- [ ] G-02 Implement three-pane layout: explorer, source editor, symbol/references panel.
- [ ] G-03 Implement lazy folder tree loading.
- [ ] G-04 Implement file opening, tabs, loading, and error states.
- [ ] G-05 Integrate Monaco or an equivalent editor.
- [ ] G-06 Implement syntax-token and semantic-token application.
- [ ] G-07 Implement file outline and symbol selection.
- [ ] G-08 Implement referencer list with file/range previews.
- [ ] G-09 Implement click-to-navigate from reference to source range.
- [ ] G-10 Implement symbol search and source search.
- [ ] G-11 Implement indexing progress and diagnostics display.
- [ ] G-12 Add loading, empty, unresolved, ambiguous, and binary-file states.
- [ ] G-13 Add frontend component and API integration tests.

**Done when:** a user can browse, open a file, select a symbol, view referencers, and navigate to a result.

## H. Language adapter implementation

**Dependencies:** C adapter SDK and fixture harness

### H1 C adapter

- [ ] H1-01 Add grammar registration and file extensions.
- [ ] H1-02 Extract functions, variables, structs, enums, typedefs, macros, includes.
- [ ] H1-03 Extract declarations and calls.
- [ ] H1-04 Add highlighting queries.
- [ ] H1-05 Add fixtures for headers, pointers, macros, and declarations.

### H2 C++ adapter

- [ ] H2-01 Extract namespaces, classes, structs, enums, aliases, templates, functions, methods.
- [ ] H2-02 Extract inheritance, using declarations, includes, and calls.
- [ ] H2-03 Add qualified-name and overload metadata where syntactically available.
- [ ] H2-04 Support optional compile-command context.
- [ ] H2-05 Add fixtures for templates, overloads, namespaces, and header/source pairs.

### H3 C# adapter

- [ ] H3-01 Extract namespaces, types, methods, properties, fields, using directives.
- [ ] H3-02 Extract inheritance, interfaces, calls, and attributes.
- [ ] H3-03 Add fixtures for generics and partial declarations.

### H4 Python adapter

- [ ] H4-01 Extract modules, imports, aliases, classes, functions, methods, decorators.
- [ ] H4-02 Extract calls and attribute references with confidence metadata.
- [ ] H4-03 Add fixtures for relative imports, aliases, nested scopes, and dynamic cases.

### H5 TypeScript/TSX adapter

- [ ] H5-01 Extract imports/exports, types, interfaces, classes, enums, functions, variables.
- [ ] H5-02 Extract JSX components and references.
- [ ] H5-03 Add fixtures for aliases, re-exports, generics, and TSX.

### H6 JavaScript/JSX adapter

- [ ] H6-01 Extract imports/exports, classes, functions, variables, methods.
- [ ] H6-02 Extract JSX component references and calls.
- [ ] H6-03 Add fixtures for CommonJS, ES modules, dynamic properties, and JSX.

### H7 Go adapter

- [ ] H7-01 Extract package, imports, types, structs, interfaces, functions, methods.
- [ ] H7-02 Extract receiver methods, calls, and interface implementations where detectable.
- [ ] H7-03 Add fixtures for aliases and multi-file packages.

### H8 Java adapter

- [ ] H8-01 Extract packages, imports, classes, interfaces, enums, fields, methods.
- [ ] H8-02 Extract inheritance, overrides, overload metadata, and calls.
- [ ] H8-03 Add fixtures for nested classes and generics.

### H9 POSIX shell/Bash adapter

- [ ] H9-01 Extract functions, variables, source directives, and command occurrences.
- [ ] H9-02 Distinguish shell built-ins and external commands when possible.
- [ ] H9-03 Add fixtures for quoting, subshells, aliases, and sourced files.

**Done for each adapter when:** grammar registration, extraction, highlighting, and representative fixtures pass; unsupported semantics are labeled rather than guessed.

## I. Testing, performance, and packaging

**Dependencies:** all relevant workstreams

- [ ] I-01 Create end-to-end sample workspace covering all languages.
- [ ] I-02 Add API contract tests against a temporary SQLite database.
- [ ] I-03 Add database migration tests from every supported schema version.
- [ ] I-04 Add malformed-source and hostile-path security tests.
- [ ] I-05 Add cancellation and restart recovery tests.
- [ ] I-06 Benchmark cold indexing and incremental indexing separately.
- [ ] I-07 Benchmark tree expansion, source range retrieval, symbol search, and reference queries.
- [ ] I-08 Add memory and database-size measurements for representative repositories.
- [ ] I-09 Add release packaging and sample configuration.
- [ ] I-10 Document limitations and language-specific resolution behavior.
- [ ] I-11 Define regression suite required before each release.

**Done when:** the acceptance criteria in `requirements.md` pass on a repeatable sample and performance regressions have thresholds.

## 8. Suggested parallel execution

### Phase 1

- A: foundation
- B: schema/domain design
- C: adapter SDK design
- F: API DTO and route contract design
- G: frontend mock screens

### Phase 2

- D: discovery/indexing
- H1/H2/H4/H5: initial high-value adapters
- F: API implementation
- G: real API integration

### Phase 3

- E: resolver/graph builder
- H3/H6/H7/H8/H9: remaining adapters
- I: end-to-end and performance tests

### Phase 4

- Hardening, security review, migration validation, packaging, and documentation.

## 9. Definition of done for any task

- Code is covered by an appropriate unit or integration test.
- Public behavior is documented.
- Errors are handled explicitly.
- No task introduces direct coupling across workstream boundaries without updating the relevant interface.
- Formatting, warnings, and tests pass.
- Limitations and unresolved behavior are represented in data rather than silently dropped.
