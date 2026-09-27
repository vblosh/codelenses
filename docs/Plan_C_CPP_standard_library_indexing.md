## Plan: C/C++ standard-library indexing

Implement C/C++ as the first provider of the shared library-indexing architecture in `docs/standard_library_support.md`.

**Result:** after explicitly configuring local toolchain/header directories, users can browse and search standard-library declarations, navigate from project references into headers, and inspect project callers without mixing unrelated SDK versions or exposing unrelated files.

This plan retains the earlier decisions: **explicit paths, local sources only, no compiler execution or downloads**.

### Findings that affect implementation

The existing code provides useful foundations, but four details need changes:

- `CompileCommandContext::include_dirs` flattens `-I`, `-iquote`, `-isystem`, and `-idirafter`. Correct header lookup needs their categories and ordering.
- `DependencyResolver::resolve_c_cpp` searches workspace files and uses filename-suffix fallbacks. Those fallbacks must not select an unrelated standard header.
- `SymbolResolver` has qualified-name and global lookup paths that need visibility filtering before adding library symbols.
- `workspace.root_path` is unique. Multiple language/configuration variants of the same physical SDK root require a schema change.

---

## Phase 1 — Define profiles and ownership

### 1. Add a C/C++ library profile

Represent a toolchain/header installation with:

| Field | Purpose |
|---|---|
| Name and version | User-visible identity; explicitly supplied |
| Language | C or C++ parsing mode |
| Toolchain/target labels | Distinguish incompatible installations |
| Language standard | For example, C17 or C++20 |
| Source roots | Authorized directories containing headers |
| Ordered default include roots | Reproduce the configured toolchain search order |
| Root role | C runtime, C++ library, compiler headers, platform headers |
| Sysroot | Optional explicit target filesystem prefix |
| Defines/undefines | Explicit configuration, not guessed compiler defaults |
| Include/exclude patterns | Limit indexed content |
| Header-language rules | `.h`, extensionless headers, implementation fragments |
| Resource limits | File count, file size, total bytes, traversal depth |

**Important distinction:** an authorized source root is not automatically an extra `-I` directory. Store indexing permissions separately from include-search behavior.

A profile can cover several directories—for example, C++ headers, architecture-specific C++ headers, compiler-provided headers, and C runtime headers.

### 2. Store library indexes separately from projects

Reuse backing workspaces for library file/symbol ownership, with:

- Workspace kind: `project` or `library`.
- Library profile and root records.
- Project-to-profile attachment records.
- Configuration fingerprint and indexing revision.

Cache identity must include canonical roots, language mode, standard, target, macros, filters, and adapter version. Two projects with the same profile can reuse an index; different configurations must not silently share incompatible facts.

### 3. Add a migration

Add migration 2 and register it in `MigrationRunner`.

The migration must:

- Preserve existing project records and IDs.
- Replace global root uniqueness with project-root uniqueness and explicit library-instance identity.
- Allow the same physical directory to have distinct C and C++ library indexes.
- Add foreign keys and lookup indexes for attachments and library roots.
- Preserve reference text when a target disappears, allowing later re-resolution.

Keep existing symbol keys stable. Change resolver caches to use **owner ID plus symbol key**, rather than rewriting all existing keys.

**Phase acceptance:** migration tests pass for both fresh databases and upgrades; two projects can share a library without sharing project-owned references.

---

## Phase 2 — Preserve compile-command semantics

*Depends on the profile contract; can run alongside persistence work.*

### 4. Replace flattened include metadata

Extend `CompileCommandContext` with typed search entries containing:

- Directory.
- Category.
- Original position.
- Origin: compile command or configured profile.
- Root role and authorization mapping.

Retain compatibility for existing DTOs while moving resolution to the typed representation.

Handle the supported GCC/Clang-style flags:

- `-I`
- `-iquote`
- `-isystem`
- `-idirafter`
- `--sysroot` and `-isysroot`
- `-nostdinc`
- `-nostdinc++`
- `-D`, `-U`, and `-std`

