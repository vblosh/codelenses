import { describe, it, expect, vi, beforeEach } from "vitest";
import { ApiClient } from "../src/api";

describe("ApiClient", () => {
  let client: ApiClient;

  beforeEach(() => {
    client = new ApiClient("http://localhost:8080");
    vi.restoreAllMocks();
  });

  it("fetches workspaces list", async () => {
    const mockData = { workspaces: [{ id: 1, name: "test", rootPath: "/test", revision: 1 }], total: 1 };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockData,
    } as any);

    const result = await client.getWorkspaces();
    expect(result.workspaces).toHaveLength(1);
    expect(result.workspaces[0].name).toBe("test");
    expect(global.fetch).toHaveBeenCalledWith("http://localhost:8080/api/v1/workspaces", expect.any(Object));
  });

  it("fetches workspace tree with path query param", async () => {
    const mockTree = { workspaceId: 1, path: "src", entries: [] };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockTree,
    } as any);

    const result = await client.getTree(1, "src/adapters");
    expect(result.workspaceId).toBe(1);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/tree?path=src%2Fadapters",
      expect.any(Object)
    );
  });

  it("fetches file content with range parameters", async () => {
    const mockContent = { fileId: 5, path: "main.cpp", content: "int main() {}", totalLines: 1 };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockContent,
    } as any);

    const result = await client.getFileContent(1, 5, 10, 50);
    expect(result.fileId).toBe(5);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/files/5/content?startLine=10&endLine=50",
      expect.any(Object)
    );
  });

  it("handles HTTP error response with error envelope", async () => {
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: false,
      status: 404,
      statusText: "Not Found",
      json: async () => ({
        error: { code: "not_found", message: "Workspace 99 not found" },
      }),
    } as any);

    await expect(client.getWorkspace(99)).rejects.toThrow("Workspace 99 not found");
  });

  it("triggers indexing with post body", async () => {
    const mockJob = { id: 10, workspaceId: 1, jobType: "incremental", status: "queued" };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockJob,
    } as any);

    const result = await client.triggerIndexing(1, "incremental", false);
    expect(result.id).toBe(10);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/index",
      expect.objectContaining({
        method: "POST",
        body: JSON.stringify({ jobType: "incremental", forceFull: false }),
      })
    );
  });

  it("searches source and symbols", async () => {
    const mockHits = { items: [{ fileId: 1, relativePath: "a.cpp", snippet: "match", rank: 1.0 }], total: 1 };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockHits,
    } as any);

    const result = await client.searchSource(1, "queryText");
    expect(result.items).toHaveLength(1);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/search?q=queryText&limit=50&offset=0",
      expect.any(Object)
    );
  });

  it("fetches workspace diagnostics with query params", async () => {
    const mockDiags = {
      workspaceId: 1,
      diagnostics: [
        { id: 1, workspaceId: 1, fileId: 2, severity: "error", message: "fail", line: 5 },
      ],
      total: 1,
    };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockDiags,
    } as any);

    const result = await client.getWorkspaceDiagnostics(1, "error", 20, 10);
    expect(result.workspaceId).toBe(1);
    expect(result.diagnostics).toHaveLength(1);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/diagnostics?severity=error&limit=20&offset=10",
      expect.any(Object)
    );
  });

  it("fetches file diagnostics", async () => {
    const mockDiags = {
      fileId: 2,
      diagnostics: [
        { id: 2, workspaceId: 1, fileId: 2, severity: "warning", message: "warn", line: 10 },
      ],
      total: 1,
    };
    vi.spyOn(global, "fetch").mockResolvedValueOnce({
      ok: true,
      json: async () => mockDiags,
    } as any);

    const result = await client.getFileDiagnostics(1, 2);
    expect(result.fileId).toBe(2);
    expect(result.diagnostics).toHaveLength(1);
    expect(global.fetch).toHaveBeenCalledWith(
      "http://localhost:8080/api/v1/workspaces/1/files/2/diagnostics",
      expect.any(Object)
    );
  });
});
