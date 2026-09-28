## Plan: C# Library Support

Extend the existing attached-library architecture from C/C++ to explicitly configured C#/.NET source libraries. Projects will resolve namespaces, types, methods, and inheritance into attached local `.cs` source/declaration trees while preserving owner isolation and labeling unavailable BCL/NuGet APIs as external.

### Findings

- Library profiles currently reject languages other than `c` and `cpp`.
- The C# adapter already extracts namespaces, types, methods, properties, fields, inheritance, and `using` directives.
- Library indexing, ownership, authorization, refresh, and caller isolation already exist for C/C++ and can serve as the implementation template.
- Current C# limitations classify BCL and NuGet assemblies as external.
- C# namespace imports differ from C/C++ includes: one `using` may expose symbols across many files, so resolution needs namespace-aware visibility rather than a single dependency target.
- The current library UI is C/C++-specific.

### Steps

#### Phase 1 — Profile and persistence

1. Extend `LibraryProfile` and library DTOs with explicit C# framework identity, preferably a dedicated target-framework/TFM field such as `net8.0`.
2. Include framework identity in profile fingerprints and cache identity.
3. Add a migration after the existing library-index migration for the new metadata and required indexes.
4. Accept `csharp`/`cs` in `ApiService::create_library`.
5. Preserve explicit local-source configuration and filesystem authorization.
6. Exclude `.csproj`/MSBuild inference, NuGet downloads, SDK discovery, DLL ingestion, and decompilation.

#### Phase 2 — C# library indexing

7. Reuse `IndexingPipeline::run_indexing` and library backing workspaces.
8. Add C# profile handling in `src/index/indexer.cpp`.
9. Discover `.cs` source and declaration files using profile filters.
10. Preserve incremental hashing, transactional file replacement, cancellation, diagnostics, and progress reporting.
11. Support multiple explicit source roots with stable root-relative identities.
12. Ensure shared C# indexes remain owner-scoped and cannot leak project references or callers.
13. Re-resolve attached projects after a library refresh, even when project files are unchanged.
14. Preserve access revocation and invalidation behavior for detach and profile deletion.

#### Phase 3 — C# extraction and resolution

15. Audit `CSharpAdapter` for:
   - Block-scoped and file-scoped namespaces
   - Nested namespaces and types
   - Generic types
   - `using` aliases
   - `using static`
   - `global using`
   - Inheritance and interface lists
   - Method and property references
   - Declaration-versus-definition metadata
16. Add structured metadata distinguishing namespace imports, type imports, aliases, static imports, and global imports.
17. Add C# dependency and visibility handling to `DependencyResolver` and `WorkspaceResolver`.
18. Build an owner-aware namespace index for indexed library symbols.
19. Extend `SymbolResolver` with C# rules:
   - Same-file lexical scope
   - Fully qualified lookup
   - Alias substitution
   - Imported namespace lookup
   - Static-member lookup
   - Attached-library filtering
   - Ambiguity across conflicting framework profiles
20. Prevent workspace-wide fallback from resolving C# library symbols without import or explicit qualification.
21. Resolve library-internal references without using consumer-project symbols.
22. Keep unavailable BCL/NuGet APIs, reflection, dynamic dispatch, and source-generator output as `external` or `unresolved`.
23. Verify C# calls, inheritance, interface implementation, overrides, namespace imports, and project-only callers.

#### Phase 4 — API and web integration

24. Extend DTOs, HTTP validation, persistence, and status responses with C# framework metadata.
25. Update file, symbol, search, navigation, and references APIs to expose library ownership where needed.
26. Preserve attached-library authorization and reject detached or unrelated library resources.
27. Generalize `library-manager.ts` and `workspace-settings.ts` to support C/C++/C# profiles.
28. Add C# language, provider, framework/TFM, and local source-root controls.
29. Update UI copy to clarify that local `.cs` sources/declarations are required.
30. Add framework labels and library-origin badges to search and navigation results.
31. Update `web/src/types.ts`, `web/src/api.ts`, and frontend tests.

#### Phase 5 — Tests and documentation