Resolve relative paths against the compile command’s directory. Support relevant sysroot-prefixed path forms explicitly rather than treating them as ordinary paths.

Continue using `CompilationDatabase::tokenize_command_safely`. **Never execute command strings.** Unsupported flags and response-file inputs should produce diagnostics rather than silently implying complete configuration.

### 5. Define deterministic include ordering

For quoted includes:

1. Including file’s directory.
2. `-iquote` directories.
3. `-I` directories.
4. `-isystem` directories.
5. Explicitly configured default toolchain directories.
6. `-idirafter` directories.

For angle includes, omit steps 1–2.

Preserve order within categories and test duplicate-directory behavior.

- `-nostdinc` disables configured default system roots, not explicitly supplied command search paths.
- `-nostdinc++` disables default C++ roots only.
- Without a compilation database, use the explicit profile and existing configured fallback command.
- Never discover missing defaults by invoking the compiler.

**Phase acceptance:** table-driven tests demonstrate quoted versus angle lookup, category ordering, sysroot handling, and default-root suppression.

---

## Phase 3 — Discover and index headers safely

*Depends on phases 1–2.*

### 6. Extend discovery for library headers

Reuse `FileDiscovery` and its existing overrides, limits, binary checks, and symlink policy.

Add profile-controlled support for:

- Extensionless headers such as `vector`, `string`, and `stddef`-style toolchain files where supplied.
- `.h`, `.hpp`, `.hh`, `.hxx`.
- Implementation fragments such as `.inc` and `.tcc`, when selected by the profile.
- Explicit C versus C++ parsing of ambiguous `.h` files.

Do not globally classify every extensionless project file as C++.

Retain internal implementation directories such as `bits`, `__memory`, and architecture-specific subdirectories: public headers frequently depend on them.

For overlapping roots, canonicalize and deduplicate physical discovery where possible while retaining include-search aliases and ordering.

### 7. Reuse `IndexingPipeline`

Run normal indexing against each library backing workspace:

- Bounded parse queue.
- Content hashing.
- Transactional per-file replacement.
- Deleted-file cleanup.
- Cancellation.
- Diagnostics and progress.
- Stored language selection for consistent highlighting.

Configuration changes must affect the parse fingerprint. Otherwise changing language standard or macro settings could leave unchanged files incorrectly cached.

A library refresh must schedule resolution for attached projects even when their source files did not change.

### 8. Define refresh behavior

Initially use a simple coordination model:

- Serialize refreshes for the same library profile.
- Mark the library unavailable for fresh resolution publication while updating.
- Publish project resolution only when its project/library revision snapshot still matches.
- Mark or suppress stale target links during refresh.
- Re-resolve consumers after successful refresh.
- Report partial/cancelled library indexes explicitly.

Do not imply whole-library rollback: the existing pipeline provides per-file transactions.

**Phase acceptance:** extensionless headers are parsed and highlighted; unchanged refreshes skip work; changed headers invalidate consumer resolution; cancellation leaves a coherent reported state.

---

## Phase 4 — Extract useful facts without false certainty

*Can begin with synthetic fixtures while phase 3 is implemented.*

### 9. Audit standard-header constructs

Reuse `CAdapter` and `CppAdapter`; add focused extraction improvements for:

- Function prototypes and definitions.
- Typedefs and type aliases.
- Structs, classes, enums, and fields.
- Namespaces and inline namespaces.
- Templates and syntactically available specializations.
- Overloads and operators.
- `using` declarations and namespace aliases.
- Macros.
- Qualified references.
- Declaration-versus-definition metadata.

Preserve exact original source ranges.

Navigation to `printf` will often reach a **declaration**, not its compiled implementation. The API and UI must describe that accurately.

### 10. Handle conditional declarations conservatively

Current C++ preprocessing logic evaluates conditions using limited macro knowledge. Installed headers often depend on macros supplied by compilers or previously included headers.

Introduce an opt-in conservative mode for library indexing:

