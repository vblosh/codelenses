# CodeLenses Release Regression Suite

This document defines the mandatory **Regression Suite** required to pass before any production or staging release of **CodeLenses**.

---

## 1. Objective and Policy

Every release candidate must pass the unit, integration, migration, and packaging checks in this specification. Performance benchmarks are available as an opt-in stage. A regression is defined as:
1. Any test failure in unit, integration, migration, or security test suites.
2. Any memory corruption, crash, uncaught exception, or data loss under stress/malformed inputs.
3. When benchmarks are enabled, any performance violation exceeding the specified regression thresholds.
4. Any packaging defect resulting in missing runtime binaries, headers, static assets, or sample configurations.

---

## 2. Regression Stages and Test Matrix

The regression suite is structured into six sequential stages:

```
[Stage 1: Code Hygiene] -> [Stage 2: Unit Tests] -> [Stage 3: Integration Tests]
       |                              |                           |
       v                              v                           v
[Stage 4: Migration Tests] -> [Stage 5: Optional Benchmarks] -> [Stage 6: Packaging & Sanity]
```

### Stage 1: Code Hygiene & Formatting
- **Clang-Format**: 100% of C/C++ source and header files must conform to repository `.clang-format`.
  - Command: `./scripts/format.sh --check`
- **Compiler Warnings**: Zero warnings allowed under `-Werror` (or `CODELENSES_WARNINGS_AS_ERRORS=ON`).
- **Static Analysis**: Clang-Tidy checks must report no blocking defects.
  - Command: `./scripts/lint.sh build`

### Stage 2: Core Unit Test Suite
- **Binary**: `build/tests/codelenses_unit_tests`
- **Coverage**:
  - Adapter registry, registration, and discovery.
  - Language adapters: C, C++, C#, Python, TypeScript/JavaScript, Go, Java, POSIX Shell/Bash.
  - Range coordinate conversion and UTF-8 validation.
  - Bounded job queue and worker thread pool concurrency.
  - Repository CRUD operations (workspace, file, symbol, occurrence, reference, relation, dependency, diagnostic, job, FTS5).
  - Scope hierarchy calculation and same-file innermost lexical shadowing.
  - Configuration loading, JSON serialization, CLI flag parsing, and validation.

### Stage 3: Integration & Contract Verification
- **API Contract Verification (`test_api_contract.cpp`)**:
  - Full conformance with Section 6 HTTP API endpoints (`/workspaces`, `/tree`, `/files`, `/content`, `/highlights`, `/symbols`, `/references`, `/definitions`, `/callers`, `/callees`, `/graph`, `/search`, `/jobs`).
  - Strict JSON error envelopes containing `code`, `message`, and `requestId`.
  - CORS header validation (`Access-Control-Allow-Origin: *`, `Allow-Methods`, `Allow-Headers`).
  - Pagination token validation (`items`, `nextCursor`, `totalCount`).
  - Cold database restart persistence.
- **Polyglot Sample Workspace (`test_sample_workspace.cpp`)**:
  - Full indexing of polyglot workspace covering all 9 language families.
  - Verification of declarations, references, calls, inheritance, and imports across all files.
- **Security & Hostile Path Tests (`test_security_and_malformed.cpp`)**:
  - Path traversal injection: `../`, nested `../../..`, absolute paths (`/etc/passwd`).
  - Symlink escaping: symlinks pointing outside workspace root rejected under default policy.
  - Cross-workspace isolation: requests with invalid or mismatched workspace IDs rejected.
  - Malformed file fuzzing: syntax errors in C, C++, C#, Python, TypeScript, Go, Java, Shell do not crash the pipeline.
  - Binary disguise detection: ELF/binary payloads disguised with `.c` extension detected and excluded from source text.
  - Pathological input handling: 0-byte files, whitespace-only files, 100,000-character single-line minified files.
- **Cancellation & Restart Recovery (`test_cancellation_and_recovery.cpp`)**:
  - Job cancellation API stops indexing pipeline within $\le 500$ ms.
  - WAL journal integrity and clean database state after cancellation.
  - Resumption skipping already-indexed files without duplicate rows.
  - Rollback on failed file transaction leaves no partial symbol/reference rows.

