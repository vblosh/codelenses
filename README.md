# CodeLenses

CodeLenses is a local-first code indexer and browser backend built in C++20. It parses multi-language source trees using Tree-sitter, indexes symbols and relationships into SQLite (with FTS5 search support), and serves a RESTful JSON HTTP API via `cpp-httplib`.

---

## Architecture & Foundation

- **Language Standard**: C++20 (`-std=c++20`, extensions disabled).
- **Build System**: CMake 3.22+ with `CMakePresets.json` and Ninja.
- **Package Management**: [vcpkg](https://vcpkg.io/) in manifest mode (`vcpkg.json`).
- **Dependencies**:
  - `cpp-httplib` (with OpenSSL HTTPS support)
  - `SQLite3` (with FTS5, RTREE, JSON1 enabled)
  - `Tree-sitter core` + Tree-sitter language grammars
  - `nlohmann-json` for modern JSON serialization
  - `Catch2` (v3) for unit testing and test discovery
- **Quality Gates**:
  - Strict compiler warning policy (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion ...`)
  - Warnings-as-errors toggle (`CODELENSES_WARNINGS_AS_ERRORS`)
  - AddressSanitizer (`ASan`) and UndefinedBehaviorSanitizer (`UBSan`) presets
  - Formatting with `clang-format`
  - Static analysis with `clang-tidy`
  - GitHub Actions CI matrix on Linux

---

## Directory Structure

```text
codelenses/
├── CMakeLists.txt              # Root build configuration
├── CMakePresets.json           # Presets for debug, release, ci, asan
├── vcpkg.json                  # vcpkg manifest defining all C++ dependencies
├── .clang-format               # Code formatting rules
├── .clang-tidy                 # Static analysis rules
├── cmake/                      # Modular CMake scripts
│   ├── CompilerWarnings.cmake  # Warning policy and -Werror configuration
│   ├── Dependencies.cmake      # Package discovery and linking
│   ├── Formatting.cmake        # format and format-check targets
│   ├── Sanitizers.cmake        # ASan / UBSan configuration
│   ├── StaticAnalysis.cmake    # clang-tidy integration
│   ├── TreeSitterGrammar.cmake # Grammar management helper
│   └── Vcpkg.cmake             # Automatic toolchain discovery
├── include/codelenses/         # Public header files
│   ├── app/                    # Config, CLI parsing, versioning
│   ├── domain/                 # Domain models (workspaces, symbols, references)
│   ├── db/                     # SQLite persistence & migrations
│   ├── parser/                 # Tree-sitter wrappers and AST extraction
│   ├── index/                  # Incremental file indexing pipeline
│   ├── resolver/               # Cross-file symbol resolution & graph
│   └── http/                   # HTTP API endpoints and DTOs
├── src/                        # Implementation files
│   ├── app/                    # main.cpp, config.cpp
│   ├── domain/
│   ├── db/
│   ├── parser/
│   ├── index/
│   ├── resolver/
│   └── http/
├── adapters/                   # Language adapters (C, C++, Python, TS, etc.)
├── migrations/                 # SQLite schema migration scripts
├── tests/                      # Unit and integration test suites
│   ├── unit/                   # Unit tests (config, dependencies, IR)
│   ├── integration/            # API and pipeline integration tests
│   └── fixtures/               # Golden test fixtures
├── scripts/                    # Utility scripts
│   ├── format.sh               # Format source code or verify in CI
│   └── lint.sh                 # Run clang-tidy static analysis
└── docs/                       # Project documentation and specifications
```

---

## Prerequisites

- **Linux** (Ubuntu 22.04+ or similar)
- **C++20 Compiler**: GCC 12+ or Clang 15+
- **CMake**: version 3.22 or newer
- **Ninja**: `ninja-build`
- **vcpkg**: installed with `VCPKG_ROOT` environment variable exported
- **Clang Tools**: `clang-format` and `clang-tidy`

Ensure `VCPKG_ROOT` is set in your environment:
```bash
export VCPKG_ROOT=/path/to/vcpkg
```

---

## Local Build & Test Commands

### 1. Building with CMake Presets (Recommended)

#### Debug Build
```bash
# Configure
cmake --preset debug

# Build
cmake --build build/debug

# Run tests
ctest --test-dir build/debug --output-on-failure
```

#### Release Build
```bash
# Configure
cmake --preset release

# Build
cmake --build build/release

# Run tests
ctest --test-dir build/release --output-on-failure
```

#### CI Build (Warnings Treated as Errors)
```bash
cmake --preset ci
cmake --build build/ci
ctest --test-dir build/ci --output-on-failure
```

#### AddressSanitizer & UndefinedBehaviorSanitizer
```bash
cmake --preset asan
cmake --build build/asan
ctest --test-dir build/asan --output-on-failure
```

---

### 2. Manual CMake Configuration

If configuring without presets:
```bash
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCODELENSES_BUILD_TESTS=ON \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

cmake --build build
ctest --test-dir build --output-on-failure
```

---

## Running the Server Executable

The main binary is produced at `build/<preset>/codelenses`.

```bash
# Display help
./build/debug/codelenses --help

# Display version
./build/debug/codelenses --version

# Run with custom host, port, and database
./build/debug/codelenses --host 127.0.0.1 -p 8080 --db codelenses.db

# Run with a configuration file
./build/debug/codelenses --config my_config.json
```

### CLI Options

| Flag | Long Flag | Description | Default |
|---|---|---|---|
| `-h` | `--help` | Display help message and exit | |
| `-v` | `--version` | Display version information and exit | |
| `-c` | `--config <file>` | Load configuration from JSON file | |
| | `--host <addr>` | Bind host address | `127.0.0.1` |
| `-p` | `--port <port>` | Bind port number | `8080` |
| `-d` | `--db <path>` | Path to SQLite database | `codelenses.db` |
| `-w` | `--workspace <path>` | Initial workspace root directory | (none) |
| `-l` | `--log-level <lvl>` | Log level (`trace`/`debug`/`info`/`warn`/`error`) | `info` |
| `-t` | `--threads <num>` | Worker thread pool size | `4` |
| | `--static-dir <dir>` | Path to frontend static assets directory | `""` |

---

## Code Quality & Static Analysis

### Formatting (`clang-format`)

Format all C++ source and header files:
```bash
./scripts/format.sh
```

Check formatting without modifying files (used in CI):
```bash
./scripts/format.sh --check
```

Or via CMake targets:
```bash
cmake --build build/debug --target format
cmake --build build/debug --target format-check
```

### Static Analysis (`clang-tidy`)

Run static analysis against the compilation database:
```bash
./scripts/lint.sh build/debug
```

Or via CMake target:
```bash
cmake --build build/debug --target tidy
```

---

## Dependency Management & Updates

### 1. Modifying / Updating vcpkg Dependencies

All third-party C++ libraries are declared in `vcpkg.json`.

To update dependencies:
1. Update `vcpkg` repository:
   ```bash
   cd "$VCPKG_ROOT" && git pull
   ```
2. Modify or add entries in `vcpkg.json`.
3. When CMake is configured, vcpkg will automatically resolve, download, build, and link the updated packages.

### 2. Managing Tree-sitter Language Grammars

Tree-sitter language grammars are managed through `cmake/TreeSitterGrammar.cmake`.
To declare a grammar, add an invocation in `CMakeLists.txt`:

```cmake
codelenses_add_tree_sitter_grammar(
    NAME <lang>
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-<lang>.git
    GIT_TAG <version_or_tag>
)
```

This creates a static library target `codelenses_grammar_<lang>` linking `tree-sitter` core.

---

## Continuous Integration

The repository includes a complete GitHub Actions workflow (`.github/workflows/ci.yml`) that runs on every push and pull request to `main`:
1. **GCC Debug Build**: Strict warnings as errors (`-Werror`).
2. **GCC Release Build**: Optimized build verification.
3. **Clang Debug Build with ASan/UBSan**: Memory and undefined behavior sanitizers.
4. **Code Quality**: `clang-format --check` and `clang-tidy` analysis.

---

## Linked Workspaces

Link workspace A to B to search B's symbols and open its declarations while keeping reference usages scoped to A. Links are directional and direct. Standard libraries and other local dependencies use the same workspace model, with optional advanced indexing settings for SDK source roots and headers.

See the [linked workspaces guide](docs/user_guide_standard_libraries.md) for browser setup, API examples, language coverage, and migration from library profiles.
