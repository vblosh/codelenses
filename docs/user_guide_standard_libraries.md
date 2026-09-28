# User Guide: Local Library Indexing

CodeLenses supports explicitly configured local C/C++ toolchain headers and C# source or declaration trees. C# profiles can represent BCL/reference-source trees when those `.cs` files are available locally.

This guide explains how local library indexing works, how to configure and index profiles, how to attach them to projects, and how reference resolution and caller isolation behave.

---

## 1. Key Principles & Architecture

- **Local Sources Only**: All header indexing operates directly on local filesystem directories. CodeLenses **never** invokes external host compilers or downloads toolchains from the internet.
- **Explicit Configuration**: Search roots, language standards, and macro definitions are explicitly supplied. CodeLenses does not guess or execute arbitrary compiler commands.
- **Shared Caching**: Library indexes are stored in separate backing workspaces identified by a configuration fingerprint (hash of roots, language mode, standard, and macros). Multiple projects with the same library profile share the indexed data without duplicating storage or re-parsing.
- **Caller Isolation**: Shared indexes do **not** leak caller information across projects. When querying callers of a standard library symbol (e.g., `std::vector::push_back` or `printf`), CodeLenses scopes the result strictly to the requesting project workspace.
- **Include-Evidence Visibility**: A standard library symbol only resolves if the file includes the applicable header (directly or through transitive includes). Unincluded library symbols are never matched through fallback guesses.
- **Conservative Preprocessor**: When indexing standard libraries, conditional preprocessor directives (`#if`, `#ifdef`, `#elif`) with unknown platform macros are evaluated conservatively—retaining declarations across all potentially applicable branches rather than silently discarding them.
- **Extensionless C++ Headers**: Standard C++ headers without file extensions (such as `<vector>`, `<string>`, `<algorithm>`, `<memory>`) and template fragments (`.tcc`, `.inc`) are discovered and classified as C++ automatically.

---

## 2. Concepts & Data Model

### Library Profile
A **Library Profile** represents configured local headers, sources, or declarations. It contains:
- `name`: Human-readable identifier (e.g. `GCC 13 Libstdc++ C++20` or `.NET 8 reference sources`).
- `language`: `cpp`, `c`, or `csharp`.
- For C#, `language` is `csharp` and `target_framework` identifies the profile (for example, `net8.0`). The TFM is metadata; CodeLenses does not apply a framework compatibility matrix.
- `language_standard`: Standard specification, such as `c++20`, `c++17`, `c17`, `c11`.
- `source_roots`: Array of directory paths whose headers will be discovered and indexed.
- `default_include_roots`: Array of directory paths searched in deterministic priority order when resolving `#include` directives.
- `defines`: Optional command-line macro definitions (e.g. `-D__linux__=1`).
- `sysroot`: Optional target filesystem prefix (e.g. `--sysroot=/opt/sysroot`).
- `include_patterns` / `exclude_patterns`: Optional glob filters.

For C# profiles, every configured source root is scanned for `.cs` files using the same include and exclude filters. `.csproj` files are not evaluated; CodeLenses does not discover SDKs, restore NuGet packages, index DLLs, or decompile assemblies. Add the local source/declaration directories explicitly.

### Attachment & Detachment
A project workspace can attach to one or more library profiles. Attaching grants the project authorization to:
1. Search within the library headers.
2. Resolve `#include <...>` directives against the library's `default_include_roots`.
3. Resolve symbols defined in included library headers.
4. Navigate from references in project code directly to standard library declarations.

When detaching a library profile:
- **Immediate Access Revocation**: Authorization for direct library file and symbol inspection via the API is revoked immediately.
- **Reference Re-Resolution**: Persisted dependency records and occurrence links in the workspace database remain until the workspace is next re-indexed. When using the Web UI, an incremental re-index is automatically triggered upon attach or detach to update these links. If invoking the API directly, trigger a workspace re-index to refresh persisted links.

---

## 3. How to Use via Web UI

### Attaching a Standard Library to a Workspace
1. Open the CodeLenses web interface in your browser (default: `http://localhost:8080`).
2. Select your project workspace.
3. Open **Workspace Settings & Build Context** (gear icon in the top-right header).
4. Scroll to the **Standard Libraries & Toolchain SDKs** section.
5. Click **+ Attach Library**.
6. Select the library profile from the dropdown and click **Attach**.
7. Click **Save & Re-index** to re-resolve project occurrences against the newly attached library.

### Detaching a Library
1. In the **Workspace Settings** modal, locate the library under **Standard Libraries & Toolchain SDKs**.
2. Click the **Detach** button next to the library name.
3. The library is immediately unlinked: direct file and symbol access is revoked immediately, and CodeLenses triggers a background incremental re-index to update persisted reference links and dependencies.

---

## 4. How to Use via REST API

You can configure, index, and attach library profiles programmatically using the HTTP REST API.