### Stage 4: Database Migration Verification
- **Test File**: `tests/unit/test_migration.cpp`
- **Requirements**:
  - Upgrades from schema version 0 (fresh DB), version 1, version 2, and version 3 to latest schema.
  - Retention of workspace, file, symbol, occurrence, reference, relation, and diagnostic records across migration.
  - Foreign key integrity strictly enforced under `PRAGMA foreign_keys = ON;`.
  - Transaction rollback upon intentional SQL fault in migration step.
  - WAL mode and checkpointing verification upon reopening on disk.

### Stage 5: Performance and Footprint Thresholds
Performance regressions are measured using automated benchmark fixtures when enabled. Any metric exceeding the threshold below constitutes a build break.

| Metric | Benchmark Target | Maximum Latency / Min Throughput |
| :--- | :--- | :--- |
| **Cold Indexing Throughput** | Polyglot Workspace | $\ge 150\text{ files/second}$ |
| **Incremental Speedup** | Unchanged Workspace | $\ge 2.0\times$ faster than cold index |
| **Incremental Skip Rate** | Unchanged Workspace | $100\%$ skipped ($0$ files reparsed) |
| **Tree Expansion Latency** | Root and nested folders | $\ge 500\text{ ops/sec}$ ($< 2.0\text{ ms/op}$) |
| **Source Range Slicing** | 5,000-line source file | $\le 10.0\text{ ms}$ per range request |
| **Symbol Prefix Search** | Symbol repository | $\ge 500\text{ queries/sec}$ ($< 2.0\text{ ms/query}$) |
| **FTS5 Match Search** | FTS repository | $\ge 500\text{ queries/sec}$ ($< 2.0\text{ ms/query}$) |
| **Reference / Graph Query** | Symbol call graph | $\ge 500\text{ queries/sec}$ ($< 2.0\text{ ms/query}$) |
| **Peak Indexing RSS** | $\le 1,000$ files | $\le 200\text{ MB}$ peak memory |
| **Net RSS Growth** | Post-indexing steady state | $\le 50\text{ MB}$ growth |
| **Database Storage / File** | Standard repositories | $\le 25\text{ KB}$ per source file |
| **Database Storage / Symbol**| Standard repositories | $\le 3.5\text{ KB}$ per indexed symbol |

### Stage 6: Packaging & Deployment Sanity
- **CPack Generation**: Successful creation of:
  - `codelenses-<version>-Linux.tar.gz`
  - `codelenses-<version>-Linux.sh` (Self-extracting script)
  - `codelenses-<version>-Linux.deb` (Debian/Ubuntu package)
- **Archive Contents Verification**:
  - `bin/codelenses` (executable binary, executable bits set)
  - `lib/libcodelenses_core.a` (static library)
  - `include/codelenses/` (complete public C++ header hierarchy)
  - `share/codelenses/web/` (bundled frontend assets, `index.html`, JS, CSS)
  - `share/codelenses/codelenses.sample.json` (valid sample config)
  - `share/codelenses/codelenses.service` (systemd service unit)
  - `share/codelenses/.codelensignore.sample` (sample ignore list)
- **Configuration Validation**:
  - Sample configuration parses without errors via `AppConfig::from_json_file()`.
  - Validation returns no errors.

---

## 3. Running the Regression Suite

The regression suite can be executed with a single command via the automated runner:

```bash
# Execute complete release regression suite
./scripts/run_regression_suite.sh
```

### Options
```bash
./scripts/run_regression_suite.sh [options]
  --build-dir <dir>    Specify build directory (default: build)
  --skip-format        Skip clang-format check
  --skip-package       Skip CPack packaging verification
  --run-benchmarks     Run optional benchmarks (requires CODELENSES_BUILD_BENCHMARKS=ON)
  --verbose            Enable verbose ctest output
  -h, --help           Display help message
```

Benchmarks are excluded from the default unit-test executable and are disabled by default. To build them, configure with `-DCODELENSES_BUILD_BENCHMARKS=ON`, then run the regression script with `--run-benchmarks`.

### Exit Codes
- `0`: All stages passed. Ready for release.
- `1`: Validation failure or threshold exceeded.