32. Add C# library fixtures containing:
   - Multiple files in one namespace
   - Fully qualified references
   - Namespace imports
   - Aliases
   - Static imports
   - Global usings
   - Generic types
   - Inheritance and interfaces
33. Add unit tests for profile validation, fingerprints, migrations, source-root authorization, namespace indexing, aliases, static/global usings, ambiguity, and external APIs.
34. Add an end-to-end test analogous to `test_library_indexing.cpp`.
35. Verify creation, indexing, attachment, navigation, project caller isolation, detachment, and access revocation.
36. Add HTTP and web tests for C# profile creation, TFM serialization, indexing, attachment, and unauthorized access.
37. Update standard-library support and C# limitation documentation.

### Relevant files

- `/home/slava/projects/codelenses/include/codelenses/domain/library.hpp` — C# framework/TFM profile identity.
- `/home/slava/projects/codelenses/include/codelenses/server/dto.hpp` — request and response DTOs.
- `/home/slava/projects/codelenses/src/server/service.cpp` — C# validation, authorization, and library lifecycle.
- `/home/slava/projects/codelenses/migrations/003_csharp_library_profiles.sql` — new persistence migration.
- `/home/slava/projects/codelenses/src/db/migration.cpp` — migration registration.
- `/home/slava/projects/codelenses/src/db/library_repository.cpp` — persistence and fingerprinting.
- `/home/slava/projects/codelenses/src/index/indexer.cpp` — C# library discovery and indexing.
- `/home/slava/projects/codelenses/src/filesystem/discovery.cpp` — source-root and filtering behavior.
- `/home/slava/projects/codelenses/src/adapters/csharp_adapter.cpp` — import metadata and extraction improvements.
- `/home/slava/projects/codelenses/src/resolver/dependency_resolver.cpp` — C# dependency handling.
- `/home/slava/projects/codelenses/src/resolver/resolver.cpp` — attached-library resolution context.
- `/home/slava/projects/codelenses/src/resolver/symbol_resolver.cpp` — namespace, alias, and owner-aware lookup.
- `/home/slava/projects/codelenses/src/resolver/relationship_builder.cpp` — C# relations.
- `/home/slava/projects/codelenses/web/src/components/library-manager.ts` — profile management UI.
- `/home/slava/projects/codelenses/web/src/components/workspace-settings.ts` — attachment UI.
- `/home/slava/projects/codelenses/web/src/types.ts` and `/home/slava/projects/codelenses/web/src/api.ts` — frontend contracts.
- `/home/slava/projects/codelenses/tests/unit/test_csharp_adapter.cpp` — adapter coverage.
- `/home/slava/projects/codelenses/tests/unit/test_library_indexing.cpp` — library integration template.
- `/home/slava/projects/codelenses/docs/standard_library_support.md` — architecture plan.
- `/home/slava/projects/codelenses/docs/user_guide_standard_libraries.md` — user-facing setup.
- `/home/slava/projects/codelenses/docs/limitations_and_resolution.md` — C# behavior and limitations.

### Verification

1. Run `cmake --preset debug`, `cmake --build --preset debug`, and `ctest --preset debug`.
2. Run `npm test` and `npm run build` from `/home/slava/projects/codelenses/web`.
3. Create and index a local C# library profile, attach it to a project, and verify namespace, alias, static, generic, inherited, and global-using resolution.
4. Verify that unimported short names remain unresolved and conflicting attached profiles produce ambiguity.
5. Verify library refresh re-resolves consumers and does not reparse unchanged project files.
6. Verify cancellation leaves consistent state and detachment immediately revokes library file and symbol access.
7. Verify caller and reference results are scoped to the requesting project.
8. Verify DLL-only, reflection, dynamic, unavailable BCL, and source-generator references are labeled external or unresolved.

### Decisions

- Support .NET/BCL libraries first.
- Use explicitly configured local `.cs` source/declaration trees.
- Do not support automatic SDK discovery, `.csproj` parsing, MSBuild evaluation, NuGet restore, DLL metadata ingestion, or decompilation.
- Reuse the existing C++ library architecture while implementing C#-specific namespace/import semantics.
- Do not promise compiler-grade overload resolution, full partial-class merging, or source-generator analysis.