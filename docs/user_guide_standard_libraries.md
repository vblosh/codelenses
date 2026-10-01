# Linked workspaces and local libraries

Libraries are ordinary workspaces. Create and index a workspace for local SDK headers, standard-library sources, or another project, then link it from the consuming workspace.

## Behavior

A link **A → B** exposes B's symbols in A's symbol search. Opening a B declaration retains A as the active workspace: the references panel shows B's declaration and usages only from A. Source-text search and the main file tree stay local to the active workspace. Select B explicitly to browse B and see usages in B.

Links are directional and direct. A → B → C does not expose C to A. Add A → C explicitly when needed. Reciprocal links are allowed. Workspace ownership distinguishes symbols and files even when roots or symbol names overlap.

Linking, unlinking, and deleting a target refresh the consumer's resolution without reparsing its files. Indexing B also refreshes its direct consumers. These operations serialize with indexing jobs, so a link change may wait for an active job. Unlinking revokes file/symbol access through A and clears persisted resolution links.

## Browser setup

1. Use **Add workspace** for B. For SDK sources, expand **Advanced indexing settings** to configure language, ordered source roots/include directories, defines, standard, and environment metadata.
2. Index B using the ordinary workspace indexing control.
3. Select A, open **Links** or workspace settings, then **Link workspace** and select B.
4. Search symbols in A. Results and declaration locations identify their owning workspace.

Settings may also be edited later. Use **Save & Re-index** after changing indexing configuration. Multiple configured source roots appear as indexed folders with stable root identities.

## API

Create an SDK workspace using the ordinary workspace endpoint:

```http
POST /api/v1/workspaces
Content-Type: application/json

{
  "rootPath": "/opt/sdk/include",
  "name": "SDK headers",
  "indexingSettings": {
    "language": "cpp",
    "languageStandard": "c++20",
    "sourceRoots": ["/opt/sdk/include", "/opt/sdk/platform"],
    "defaultIncludeRoots": ["/opt/sdk/include", "/opt/sdk/platform"],
    "defines": []
  }
}
```

`indexingSettings` is optional. Omit it for ordinary automatic language detection. It also supports `provider`, `sdkVersion`, `targetEnvironment`, `targetFramework`, `sysroot`, `includePatterns`, and `excludePatterns`. An empty source-root list uses the workspace root. `PATCH /api/v1/workspaces/{id}` accepts a replacement `indexingSettings` object.

- Index B: `POST /api/v1/workspaces/{b}/index`.
- Link A to B: `POST /api/v1/workspaces/{a}/links` with `{"targetWorkspaceId": b}`.
- List A's direct targets: `GET /api/v1/workspaces/{a}/links` returns `{workspaceId, workspaces, total}`.
- Unlink: `DELETE /api/v1/workspaces/{a}/links/{b}`.

Repeated link/unlink operations are idempotent. Self-links and nonexistent target IDs are rejected. File and symbol responses include `ownerWorkspaceId` and `ownerWorkspaceName`.

## Languages and migration

All supported languages participate in symbol search and declaration navigation. Cross-workspace reference resolution preserves the existing C/C++ include-evidence rules and C# namespace/import rules. A symbol being searchable does not mean every use can be resolved automatically.

C++ settings retain extensionless header discovery and conservative preprocessing. C# accepts local `.cs` source/declaration trees; target-framework metadata is descriptive and optional. SDK discovery, package restoration, compiler execution, DLL ingestion, and decompilation are not provided.

Database migration 005 converts existing library profiles into workspace indexing settings and attachments into workspace links. Indexed workspace/file/symbol IDs and source-root IDs are preserved, including configurations sharing a filesystem root. Former library workspaces become selectable directly. The old `/libraries` endpoints are removed; use workspace CRUD, indexing, and links instead.