### Step 1: Create a Library Profile

Send a `POST` request to `/api/v1/libraries`:

```bash
curl -X POST http://localhost:8080/api/v1/libraries \
  -H "Content-Type: application/json" \
  -d '{
    "name": "GCC 13 Libstdc++ C++20",
    "language": "cpp",
    "provider": "toolchain",
    "languageStandard": "c++20",
    "sourceRoots": [
      "/usr/include/c++/13",
      "/usr/include/x86_64-linux-gnu/c++/13",
      "/usr/include"
    ],
    "defaultIncludeRoots": [
      "/usr/include/c++/13",
      "/usr/include/x86_64-linux-gnu/c++/13",
      "/usr/include/c++/13/backward",
      "/usr/lib/gcc/x86_64-linux-gnu/13/include",
      "/usr/local/include",
      "/usr/include/x86_64-linux-gnu",
      "/usr/include"
    ],
    "defines": [
      "-D__linux__=1",
      "-D__x86_64__=1"
    ]
  }'
```

For a local C# source library, provide its framework identity and each source root explicitly:

```bash
curl -X POST http://localhost:8080/api/v1/libraries \
  -H "Content-Type: application/json" \
  -d '{
    "name": "Example BCL reference sources",
    "language": "csharp",
    "provider": "dotnet-reference-source",
    "targetFramework": "net8.0",
    "sourceRoots": [
      "/opt/dotnet/reference-source/System.Private.CoreLib",
      "/opt/dotnet/reference-source/System.Runtime"
    ]
  }'
```

**Response:**
```json
{
  "id": 1,
  "workspaceId": 2,
  "name": "GCC 13 Libstdc++ C++20",
  "language": "cpp",
  "provider": "toolchain",
  "languageStandard": "c++20",
  "fingerprint": "a7b3c8...",
  "sourceRoots": ["/usr/include/c++/13", ...],
  "defaultIncludeRoots": ["/usr/include/c++/13", ...]
}
```

### Step 2: Index the Library Profile

Trigger background indexing for the library profile:

```bash
curl -X POST http://localhost:8080/api/v1/libraries/1/index \
  -H "Content-Type: application/json" \
  -d '{"jobType": "full", "forceFull": false}'
```

**Response:**
```json
{
  "id": 10,
  "workspaceId": 2,
  "jobType": "full",
  "status": "queued"
}
```

You can check job progress with:
```bash
curl http://localhost:8080/api/v1/jobs/10
```

### Step 3: Attach the Library Profile to Your Project

Attach profile `1` to project workspace `100`:

```bash
curl -X POST http://localhost:8080/api/v1/workspaces/100/libraries \
  -H "Content-Type: application/json" \
  -d '{"profileId": 1}'
```

### Step 4: Re-index the Project Workspace

Trigger indexing on the project workspace so that the resolver builds the transitive include closures and resolves references into the library headers:

```bash
curl -X POST http://localhost:8080/api/v1/workspaces/100/index \
  -H "Content-Type: application/json" \
  -d '{"jobType": "full", "forceFull": true}'
```

### Listing Attached Libraries for a Workspace

```bash
curl http://localhost:8080/api/v1/workspaces/100/libraries
```

### Detaching a Library Profile

```bash
curl -X DELETE http://localhost:8080/api/v1/workspaces/100/libraries/1
```

> [!NOTE]
> Detaching immediately revokes direct API authorization for inspecting library files and symbols. To re-resolve and unlink persisted references in the project's database, trigger an incremental re-index of the project workspace:
> ```bash
> curl -X POST http://localhost:8080/api/v1/workspaces/100/index \
>   -H "Content-Type: application/json" \
>   -d '{"jobType": "incremental", "forceFull": false}'
> ```

### Deleting a Library Profile

```bash
curl -X DELETE http://localhost:8080/api/v1/libraries/1
```

---

## 5. Resolution & Navigation Details

### Include Search Ordering
CodeLenses adheres to standard GCC/Clang header search rules:
1. **Quoted includes** (`#include "my_header.h"`):
   - Current file directory.
   - `-iquote` directories.
   - `-I` directories.
   - `-isystem` directories.
   - Library `default_include_roots`.
   - `-idirafter` directories.
2. **Angle-bracket includes** (`#include <vector>`):
   - Current directory and `-iquote` directories are skipped.
   - First match in search order wins.
3. **`-nostdinc` / `-nostdinc++`**:
   - If `-nostdinc` is present in a file's compile command, `default_include_roots` are suppressed.
   - If `-nostdinc++` is present, C++ default roots are suppressed.

### Transitive Include Closure
Standard library headers frequently declare types in internal headers (for example, `<vector>` includes `<bits/stl_vector.h>`, which contains `class vector`).

CodeLenses constructs a bounded, cycle-safe transitive include closure per source file. If `main.cpp` includes `<vector>`, all symbols declared in `<bits/stl_vector.h>` become visible to `main.cpp`.