- Known condition: record the supported outcome.
- Unknown condition: retain conditional facts from potentially applicable branches.
- Attach condition/uncertainty metadata.
- Never share mutable include-guard macro state across separately parsed files.
- Never assume missing compiler macros are known false when the environment is incomplete.

The shared library index represents header facts, not a fully preprocessed translation unit. Consumer context can filter or downgrade candidates; it must not mutate the shared facts.

Macro-generated declarations and namespace wrappers that cannot be interpreted safely remain diagnosed limitations. Avoid text substitutions that alter coordinates or hard-coded rewriting of implementation namespaces into `std`.

**Phase acceptance:** conditional declarations are not silently erased, overloads remain distinct, and unsupported macro-generated constructs are visible as limitations.

---

## Phase 5 — Resolve includes and symbols across owners

*Depends on indexed files and extraction metadata.*

### 11. Make dependency targets owner-aware

Extend registered files and `CandidateTarget` with:

- Owning workspace/library identity.
- Target file ID.
- Root-relative path.
- Include-search evidence.
- Search-directory position where needed.

Register only project files and libraries attached to that project.

Library-internal resolution may use the other roots in its profile, but must never depend on consumer-project symbols.

### 12. Implement authoritative header lookup

Resolve each include using the ordered search configuration.

Important edge cases:

- **First matching header wins.** Different headers in later directories are not ambiguous alternatives.
- If a higher-priority matching header exists but was excluded from indexing, do not fall through to a lower-priority indexed header.
- If a search location is not authorized for inspection, report incomplete evidence rather than silently claiming certainty.
- Disable suffix guessing and conventional-directory guessing for authoritative library lookup.
- Keep missing, inaccessible, excluded, and unindexed targets distinguishable through diagnostic reasons.

Implement `#include_next` using the search position that found the current header. If this provenance is unavailable, retain uncertainty rather than treating it as ordinary `#include`.

### 13. Build a bounded include closure

Standard-library declarations frequently live several headers beneath the public include.

Build a cycle-safe closure that:

- Tracks translation-unit search context.
- Handles include guards and cycles without infinite traversal.
- Propagates uncertainty through conditional includes.
- Supports configured forced includes where implemented.
- Caches by configuration and library revision.
- Enforces traversal limits.

### 14. Restrict symbol lookup to visible candidates

Apply visibility checks to **every** lookup path, including qualified lookup and global fallback.

Use evidence in this order:

1. Same-file lexical declarations.
2. Visible declarations from the include closure.
3. Qualified names and namespace scope.
4. Explicit `using`/alias evidence.
5. Existing project-only heuristics, clearly labeled where retained.

Do not resolve `std::vector` merely because an attached library contains it. It must be reachable through applicable include and namespace evidence.

Preserve ambiguity for overloads, dependent names, and template cases that Tree-sitter cannot decide. Do not merge declarations solely because normalized signatures happen to match.

**Phase acceptance:** a project can reach declarations through public-to-internal header chains, while unattached or non-visible library symbols cannot become accidental targets.

---

## Phase 6 — API and browser integration

*API contracts can be drafted earlier; integration depends on phase 5.*

### 15. Add library operations and provenance

Provide profile configuration, attachment, detachment, indexing, and status operations.

Add compatible DTO fields for:

- Library/profile identity.
- Owner kind.
- Toolchain/version label.
- Declaration versus definition.
- Resolution reason and confidence.
- Index revision/status.

Keep library origin separate from resolution state: a standard-library reference can be `resolved`.

### 16. Authorize all navigation paths

Update `ApiService::require_file` and `require_symbol` around an explicit consumer-project access context.

Apply checks consistently to:

- Content and ranges.
- Highlights and outline.
- Definitions.
- References.
- Callers/callees.
- Graphs.
- Search results.

Read library content relative to its authorized library root, never by weakening project containment checks.

When two projects share an index, project A’s library reference view must not expose project B’s callers.

### 17. Add browser support

