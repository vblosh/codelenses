## Plan: Standard-library indexing

Add **explicitly configured library indexes** that projects can attach to, search, and resolve references against. Reuse the existing indexing pipeline, but keep library ownership and filesystem permissions separate from project files.

### Current constraints

- `IndexingPipeline::run_indexing` and `WorkspaceResolver::resolve_workspace` currently operate on one workspace.
- Dependency lookup uses workspace-relative paths; library roots need distinct identities.
- API content reads enforce workspace-root containment. Adding include paths alone will not enable library navigation.
- Language adapters can be reused, with targeted improvements for declaration files and library-specific imports.

### Decisions

- Cover **all supported languages**.
- Enable **navigation, search, and reference resolution**.
- Index **local sources and declarations** only.
- Use **explicit paths only**: no SDK discovery commands, downloads, or decompilation.
- Preserve uncertainty: source availability does not imply compiler-grade semantic resolution.

### Implementation phases

**1. Introduce library profiles and persistence**

- Define profiles containing language/provider, SDK version, target environment, ordered source roots, and file filters.
- Represent library roots as library-kind backing workspaces, reusing existing file/symbol ownership and indexing.
- Attach profiles to project workspaces; share indexes only when their root and configuration identities match.
- Add a versioned database migration without modifying existing project behavior.
- Scope symbol-key caches by owner to prevent collisions between SDK versions.

**2. Reuse the indexing pipeline**

*Depends on phase 1.*

- Reuse discovery, bounded parsing, incremental hashes, transactional persistence, cancellation, and progress reporting.
- Preserve root containment for each library; do not expand project-root permissions.
- Support extensionless C++ headers and Python declaration files.
- Refresh affected project resolution after a library changes—even when project files are unchanged.
- Handle missing sources, stale indexes, shared refresh requests, and detach/delete safely.

**3. Add language-specific providers**

*Providers can proceed in parallel after the profile contract is established.*

| Language | Explicitly supplied inputs and required behavior |
|---|---|
| C/C++ | Runtime/toolchain headers, including extensionless C++ headers; ordered include resolution. |
| Go | `GOROOT/src` equivalent; multi-file packages, aliases, receiver methods, configured platform/build constraints. |
| Python | Standard-library `.py` sources and optional `.pyi` stubs; package resolution and explicit stub precedence. |
| JavaScript/TypeScript | Selected `lib*.d.ts` files and optional local Node declarations; separate runtime environments and ambient/module declarations. |
| Java | Extracted JDK sources; package/module layouts and implicit `java.lang` imports. |
| C# | Compatible BCL/reference sources or C# declarations; namespace/type lookup and framework identity. |
| Shell/Bash | Explicit source-script libraries; built-ins and executables remain classified without fabricated source definitions. |

Unavailable native or compiled-only APIs remain clearly labeled. Java archives must be extracted manually for this first version.

**4. Extend resolution and API visibility**

*Depends on indexing and each provider.*

- Resolve against **the project plus its attached libraries**, not every indexed symbol.
- Carry owner/file/package identity through dependency candidates and symbol lookup.
- Respect imports, aliases, lexical shadowing, include order, and runtime environments.
- Separate origin from resolution: a target can be **resolved** and originate in a **standard library**.
- Add attach/detach/index/status endpoints and project/library/all search scopes.
- Update definitions, references, callers, callees, content, and highlights to authorize attached-library access.
- Prevent shared indexes from exposing references belonging to unrelated projects.

**5. Add browser support**

*Can develop against agreed API contracts alongside phase 4.*

- Library settings for roots, version, and environment.
- Separate **Libraries** explorer section.
- Indexing progress and missing-source diagnostics.
- Search-scope selector and source/declaration provenance badges.
- Owner-aware navigation and tab identity, preserving the consuming-project context when viewing library references.

**6. Verify and document**

- Add miniature library fixtures for every provider; tests must not require installed SDKs.
- Document setup, supported formats, declaration-versus-implementation navigation, and unresolved behavior.
- Track this as a cross-language workstream rather than treating it solely as H7 Go work.

### Critical files

All paths are under `/home/slava/projects/codelenses`:

- `src/db/migration.cpp`, new `migrations/002_library_indexes.sql` — persistence.
- `include/codelenses/domain/workspace.hpp`, new `include/codelenses/domain/library.hpp` — ownership and profiles.
- `src/index/indexer.cpp`, `src/filesystem/discovery.cpp` — library indexing.
- `src/resolver/{resolver,dependency_resolver,symbol_resolver,relationship_builder}.cpp` — cross-owner resolution.
- `src/server/service.cpp`, `include/codelenses/server/dto.hpp` — authorization, search, navigation contracts.
- `web/src/components/{workspace-settings,explorer,code-window,references}.ts` — browser integration.
- `tests/unit/`, `tests/integration/`, `docs/limitations_and_resolution.md` — coverage and behavior documentation.

### Verification

1. Run `cmake --preset debug`, `cmake --build --preset debug`, and `ctest --preset debug`.
2. Run `npm test` and `npm run build` in `web`.
3. For each provider: attach fixture sources → index → resolve a project reference → search → navigate → inspect project callers.
4. Test SDK-version isolation, multi-file packages, shadowing, ambiguity, refresh, cancellation, and detach.
5. Test traversal, symlink escapes, unauthorized library IDs, and cross-project reference leakage.
6. Measure cold indexing, incremental reuse, search latency, and memory with a representative large library.

**Scope boundary:** this makes supplied library sources usable across the application; it does not promise complete language semantics or definitions for APIs whose sources/declarations are unavailable.