### Strict Visibility Rules
- **Explicit Include Required**: If `worker.cpp` does **not** include `<vector>`, attempting to use `std::vector` will be marked as `unresolved`. CodeLenses will **never** fall back to matching library symbols across the workspace without include evidence.
- **Project Precedence**: If both your project and an attached library define a matching symbol (e.g. custom `vector`), the project-owned symbol takes precedence.

### C# Source Libraries

- Only indexed local `.cs` files can provide navigable C# definitions. `using` namespace directives, aliases, `using static`, `global using`, and fully qualified names control visibility. A short name without a visible declaration remains `unresolved`.
- Set `targetFramework` to identify the source profile. The value is carried in library metadata and origin badges; it does not filter compatible frameworks. If attached profiles contain conflicting matching declarations, the result is `ambiguous`.
- Missing BCL or NuGet declarations remain `unresolved`; the indexer does not infer `external` merely because a name resembles a framework API. DLL-only APIs, reflection, dynamic dispatch, and source-generator output have no source navigation support.
- Partial type declarations remain separate indexed symbols. Overload selection and other compiler-level semantics are outside the resolver's scope.

### Cross-Project Caller Isolation
When multiple projects attach to the same library index (for instance, Project A and Project B both use GCC 13 Libstdc++):
- Project A references to `std::vector::push_back` are recorded in Project A.
- Project B references to `std::vector::push_back` are recorded in Project B.
- When an engineer inspects callers of `push_back` while working in Project A, CodeLenses queries callers strictly filtered to Project A's workspace ID. Project B's internal code is never revealed.

---

## 6. Linux Toolchain Configuration Examples

### Example A: GNU GCC 13 / 14 (Ubuntu / Debian)

To find your host's exact GCC include paths without executing the compiler in CodeLenses, check your system include locations or GCC installation:

```json
{
  "name": "GNU libstdc++ GCC 13",
  "language": "cpp",
  "provider": "toolchain",
  "languageStandard": "c++20",
  "sourceRoots": [
    "/usr/include/c++/13",
    "/usr/include/x86_64-linux-gnu/c++/13",
    "/usr/include"
  ],
  "defaultIncludeRoots": [
    "/usr/include/c++/13",
    "/usr/include/x86_64-linux-gnu/c++/13",
    "/usr/include/c++/13/backward",
    "/usr/lib/gcc/x86_64-linux-gnu/13/include",
    "/usr/local/include",
    "/usr/include/x86_64-linux-gnu",
    "/usr/include"
  ],
  "defines": [
    "-D__linux__=1",
    "-D__x86_64__=1"
  ]
}
```

### Example B: Clang LLVM libc++

```json
{
  "name": "LLVM libc++ 17",
  "language": "cpp",
  "provider": "toolchain",
  "languageStandard": "c++20",
  "sourceRoots": [
    "/usr/lib/llvm-17/include/c++/v1",
    "/usr/include"
  ],
  "defaultIncludeRoots": [
    "/usr/lib/llvm-17/include/c++/v1",
    "/usr/lib/llvm-17/lib/clang/17/include",
    "/usr/local/include",
    "/usr/include/x86_64-linux-gnu",
    "/usr/include"
  ],
  "defines": [
    "-D__linux__=1",
    "-D__clang__=1"
  ]
}
```

### Example C: Standard C (Glibc / C17)

```json
{
  "name": "GNU C Library (glibc)",
  "language": "c",
  "provider": "toolchain",
  "languageStandard": "c17",
  "sourceRoots": [
    "/usr/include",
    "/usr/include/x86_64-linux-gnu"
  ],
  "defaultIncludeRoots": [
    "/usr/lib/gcc/x86_64-linux-gnu/13/include",
    "/usr/local/include",
    "/usr/include/x86_64-linux-gnu",
    "/usr/include"
  ],
  "defines": [
    "-D__linux__=1",
    "-D_GNU_SOURCE=1"
  ]
}
```

---

## 7. Troubleshooting & FAQs

### Why does a standard library symbol show as "unresolved"?
1. Verify that your source file explicitly includes the header declaring the symbol or a public header that transitively includes it.
2. Check that the library profile has finished indexing (`GET /api/v1/libraries/{id}` status is `idle`).
3. Ensure the project workspace was re-indexed after attaching the library.
4. Verify that the directory containing the header is listed in `defaultIncludeRoots` or the file's compile command `-I` / `-isystem` paths.

### Can I share an index across different machines?
Yes, as long as the physical paths match (such as standardized `/usr/include` or containerized environments). The profile fingerprint is deterministic and computed from the configured roots, language mode, standard, and defines.

### Does indexing a library modify my project files?
No. Library files and symbols are stored in their own backing workspace in the SQLite database (`workspace.kind = 'library'`). Project source files are never altered.