- Explicit C/C++ library configuration.
- Ordered include-root editing.
- Separate Libraries explorer section.
- Project/library/all-authorized search scopes.
- Declaration and library-version badges.
- Progress, missing-header, and partial-index diagnostics.
- Owner-aware tabs and navigation that preserve the consuming project.

Detach must immediately revoke access through that project.

---

## Critical implementation files

Paths below use `/home/slava/projects/codelenses` as the repository root.

| Full path | Changes |
|---|---|
| `/home/slava/projects/codelenses/include/codelenses/domain/workspace.hpp` | Project/library ownership |
| `/home/slava/projects/codelenses/include/codelenses/domain/library.hpp` — new | Profile, roots, attachments |
| `/home/slava/projects/codelenses/src/db/migration.cpp` | Register schema upgrade |
| `/home/slava/projects/codelenses/migrations/002_cpp_library_indexes.sql` — new | Library persistence and uniqueness changes |
| `/home/slava/projects/codelenses/include/codelenses/adapters/adapter.hpp` | Typed compile context and conditional-fact metadata |
| `/home/slava/projects/codelenses/src/resolver/compile_commands.cpp` | Flag categorization and path semantics |
| `/home/slava/projects/codelenses/src/filesystem/discovery.cpp` | Profile-controlled header discovery |
| `/home/slava/projects/codelenses/src/index/indexer.cpp` | Library configuration, refresh, and invalidation |
| `/home/slava/projects/codelenses/src/adapters/c_adapter.cpp` | C declaration and conditional handling |
| `/home/slava/projects/codelenses/src/adapters/cpp_adapter.cpp` | C++ library constructs and conditional handling |
| `/home/slava/projects/codelenses/src/resolver/dependency_resolver.cpp` | Ordered owner-aware include lookup |
| `/home/slava/projects/codelenses/src/resolver/symbol_resolver.cpp` | Visibility-filtered candidates |
| `/home/slava/projects/codelenses/src/resolver/resolver.cpp` | Library resolution universe and revision checks |
| `/home/slava/projects/codelenses/src/server/service.cpp` | Authorized library navigation/search |
| `/home/slava/projects/codelenses/web/src/components/workspace-settings.ts` | Profile configuration UI |
| `/home/slava/projects/codelenses/docs/standard_library_support.md` | Detailed design and setup documentation |

Related repository, DTO, relationship-builder, frontend state/navigation, and build-registration files must be updated alongside their corresponding phase.

## Verification matrix

Use small synthetic SDK trees in automated tests, not the host’s installed headers.

| Area | Required cases |
|---|---|
| Discovery | Extensionless headers, `.h` language mode, internal fragments, binary files |
| Include ordering | Quotes/angles, duplicate names, `-iquote`, `-I`, `-isystem`, `-idirafter` |
| Toolchain settings | Sysroot, `-nostdinc`, `-nostdinc++`, standard/macro changes |
| Resolution | Transitive includes, cycles, `include_next`, qualified names, aliases, overloads |
| Accuracy | Conditional declarations, unavailable macros, no include evidence, local shadowing |
| Ownership | Multiple SDK versions, same root with different modes, shared cache |
| Lifecycle | Refresh, deletion, cancellation, restart, detach, stale targets |
| Security | Traversal, symlink escape/replacement, unauthorized root/ID, cross-project leakage |

Run:

- `cmake --preset debug`
- `cmake --build --preset debug`
- `ctest --preset debug`
- `npm test` and `npm run build` in `web`

Then manually attach a real, explicitly configured libc/libstdc++ or libc++ installation and verify include navigation, declaration search, project references, incremental refresh, and memory limits.

### Delivery boundary

The first delivery supports **textual C/C++ headers and GCC/Clang-style configuration on the existing Linux platform**.

Defer compiler execution, automatic SDK discovery, full preprocessing, binary implementation lookup, complete template/type resolution, MSVC flag emulation, C++ modules, and header units. These should remain explicit capabilities or limitations—not silently approximated as fully supported behavior.
